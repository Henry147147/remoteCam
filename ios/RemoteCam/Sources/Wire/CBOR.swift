import Foundation

enum CBORValue: Equatable, Sendable {
    case unsigned(UInt64)
    case negative(Int64)
    case bytes(Data)
    case string(String)
    case array([CBORValue])
    case map([String: CBORValue])
    case boolean(Bool)
    case null
    case double(Double)

    var stringValue: String? {
        guard case .string(let value) = self else { return nil }
        return value
    }

    var unsignedValue: UInt64? {
        guard case .unsigned(let value) = self else { return nil }
        return value
    }

    var boolValue: Bool? {
        guard case .boolean(let value) = self else { return nil }
        return value
    }
}

enum CBORError: Error, Equatable {
    case truncated
    case invalidAdditionalInformation(UInt8)
    case invalidUTF8
    case unsupportedMapKey
    case unsupportedSimpleValue(UInt8)
    case nestingLimitExceeded
    case trailingBytes
    case integerOverflow
}

enum CBOREncoder {
    static func encode(_ value: CBORValue) -> Data {
        var result = Data()
        append(value, to: &result)
        return result
    }

    private static func append(_ value: CBORValue, to data: inout Data) {
        switch value {
        case .unsigned(let number):
            appendMajor(0, value: number, to: &data)
        case .negative(let number):
            precondition(number < 0)
            appendMajor(1, value: UInt64(-(number + 1)), to: &data)
        case .bytes(let bytes):
            appendMajor(2, value: UInt64(bytes.count), to: &data)
            data.append(bytes)
        case .string(let string):
            let utf8 = Data(string.utf8)
            appendMajor(3, value: UInt64(utf8.count), to: &data)
            data.append(utf8)
        case .array(let values):
            appendMajor(4, value: UInt64(values.count), to: &data)
            values.forEach { append($0, to: &data) }
        case .map(let values):
            appendMajor(5, value: UInt64(values.count), to: &data)
            // Canonical ordering keeps authenticated payloads byte-for-byte stable.
            for key in values.keys.sorted(by: canonicalKeyOrder) {
                append(.string(key), to: &data)
                append(values[key]!, to: &data)
            }
        case .boolean(let value):
            data.append(value ? 0xf5 : 0xf4)
        case .null:
            data.append(0xf6)
        case .double(let value):
            data.append(0xfb)
            appendBigEndian(value.bitPattern, to: &data)
        }
    }

    private static func canonicalKeyOrder(_ lhs: String, _ rhs: String) -> Bool {
        let left = Data(lhs.utf8)
        let right = Data(rhs.utf8)
        return left.count == right.count ? left.lexicographicallyPrecedes(right) : left.count < right.count
    }

    private static func appendMajor(_ major: UInt8, value: UInt64, to data: inout Data) {
        if value < 24 {
            data.append((major << 5) | UInt8(value))
        } else if value <= UInt8.max {
            data.append((major << 5) | 24)
            data.append(UInt8(value))
        } else if value <= UInt16.max {
            data.append((major << 5) | 25)
            appendBigEndian(UInt16(value), to: &data)
        } else if value <= UInt32.max {
            data.append((major << 5) | 26)
            appendBigEndian(UInt32(value), to: &data)
        } else {
            data.append((major << 5) | 27)
            appendBigEndian(value, to: &data)
        }
    }

    private static func appendBigEndian<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var bigEndian = value.bigEndian
        withUnsafeBytes(of: &bigEndian) { data.append(contentsOf: $0) }
    }
}

enum CBORDecoder {
    static func decode(_ data: Data) throws -> CBORValue {
        var reader = Reader(data: data)
        let value = try reader.readValue(depth: 0)
        guard reader.isAtEnd else { throw CBORError.trailingBytes }
        return value
    }

    private struct Reader {
        let data: Data
        var offset = 0
        var isAtEnd: Bool { offset == data.count }

        mutating func readValue(depth: Int) throws -> CBORValue {
            guard depth <= 32 else { throw CBORError.nestingLimitExceeded }
            let initial = try readByte()
            let major = initial >> 5
            let additional = initial & 0x1f

            switch major {
            case 0:
                return .unsigned(try readLength(additional))
            case 1:
                let encoded = try readLength(additional)
                guard encoded <= UInt64(Int64.max) else { throw CBORError.integerOverflow }
                return .negative(-1 - Int64(encoded))
            case 2:
                return .bytes(try readData(count: try integerCount(additional)))
            case 3:
                let bytes = try readData(count: try integerCount(additional))
                guard let string = String(data: bytes, encoding: .utf8) else { throw CBORError.invalidUTF8 }
                return .string(string)
            case 4:
                let count = try integerCount(additional)
                var values: [CBORValue] = []
                values.reserveCapacity(count)
                for _ in 0..<count { values.append(try readValue(depth: depth + 1)) }
                return .array(values)
            case 5:
                let count = try integerCount(additional)
                var values: [String: CBORValue] = [:]
                values.reserveCapacity(count)
                for _ in 0..<count {
                    guard case .string(let key) = try readValue(depth: depth + 1) else {
                        throw CBORError.unsupportedMapKey
                    }
                    values[key] = try readValue(depth: depth + 1)
                }
                return .map(values)
            case 7:
                switch additional {
                case 20: return .boolean(false)
                case 21: return .boolean(true)
                case 22: return .null
                case 27:
                    return .double(Double(bitPattern: try readInteger(UInt64.self)))
                default:
                    throw CBORError.unsupportedSimpleValue(additional)
                }
            default:
                throw CBORError.invalidAdditionalInformation(additional)
            }
        }

        mutating func readLength(_ additional: UInt8) throws -> UInt64 {
            switch additional {
            case 0...23: return UInt64(additional)
            case 24: return UInt64(try readByte())
            case 25: return UInt64(try readInteger(UInt16.self))
            case 26: return UInt64(try readInteger(UInt32.self))
            case 27: return try readInteger(UInt64.self)
            default: throw CBORError.invalidAdditionalInformation(additional)
            }
        }

        mutating func integerCount(_ additional: UInt8) throws -> Int {
            let value = try readLength(additional)
            guard value <= UInt64(Int.max) else { throw CBORError.integerOverflow }
            return Int(value)
        }

        mutating func readByte() throws -> UInt8 {
            guard offset < data.count else { throw CBORError.truncated }
            defer { offset += 1 }
            return data[offset]
        }

        mutating func readData(count: Int) throws -> Data {
            guard count >= 0, offset <= data.count, count <= data.count - offset else {
                throw CBORError.truncated
            }
            defer { offset += count }
            return data.subdata(in: offset..<(offset + count))
        }

        mutating func readInteger<T: FixedWidthInteger>(_ type: T.Type) throws -> T {
            let bytes = try readData(count: MemoryLayout<T>.size)
            return bytes.reduce(into: T.zero) { result, byte in
                result = (result << 8) | T(byte)
            }
        }
    }
}
