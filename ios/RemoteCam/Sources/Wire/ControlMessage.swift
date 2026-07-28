import Foundation
import UIKit

struct ControlMessage: Equatable, Sendable {
    let type: String
    var fields: [String: CBORValue]

    init(type: String, fields: [String: CBORValue] = [:]) {
        self.type = type
        self.fields = fields
    }

    init(payload: Data) throws {
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
}
