import CryptoKit
import Foundation
import Synchronization

/// A bounded, sealed, presentation-only conversation store ported from Android
/// `EncryptedConversationFiles`. Each object is AES-GCM sealed with its file name
/// as authenticated data, written atomically and verified against the exact
/// target on read. Cached data never verifies permission to send.
public final class ConversationFiles: Sendable {
    public struct Generation: Hashable, Sendable {
        let global: Int64
        let workspace: Int64
    }

    struct Entry: Sendable {
        let target: Target
        let payload: Data
        let storedAt: Int64
        let observedAt: Int64
    }

    struct Memory: Sendable {
        var entries: [String: Entry] = [:]
        /// Access order, oldest first.
        var order: [String] = []
        var generations: [String: Int64] = [:]
        var disconnected: Set<String> = []
        var global: Int64 = 0
        var pendingClears = 0

        mutating func touch(_ key: String) {
            order.removeAll { $0 == key }
            order.append(key)
        }
        mutating func remove(where predicate: (Entry) -> Bool) {
            for (key, entry) in entries where predicate(entry) {
                entries[key] = nil
                order.removeAll { $0 == key }
            }
        }
        func generation(_ connection: String) -> Generation { Generation(global: global, workspace: generations[connection] ?? 0) }
        func blocked(_ connection: String) -> Bool { pendingClears > 0 || disconnected.contains(connection) }
    }

    struct Envelope: Codable {
        var version: Int
        var target: Target
        var identity: JSON
        var payload: Data
        var storedAt: Int64
        var observedAt: Int64
    }

    public let root: URL
    let keys: KeyProvider
    let now: @Sendable () -> Int64
    let maxEntries: Int
    let maxBytes: Int64
    let maxPayloadBytes: Int
    let ttl: Int64
    let memory = Mutex(Memory())
    let disk = NSLock()
    /// Runs after each atomic rename; tests use it to interleave invalidation.
    let afterRename = Mutex<(@Sendable () -> Void)?>(nil)

    public init(root: URL, keys: KeyProvider, now: @escaping @Sendable () -> Int64 = { Clock.milliseconds() },
                maxEntries: Int = 20, maxBytes: Int64 = 20 * 1024 * 1024, maxPayloadBytes: Int = 1024 * 1024,
                ttl: Int64 = 7 * 24 * 60 * 60 * 1000) {
        self.root = root
        self.keys = keys
        self.now = now
        self.maxEntries = maxEntries
        self.maxBytes = maxBytes
        self.maxPayloadBytes = maxPayloadBytes
        self.ttl = ttl
    }

    var maxFileBytes: Int { maxPayloadBytes + 65536 }

    public func generation(_ connection: String) -> Generation { memory.withLock { $0.generation(connection) } }

    /// Stops a workspace (or every workspace) from reading or admitting history until its purge finishes.
    public func invalidate(_ connection: String?) {
        memory.withLock { state in
            if let connection {
                state.disconnected.insert(connection)
                state.generations[connection, default: 0] += 1
                state.remove { $0.target.connectionId == connection }
            } else {
                state.global += 1
                state.pendingClears += 1
                state.entries = [:]
                state.order = []
            }
        }
    }

    func validAge(_ at: Int64) -> Bool {
        let current = now()
        return at > 0 && at <= current && current - at <= ttl
    }

    /// Payload bytes already in memory; no filesystem or decryption work.
    public func memoryPayload(_ target: Target) -> Data? {
        memory.withLock { state in
            guard !state.blocked(target.connectionId), let entry = state.entries[target.key], validAge(entry.storedAt) else { return nil }
            state.touch(target.key)
            return entry.payload
        }
    }

    static func identity(_ target: Target, _ raw: JSONObject) -> Bool {
        !target.connectionId.isEmpty && !target.computerId.isEmpty && !target.session.isEmpty && target.key.utf8.count <= 16 * 1024 &&
            raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation &&
            raw.string("archive_id") == target.archiveId && (!target.archiveId.isEmpty || raw.string("state") != "archived") &&
            raw.string("agent_id") == target.agentId && (target.agentId.isEmpty || raw.string("parent_conversation_id") == target.parentConversation)
    }

    static func name(_ target: Target) -> String {
        SHA256.hash(data: Data(target.key.utf8)).map { String(format: "%02x", $0) }.joined() + ".bin"
    }

