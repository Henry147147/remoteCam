import ActivityKit
import Foundation

struct RemoteCamActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var hostName: String
        var resolution: String
        var framesPerSecond: Int
        var status: String
    }

    let startedAt: Date
}
