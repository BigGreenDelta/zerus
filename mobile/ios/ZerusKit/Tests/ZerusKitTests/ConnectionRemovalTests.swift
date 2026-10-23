import CryptoKit
import Foundation
import SQLite3
import XCTest
@testable import ZerusKit

final class ConnectionRemovalTests: XCTestCase {
    private enum Fault: Sendable { case key, index }
    private struct SyntheticFailure: Error {}
    private actor Index {
        let connection: Connection
        let other: Connection
        let journal: SQLiteWorkspaceJournal
        var durable: [Connection]
        var visible: [Connection]
        var fault: Fault?
        var keyPresent = true
        init(connection: Connection, journal: SQLiteWorkspaceJournal, fault: Fault?) {
            self.connection = connection; self.journal = journal; self.fault = fault
            other = Connection(id: "other", label: "Synthetic", url: "https://relay.example", token: "synthetic", protocolVersion: 2)
            durable = [connection, other]; visible = durable
        }
        func erase(_ connection: Connection) throws {
            guard connection == self.connection else { throw SyntheticFailure() }
            if fault == .key { throw SyntheticFailure() }
            keyPresent = false // The test key is synthetic; no Keychain operation is called.
            try journal.forget(connection)
        }
        func save(_ next: [Connection]) throws {
            if fault == .index { throw SyntheticFailure() }
            durable = next
        }
        func publish(_ next: [Connection]) { visible = next }
        func reloadForRetry() { fault = nil; visible = durable }
        func snapshot() -> (durable: [Connection], visible: [Connection], keyPresent: Bool) { (durable, visible, keyPresent) }
    }

    private func fixture(_ fault: Fault? = nil) throws -> (URL, SQLiteWorkspaceJournal, Connection, Index) {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let journal = SQLiteWorkspaceJournal(directory: directory, testKeys: StaticKeyProvider(SymmetricKey(data: Data(repeating: 9, count: 32))))
        let connection = Connection(id: "protected", label: "Synthetic", url: "https://relay.example", token: "synthetic", protocolVersion: 2)
        let f = try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object)
        let invitation = try XCTUnwrap(f["invitation"]?.object)
        let state = try WorkspaceState(identity: WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object)),
            candidate: WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object)), issuer: WorkspaceDevice.parse(XCTUnwrap(invitation["issuer"]?.object)),
            pinnedRevision: WorkspaceProtocol.number(invitation, "membership_revision"), pinnedHead: invitation.string("membership_head"))
        try journal.create(connection, state: state)
        return (directory, journal, connection, Index(connection: connection, journal: journal, fault: fault))
    }
    private func forget(_ index: Index, _ connection: Connection) async throws {
        let current = await index.snapshot().visible
        let next = try await ConnectionRemoval.forget(connection, from: current,
            eraseProtected: { try await index.erase($0) }, saveIndex: { try await index.save($0) })
        await index.publish(next)
    }
    private func assertRetryable(_ index: Index, _ connection: Connection, keyPresent: Bool, file: StaticString = #filePath, line: UInt = #line) async {
        let snapshot = await index.snapshot()
        XCTAssertEqual(snapshot.visible, snapshot.durable, file: file, line: line)
        XCTAssertTrue(snapshot.durable.contains(connection), file: file, line: line)
        XCTAssertEqual(snapshot.keyPresent, keyPresent, file: file, line: line)
    }
    private func retry(_ index: Index, _ connection: Connection, _ journal: SQLiteWorkspaceJournal) async throws {
        await index.reloadForRetry()
        try await forget(index, connection)
        let snapshot = await index.snapshot()
        XCTAssertEqual(snapshot.visible, snapshot.durable)
        XCTAssertEqual(snapshot.visible.map(\.id), ["other"])
        XCTAssertFalse(snapshot.keyPresent)
        XCTAssertThrowsError(try journal.load(connection))
    }

    func testKeyDeletionFailureKeepsActionableConnectionAcrossReload() async throws {
        let (directory, journal, connection, index) = try fixture(.key)
        defer { try? FileManager.default.removeItem(at: directory) }
        do { try await forget(index, connection); XCTFail("Expected synthetic key deletion failure") } catch {}
        await assertRetryable(index, connection, keyPresent: true)
        XCTAssertNoThrow(try journal.load(connection))
        try await retry(index, connection, journal)
    }
    func testSQLFailureAfterKeyDeletionKeepsActionableConnectionAndRetriesTransaction() async throws {
        let (directory, journal, connection, index) = try fixture()
        defer { try? FileManager.default.removeItem(at: directory) }
        var database: OpaquePointer?
        XCTAssertEqual(sqlite3_open(directory.appendingPathComponent("journal.sqlite3").path, &database), SQLITE_OK)
        defer { sqlite3_close(database) }
        XCTAssertEqual(sqlite3_exec(database, "CREATE TRIGGER fail_forget BEFORE DELETE ON workspace_state BEGIN SELECT RAISE(ABORT,'synthetic'); END", nil, nil, nil), SQLITE_OK)
        do { try await forget(index, connection); XCTFail("Expected SQL transaction failure") } catch {}
        await assertRetryable(index, connection, keyPresent: false)
        XCTAssertNoThrow(try journal.load(connection)) // Transaction rolled back, with a test-only key.
        XCTAssertEqual(sqlite3_exec(database, "DROP TRIGGER fail_forget", nil, nil, nil), SQLITE_OK)
        try await retry(index, connection, journal)
    }
    func testIndexSaveFailureKeepsRowAfterCleanupAndRepeatedForgetIsIdempotent() async throws {
        let (directory, journal, connection, index) = try fixture(.index)
        defer { try? FileManager.default.removeItem(at: directory) }
        do { try await forget(index, connection); XCTFail("Expected index save failure") } catch {}
        await assertRetryable(index, connection, keyPresent: false)
        XCTAssertThrowsError(try journal.load(connection))
        try await retry(index, connection, journal)
    }
    func testLegacyRemovalDoesNotEraseProtectedStorageAndChangedConnectionHasNoEffects() async throws {
        let legacy = Connection(id: "legacy", label: "Synthetic", url: "https://relay.example", token: "synthetic")
        let store = InMemoryConnectionStore([legacy])
        let next = try await ConnectionRemoval.forget(legacy, from: [legacy], eraseProtected: { _ in XCTFail("Legacy must not erase protected storage") }, saveIndex: { try store.save($0) })
        XCTAssertTrue(next.isEmpty); XCTAssertTrue(try store.load().isEmpty)
        let changed = Connection(id: "legacy", label: "Synthetic", url: "https://relay.example", token: "changed")
        do {
            _ = try await ConnectionRemoval.forget(legacy, from: [changed], eraseProtected: { _ in XCTFail("Changed connection must not erase protected storage") }, saveIndex: { _ in XCTFail("Changed connection must not save") })
            XCTFail("Expected changed connection refusal")
        } catch {}
    }
}
