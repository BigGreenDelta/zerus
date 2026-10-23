import CryptoKit
import Foundation
import SQLite3

struct WorkspaceRoute: Hashable, Sendable {
    let computerId: String
    let gatewayId: String
    let routeId: String
    let machineId: String
    var json: JSONObject { ["computer_id": .string(computerId), "gateway_id": .string(gatewayId), "route_id": .string(routeId), "machine_id": .string(machineId)] }
    static func parse(_ j: JSONObject) throws -> WorkspaceRoute {
        try WorkspaceProtocol.fields(j, ["computer_id", "gateway_id", "route_id", "machine_id"])
        let value = try WorkspaceRoute(computerId: WorkspaceProtocol.string(j, "computer_id"), gatewayId: WorkspaceProtocol.string(j, "gateway_id"), routeId: WorkspaceProtocol.string(j, "route_id"), machineId: WorkspaceProtocol.string(j, "machine_id"))
        for id in [value.computerId, value.gatewayId, value.routeId, value.machineId] { try WorkspaceProtocol.identifier(id) }
        return value
    }
}

/// Only the journal may serialize this private state into protected storage.
struct WorkspaceState: Sendable {
    let identity: WorkspaceIdentity
    let candidate: WorkspaceCandidate
    let issuer: WorkspaceDevice
    let pinnedRevision: Int64
    let pinnedHead: String
    var membership: WorkspaceMembership?
    var routes: [String: WorkspaceRoute] = [:]
    var enrollment: WorkspaceApproval?
    var json: JSONObject {
        ["version": 1, "identity": .object(identity.privateJSON), "candidate": .object(candidate.json), "issuer": .object(issuer.json),
         "pinned_revision": .int(pinnedRevision), "pinned_head": .string(pinnedHead), "membership": membership.map { .object($0.json) } ?? .null,
         "routes": .object(JSONObject(routes.keys.sorted().map { ($0, .object(routes[$0]!.json)) })), "enrollment": enrollment.map { .object($0.json) } ?? .null]
    }
    static func parse(_ j: JSONObject) throws -> WorkspaceState {
        try WorkspaceProtocol.fields(j, ["version", "identity", "candidate", "issuer", "pinned_revision", "pinned_head", "membership", "routes", "enrollment"])
        guard try WorkspaceProtocol.number(j, "version") == 1, let identity = j["identity"]?.object, let candidate = j["candidate"]?.object,
              let issuer = j["issuer"]?.object, let routeRows = j["routes"]?.object, routeRows.count <= 4096 else { throw ZerusError.invalid("Invalid private workspace state.") }
        var routes: [String: WorkspaceRoute] = [:]
        for (id, row) in routeRows {
            let route = try WorkspaceRoute.parse(row.object ?? JSONObject())
            guard route.computerId == id else { throw ZerusError.mismatch("Different private workspace route.") }
            routes[id] = route
        }
        let state = try WorkspaceState(identity: WorkspaceIdentity.parse(identity), candidate: WorkspaceCandidate.parse(candidate), issuer: WorkspaceDevice.parse(issuer),
            pinnedRevision: WorkspaceProtocol.number(j, "pinned_revision"), pinnedHead: WorkspaceProtocol.string(j, "pinned_head"),
            membership: j["membership"]?.isNull == true ? nil : WorkspaceMembership.parse(j["membership"]?.object ?? JSONObject()), routes: routes,
            enrollment: j["enrollment"]?.isNull == true ? nil : WorkspaceApproval.parse(j["enrollment"]?.object ?? JSONObject()))
        guard state.identity.device == state.candidate.device, state.identity.device.role == "phone", state.issuer.role == "computer",
              state.membership == nil || state.membership?.workspaceId == state.candidate.workspaceId else { throw ZerusError.mismatch("Private workspace state has different keys.") }
        _ = try WorkspaceProtocol.decode(state.pinnedHead, size: 32)
        return state
    }
    func active() throws -> WorkspaceMembership {
        guard let membership, enrollment != nil, membership.members[identity.device.deviceId] == identity.device else {
            throw ZerusError(.unavailable, "Waiting for the inviting computer to approve this phone. Refresh to check again.")
        }
        return membership
    }
}

