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

    func prepare(configuration: StreamConfiguration) async throws {
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
        guard authorized else {
            throw CaptureEngineError.configurationFailed("Camera access is required to stream.")
        }

        cameras = await engine.availableCameras()
        capabilities = await engine.capabilities()
        controls = try await engine.configure(configuration, deviceID: controls.deviceID)
        await engine.start()
        isRunning = true
        errorMessage = nil
    }

    func stop() async {
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
            Task { @MainActor in self?.handleInterruption(reasonValue: reason) }
        })
        observers.append(center.addObserver(
            forName: AVCaptureSession.interruptionEndedNotification,
            object: engine.session,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.interruptionMessage = nil }
        })
        observers.append(center.addObserver(
            forName: AVCaptureSession.runtimeErrorNotification,
            object: engine.session,
            queue: .main
        ) { [weak self] notification in
            let message = (notification.userInfo?[AVCaptureSessionErrorKey] as? Error)?.localizedDescription
            Task { @MainActor in
                self?.errorMessage = message
            }
        })
    }

    private func handleInterruption(reasonValue: Int?) {
        let reason = reasonValue.flatMap(AVCaptureSession.InterruptionReason.init(rawValue:))
        switch reason {
        case .videoDeviceNotAvailableInBackground:
            interruptionMessage = "Camera access was interrupted in the background. Keep RemoteCam visible on iOS 17; iOS 18 and newer support background capture."
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
