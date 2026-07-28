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
}
