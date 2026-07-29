import XCTest

final class ControlMessageTests: XCTestCase {
    func testCameraStateUsesDocumentedUnitsAndKeys() throws {
        var state = CameraControlState()
        state.deviceID = "camera-1"
        state.iso = 125
        state.exposureSeconds = 1.0 / 60.0
        state.whiteBalanceKelvin = 5_600
        state.torchEnabled = true

        let decoded = try ControlMessage(payload: ControlMessage.cameraState(state).encoded())
        XCTAssertEqual(decoded.type, "camera_state")
        XCTAssertEqual(decoded.fields["device_id"]?.stringValue, "camera-1")
        XCTAssertEqual(decoded.fields["torch"]?.boolValue, true)
        XCTAssertEqual(decoded.fields["iso"], .double(125))
        XCTAssertEqual(decoded.fields["exposure"], .double(1.0 / 60.0))
        XCTAssertEqual(decoded.fields["wb"], .double(5_600))
    }

    func testCapabilitiesIncludeEveryCameraFormat() throws {
        let camera = CameraDescriptor(id: "back-wide", name: "Wide", position: .back, lens: .wide)
        let capabilities = [CameraCapability(camera: camera, formats: [
            CaptureFormatDescriptor(width: 1_920, height: 1_080, framesPerSecond: 60),
            CaptureFormatDescriptor(width: 1_280, height: 720, framesPerSecond: 30)
        ])]

        let decoded = try ControlMessage(payload: ControlMessage.capabilities(capabilities).encoded())
        guard case .array(let cameras) = decoded.fields["cameras"],
              case .map(let first) = cameras.first,
              case .array(let formats) = first["formats"] else {
            return XCTFail("Missing nested capability arrays")
        }
        XCTAssertEqual(first["position"], .string("back"))
        XCTAssertEqual(first["lens"], .string("wide"))
        XCTAssertEqual(formats.count, 2)
    }

    func testRejectsOversizedControlPayload() {
        XCTAssertThrowsError(try ControlMessage(payload: Data(repeating: 0, count: ControlMessage.maximumPayloadLength + 1))) { error in
            XCTAssertEqual(error as? ControlMessageError, .payloadTooLarge)
        }
    }

    func testRejectsHostileStreamConfigurationsBeforeFrameworkConversions() {
        let cases = [
            StreamConfiguration(codec: .h264, width: Int.max, height: 1_080, framesPerSecond: 30, bitrate: 4_000_000),
            StreamConfiguration(codec: .h264, width: 1_919, height: 1_080, framesPerSecond: 30, bitrate: 4_000_000),
            StreamConfiguration(codec: .h264, width: 1_920, height: 1_080, framesPerSecond: Int.max, bitrate: 4_000_000),
            StreamConfiguration(codec: .h264, width: 1_920, height: 1_080, framesPerSecond: 30, bitrate: Int.max)
        ]

        for configuration in cases {
            XCTAssertThrowsError(try configuration.validated())
        }
        XCTAssertNoThrow(try StreamConfiguration.default1080p.validated())
        XCTAssertNoThrow(try StreamConfiguration.thermalFallback.validated())
    }
}
