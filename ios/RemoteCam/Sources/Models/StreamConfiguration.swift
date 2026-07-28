import Foundation

enum VideoCodec: String, CaseIterable, Codable, Sendable {
    case hevc
    case h264
}

struct StreamConfiguration: Codable, Equatable, Sendable {
    var codec: VideoCodec
    var width: Int
    var height: Int
    var framesPerSecond: Int
    var bitrate: Int

    static let default1080p = StreamConfiguration(
        codec: .hevc,
        width: 1_920,
        height: 1_080,
        framesPerSecond: 60,
        bitrate: 12_000_000
    )

    static let thermalFallback = StreamConfiguration(
        codec: .h264,
        width: 1_280,
        height: 720,
        framesPerSecond: 30,
        bitrate: 4_000_000
    )
}

enum ConnectionPhase: Equatable, Sendable {
    case idle
    case connecting(RemoteHost)
    case awaitingPairing(RemoteHost)
    case ready(RemoteHost, StreamConfiguration)
    case streaming(RemoteHost, StreamConfiguration)
    case reconnecting(RemoteHost, attempt: Int)
    case failed(String)

    var host: RemoteHost? {
        switch self {
        case .connecting(let host), .awaitingPairing(let host),
                .ready(let host, _), .streaming(let host, _),
                .reconnecting(let host, _):
            return host
        case .idle, .failed:
            return nil
        }
    }
}
