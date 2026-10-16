import CryptoKit
import Foundation

public protocol KeyProvider: Sendable {
    func key() throws -> SymmetricKey
}

/// A fixed key, for tests and previews.
public struct StaticKeyProvider: KeyProvider {
    private let value: SymmetricKey
    public init(_ value: SymmetricKey) { self.value = value }
    public func key() throws -> SymmetricKey { value }
}

/// Private application state sealed with AES-GCM. The file name is authenticated
/// data, so a sealed file cannot be replayed under another name. Files are
/// written atomically, protected until first unlock and excluded from backup.
public struct SealedFileStore: Sendable {
    public let directory: URL
    private let keys: KeyProvider

    public init(directory: URL, keys: KeyProvider) {
        self.directory = directory
        self.keys = keys
    }

    public static func applicationSupport(_ name: String = "Zerus", keys: KeyProvider) throws -> SealedFileStore {
        let base = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return SealedFileStore(directory: base.appendingPathComponent(name, isDirectory: true), keys: keys)
    }

    public func readData(_ name: String) throws -> Data? {
        let url = try file(name)
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let sealed = try AES.GCM.SealedBox(combined: try Data(contentsOf: url))
        return try AES.GCM.open(sealed, using: try keys.key(), authenticating: Data(name.utf8))
    }

    public func writeData(_ name: String, _ data: Data) throws {
        let url = try file(name)
        try prepareDirectory()
        guard let sealed = try AES.GCM.seal(data, using: try keys.key(), authenticating: Data(name.utf8)).combined else {
            throw ZerusError.invalid("Could not seal private state.")
        }
        #if os(iOS)
        try sealed.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        #else
        try sealed.write(to: url, options: [.atomic])
        #endif
    }

    public func read<T: Decodable>(_ name: String, as type: T.Type) throws -> T? {
        guard let data = try readData(name) else { return nil }
        return try JSONDecoder().decode(T.self, from: data)
    }

    public func write<T: Encodable>(_ name: String, _ value: T) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        try writeData(name, try encoder.encode(value))
    }

    public func remove(_ name: String) throws {
        let url = try file(name)
        if FileManager.default.fileExists(atPath: url.path) { try FileManager.default.removeItem(at: url) }
    }

    /// Removes every sealed file whose name starts with `prefix`.
    public func removeAll(prefix: String) throws {
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path) else { return }
        for name in names where name.hasPrefix(prefix) && name.hasSuffix(".sealed") {
            try FileManager.default.removeItem(at: directory.appendingPathComponent(name))
        }
    }

    private func file(_ name: String) throws -> URL {
        guard !name.isEmpty, name.count <= 200, !name.hasPrefix("."),
              name.unicodeScalars.allSatisfy({ CharacterSet.alphanumerics.contains($0) || "-_.:".unicodeScalars.contains($0) }),
              !name.contains("..") else {
            throw ZerusError.invalid("Invalid private state name.")
        }
        return directory.appendingPathComponent(name + ".sealed")
    }

    private func prepareDirectory() throws {
        var url = directory
        if !FileManager.default.fileExists(atPath: url.path) {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        }
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try url.setResourceValues(values)
    }
}