    func ensureRoot() throws {
        let manager = FileManager.default
        var isDirectory: ObjCBool = false
        if !manager.fileExists(atPath: root.path, isDirectory: &isDirectory) {
            try manager.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            var url = root
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? url.setResourceValues(values)
        }
        let attributes = try manager.attributesOfItem(atPath: root.path)
        guard attributes[.type] as? FileAttributeType == .typeDirectory else { throw ZerusError.invalid("Invalid history cache directory") }
        for name in (try? manager.contentsOfDirectory(atPath: root.path)) ?? [] where name.hasPrefix(".") && name.hasSuffix(".part") {
            try? manager.removeItem(at: root.appendingPathComponent(name))
        }
    }

    func objects() -> [URL] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        return names.filter { $0.count == 68 && $0.hasSuffix(".bin") && $0.dropLast(4).allSatisfy { $0.isHexDigit && !$0.isUppercase } }
            .map { root.appendingPathComponent($0) }
    }

    /// Regular, bounded file metadata, never following a symbolic link.
    func regular(_ url: URL) -> (size: Int64, modified: Int64)? {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              attributes[.type] as? FileAttributeType == .typeRegular,
              let size = (attributes[.size] as? NSNumber)?.int64Value, (29...Int64(maxFileBytes)).contains(size),
              let modified = attributes[.modificationDate] as? Date else { return nil }
        return (size, Int64((modified.timeIntervalSince1970 * 1000).rounded()))
    }

    func decode(_ url: URL) -> Entry? {
        guard regular(url) != nil, let sealed = try? Data(contentsOf: url),
              let box = try? AES.GCM.SealedBox(combined: sealed), let key = try? keys.key(),
              let opened = try? AES.GCM.open(box, using: key, authenticating: Data(url.lastPathComponent.utf8)),
              let envelope = try? JSONDecoder().decode(Envelope.self, from: opened), envelope.version == 1,
              let identity = envelope.identity.object, Self.name(envelope.target) == url.lastPathComponent,
              Self.identity(envelope.target, identity), envelope.payload.count <= maxPayloadBytes else { return nil }
        let entry = Entry(target: envelope.target, payload: envelope.payload, storedAt: envelope.storedAt, observedAt: envelope.observedAt)
        guard validAge(entry.storedAt), entry.observedAt > 0, entry.observedAt <= entry.storedAt else { return nil }
        return entry
    }

    @discardableResult
    func remember(_ entry: Entry, _ captured: Generation) -> Bool {
        memory.withLock { state in
            guard !state.blocked(entry.target.connectionId), captured == state.generation(entry.target.connectionId) else { return false }
            if let old = state.entries[entry.target.key], old.observedAt > entry.observedAt { return true }
            state.entries[entry.target.key] = entry
            state.touch(entry.target.key)
            while state.entries.count > maxEntries || state.entries.values.reduce(0, { $0 + Int64($1.payload.count) }) > maxBytes {
                let oldest = state.order.removeFirst()
                state.entries[oldest] = nil
            }
            return true
        }
    }

    /// The verified payload for this exact target, from memory or the sealed file.
    public func read(_ target: Target) -> Data? {
        disk.lock()
        defer { disk.unlock() }
        let captured = generation(target.connectionId)
        if memory.withLock({ $0.blocked(target.connectionId) }) { return nil }
        if let payload = memoryPayload(target) { return payload }
        guard (try? ensureRoot()) != nil else { return nil }
        prune()
        let url = root.appendingPathComponent(Self.name(target))
        guard let entry = decode(url), entry.target == target else {
            try? FileManager.default.removeItem(at: url)
            return nil
        }
        return remember(entry, captured) ? entry.payload : nil
    }

    /// Seals and atomically stores a payload whose identity matches the exact target.
    /// An older observation never replaces a newer confirmed one.
    public func write(_ target: Target, identity: JSONObject, payload: Data, observedAt: Int64? = nil, generation captured: Generation? = nil) {
        disk.lock()
        defer { disk.unlock() }
        let captured = captured ?? generation(target.connectionId)
        let observed = observedAt ?? now()
        guard !memory.withLock({ $0.blocked(target.connectionId) }), captured == generation(target.connectionId),
              Self.identity(target, identity), payload.count <= maxPayloadBytes, observed > 0, observed <= now() else { return }
        guard (try? ensureRoot()) != nil else { return }
        let name = Self.name(target)
        let url = root.appendingPathComponent(name)
        if let previous = decode(url), previous.observedAt > observed {
            remember(previous, captured)
            return
        }
        let entry = Entry(target: target, payload: payload, storedAt: now(), observedAt: observed)
        let envelope = Envelope(version: 1, target: target, identity: .object(identity), payload: payload, storedAt: entry.storedAt, observedAt: observed)
        guard let plain = try? JSONEncoder().encode(envelope), let key = try? keys.key(),
              let sealed = try? AES.GCM.seal(plain, using: key, authenticating: Data(name.utf8)).combined,
              sealed.count <= maxFileBytes, Int64(sealed.count) <= maxBytes else { return }
        let temporary = root.appendingPathComponent(".\(UUID().uuidString.lowercased()).part")
        defer { try? FileManager.default.removeItem(at: temporary) }
        guard FileManager.default.createFile(atPath: temporary.path, contents: nil, attributes: [.posixPermissions: 0o600]),
              let handle = try? FileHandle(forWritingTo: temporary) else { return }
        do {
            try handle.write(contentsOf: sealed)
            try handle.synchronize()
            try handle.close()
        } catch {
            try? handle.close()
            return
        }
        #if os(iOS)
        try? FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: temporary.path)
        #endif
        guard captured == generation(target.connectionId), rename(temporary.path, url.path) == 0 else { return }
        try? FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSince1970: Double(now()) / 1000)], ofItemAtPath: url.path)
        syncDirectory()
        afterRename.withLock { $0 }?()
        remember(entry, captured)
        prune()
    }

    func syncDirectory() {
        let descriptor = open(root.path, O_RDONLY)
        guard descriptor >= 0 else { return }
        fsync(descriptor)
        close(descriptor)
    }

    /// Disk age is the last confirmed write, never a presentation read.
    func prune() {
        memory.withLock { state in state.remove { !validAge($0.storedAt) } }
        var rows: [(url: URL, size: Int64, modified: Int64)] = objects().compactMap { url in
            guard let metadata = regular(url), validAge(metadata.modified) else {
                try? FileManager.default.removeItem(at: url)
                return nil
            }
            return (url, metadata.size, metadata.modified)
        }.sorted { $0.modified < $1.modified }
        var bytes = rows.reduce(0) { $0 + $1.size }
        while rows.count > maxEntries || bytes > maxBytes {
            let oldest = rows.removeFirst()
            bytes -= oldest.size
            try? FileManager.default.removeItem(at: oldest.url)
            memory.withLock { state in state.remove { Self.name($0.target) == oldest.url.lastPathComponent } }
        }
    }

    /// Deletes this workspace's files; unreadable files are removed too.
    public func purgeWorkspace(_ connection: String) {
        disk.lock()
        defer { disk.unlock() }
        guard (try? ensureRoot()) != nil else { return }
        for url in objects() {
            let owner = decode(url)?.target.connectionId
            if owner == nil || owner == connection { try? FileManager.default.removeItem(at: url) }
        }
        memory.withLock { state in state.remove { $0.target.connectionId == connection } }
        syncDirectory()
    }

    public func clear() {
        disk.lock()
        defer { disk.unlock() }
        if (try? ensureRoot()) != nil {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? [] {
                try? FileManager.default.removeItem(at: root.appendingPathComponent(name))
            }
        }
        memory.withLock { state in
            state.entries = [:]
            state.order = []
            if state.pendingClears > 0 { state.pendingClears -= 1 }
        }
        syncDirectory()
    }
}

