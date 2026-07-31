@preconcurrency import Network
import Foundation

private enum TransportEvent: Sendable {
    case ready
    case frame(WireFrame)
    case failed(String)
    case cancelled
}

private final class RemoteCamTransport: @unchecked Sendable {
    private let queue = DispatchQueue(label: "org.remotecam.transport", qos: .userInteractive)
    private var connection: NWConnection?
    private var decoder = WireFrameDecoder()
    private let videoBudgetLock = NSLock()
    private var reservedVideoBytes = 0
    private let maximumPendingVideoBytes = 20 * 1_024 * 1_024
    private var failureReported = false
    private var droppedVideoFrames: UInt64 = 0
    private let event: @Sendable (TransportEvent) -> Void

    init(event: @escaping @Sendable (TransportEvent) -> Void) {
        self.event = event
    }

    func connect(endpoint: NWEndpoint) {
        queue.async { [weak self] in
            guard let self else { return }
            RemoteCamLog.info("transport", "connecting to \(String(describing: endpoint))")
            self.connection?.cancel()
            self.decoder = WireFrameDecoder()
            self.failureReported = false
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            let parameters = NWParameters(tls: nil, tcp: tcp)
            parameters.includePeerToPeer = true
            let connection = NWConnection(to: endpoint, using: parameters)
            self.connection = connection
            connection.stateUpdateHandler = { [weak self, weak connection] state in
                guard let self, let connection, connection === self.connection else { return }
                RemoteCamLog.debug("transport", "NWConnection state=\(String(describing: state))")
                switch state {
                case .ready:
                    RemoteCamLog.info("transport", "TCP connection ready")
                    self.event(.ready)
                    self.receive(on: connection)
                case .failed(let error):
                    self.reportFailure(error.localizedDescription)
                case .cancelled:
                    if !self.failureReported { self.event(.cancelled) }
                default:
                    break
                }
            }
            connection.start(queue: self.queue)
        }
    }

    func send(_ frame: WireFrame) {
        let content: Data
        do {
            content = try frame.encoded()
        } catch {
            queue.async { [weak self] in self?.reportFailure(error.localizedDescription) }
            return
        }

        let isVideo = frame.channel == WireChannel.video.rawValue
        if isVideo {
            videoBudgetLock.lock()
            let available = maximumPendingVideoBytes - min(
                reservedVideoBytes,
                maximumPendingVideoBytes
            )
            guard content.count <= available else {
                droppedVideoFrames &+= 1
                let dropped = droppedVideoFrames
                videoBudgetLock.unlock()
                if dropped == 1 || dropped.isMultiple(of: 30) {
                    RemoteCamLog.error(
                        "transport",
                        "video queue budget exhausted; dropped_frames=\(dropped), frame_bytes=\(content.count)"
                    )
                }
                return
            }
            reservedVideoBytes += content.count
            videoBudgetLock.unlock()
        }

        queue.async { [weak self] in
            guard let self else { return }
            guard let connection = self.connection else {
                if isVideo { self.releaseVideoReservation(content.count) }
                return
            }
            connection.send(content: content, completion: .contentProcessed { [weak self, weak connection] error in
                guard let self else { return }
                if isVideo { self.releaseVideoReservation(content.count) }
                guard let connection, connection === self.connection else { return }
                if let error { self.reportFailure(error.localizedDescription) }
            })
        }
    }

    func cancel() {
        queue.async { [weak self] in
            RemoteCamLog.info("transport", "cancelling connection")
            self?.connection?.cancel()
            self?.connection = nil
        }
    }

    private func receive(on connection: NWConnection) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1_024) { [weak self, weak connection] data, _, complete, error in
            guard let self, let connection, connection === self.connection else { return }
            do {
                if let data, !data.isEmpty {
                    for frame in try self.decoder.append(data) { self.event(.frame(frame)) }
                }
            } catch {
                self.reportFailure(error.localizedDescription)
                connection.cancel()
                return
            }
            if let error {
                self.reportFailure(error.localizedDescription)
            } else if complete {
                self.reportFailure("The Windows computer closed the connection.")
            } else {
                self.receive(on: connection)
            }
        }
    }

    private func reportFailure(_ message: String) {
        guard !failureReported else { return }
        failureReported = true
        RemoteCamLog.error("transport", "connection failed: \(message)")
        connection?.cancel()
        event(.failed(message))
    }

    private func releaseVideoReservation(_ bytes: Int) {
        videoBudgetLock.lock()
        reservedVideoBytes = max(0, reservedVideoBytes - bytes)
        videoBudgetLock.unlock()
    }
}

