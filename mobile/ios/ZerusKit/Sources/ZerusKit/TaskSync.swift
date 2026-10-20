import CryptoKit
import Foundation
import ZerusReplica

/// The phone's task replicas (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md,
/// "Phone"), ported from Android `TaskSync.kt`: one folder per relay connection and swarm below
/// `root`, so two relays never share documents. The documents are not encrypted further; `root` is
/// excluded from backup and protected until first unlock.
public final class TaskReplicas: Sendable {
    public let root: URL
    /// Store calls may wait seconds for the project's file lock: they run here, never on the
    /// main actor or the cooperative pool.
    private let queue = DispatchQueue(label: "app.zerus.task-replicas", qos: .utility, attributes: .concurrent)

    public init(root: URL) throws {
        self.root = root
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        #if os(iOS)
        try FileManager.default.setAttributes([.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication], ofItemAtPath: root.path)
        #endif
        #if !os(Linux)
        var excluded = URLResourceValues()
        excluded.isExcludedFromBackup = true
        var folder = root
        try folder.setResourceValues(excluded)
        #endif
    }

    /// `Application Support/task-replicas-v1`.
    public static func live() throws -> TaskReplicas {
        let support = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        return try TaskReplicas(root: support.appendingPathComponent("task-replicas-v1", isDirectory: true))
    }

    public func run<T: Sendable>(_ body: @escaping @Sendable () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { continuation in
            queue.async { continuation.resume(with: Result { try body() }) }
        }
    }

    /// The project's tasks; each call opens the project on its own.
    public func store(_ key: ProjectKey, name: String) -> TaskStore {
        TaskStore(root: folder(key).path, project: key.id, projectName: name)
    }

    public func folder(_ key: ProjectKey) -> URL {
        root.appendingPathComponent(Self.hex(key.connectionId), isDirectory: true).appendingPathComponent(Self.hex(key.swarmId), isDirectory: true)
    }

    /// Drops every replica the connection holds.
    public func purge(connectionId: String) async throws {
        let folder = root.appendingPathComponent(Self.hex(connectionId), isDirectory: true)
        try await run {
            guard FileManager.default.fileExists(atPath: folder.path) else { return }
            try FileManager.default.removeItem(at: folder)
        }
    }

    static func hex(_ value: String) -> String { SHA256.hash(data: Data(value.utf8)).map { String(format: "%02x", $0) }.joined() }
}

/// How a Send now or Cancel ended on the session's computer.
public enum TaskOutcome: Equatable, Sendable {
    case done
    /// Nothing happened: hgs's reason, or why the phone's edits could not reach that computer.
    case refused(String)
    /// It may have happened: the next exchange shows the task's stage.
    case uncertain(String)
}

/// Where the next Done page starts.
public struct DoneCursor: Equatable, Sendable {
    public let at: Int64
    public let uuid: String
    public init(at: Int64, uuid: String) {
        self.at = at
        self.uuid = uuid
    }
}

public struct DoneRow: Equatable, Sendable {
    public let uuid: String
    public let id: String
    public let text: String
    public let session: SessionRef?
    public let acceptedAt: Int64
}

/// One page of a computer's Done list, newest first; `recent` counts the last 7 days.
public struct DonePage: Equatable, Sendable {
    public let rows: [DoneRow]
    public let next: DoneCursor?
    public let total: Int
    public let recent: Int
    public let damaged: Int
}

/// A computer of a project that can exchange its tasks now, and the digest its heartbeat names.
public struct TaskPeer: Hashable, Sendable {
    public let computerId: String
    public let digest: String
    public init(computerId: String, digest: String) {
        self.computerId = computerId
        self.digest = digest
    }
}

public enum TaskSyncPlan {
    public static let operation = "task_exchange"

    /// Each catalogued project's computers that are online, not last known, and offer the exchange, as their relay does.
    public static func peers(_ digests: [ProjectKey: [String: String]], relayOperations: [String: Set<String>],
                             machineOperations: [MachineKey: Set<String>], machines: [Machine]) -> [ProjectKey: [TaskPeer]] {
        var plan: [ProjectKey: [TaskPeer]] = [:]
        for (key, byComputer) in digests where relayOperations[key.connectionId]?.contains(operation) == true {
            let peers = byComputer.filter { computer, _ in
                machineOperations[MachineKey(connectionId: key.connectionId, computerId: computer)]?.contains(operation) == true &&
                    machines.contains { $0.connectionId == key.connectionId && $0.id == computer && $0.online && !$0.lastKnown }
            }.map { TaskPeer(computerId: $0.key, digest: $0.value) }.sorted { $0.computerId < $1.computerId }
            if !peers.isEmpty { plan[key] = peers }
        }
        return plan
    }
}

