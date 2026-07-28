import Foundation

enum WireChannel: UInt8, Sendable {
    case control = 0
    case video = 1
    case audio = 2
    case stats = 3
}

struct WireFlags: OptionSet, Equatable, Sendable {
    let rawValue: UInt8

    static let keyframe = WireFlags(rawValue: 1 << 0)
    static let encrypted = WireFlags(rawValue: 1 << 1)
    static let endOfFragment = WireFlags(rawValue: 1 << 2)
    static let knownMask: UInt8 = 0b0000_0111
}

struct WireFrame: Equatable, Sendable {
    static let headerLength = 16
    static let maximumPayloadLength = 16 * 1_024 * 1_024

    let channel: UInt8
    let flags: WireFlags
    let presentationTimeMicros: UInt64
    let payload: Data

    init(channel: WireChannel, flags: WireFlags = [], presentationTimeMicros: UInt64, payload: Data) {
        self.init(channel: channel.rawValue, flags: flags, presentationTimeMicros: presentationTimeMicros, payload: payload)
    }

    init(channel: UInt8, flags: WireFlags = [], presentationTimeMicros: UInt64, payload: Data) {
        self.channel = channel
        self.flags = flags
        self.presentationTimeMicros = presentationTimeMicros
        self.payload = payload
    }

    func encoded() throws -> Data {
        guard payload.count <= Self.maximumPayloadLength else { throw WireFrameError.payloadTooLarge }
        guard flags.rawValue & ~WireFlags.knownMask == 0 else { throw WireFrameError.reservedFlagsSet }

        var data = Data(capacity: Self.headerLength + payload.count)
        data.appendBigEndian(UInt32(payload.count))
        data.append(channel)
        data.append(flags.rawValue)
        data.appendBigEndian(UInt16(0))
        data.appendBigEndian(presentationTimeMicros)
        data.append(payload)
        return data
    }
}

enum WireFrameError: Error, Equatable {
    case payloadTooLarge
    case reservedFlagsSet
    case reservedHeaderNonZero
}

struct WireFrameDecoder: Sendable {
    private var buffer = Data()
    private var readOffset = 0

    mutating func append(_ bytes: Data) throws -> [WireFrame] {
        buffer.append(bytes)
        var frames: [WireFrame] = []

        while buffer.count - readOffset >= WireFrame.headerLength {
            let payloadLength = Int(buffer.readBigEndian(UInt32.self, at: readOffset))
            guard payloadLength <= WireFrame.maximumPayloadLength else { throw WireFrameError.payloadTooLarge }
            let flags = WireFlags(rawValue: buffer[readOffset + 5])
            guard flags.rawValue & ~WireFlags.knownMask == 0 else { throw WireFrameError.reservedFlagsSet }
            guard buffer[readOffset + 6] == 0, buffer[readOffset + 7] == 0 else { throw WireFrameError.reservedHeaderNonZero }

            let messageLength = WireFrame.headerLength + payloadLength
            guard buffer.count - readOffset >= messageLength else { break }
            let messageEnd = readOffset + messageLength

            let frame = WireFrame(
                channel: buffer[readOffset + 4],
                flags: flags,
                presentationTimeMicros: buffer.readBigEndian(UInt64.self, at: readOffset + 8),
                payload: buffer.subdata(in: (readOffset + WireFrame.headerLength)..<messageEnd)
            )
            frames.append(frame)
            readOffset = messageEnd
        }

        if readOffset == buffer.count {
            buffer.removeAll(keepingCapacity: true)
            readOffset = 0
        } else if readOffset >= 1_024 * 1_024 || readOffset > buffer.count / 2 {
            buffer.removeSubrange(0..<readOffset)
            readOffset = 0
        }

        return frames
    }
}

private extension Data {
    mutating func appendBigEndian<T: FixedWidthInteger>(_ value: T) {
        var bigEndian = value.bigEndian
        Swift.withUnsafeBytes(of: &bigEndian) { append(contentsOf: $0) }
    }

    func readBigEndian<T: FixedWidthInteger>(_ type: T.Type, at offset: Int) -> T {
        self[offset..<(offset + MemoryLayout<T>.size)].reduce(into: T.zero) { result, byte in
            result = (result << 8) | T(byte)
        }
    }
}
