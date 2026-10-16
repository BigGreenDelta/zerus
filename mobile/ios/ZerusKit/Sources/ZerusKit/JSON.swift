import Foundation

/// An immutable JSON value. Native inspection results stay raw, as the Android
/// client keeps `JSONObject`, so unknown fields survive and can be compared.
public enum JSON: Sendable, Hashable {
    case null
    case bool(Bool)
    case int(Int64)
    case double(Double)
    case string(String)
    case array([JSON])
    case object(JSONObject)

    public struct ParseError: Error, CustomStringConvertible {
        public let offset: Int
        public let reason: String
        public var description: String { "Invalid JSON at byte \(offset): \(reason)" }
    }
    public struct EncodeError: Error {}

    public static let maxDepth = 512

    public static func parse(_ data: Data) throws -> JSON {
        try data.withUnsafeBytes { raw in
            var parser = JSONParser(bytes: raw.bindMemory(to: UInt8.self))
            return try parser.document()
        }
    }

    public static func parse(_ text: String) throws -> JSON { try parse(Data(text.utf8)) }

    /// Compact serialization in key order, like `JSONObject.toString()`.
    public func data() throws -> Data {
        var out = [UInt8]()
        out.reserveCapacity(256)
        try JSONWriter.write(self, into: &out)
        return Data(out)
    }

    public func text() throws -> String { String(decoding: try data(), as: UTF8.self) }

    /// Serialization that never fails for values built from parsed or literal input
    /// without non-finite numbers; non-finite numbers are written as `null`.
    public var compact: String {
        var out = [UInt8]()
        JSONWriter.writeLenient(self, into: &out)
        return String(decoding: out, as: UTF8.self)
    }

    public var object: JSONObject? { if case .object(let value) = self { value } else { nil } }
    public var array: [JSON]? { if case .array(let value) = self { value } else { nil } }
    public var isNull: Bool { if case .null = self { true } else { false } }

    /// Text as `optString` renders it: strings verbatim, other values as JSON text.
    public var textValue: String? {
        switch self {
        case .null: nil
        case .string(let value): value
        case .bool(let value): value ? "true" : "false"
        case .int(let value): String(value)
        case .double(let value): value.isFinite ? (value == value.rounded() && abs(value) < 1e15 ? String(format: "%.1f", value) : String(value)) : String(value)
        case .array, .object: compact
        }
    }

    public var numberValue: Double? {
        switch self {
        case .int(let value): Double(value)
        case .double(let value): value
        default: nil
        }
    }

    public subscript(key: String) -> JSON? { object?[key] }
    public subscript(index: Int) -> JSON? {
        guard case .array(let values) = self, values.indices.contains(index) else { return nil }
        return values[index]
    }
}

/// An object that preserves insertion order, as Android's `JSONObject` does.
public struct JSONObject: Sendable, Hashable, Sequence, ExpressibleByDictionaryLiteral {
    public private(set) var keys: [String] = []
    private var storage: [String: JSON] = [:]

    public init() {}
    public init(dictionaryLiteral elements: (String, JSON)...) {
        for (key, value) in elements { self[key] = value }
    }
    public init(_ pairs: [(String, JSON)]) {
        for (key, value) in pairs { self[key] = value }
    }

    public var count: Int { keys.count }
    public var isEmpty: Bool { keys.isEmpty }

    public subscript(key: String) -> JSON? {
        get { storage[key] }
        set {
            if let newValue {
                if storage.updateValue(newValue, forKey: key) == nil { keys.append(key) }
            } else if storage.removeValue(forKey: key) != nil {
                keys.removeAll { $0 == key }
            }
        }
    }

    /// Returns a copy with `key` set, for chained construction.
    public func putting(_ key: String, _ value: JSON?) -> JSONObject {
        var copy = self
        copy[key] = value
        return copy
    }

    public func makeIterator() -> AnyIterator<(key: String, value: JSON)> {
        var index = 0
        return AnyIterator {
            guard index < keys.count else { return nil }
            defer { index += 1 }
            return (keys[index], storage[keys[index]]!)
        }
    }

