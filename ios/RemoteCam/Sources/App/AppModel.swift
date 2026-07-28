import Foundation
@preconcurrency import Network
@preconcurrency import AVFoundation

@MainActor
final class AppModel: ObservableObject {
    let discovery = BonjourBrowser()
    let recentHosts = RecentHostsStore()
    let remoteSession = RemoteCamSession()
    let encoder = VideoEncoder()
    let camera: CameraController

    @Published var connectionPhase: ConnectionPhase = .idle
    @Published var showingManualConnection = false
    @Published var streamError: String?

    init() {
        let encoder = self.encoder
        camera = CameraController { sampleBuffer in encoder.encode(sampleBuffer) }

        remoteSession.onPhaseChange = { [weak self] phase in self?.connectionPhase = phase }
        remoteSession.onReady = { [weak self] configuration in
            guard let self else { return }
            Task { await self.startStreaming(configuration: configuration) }
        }
        remoteSession.onTargetBitrate = { [weak encoder] bitrate in encoder?.updateBitrate(bitrate) }
        remoteSession.onControl = { [weak self] message in self?.handle(message) }
        encoder.onAccessUnit = { [weak remoteSession] unit in
            Task { @MainActor in remoteSession?.sendVideo(unit) }
        }
        encoder.onError = { [weak self] error in
            Task { @MainActor in self?.streamError = error.localizedDescription }
        }
    }

    func start() {
        discovery.start()
    }

    func connect(to host: RemoteHost) {
        let endpoint: NWEndpoint?
        if host.source == .bonjour {
            endpoint = discovery.endpoint(for: host)
        } else if let port = NWEndpoint.Port(rawValue: host.port) {
            endpoint = .hostPort(host: NWEndpoint.Host(host.hostname), port: port)
        } else {
            endpoint = nil
        }
        guard let endpoint else {
            connectionPhase = .failed("This computer no longer has a reachable network endpoint.")
            return
        }
        remoteSession.connect(to: host, endpoint: endpoint)
    }

    func disconnect() {
        remoteSession.disconnect()
        encoder.invalidate()
        Task { await camera.stop() }
    }

    private func startStreaming(configuration: StreamConfiguration) async {
        do {
            try encoder.configure(configuration)
            try await camera.prepare(configuration: configuration)
            if let host = connectionPhase.host { recentHosts.record(host) }
            remoteSession.markStreaming(configuration: configuration)
        } catch {
            streamError = error.localizedDescription
            connectionPhase = .failed(error.localizedDescription)
        }
    }

    private func handle(_ message: ControlMessage) {
        switch message.type {
        case "request_keyframe":
            encoder.requestKeyframe()
        case "set_preview":
            if let enabled = message.fields["enabled"]?.boolValue { camera.previewEnabled = enabled }
        case "set_camera":
            guard let lens = message.fields["lens"]?.stringValue else { return }
            let position = message.fields["position"]?.stringValue
            if let match = camera.cameras.first(where: {
                $0.lens.rawValue == lens && (position == nil || $0.position.rawValue == position)
            }) {
                Task { try? await camera.switchCamera(to: match.id) }
            }
        case "set_control":
            let update = CameraControlUpdate(
                zoom: message.fields["zoom"]?.numericDouble,
                focusMode: message.fields["focus_mode"]?.stringValue.flatMap(FocusMode.init(rawValue:)),
                focus: message.fields["focus"]?.numericDouble,
                exposureMode: message.fields["exposure_mode"]?.stringValue.flatMap(ExposureMode.init(rawValue:)),
                iso: message.fields["iso"]?.numericDouble,
                exposureSeconds: message.fields["exposure"]?.numericDouble,
                exposureBias: message.fields["ev"]?.numericDouble,
                whiteBalanceMode: message.fields["wb_mode"]?.stringValue.flatMap(WhiteBalanceMode.init(rawValue:)),
                whiteBalanceKelvin: message.fields["wb"]?.numericDouble,
                torchEnabled: message.fields["torch"]?.boolValue
            )
            Task { try? await camera.apply(update) }
        default:
            break
        }
    }
}

private extension CBORValue {
    var numericDouble: Double? {
        switch self {
        case .double(let value): value
        case .unsigned(let value): Double(value)
        case .negative(let value): Double(value)
        default: nil
        }
    }
}
