@preconcurrency import AVFoundation
import Foundation

enum CameraPermissionState: Equatable, Sendable {
    case unknown
    case denied
    case granted
}

@MainActor
final class CameraController: ObservableObject {
    @Published private(set) var permission: CameraPermissionState = .unknown
    @Published private(set) var cameras: [CameraDescriptor] = []
    @Published private(set) var controls = CameraControlState()
    @Published private(set) var isRunning = false
    @Published private(set) var interruptionMessage: String?
    @Published private(set) var errorMessage: String?
    @Published var previewEnabled = true
    @Published private(set) var capabilities: [CameraCapability] = []

    private let engine: CaptureEngine
    private var observers: [NSObjectProtocol] = []
    var session: AVCaptureSession { engine.session }

    init(sampleHandler: @escaping @Sendable (CMSampleBuffer) -> Void) {
        engine = CaptureEngine(sampleHandler: sampleHandler)
        observeSession()
    }

    func requestPermissionAndLoadCapabilities() async {
        guard await requestPermission() else { return }
        cameras = await engine.availableCameras()
        capabilities = await engine.capabilities()
        RemoteCamLog.info(
            "camera",
            "setup complete; devices=\(cameras.count), capabilities=\(capabilities.count)"
        )
    }

    private func requestPermission() async -> Bool {
        RemoteCamLog.info("camera", "preparing camera")
        let authorized: Bool
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            authorized = true
        case .notDetermined:
            authorized = await AVCaptureDevice.requestAccess(for: .video)
        default:
            authorized = false
        }
        permission = authorized ? .granted : .denied
        RemoteCamLog.info("camera", "authorization=\(authorized ? "granted" : "denied")")
        return authorized
    }

    func prepare(configuration: StreamConfiguration) async throws {
        guard await requestPermission() else {
            throw CaptureEngineError.configurationFailed("Camera access is required to stream.")
        }
        if cameras.isEmpty { cameras = await engine.availableCameras() }
        if capabilities.isEmpty { capabilities = await engine.capabilities() }
        controls = try await engine.configure(configuration, deviceID: controls.deviceID)
        await engine.start()
        isRunning = true
        errorMessage = nil
        RemoteCamLog.info("camera", "camera prepared; devices=\(cameras.count), capabilities=\(capabilities.count)")
    }

    func stop() async {
        RemoteCamLog.info("camera", "stop requested")
        await engine.stop()
        isRunning = false
    }

    func switchCamera(to deviceID: String) async throws {
        controls = try await engine.switchCamera(deviceID: deviceID)
    }

    func apply(_ update: CameraControlUpdate) async throws {
        controls = try await engine.apply(update)
    }

    private func observeSession() {
        let center = NotificationCenter.default
        observers.append(center.addObserver(
            forName: AVCaptureSession.wasInterruptedNotification,
            object: engine.session,
            queue: .main
        ) { [weak self] notification in
            let reason = (notification.userInfo?[AVCaptureSessionInterruptionReasonKey] as? NSNumber)?.intValue
            RemoteCamLog.error("camera", "capture interrupted; reason=\(reason.map(String.init) ?? "unknown")")
            Task { @MainActor in self?.handleInterruption(reasonValue: reason) }
        })
        observers.append(center.addObserver(
            forName: AVCaptureSession.interruptionEndedNotification,
            object: engine.session,
            queue: .main
        ) { [weak self] _ in
            RemoteCamLog.info("camera", "capture interruption ended")
            Task { @MainActor in self?.interruptionMessage = nil }
        })
        observers.append(center.addObserver(
            forName: AVCaptureSession.runtimeErrorNotification,
            object: engine.session,
            queue: .main
        ) { [weak self] notification in
            let message = (notification.userInfo?[AVCaptureSessionErrorKey] as? Error)?.localizedDescription
            RemoteCamLog.error("camera", "capture runtime error: \(message ?? "unknown")")
            Task { @MainActor in
                self?.errorMessage = message
            }
        })
    }

    private func handleInterruption(reasonValue: Int?) {
        let reason = reasonValue.flatMap(AVCaptureSession.InterruptionReason.init(rawValue:))
        switch reason {
        case .videoDeviceNotAvailableInBackground:
            interruptionMessage = "Camera access was interrupted in the background. Keep RemoteCam visible on iOS 17; supported iOS 18 and newer devices can continue capture."
        case .videoDeviceInUseByAnotherClient:
            interruptionMessage = "Another app is using the camera."
        default:
            interruptionMessage = "Camera capture was interrupted."
        }
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
    }
}