    public static func == (lhs: JSONObject, rhs: JSONObject) -> Bool { lhs.storage == rhs.storage }
    public func hash(into hasher: inout Hasher) { hasher.combine(storage) }
}

// MARK: - Android-compatible accessors

/// `opt*` style accessors shared by objects and values. A missing key or a
/// non-object receiver yields the same defaults as `org.json`.
public protocol JSONAccess {
    func value(_ key: String) -> JSON?
}

extension JSON: JSONAccess {
    public func value(_ key: String) -> JSON? { object?[key] }
}

extension JSONObject: JSONAccess {
    public func value(_ key: String) -> JSON? { self[key] }
}

extension JSONAccess {
    /// The first value that is neither empty nor the text `null`.
    public func string(_ keys: String...) -> String { string(keys) }
    public func string(_ keys: [String]) -> String {
        for key in keys {
            if let text = value(key)?.textValue, !text.isEmpty, text != "null" { return text }
        }
        return ""
    }

    public func has(_ key: String) -> Bool { value(key) != nil }

    public func bool(_ key: String, default fallback: Bool = false) -> Bool {
        switch value(key) {
        case .bool(let flag): flag
        case .string(let text) where text.lowercased() == "true": true
        case .string(let text) where text.lowercased() == "false": false
        default: fallback
        }
    }

    public func int64(_ key: String, default fallback: Int64 = 0) -> Int64 {
        switch value(key) {
        case .int(let number): number
        case .double(let number) where number.isFinite && abs(number) < 9.2e18: Int64(number)
        case .string(let text): Int64(text.trimmingCharacters(in: .whitespaces)) ?? Double(text).flatMap { $0.isFinite && abs($0) < 9.2e18 ? Int64($0) : nil } ?? fallback
        default: fallback
        }
    }

    public func int(_ key: String, default fallback: Int = 0) -> Int {
        let value = int64(key, default: Int64(fallback))
        return Int(clamping: value)
    }

    public func double(_ key: String, default fallback: Double = .nan) -> Double {
        switch value(key) {
        case .int(let number): Double(number)
        case .double(let number): number
        case .string(let text): Double(text) ?? fallback
        default: fallback
        }
    }

    /// `(opt(key) as? Number)`: only actual JSON numbers.
    public func number(_ key: String) -> Double? { value(key)?.numberValue }

    public func object(_ key: String) -> JSONObject? { value(key)?.object }
    public func array(_ key: String) -> [JSON]? { value(key)?.array }

    /// `optJSONArray(key)?.objects()`: the object members of an array.
    public func objects(_ key: String) -> [JSONObject] { (value(key)?.array ?? []).compactMap(\.object) }

    /// String members of an array, skipping other values.
    public func strings(_ key: String) -> [String] {
        (value(key)?.array ?? []).compactMap { if case .string(let text) = $0 { text } else { nil } }
    }
}

// MARK: - Literals

extension JSON: ExpressibleByStringLiteral, ExpressibleByIntegerLiteral, ExpressibleByBooleanLiteral,
    ExpressibleByFloatLiteral, ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral, ExpressibleByNilLiteral {
    public init(stringLiteral value: String) { self = .string(value) }
    public init(integerLiteral value: Int64) { self = .int(value) }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
    public init(floatLiteral value: Double) { self = .double(value) }
    public init(arrayLiteral elements: JSON...) { self = .array(elements) }
    public init(dictionaryLiteral elements: (String, JSON)...) { self = .object(JSONObject(elements)) }
    public init(nilLiteral: ()) { self = .null }
}

extension JSON {
    public init(_ value: String) { self = .string(value) }
    public init(_ value: Int) { self = .int(Int64(value)) }
    public init(_ value: Bool) { self = .bool(value) }
    public init(_ value: Double) { self = .double(value) }
    public init(_ values: [String]) { self = .array(values.map(JSON.string)) }
}

extension JSON: Codable {
    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        let text = try container.decode(String.self)
        self = try JSON.parse(text)
    }
    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(compact)
    }
}

