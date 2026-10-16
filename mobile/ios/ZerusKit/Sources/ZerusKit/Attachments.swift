import CryptoKit
import Foundation
import UniformTypeIdentifiers

public enum AttachmentAccess: Sendable { case supported, unsupported, offline, unknown }

public struct AttachmentOpenError: LocalizedError, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

/// Exact identities and the additive `attachment_read` contract.
extension AttachmentIdentity {
    public static func remoteKey(_ target: Target, _ file: DisplayedAttachment) -> String {
        JSON.array([.string("remote"), .string(target.connectionId), .string(target.computerId), .string(target.session), .string(target.run),
                    .string(target.conversation), .string(target.archiveId), .string(target.agentId), .string(target.parentConversation),
                    .string(file.messageId), .int(Int64(file.index))]).compact
    }

    public static func localKey(_ attachment: Attachment) -> String {
        JSON.array([.string("local"), .string(attachment.id), .string(attachment.sha256)]).compact
    }

    /// Row identity for progress and errors: the local copy when present, else the receipt position.
    public static func rowKey(_ target: Target?, _ file: DisplayedAttachment) -> String {
        if let local = local(file) { return localKey(local) }
        if let target, remote(file) { return remoteKey(target, file) }
        return JSON.array([.string("none"), .string(target?.key ?? ""), .string(file.name), .string(file.mime), .int(file.bytes), .string(file.reference)]).compact
    }

    /// Playback belongs to one displayed message even when its encrypted bytes share a cache entry.
    public static func playbackKey(_ target: Target?, _ file: DisplayedAttachment) -> String {
        JSON.array([.string("audio"), .string(target?.key ?? ""), .string(file.messageId), .int(Int64(file.index)), .string(rowKey(target, file))]).compact
    }

    public static func unavailableReason(_ target: Target?, _ file: DisplayedAttachment, _ access: AttachmentAccess) -> String? {
        guard let target, remote(file) else { return "This attachment cannot be opened from the phone." }
        if target.connectionId == "demo" { return "Attachments are not available in the demo." }
        if target.run.isEmpty || conversation(target).isEmpty { return "Refresh the conversation, then try again." }
        switch access {
        case .unsupported: return updateRequired
        case .offline: return "This computer is offline. Try again when it is connected."
        case .unknown: return "Connect to this computer, then try again."
        case .supported: return nil
        }
    }

    public static func access(relay: Set<String>?, machine: Set<String>?, online: Bool?) -> AttachmentAccess {
        guard let relay, let machine else { return .unknown }
        if !relay.contains(operation) || !machine.contains(operation) { return .unsupported }
        return online == false ? .offline : .supported
    }

    public static func payload(_ target: Target, _ file: DisplayedAttachment, requestId: String, offset: Int64, length: Int) throws -> JSONObject {
        guard remote(file), !target.run.isEmpty, !conversation(target).isEmpty, (0..<file.bytes).contains(offset), (1...chunkBytes).contains(length),
              offset + Int64(length) <= file.bytes else { throw ZerusError.invalid("Invalid attachment range.") }
        var raw: JSONObject = ["request_id": .string(requestId), "expected_run_id": .string(target.run), "expected_conversation_id": .string(conversation(target)),
                               "message_id": .string(file.messageId), "index": .int(Int64(file.index)), "offset": .int(offset), "length": .int(Int64(length))]
        if !target.archiveId.isEmpty { raw["archive_id"] = .string(target.archiveId) }
        if !target.agentId.isEmpty { raw["agent_id"] = .string(target.agentId) }
        return raw
    }

    public struct Chunk: Sendable {
        public let data: Data
        public let total: Int64
        public let sha256: String
    }

    static let digest = try! NSRegularExpression(pattern: "^[a-f0-9]{64}$")
    static func validDigest(_ value: String) -> Bool { digest.firstMatch(in: value, range: NSRange(value.startIndex..., in: value)) != nil }

    /// Strict base64 that re-encodes to the same text.
    static func decode(_ text: String) -> Data? {
        guard let data = Data(base64Encoded: text), data.base64EncodedString() == text else { return nil }
        return data
    }

    static func integer(_ raw: JSONObject, _ key: String) -> Int64? {
        if case .int(let value)? = raw[key] { return value }
        return nil
    }