// MARK: - Display cache

/// Private display rows supplement a sealed cache, never native mutation evidence.
public struct ConversationSnapshot: Codable, Sendable {
    public var raw: JSON
    public var events: [Event]
    public var truncated: Bool
}

public enum ConversationDisplayCache {
    static let streams = ["events", "message_events", "provider_messages", "attachment_messages"]

    public struct Decoded: Sendable {
        public let raw: JSONObject
        public let events: [Event]
        public let truncated: Bool
    }

    /// Native metadata without its event streams, plus whole newest display rows within budget.
    public static func encode(_ target: Target, raw: JSONObject, events: [Event], truncated: Bool) throws -> Data {
        var metadata = raw
        for stream in streams { metadata[stream] = nil }
        let metadataBytes = JSON.object(metadata).compact.utf8.count
        let budget = min(max(1_048_576 - metadataBytes - 4096, 2), 700 * 1024)
        let retained = RollingConversationHistory.merge(target: target, previousTarget: target, previous: [], incoming: events, maxBytes: budget)
        return try JSONEncoder().encode(ConversationSnapshot(raw: .object(metadata), events: retained.events, truncated: truncated || retained.truncated))
    }

    public static func decode(_ data: Data) -> Decoded? {
        guard let snapshot = try? JSONDecoder().decode(ConversationSnapshot.self, from: data), let raw = snapshot.raw.object,
              snapshot.events.count <= 500 else { return nil }
        return Decoded(raw: raw, events: snapshot.events.map(ActivityNotices.cached), truncated: snapshot.truncated || raw.bool("mobile_history_truncated"))
    }
}