extension JSONObject: Codable {
    public init(from decoder: Decoder) throws {
        let value = try JSON(from: decoder)
        guard let object = value.object else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath, debugDescription: "Expected a JSON object."))
        }
        self = object
    }
    public func encode(to encoder: Encoder) throws { try JSON.object(self).encode(to: encoder) }
}

// MARK: - Parser

private struct JSONParser {
    let bytes: UnsafeBufferPointer<UInt8>
    var index = 0
    var depth = 0

    init(bytes: UnsafeBufferPointer<UInt8>) { self.bytes = bytes }

    func fail(_ reason: String) -> JSON.ParseError { JSON.ParseError(offset: index, reason: reason) }

    mutating func document() throws -> JSON {
        skipWhitespace()
        let value = try parseValue()
        skipWhitespace()
        guard index == bytes.count else { throw fail("trailing data") }
        return value
    }

    mutating func skipWhitespace() {
        while index < bytes.count {
            switch bytes[index] {
            case 0x20, 0x0A, 0x0D, 0x09: index += 1
            default: return
            }
        }
    }

    mutating func parseValue() throws -> JSON {
        guard index < bytes.count else { throw fail("unexpected end") }
        switch bytes[index] {
        case UInt8(ascii: "{"): return try parseObject()
        case UInt8(ascii: "["): return try parseArray()
        case UInt8(ascii: "\""): return .string(try parseString())
        case UInt8(ascii: "t"): try literal("true"); return .bool(true)
        case UInt8(ascii: "f"): try literal("false"); return .bool(false)
        case UInt8(ascii: "n"): try literal("null"); return .null
        default: return try parseNumber()
        }
    }

    mutating func literal(_ word: StaticString) throws {
        let count = word.utf8CodeUnitCount
        guard index + count <= bytes.count else { throw fail("invalid literal") }
        let expected = UnsafeBufferPointer(start: word.utf8Start, count: count)
        for offset in 0..<count where bytes[index + offset] != expected[offset] { throw fail("invalid literal") }
        index += count
    }

    mutating func enter() throws {
        depth += 1
        guard depth <= JSON.maxDepth else { throw fail("nesting too deep") }
    }

    mutating func parseObject() throws -> JSON {
        try enter()
        index += 1
        var object = JSONObject()
        skipWhitespace()
        if index < bytes.count, bytes[index] == UInt8(ascii: "}") { index += 1; depth -= 1; return .object(object) }
        while true {
            skipWhitespace()
            guard index < bytes.count, bytes[index] == UInt8(ascii: "\"") else { throw fail("expected key") }
            let key = try parseString()
            skipWhitespace()
            guard index < bytes.count, bytes[index] == UInt8(ascii: ":") else { throw fail("expected colon") }
            index += 1
            skipWhitespace()
            object[key] = try parseValue()
            skipWhitespace()
            guard index < bytes.count else { throw fail("unterminated object") }
            if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
            if bytes[index] == UInt8(ascii: "}") { index += 1; depth -= 1; return .object(object) }
            throw fail("expected comma or end of object")
        }
    }

    mutating func parseArray() throws -> JSON {
        try enter()
        index += 1
        var values = [JSON]()
        skipWhitespace()
        if index < bytes.count, bytes[index] == UInt8(ascii: "]") { index += 1; depth -= 1; return .array(values) }
        while true {
            skipWhitespace()
            values.append(try parseValue())
            skipWhitespace()
            guard index < bytes.count else { throw fail("unterminated array") }
            if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
            if bytes[index] == UInt8(ascii: "]") { index += 1; depth -= 1; return .array(values) }
            throw fail("expected comma or end of array")
        }
    }

