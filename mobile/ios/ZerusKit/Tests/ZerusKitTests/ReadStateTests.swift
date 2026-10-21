import XCTest
@testable import ZerusKit

/// Ported from Android ConversationReadStateTest, ReadAttentionTest, ReadAllTest, SharedReadTest and LoadingGateTest.
final class ReadStateTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private func roundTrip(_ state: ConversationReadState) -> ConversationReadState {
        try! JSONDecoder().decode(ConversationReadState.self, from: JSONEncoder().encode(state)).validated()
    }
    private func reply(_ sequence: Int64) -> Event {
        Event(id: "history:reply-\(sequence)", role: "AgentMessage", text: "Reply \(sequence)", time: "", at: 100 - Double(sequence), source: "fixture",
              nativeType: "AgentMessage", historyId: "reply-\(sequence)", historyCursor: "cursor-\(sequence)", historyEpoch: "epoch", incomingSeq: sequence)
    }
    private func own() -> Event {
        Event(id: "history:own", role: "You", text: "Tall outgoing summary", time: "", at: 0, source: "fixture", nativeType: "UserPromptSubmit",
              historyId: "own", historyCursor: "own-cursor", historyEpoch: "epoch")
    }

    func testOpeningAndViewportDoNotAcknowledgeMessages() {
        let state = ConversationReadState().viewport(target, eventId: "incoming", offset: 24)
        XCTAssertTrue(state.record(target)!.seen.isEmpty)
        XCTAssertEqual(state.record(target)!.viewport, SavedConversationViewport(eventId: "incoming", offset: 24))
    }

    func testViewedRowsSurviveRestartWithoutCrossConversationLeak() {
        let state = ConversationReadState().viewed(target, ["incoming:1", "incoming:2"]).viewport(target, eventId: "incoming:1", offset: 30)
        let restored = roundTrip(state)
        XCTAssertEqual(restored, state)
        XCTAssertNil(restored.record(target.with(conversation: "other")))
        XCTAssertNil(restored.record(target.with(connectionId: "other")))
        XCTAssertNil(restored.record(target.with(agentId: "child", parentConversation: "conversation")))
        XCTAssertEqual(restored.record(target.with(run: "resumed")), state.record(target))
    }

    func testCapacityDoesNotEraseSeenRows() {
        let ids = Set((1...5000).map { "message:\($0)" })
        let state = ConversationReadState().viewed(target, ids).viewed(target, ["new"])
        XCTAssertEqual(Set(state.record(target)!.seen), ids)
        XCTAssertTrue(state.record(target)!.incomplete)
    }

    func testOwnCanonicalPositionReadsItsIncomingPrefix() {
        let replies = (1...19).map { reply(Int64($0)) }
        let evidence = ConversationReadPolicies.incomingThroughPosition("epoch", replies + [own()], [own()])
        let read = ConversationReadState().head(target, epoch: "epoch", latest: 19, total: 19, complete: true).visible(target, evidence)
        XCTAssertEqual(evidence.count, 19)
        XCTAssertFalse(evidence.contains { $0.role == "You" })
        XCTAssertEqual(read.record(target)!.readThrough, 19)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(read.record(target), replies + [own()]), 0)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(read.head(target, epoch: "epoch", latest: 20, total: 20, complete: true).record(target), replies + [own()]), 1)
        let first = reply(1), later = reply(2)
        let ordered = [first, own(), later]
        let partial = ConversationReadPolicies.incomingThroughPosition("epoch", ordered, [own()])
        XCTAssertEqual(partial, [first])
        let unproven: [Event] = [{ var e = own(); e.historyEpoch = "other"; return e }(), { var e = own(); e.historyCursor = "other"; return e }(),
                        { var e = own(); e.source = "other"; return e }(), { var e = own(); e.nativeType = "ToolFinished"; return e }()]
        for row in unproven { XCTAssertTrue(ConversationReadPolicies.incomingThroughPosition("epoch", [first, own()], [row]).isEmpty) }
        XCTAssertTrue(ConversationReadPolicies.incomingThroughPosition("", [first, own()], [own()]).isEmpty)
        XCTAssertTrue(ConversationReadPolicies.incomingThroughPosition("epoch", [first, own(), own()], [own()]).isEmpty)
    }

    func testToolsAndOwnRowsNeverAdvanceIncomingReadState() {
        XCTAssertFalse(ConversationReadPolicies.incoming(Event(id: "own", role: "You", text: "text", time: "", nativeType: "UserPromptSubmit")))
        XCTAssertFalse(ConversationReadPolicies.incoming(Event(id: "tool", role: "Tool", text: "text", time: "", nativeType: "ToolFinished")))
        XCTAssertTrue(ConversationReadPolicies.incoming(Event(id: "reply", role: "AgentMessage", text: "text", time: "", nativeType: "AgentMessage")))
    }

    func testCanonicalEvidenceChangesRequireResampling() {
        let old = Event(id: "reply", role: "AgentMessage", text: "Same visible text", time: "", nativeType: "AgentMessage", historyEpoch: "old", incomingSeq: 10)
        let state = ConversationReadState().head(target, epoch: "old", latest: 10, total: 10, complete: true).visible(target, [old])
        var fresh = old; fresh.historyEpoch = "new"; fresh.incomingSeq = 12
        let rebuilt = state.head(target, epoch: "new", latest: 12, total: 12, complete: true)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(rebuilt.record(target), [fresh]), 12)
        XCTAssertNotEqual(ConversationReadPolicies.visibilityEvidence([old]), ConversationReadPolicies.visibilityEvidence([fresh]))
        XCTAssertEqual(ConversationReadPolicies.lowerBound(rebuilt.visible(target, [fresh]).record(target), [fresh]), 0)
    }

    func testExplicitReadUsesCapturedHead() {
        let evidence = ReadAttentionEvidence(target: target, fingerprint: "evidence", epoch: "epoch", head: 10, authoritative: true, loadedIds: [])
        let initial = ConversationReadState().head(target, epoch: "epoch", latest: 10, total: 10, complete: true).review(target, true)
        let next = ReadAttentionPolicies.mark(initial, evidence)
        XCTAssertEqual(next.record(target)!.readThrough, 10)
        XCTAssertFalse(next.record(target)!.reviewLater)
        let changed = initial.head(target, epoch: "epoch", latest: 11, total: 11, complete: true)
        XCTAssertEqual(ReadAttentionPolicies.mark(changed, evidence), changed)
        let loaded = ReadAttentionPolicies.mark(roundTrip(ConversationReadState().review(target, true)),
                                                ReadAttentionEvidence(target: target, fingerprint: "", epoch: "", head: nil, authoritative: false, loadedIds: ["loaded"]))
        XCTAssertEqual(loaded.record(target)!.seen, ["loaded"])
        XCTAssertEqual(loaded.record(target)!.readThrough, 0)
        XCTAssertFalse(loaded.record(target)!.reviewLater)
    }

    private func evidence(_ t: Target? = nil, head: Int64 = 10) -> ReadAttentionEvidence {
        ReadAttentionEvidence(target: t ?? target, fingerprint: "fingerprint", epoch: "epoch", head: head, authoritative: true, loadedIds: [])
    }
    private func capture(_ e: ReadAttentionEvidence) -> ReadAllCapture { ReadAllCapture(target: e.target, evidence: e) }

    func testReadAllBatchDeduplicatesAndSkipsChangedRows() {
        let other = target.with(conversation: "other"), outside = target.with(connectionId: "outside")
        let state = ConversationReadState().head(target, epoch: "epoch", latest: 10, total: 10, complete: true).review(target, true)
            .viewport(target, eventId: "anchor", offset: 7).head(other, epoch: "epoch", latest: 10, total: 10, complete: true)
            .head(outside, epoch: "epoch", latest: 30, total: 30, complete: true)
        let rows = [capture(evidence()), capture(evidence()), capture(evidence(other))]
        let next = ReadAllPolicies.apply(state, captured: rows, current: rows)
        XCTAssertEqual(next.result, ReadAllResult(marked: 2, skipped: 0, partial: 0))
        XCTAssertEqual(next.state.record(target)!.readThrough, 10)
        XCTAssertEqual(next.state.record(target)!.viewport, state.record(target)!.viewport)
        XCTAssertEqual(next.state.record(outside), state.record(outside))
        let initial = ConversationReadState().head(target, epoch: "epoch", latest: 10, total: 10, complete: true).review(target, true)
        let one = [capture(evidence())]
        for changed in [initial.head(target, epoch: "epoch", latest: 11, total: 11, complete: true), initial.head(target, epoch: "new-epoch", latest: 10, total: 10, complete: true)] {
            let result = ReadAllPolicies.apply(changed, captured: one, current: one)
            XCTAssertEqual(result.result, ReadAllResult(marked: 0, skipped: 1, partial: 0))
            XCTAssertEqual(result.state, changed)
        }
        var fingerprint = evidence(); fingerprint.fingerprint = "new"
        XCTAssertEqual(ReadAllPolicies.apply(initial, captured: one, current: [capture(fingerprint)]).result, ReadAllResult(marked: 0, skipped: 1, partial: 0))
    }

    func testPartialLoadedRepliesKeepUnknownUnclaimed() {
        let unknown = target.with(conversation: "unknown"), reminder = target.with(conversation: "reminder")
        let state = ConversationReadState().review(target, true).review(reminder, true)
        let loaded = ReadAttentionEvidence(target: target, fingerprint: "", epoch: "", head: nil, authoritative: false, loadedIds: ["loaded"])
        let empty = ReadAttentionEvidence(target: reminder, fingerprint: "", epoch: "", head: nil, authoritative: false, loadedIds: [])
        let rows = [capture(loaded), ReadAllCapture(target: unknown, evidence: nil), capture(empty)]
        let next = ReadAllPolicies.apply(state, captured: rows, current: rows)
        XCTAssertEqual(next.result, ReadAllResult(marked: 1, skipped: 2, partial: 1))
        XCTAssertEqual(next.state.record(target)!.seen, ["loaded"])
        XCTAssertTrue(next.state.record(reminder)!.reviewLater)
        XCTAssertNil(next.state.record(unknown))
        XCTAssertFalse(ReadAllPolicies.available(empty))
    }

    func testListCandidatesFollowVisibleReplies() {
        func session(_ raw: JSONObject, _ t: Target? = nil) -> Session {
            Session(target: t ?? target, title: "Title", agent: "codex", status: "idle", project: "Project", preview: "reply", pendingCount: 0, raw: raw)
        }
        let working = session(["activity": "busy", "mobile_unread_reply": true])
        XCTAssertFalse(ReadAllPolicies.listCandidate(working, online: true))
        XCTAssertTrue(ReadAllPolicies.listCandidate(working, online: true, reviewLater: true))
        XCTAssertFalse(ReadAllPolicies.listCandidate(session(["activity": "busy"], target.with(archiveId: "archive")), online: true, reviewLater: true))
        let read = session(["activity": "idle", "reply_id": "reply", "read": ["conversation_id": .string(target.conversation), "reply_id": "reply"]])
        XCTAssertFalse(ReadAllPolicies.listCandidate(read, online: true))
        XCTAssertTrue(ReadAllPolicies.listCandidate(read.withRaw("reply_id", "new-reply"), online: true))
    }

    private let signature = String(repeating: "a", count: 64)
    private func row(_ read: JSON = .null, reply: String = "7:100.5", attention: JSON? = nil, _ t: Target? = nil) -> Session {
        let t = t ?? target
        return Session(target: t, title: "Review", agent: "codex", status: "input", project: "project", preview: "", pendingCount: 0,
                       raw: ["run_id": .string(t.run), "conversation_id": .string(t.conversation), "reply_id": .string(reply),
                             "attention_signature": attention ?? .string(signature), "read": read])
    }
    private func mark(_ reply: String?, _ attention: String?, _ conversation: String = "conversation") -> JSON {
        ["conversation_id": .string(conversation), "reply_id": reply.map(JSON.string) ?? .null, "attention_signature": attention.map(JSON.string) ?? .null, "at": 1.0]
    }

    /// Ported from Android SharedReadTest (0082fff0).
    func testOnlyAppliedMarksCountAsSharedAndUnappliedOnesWaitBeforeAnotherTry() {
        let payload = SharedReadPolicies.payload(row(), freshHead: true, localUnread: 0)!
        func receipt(_ reply: Bool, _ attention: Bool, state: String = "completed") -> JSONObject {
            ["request_id": "r", "state": .string(state),
             "result": ["ok": true, "applied": ["reply_id": .bool(reply), "attention_signature": .bool(attention)], "read": .null]]
        }
        XCTAssertTrue(SharedReadPolicies.applied(receipt(true, true), payload: payload))
        // Completed without applying: the request is no longer current, or the computer's question index is not ready.
        XCTAssertFalse(SharedReadPolicies.applied(receipt(true, false), payload: payload))
        XCTAssertFalse(SharedReadPolicies.applied(receipt(false, false), payload: payload))
        XCTAssertFalse(SharedReadPolicies.applied(receipt(true, true, state: "failed"), payload: payload))
        XCTAssertFalse(SharedReadPolicies.applied(["state": "completed", "result": ["ok": false, "error": "not_found"]], payload: payload))
        XCTAssertFalse(SharedReadPolicies.applied(["state": "completed", "result": ["ok": true]], payload: payload))
        // A field this phone did not send need not be applied.
        XCTAssertTrue(SharedReadPolicies.applied(receipt(true, false), payload: SharedReadPolicies.payload(row(), freshHead: true, localUnread: 0, acknowledge: false)!))
        XCTAssertEqual((0...4).map(SharedReadPolicies.retryDelay), [.seconds(120), .seconds(240), .seconds(480), .seconds(960), .seconds(960)])
    }

    func testSharedReadPayloadAndMarks() {
        let payload = SharedReadPolicies.payload(row(), freshHead: true, localUnread: 0)!
        XCTAssertEqual(Set(payload.keys), ["run_id", "conversation_id", "reply_id", "attention_signature"])
        XCTAssertEqual(payload.string("reply_id"), "7:100.5")
        XCTAssertEqual(payload.string("attention_signature"), signature)
        XCTAssertNil(SharedReadPolicies.payload(row(), freshHead: false, localUnread: 0))
        XCTAssertNil(SharedReadPolicies.payload(row(), freshHead: true, localUnread: 2))
        XCTAssertNil(SharedReadPolicies.payload(row(), freshHead: true, localUnread: nil))
        let read = row(mark("7:100.5", signature))
        XCTAssertTrue(SharedReadPolicies.replyRead(read))
        XCTAssertTrue(SharedReadPolicies.attentionAcknowledged(read))
        XCTAssertNil(SharedReadPolicies.payload(read, freshHead: true, localUnread: 0))
        let partial = SharedReadPolicies.payload(row(mark("7:100.5", nil)), freshHead: true, localUnread: 0)!
        XCTAssertEqual(partial["reply_id"], .null)
        XCTAssertEqual(partial.string("attention_signature"), signature)
        XCTAssertFalse(SharedReadPolicies.replyRead(row(mark("7:100.5", signature, "other-conversation"))))
        let readAll = SharedReadPolicies.payload(row(), freshHead: true, localUnread: 0, acknowledge: false)!
        XCTAssertEqual(readAll["attention_signature"], .null)
        XCTAssertNil(SharedReadPolicies.payload(row(mark("7:100.5", nil)), freshHead: true, localUnread: 0, acknowledge: false))
        XCTAssertNil(SharedReadPolicies.payload(row(.null, target.with(agentId: "child", parentConversation: "conversation")), freshHead: true, localUnread: 0))
        XCTAssertNil(SharedReadPolicies.payload(row(.null, reply: "", attention: "not-a-signature"), freshHead: true, localUnread: 0))
    }

    func testUnreadBadges() {
        XCTAssertEqual(UnreadPresentation.badge(total: 3, loaded: 1)?.text, "3")
        XCTAssertNil(UnreadPresentation.badge(total: 0, loaded: 4))
        XCTAssertEqual(UnreadPresentation.badge(total: nil, loaded: 2)?.text, "2+")
        XCTAssertEqual(UnreadPresentation.badge(total: nil, loaded: 0, nativeUnread: true)?.text, "?")
        XCTAssertNil(UnreadPresentation.badge(total: nil, loaded: nil))
    }

    func testLoadingGateDelaysAutomaticReadsAndPromotesExplicitOnes() {
        var time = 42.0
        let gate = LoadingGate(now: { time })
        let operation = gate.begin("session-a").operation
        XCTAssertFalse(gate.visible())
        time += 4.999
        XCTAssertFalse(gate.visible())
        time += 0.001
        XCTAssertTrue(gate.visible())
        XCTAssertEqual(gate.remaining(operation.id), 0)
        let repeated = gate.begin("session-a", explicit: true)
        XCTAssertFalse(repeated.newRequest)
        XCTAssertEqual(repeated.operation.id, operation.id)
        let next = gate.begin("session-b").operation
        XCTAssertFalse(gate.finish(operation.id))
        XCTAssertTrue(gate.owns(next.id))
    }

    func testHeadSchedulerIsFairAndBacksOff() {
        let scheduler = HistoryHeadScheduler()
        let targets = (0..<4).map { target.with(conversation: "c\($0)") }
        XCTAssertEqual(scheduler.choose(targets, now: 0).map(\.conversation), ["c0", "c1"])
        XCTAssertEqual(scheduler.choose(targets, now: 1).map(\.conversation), ["c2", "c3"])
        scheduler.result(targets[0], now: 1, success: false)
        XCTAssertEqual(scheduler.choose(targets, now: 6).map(\.conversation), ["c1", "c2"])
        XCTAssertTrue(scheduler.choose([targets[0]], now: 10).isEmpty)
        XCTAssertEqual(scheduler.choose([targets[0]], now: 11).map(\.conversation), ["c0"])
    }
}

extension Session {
    func withRaw(_ key: String, _ value: JSON) -> Session {
        var copy = self
        copy.raw[key] = value
        return copy
    }
}