/// The sealed last-known conversation per exact target.
public final class ConversationCache: Sendable {
    public let files: ConversationFiles

    public init(files: ConversationFiles) { self.files = files }

    public static func live() throws -> ConversationCache {
        let base = try FileManager.default.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return ConversationCache(files: ConversationFiles(root: base.appendingPathComponent("conversation-cache-v1", isDirectory: true),
                                                          keys: KeychainKeyProvider(account: "conversation-cache-v1")))
    }

    public func memory(_ target: Target) -> ConversationDisplayCache.Decoded? {
        files.memoryPayload(target).flatMap(ConversationDisplayCache.decode)
    }

    public func read(_ target: Target) async -> ConversationDisplayCache.Decoded? {
        let files = self.files
        return await Task.detached(priority: .userInitiated) { files.read(target).flatMap(ConversationDisplayCache.decode) }.value
    }

    public func write(_ target: Target, raw: JSONObject, events: [Event], truncated: Bool, observedAt: Int64) async {
        let files = self.files
        let captured = files.generation(target.connectionId)
        await Task.detached(priority: .utility) {
            guard let payload = try? ConversationDisplayCache.encode(target, raw: raw, events: events, truncated: truncated) else { return }
            files.write(target, identity: raw, payload: payload, observedAt: observedAt, generation: captured)
        }.value
    }

    public func purgeWorkspace(_ connection: String) async {
        files.invalidate(connection)
        let files = self.files
        await Task.detached(priority: .utility) { files.purgeWorkspace(connection) }.value
    }
}

// MARK: - Page cache

/// A retained canonical page or window slice. Cached pages are presentation only.
public struct CachedHistoryPage: Codable, Hashable, Sendable {
    public var target: Target
    public var epoch: String
    public var events: [Event]
    public var before: String?
    public var after: String?
    public var hasBefore: Bool
    public var hasAfter: Bool
    public var latestIncoming: Int64?
    public var totalIncoming: Int64?
    public var complete: Bool
    public var indexing: Bool
    public var truncated: Bool
    // Optional decoding preserves old encrypted pages; absent coverage is zero.
    public var projectionApplied: Int64?

    public init(_ page: HistoryPage) {
        self.init(target: page.target, epoch: page.epoch, events: page.events, before: page.before, after: page.after, hasBefore: page.hasBefore,
                  hasAfter: page.hasAfter, latestIncoming: page.latestIncoming, totalIncoming: page.totalIncoming, complete: page.complete,
                  indexing: page.indexing, truncated: page.truncated, projectionApplied: page.projectionApplied)
    }

    public init(target: Target, epoch: String, events: [Event], before: String?, after: String?, hasBefore: Bool, hasAfter: Bool,
                latestIncoming: Int64?, totalIncoming: Int64?, complete: Bool, indexing: Bool, truncated: Bool, projectionApplied: Int64 = 0) {
        self.target = target
        self.epoch = epoch
        self.events = events
        self.before = before
        self.after = after
        self.hasBefore = hasBefore
        self.hasAfter = hasAfter
        self.latestIncoming = latestIncoming
        self.totalIncoming = totalIncoming
        self.complete = complete
        self.indexing = indexing
        self.truncated = truncated
        self.projectionApplied = projectionApplied
    }

