import AVFoundation
import XCTest

final class VideoEncoderTests: XCTestCase {
    func testRejectsInvalidDimensionsWithoutNarrowingTrap() {
        let encoder = VideoEncoder()
        XCTAssertThrowsError(try encoder.configure(StreamConfiguration(
            codec: .h264,
            width: Int.max,
            height: 1_080,
            framesPerSecond: 30,
            bitrate: 1_000_000
        ))) { error in
            XCTAssertEqual(error as? StreamConfigurationError, .invalidDimensions)
        }
    }

    func testH264ProducesAnnexBKeyframe() throws {
        let encoder = VideoEncoder()
        let encoded = expectation(description: "VideoToolbox encoded a frame")
        let result = AccessUnitBox()
        encoder.onAccessUnit = { accessUnit in
            result.value = accessUnit
            encoded.fulfill()
        }

        do {
            try encoder.configure(StreamConfiguration(
                codec: .h264,
                width: 640,
                height: 480,
                framesPerSecond: 30,
                bitrate: 1_000_000
            ))
        } catch {
            throw XCTSkip("No H.264 encoder is available on this simulator: \(error.localizedDescription)")
        }

        encoder.encode(try makeSampleBuffer(width: 640, height: 480))
        wait(for: [encoded], timeout: 5)
        encoder.invalidate()

        let accessUnit = try XCTUnwrap(result.value)
        XCTAssertTrue(accessUnit.isKeyframe)
        XCTAssertGreaterThan(accessUnit.data.count, 4)
        XCTAssertEqual(Array(accessUnit.data.prefix(4)), [0, 0, 0, 1])
        XCTAssertGreaterThanOrEqual(startCodeCount(in: accessUnit.data), 3, "A keyframe must contain SPS, PPS, and picture NAL units")
    }

    private func makeSampleBuffer(width: Int, height: Int) throws -> CMSampleBuffer {
        var pixelBuffer: CVPixelBuffer?
        let attributes: [String: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey as String: [:],
            kCVPixelBufferMetalCompatibilityKey as String: true
        ]
        XCTAssertEqual(CVPixelBufferCreate(
            kCFAllocatorDefault,
            width,
            height,
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            attributes as CFDictionary,
            &pixelBuffer
        ), kCVReturnSuccess)
        let buffer = try XCTUnwrap(pixelBuffer)

        CVPixelBufferLockBaseAddress(buffer, [])
        if let yPlane = CVPixelBufferGetBaseAddressOfPlane(buffer, 0) {
            memset(yPlane, 96, CVPixelBufferGetBytesPerRowOfPlane(buffer, 0) * height)
        }
        if let uvPlane = CVPixelBufferGetBaseAddressOfPlane(buffer, 1) {
            memset(uvPlane, 128, CVPixelBufferGetBytesPerRowOfPlane(buffer, 1) * height / 2)
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])

        var format: CMVideoFormatDescription?
        XCTAssertEqual(CMVideoFormatDescriptionCreateForImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: buffer,
            formatDescriptionOut: &format
        ), noErr)
        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 30),
            presentationTimeStamp: CMTime(value: 1, timescale: 30),
            decodeTimeStamp: .invalid
        )
        var sampleBuffer: CMSampleBuffer?
        XCTAssertEqual(CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: buffer,
            formatDescription: try XCTUnwrap(format),
            sampleTiming: &timing,
            sampleBufferOut: &sampleBuffer
        ), noErr)
        return try XCTUnwrap(sampleBuffer)
    }

    private func startCodeCount(in data: Data) -> Int {
        guard data.count >= 4 else { return 0 }
        return (0...(data.count - 4)).reduce(into: 0) { count, offset in
            if data[offset] == 0, data[offset + 1] == 0, data[offset + 2] == 0, data[offset + 3] == 1 {
                count += 1
            }
        }
    }
}

private final class AccessUnitBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: EncodedAccessUnit?

    var value: EncodedAccessUnit? {
        get { lock.withLock { stored } }
        set { lock.withLock { stored = newValue } }
    }
}
