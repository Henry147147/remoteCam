@preconcurrency import AVFoundation
@preconcurrency import VideoToolbox
import Foundation
import OSLog

enum RemoteCamLog {
    private static let logger = Logger(subsystem: "org.remotecam.ios", category: "runtime")

    static func debug(_ category: String, _ message: String) {
        record(level: .debug, category: category, message: message)
    }

    static func info(_ category: String, _ message: String) {
        record(level: .info, category: category, message: message)
    }

    static func error(_ category: String, _ message: String) {
        record(level: .error, category: category, message: message)
    }

    private static func record(level: OSLogType, category: String, message: String) {
        let line = "[\(category)] \(message)"
        logger.log(level: level, "\(line, privacy: .public)")
#if DEBUG
        print("[RemoteCam]\(line)")
#endif
    }
}

struct EncodedAccessUnit: Sendable {
    let data: Data
    let presentationTimeMicros: UInt64
    let isKeyframe: Bool
}

enum VideoEncoderError: LocalizedError {
    case cannotCreate(OSStatus)
    case propertyFailed(String, OSStatus)
    case missingImageBuffer

    var errorDescription: String? {
        switch self {
        case .cannotCreate(let status): "VideoToolbox could not create an encoder (\(status))."
        case .propertyFailed(let property, let status): "VideoToolbox rejected \(property) (\(status))."
        case .missingImageBuffer: "The camera frame did not contain an image buffer."
        }
    }
}

final class VideoEncoder: @unchecked Sendable {
    private let queue = DispatchQueue(label: "org.remotecam.encoder", qos: .userInteractive)
    private var session: VTCompressionSession?
    private var configuration: StreamConfiguration?
    private var forceKeyframe = false
    private var encodedFrameCount: UInt64 = 0
    private var encodedByteCount: UInt64 = 0

    var onAccessUnit: (@Sendable (EncodedAccessUnit) -> Void)?
    var onError: (@Sendable (Error) -> Void)?