    /// Verifies one range answers this exact request, file and position.
    public static func chunk(_ result: JSONObject, requestId: String, target: Target, file: DisplayedAttachment, offset: Int64, length: Int, first: Chunk?) throws -> Chunk {
        guard result.string("request_id") == requestId, result.string("name") == target.session, result.string("message_id") == file.messageId,
              integer(result, "index") == Int64(file.index), result.string("archive_id") == target.archiveId, result.string("agent_id") == target.agentId else {
            throw AttachmentOpenError("The computer returned a different attachment. Refresh and try again.")
        }
        guard let total = integer(result, "bytes"), total == file.bytes else {
            throw AttachmentOpenError("The attachment changed on the computer. Refresh and try again.")
        }
        let sha = result.string("sha256")
        guard validDigest(sha), first == nil || first?.sha256 == sha, integer(result, "offset") == offset else {
            throw AttachmentOpenError("The attachment changed while downloading. Try again.")
        }
        guard let data = decode(result.string("data_base64")) else { throw AttachmentOpenError("The computer returned invalid attachment data.") }
        let expected = min(Int64(length), total - offset)
        guard Int64(data.count) == expected, integer(result, "length") == expected else {
            throw AttachmentOpenError("The computer returned an incomplete attachment range. Try again.")
        }
        return Chunk(data: data, total: total, sha256: sha)
    }

    /// A clear message for a failed or unconfirmed read receipt.
    public static func failure(_ receipt: JSONObject) -> String {
        switch receipt.string("state") {
        case "queued", "claimed": return "The computer did not answer in time. Check that it is online, then try again."
        default:
            let error = receipt.string("error")
            if error.contains("unavailable") || error.contains("unsupported operation") { return updateRequired }
            if error.isEmpty { return "The computer could not read this attachment." }
            return "The computer could not read this attachment: \(String(error.prefix(200)))"
        }
    }

    public static func relayFailure(_ status: Int) -> String {
        switch status {
        case 400, 409: updateRequired
        case 401, 403: "Reconnect to this gateway, then try again."
        case 429: "The gateway is busy. Try again in a moment."
        default: "The gateway could not deliver this request (\(status)). Try again."
        }
    }
}

public enum NativeAttachments {
    public static func reason(_ target: Target, _ raw: JSONObject?) -> String {
        if let raw, raw.has("attachments_supported"), !raw.bool("attachments_supported") {
            return raw.string("attachments_reason").nonEmpty ?? "This native adapter does not support file attachments."
        }
        if target.session.components(separatedBy: "/").first == "opencode" {
            return "OpenCode file attachments are not supported yet. Your existing files remain saved; remove them to send text."
        }
        return ""
    }
}

extension String {
    var nonEmpty: String? { trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : self }
}

public enum AttachmentViewing {
    /// A safe cache file name; previews infer the format from its extension, so one is added from the type when missing.
    public static func name(_ name: String, mime: String) -> String {
        let safe = AttachmentPolicy.filename(name)
        guard (safe as NSString).pathExtension.isEmpty, let ext = UTType(mimeType: mime)?.preferredFilenameExtension else { return safe }
        return AttachmentPolicy.filename(safe + "." + ext)
    }
}

// MARK: - Computer file links

/// A link belongs to the conversation computer. Parsing never reads the filesystem.
public struct ComputerFileReference: Hashable, Sendable {
    public let path: String
    public let location: String

    public init(path: String, location: String = "") {
        self.path = path
        self.location = location
    }

    public var name: String { LaunchPresentation.leaf(path).nonEmpty ?? "File" }

    static let lineSuffix = try! NSRegularExpression(pattern: "(:[1-9][0-9]*(?::[1-9][0-9]*)?)$")

    /// Every `%` must start a complete escape, as `java.net.URI` requires.
    static func validEscapes(_ value: String) -> Bool {
        let bytes = Array(value.utf8)
        var index = 0
        while index < bytes.count {
            if bytes[index] == UInt8(ascii: "%") {
                guard index + 2 < bytes.count,
                      [bytes[index + 1], bytes[index + 2]].allSatisfy({ Character(Unicode.Scalar($0)).isHexDigit }) else { return false }
                index += 3
            } else {
                index += 1
            }
        }
        return true
    }

