import XCTest
@testable import ZerusKit

/// activity-notice-contract.json, shared with the hgs journal, desktop and Android.
final class ActivityNoticesTests: XCTestCase {
    private func cases() throws -> [JSONObject] {
        try XCTUnwrap(SharedFixture.json("activity-notice-contract.json").object).objects("cases")
    }

    private func parse(_ row: JSONObject) -> [Event] { NativeEvents.events(["events": .array([.object(row)])]) }

    private func label(_ event: Event) -> String { ActivityNotices.label(origin: event.origin ?? "", sender: event.sender ?? "") }

    func testNativePromptsArePresentedAsTheSharedContractSays() throws {
        let all = try cases()
        XCTAssertFalse(all.isEmpty)
        for (index, item) in all.enumerated() {
            let name = item.string("name")
            var row = try XCTUnwrap(item.object("event"), name)
            row["seq"] = .int(Int64(index + 1))
            row["at"] = .double(1_791_018_000 + Double(index))
            let event = try XCTUnwrap(parse(row).first, name)
            let shown = try XCTUnwrap(item.object("presentation"), name)
            if shown.string("kind") == "notice" {
                XCTAssertEqual(event.role, ActivityNotices.role, name)
                XCTAssertEqual(label(event), shown.string("label"), name)
                // Nobody sent it from this phone: no delivery state, no outgoing identity, no reply.
                XCTAssertEqual(event.delivery, "", name)
                XCTAssertEqual(event.requestId, "", name)
                XCTAssertFalse(QuestionReplyPresentation.isOwnRole(event.role), name)
                XCTAssertFalse(ConversationReadPolicies.incoming(event), name)
            } else {
                XCTAssertEqual(event.role, "You", name)
            }
            XCTAssertEqual(event.text, shown.string("body"), name)
            XCTAssertEqual(event.report ?? "", shown.string("report"), name)
            XCTAssertFalse((label(event) + event.text).localizedCaseInsensitiveContains("verified"), name)
            // The private display cache keeps the presentation.
            XCTAssertEqual(try JSONDecoder().decode([Event].self, from: JSONEncoder().encode([event])), [event], name)
        }
    }

    func testNoticesKeepTheIdentitiesOfTheirNativeRows() throws {
        let peer = try XCTUnwrap(cases().first { $0.object("event")?.string("origin") == "peer_message" }?.object("event"))
        var marked = peer
        marked["seq"] = .int(7)
        marked["at"] = .double(1)
        var unmarked = marked
        unmarked["origin"] = nil
        let notice = try XCTUnwrap(parse(marked).first), person = try XCTUnwrap(parse(unmarked).first)
        XCTAssertEqual(person.role, "You")
        XCTAssertEqual(notice.id, person.id)
        XCTAssertEqual(notice.originalId, person.originalId)
    }

    func testSearchNamesTheNoticeLikeItsCard() throws {
        let item = try XCTUnwrap(cases().first { $0.object("event")?.string("origin") == "peer_message" })
        var row = try XCTUnwrap(item.object("event"))
        row["seq"] = .int(1)
        row["at"] = .double(1)
        let event = try XCTUnwrap(parse(row).first)
        XCTAssertEqual(ConversationSearch.prepare([event]).first?.role, item.object("presentation")?.string("label"))
    }

