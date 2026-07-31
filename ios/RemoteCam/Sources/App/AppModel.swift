import Foundation
@preconcurrency import Network
@preconcurrency import AVFoundation
import UIKit

@MainActor
final class AppModel: ObservableObject {
    let discovery = BonjourBrowser()
    let recentHosts = RecentHostsStore()
    let remoteSession = RemoteCamSession()
    let encoder = VideoEncoder()
    let telemetry = DeviceTelemetry()
    let camera: CameraController
    private let liveActivity = LiveActivityController()

    @Published var connectionPhase: ConnectionPhase = .idle
    @Published var showingManualConnection = false
    @Published var showingDiagnostics = false
    @Published var streamError: String?
    private var currentConfiguration: StreamConfiguration?
    private var lastTelemetry: DeviceTelemetrySnapshot?
    private var streamGeneration = 0
    private var lastFormatGeneration: UInt64 = 0
    private var hasStarted = false

    init() {
        RemoteCamLog.info("app", "AppModel initialized; iOS=\(UIDevice.current.systemVersion)")
        let encoder = self.encoder
        camera = CameraController { sampleBuffer in encoder.encode(sampleBuffer) }

        remoteSession.onPhaseChange = { [weak self] phase in
            guard let self else { return }
            connectionPhase = phase
            if case .reconnecting(let host, _) = phase, let configuration = currentConfiguration {
                Task { await liveActivity.update(host: host, configuration: configuration, status: "Reconnecting") }
            }
            if case .failed = phase {
                stopCaptureAfterSessionEnded()
            } else if case .idle = phase, currentConfiguration != nil {
                stopCaptureAfterSessionEnded()
            }
        }
        remoteSession.onReady = { [weak self] configuration in
            guard let self else { return }
            streamGeneration &+= 1
            lastFormatGeneration = 0
            remoteSession.setVideoSuspended(true)
            let generation = streamGeneration
            Task {
                await self.startStreaming(
                    configuration: configuration,
                    generation: generation
                )
            }
        }
        remoteSession.onTargetBitrate = { [weak encoder] bitrate in encoder?.updateBitrate(bitrate) }
        remoteSession.onControl = { [weak self] message in self?.handle(message) }
        encoder.onAccessUnit = { [weak remoteSession] unit in
            Task { @MainActor in remoteSession?.sendVideo(unit) }
        }
        encoder.onError = { [weak self] error in
            RemoteCamLog.error("encoder", "runtime error: \(error.localizedDescription)")
            Task { @MainActor in self?.streamError = error.localizedDescription }
        }
        telemetry.onUpdate = { [weak self] snapshot in self?.sendTelemetry(snapshot) }
    }

    func start() {
        guard !hasStarted else { return }
        hasStarted = true
        RemoteCamLog.info("app", "starting app services")
        discovery.start()
        Task { await camera.requestPermissionAndLoadCapabilities() }
    }

    func connect(to host: RemoteHost) {
        let endpoint: NWEndpoint?
        if host.source == .bonjour || (host.source == .recent && host.port == 0) {
            endpoint = discovery.endpoint(for: host)
        } else if host.port > 0, let port = NWEndpoint.Port(rawValue: host.port) {
            endpoint = .hostPort(host: NWEndpoint.Host(host.hostname), port: port)
        } else {
            endpoint = nil
        }
        guard let endpoint else {
            RemoteCamLog.error("app", "no reachable endpoint for selected host")
            connectionPhase = .failed("This computer no longer has a reachable network endpoint.")
            return
        }
        remoteSession.connect(to: host, endpoint: endpoint)
    }

    func disconnect() {
        RemoteCamLog.info("app", "disconnect requested")
        streamGeneration &+= 1
        lastFormatGeneration = 0
        remoteSession.setVideoSuspended(true)
        remoteSession.disconnect()
        encoder.invalidate()
        currentConfiguration = nil
        streamError = nil
        UIApplication.shared.isIdleTimerDisabled = false
        Task {
            await camera.stop()
            await liveActivity.end()
        }
    }