    /// POSIX file destinations, with the same line suffixes as desktop file references.
    public static func parse(_ destination: String) -> ComputerFileReference? {
        if destination.trimmingCharacters(in: .whitespaces).isEmpty || destination.unicodeScalars.contains(where: { PairingInvite.isISOControl($0) || $0 == "\\" }) {
            return nil
        }
        guard validEscapes(destination), let components = URLComponents(string: destination.replacingOccurrences(of: " ", with: "%20")) else { return nil }
        if let scheme = components.scheme, scheme.lowercased() != "file" { return nil }
        if components.host.map({ !$0.isEmpty }) == true || components.user != nil || components.port != nil || components.percentEncodedQuery != nil { return nil }
        if components.scheme != nil && !destination.lowercased().hasPrefix("file:/") { return nil }
        var path = components.path
        var location = components.fragment ?? ""
        if path.trimmingCharacters(in: .whitespaces).isEmpty || path.hasPrefix("//") || path.utf8.count > 8192 ||
            path.unicodeScalars.contains(where: { PairingInvite.isISOControl($0) || $0 == "\\" }) || location.utf16.count > 2048 ||
            location.unicodeScalars.contains(where: PairingInvite.isISOControl) { return nil }
        if components.scheme == nil && components.percentEncodedHost != nil { return nil }
        if !location.isEmpty {
            location = "#" + location
        } else if let match = lineSuffix.firstMatch(in: path, range: NSRange(path.startIndex..., in: path)), let range = Range(match.range, in: path) {
            location = String(path[range])
            path.removeSubrange(range)
        }
        return path.trimmingCharacters(in: .whitespaces).isEmpty ? nil : ComputerFileReference(path: path, location: location)
    }
}

/// Bounded file reads pinned to the computer and conversation of the tapped link.
public enum ComputerFileProtocol {
    public static let operation = "file_read"
    public static let chunkBytes = 512 * 1024
    public static let updateRequired = "Update this gateway and Zerus on the computer to open file links."

    public struct Chunk: Sendable {
        public let data: Data
        public let bytes: Int64
        public let sha256: String
        public let path: String
        public let name: String
        public let mime: String
    }

    public static func access(relay: Set<String>?, machine: Set<String>?, online: Bool?) -> AttachmentAccess {
        guard let relay, let machine else { return .unknown }
        if !relay.contains(operation) || !machine.contains(operation) { return .unsupported }
        return online == false ? .offline : .supported
    }

    public static func payload(_ target: Target, _ reference: ComputerFileReference, requestId: String, offset: Int64, length: Int, first: Chunk?) throws -> JSONObject {
        guard AttachmentPolicy.validId(requestId), !target.run.isEmpty, !AttachmentIdentity.conversation(target).isEmpty, offset >= 0,
              (1...chunkBytes).contains(length), offset + Int64(length) <= AttachmentPolicy.maxFileBytes, first != nil || offset == 0 else {
            throw ZerusError.invalid("Invalid file range.")
        }
        var raw: JSONObject = ["request_id": .string(requestId), "expected_run_id": .string(target.run),
                               "expected_conversation_id": .string(AttachmentIdentity.conversation(target)), "path": .string(reference.path),
                               "offset": .int(offset), "length": .int(Int64(length))]
        if let first { raw["sha256"] = .string(first.sha256) }
        if !target.archiveId.isEmpty { raw["archive_id"] = .string(target.archiveId) }
        if !target.agentId.isEmpty { raw["agent_id"] = .string(target.agentId) }
        return raw
    }

