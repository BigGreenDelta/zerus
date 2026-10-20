import CryptoKit
import Foundation
import Synchronization
import XCTest
import ZerusReplica
@testable import ZerusKit

/// Ported from Android TaskSyncTest: the phone's replica exchanged with a computer through the relay.
final class TaskSyncTests: XCTestCase {
    private let key = ProjectKey(connectionId: "workspace-a", swarmId: "swarm-1", id: "project-1")
    private let connection = Connection(id: "workspace-a", label: "Workspace", url: "https://gateway.example", token: "synthetic")
    private let digest = String(repeating: "ab", count: 32)

    private func replicas() throws -> TaskReplicas { try TaskReplicas(root: URL(fileURLWithPath: FakeComputer.folder())) }
    private func peer(_ computer: FakeComputer) throws -> TaskPeer { TaskPeer(computerId: "computer-1", digest: try computer.store.liveDigest()) }
    private func sync(_ replicas: TaskReplicas, timeout: Duration = .seconds(300),
                      clock: @escaping @Sendable () -> Date = Date.init) -> TaskSync {
        TaskSync(client: StubProtocol.client(), replicas: replicas, exchangeTimeout: timeout, clock: clock)
    }
    private func sha256(_ value: String) -> String { SHA256.hash(data: Data(value.utf8)).map { String(format: "%02x", $0) }.joined() }
    /// Text that does not compress.
    private func noise(_ seed: String, _ length: Int) -> String {
        var out = ""
        while out.count < length { out += sha256("\(seed)-\(out.count)") }
        return String(out.prefix(length))
    }