    private func stopCaptureAfterSessionEnded() {
        streamGeneration &+= 1
        lastFormatGeneration = 0
        remoteSession.setVideoSuspended(true)
        encoder.invalidate()
        currentConfiguration = nil
        UIApplication.shared.isIdleTimerDisabled = false
        Task {
            await camera.stop()
            await liveActivity.end()
        }
    }

    func applyCameraUpdate(_ update: CameraControlUpdate) {
        Task {
            do {
                try await camera.apply(update)
                remoteSession.sendControl(.cameraState(camera.controls))
            } catch {
                streamError = error.localizedDescription
            }
        }
    }

    func switchCamera(to deviceID: String) {
        Task {
            do {
                try await camera.switchCamera(to: deviceID)
                remoteSession.sendControl(.cameraState(camera.controls))
                encoder.requestKeyframe()
            } catch {
                streamError = error.localizedDescription
            }
        }
    }

    func focus(at point: CGPoint) {
        applyCameraUpdate(CameraControlUpdate(focusPointX: point.x, focusPointY: point.y))
    }

    private func startStreaming(
        configuration: StreamConfiguration,
        generation: Int
    ) async {
        RemoteCamLog.info("app", "starting stream generation=\(generation)")
        do {
            guard generation == streamGeneration else { return }
            let configuration = try configuration.validated()
            try encoder.configure(configuration)
            do {
                try await camera.prepare(configuration: configuration)
            } catch {
                encoder.invalidate()
                throw error
            }
            guard generation == streamGeneration else { return }
            currentConfiguration = configuration
            streamError = nil
            remoteSession.sendControl(.capabilities(camera.capabilities))
            remoteSession.sendControl(.cameraState(camera.controls))
            if let host = connectionPhase.host {
                recentHosts.record(host)
                await liveActivity.start(host: host, configuration: configuration)
            }
            UIApplication.shared.isIdleTimerDisabled = true
            remoteSession.markStreaming(configuration: configuration)
            encoder.requestKeyframe()
            remoteSession.setVideoSuspended(false)
            RemoteCamLog.info("app", "stream started generation=\(generation)")
            sendTelemetry(telemetry.snapshot, force: true)
        } catch {
            RemoteCamLog.error("app", "stream start failed: \(error.localizedDescription)")
            guard generation == streamGeneration else { return }
            encoder.invalidate()
            await camera.stop()
            currentConfiguration = nil
            UIApplication.shared.isIdleTimerDisabled = false
            remoteSession.disconnect()
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
                switchCamera(to: match.id)
            }
        case "set_format":
            guard let formatGeneration = message.fields["generation"]?.unsignedValue,
                  formatGeneration > lastFormatGeneration else { return }
            lastFormatGeneration = formatGeneration
            guard let configuration = configuration(from: message.fields) else {
                remoteSession.sendControl(.formatRejected(
                    generation: formatGeneration,
                    code: "invalid_format",
                    message: "The requested stream format is invalid."
                ))
                encoder.requestKeyframe()
                return
            }
            streamGeneration &+= 1
            remoteSession.setVideoSuspended(true)
            let taskGeneration = streamGeneration
            Task {
                await reconfigure(
                    configuration,
                    taskGeneration: taskGeneration,
                    formatGeneration: formatGeneration
                )
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
                torchEnabled: message.fields["torch"]?.boolValue,
                stabilizationEnabled: message.fields["stabilization"]?.boolValue
            )
            applyCameraUpdate(update)
        default:
            break
        }
    }

    private func reconfigure(
        _ configuration: StreamConfiguration,
        taskGeneration: Int,
        formatGeneration: UInt64
    ) async {
        let previousConfiguration = currentConfiguration
        do {
            guard taskGeneration == streamGeneration else { return }
            let configuration = try configuration.validated()
            await camera.stop()
            guard taskGeneration == streamGeneration else { return }
            do {
                try encoder.configure(configuration)
                try await camera.prepare(configuration: configuration)
            } catch {
                encoder.invalidate()
                throw error
            }
            guard taskGeneration == streamGeneration else { return }
            currentConfiguration = configuration
            streamError = nil
            remoteSession.sendControl(.cameraState(camera.controls))
            remoteSession.sendControl(.formatAcknowledged(generation: formatGeneration))
            encoder.requestKeyframe()
            remoteSession.setVideoSuspended(false)
            if let host = connectionPhase.host {
                remoteSession.markStreaming(configuration: configuration, announceStart: false)
                await liveActivity.update(host: host, configuration: configuration, status: "Live")
            }
        } catch {
            guard taskGeneration == streamGeneration else { return }
            let requestedError = error
            encoder.invalidate()
            if let previousConfiguration {
                do {
                    try encoder.configure(previousConfiguration)
                    try await camera.prepare(configuration: previousConfiguration)
                    guard taskGeneration == streamGeneration else { return }
                    currentConfiguration = previousConfiguration
                    remoteSession.sendControl(.cameraState(camera.controls))
                    remoteSession.sendControl(.formatRejected(
                        generation: formatGeneration,
                        code: "format_unavailable",
                        message: requestedError.localizedDescription
                    ))
                    remoteSession.markStreaming(
                        configuration: previousConfiguration,
                        announceStart: false
                    )
                    encoder.requestKeyframe()
                    remoteSession.setVideoSuspended(false)
                    streamError = "Format change rejected: \(requestedError.localizedDescription). Continuing with the previous format."
                    return
                } catch {
                    streamError = "Format change failed and the previous stream could not be restored: \(error.localizedDescription)"
                }
            } else {
                streamError = requestedError.localizedDescription
            }
            await camera.stop()
            currentConfiguration = nil
            UIApplication.shared.isIdleTimerDisabled = false
            remoteSession.disconnect()
            connectionPhase = .failed(streamError ?? "Video reconfiguration failed.")
        }
    }

    private func sendTelemetry(_ snapshot: DeviceTelemetrySnapshot, force: Bool = false) {
        guard currentConfiguration != nil else { return }
        if force || lastTelemetry?.orientationDegrees != snapshot.orientationDegrees {
            remoteSession.sendControl(.orientation(degrees: snapshot.orientationDegrees))
        }
        if force || lastTelemetry?.thermal != snapshot.thermal {
            remoteSession.sendControl(.thermal(snapshot.thermal.rawValue))
        }
        if force || lastTelemetry?.batteryLevel != snapshot.batteryLevel || lastTelemetry?.charging != snapshot.charging {
            remoteSession.sendControl(.battery(level: snapshot.batteryLevel, charging: snapshot.charging))
        }
        lastTelemetry = snapshot
    }

    private func configuration(from fields: [String: CBORValue]) -> StreamConfiguration? {
        guard let codec = fields["codec"]?.stringValue.flatMap(VideoCodec.init(rawValue:)),
              let width = fields["width"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let height = fields["height"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let fps = fields["fps"]?.unsignedValue.flatMap(Int.init(exactly:)),
              let bitrate = fields["bitrate"]?.unsignedValue.flatMap(Int.init(exactly:)) else { return nil }
        return try? StreamConfiguration(
            codec: codec,
            width: width,
            height: height,
            framesPerSecond: fps,
            bitrate: bitrate
        ).validated()
    }
}

private extension CBORValue {
    var numericDouble: Double? {
        let value: Double?
        switch self {
        case .double(let number): value = number
        case .unsigned(let number): value = Double(number)
        case .negative(let number): value = Double(number)
        default: value = nil
        }
        return value.flatMap { $0.isFinite ? $0 : nil }
    }
}