    public static func chunk(_ result: JSONObject, requestId: String, target: Target, reference: ComputerFileReference, offset: Int64, length: Int,
                             first: Chunk?) throws -> Chunk {
        guard result.string("request_id") == requestId, result.string("name") == target.session, result.string("run_id") == target.run,
              result.string("conversation_id") == AttachmentIdentity.conversation(target), result.string("archive_id") == target.archiveId,
              result.string("agent_id") == target.agentId, result.string("requested_path") == reference.path,
              AttachmentIdentity.integer(result, "offset") == offset else {
            throw AttachmentOpenError("The computer returned a different file. Open the link again.")
        }
        guard let bytes = AttachmentIdentity.integer(result, "bytes"), (1...AttachmentPolicy.maxFileBytes).contains(bytes) else {
            throw AttachmentOpenError("Only nonempty files up to 10 MiB can be opened.")
        }
        let sha = result.string("sha256"), path = result.string("path"), name = result.string("file_name"), mime = result.string("mime")
        guard AttachmentIdentity.validDigest(sha), path.hasPrefix("/"), !path.hasPrefix("//"), !path.unicodeScalars.contains(where: PairingInvite.isISOControl),
              path.utf8.count <= 8192, !name.trimmingCharacters(in: .whitespaces).isEmpty, name == LaunchPresentation.leaf(path), name.utf8.count <= 255,
              !name.unicodeScalars.contains(where: { PairingInvite.isISOControl($0) || $0 == "\\" }), !mime.isEmpty, mime == AttachmentPolicy.mime(mime) else {
            throw AttachmentOpenError("The computer returned invalid file details.")
        }
        if let first, sha != first.sha256 || bytes != first.bytes || path != first.path || name != first.name || mime != first.mime {
            throw AttachmentOpenError("The file changed during download. Open it again.")
        }
        guard let data = AttachmentIdentity.decode(result.string("data_base64")) else { throw AttachmentOpenError("The computer returned invalid file data.") }
        let expected = min(Int64(length), bytes - offset)
        guard offset < bytes, Int64(data.count) == expected, AttachmentIdentity.integer(result, "length") == expected else {
            throw AttachmentOpenError("The computer returned an incomplete file. Open it again.")
        }
        return Chunk(data: data, bytes: bytes, sha256: sha, path: path, name: name, mime: mime)
    }

    public static func cacheKey(_ target: Target, _ file: Chunk) -> String {
        JSON.array([.string("computer-file"), .string(target.key), .string(file.path), .string(file.sha256)]).compact
    }

    public static func failure(_ receipt: JSONObject) -> String {
        switch receipt.string("state") {
        case "queued", "claimed": return "The computer did not answer in time. Check its connection and try again."
        default:
            let error = receipt.string("error")
            if error.isEmpty { return "The computer could not read this file." }
            if error.contains("unsupported") || error.contains("unavailable") { return updateRequired }
            return String(error.prefix(300))
        }
    }
}

// MARK: - Sealed attachment store

/// Sealed durable copies of selected files, separate from private state. Removal requires the
/// complete protected-ID set; this store never deletes a referenced file on its own.
public final class AttachmentStore: Sendable {
    public let root: URL
    let keys: KeyProvider
    let lock = NSLock()
    static let magic = Data("ZAT1".utf8)

    public init(root: URL, keys: KeyProvider) {
        self.root = root
        self.keys = keys
    }

    public static func live() throws -> AttachmentStore {
        let base = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return AttachmentStore(root: base.appendingPathComponent("attachments-v1", isDirectory: true), keys: KeychainKeyProvider(account: "attachments-v1"))
    }

    func directory() throws {
        let manager = FileManager.default
        if !manager.fileExists(atPath: root.path) {
            try manager.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            var url = root
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? url.setResourceValues(values)
        }
        guard (try? manager.attributesOfItem(atPath: root.path)[.type] as? FileAttributeType) == .typeDirectory else {
            throw ZerusError.invalid("Private attachment storage is unavailable.")
        }
    }

    func file(_ id: String) throws -> URL {
        guard AttachmentPolicy.validId(id) else { throw ZerusError.invalid("Invalid attachment reference.") }
        return root.appendingPathComponent("\(id).bin")
    }

    static func aad(_ id: String, _ name: String, _ mime: String) -> Data {
        Data(JSON.array([.string("zerus-attachment-v1"), .string(id), .string(name), .string(mime)]).compact.utf8)
    }

