import Foundation

public struct EndpointError: Error, LocalizedError, Equatable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

public enum EndpointPolicy {
    /// An HTTPS gateway URL without credentials, query or fragment, and without a trailing slash.
    public static func normalize(_ value: String) throws -> String {
        let trimmed = value.trimmingCharacters(in: CharacterSet(charactersIn: Unicode.Scalar(0)...Unicode.Scalar(0x20)))
        guard let components = URLComponents(string: trimmed), components.scheme == "https" else {
            throw EndpointError("Use an HTTPS URL for your computer or gateway.")
        }
        guard let host = components.percentEncodedHost, !host.isEmpty, components.percentEncodedUser == nil,
              components.percentEncodedPassword == nil, components.percentEncodedQuery == nil, components.percentEncodedFragment == nil else {
            throw EndpointError("Enter a server URL without credentials, query parameters, or fragments.")
        }
        guard let text = components.string else { throw EndpointError("Use an HTTPS URL for your computer or gateway.") }
        var result = Substring(text)
        while result.hasSuffix("/") { result = result.dropLast() }
        return String(result)
    }
}

/// A transport alias, never a connection, workspace or request identity migration.
public enum ManagedRelay {
    public static let current = "https://relay.zerus.dev"
    static let legacyHost = "zerus.dev.guthub.dev"

    public static func transport(_ stored: String) -> String {
        guard let uri = URLComponents(string: stored), uri.scheme == "https",
              uri.percentEncodedHost?.caseInsensitiveCompare(legacyHost) == .orderedSame,
              uri.port == nil || uri.port == 443, uri.percentEncodedUser == nil, uri.percentEncodedPassword == nil,
              uri.percentEncodedQuery == nil, uri.percentEncodedFragment == nil,
              uri.percentEncodedPath == "" || uri.percentEncodedPath == "/" else { return stored }
        return current
    }
}

/// Presentation-only invitation. Neither parsing nor scanning performs network activity.
public struct PairingInvite: Hashable, Sendable, CustomDebugStringConvertible {
    public static let maxLength = 4096
    public let server: String
    public let code: String
    public var debugDescription: String { "PairingInvite(redacted)" }

    public init(server: String, code: String) {
        self.server = server
        self.code = code
    }

    public static func parse(_ value: String) -> PairingInvite? {
        guard value.count <= maxLength, !value.unicodeScalars.contains(where: isISOControl) else { return nil }
        guard let uri = URLComponents(string: value), uri.scheme == "zerus", uri.percentEncodedHost == "pair",
              uri.port == nil, uri.percentEncodedUser == nil, uri.percentEncodedPassword == nil,
              uri.percentEncodedPath == "" || uri.percentEncodedPath == "/", uri.percentEncodedFragment == nil,
              let query = uri.percentEncodedQuery,
              // URLComponents accepts `zerus:pair`-style forms; require the authority syntax.
              value.hasPrefix("zerus://pair") else { return nil }
        var fields: [String: String] = [:]
        for entry in query.split(separator: "&", omittingEmptySubsequences: false) {
            let parts = entry.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, let key = formDecode(parts[0]), let decoded = formDecode(parts[1]),
                  key == "server" || key == "code", fields[key] == nil else { return nil }
            fields[key] = decoded
        }
        guard let code = fields["code"], let server = fields["server"], server.count <= 2048,
              !server.unicodeScalars.contains(where: { $0.properties.isWhitespace || isISOControl($0) || $0 == "\u{FFFD}" }),
              let normalized = try? EndpointPolicy.normalize(server),
              let endpoint = URLComponents(string: normalized),
              endpoint.port == nil || (1...65535).contains(endpoint.port!) else { return nil }
        if WorkspaceProtocol.encryptedInvitationCode(code) {
            guard (try? WorkspaceInvitation.parse(code)) != nil else { return nil }
        } else {
            guard (16...128).contains(code.count), code.unicodeScalars.allSatisfy({ ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_" }) else { return nil }
        }
        return PairingInvite(server: ManagedRelay.transport(normalized), code: code)
    }

    static func isISOControl(_ scalar: Unicode.Scalar) -> Bool {
        scalar.value <= 0x1F || (0x7F...0x9F).contains(scalar.value)
    }

    /// `URLDecoder.decode(value, "UTF-8")`: `+` is a space, malformed escapes are rejected
    /// and invalid UTF-8 becomes U+FFFD.
    static func formDecode(_ value: Substring) -> String? {
        var bytes = [UInt8]()
        var iterator = Array(value.utf8).makeIterator()
        while let byte = iterator.next() {
            switch byte {
            case UInt8(ascii: "+"): bytes.append(0x20)
            case UInt8(ascii: "%"):
                guard let high = iterator.next().flatMap(hexValue), let low = iterator.next().flatMap(hexValue) else { return nil }
                bytes.append(high << 4 | low)
            default: bytes.append(byte)
            }
        }
        return String(decoding: bytes, as: UTF8.self)
    }

    private static func hexValue(_ byte: UInt8) -> UInt8? {
        switch byte {
        case UInt8(ascii: "0")...UInt8(ascii: "9"): byte - UInt8(ascii: "0")
        case UInt8(ascii: "a")...UInt8(ascii: "f"): byte - UInt8(ascii: "a") + 10
        case UInt8(ascii: "A")...UInt8(ascii: "F"): byte - UInt8(ascii: "A") + 10
        default: nil
        }
    }
}