    func testAPhonePullsAComputersTasksThroughTheRelay() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        for text in ["Add rate limiting", "Measure first", "Document the limits"] { try computer.store.filed(text) }
        let failures = await sync(replicas).sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(failures, [:])
        let phone = replicas.store(key, name: "orchard")
        XCTAssertEqual(try phone.liveDigest(), try computer.store.liveDigest())
        XCTAssertEqual(try phone.list().tasks.count, 3)
        XCTAssertFalse(computer.envelopes.isEmpty)
        for envelope in computer.envelopes {
            XCTAssertEqual(envelope.string("operation"), "task_exchange")
            XCTAssertEqual(envelope.string("session"), "")
            XCTAssertEqual(envelope.string("computer_id"), "computer-1")
            XCTAssertEqual(envelope.object("payload")?.string("project"), "project-1")
        }
        XCTAssertTrue(computer.state.withLock { $0.paths }.allSatisfy { $0 == "/v1/requests" })
    }

    func testThePhonesEditsReachTheComputer() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        let task = try computer.store.filed("Add rate limiting")
        let sync = sync(replicas)
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        _ = try replicas.store(key, name: "orchard").comment(reference: task.uuid, text: "From the phone")
        let failures = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(failures, [:])
        XCTAssertEqual(try computer.store.get(reference: task.uuid).log.last { $0.kind == "comment" }?.text, "From the phone")
    }

    func testAMatchingDigestSendsNothing() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        let sync = sync(replicas)
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        let sent = computer.envelopes.count
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(computer.envelopes.count, sent)
    }

    func testAStaleHeartbeatDigestSendsNothingAfterAnExchange() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        let sync = sync(replicas)
        let heartbeat = try peer(computer)
        let first = await sync.sync(connection, key, name: "orchard", peers: [heartbeat])
        XCTAssertEqual(first, [:])
        let phone = replicas.store(key, name: "orchard")
        _ = try phone.comment(reference: try phone.list().tasks[0].uuid, text: "From the phone")
        _ = await sync.sync(connection, key, name: "orchard", peers: [heartbeat])
        let sent = computer.envelopes.count
        // The heartbeat still names the digest from before the exchange; nothing changed since.
        _ = await sync.sync(connection, key, name: "orchard", peers: [heartbeat])
        XCTAssertEqual(computer.envelopes.count, sent)
    }

    func testAnUnconvergedProjectWaitsUntilADigestChanges() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        let task = try computer.store.filed("Add rate limiting")
        let sync = sync(replicas)
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        // More than one request can carry: the phone keeps it, and the digests stay apart.
        let phone = replicas.store(key, name: "orchard")
        for n in 0..<10 { _ = try phone.comment(reference: task.uuid, text: noise("comment-\(n)", 60_000)) }
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertNotEqual(try computer.store.liveDigest(), try phone.liveDigest())
        let sent = computer.envelopes.count
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(computer.envelopes.count, sent)
        // Something new on the computer: the phone exchanges again.
        try computer.store.filed("Measure first")
        _ = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertGreaterThan(computer.envelopes.count, sent)
        XCTAssertEqual(try phone.list().tasks.count, 2)
    }

    func testAReplicaThatCannotBeReadFailsOnlyItsExchange() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        // A damaged project document: the phone cannot read its own digest.
        let project = replicas.folder(key).appendingPathComponent(sha256(key.id))
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        try Data("damaged".utf8).write(to: project.appendingPathComponent("project.loro"))
        let failures = await sync(replicas).sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(Set(failures.keys), ["computer-1"])
        XCTAssertTrue(computer.envelopes.isEmpty)
    }

    func testAReplicaRefusalReadsAsItsMessage() {
        // The replica's own wording reaches the notice, as on Android.
        XCTAssertEqual(TaskSyncError.reason(ReplicaError.Refused(message: "a task cannot go from To do to Done")),
                       "a task cannot go from To do to Done")
        XCTAssertEqual(TaskSyncError.reason(ReplicaError.Failed(message: "the project's tasks are busy; try again")),
                       "the project's tasks are busy; try again")
    }

    func testASlowExchangeStopsAtItsDeadline() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        // The computer does not claim the exchange; the phone stops waiting at its deadline.
        computer.update { $0.queued = ["task_exchange"] }
        let started = Date()
        let failures = await sync(replicas, timeout: .milliseconds(300)).sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertLessThan(Date().timeIntervalSince(started), 5)
        XCTAssertEqual(Set(failures.keys), ["computer-1"])
        XCTAssertTrue(failures["computer-1"]?.contains("took longer") == true, failures["computer-1"] ?? "")
    }

    func testAFailedExchangeWaitsAMinute() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        let now = Mutex(Date(timeIntervalSince1970: 0))
        let sync = sync(replicas, clock: { now.withLock { $0 } })
        computer.update { $0.refusal = "the project's tasks are busy; try again" }
        let failed = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(failed, ["computer-1": "the project's tasks are busy; try again"])
        XCTAssertEqual(computer.envelopes.count, 1)
        computer.update { $0.refusal = nil }
        now.withLock { $0 = Date(timeIntervalSince1970: 59) }
        let waiting = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(waiting, [:])
        XCTAssertEqual(computer.envelopes.count, 1)
        now.withLock { $0 = Date(timeIntervalSince1970: 61) }
        let again = await sync.sync(connection, key, name: "orchard", peers: [try peer(computer)])
        XCTAssertEqual(again, [:])
        XCTAssertEqual(try replicas.store(key, name: "orchard").liveDigest(), try computer.store.liveDigest())
    }

    func testReplicasAreSeparatePerConnectionAndSwarm() async throws {
        let replicas = try replicas()
        var elsewhere = key
        elsewhere.connectionId = "workspace-b"
        var otherSwarm = key
        otherSwarm.swarmId = "swarm-2"
        XCTAssertNotEqual(replicas.folder(key), replicas.folder(elsewhere))
        XCTAssertNotEqual(replicas.folder(key), replicas.folder(otherSwarm))
        XCTAssertEqual(replicas.folder(key).deletingLastPathComponent().lastPathComponent, sha256("workspace-a"))
        try replicas.store(key, name: "orchard").filed("Add rate limiting")
        try replicas.store(elsewhere, name: "orchard").filed("Measure first")
        try await replicas.purge(connectionId: "workspace-a")
        XCTAssertFalse(FileManager.default.fileExists(atPath: replicas.folder(key).path))
        XCTAssertEqual(try replicas.store(elsewhere, name: "orchard").list().tasks.map(\.text), ["Measure first"])
    }

    func testPurgeWaitsForARunningExchange() async throws {
        let computer = FakeComputer(), replicas = try replicas()
        try computer.store.filed("Add rate limiting")
        let sync = sync(replicas)
        computer.update { $0.delay = .milliseconds(400) }
        let peers = [try peer(computer)]
        let connection = connection, key = key
        let running = Task { await sync.sync(connection, key, name: "orchard", peers: peers) }
        try await Task.sleep(for: .milliseconds(150))
        await sync.purge(connectionId: key.connectionId)
        // The exchange ended before the purge: nothing writes the replica again.
        let ended = await running.value
        XCTAssertEqual(ended, [:])
        XCTAssertFalse(FileManager.default.fileExists(atPath: replicas.folder(key).path))
    }

    func testOnlyComputersThatCanExchangeNowAreKept() {
        let digests = [key: Dictionary(uniqueKeysWithValues: ["a", "b", "c", "d"].map { ($0, digest) })]
        let machines = [Machine(connectionId: "workspace-a", id: "a", name: "A", online: true),
                        Machine(connectionId: "workspace-a", id: "b", name: "B", online: false),
                        Machine(connectionId: "workspace-a", id: "c", name: "C", online: true, lastKnown: true),
                        Machine(connectionId: "workspace-a", id: "d", name: "D", online: true)]
        let exchange: Set<String> = ["task_exchange"]
        let operations = [MachineKey(connectionId: "workspace-a", computerId: "a"): exchange,
                          MachineKey(connectionId: "workspace-a", computerId: "b"): exchange,
                          MachineKey(connectionId: "workspace-a", computerId: "c"): exchange,
                          MachineKey(connectionId: "workspace-a", computerId: "d"): ["inspect"]]
        let relay = ["workspace-a": exchange]
        XCTAssertEqual(TaskSyncPlan.peers(digests, relayOperations: relay, machineOperations: operations, machines: machines),
                       [key: [TaskPeer(computerId: "a", digest: digest)]])
        XCTAssertEqual(TaskSyncPlan.peers(digests, relayOperations: ["workspace-a": ["inspect"]], machineOperations: operations, machines: machines), [:])
        XCTAssertEqual(TaskSyncPlan.peers(digests, relayOperations: [:], machineOperations: operations, machines: machines), [:])
    }
}