    /// Copies selected bytes into a sealed file and returns its exact metadata.
    public func importData(_ data: Data, name rawName: String?, mime rawMime: String?, current: [Attachment]) throws -> Attachment {
        lock.lock()
        defer { lock.unlock() }
        let existing = try AttachmentPolicy.selection(current, adding: true)
        if data.isEmpty { throw ZerusError.invalid("Empty files cannot be attached.") }
        if Int64(data.count) > AttachmentPolicy.maxFileBytes { throw ZerusError.invalid("Each attachment must be at most 10 MiB.") }
        if existing + Int64(data.count) > AttachmentPolicy.maxTotalBytes { throw ZerusError.invalid("Attachments exceed 20 MiB in total.") }
        try directory()
        let id = UUID().uuidString.lowercased()
        let name = AttachmentPolicy.filename(rawName), mime = AttachmentPolicy.mime(rawMime)
        guard let sealed = try AES.GCM.seal(data, using: keys.key(), authenticating: Self.aad(id, name, mime)).combined else {
            throw ZerusError.invalid("The selected file could not be saved.")
        }
        let destination = try file(id)
        let part = root.appendingPathComponent("\(id).part")
        defer { try? FileManager.default.removeItem(at: part) }
        var protection: Data.WritingOptions = [.withoutOverwriting]
        #if os(iOS)
        protection.insert(.completeFileProtectionUntilFirstUserAuthentication)
        #endif
        try (Self.magic + sealed).write(to: part, options: protection)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: part.path)
        guard rename(part.path, destination.path) == 0 else { throw ZerusError.invalid("Could not reserve an attachment reference.") }
        let sha = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        return Attachment(id: id, name: name, mime: mime, bytes: Int64(data.count), sha256: sha)
    }

    /// Copies a picked file, checking its actual size before reading it; provider metadata is ignored.
    public func importFile(_ source: URL, name: String?, mime: String?, current: [Attachment]) throws -> Attachment {
        let handle: FileHandle
        do { handle = try FileHandle(forReadingFrom: source) } catch { throw ZerusError.invalid("The selected document could not be opened.") }
        defer { try? handle.close() }
        let data: Data
        do { data = try handle.read(upToCount: Int(AttachmentPolicy.maxFileBytes) + 1) ?? Data() } catch {
            throw ZerusError.invalid("The selected document could not be copied. Check access and available storage.")
        }
        return try importData(data, name: name ?? source.lastPathComponent, mime: mime, current: current)
    }

    /// Decrypts a saved file into the view cache under its exact local identity.
    public func open(_ attachment: Attachment, cache: AttachmentViewCache, name: String? = nil) throws -> AttachmentViewCache.Entry {
        let key = AttachmentIdentity.localKey(attachment)
        if let cached = cache.lookup(key) { return cached }
        return try cache.store(key, name: name ?? attachment.name, mime: attachment.mime, data: decrypt(attachment), sha256: attachment.sha256)
    }

    /// Authenticated plaintext of a saved draft or outbox file.
    public func decrypt(_ attachment: Attachment) throws -> Data {
        lock.lock()
        defer { lock.unlock() }
        try AttachmentPolicy.validate(attachment)
        let source = try file(attachment.id)
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: source.path), attributes[.type] as? FileAttributeType == .typeRegular,
              let size = (attributes[.size] as? NSNumber)?.int64Value, (33...(AttachmentPolicy.maxFileBytes + 32)).contains(size) else {
            throw ZerusError.invalid("Saved attachment is missing or changed. Select the file again.")
        }
        let stored = try Data(contentsOf: source)
        guard stored.prefix(4) == Self.magic else { throw ZerusError.invalid("Saved attachment is invalid.") }
        let plain: Data
        do {
            plain = try AES.GCM.open(AES.GCM.SealedBox(combined: stored.dropFirst(4)), using: keys.key(),
                                     authenticating: Self.aad(attachment.id, attachment.name, attachment.mime))
        } catch {
            throw ZerusError.invalid("Saved attachment could not be verified. Select the file again.")
        }
        let sha = SHA256.hash(data: plain).map { String(format: "%02x", $0) }.joined()
        guard Int64(plain.count) == attachment.bytes, sha == attachment.sha256 else {
            throw ZerusError.invalid("Saved attachment content changed. Select the file again.")
        }
        return plain
    }

    /// The wire form of a message's files: exact names, types and verified bytes.
    public func payload(_ attachments: [Attachment]) throws -> JSON {
        try AttachmentPolicy.selection(attachments)
        return .array(try attachments.map { attachment in
            .object(["name": .string(attachment.name), "mime": .string(attachment.mime), "data_base64": .string(try decrypt(attachment).base64EncodedString())])
        })
    }

    @discardableResult
    public func remove(_ id: String, protected: Set<String>) throws -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let url = try file(id)
        if protected.contains(id) { return false }
        guard FileManager.default.fileExists(atPath: url.path) else { return false }
        try FileManager.default.removeItem(at: url)
        return true
    }
}

// MARK: - View cache