    func testOlderCachedRowsWithoutNoticeFieldsStillDecode() throws {
        let row = Event(id: "id", role: "You", text: "Hello", time: "")
        var stored = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(row)) as? [String: Any])
        for key in ["origin", "sender", "report"] { stored.removeValue(forKey: key) }
        let decoded = try JSONDecoder().decode(Event.self, from: JSONSerialization.data(withJSONObject: stored))
        XCTAssertNil(decoded.origin)
        XCTAssertEqual(decoded.text, "Hello")
    }

    func testAnIdlessNoticeReplacesItsLegacyCachedRowWithoutChangingIdentity() throws {
        let row: JSONObject = ["type":"UserPromptSubmit", "origin":"peer_message", "sender":"x/y",
                               "detail":"Message from x/y", "report":"The full peer message.", "at":1]
        // No seq, source_offset, message_id, original_id or history_id: exercise the fallback digest.
        var old = row
        old["origin"] = nil
        let legacy = try XCTUnwrap(parse(old).first)
        XCTAssertEqual(legacy.role, "You")
        XCTAssertEqual(legacy.text, "Message from x/y")
        var cache = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode([legacy])) as? [[String: Any]])
        for key in ["origin", "sender", "report"] { cache[0].removeValue(forKey: key) }
        let restored = try JSONDecoder().decode([Event].self, from: JSONSerialization.data(withJSONObject: cache))
        let notice = try XCTUnwrap(parse(row).first)
        XCTAssertEqual(legacy.id, notice.id)
        XCTAssertEqual(legacy.originalId, notice.originalId)
        let target = Target(computerId: "computer", session: "codex/project/session", run: "run", conversation: "conversation")
        let merged = RollingConversationHistory.merge(target: target, previousTarget: target, previous: restored, incoming: [notice])
        XCTAssertEqual(merged.events, [notice])
        XCTAssertEqual(notice.role, ActivityNotices.role)
        XCTAssertEqual(notice.text, "The full peer message.")
        XCTAssertEqual(notice.requestId, "")
        XCTAssertEqual(notice.delivery, "")
        XCTAssertFalse(ConversationReadPolicies.incoming(notice))
    }

    func testCachedNoticesCannotBecomeOutgoingReceipts() throws {
        let notice = Event(id: "history:1", role: "Notice", text: "Peer text", time: "", requestId: "other-request",
                           source: "hgs_delivery", delivery: "recorded", outgoingId: "other-request", submittedText: "Peer text",
                           originalId: "journal:original", historyId: "1", historyCursor: "cursor", historyEpoch: "epoch",
                           deliveryState: "queued", origin: "peer_message", sender: "claude/project/review")
        XCTAssertEqual(notice.receiptRequestId, "")
        let target = Target(computerId: "computer", session: "claude/project/session", run: "run", conversation: "conversation")
        let encoded = try ConversationDisplayCache.encode(target, raw: [:], events: [notice], truncated: false)
        let restored = try XCTUnwrap(ConversationDisplayCache.decode(encoded)).events
        XCTAssertEqual(restored, [ActivityNotices.cached(notice)])
        let cache = CachedHistoryPage(target: target, epoch: "epoch", events: [notice], before: "cursor", after: "cursor",
                                      hasBefore: false, hasAfter: false, latestIncoming: nil, totalIncoming: nil,
                                      complete: true, indexing: false, truncated: false, projectionApplied: 37)
        let stored = try JSONDecoder().decode(CachedHistoryPage.self, from: JSONEncoder().encode(cache))
        let page = try stored.page(for: target)
        XCTAssertEqual(page.projectionApplied, 37)
        let decoded = try XCTUnwrap(page.events.first)
        XCTAssertEqual(decoded, ActivityNotices.cached(notice))
        XCTAssertEqual(decoded.requestId, "")
        XCTAssertEqual(decoded.receiptRequestId, "")
        XCTAssertEqual(decoded.outgoingId, "")
        XCTAssertEqual(decoded.delivery, "")
        XCTAssertNil(decoded.deliveryState)
        XCTAssertEqual(decoded.id, "history:1")
        XCTAssertEqual(decoded.historyCursor, "cursor")
    }

    func testOriginCannotTurnAnAgentReplyOrAnswerIntoAPeerPrompt() throws {
        for (type, role) in [("AgentMessage", "AgentMessage"), ("QuestionAnswered", "You (answer)")] {
            let event = try XCTUnwrap(parse(["type": .string(type), "origin": "peer_message", "sender": "x/y",
                                             "detail": "Native text", "report": "Not the native text", "message_id": "other-request",
                                             "delivery_state": "queued"]).first)
            XCTAssertEqual(event.role, role)
            XCTAssertEqual(event.text, "Native text")
            XCTAssertNil(event.origin)
            XCTAssertEqual(event.requestId, "")
            XCTAssertNil(event.deliveryState)
        }
        XCTAssertEqual(ActivityNotices.body(origin: "peer_message", detail: "Summary", report: " \n "), " \n ")
    }
}
