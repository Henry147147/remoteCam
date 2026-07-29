@preconcurrency import AVFoundation
import Foundation

enum CaptureEngineError: LocalizedError {
    case noCamera
    case cannotAddInput
    case cannotAddOutput
    case unsupportedFormat(width: Int, height: Int, fps: Int)
    case configurationFailed(String)

    var errorDescription: String? {
        switch self {
        case .noCamera:
            "No compatible camera is available."
        case .cannotAddInput:
            "The camera input could not be added to the capture session."
        case .cannotAddOutput:
            "The video output could not be added to the capture session."
        case .unsupportedFormat(let width, let height, let fps):
            "This camera does not support \(width)×\(height) at \(fps) fps."
        case .configurationFailed(let message):
            message
        }
    }
}

actor CaptureEngine {
    nonisolated(unsafe) let session = AVCaptureSession()

    private let output = AVCaptureVideoDataOutput()
    private let outputQueue = DispatchQueue(label: "org.remotecam.capture.output", qos: .userInteractive)
    private let delegate: SampleBufferRelay
    private var input: AVCaptureDeviceInput?
    private var selectedDevice: AVCaptureDevice?
    private var configuration: StreamConfiguration = .default1080p
    private var requestedFocusMode: FocusMode = .auto
    private var requestedWhiteBalanceMode: WhiteBalanceMode = .auto

    init(sampleHandler: @escaping @Sendable (CMSampleBuffer) -> Void) {
        delegate = SampleBufferRelay(handler: sampleHandler)
    }

    func availableCameras() -> [CameraDescriptor] {
        Self.discoverDevices().map(Self.descriptor)
    }

    func capabilities() -> [CameraCapability] {
        Self.discoverDevices().map { device in
            let formats = Set(device.formats.flatMap { format -> [CaptureFormatDescriptor] in
                let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
                return [30, 60].compactMap { fps in
                    guard format.videoSupportedFrameRateRanges.contains(where: {
                        $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate
                    }) else { return nil }
                    return CaptureFormatDescriptor(width: Int(dimensions.width), height: Int(dimensions.height), framesPerSecond: fps)
                }
            })
            return CameraCapability(camera: Self.descriptor(device), formats: formats)
        }
    }

    func configure(_ configuration: StreamConfiguration, deviceID: String? = nil) throws -> CameraControlState {
        let configuration = try configuration.validated()
        let devices = Self.discoverDevices()
        guard let device = devices.first(where: { $0.uniqueID == deviceID })
                ?? devices.first(where: { $0.position == .back && Self.isVirtualMultiLens($0.deviceType) })
                ?? devices.first(where: { $0.position == .back && $0.deviceType == .builtInWideAngleCamera })
                ?? devices.first else {
            throw CaptureEngineError.noCamera
        }

        let newInput = try AVCaptureDeviceInput(device: device)
        try Self.selectFormat(on: device, configuration: configuration)

        session.beginConfiguration()
        defer { session.commitConfiguration() }

        let previousInput = input
        if let previousInput { session.removeInput(previousInput) }

        guard session.canAddInput(newInput) else {
            if let previousInput, session.canAddInput(previousInput) {
                session.addInput(previousInput)
            }
            throw CaptureEngineError.cannotAddInput
        }
        session.addInput(newInput)

        let outputWasPresent = session.outputs.contains(output)
        if !outputWasPresent {
            guard session.canAddOutput(output) else {
                session.removeInput(newInput)
                if let previousInput, session.canAddInput(previousInput) {
                    session.addInput(previousInput)
                }
                throw CaptureEngineError.cannotAddOutput
            }
            output.alwaysDiscardsLateVideoFrames = true
            output.videoSettings = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
            ]
            output.setSampleBufferDelegate(delegate, queue: outputQueue)
            session.addOutput(output)
        }

        requestedFocusMode = .auto
        requestedWhiteBalanceMode = .auto
        if let connection = output.connection(with: .video), connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = false
        }

        if session.isMultitaskingCameraAccessSupported {
            session.isMultitaskingCameraAccessEnabled = true
        }

        self.input = newInput
        selectedDevice = device
        self.configuration = configuration
        return controlStateForCurrentOutput(device)
    }

    func start() {
        guard !session.isRunning else { return }
        session.startRunning()
    }

    func stop() {
        guard session.isRunning else { return }
        session.stopRunning()
    }

    func switchCamera(deviceID: String) throws -> CameraControlState {
        try configure(configuration, deviceID: deviceID)
    }

    func apply(_ update: CameraControlUpdate) throws -> CameraControlState {
        guard let device = selectedDevice else { throw CaptureEngineError.noCamera }
        try device.lockForConfiguration()
        defer { device.unlockForConfiguration() }

        if let zoom = update.zoom, zoom.isFinite {
            device.videoZoomFactor = min(max(CGFloat(zoom), device.minAvailableVideoZoomFactor), device.maxAvailableVideoZoomFactor)
        }

        if let x = update.focusPointX, let y = update.focusPointY, x.isFinite, y.isFinite {
            let point = CGPoint(x: min(max(x, 0), 1), y: min(max(y, 0), 1))
            if device.isFocusPointOfInterestSupported { device.focusPointOfInterest = point }
            if device.isExposurePointOfInterestSupported { device.exposurePointOfInterest = point }
            if device.isFocusModeSupported(.autoFocus) { device.focusMode = .autoFocus }
            if device.isExposureModeSupported(.autoExpose) { device.exposureMode = .autoExpose }
            requestedFocusMode = .auto
        }

        if let focusMode = update.focusMode {
            switch focusMode {
            case .auto where device.isFocusModeSupported(.continuousAutoFocus):
                device.focusMode = .continuousAutoFocus
            case .locked where device.isFocusModeSupported(.locked):
                device.focusMode = .locked
            case .manual where device.isLockingFocusWithCustomLensPositionSupported:
                let requested = update.focus.flatMap { $0.isFinite ? $0 : nil }
                    ?? Double(device.lensPosition)
                device.setFocusModeLocked(lensPosition: Float(min(max(requested, 0), 1)))
            default:
                break
            }
            requestedFocusMode = focusMode
        } else if let focus = update.focus, focus.isFinite,
                  device.isLockingFocusWithCustomLensPositionSupported {
            device.setFocusModeLocked(lensPosition: Float(min(max(focus, 0), 1)))
            requestedFocusMode = .manual
        }

        if let exposureMode = update.exposureMode {
            switch exposureMode {
            case .auto where device.isExposureModeSupported(.continuousAutoExposure):
                device.exposureMode = .continuousAutoExposure
            case .locked where device.isExposureModeSupported(.locked):
                device.exposureMode = .locked
            case .manual where device.isExposureModeSupported(.custom):
                let duration = Self.clampedDuration(update.exposureSeconds, for: device)
                let iso = Self.clampedISO(update.iso, for: device)
                device.setExposureModeCustom(duration: duration, iso: iso)
            default:
                break
            }
        } else if update.iso != nil || update.exposureSeconds != nil, device.isExposureModeSupported(.custom) {
            device.setExposureModeCustom(
                duration: Self.clampedDuration(update.exposureSeconds, for: device),
                iso: Self.clampedISO(update.iso, for: device)
            )
        }

        if let bias = update.exposureBias, bias.isFinite {
            device.setExposureTargetBias(min(max(Float(bias), device.minExposureTargetBias), device.maxExposureTargetBias))
        }

        if let whiteBalanceMode = update.whiteBalanceMode {
            switch whiteBalanceMode {
            case .auto where device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance):
                device.whiteBalanceMode = .continuousAutoWhiteBalance
            case .locked where device.isWhiteBalanceModeSupported(.locked):
                device.whiteBalanceMode = .locked
            case .manual where device.isLockingWhiteBalanceWithCustomDeviceGainsSupported:
                let gains = Self.whiteBalanceGains(kelvin: update.whiteBalanceKelvin ?? 5_000, device: device)
                device.setWhiteBalanceModeLocked(with: gains)
            default:
                break
            }
            requestedWhiteBalanceMode = whiteBalanceMode
        } else if let kelvin = update.whiteBalanceKelvin,
                  device.isLockingWhiteBalanceWithCustomDeviceGainsSupported {
            device.setWhiteBalanceModeLocked(with: Self.whiteBalanceGains(kelvin: kelvin, device: device))
            requestedWhiteBalanceMode = .manual
        }

        if let torchEnabled = update.torchEnabled, device.hasTorch, device.isTorchAvailable {
            device.torchMode = torchEnabled ? .on : .off
        }


        if let stabilizationEnabled = update.stabilizationEnabled,
           let connection = output.connection(with: .video),
           connection.isVideoStabilizationSupported {
            connection.preferredVideoStabilizationMode = stabilizationEnabled ? .auto : .off
        }

        return controlStateForCurrentOutput(device)
    }

    private static func discoverDevices() -> [AVCaptureDevice] {
        let types: [AVCaptureDevice.DeviceType] = [
            .builtInTripleCamera,
            .builtInDualWideCamera,
            .builtInDualCamera,
            .builtInUltraWideCamera,
            .builtInWideAngleCamera,
            .builtInTelephotoCamera,
            .builtInTrueDepthCamera
        ]
        return AVCaptureDevice.DiscoverySession(
            deviceTypes: types,
            mediaType: .video,
            position: .unspecified
        ).devices.sorted { deviceRank($0) < deviceRank($1) }
    }

    private static func descriptor(_ device: AVCaptureDevice) -> CameraDescriptor {
        CameraDescriptor(
            id: device.uniqueID,
            name: device.localizedName,
            position: device.position == .front ? .front : .back,
            lens: lens(for: device.deviceType)
        )
    }

    private static func lens(for type: AVCaptureDevice.DeviceType) -> CameraLens {
        switch type {
        case .builtInUltraWideCamera: .ultraWide
        case .builtInWideAngleCamera, .builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera: .wide
        case .builtInTelephotoCamera: .telephoto
        case .builtInTrueDepthCamera: .trueDepth
        default: .other
        }
    }

    private static func isVirtualMultiLens(_ type: AVCaptureDevice.DeviceType) -> Bool {
        type == .builtInTripleCamera || type == .builtInDualWideCamera || type == .builtInDualCamera
    }

    private static func deviceRank(_ device: AVCaptureDevice) -> Int {
        if device.position == .back, isVirtualMultiLens(device.deviceType) { return 0 }
        if device.position == .back, device.deviceType == .builtInUltraWideCamera { return 1 }
        if device.position == .back, device.deviceType == .builtInWideAngleCamera { return 2 }
        if device.position == .back, device.deviceType == .builtInTelephotoCamera { return 3 }
        return device.position == .front ? 10 : 9
    }

    private static func selectFormat(on device: AVCaptureDevice, configuration: StreamConfiguration) throws {
        let candidates = device.formats.compactMap { format -> (AVCaptureDevice.Format, Int, Int)? in
            let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            guard dimensions.width == configuration.width,
                  dimensions.height == configuration.height,
                  format.videoSupportedFrameRateRanges.contains(where: {
                      $0.minFrameRate <= Double(configuration.framesPerSecond)
                          && Double(configuration.framesPerSecond) <= $0.maxFrameRate
                  }) else {
                return nil
            }
            return (format, Int(dimensions.width), Int(dimensions.height))
        }
        guard let format = candidates.first?.0 else {
            throw CaptureEngineError.unsupportedFormat(
                width: configuration.width,
                height: configuration.height,
                fps: configuration.framesPerSecond
            )
        }

        try device.lockForConfiguration()
        defer { device.unlockForConfiguration() }
        device.activeFormat = format
        let duration = CMTime(value: 1, timescale: CMTimeScale(configuration.framesPerSecond))
        device.activeVideoMinFrameDuration = duration
        device.activeVideoMaxFrameDuration = duration
        if device.isFocusModeSupported(.continuousAutoFocus) { device.focusMode = .continuousAutoFocus }
        if device.isExposureModeSupported(.continuousAutoExposure) { device.exposureMode = .continuousAutoExposure }
        if device.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) { device.whiteBalanceMode = .continuousAutoWhiteBalance }
    }

    private static func clampedDuration(_ seconds: Double?, for device: AVCaptureDevice) -> CMTime {
        guard let seconds, seconds.isFinite, seconds > 0 else { return device.exposureDuration }
        let requested = CMTime(seconds: seconds, preferredTimescale: 1_000_000_000)
        return CMTimeMaximum(device.activeFormat.minExposureDuration, CMTimeMinimum(requested, device.activeFormat.maxExposureDuration))
    }

    private static func clampedISO(_ iso: Double?, for device: AVCaptureDevice) -> Float {
        guard let iso, iso.isFinite else { return device.iso }
        return min(max(Float(iso), device.activeFormat.minISO), device.activeFormat.maxISO)
    }

    private static func whiteBalanceGains(kelvin: Double, device: AVCaptureDevice) -> AVCaptureDevice.WhiteBalanceGains {
        let kelvin = kelvin.isFinite ? kelvin : 5_000
        let values = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(
            temperature: Float(min(max(kelvin, 2_000), 10_000)),
            tint: 0
        )
        var gains = device.deviceWhiteBalanceGains(for: values)
        let maximum = device.maxWhiteBalanceGain
        gains.redGain = min(max(gains.redGain, 1), maximum)
        gains.greenGain = min(max(gains.greenGain, 1), maximum)
        gains.blueGain = min(max(gains.blueGain, 1), maximum)
        return gains
    }

    private static func controlState(for device: AVCaptureDevice) -> CameraControlState {
        let temperature = device.temperatureAndTintValues(for: device.deviceWhiteBalanceGains).temperature
        return CameraControlState(
            deviceID: device.uniqueID,
            position: device.position == .front ? .front : .back,
            lens: lens(for: device.deviceType),
            zoom: Double(device.videoZoomFactor),
            minimumZoom: Double(device.minAvailableVideoZoomFactor),
            maximumZoom: Double(min(device.maxAvailableVideoZoomFactor, 20)),
            focusMode: device.focusMode == .locked ? .locked : .auto,
            focus: Double(device.lensPosition),
            exposureMode: device.exposureMode == .custom ? .manual : (device.exposureMode == .locked ? .locked : .auto),
            iso: Double(device.iso),
            minimumISO: Double(device.activeFormat.minISO),
            maximumISO: Double(device.activeFormat.maxISO),
            exposureSeconds: CMTimeGetSeconds(device.exposureDuration),
            minimumExposureSeconds: CMTimeGetSeconds(device.activeFormat.minExposureDuration),
            maximumExposureSeconds: CMTimeGetSeconds(device.activeFormat.maxExposureDuration),
            exposureBias: Double(device.exposureTargetBias),
            minimumExposureBias: Double(device.minExposureTargetBias),
            maximumExposureBias: Double(device.maxExposureTargetBias),
            whiteBalanceMode: device.whiteBalanceMode == .locked ? .locked : .auto,
            whiteBalanceKelvin: Double(temperature),
            torchEnabled: device.torchMode == .on,
            torchAvailable: device.hasTorch && device.isTorchAvailable
        )
    }

    private func controlStateForCurrentOutput(_ device: AVCaptureDevice) -> CameraControlState {
        var state = Self.controlState(for: device)
        state.focusMode = requestedFocusMode
        state.whiteBalanceMode = requestedWhiteBalanceMode
        if let mode = output.connection(with: .video)?.preferredVideoStabilizationMode {
            state.stabilizationEnabled = mode != .off
        }
        return state
    }
}

private final class SampleBufferRelay: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let handler: @Sendable (CMSampleBuffer) -> Void

    init(handler: @escaping @Sendable (CMSampleBuffer) -> Void) {
        self.handler = handler
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        handler(sampleBuffer)
    }
}