/// A bounded private cache of opened files: `<sha256(identity)>/f/<safe name>` plus `entry.json`
/// with the exact identity, size and digest, verified on every reuse. Entries expire a day after
/// creation and the least recently opened go first beyond the total budget.
public final class AttachmentViewCache: @unchecked Sendable {
    public struct Entry: Sendable {
        public let file: URL
        public let name: String
        public let mime: String
        public let sha256: String
        public let bytes: Int64
    }

    struct Metadata: Codable {
        var key: String
        var name: String
        var mime: String
        var file: String
        var bytes: Int64
        var sha256: String
        var created: Int64
    }

    public let root: URL
    let now: @Sendable () -> Int64
    let maxBytes: Int64
    let maxAge: Int64
    let lock = NSRecursiveLock()
    var active: Set<String> = []

    public init(root: URL, now: @escaping @Sendable () -> Int64 = { Clock.milliseconds() }, maxBytes: Int64 = 100 * 1024 * 1024,
                maxAge: Int64 = 24 * 60 * 60 * 1000) {
        self.root = root
        self.now = now
        self.maxBytes = maxBytes
        self.maxAge = maxAge
    }

    func prepare() throws {
        let manager = FileManager.default
        if (try? root.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true {
            throw AttachmentOpenError("Attachment storage on this phone is unavailable.")
        }
        if !manager.fileExists(atPath: root.path) {
            try manager.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        }
    }

    func directory(_ key: String) -> URL { root.appendingPathComponent(Self.digest(Data(key.utf8)), isDirectory: true) }

    static func digest(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    static func digest(file: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return nil }
        defer { try? handle.close() }
        var hasher = SHA256()
        while let data = try? handle.read(upToCount: 256 * 1024), !data.isEmpty { hasher.update(data: data) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    static func regular(_ url: URL) -> Bool {
        (try? FileManager.default.attributesOfItem(atPath: url.path)[.type] as? FileAttributeType) == .typeRegular
    }

    func touch(_ folder: URL) {
        try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: Double(now()) / 1000)], ofItemAtPath: folder.path)
    }

    public func lookup(_ key: String) -> Entry? {
        lock.lock()
        defer { lock.unlock() }
        guard (try? prepare()) != nil else { return nil }
        let folder = directory(key)
        let entry: Entry? = {
            let metadataFile = folder.appendingPathComponent("entry.json")
            guard Self.regular(metadataFile), let data = try? Data(contentsOf: metadataFile),
                  let metadata = try? JSONDecoder().decode(Metadata.self, from: data) else { return nil }
            let file = folder.appendingPathComponent("f", isDirectory: true).appendingPathComponent(metadata.file)
            let current = now()
            guard metadata.key == key, current - metadata.created < maxAge, metadata.created <= current + 60_000,
                  metadata.file == AttachmentPolicy.filename(metadata.file), Self.regular(file),
                  (try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? NSNumber)?.int64Value == metadata.bytes,
                  Self.digest(file: file) == metadata.sha256 else { return nil }
            return Entry(file: file, name: metadata.name, mime: metadata.mime, sha256: metadata.sha256, bytes: metadata.bytes)
        }()
        guard let entry else {
            try? FileManager.default.removeItem(at: folder)
            return nil
        }
        touch(folder)
        return entry
    }

    /// One download or decryption in progress; nothing is visible until `commit`.
    public final class Pending: @unchecked Sendable {
        let cache: AttachmentViewCache
        let key: String
        let name: String
        let mime: String
        let part: URL
        let file: URL
        let handle: FileHandle
        var hasher = SHA256()
        var written: Int64 = 0
        var finished = false

        init(cache: AttachmentViewCache, key: String, name: String, mime: String, part: URL) throws {
            self.cache = cache
            self.key = key
            self.name = name
            self.mime = mime
            self.part = part
            file = part.appendingPathComponent("f", isDirectory: true).appendingPathComponent(AttachmentPolicy.filename(name))
            guard FileManager.default.createFile(atPath: file.path, contents: nil, attributes: [.posixPermissions: 0o600]) else {
                throw AttachmentOpenError("Attachment storage on this phone is unavailable.")
            }
            handle = try FileHandle(forWritingTo: file)
        }

        public func write(_ data: Data) throws {
            precondition(!finished)
            written += Int64(data.count)
            if written > AttachmentPolicy.maxFileBytes { throw AttachmentOpenError("Attachments larger than 10 MiB cannot be opened.") }
            hasher.update(data: data)
            try handle.write(contentsOf: data)
        }

