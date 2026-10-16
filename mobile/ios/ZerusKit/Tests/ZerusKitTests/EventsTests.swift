import XCTest
@testable import ZerusKit

/// Ported from Android NativeEventIdentityTest, RollingConversationHistoryTest,
/// NativeStopExcerptTest and the event parts of RelaySafetyTest.
final class EventsTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")

    private func row(_ text: String, _ at: Double, id: String = "") -> JSON {
        var value: JSONObject = ["type": "AgentMessage", "at": .double(at), "detail": .string(text)]
        if !id.isEmpty { value["message_id"] = .string(id) }
        return .object(value)
    }
    private func history(_ rows: JSON...) -> JSONObject { ["provider_messages": .array(rows)] }
    private func codec(_ rows: [Event]) -> [Event] { try! JSONDecoder().decode([Event].self, from: JSONEncoder().encode(rows)) }

    func testPrependingHistoryPreservesUnchangedFallbackKeys() {
        let first = row("First", 2), second = row("Second", 3)
        let before = NativeEvents.events(history(first, second))
        let after = NativeEvents.events(history(row("Older", 1), first, second, row("Newer", 4)))
        XCTAssertEqual(after.filter { ["First", "Second"].contains($0.text) }.map(\.id), before.map(\.id))
    }

    func testIdenticalRowsRemainDistinctAndKeepKeys() {
        let repeated = row("Repeated", 2)
        let before = NativeEvents.events(history(repeated, repeated))
        XCTAssertEqual(before.count, 2)
        XCTAssertEqual(Set(before.map(\.id)).count, 2)
        let after = NativeEvents.events(history(row("Older", 1), repeated, repeated))
        XCTAssertEqual(after.filter { $0.text == "Repeated" }.map(\.id), before.map(\.id))
        let distinct = NativeEvents.events(history(row("Repeated", 2, id: "one"), row("Repeated", 2, id: "two")))
        XCTAssertEqual(distinct.count, 2)
        XCTAssertNotEqual(distinct[0].id, distinct[1].id)
        XCTAssertEqual(NativeEvents.events(history(row("Same record", 2, id: "one"), row("Same record", 2, id: "one"), row("Same record", 2, id: "two"))).count, 2)
    }

    func testNativeIdsSurviveContentUpdates() {
        XCTAssertEqual(NativeEvents.events(history(row("Partial", 2, id: "one")))[0].id, NativeEvents.events(history(row("Partial completed", 2, id: "one")))[0].id)
        var first: JSONObject = ["type": "UserPromptSubmit", "seq": 12, "at": 1, "detail": "Original"]
        let id = NativeEvents.events(["events": [.object(first)]])[0].id
        first["detail"] = "Updated"
        XCTAssertEqual(NativeEvents.events(["events": [.object(first)]])[0].id, id)
    }

    func testExactChildInspectionAcceptsOnlyItsOwnRows() {
        let own = row("Child reply", 2, id: "child-one").object!.putting("agent_id", "child")
        let normalized = row("Normalized reply", 3, id: "child-two").object!
        let parent = row("Parent reply", 4, id: "parent").object!.putting("agent_id", "main")
        let foreign = row("Other reply", 5, id: "foreign").object!.putting("agent_id", "other-child")
        let rows: [JSON] = [.object(own), .object(normalized), .object(parent), .object(foreign)]
        XCTAssertEqual(NativeEvents.events(["provider_messages": .array(rows), "agent_id": "child"]).map(\.text), ["Child reply", "Normalized reply"])
        XCTAssertEqual(NativeEvents.events(["provider_messages": .array(rows)]).map(\.text), ["Normalized reply", "Parent reply"])
    }

    func testCanonicalPagesPreserveServerOrderAndProvenance() {
        let first: JSON = ["type": "UserPromptSubmit", "seq": 3, "at": 9, "detail": "First canonical", "history_id": "one",
                           "history_stream": "journal", "history_cursor": "opaque-one", "detail_truncated": true]
        let second: JSON = ["type": "AgentMessage", "message_id": "reply", "source": "codex_transcript", "at": 1, "detail": "Second canonical",
                            "history_id": "two", "history_stream": "provider", "incoming_seq": 7]
        let rows = NativeEvents.events(["history_epoch": "epoch", "events": [first, second]])
        XCTAssertEqual(rows.map(\.id), ["history:one", "history:two"])
        XCTAssertEqual(rows[0].originalId, "journal:journal:You:native:3")
        XCTAssertEqual(rows[1].originalId, "provider:codex_transcript:AgentMessage:native:reply")
        XCTAssertEqual(rows[1].incomingSeq, 7)
        XCTAssertEqual(rows[0].historyCursor, "opaque-one")
        XCTAssertTrue(rows[0].detailTruncated)
    }

    func testStopExcerptDeduplicatesOnlyUniquePairs() {
        let raw: JSONObject = ["events": [["type": "UserPromptSubmit", "at": 1, "seq": 1, "detail": "Please review"], ["type": "Stop", "at": 3, "seq": 2, "detail": "Review complete"]],
                               "provider_messages": [["type": "AgentMessage", "at": 2, "message_id": "m1", "detail": "Review complete with details", "agent_id": "main"]]]
        let events = NativeEvents.events(raw)
        XCTAssertEqual(events.count, 2)
        XCTAssertEqual(events[0].role, "You")
        XCTAssertEqual(events[1].text, "Review complete with details")
        let repeated: JSONObject = ["provider_messages": [row("Repeated full", 2, id: "reply")],
                                    "events": [["type": "Stop", "seq": 1, "at": 2, "detail": "Repeated"], ["type": "Stop", "seq": 2, "at": 2, "detail": "Repeated"]]]
        XCTAssertEqual(NativeEvents.events(repeated).count, 3)
    }

    func testNativeUnicodeClippingShape() {
        let prefix = String(repeating: "🦀a", count: 600)
        let full = prefix + String(repeating: " continuation", count: 50)
        let clipped = prefix + "…"
        XCTAssertEqual(prefix.unicodeScalars.count, 1200)
        XCTAssertTrue(NativeStopExcerpt.matches(full, clipped))
        XCTAssertFalse(NativeStopExcerpt.matches(full, String(prefix.dropLast()) + "…"))
        XCTAssertFalse(NativeStopExcerpt.matches(full, prefix + "a…"))
        XCTAssertFalse(NativeStopExcerpt.matches("Wait for the answer", "Wait…"))
        XCTAssertTrue(NativeStopExcerpt.matches("Wait… for the answer", "Wait…"))
        for excerpt in ["", " \n\t", "…", " … ", "……"] { XCTAssertFalse(NativeStopExcerpt.matches("Prefix \(excerpt) suffix", excerpt)) }
        let hook: JSON = ["type": "Stop", "seq": 1, "at": 2, "detail": .string(clipped)]
        let provider: JSON = ["type": "AgentMessage", "message_id": "reply", "at": 3, "detail": .string(full)]
        let expected = NativeEvents.events(["provider_messages": [provider]])
        XCTAssertEqual(NativeEvents.events(["events": [hook], "provider_messages": [provider]]), expected)
        let separate = NativeEvents.events(["events": [hook]]) + expected
        let cached = RollingConversationHistory.merge(target: target, previousTarget: target, previous: codec(separate), incoming: [])
        XCTAssertEqual(cached.events, [separate[1]])
        XCTAssertEqual(cached.supersededIds, [separate[0].id: separate[1].id])
        for (at, count) in [(11.999, 1), (12.0, 2), (-7.999, 1), (-8.0, 2)] {
            let shifted: JSON = ["type": "AgentMessage", "message_id": "reply", "at": .double(at), "detail": .string(full)]
            XCTAssertEqual(NativeEvents.events(["events": [hook], "provider_messages": [shifted]]).count, count, "\(at)")
        }
    }

    func testRollingHistoryKeepsOwnRowsAndExactTargets() {
        func event(_ id: String, _ at: Double, _ text: String? = nil) -> Event { Event(id: id, role: "You", text: text ?? id, time: "", at: at) }
        let own = event("journal:1", 1, "Own confirmed message")
        var reply = event("provider:2", 2, "Partial")
        var complete = reply; complete.text = "Complete"
        let result = RollingConversationHistory.merge(target: target, previousTarget: target, previous: [own, reply], incoming: [complete, event("provider:3", 3)])
        XCTAssertEqual(result.events.map(\.id), ["journal:1", "provider:2", "provider:3"])
        XCTAssertEqual(result.events[1].text, "Complete")
        XCTAssertFalse(result.truncated)
        for previous in [target.with(connectionId: "other"), target.with(computerId: "other"), target.with(session: "other"), target.with(run: "other"),
                         target.with(conversation: "other"), target.with(archiveId: "archive")] {
            XCTAssertEqual(RollingConversationHistory.merge(target: target, previousTarget: previous, previous: [event("old", 1)], incoming: [event("fresh", 2)]).events.map(\.id), ["fresh"])
        }
        let many = (0...9).map { event("id\($0)", Double($0), "Same text") }
        let bounded = RollingConversationHistory.merge(target: target, previousTarget: target, previous: many, incoming: [], maxEvents: 3)
        XCTAssertEqual(bounded.events.map(\.id), ["id7", "id8", "id9"])
        XCTAssertTrue(bounded.truncated)
        reply.text = String(repeating: "🦀\n\"", count: 100)
        let large = (0...9).map { index -> Event in var copy = reply; copy.id = "id\(index)"; copy.at = Double(index); return copy }
        let sized = RollingConversationHistory.merge(target: target, previousTarget: target, previous: large, incoming: [], maxBytes: 2000)
        XCTAssertTrue(sized.truncated)
        XCTAssertLessThanOrEqual(sized.events.reduce(2) { $0 + $1.approximateBytes + 1 }, 2000)
    }

    func testRecordedOutboxStaysAndAnswersCannotConfirmSends() {
        let outgoing = OutgoingMessage(requestId: "receipt", target: target, text: "Own message", attachments: [], createdAt: 1, status: "recorded", submittedAt: 2)
        let result = ConversationMerge.merge(target: target, native: [], outgoing: [outgoing])
        XCTAssertEqual(result.events.map(\.id), ["outgoing:receipt"])
        XCTAssertEqual(result.events[0].delivery, "recorded")
        XCTAssertEqual(result.events[0].at, 2)
        XCTAssertTrue(result.recordedRequestIds.isEmpty)
        let answer: JSON = ["type": "QuestionAnswered", "seq": 1, "at": 2, "detail": "Same text"]
        let events = NativeEvents.events(["message_events": [answer]])
        XCTAssertEqual(events[0].role, "You (answer)")
        let sent = OutgoingMessage(requestId: "receipt", target: target, text: "Same text", attachments: [], createdAt: 1, status: "submitted", submittedAt: 2)
        let merged = ConversationMerge.merge(target: target, native: events, outgoing: [sent])
        XCTAssertEqual(merged.events.count, 2)
        XCTAssertTrue(merged.recordedRequestIds.isEmpty)
        let own: JSON = ["type": "UserPromptSubmit", "seq": 1, "at": 1, "detail": "Own message"]
        XCTAssertEqual(NativeEvents.events(["events": [own], "message_events": [own]]), NativeEvents.events(["events": [own]]))
    }

    func testAttachmentReceiptsReplaceOnlyExactPrompts() {
        let text = "Compare images"
        let submitted = text + "\n\nInspect the attached image at \"/synthetic/request-one/image.png\""
        let prompt: JSON = ["type": "UserPromptSubmit", "seq": 1, "at": 202, "detail": .string(submitted)]
        let receipt: JSON = ["type": "UserPromptSubmit", "source": "hgs_delivery", "message_id": "receipt", "at": 100, "submitted_text": .string(submitted),
                             "detail": .string(text), "attachments": [["name": "image.png", "mime": "image/png", "bytes": 120]]]
        let previous = NativeEvents.events(["events": [prompt]])
        let incoming = NativeEvents.events(["attachment_messages": [receipt]])
        let rolling = RollingConversationHistory.merge(target: target, previousTarget: target, previous: previous, incoming: incoming)
        XCTAssertEqual(rolling.events.map(\.text), [text])
        XCTAssertEqual(rolling.supersededIds, [previous[0].id: incoming[0].id])
        let joint = NativeEvents.events(["events": [prompt], "attachment_messages": [receipt]])
        XCTAssertEqual(joint.map(\.text), [text])
        XCTAssertEqual(joint[0].originalIds, [previous[0].id])
        XCTAssertEqual(joint[0].at, 202)
        var ambiguous = previous[0]; ambiguous.id = "journal:other"
        XCTAssertEqual(RollingConversationHistory.merge(target: target, previousTarget: target, previous: previous + [ambiguous], incoming: incoming).events.count, 3)
        let other = receipt.object!.putting("message_id", "other-receipt")
        XCTAssertEqual(NativeEvents.events(["events": [prompt], "attachment_messages": [receipt, .object(other)]]).count, 3)
    }

    func testCanonicalAliasesAreExact() {
        let hook = Event(id: "journal:Stop:1", role: "AgentMessage", text: "Repeated", time: "", at: 2, nativeType: "Stop")
        var second = hook; second.id = "journal:Stop:2"
        let provider = Event(id: "provider:AgentMessage:1", role: "AgentMessage", text: "Repeated full", time: "", at: 2, nativeType: "AgentMessage")
        let ambiguous = RollingConversationHistory.merge(target: target, previousTarget: target, previous: [hook, second], incoming: [provider])
        XCTAssertEqual(ambiguous.events.count, 3)
        XCTAssertTrue(ambiguous.supersededIds.isEmpty)
        var canonical = hook
        canonical.id = "history:canonical"; canonical.historyId = "canonical"; canonical.originalId = provider.id; canonical.originalIds = [hook.id]
        let merged = RollingConversationHistory.merge(target: target, previousTarget: target, previous: [hook], incoming: [canonical])
        XCTAssertEqual(merged.events.map(\.id), ["history:canonical"])
        XCTAssertEqual(merged.supersededIds, [hook.id: canonical.id])
        XCTAssertEqual(codec([canonical]), [canonical])
    }

    func testMessagingUsesVerifiedNativeReadiness() {
        let raw: JSONObject = ["tracked": true, "run_id": "r", "conversation_id": "c", "runtime_state": "live", "process_state": "running", "activity": "idle", "phase": "idle"]
        XCTAssertEqual(NativeEvents.messageBlockReason(raw), "")
        XCTAssertFalse(NativeEvents.messageBlockReason(raw.putting("phase", "approval")).isEmpty)
        XCTAssertFalse(NativeEvents.messageBlockReason(raw.putting("run_id", "")).isEmpty)
        XCTAssertFalse(NativeEvents.messageBlockReason(raw.putting("runtime_state", "dead")).isEmpty)
        XCTAssertFalse(NativeEvents.messageBlockReason(nil).isEmpty)
    }

    func testQuestionsParseNativeCards() {
        let raw: JSONObject = ["pending_questions": [["question_id": "q", "question_hash": "h", "can_answer": true, "optional": true, "created_at": 10,
                                                      "questions": [["id": "p", "question": "Pick", "multi_select": true, "allow_other": true,
                                                                     "options": [["id": "a", "label": "A", "description": "First"]]]],
                                                      "answer_delivery": ["request_id": "r", "status": "submitted", "answers": [["question_id": "p", "selected_option_ids": ["a"], "text": ""]]]]]]
        let question = NativeEvents.questions(raw)[0]
        XCTAssertEqual(question.id, "q")
        XCTAssertTrue(question.optional)
        XCTAssertEqual(question.prompts[0].choices.map(\.id), ["a"])
        XCTAssertTrue(question.prompts[0].required)
        XCTAssertEqual(question.delivery, "submitted")
        XCTAssertEqual(question.answerDelivery?.answers[0].optionIds, ["a"])
    }
}