    /// Strict decoding: a foreign scope or a non-canonical row is a miss.
    public func page(for target: Target) throws -> HistoryPage {
        func require(_ condition: Bool) throws { if !condition { throw ZerusError.mismatch("Saved history page is invalid.") } }
        try require((projectionApplied ?? 0) >= 0)
        try require(self.target == target && !epoch.isEmpty && epoch.count <= 512 && events.count <= 100)
        try require(events.allSatisfy { $0.historyEpoch == epoch && !$0.historyId.isEmpty && !$0.historyCursor.isEmpty })
        for cursor in [before, after].compactMap({ $0 }) { try require(!cursor.isEmpty && cursor.count <= 8192) }
        return HistoryPage(target: target, epoch: epoch, events: events.map(ActivityNotices.cached), before: before, after: after, hasBefore: hasBefore, hasAfter: hasAfter,
                           latestIncoming: latestIncoming, totalIncoming: totalIncoming, complete: complete, indexing: indexing, truncated: truncated, projectionApplied: projectionApplied ?? 0)
    }

    static func slice(_ window: HistoryWindow, _ start: Int, _ end: Int) -> CachedHistoryPage? {
        let rows = Array(window.events[start..<end])
        guard let first = rows.first, let last = rows.last else { return nil }
        let page = CachedHistoryPage(target: window.target, epoch: window.epoch, events: rows, before: first.historyCursor, after: last.historyCursor,
                                     hasBefore: window.hasBefore || start > 0, hasAfter: window.hasAfter || end < window.events.count,
                                     latestIncoming: nil, totalIncoming: nil, complete: false, indexing: false, truncated: true, projectionApplied: window.projectionApplied)
        return rows.reduce(0, { $0 + $1.approximateBytes }) < 900 * 1024 ? page : nil
    }

    /// Inspection rows stay in memory but never supply a canonical cursor or read sequence.
    static func canonical(_ window: HistoryWindow) -> HistoryWindow {
        var copy = window
        copy.events = window.events.filter { $0.historyEpoch == window.epoch && !$0.historyId.isEmpty && !$0.historyCursor.isEmpty }
        let omitted = copy.events.count != window.events.count
        copy.hasAfter = window.hasAfter || omitted
        copy.atHead = window.atHead && !omitted
        return copy
    }

    /// The 100-row slice around a reading anchor.
    public static func retained(_ window: HistoryWindow, anchor: String) -> CachedHistoryPage? {
        let canonical = canonical(window)
        guard let index = canonical.events.firstIndex(where: { $0.historyCursor == anchor }) else { return nil }
        let start = max(index - 49, 0)
        return slice(canonical, start, min(start + 100, canonical.events.count))
    }

    /// The neighboring slices, keyed as the requests that would load them.
    public static func neighbors(_ window: HistoryWindow, anchor: String) -> [(direction: String, cursor: String, page: CachedHistoryPage)] {
        let canonical = canonical(window)
        guard let index = canonical.events.firstIndex(where: { $0.historyCursor == anchor }) else { return [] }
        let start = max(index - 49, 0), end = min(start + 100, canonical.events.count)
        var result: [(String, String, CachedHistoryPage)] = []
        if start > 0, let page = slice(canonical, max(start - 100, 0), start) { result.append(("before", canonical.events[start].historyCursor, page)) }
        if end < canonical.events.count, let page = slice(canonical, end, min(end + 100, canonical.events.count)) {
            result.append(("after", canonical.events[end - 1].historyCursor, page))
        }
        return result
    }
}

public enum ConversationPageKeys {
    public static let modes: Set<String> = ["", "before", "after", "around", "unread"]

    public static func storageTarget(_ target: Target, mode: String, cursor: String) throws -> Target {
        guard modes.contains(mode), cursor.count <= 8192 else { throw ZerusError.invalid("Invalid history page key.") }
        let digest = SHA256.hash(data: Data(JSON.array([.string(mode), .string(cursor)]).compact.utf8)).map { String(format: "%02x", $0) }.joined()
        var storage = target
        storage.session = target.session + "/zerus-private-page/" + digest
        return storage
    }

    static func identity(_ target: Target, _ storage: Target) -> JSONObject {
        var raw: JSONObject = ["name": .string(storage.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation)]
        if !target.archiveId.isEmpty { raw["archive_id"] = .string(target.archiveId) }
        if !target.agentId.isEmpty {
            raw["agent_id"] = .string(target.agentId)
            raw["parent_conversation_id"] = .string(target.parentConversation)
        }
        return raw
    }
}

