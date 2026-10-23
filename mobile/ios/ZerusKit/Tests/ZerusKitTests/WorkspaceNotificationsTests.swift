import Foundation
import XCTest
@testable import ZerusKit

final class WorkspaceNotificationsTests: XCTestCase {
    private let computer = "22222222-2222-4222-8222-222222222222"
    private let connection = Connection(id: "protected", label: "Fixture", url: "https://relay.example", token: "synthetic", protocolVersion: 2)
    private func session() -> JSONObject {
        ["name": "provider/fixture/session", "run_id": "run", "conversation_id": "conversation", "state": "running", "process_state": "running",
         "activity": "idle", "phase": "input", "relay_scope_id": "opaque-scope", "attention_signature": .string(String(repeating: "a", count: 64))]
    }
    private func preference() -> JSON {
        .object(["conversation_id": "conversation", "muted": true, "revision": 1, "suppressed": .object(["reply_id": .null, "attention_signature": .null,
            "optional_question_ids": [], "main_attention": .null, "child_attention": .object(JSONObject())])])
    }
    private func mapped(_ row: JSONObject, kind: String = "input", scope: String = "opaque-scope", online: Bool = true, extra: JSONObject? = nil) throws -> JSONObject {
        let records: [JSONObject] = [["id": .string(computer), "name": "Fixture", "online": .bool(online), "via": .null,
                                     "snapshot": .object(["sessions": .array(([row] + (extra.map { [$0] } ?? [])).map(JSON.object))])]]
        let page: JSONObject = ["events": .array([.object(["id": 1, "computer_id": .string(computer), "scope_id": .string(scope), "kind": "alert", "alert_kind": .string(kind), "created_at": 1000])]), "cursor": 1]
        return try XCTUnwrap(WorkspaceNotifications.events(connection, records: records, page: page, after: 0, now: 1000).objects("events").first)
    }
    func testHintsNeedCurrentNativeIdentityNeedAndUnreadUnmutedState() throws {
        XCTAssertEqual(try mapped(session()).string("alert_kind"), "attention")
        XCTAssertEqual(try mapped(session().putting("phase", "error"), kind: "error").string("alert_kind"), "error")
        let read: JSON = .object(["conversation_id": "conversation", "attention_signature": .string(String(repeating: "a", count: 64))])
        let variants: [(String, JSON)] = [("phase", "idle"), ("run_id", ""), ("conversation_id", ""), ("state", "paused"),
                                        ("state", "archived"), ("process_state", "exited"), ("notification_preferences", preference()), ("read", read)]
        for (key, value) in variants { XCTAssertEqual(try mapped(session().putting(key, value)).string("kind"), "refresh", key) }
        XCTAssertEqual(try mapped(session(), kind: "error").string("kind"), "refresh")
        XCTAssertEqual(try mapped(session(), scope: "unknown").string("kind"), "refresh")
        XCTAssertEqual(try mapped(session(), online: false).string("kind"), "refresh")
        XCTAssertEqual(try mapped(session(), extra: session().putting("name", "provider/fixture/other")).string("kind"), "refresh")
    }
    func testCompletedHintNeedsRecentSignedUnreadReplyAndNoNativeWork() throws {
        func reply() -> JSONObject { session().putting("phase", "idle").putting("reply_id", "reply-current").putting("reply_at", 990) }
        let valid = try mapped(reply(), kind: "completed")
        XCTAssertEqual(valid.string("alert_kind"), "completed"); XCTAssertEqual(valid.double("created_at"), 990)
        XCTAssertEqual(valid.string("native_reply_id"), "reply-current")
        let read: JSON = .object(["conversation_id": "conversation", "reply_id": "reply-current"])
        let variants: [(String, JSON)] = [("reply_at", 100), ("reply_at", 1001), ("reply_id", ""), ("activity", "busy"),
                                        ("process_work", .object(["count": 1])), ("notification_preferences", preference()), ("read", read)]
        for (key, value) in variants { XCTAssertEqual(try mapped(reply().putting(key, value), kind: "completed").string("kind"), "refresh", key) }
    }
    func testChangedServiceEventIdentityCannotReannounceSameNativeReply() throws {
        let slot = NotificationSlot(connection: connection.id, computer: computer, session: "provider/fixture/session")
        let current = NotificationCurrent(slot: slot, target: "[run,conversation]", title: "Fixture", machine: "Computer", kind: nil, identity: "", idle: true, conversation: "conversation")
        let preferences = NotificationPreferences(finished: true)
        let before = NotificationState(bootstrap: false, settingsGeneration: 0, records: [NotificationRecord(slot: slot, target: current.target, observedAt: 980)])
        let event = NotificationEvent(id: 1, computer: computer, session: slot.session, kind: "alert", at: 990, alertKind: "completed", nativeReplyId: "reply-current")
        let first = NotificationPolicy.plan(before, current: [current], events: [(slot, event)], preferences: preferences, now: 1000, alertsOnly: true)
        XCTAssertTrue(try XCTUnwrap(first.desired.first).fresh)
        let posted = NotificationPolicy.reserve(first.state, try XCTUnwrap(first.desired.first), audible: false, elapsed: 1000)
        var changed = event; changed.id = 2; changed.at = 1001
        let replay = NotificationPolicy.plan(posted, current: [current], events: [(slot, changed)], preferences: preferences, now: 1002, alertsOnly: true)
        XCTAssertEqual(first.desired.first?.fingerprint, replay.desired.first?.fingerprint)
        XCTAssertFalse(try XCTUnwrap(replay.desired.first).fresh)
        let saved = try JSONDecoder().decode(NotificationEvent.self, from: JSONEncoder().encode(event))
        XCTAssertEqual(saved.nativeReplyId, "reply-current")
    }
}
