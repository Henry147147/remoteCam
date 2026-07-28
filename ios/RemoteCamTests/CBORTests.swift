import XCTest

final class CBORTests: XCTestCase {
    func testRoundTripsSupportedValues() throws {
        let value = CBORValue.map([
            "text": .string("RemoteCam 🎥"),
            "count": .unsigned(65_536),
            "negative": .negative(-42),
            "bytes": .bytes(Data([0, 1, 2, 255])),
            "array": .array([.boolean(true), .null, .double(60.0)])
        ])
        XCTAssertEqual(try CBORDecoder.decode(CBOREncoder.encode(value)), value)
    }

    func testCanonicalMapEncodingIsStable() {
        let first = CBOREncoder.encode(.map(["codec": .string("hevc"), "t": .string("ready")]))
        let second = CBOREncoder.encode(.map(["t": .string("ready"), "codec": .string("hevc")]))
        XCTAssertEqual(first, second)
    }

    func testRejectsTrailingBytes() {
        XCTAssertThrowsError(try CBORDecoder.decode(Data([0x01, 0x02]))) { error in
            XCTAssertEqual(error as? CBORError, .trailingBytes)
        }
    }
}
