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

    func testDecodesFloat16AndFloat32() throws {
        XCTAssertEqual(try CBORDecoder.decode(Data([0xf9, 0x3e, 0x00])), .double(1.5))
        XCTAssertEqual(try CBORDecoder.decode(Data([0xfa, 0x42, 0x48, 0x00, 0x00])), .double(50.0))
    }

    func testRejectsDuplicateMapKeys() {
        // {"t": 1, "t": 2} is ambiguous and cannot be safely authenticated.
        let duplicate = Data([0xa2, 0x61, 0x74, 0x01, 0x61, 0x74, 0x02])
        XCTAssertThrowsError(try CBORDecoder.decode(duplicate)) { error in
            XCTAssertEqual(error as? CBORError, .duplicateMapKey("t"))
        }
    }

    func testRejectsNonMinimalIntegersAndLengths() {
        for encoded in [
            Data([0x18, 0x17]),                         // 23 encoded in one extra byte
            Data([0x19, 0x00, 0xff]),                   // 255 encoded in two extra bytes
            Data([0x1a, 0x00, 0x00, 0xff, 0xff]),       // 65535 encoded in four bytes
            Data([0x1b, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff]),
            Data([0x78, 0x01, 0x61])                    // one-byte string with wide length
        ] {
            XCTAssertThrowsError(try CBORDecoder.decode(encoded)) { error in
                XCTAssertEqual(error as? CBORError, .nonMinimalInteger)
            }
        }
    }
}