/// Native SQLite keeps state, signed history and its index in one FULL transaction.
/// Each connection's private identity has its own ThisDeviceOnly wrapping key.
public final class SQLiteWorkspaceJournal: @unchecked Sendable {
    private static let lock = NSRecursiveLock()
    private var database: OpaquePointer?
    private let directory: URL
    private let testKeys: KeyProvider?
    private let transient = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
    private enum Value { case text(String), integer(Int64), blob(Data), null }

    public init(directory: URL, testKeys: KeyProvider? = nil) { self.directory = directory; self.testKeys = testKeys }
    deinit { if let database { sqlite3_close(database) } }
    public static func applicationSupport() throws -> SQLiteWorkspaceJournal {
        let base = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return SQLiteWorkspaceJournal(directory: base.appendingPathComponent("Zerus/workspace-transport-v2", isDirectory: true))
    }
    private func locked<T>(_ action: () throws -> T) rethrows -> T { Self.lock.lock(); defer { Self.lock.unlock() }; return try action() }
    private func fail() -> ZerusError { ZerusError(.unavailable, "Encrypted workspace journal is unavailable. Plaintext fallback is disabled.") }
    private func open() throws -> OpaquePointer {
        if let database { return database }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        #if os(iOS)
        try FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: directory.path)
        #endif
        var dir = directory, resources = URLResourceValues(); resources.isExcludedFromBackup = true; try dir.setResourceValues(resources)
        let file = directory.appendingPathComponent("journal.sqlite3")
        var db: OpaquePointer?
        guard sqlite3_open_v2(file.path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nil) == SQLITE_OK, let db else { throw fail() }
        database = db
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        #if os(iOS)
        try FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: file.path)
        #endif
        try execute("PRAGMA synchronous=FULL")
        try execute("CREATE TABLE IF NOT EXISTS workspace_state (connection TEXT PRIMARY KEY,token_hash TEXT NOT NULL,record BLOB NOT NULL)")
        try execute("CREATE TABLE IF NOT EXISTS membership (connection TEXT NOT NULL,revision INTEGER NOT NULL,head TEXT NOT NULL,change TEXT,PRIMARY KEY(connection,revision))")
        try execute("CREATE TABLE IF NOT EXISTS outgoing (connection TEXT NOT NULL,request_id TEXT NOT NULL,plain_hash TEXT NOT NULL,header TEXT NOT NULL,envelope TEXT,PRIMARY KEY(connection,request_id))")
        try execute("CREATE TABLE IF NOT EXISTS outgoing_quota (id INTEGER PRIMARY KEY CHECK(id=1),bytes INTEGER NOT NULL)")
        try execute("INSERT OR IGNORE INTO outgoing_quota(id,bytes) VALUES(1,0)")
        return db
    }
    private func statement<T>(_ sql: String, _ values: [Value] = [], _ action: (OpaquePointer) throws -> T) throws -> T {
        let db = try open(); var pointer: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &pointer, nil) == SQLITE_OK, let pointer else { throw fail() }
        defer { sqlite3_finalize(pointer) }
        for (offset, value) in values.enumerated() {
            let index = Int32(offset + 1), result: Int32
            switch value {
            case .text(let text): result = sqlite3_bind_text(pointer, index, text, -1, transient)
            case .integer(let integer): result = sqlite3_bind_int64(pointer, index, integer)
            case .blob(let data): result = data.withUnsafeBytes { sqlite3_bind_blob(pointer, index, $0.baseAddress, Int32(data.count), transient) }
            case .null: result = sqlite3_bind_null(pointer, index)
            }
            guard result == SQLITE_OK else { throw fail() }
        }
        return try action(pointer)
    }
    private func execute(_ sql: String, _ values: [Value] = []) throws {
        try statement(sql, values) { guard sqlite3_step($0) == SQLITE_DONE else { throw fail() } }
    }
    private func transaction<T>(_ action: () throws -> T) throws -> T {
        try execute("BEGIN IMMEDIATE")
        do { let value = try action(); try execute("COMMIT"); return value }
        catch { try? execute("ROLLBACK"); throw error }
    }
    private func string(_ row: OpaquePointer, _ index: Int32) throws -> String {
        guard let text = sqlite3_column_text(row, index) else { throw fail() }; return String(cString: text)
    }
    private func hash(_ data: Data) -> String { WorkspaceProtocol.encode(Data(SHA256.hash(data: data))) }
    private func key(_ connection: Connection, create: Bool = false) throws -> SymmetricKey {
        if let testKeys { return try testKeys.key() }
        try WorkspaceProtocol.identifier(connection.id)
        let account = "workspace-wrap-v2:\(connection.id)"
        if let data = try Keychain.read(account) {
            guard data.count == 32 else { throw fail() }; return SymmetricKey(data: data)
        }
        guard create else { throw ZerusError(.unavailable, "This encrypted workspace key is unavailable. Obtain a new phone invitation.") }
        let key = SymmetricKey(size: .bits256)
        try Keychain.write(account, key.withUnsafeBytes { Data($0) }); return key
    }
    private func seal(_ connection: Connection, _ state: WorkspaceState) throws -> Data {
        guard let data = try AES.GCM.seal(JSON.object(state.json).data(), using: key(connection), authenticating: Data(connection.id.utf8)).combined else { throw fail() }; return data
    }
    func create(_ connection: Connection, state: WorkspaceState) throws { try locked {
        guard connection.protocolVersion == 2 else { throw fail() }
        _ = try key(connection, create: true)
        try execute("INSERT INTO workspace_state(connection,token_hash,record) VALUES(?,?,?)", [.text(connection.id), .text(hash(Data(connection.token.utf8))), .blob(seal(connection, state))])
    } }
    func load(_ connection: Connection) throws -> WorkspaceState { try locked {
        guard connection.protocolVersion == 2 else { throw fail() }
        return try statement("SELECT token_hash,record FROM workspace_state WHERE connection=?", [.text(connection.id)]) { row in
            guard sqlite3_step(row) == SQLITE_ROW, try string(row, 0) == hash(Data(connection.token.utf8)), let bytes = sqlite3_column_blob(row, 1) else { throw fail() }
            let data = Data(bytes: bytes, count: Int(sqlite3_column_bytes(row, 1)))
            let clear = try AES.GCM.open(AES.GCM.SealedBox(combined: data), using: key(connection), authenticating: Data(connection.id.utf8))
            guard let json = try JSON.parse(clear).object else { throw fail() }; return try WorkspaceState.parse(json)
        }
    } }
    func update(_ connection: Connection, expected: WorkspaceState, next: WorkspaceState, changes: [WorkspaceMembershipChange] = []) throws { try locked { try transaction {
        guard try load(connection).json == expected.json, expected.identity.device == next.identity.device, expected.candidate == next.candidate else {
            throw ZerusError.mismatch("Encrypted workspace changed during this check. Refresh again.")
        }
        try execute("UPDATE workspace_state SET record=? WHERE connection=?", [.blob(seal(connection, next)), .text(connection.id)])
        if let membership = next.membership {
            if expected.membership == nil { try execute("INSERT INTO membership(connection,revision,head,change) VALUES(?,?,?,NULL)", [.text(connection.id), .integer(next.pinnedRevision), .text(next.pinnedHead)]) }
            for change in changes { try execute("INSERT INTO membership(connection,revision,head,change) VALUES(?,?,?,?)", [.text(connection.id), .integer(change.revision), .text(change.head()), .text(JSON.object(change.json).text())]) }
            guard try head(connection, revision: membership.revision) == membership.head else { throw fail() }
        }
    } } }
    func head(_ connection: Connection, revision: Int64) throws -> String? { try locked {
        try statement("SELECT head FROM membership WHERE connection=? AND revision=?", [.text(connection.id), .integer(revision)]) { row in
            let result = sqlite3_step(row)
            if result == SQLITE_DONE { return nil }; guard result == SQLITE_ROW else { throw fail() }; return try string(row, 0)
        }
    } }
    func reserve(_ connection: Connection, id: String, plainHash: String, envelope: WorkspaceEnvelope) throws -> WorkspaceEnvelope { try locked { try transaction {
        let state = try load(connection), membership = try state.active()
        _ = try membership.senderAtKnownHead(envelope, knownHead: head(connection, revision: envelope.header.membershipRevision))
        let retained: WorkspaceEnvelope? = try statement("SELECT plain_hash,envelope FROM outgoing WHERE connection=? AND request_id=?", [.text(connection.id), .text(id)]) { row in
            if sqlite3_step(row) == SQLITE_DONE { return nil }
            guard try string(row, 0) == plainHash, sqlite3_column_type(row, 1) != SQLITE_NULL,
                  let json = try JSON.parse(string(row, 1)).object else { throw ZerusError.mismatch("This request identity is already retained. Check its original receipt.") }
            return try WorkspaceEnvelope.parse(json)
        }
        if let retained { return retained }
        let text = try JSON.object(envelope.json).text(), header = try JSON.object(envelope.header.json).text()
        let bytes = Int64(text.utf8.count + header.utf8.count + 256)
        let used = try statement("SELECT bytes FROM outgoing_quota WHERE id=1") { row in guard sqlite3_step(row) == SQLITE_ROW else { throw fail() }; return sqlite3_column_int64(row, 0) }
        guard used + bytes <= 256 * 1024 * 1024 else { throw ZerusError(.limit, "Encrypted outgoing storage is full. Check outstanding deliveries.") }
        try execute("INSERT INTO outgoing(connection,request_id,plain_hash,header,envelope) VALUES(?,?,?,?,?)", [.text(connection.id), .text(id), .text(plainHash), .text(header), .text(text)])
        try execute("UPDATE outgoing_quota SET bytes=bytes+? WHERE id=1", [.integer(bytes)])
        return envelope
    } } }
    func recordedRequest(_ connection: Connection, id: String) throws -> WorkspaceHeader? { try locked {
        try statement("SELECT header FROM outgoing WHERE connection=? AND request_id=?", [.text(connection.id), .text(id)]) { row in
            if sqlite3_step(row) == SQLITE_DONE { return nil }
            guard let json = try JSON.parse(string(row, 0)).object else { throw ZerusError.mismatch("The original encrypted request is unavailable. Delivery cannot be confirmed.") }
            return try WorkspaceHeader.parse(json)
        }
    } }
    func request(_ connection: Connection, id: String) throws -> WorkspaceHeader {
        guard let recorded = try recordedRequest(connection, id: id) else { throw ZerusError.mismatch("The original encrypted request is unavailable. Delivery cannot be confirmed.") }
        return recorded
    }
    func openEnvelope(_ connection: Connection, envelope: WorkspaceEnvelope, expected: WorkspaceHeader) throws -> Data { try locked {
        let state = try load(connection), membership = try state.active()
        let sender = try membership.senderAtKnownHead(envelope, knownHead: head(connection, revision: envelope.header.membershipRevision))
        guard sender.role == "computer" else { throw ZerusError.mismatch("Unrecognized executing computer.") }
        return try envelope.open(identity: state.identity, sender: sender, expected: expected)
    } }
    func settle(_ connection: Connection, id: String) throws { try locked { try transaction {
        let count = try statement("SELECT COALESCE(LENGTH(envelope),0) FROM outgoing WHERE connection=? AND request_id=?", [.text(connection.id), .text(id)]) { row in sqlite3_step(row) == SQLITE_ROW ? sqlite3_column_int64(row, 0) : 0 }
        try execute("UPDATE outgoing SET envelope=NULL WHERE connection=? AND request_id=?", [.text(connection.id), .text(id)])
        try execute("UPDATE outgoing_quota SET bytes=bytes-? WHERE id=1 AND bytes>=?", [.integer(count), .integer(count)])
    } } }
    public func forget(_ connection: Connection) throws { try locked {
        try WorkspaceProtocol.identifier(connection.id)
        if testKeys == nil { try Keychain.delete("workspace-wrap-v2:\(connection.id)") }
        try transaction {
            let bytes = try statement("SELECT COALESCE(SUM(LENGTH(envelope)),0) FROM outgoing WHERE connection=?", [.text(connection.id)]) { row in
                guard sqlite3_step(row) == SQLITE_ROW else { throw fail() }; return sqlite3_column_int64(row, 0)
            }
            try execute("UPDATE outgoing SET envelope=NULL WHERE connection=?", [.text(connection.id)])
            try execute("UPDATE outgoing_quota SET bytes=bytes-? WHERE id=1 AND bytes>=?", [.integer(bytes), .integer(bytes)])
            try execute("DELETE FROM workspace_state WHERE connection=?", [.text(connection.id)])
        }
    } }
}