    func configure(_ configuration: StreamConfiguration) throws {
        try queue.sync {
            invalidateLocked()
            let configuration = try configuration.validated()
            RemoteCamLog.info(
                "encoder",
                "configuring \(configuration.codec.rawValue) \(configuration.width)x\(configuration.height) " +
                    "at \(configuration.framesPerSecond) fps, bitrate=\(configuration.bitrate)"
            )
            let codec: CMVideoCodecType = configuration.codec == .hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
            let encoderSpecification = [
                kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true
            ] as CFDictionary
            var created: VTCompressionSession?
            let status = VTCompressionSessionCreate(
                allocator: kCFAllocatorDefault,
                width: Int32(configuration.width),
                height: Int32(configuration.height),
                codecType: codec,
                encoderSpecification: encoderSpecification,
                imageBufferAttributes: nil,
                compressedDataAllocator: nil,
                outputCallback: Self.outputCallback,
                refcon: Unmanaged.passUnretained(self).toOpaque(),
                compressionSessionOut: &created
            )
            guard status == noErr, let created else { throw VideoEncoderError.cannotCreate(status) }
            session = created
            self.configuration = configuration
            do {
                try set(kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue, on: created)
                try set(kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse, on: created)
                try set(kVTCompressionPropertyKey_ExpectedFrameRate, value: configuration.framesPerSecond as CFNumber, on: created)
                try set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: 2 as CFNumber, on: created)
                try applyBitrateLocked(configuration.bitrate)
                if configuration.codec == .h264 {
                    try set(kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel, on: created)
                }
                let prepareStatus = VTCompressionSessionPrepareToEncodeFrames(created)
                guard prepareStatus == noErr else { throw VideoEncoderError.cannotCreate(prepareStatus) }
                encodedFrameCount = 0
                encodedByteCount = 0
                RemoteCamLog.info("encoder", "VideoToolbox session ready")
            } catch {
                RemoteCamLog.error("encoder", "configuration failed: \(error.localizedDescription)")
                invalidateLocked()
                throw error
            }
        }
    }

    func encode(_ sampleBuffer: CMSampleBuffer) {
        let box = SendableSampleBuffer(sampleBuffer)
        queue.async { [weak self, box] in
            guard let self, let session = self.session else { return }
            let sampleBuffer = box.value
            guard let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
                self.onError?(VideoEncoderError.missingImageBuffer)
                return
            }
            let frameProperties: CFDictionary? = self.forceKeyframe
                ? [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary
                : nil
            self.forceKeyframe = false
            let status = VTCompressionSessionEncodeFrame(
                session,
                imageBuffer: imageBuffer,
                presentationTimeStamp: CMSampleBufferGetPresentationTimeStamp(sampleBuffer),
                duration: CMSampleBufferGetDuration(sampleBuffer),
                frameProperties: frameProperties,
                sourceFrameRefcon: nil,
                infoFlagsOut: nil
            )
            if status != noErr { self.onError?(VideoEncoderError.cannotCreate(status)) }
        }
    }

    func updateBitrate(_ bitrate: Int) {
        queue.async { [weak self] in
            do {
                try self?.applyBitrateLocked(bitrate)
                RemoteCamLog.info("encoder", "bitrate updated to \(bitrate)")
            } catch {
                RemoteCamLog.error("encoder", "bitrate update failed: \(error.localizedDescription)")
                self?.onError?(error)
            }
        }
    }

    func requestKeyframe() {
        queue.async { [weak self] in
            self?.forceKeyframe = true
            RemoteCamLog.debug("encoder", "keyframe requested")
        }
    }

    func invalidate() {
        queue.sync { invalidateLocked() }
    }

    private func applyBitrateLocked(_ bitrate: Int) throws {
        guard let session else { return }
        guard (StreamConfiguration.minimumBitrate...StreamConfiguration.maximumBitrate).contains(bitrate) else {
            throw StreamConfigurationError.invalidBitrate
        }
        try set(kVTCompressionPropertyKey_AverageBitRate, value: bitrate as CFNumber, on: session)
        let bytesPerSecond = max(bitrate / 8, 1)
        try set(
            kVTCompressionPropertyKey_DataRateLimits,
            value: [bytesPerSecond, 1] as CFArray,
            on: session
        )
    }

    private func set(_ key: CFString, value: CFTypeRef, on session: VTCompressionSession) throws {
        let status = VTSessionSetProperty(session, key: key, value: value)
        guard status == noErr else { throw VideoEncoderError.propertyFailed(key as String, status) }
    }

    private func invalidateLocked() {
        guard let session else { return }
        RemoteCamLog.info(
            "encoder",
            "invalidating after \(encodedFrameCount) access units / \(encodedByteCount) bytes"
        )
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        self.session = nil
        configuration = nil
    }

    private static let outputCallback: VTCompressionOutputCallback = { refcon, _, status, _, sampleBuffer in
        guard status == noErr,
              let refcon,
              let sampleBuffer,
              CMSampleBufferDataIsReady(sampleBuffer) else { return }
        let encoder = Unmanaged<VideoEncoder>.fromOpaque(refcon).takeUnretainedValue()
        do {
            let isKeyframe = VideoEncoder.isKeyframe(sampleBuffer)
            let data = try VideoEncoder.annexBData(from: sampleBuffer, includeParameterSets: isKeyframe)
            let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            let micros = UInt64(max(CMTimeGetSeconds(pts), 0) * 1_000_000)
            encoder.encodedFrameCount &+= 1
            encoder.encodedByteCount &+= UInt64(data.count)
            if encoder.encodedFrameCount == 1 || encoder.encodedFrameCount.isMultiple(of: 120) {
                RemoteCamLog.info(
                    "encoder",
                    "encoded access units=\(encoder.encodedFrameCount), bytes=\(encoder.encodedByteCount), " +
                        "latest_keyframe=\(isKeyframe)"
                )
            } else if isKeyframe {
                RemoteCamLog.debug("encoder", "encoded keyframe bytes=\(data.count)")
            }
            encoder.onAccessUnit?(EncodedAccessUnit(data: data, presentationTimeMicros: micros, isKeyframe: isKeyframe))
        } catch {
            RemoteCamLog.error("encoder", "output conversion failed: \(error.localizedDescription)")
            encoder.onError?(error)
        }
    }

    private static func isKeyframe(_ sampleBuffer: CMSampleBuffer) -> Bool {
        guard let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false),
              CFArrayGetCount(attachments) > 0 else { return true }
        let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFDictionary.self)
        return !CFDictionaryContainsKey(
            dictionary,
            Unmanaged.passUnretained(kCMSampleAttachmentKey_NotSync).toOpaque()
        )
    }

    private static func annexBData(from sampleBuffer: CMSampleBuffer, includeParameterSets: Bool) throws -> Data {
        var result = Data()
        if includeParameterSets, let format = CMSampleBufferGetFormatDescription(sampleBuffer) {
            appendParameterSets(from: format, to: &result)
        }
        guard let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return result }
        var totalLength = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        let status = CMBlockBufferGetDataPointer(
            block,
            atOffset: 0,
            lengthAtOffsetOut: nil,
            totalLengthOut: &totalLength,
            dataPointerOut: &dataPointer
        )
        guard status == kCMBlockBufferNoErr, let dataPointer else { throw VideoEncoderError.cannotCreate(status) }

        var offset = 0
        while offset + 4 <= totalLength {
            let lengthBytes = UnsafeRawBufferPointer(start: dataPointer + offset, count: 4)
            let length = lengthBytes.reduce(into: UInt32.zero) { $0 = ($0 << 8) | UInt32($1) }
            offset += 4
            guard length > 0, offset + Int(length) <= totalLength else { break }
            result.append(contentsOf: [0, 0, 0, 1])
            result.append(UnsafeRawBufferPointer(start: dataPointer + offset, count: Int(length)).bindMemory(to: UInt8.self))
            offset += Int(length)
        }
        return result
    }

    private static func appendParameterSets(from format: CMFormatDescription, to data: inout Data) {
        let codec = CMFormatDescriptionGetMediaSubType(format)
        if codec == kCMVideoCodecType_H264 {
            var count = 0
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
                      let pointer else { continue }
                data.append(contentsOf: [0, 0, 0, 1])
                data.append(pointer, count: size)
            }
        } else if codec == kCMVideoCodecType_HEVC {
            var count = 0
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            for index in 0..<count {
                var pointer: UnsafePointer<UInt8>?
                var size = 0
                guard CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr,
                      let pointer else { continue }
                data.append(contentsOf: [0, 0, 0, 1])
                data.append(pointer, count: size)
            }
        }
    }

    deinit {
        invalidateLocked()
    }
}

private final class SendableSampleBuffer: @unchecked Sendable {
    let value: CMSampleBuffer

    init(_ value: CMSampleBuffer) {
        self.value = value
    }
}