/// Independent sealed page retention. Internal storage identities never leave this type.
public final class ConversationPageCache: Sendable {
    public let files: ConversationFiles

    public init(files: ConversationFiles) { self.files = files }

    public static func live() throws -> ConversationPageCache {
        let base = try FileManager.default.url(for: .cachesDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return ConversationPageCache(files: ConversationFiles(root: base.appendingPathComponent("conversation-pages-v1", isDirectory: true),
                                                              keys: KeychainKeyProvider(account: "conversation-pages-v1"),
                                                              maxEntries: 128, maxBytes: 64 * 1024 * 1024))
    }

    public func read(_ target: Target, mode: String, cursor: String = "") async throws -> HistoryPage? {
        let storage = try ConversationPageKeys.storageTarget(target, mode: mode, cursor: cursor)
        let files = self.files
        guard let data = await Task.detached(priority: .userInitiated, operation: { files.read(storage) }).value else { return nil }
        return try JSONDecoder().decode(CachedHistoryPage.self, from: data).page(for: target)
    }

    public func write(_ target: Target, mode: String, cursor: String, page: CachedHistoryPage) async {
        guard let storage = try? ConversationPageKeys.storageTarget(target, mode: mode, cursor: cursor), page.target == target else { return }
        let files = self.files
        let captured = files.generation(target.connectionId)
        await Task.detached(priority: .utility) {
            guard let payload = try? JSONEncoder().encode(page) else { return }
            files.write(storage, identity: ConversationPageKeys.identity(target, storage), payload: payload, generation: captured)
        }.value
    }

    public func purgeWorkspace(_ connection: String) async {
        files.invalidate(connection)
        let files = self.files
        await Task.detached(priority: .utility) { files.purgeWorkspace(connection) }.value
    }
}

public enum HistoryPageRetention {
    /// Metadata-only probes use the same newest selector but cannot replace a full page.
    public static func retains(limit: Int, page: HistoryPage, direction: String) -> Bool {
        limit == 100 && (page.complete || !direction.isEmpty)
    }
}

// MARK: - Read paths

public enum HistoryReadPaths {
    public struct Result: Sendable {
        public let page: HistoryPage
        public let verified: Bool
        public init(page: HistoryPage, verified: Bool) {
            self.page = page
            self.verified = verified
        }
    }

    public struct Reopened: Sendable {
        public let result: Result
        public let head: Bool
    }

    /// A corrupt or incompatible saved page is a miss, never evidence for a live action.
    public static func saved(isolation: isolated (any Actor)? = #isolation, _ cached: () async throws -> HistoryPage?) async throws -> HistoryPage? {
        do { return try await cached() }
        catch is CancellationError { throw CancellationError() }
        catch { return nil }
    }

    /// A network page is verified; a saved page only stands in when the network read fails.
    public static func read(isolation: isolated (any Actor)? = #isolation, network: () async throws -> HistoryPage,
                            cached: () async throws -> HistoryPage?) async throws -> Result {
        do { return Result(page: try await network(), verified: true) }
        catch is CancellationError { throw CancellationError() }
        catch {
            guard let page = try await saved(cached) else { throw error }
            return Result(page: page, verified: false)
        }
    }

    /// A reset index rejects every cursor of its previous epoch. Reopen the verified head once it reports another epoch.
    public static func reopening(isolation: isolated (any Actor)? = #isolation, epoch: String?, read: () async throws -> Result,
                                 head: () async throws -> HistoryPage) async throws -> Reopened {
        var failure: Error?
        var first: Result?
        do { first = try await read() }
        catch is CancellationError { throw CancellationError() }
        catch { failure = error }
        if let epoch, !epoch.isEmpty, first?.verified != true {
            let latest: HistoryPage?
            do { latest = try await head() }
            catch is CancellationError { throw CancellationError() }
            catch { latest = nil }
            if let latest, !latest.epoch.isEmpty, latest.epoch != epoch { return Reopened(result: Result(page: latest, verified: true), head: true) }
        }
        if let first { return Reopened(result: first, head: false) }
        throw failure ?? ZerusError(.unavailable, "History is not available yet. Try again.")
    }
}