/// Why a task exchange or action did not finish.
public struct TaskSyncError: Error, Equatable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }

    /// The reason a person reads: the relay's or hgs's message, or the replica's.
    public static func reason(_ error: Error) -> String {
        switch error {
        case let error as TaskSyncError: error.message
        case let error as RelayError: error.message
        case let error as ZerusError: error.message
        // UniFFI's own description names the case; the person reads the replica's message.
        case let error as ReplicaError:
            switch error {
            case .Refused(let message), .Failed(let message): message
            }
        case let error as LocalizedError: error.errorDescription ?? "\(error)"
        default: "\(error)"
        }
    }
}

/// Exchanges a project's tasks with its computers through the relay: one project at a time
/// (a lock per project), each computer whose digest is not the phone's, until their copies
/// meet. The Rust exchange decides every request; this only carries its JSON.
public actor TaskSync {
    public static let retry: TimeInterval = 60
    public static let exchangePolls = 80
    public static let actionPolls = 150
    public static let pollDelay: Duration = .milliseconds(400)

    private struct PeerKey: Hashable { let project: ProjectKey; let computerId: String }
    /// After an exchange: the phone's digest, the heartbeat's it answered, and the computer's after it.
    private struct Quiet { let mine: String; let heartbeat: String; let theirs: String }

    let client: RelayClient
    let replicas: TaskReplicas
    private let exchangeTimeout: Duration
    private let clock: @Sendable () -> Date
    private var waiting: [PeerKey: Date] = [:]
    private var quiet: [PeerKey: Quiet] = [:]
    private var locked: Set<ProjectKey> = []
    private var queued: [ProjectKey: [CheckedContinuation<Void, Never>]] = [:]
    /// Tasks whose Send now may have happened, with their project and computer.
    private var sends: [String: PeerKey] = [:]
    private var known: Set<ProjectKey> = []

    public init(client: RelayClient, replicas: TaskReplicas, exchangeTimeout: Duration = .seconds(300),
                clock: @escaping @Sendable () -> Date = Date.init) {
        self.client = client
        self.replicas = replicas
        self.exchangeTimeout = exchangeTimeout
        self.clock = clock
    }

    /// Exchanges `key`'s tasks with each of `peers` whose digest is not the phone's; the failures by computer ID.
    public func sync(_ connection: Connection, _ key: ProjectKey, name: String, peers: [TaskPeer]) async -> [String: String] {
        await lock(key)
        defer { unlock(key) }
        var failures: [String: String] = [:]
        let store = replicas.store(key, name: name)
        for peer in peers.sorted(by: { $0.computerId < $1.computerId }) {
            if Task.isCancelled { break }
            let wait = PeerKey(project: key, computerId: peer.computerId)
            if clock() < waiting[wait, default: .distantPast] { continue }
            do {
                let mine = try await replicas.run { try store.liveDigest() }
                if mine == peer.digest { continue }
                if let known = quiet[wait], known.mine == mine, [known.heartbeat, known.theirs].contains(peer.digest) { continue }
                let theirs = try await bounded { try await self.exchange(connection, store, computerId: peer.computerId) }
                quiet[wait] = Quiet(mine: try await replicas.run { try store.liveDigest() }, heartbeat: peer.digest, theirs: theirs)
                waiting[wait] = nil
            } catch {
                if Task.isCancelled { break }
                waiting[wait] = clock().addingTimeInterval(Self.retry)
                let reason = TaskSyncError.reason(error)
                failures[peer.computerId] = reason.isEmpty ? "The tasks could not be exchanged." : reason
            }
        }
        return failures
    }

    /// Drops the connection's replicas once its running exchanges end, so none writes them again.
    public func purge(connectionId: String) async {
        let keys = known.filter { $0.connectionId == connectionId }.sorted { $0.key < $1.key }
        for key in keys { await lock(key) }
        defer { keys.forEach(unlock) }
        try? await replicas.purge(connectionId: connectionId)
    }

    // MARK: The project's lock

    func lock(_ key: ProjectKey) async {
        known.insert(key)
        guard locked.contains(key) else { locked.insert(key); return }
        await withCheckedContinuation { queued[key, default: []].append($0) }
    }

    func unlock(_ key: ProjectKey) {
        if var waiters = queued[key], !waiters.isEmpty {
            let next = waiters.removeFirst()
            queued[key] = waiters
            next.resume()
        } else {
            locked.remove(key)
        }
    }

    /// Runs `body` under the project's lock with its store.
    func project<T: Sendable>(_ key: ProjectKey, name: String, _ body: (TaskStore) async throws -> T) async rethrows -> T {
        await lock(key)
        defer { unlock(key) }
        return try await body(replicas.store(key, name: name))
    }

    // MARK: Actions

    /// One exchange with this computer now, whatever the digests: the phone's edits reach it, and `fetch` brings tasks it does not hold.
    public func exchangeNow(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, fetch: [String] = []) async throws {
        try await project(key, name: name) { store in
            _ = try await bounded { try await self.exchange(connection, store, computerId: computerId, fetch: fetch) }
        }
    }

    /// Send now: the phone's edits reach the session's computer first, then it sends, then its copy comes back.
    public func send(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, task: String,
                     session: String, computerName: String? = nil) async -> TaskOutcome {
        let payload: JSONObject = ["schema": 1, "project": .string(key.id), "task": .string(task), "session": .string(session)]
        let outcome = await act(connection, key, name: name, computerId: computerId, computerName: computerName ?? computerId,
                                operation: "task_send", payload: payload)
        if case .uncertain = outcome { sends[task] = PeerKey(project: key, computerId: computerId) }
        return outcome
    }

    /// A Send now that may have happened holds the task until an exchange with that computer shows its stage.
    public func held(_ task: String) -> Bool { sends[task] != nil }
    public func heldTasks() -> Set<String> { Set(sends.keys) }

    /// Exchanges once with each computer of this connection that holds a task back; one that completes releases them.
    public func settleSends(_ connection: Connection, names: @Sendable (ProjectKey) -> String) async {
        for pending in Set(sends.values.filter { $0.project.connectionId == connection.id }) {
            do {
                try await exchangeNow(connection, pending.project, name: names(pending.project), computerId: pending.computerId)
                sends = sends.filter { $0.value != pending }
            } catch {
                if Task.isCancelled { return }
                // The next sync tries again.
            }
        }
    }

    public func cancel(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, task: String,
                       reason: String, computerName: String? = nil) async -> TaskOutcome {
        var payload: JSONObject = ["schema": 1, "project": .string(key.id), "task": .string(task)]
        if !reason.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { payload = payload.putting("reason", .string(reason)) }
        return await act(connection, key, name: name, computerId: computerId, computerName: computerName ?? computerId,
                         operation: "task_cancel", payload: payload)
    }

    /// One page of that computer's Done list; with `session`, of the tasks sent to it.
    public func donePage(_ connection: Connection, _ key: ProjectKey, computerId: String, before: DoneCursor?,
                         limit: Int = 50, session: SessionRef? = nil) async throws -> DonePage {
        var payload: JSONObject = ["schema": 1, "project": .string(key.id), "limit": .int(Int64(limit))]
        if let before { payload = payload.putting("before", ["at": .int(before.at), "uuid": .string(before.uuid)]) }
        if let session { payload = payload.putting("session", ["machine": .string(session.machine), "session": .string(session.session)]) }
        let receipt = try await request(connection, computerId: computerId, operation: "task_done", payload: payload, maxPolls: Self.exchangePolls)
        guard receipt.string("state") == "completed" else {
            let error = receipt.string("error")
            throw TaskSyncError(error.isEmpty ? "The computer did not list its Done tasks." : error)
        }
        guard let result = receipt.object("result") else { throw TaskSyncError("The computer's Done list has no result.") }
        let rows = result.objects("tasks").map { row in
            DoneRow(uuid: row.string("uuid"), id: row.string("id"), text: row.string("text"),
                    session: row.object("session").map { SessionRef(machine: $0.string("machine"), session: $0.string("session")) },
                    acceptedAt: Int64(row.int("accepted_at")))
        }
        let next = result.object("next").map { DoneCursor(at: Int64($0.int("at")), uuid: $0.string("uuid")) }
        return DonePage(rows: rows, next: next, total: result.int("total"), recent: result.int("recent"), damaged: result.int("damaged"))
    }

    /// The exchange, then `operation` on that computer, then the exchange that shows its result.
    private func act(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, computerName: String,
                     operation: String, payload: JSONObject) async -> TaskOutcome {
        await project(key, name: name) { store in
            do { _ = try await bounded { try await self.exchange(connection, store, computerId: computerId) } }
            catch { return .refused("The tasks could not reach \(computerName): \(TaskSyncError.reason(error))") }
            let outcome: TaskOutcome
            do {
                let receipt = try await request(connection, computerId: computerId, operation: operation, payload: payload,
                                                maxPolls: Self.actionPolls)
                let reason = receipt.string("error")
                switch receipt.string("state") {
                case "completed": outcome = .done
                case "failed": outcome = .refused(reason.isEmpty ? "The computer refused it." : reason)
                case "uncertain": outcome = .uncertain(reason.isEmpty ? "The computer could not confirm it." : reason)
                default: outcome = .uncertain("The computer has not answered yet.")
                }
            } catch let error as RelayError where (400..<500).contains(error.status) && error.status != 408 {
                // The relay refused the request itself: nothing was queued.
                outcome = .refused(error.message.isEmpty ? "The relay refused it." : error.message)
            } catch {
                // The request may be queued although its answer was lost.
                outcome = .uncertain("The relay did not confirm it: \(TaskSyncError.reason(error))")
            }
            // Its copy comes back now; when that fails the next sync brings it.
            _ = try? await bounded { try await self.exchange(connection, store, computerId: computerId) }
            return outcome
        }
    }

    // MARK: Exchange

    /// Besides its request budget, an exchange ends after its time.
    func bounded<T: Sendable>(_ body: @escaping @Sendable () async throws -> T) async throws -> T {
        let seconds = exchangeTimeout.components.seconds
        return try await withThrowingTaskGroup(of: T?.self) { group in
            group.addTask { try await body() }
            group.addTask { [exchangeTimeout] in
                try await Task.sleep(for: exchangeTimeout)
                return nil
            }
            defer { group.cancelAll() }
            guard let first = try await group.next(), let value = first else {
                throw TaskSyncError("The task exchange took longer than \(seconds) seconds.")
            }
            return value
        }
    }

    /// One whole exchange with the computer; its live digest after the last answer.
    nonisolated func exchange(_ connection: Connection, _ store: TaskStore, computerId: String, fetch: [String] = []) async throws -> String {
        let exchange = try await replicas.run { fetch.isEmpty ? store.exchange() : store.exchangeFetching(tasks: fetch) }
        var digest = ""
        while true {
            guard let request = try await replicas.run({ try exchange.nextRequest() }) else { return digest }
            guard let payload = try JSON.parse(request).object else { throw TaskSyncError("The task exchange made an invalid request.") }
            let receipt = try await self.request(connection, computerId: computerId, operation: TaskSyncPlan.operation, payload: payload,
                                                 maxPolls: Self.exchangePolls)
            guard receipt.string("state") == "completed" else {
                let error = receipt.string("error")
                throw TaskSyncError(error.isEmpty ? "The computer did not answer the task exchange." : error)
            }
            guard let result = receipt.object("result") else { throw TaskSyncError("The computer's answer has no result.") }
            let answer = JSON.object(result).compact
            digest = try await replicas.run { try exchange.accept(answer: answer) }.digest
        }
    }

    /// One computer-scoped request through the relay, and its receipt once it settles or the polls run out.
    nonisolated func request(_ connection: Connection, computerId: String, operation: String, payload: JSONObject,
                             maxPolls: Int) async throws -> JSONObject {
        let posted = try await client.post(connection, computerId: computerId, operation: operation, payload: payload,
                                           id: UUID().uuidString.lowercased())
        return try await client.waitForReceipt(connection, initial: posted, maxPolls: maxPolls, pollDelay: Self.pollDelay)
    }
}