@MainActor
final class RemoteCamSession: ObservableObject {
    @Published private(set) var phase: ConnectionPhase = .idle

    var onPhaseChange: ((ConnectionPhase) -> Void)?
    var onReady: ((StreamConfiguration) -> Void)?
    var onControl: ((ControlMessage) -> Void)?
    var onTargetBitrate: ((Int) -> Void)?

    private lazy var transport = RemoteCamTransport { [weak self] event in
        Task { @MainActor in self?.handle(event) }
    }
    private var host: RemoteHost?
    private var endpoint: NWEndpoint?
    private var reconnectAttempt = 0
    private var reconnectTask: Task<Void, Never>?
    private var intentionalDisconnect = false
    private var controlChannelAuthenticated = false
    private let deviceID = DeviceIdentity.loadOrCreate()
#if DEBUG
    private let allowsInsecureDevelopmentSession = ProcessInfo.processInfo.arguments.contains("--allow-insecure-session")
#else
    private let allowsInsecureDevelopmentSession = false
#endif

    func connect(to host: RemoteHost, endpoint: NWEndpoint) {
        RemoteCamLog.info("session", "connect requested for host=\(host.name), source=\(host.source.rawValue)")
        intentionalDisconnect = false
        reconnectTask?.cancel()
        self.host = host
        self.endpoint = endpoint
        reconnectAttempt = 0
        controlChannelAuthenticated = false
        updatePhase(.connecting(host))
        transport.connect(endpoint: endpoint)
    }

    func disconnect() {
        RemoteCamLog.info("session", "intentional disconnect")
        intentionalDisconnect = true
        reconnectTask?.cancel()
        transport.cancel()
        updatePhase(.idle)
    }

    func sendControl(_ message: ControlMessage) {
        if message.type != "orientation" && message.type != "battery" {
            RemoteCamLog.debug("protocol", "send control type=\(message.type)")
        }
        transport.send(WireFrame(
            channel: .control,
            presentationTimeMicros: Self.monotonicMicros,
            payload: message.encoded()
        ))
    }

    func sendVideo(_ unit: EncodedAccessUnit) {
        if unit.isKeyframe {
            RemoteCamLog.debug("protocol", "send keyframe bytes=\(unit.data.count), pts_us=\(unit.presentationTimeMicros)")
        }
        transport.send(WireFrame(
            channel: .video,
            flags: unit.isKeyframe ? .keyframe : [],
            presentationTimeMicros: unit.presentationTimeMicros,
            payload: unit.data
        ))
    }

    func markStreaming(configuration: StreamConfiguration, announceStart: Bool = true) {
        guard let host else { return }
        if announceStart { sendControl(.streamStart()) }
        updatePhase(.streaming(host, configuration))
    }

    private func handle(_ event: TransportEvent) {
        switch event {
        case .ready:
            RemoteCamLog.info("session", "transport ready; sending hello")
            reconnectAttempt = 0
            sendControl(.hello(deviceID: deviceID))
        case .frame(let frame):
            handle(frame)
        case .failed(let message):
            scheduleReconnect(lastError: message)
        case .cancelled:
            RemoteCamLog.info("session", "transport cancelled")
            if !intentionalDisconnect { scheduleReconnect(lastError: "Connection cancelled.") }
        }
    }

