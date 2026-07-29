import Foundation

enum VideoCodec: String, CaseIterable, Codable, Sendable {
    case hevc
    case h264
}

enum StreamConfigurationError: LocalizedError, Equatable {
    case invalidDimensions
    case invalidFrameRate
    case invalidBitrate

    var errorDescription: String? {
        switch self {
        case .invalidDimensions:
            "The requested video dimensions are unsupported."
        case .invalidFrameRate:
            "The requested frame rate is unsupported."
        case .invalidBitrate:
            "The requested video bitrate is unsupported."
        }
    }
}

struct StreamConfiguration: Codable, Equatable, Sendable {
    static let maximumDimension = 4_096
    static let maximumPixelCount = 3_840 * 2_160
    static let minimumBitrate = 64_000
    static let maximumBitrate = 100_000_000

    var codec: VideoCodec
    var width: Int
    var height: Int
    var framesPerSecond: Int
    var bitrate: Int

    func validated() throws -> StreamConfiguration {
        guard width > 0, height > 0,
              width <= Self.maximumDimension, height <= Self.maximumDimension,
              width.isMultiple(of: 2), height.isMultiple(of: 2),
              width <= Self.maximumPixelCount / height else {
            throw StreamConfigurationError.invalidDimensions
        }
        guard (1...120).contains(framesPerSecond) else {
            throw StreamConfigurationError.invalidFrameRate
        }
        guard (Self.minimumBitrate...Self.maximumBitrate).contains(bitrate) else {
            throw StreamConfigurationError.invalidBitrate
        }
        return self
    }

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
