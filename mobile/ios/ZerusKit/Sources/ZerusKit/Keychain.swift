import CryptoKit
import Foundation
import Security
import Synchronization

/// Generic-password items that never leave this device (no iCloud Keychain, no backup restore).
public enum Keychain {
    public static let service = "app.zerus.mobile"

    public struct Failure: Error, LocalizedError {
        public let status: OSStatus
        public var errorDescription: String? {
            "Keychain error \(status): \((SecCopyErrorMessageString(status, nil) as String?) ?? "unknown")"
        }
    }

    static func query(_ account: String) -> [String: Any] {
        var query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service,
                                    kSecAttrAccount as String: account]
        #if os(macOS)
        query[kSecUseDataProtectionKeychain as String] = true
        #endif
        return query
    }

    public static func read(_ account: String) throws -> Data? {
        var query = query(account)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess else { throw Failure(status: status) }
        return result as? Data
    }

    /// Adds or replaces an item atomically.
    public static func write(_ account: String, _ data: Data) throws {
        var attributes: [String: Any] = [kSecValueData as String: data,
                                         kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        let status = SecItemUpdate(query(account) as CFDictionary, attributes as CFDictionary)
        if status == errSecSuccess { return }
        guard status == errSecItemNotFound else { throw Failure(status: status) }
        attributes.merge(query(account)) { current, _ in current }
        let added = SecItemAdd(attributes as CFDictionary, nil)
        guard added == errSecSuccess else { throw Failure(status: added) }
    }

    public static func delete(_ account: String) throws {
        let status = SecItemDelete(query(account) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw Failure(status: status) }
    }
}

/// The 256-bit state key, created once and kept in the Keychain.
public final class KeychainKeyProvider: KeyProvider {
    private let account: String
    private let cached = Mutex<SymmetricKey?>(nil)

    public init(account: String = "sealed-state-v1") { self.account = account }

    public func key() throws -> SymmetricKey {
        try cached.withLock { cached in
            if let cached { return cached }
            if let stored = try Keychain.read(account), stored.count == 32 {
                cached = SymmetricKey(data: stored)
                return cached!
            }
            let fresh = SymmetricKey(size: .bits256)
            let data = fresh.withUnsafeBytes { Data($0) }
            var query = Keychain.query(account)
            query[kSecValueData as String] = data
            query[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            let status = SecItemAdd(query as CFDictionary, nil)
            if status == errSecDuplicateItem, let stored = try Keychain.read(account), stored.count == 32 {
                cached = SymmetricKey(data: stored)
                return cached!
            }
            guard status == errSecSuccess else { throw Keychain.Failure(status: status) }
            cached = fresh
            return fresh
        }
    }
}

/// Paired gateways, including their device credentials.
public protocol ConnectionStore: Sendable {
    func load() throws -> [Connection]
    func save(_ connections: [Connection]) throws
}

/// The whole list is one Keychain item, so pairing and disconnect replace it atomically.
public struct KeychainConnectionStore: ConnectionStore {
    let account: String
    let legacyAccount: String
    public init(account: String = "connections-v2", legacyAccount: String = "connections-v1") { self.account = account; self.legacyAccount = legacyAccount }
    public func load() throws -> [Connection] {
        if let data = try Keychain.read(account) {
            guard let json = try JSON.parse(data).object, let rows = json["connections"]?.array,
                  rows.allSatisfy({ row in if case .int(let value)? = row["protocolVersion"] { return value == 1 || value == 2 }; return false }) else {
                throw ZerusError.invalid("Workspace transport marker is missing or invalid.")
            }
            let index = try JSONDecoder().decode(ConnectionIndex.self, from: data)
            guard index.version == 2, Set(index.connections.map(\.id)).count == index.connections.count else { throw ZerusError.invalid("Unsupported workspace index.") }
            return index.connections
        }
        guard let data = try Keychain.read(legacyAccount) else { return [] }
        let legacy = try JSONDecoder().decode([Connection].self, from: data)
        guard legacy.allSatisfy({ $0.protocolVersion == 1 }) else { throw ZerusError.invalid("Protected credentials cannot be loaded from the legacy index.") }
        return legacy
    }
    public func save(_ connections: [Connection]) throws {
        // An old client ignores new fields, so protected credentials never enter its item.
        try Keychain.write(account, try JSONEncoder().encode(ConnectionIndex(version: 2, connections: connections)))
    }
    struct ConnectionIndex: Codable { let version: Int; let connections: [Connection] }
}

public final class InMemoryConnectionStore: ConnectionStore {
    private let value = Mutex<[Connection]>([])
    public init(_ initial: [Connection] = []) { value.withLock { $0 = initial } }
    public func load() throws -> [Connection] { value.withLock { $0 } }
    public func save(_ connections: [Connection]) throws { value.withLock { $0 = connections } }
}
