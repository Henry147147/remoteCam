import Foundation
import UIKit

struct ControlMessage: Equatable, Sendable {
    static let maximumPayloadLength = 1_024 * 1_024
    let type: String
    var fields: [String: CBORValue]

    init(type: String, fields: [String: CBORValue] = [:]) {
        self.type = type
        self.fields = fields
    }

    init(payload: Data) throws {
        guard payload.count <= Self.maximumPayloadLength else { throw ControlMessageError.payloadTooLarge }
        guard case .map(var map) = try CBORDecoder.decode(payload),
              let type = map.removeValue(forKey: "t")?.stringValue else {
            throw ControlMessageError.invalidMap
        }
        self.type = type
        fields = map
    }

    func encoded() -> Data {
        var map = fields
        map["t"] = .string(type)
        return CBOREncoder.encode(.map(map))
    }
}

enum ControlMessageError: Error, Equatable {
    case invalidMap
    case payloadTooLarge
}

extension ControlMessage {
    @MainActor
    static func hello(deviceID: String) -> ControlMessage {
        ControlMessage(type: "hello", fields: [
            "v": .unsigned(1),
            "device_name": .string(UIDevice.current.name),
            "device_id": .string(deviceID),
            "platform": .string("ios"),
            "model": .string(UIDevice.current.model),
            "caps": .array([.string("h264"), .string("hevc")])
        ])
    }

    static func streamStart() -> ControlMessage {
        ControlMessage(type: "stream_start")
    }

    static func capabilities(_ capabilities: [CameraCapability]) -> ControlMessage {
        let cameras = capabilities.map { capability in
            CBORValue.map([
                "id": .string(capability.camera.id),
                "name": .string(capability.camera.name),
                "position": .string(capability.camera.position.rawValue),
                "lens": .string(capability.camera.lens.rawValue),
                "formats": .array(capability.formats.sorted {
                    ($0.width, $0.height, $0.framesPerSecond) < ($1.width, $1.height, $1.framesPerSecond)
                }.map { format in
                    .map([
                        "width": .unsigned(UInt64(format.width)),
                        "height": .unsigned(UInt64(format.height)),
                        "fps": .unsigned(UInt64(format.framesPerSecond))
                    ])
                })
            ])
        }
        return ControlMessage(type: "caps", fields: [
            "cameras": .array(cameras),
            "codecs": .array(VideoCodec.allCases.map { .string($0.rawValue) })
        ])
    }

    static func cameraState(_ state: CameraControlState) -> ControlMessage {
        ControlMessage(type: "camera_state", fields: [
            "device_id": state.deviceID.map(CBORValue.string) ?? .null,
            "position": .string(state.position.rawValue),
            "lens": .string(state.lens.rawValue),
            "zoom": .double(state.zoom),
            "focus_mode": .string(state.focusMode.rawValue),
            "focus": .double(state.focus),
            "exposure_mode": .string(state.exposureMode.rawValue),
            "iso": .double(state.iso),
            "exposure": .double(state.exposureSeconds),
            "ev": .double(state.exposureBias),
            "wb_mode": .string(state.whiteBalanceMode.rawValue),
            "wb": .double(state.whiteBalanceKelvin),
            "torch": .boolean(state.torchEnabled),
            "stabilization": .boolean(state.stabilizationEnabled)
        ])
    }

    static func orientation(degrees: Double) -> ControlMessage {
        ControlMessage(type: "orientation", fields: [
            "deg": .double(degrees),
            "locked": .boolean(false)
        ])
    }

    static func thermal(_ state: String) -> ControlMessage {
        ControlMessage(type: "thermal", fields: ["state": .string(state)])
    }

    static func battery(level: Double, charging: Bool) -> ControlMessage {
        ControlMessage(type: "battery", fields: [
            "level": .double(level),
            "charging": .boolean(charging)
        ])
    }
}