    mutating func parseString() throws -> String {
        index += 1
        let start = index
        // Fast path: no escapes.
        while index < bytes.count {
            let byte = bytes[index]
            if byte == UInt8(ascii: "\"") {
                let text = String(decoding: UnsafeBufferPointer(rebasing: bytes[start..<index]), as: UTF8.self)
                index += 1
                return text
            }
            if byte == UInt8(ascii: "\\") { break }
            if byte < 0x20 { throw fail("control character in string") }
            index += 1
        }
        var buffer = [UInt8](UnsafeBufferPointer(rebasing: bytes[start..<index]))
        while index < bytes.count {
            let byte = bytes[index]
            if byte == UInt8(ascii: "\"") {
                index += 1
                return String(decoding: buffer, as: UTF8.self)
            }
            if byte < 0x20 { throw fail("control character in string") }
            if byte != UInt8(ascii: "\\") { buffer.append(byte); index += 1; continue }
            index += 1
            guard index < bytes.count else { break }
            let escape = bytes[index]
            index += 1
            switch escape {
            case UInt8(ascii: "\""): buffer.append(0x22)
            case UInt8(ascii: "\\"): buffer.append(0x5C)
            case UInt8(ascii: "/"): buffer.append(0x2F)
            case UInt8(ascii: "b"): buffer.append(0x08)
            case UInt8(ascii: "f"): buffer.append(0x0C)
            case UInt8(ascii: "n"): buffer.append(0x0A)
            case UInt8(ascii: "r"): buffer.append(0x0D)
            case UInt8(ascii: "t"): buffer.append(0x09)
            case UInt8(ascii: "u"):
                var scalar = try hex4()
                if (0xD800...0xDBFF).contains(scalar) {
                    if index + 6 <= bytes.count, bytes[index] == UInt8(ascii: "\\"), bytes[index + 1] == UInt8(ascii: "u") {
                        index += 2
                        let low = try hex4()
                        if (0xDC00...0xDFFF).contains(low) {
                            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (low - 0xDC00)
                        } else {
                            appendScalar(0xFFFD, to: &buffer)
                            scalar = low
                        }
                    } else {
                        scalar = 0xFFFD
                    }
                } else if (0xDC00...0xDFFF).contains(scalar) {
                    scalar = 0xFFFD
                }
                appendScalar(scalar, to: &buffer)
            default: throw fail("invalid escape")
            }
        }
        throw fail("unterminated string")
    }

    mutating func hex4() throws -> UInt32 {
        guard index + 4 <= bytes.count else { throw fail("short unicode escape") }
        var value: UInt32 = 0
        for _ in 0..<4 {
            let byte = bytes[index]
            let digit: UInt32
            switch byte {
            case UInt8(ascii: "0")...UInt8(ascii: "9"): digit = UInt32(byte - UInt8(ascii: "0"))
            case UInt8(ascii: "a")...UInt8(ascii: "f"): digit = UInt32(byte - UInt8(ascii: "a") + 10)
            case UInt8(ascii: "A")...UInt8(ascii: "F"): digit = UInt32(byte - UInt8(ascii: "A") + 10)
            default: throw fail("invalid unicode escape")
            }
            value = value << 4 | digit
            index += 1
        }
        return value
    }

    func appendScalar(_ value: UInt32, to buffer: inout [UInt8]) {
        let scalar = Unicode.Scalar(value) ?? "\u{FFFD}"
        buffer.append(contentsOf: Array(String(Character(scalar)).utf8))
    }

    mutating func parseNumber() throws -> JSON {
        let start = index
        var integral = true
        if index < bytes.count, bytes[index] == UInt8(ascii: "-") { index += 1 }
        guard index < bytes.count, isDigit(bytes[index]) else { throw fail("invalid value") }
        if bytes[index] == UInt8(ascii: "0") {
            index += 1
            if index < bytes.count, isDigit(bytes[index]) { throw fail("leading zero") }
        } else {
            while index < bytes.count, isDigit(bytes[index]) { index += 1 }
        }
        if index < bytes.count, bytes[index] == UInt8(ascii: ".") {
            integral = false
            index += 1
            guard index < bytes.count, isDigit(bytes[index]) else { throw fail("invalid fraction") }
            while index < bytes.count, isDigit(bytes[index]) { index += 1 }
        }
        if index < bytes.count, bytes[index] == UInt8(ascii: "e") || bytes[index] == UInt8(ascii: "E") {
            integral = false
            index += 1
            if index < bytes.count, bytes[index] == UInt8(ascii: "+") || bytes[index] == UInt8(ascii: "-") { index += 1 }
            guard index < bytes.count, isDigit(bytes[index]) else { throw fail("invalid exponent") }
            while index < bytes.count, isDigit(bytes[index]) { index += 1 }
        }
        let text = String(decoding: UnsafeBufferPointer(rebasing: bytes[start..<index]), as: UTF8.self)
        if integral, let value = Int64(text) { return .int(value) }
        guard let value = Double(text), value.isFinite else { throw fail("number out of range") }
        return .double(value)
    }

