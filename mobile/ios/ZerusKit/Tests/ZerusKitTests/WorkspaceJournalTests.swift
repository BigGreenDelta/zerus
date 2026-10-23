import CryptoKit
import Foundation
import SQLite3
import XCTest
@testable import ZerusKit

final class WorkspaceJournalTests: XCTestCase {
    private func fixture() throws -> JSONObject { try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object) }
    private func pending(_ f: JSONObject) throws -> WorkspaceState {
        let invitation = try XCTUnwrap(f["invitation"]?.object)
        return try WorkspaceState(identity: WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object)), candidate: WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object)),
            issuer: WorkspaceDevice.parse(XCTUnwrap(invitation["issuer"]?.object)), pinnedRevision: WorkspaceProtocol.number(invitation,"membership_revision"), pinnedHead: invitation.string("membership_head"))
    }
    private func active(_ f: JSONObject) throws -> WorkspaceState {
        var state = try pending(f)
        state.membership = try WorkspaceMembership.parse(XCTUnwrap(f["final_state"]?.object)); state.enrollment = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object))
        return state
    }
    func testAtomicMembershipHistoryAndExactCiphertextSurviveReopen() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let key = StaticKeyProvider(SymmetricKey(data: Data(repeating: 9, count: 32))), journal = SQLiteWorkspaceJournal(directory: dir, testKeys: key)
        let connection = Connection(id: "fixture", label: "Fixture", url: "https://relay.example", token: "synthetic", protocolVersion: 2), f = try fixture()
        let prepared = try pending(f); try journal.create(connection, state: prepared)
        let bootstrap = try WorkspaceBootstrap.parse(XCTUnwrap(f["bootstrap_snapshot"]?.object)), approval = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object))
        var enrolled = prepared; enrolled.membership = bootstrap.state; enrolled.enrollment = approval
        try journal.update(connection, expected: prepared, next: enrolled)
        var accepted = enrolled; accepted.membership = try enrolled.membership!.applying(approval.membership)
        try journal.update(connection, expected: enrolled, next: accepted, changes: [approval.membership])
        XCTAssertThrowsError(try journal.update(connection, expected: prepared, next: enrolled))
        let reopened = SQLiteWorkspaceJournal(directory: dir, testKeys: key)
        XCTAssertEqual(try reopened.load(connection).json, accepted.json)
        XCTAssertEqual(try reopened.head(connection, revision: 0), prepared.pinnedHead)
        XCTAssertEqual(try reopened.head(connection, revision: 1), accepted.membership?.head)
        let original = try WorkspaceEnvelope.parse(XCTUnwrap(f["envelope"]?.object)), originalJSON = original.json
        _ = try reopened.reserve(connection, id: original.header.requestId, plainHash: "original-fingerprint", envelope: original)
        let gateway = try WorkspaceIdentity.parse(XCTUnwrap(f["gateway_identity"]?.object))
        let fresh = try WorkspaceEnvelope.seal(identity: accepted.identity, recipient: gateway.device, header: original.header, plaintext: WorkspaceProtocol.decode(f.string("plaintext_b64url")))
        XCTAssertNotEqual(fresh.json, originalJSON)
        XCTAssertEqual(try reopened.reserve(connection, id: original.header.requestId, plainHash: "original-fingerprint", envelope: fresh).json, originalJSON)
        XCTAssertThrowsError(try reopened.reserve(connection, id: original.header.requestId, plainHash: "changed-fingerprint", envelope: fresh))
        try reopened.settle(connection, id: original.header.requestId)
        XCTAssertEqual(try journal.request(connection, id: original.header.requestId), original.header)
        XCTAssertThrowsError(try reopened.reserve(connection, id: original.header.requestId, plainHash: "original-fingerprint", envelope: fresh))
    }
    func testCorruptJournalRefusesWithoutGeneratingReplacementKeys() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let journal = SQLiteWorkspaceJournal(directory: dir, testKeys: StaticKeyProvider(SymmetricKey(data: Data(repeating: 7, count: 32))))
        let connection = Connection(id: "fixture", label: "Fixture", url: "https://relay.example", token: "synthetic", protocolVersion: 2)
        try journal.create(connection, state: pending(fixture()))
        var db: OpaquePointer?
        XCTAssertEqual(sqlite3_open(dir.appendingPathComponent("journal.sqlite3").path, &db), SQLITE_OK)
        defer { sqlite3_close(db) }
        XCTAssertEqual(sqlite3_exec(db, "UPDATE workspace_state SET record=X'00'", nil, nil, nil), SQLITE_OK)
        XCTAssertThrowsError(try journal.load(connection))
        XCTAssertThrowsError(try journal.load(Connection(id: "missing", label: "Fixture", url: "https://relay.example", token: "synthetic", protocolVersion: 2)))
    }
    func testProtectedConnectionMarkerSurvivesCodecAndUnknownVersionRefuses() throws {
        let old = Data(#"{"id":"legacy","label":"Fixture","url":"https://relay.example","token":"synthetic"}"#.utf8)
        XCTAssertEqual(try JSONDecoder().decode(Connection.self, from: old).protocolVersion, 1)
        let secure = Connection(id: "secure", label: "Fixture", url: "https://relay.example", token: "synthetic", protocolVersion: 2)
        XCTAssertEqual(try JSONDecoder().decode(Connection.self, from: JSONEncoder().encode(secure)).protocolVersion, 2)
        XCTAssertThrowsError(try JSONDecoder().decode(Connection.self, from: Data(#"{"id":"bad","label":"Fixture","url":"https://relay.example","token":"synthetic","protocolVersion":3}"#.utf8)))
    }
}