    private func handle(_ frame: WireFrame) {
        // Audio is reserved in v1 and must be ignored rather than rejected.
        guard frame.channel != WireChannel.audio.rawValue else { return }
        guard frame.channel == WireChannel.control.rawValue || frame.channel == WireChannel.stats.rawValue else { return }
        do {
            let message = try ControlMessage(payload: frame.payload)
            RemoteCamLog.debug(
                "protocol",
                "received channel=\(frame.channel), type=\(message.type), bytes=\(frame.payload.count)"
            )
            switch message.type {
            case "server_info":
                if message.fields["paired"]?.boolValue == false, let host {
                    updatePhase(.awaitingPairing(host))
                }
            case "pair_required":
                if let host { updatePhase(.awaitingPairing(host)) }
            case "ready":
                guard sessionIsTrusted else {
                    RemoteCamLog.error("session", "server sent ready before authenticated pairing")
                    intentionalDisconnect = true
                    reconnectTask?.cancel()
                    transport.cancel()
                    updatePhase(.failed("The server tried to start an unauthenticated session. Secure pairing must complete first."))
                    return
                }
                guard let configuration = Self.configuration(from: message), let host else {
                    RemoteCamLog.error("session", "server sent invalid ready configuration")
                    intentionalDisconnect = true
                    transport.cancel()
                    updatePhase(.failed("The server requested an invalid video configuration."))
                    return
                }
                updatePhase(.ready(host, configuration))
                RemoteCamLog.info(
                    "session",
                    "ready accepted: \(configuration.codec.rawValue) \(configuration.width)x\(configuration.height) " +
                        "at \(configuration.framesPerSecond) fps"
                )
                onReady?(configuration)
            case "stats":
                guard sessionIsTrusted else { return }
                if let bitrate = message.fields["target_bitrate"]?.unsignedValue.flatMap(Int.init(exactly:)),
                   (StreamConfiguration.minimumBitrate...StreamConfiguration.maximumBitrate).contains(bitrate) {
                    onTargetBitrate?(bitrate)
                }
            default:
                guard sessionIsTrusted else { return }
                onControl?(message)
            }
        } catch {
            RemoteCamLog.error("protocol", "discarding malformed control message: \(error.localizedDescription)")
            // Unknown/malformed control data is isolated to this message. The Windows
            // side can surface the protocol error without killing an otherwise live feed.
        }
    }

    private func scheduleReconnect(lastError: String) {
        guard !intentionalDisconnect, let host, let endpoint else {
            updatePhase(.failed(lastError))
            return
        }
        reconnectTask?.cancel()
        reconnectAttempt += 1
        let attempt = reconnectAttempt
        updatePhase(.reconnecting(host, attempt: attempt))
        let delay = min(pow(2.0, Double(attempt - 1)), 30)
        RemoteCamLog.info("session", "reconnect attempt=\(attempt), delay_seconds=\(delay), reason=\(lastError)")
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self else { return }
            self.transport.connect(endpoint: endpoint)
        }
    }

    private func updatePhase(_ phase: ConnectionPhase) {
        RemoteCamLog.info("session", "phase=\(Self.phaseDescription(phase))")
        self.phase = phase
        onPhaseChange?(phase)
    }

    private static func phaseDescription(_ phase: ConnectionPhase) -> String {
        switch phase {
        case .idle: "idle"
        case .connecting: "connecting"
        case .awaitingPairing: "awaiting_pairing"
        case .ready: "ready"
        case .streaming: "streaming"
        case .reconnecting(_, let attempt): "reconnecting(attempt=\(attempt))"
        case .failed(let message): "failed(\(message))"
        }
    }

    private static func configuration(from message: ControlMessage) -> StreamConfiguration? {
        guard let codecString = message.fields["codec"]?.stringValue,
              let codec = VideoCodec(rawValue: codecString),
              let width = message.fields["width"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let height = message.fields["height"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let fps = message.fields["fps"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let bitrate = message.fields["bitrate"]?.unsignedValue.flatMap(Int.init(exactly:)) else {
            return nil
        }
        return try? StreamConfiguration(
            codec: codec,
            width: width,
            height: height,
            framesPerSecond: fps,
            bitrate: bitrate
        ).validated()
    }

    private var sessionIsTrusted: Bool {
        controlChannelAuthenticated || allowsInsecureDevelopmentSession
    }

    private static var monotonicMicros: UInt64 {
        DispatchTime.now().uptimeNanoseconds / 1_000
    }
}