    func isDigit(_ byte: UInt8) -> Bool { byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9") }
}

// MARK: - Writer

private enum JSONWriter {
    static func write(_ value: JSON, into out: inout [UInt8]) throws {
        switch value {
        case .double(let number) where !number.isFinite: throw JSON.EncodeError()
        case .array(let values): out.append(UInt8(ascii: "["))
            for (offset, element) in values.enumerated() {
                if offset > 0 { out.append(UInt8(ascii: ",")) }
                try write(element, into: &out)
            }
            out.append(UInt8(ascii: "]"))
        case .object(let object): out.append(UInt8(ascii: "{"))
            for (offset, entry) in object.enumerated() {
                if offset > 0 { out.append(UInt8(ascii: ",")) }
                string(entry.key, into: &out)
                out.append(UInt8(ascii: ":"))
                try write(entry.value, into: &out)
            }
            out.append(UInt8(ascii: "}"))
        default: scalar(value, into: &out)
        }
    }

    static func writeLenient(_ value: JSON, into out: inout [UInt8]) {
        switch value {
        case .array(let values): out.append(UInt8(ascii: "["))
            for (offset, element) in values.enumerated() {
                if offset > 0 { out.append(UInt8(ascii: ",")) }
                writeLenient(element, into: &out)
            }
            out.append(UInt8(ascii: "]"))
        case .object(let object): out.append(UInt8(ascii: "{"))
            for (offset, entry) in object.enumerated() {
                if offset > 0 { out.append(UInt8(ascii: ",")) }
                string(entry.key, into: &out)
                out.append(UInt8(ascii: ":"))
                writeLenient(entry.value, into: &out)
            }
            out.append(UInt8(ascii: "}"))
        default: scalar(value, into: &out)
        }
    }

    static func scalar(_ value: JSON, into out: inout [UInt8]) {
        switch value {
        case .null: out.append(contentsOf: Array("null".utf8))
        case .bool(let flag): out.append(contentsOf: Array((flag ? "true" : "false").utf8))
        case .int(let number): out.append(contentsOf: Array(String(number).utf8))
        case .double(let number): out.append(contentsOf: Array((number.isFinite ? String(number) : "null").utf8))
        case .string(let text): string(text, into: &out)
        case .array, .object: break
        }
    }

    static func string(_ text: String, into out: inout [UInt8]) {
        out.append(UInt8(ascii: "\""))
        for byte in text.utf8 {
            switch byte {
            case 0x22: out.append(contentsOf: [0x5C, 0x22])
            case 0x5C: out.append(contentsOf: [0x5C, 0x5C])
            case 0x0A: out.append(contentsOf: [0x5C, UInt8(ascii: "n")])
            case 0x0D: out.append(contentsOf: [0x5C, UInt8(ascii: "r")])
            case 0x09: out.append(contentsOf: [0x5C, UInt8(ascii: "t")])
            case 0x08: out.append(contentsOf: [0x5C, UInt8(ascii: "b")])
            case 0x0C: out.append(contentsOf: [0x5C, UInt8(ascii: "f")])
            case 0x00..<0x20:
                let hex = Array("0123456789abcdef".utf8)
                out.append(contentsOf: [0x5C, UInt8(ascii: "u"), UInt8(ascii: "0"), UInt8(ascii: "0"), hex[Int(byte >> 4)], hex[Int(byte & 0xF)]])
            default: out.append(byte)
            }
        }
        out.append(UInt8(ascii: "\""))
    }
}
