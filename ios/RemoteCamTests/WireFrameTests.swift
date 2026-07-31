import XCTest

final class WireFrameTests: XCTestCase {
    func testRoundTripAcrossArbitraryTCPChunks() throws {
        let expected = [
            WireFrame(channel: .control, presentationTimeMicros: 123, payload: Data([1, 2, 3])),
            WireFrame(channel: .video, flags: .keyframe, presentationTimeMicros: 9_876_543, payload: Data(repeating: 0xa5, count: 2_049)),
            WireFrame(channel: .audio, presentationTimeMicros: 0, payload: Data())
        ]
        let stream = try expected.reduce(into: Data()) { $0.append(try $1.encoded()) }
        var decoder = WireFrameDecoder()
        var actual: [WireFrame] = []
        var cursor = 0
        for size in [1, 7, 13, 256, 1_024, stream.count] where cursor < stream.count {
            let end = min(cursor + size, stream.count)
            actual.append(contentsOf: try decoder.append(stream.subdata(in: cursor..<end)))
            cursor = end
        }
        XCTAssertEqual(actual, expected)
    }

    func testHeaderUsesBigEndianLayout() throws {
        let frame = WireFrame(channel: .stats, flags: [.encrypted], presentationTimeMicros: 0x0102_0304_0506_0708, payload: Data([0xaa, 0xbb]))
        XCTAssertEqual(
            Array(try frame.encoded().prefix(16)),
            [0, 0, 0, 2, 3, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]
        )
    }

    func testRejectsOversizedPayloadBeforeBufferingBody() {
        var decoder = WireFrameDecoder()
        let advertised = UInt32(WireFrame.maximumPayloadLength + 1)
        let header = Data([
            UInt8(truncatingIfNeeded: advertised >> 24), UInt8(truncatingIfNeeded: advertised >> 16),
            UInt8(truncatingIfNeeded: advertised >> 8), UInt8(truncatingIfNeeded: advertised),
            1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        ])
        XCTAssertThrowsError(try decoder.append(header)) { error in
            XCTAssertEqual(error as? WireFrameError, .payloadTooLarge)
        }
    }

    func testRejectsFragmentationFlagInV1() {
        XCTAssertThrowsError(try WireFrame(
            channel: .control,
            flags: .endOfFragment,
            presentationTimeMicros: 0,
            payload: Data()
        ).encoded()) { error in
            XCTAssertEqual(error as? WireFrameError, .fragmentationUnsupported)
        }

        var decoder = WireFrameDecoder()
        let header = Data([0, 0, 0, 0, 0, WireFlags.endOfFragment.rawValue,
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
        XCTAssertThrowsError(try decoder.append(header)) { error in
            XCTAssertEqual(error as? WireFrameError, .fragmentationUnsupported)
        }
    }

    func testRejectsKeyframeFlagOutsideVideoChannel() {
        XCTAssertThrowsError(try WireFrame(
            channel: .control,
            flags: .keyframe,
            presentationTimeMicros: 0,
            payload: Data()
        ).encoded()) { error in
            XCTAssertEqual(error as? WireFrameError, .invalidChannelFlags)
        }
    }
}