        public func commit(_ sha256: String) throws -> Entry {
            precondition(!finished)
            try handle.synchronize()
            try handle.close()
            let actual = hasher.finalize().map { String(format: "%02x", $0) }.joined()
            if written == 0 || actual != sha256 {
                abort()
                throw AttachmentOpenError("The downloaded attachment did not match its digest. Try again.")
            }
            let metadata = Metadata(key: key, name: name, mime: mime, file: file.lastPathComponent, bytes: written, sha256: actual, created: cache.now())
            try JSONEncoder().encode(metadata).write(to: part.appendingPathComponent("entry.json"))
            cache.lock.lock()
            defer { cache.lock.unlock() }
            let destination = cache.directory(key)
            try? FileManager.default.removeItem(at: destination)
            try FileManager.default.moveItem(at: part, to: destination)
            cache.active.remove(part.lastPathComponent)
            finished = true
            cache.touch(destination)
            cache.trimLocked()
            return Entry(file: destination.appendingPathComponent("f", isDirectory: true).appendingPathComponent(file.lastPathComponent),
                         name: name, mime: mime, sha256: actual, bytes: written)
        }

        public func abort() {
            if finished { return }
            finished = true
            try? handle.close()
            cache.lock.lock()
            defer { cache.lock.unlock() }
            try? FileManager.default.removeItem(at: part)
            cache.active.remove(part.lastPathComponent)
        }
    }

    public func begin(_ key: String, name: String, mime: String) throws -> Pending {
        lock.lock()
        defer { lock.unlock() }
        try prepare()
        let part = root.appendingPathComponent(".part-\(UUID().uuidString.lowercased())", isDirectory: true)
        try FileManager.default.createDirectory(at: part.appendingPathComponent("f", isDirectory: true), withIntermediateDirectories: true)
        active.insert(part.lastPathComponent)
        do {
            return try Pending(cache: self, key: key, name: name, mime: mime, part: part)
        } catch {
            try? FileManager.default.removeItem(at: part)
            active.remove(part.lastPathComponent)
            throw error
        }
    }

    /// Stores complete bytes at once; nothing is visible unless they match the expected digest.
    public func store(_ key: String, name: String, mime: String, data: Data, sha256: String) throws -> Entry {
        let pending = try begin(key, name: name, mime: mime)
        defer { pending.abort() }
        try pending.write(data)
        return try pending.commit(sha256)
    }

    public func trim() {
        lock.lock()
        defer { lock.unlock() }
        guard (try? prepare()) != nil else { return }
        trimLocked()
    }

