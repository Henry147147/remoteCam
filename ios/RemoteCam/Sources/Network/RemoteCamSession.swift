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
    private let event: @Sendable (TransportEvent) -> Void

    init(event: @escaping @Sendable (TransportEvent) -> Void) {
        self.event = event
    }

    func connect(endpoint: NWEndpoint) {
        queue.async { [weak self] in
            guard let self else { return }
            self.connection?.cancel()
            self.decoder = WireFrameDecoder()
            let tcp = NWProtocolTCP.Options()
            tcp.noDelay = true
            let parameters = NWParameters(tls: nil, tcp: tcp)
            parameters.includePeerToPeer = true
            let connection = NWConnection(to: endpoint, using: parameters)
            self.connection = connection
            connection.stateUpdateHandler = { [weak self, weak connection] state in
                guard let self, let connection, connection === self.connection else { return }
                switch state {
                case .ready:
                    self.event(.ready)
                    self.receive(on: connection)
                case .failed(let error):
                    self.event(.failed(error.localizedDescription))
                case .cancelled:
                    self.event(.cancelled)
                default:
                    break
                }
            }
            connection.start(queue: self.queue)
        }
    }

    func send(_ frame: WireFrame) {
        queue.async { [weak self] in
            guard let self, let connection = self.connection else { return }
            do {
                connection.send(content: try frame.encoded(), completion: .contentProcessed { [weak self] error in
                    if let error { self?.event(.failed(error.localizedDescription)) }
                })
            } catch {
                self.event(.failed(error.localizedDescription))
            }
        }
    }

    func cancel() {
        queue.async { [weak self] in
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
                self.event(.failed(error.localizedDescription))
                connection.cancel()
                return
            }
            if let error {
                self.event(.failed(error.localizedDescription))
            } else if complete {
                self.event(.failed("The Windows computer closed the connection."))
            } else {
                self.receive(on: connection)
            }
        }
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
        intentionalDisconnect = true
        reconnectTask?.cancel()
        transport.cancel()
        updatePhase(.idle)
    }

    func sendControl(_ message: ControlMessage) {
        transport.send(WireFrame(
            channel: .control,
            presentationTimeMicros: Self.monotonicMicros,
            payload: message.encoded()
        ))
    }

    func sendVideo(_ unit: EncodedAccessUnit) {
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
            reconnectAttempt = 0
            sendControl(.hello(deviceID: deviceID))
        case .frame(let frame):
            handle(frame)
        case .failed(let message):
            scheduleReconnect(lastError: message)
        case .cancelled:
            if !intentionalDisconnect { scheduleReconnect(lastError: "Connection cancelled.") }
        }
    }

    private func handle(_ frame: WireFrame) {
        // Audio is reserved in v1 and must be ignored rather than rejected.
        guard frame.channel != WireChannel.audio.rawValue else { return }
        guard frame.channel == WireChannel.control.rawValue || frame.channel == WireChannel.stats.rawValue else { return }
        do {
            let message = try ControlMessage(payload: frame.payload)
            switch message.type {
            case "server_info":
                if message.fields["paired"]?.boolValue == false, let host {
                    updatePhase(.awaitingPairing(host))
                }
            case "pair_required":
                if let host { updatePhase(.awaitingPairing(host)) }
            case "ready":
                guard controlChannelAuthenticated || allowsInsecureDevelopmentSession else {
                    updatePhase(.failed("The server tried to start an unauthenticated session. Secure pairing must complete first."))
                    return
                }
                guard let configuration = Self.configuration(from: message), let host else { return }
                updatePhase(.ready(host, configuration))
                onReady?(configuration)
            case "stats":
                if let bitrate = message.fields["target_bitrate"]?.unsignedValue.flatMap(Int.init(exactly:)) {
                    onTargetBitrate?(bitrate)
                }
            default:
                onControl?(message)
            }
        } catch {
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
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(delay))
            guard !Task.isCancelled, let self else { return }
            self.transport.connect(endpoint: endpoint)
        }
    }

    private func updatePhase(_ phase: ConnectionPhase) {
        self.phase = phase
        onPhaseChange?(phase)
    }

    private static func configuration(from message: ControlMessage) -> StreamConfiguration? {
        guard let codecString = message.fields["codec"]?.stringValue,
              let codec = VideoCodec(rawValue: codecString),
              let width = message.fields["width"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let height = message.fields["height"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let fps = message.fields["fps"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let bitrate = message.fields["bitrate"]?.unsignedValue.flatMap(Int.init(exactly:)),
              (1...120).contains(fps), width > 0, height > 0, bitrate > 0 else { return nil }
        return StreamConfiguration(codec: codec, width: width, height: height, framesPerSecond: fps, bitrate: bitrate)
    }

    private static var monotonicMicros: UInt64 {
        DispatchTime.now().uptimeNanoseconds / 1_000
    }
}