    func trimLocked() {
        let manager = FileManager.default
        let current = now()
        var entries: [(url: URL, opened: Date, bytes: Int64)] = []
        for name in (try? manager.contentsOfDirectory(atPath: root.path)) ?? [] where !active.contains(name) {
            let url = root.appendingPathComponent(name)
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .contentModificationDateKey])
            guard !name.hasPrefix(".part-"), values?.isDirectory == true, values?.isSymbolicLink != true,
                  let data = try? Data(contentsOf: url.appendingPathComponent("entry.json")),
                  let metadata = try? JSONDecoder().decode(Metadata.self, from: data),
                  current - metadata.created < maxAge, metadata.created <= current + 60_000 else {
                try? manager.removeItem(at: url)
                continue
            }
            let files = (try? manager.contentsOfDirectory(at: url.appendingPathComponent("f"), includingPropertiesForKeys: [.fileSizeKey])) ?? []
            let size = files.reduce(Int64(0)) { $0 + Int64((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
            entries.append((url, values?.contentModificationDate ?? .distantPast, size))
        }
        entries.sort { $0.opened < $1.opened }
        var total = entries.reduce(Int64(0)) { $0 + $1.bytes }
        while total > maxBytes, !entries.isEmpty {
            let oldest = entries.removeFirst()
            try? manager.removeItem(at: oldest.url)
            total -= oldest.bytes
        }
    }

    public func clear() {
        lock.lock()
        defer { lock.unlock() }
        for name in (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? [] where !active.contains(name) {
            try? FileManager.default.removeItem(at: root.appendingPathComponent(name))
        }
    }
}

// MARK: - Downloads

/// Reads a message attachment range by range from its owning computer. Reads are never retried
/// automatically; every range must answer the exact request, and nothing is visible until the
/// complete bytes match the first range's digest.
public enum AttachmentDownloads {
    /// Submits one range request and returns its settled receipt.
    public typealias Read = @Sendable (_ requestId: String, _ payload: JSONObject) async throws -> JSONObject

    public static func load(_ cache: AttachmentViewCache, key: String, target: Target, file: DisplayedAttachment, read: Read,
                            progress: @Sendable (Double) async -> Void = { _ in }) async throws -> AttachmentViewCache.Entry {
        let pending = try cache.begin(key, name: file.name, mime: file.mime)
        defer { pending.abort() }
        var offset: Int64 = 0
        var first: AttachmentIdentity.Chunk?
        while offset < file.bytes {
            try Task.checkCancellation()
            let length = Int(min(Int64(AttachmentIdentity.chunkBytes), file.bytes - offset))
            let id = UUID().uuidString.lowercased()
            let payload = try AttachmentIdentity.payload(target, file, requestId: id, offset: offset, length: length)
            let receipt: JSONObject
            do { receipt = try await read(id, payload) } catch let error as RelayError {
                throw AttachmentOpenError(AttachmentIdentity.relayFailure(error.status))
            }
            guard receipt.string("state") == "completed", let result = receipt.object("result") else {
                throw AttachmentOpenError(AttachmentIdentity.failure(receipt))
            }
            let chunk = try AttachmentIdentity.chunk(result, requestId: id, target: target, file: file, offset: offset, length: length, first: first)
            if first == nil { first = chunk }
            try pending.write(chunk.data)
            offset += Int64(chunk.data.count)
            await progress(Double(offset) / Double(file.bytes))
        }
        guard let digest = first?.sha256 else { throw AttachmentOpenError("The attachment is empty.") }
        return try pending.commit(digest)
    }
}

/// Reads a linked computer file range by range, keyed by its exact computer, conversation, path and digest.
public enum ComputerFileDownloads {
    public typealias Read = @Sendable (_ requestId: String, _ payload: JSONObject) async throws -> JSONObject

    static func relayFailure(_ status: Int) -> String {
        switch status {
        case 400, 409: ComputerFileProtocol.updateRequired
        case 401, 403: "Reconnect to this gateway, then try again."
        case 429: "The gateway is busy. Try again in a moment."
        default: "The gateway could not deliver this file. Try again."
        }
    }

    public static func load(_ cache: AttachmentViewCache, target: Target, reference: ComputerFileReference, read: Read,
                            progress: @Sendable (Double) async -> Void = { _ in }) async throws -> (key: String, entry: AttachmentViewCache.Entry) {
        func range(_ offset: Int64, _ length: Int, _ first: ComputerFileProtocol.Chunk?) async throws -> ComputerFileProtocol.Chunk {
            try Task.checkCancellation()
            let id = UUID().uuidString.lowercased()
            let payload = try ComputerFileProtocol.payload(target, reference, requestId: id, offset: offset, length: length, first: first)
            let receipt: JSONObject
            do { receipt = try await read(id, payload) } catch let error as RelayError { throw AttachmentOpenError(relayFailure(error.status)) }
            guard receipt.string("state") == "completed", let result = receipt.object("result") else {
                throw AttachmentOpenError(ComputerFileProtocol.failure(receipt))
            }
            return try ComputerFileProtocol.chunk(result, requestId: id, target: target, reference: reference, offset: offset, length: length, first: first)
        }
        let first = try await range(0, ComputerFileProtocol.chunkBytes, nil)
        let key = ComputerFileProtocol.cacheKey(target, first)
        if let cached = cache.lookup(key) { return (key, cached) }
        let pending = try cache.begin(key, name: first.name, mime: first.mime)
        defer { pending.abort() }
        try pending.write(first.data)
        var offset = Int64(first.data.count)
        await progress(Double(offset) / Double(first.bytes))
        while offset < first.bytes {
            let length = Int(min(Int64(ComputerFileProtocol.chunkBytes), first.bytes - offset))
            let chunk = try await range(offset, length, first)
            try pending.write(chunk.data)
            offset += Int64(chunk.data.count)
            await progress(Double(offset) / Double(first.bytes))
        }
        return (key, try pending.commit(first.sha256))
    }
}
