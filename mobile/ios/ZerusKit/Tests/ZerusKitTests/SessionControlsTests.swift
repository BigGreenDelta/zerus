import XCTest
@testable import ZerusKit

/// Ported from Android SessionActionTest, SessionActionFollowUpTest, SessionControlPresentationTest,
/// ScheduledSettingsAttemptTest, RecoveryPresentationTest, RecoveryActionTest and ContextOperationsTest.
final class SessionActionTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private var action: SessionAction { SessionAction(requestId: "request", target: target, operation: "resume", arguments: [:]) }
    private func receipt(_ result: JSONObject = [:]) -> JSONObject {
        var native: JSONObject = ["request_id": "request", "name": "session", "run_id": "run", "conversation_id": "conversation", "status": "completed"]
        for key in result.keys { native[key] = result[key] }
        return ["request_id": "request", "state": "completed", "result": .object(native)]
    }

    func testMismatchedNativeReceiptNeverUnblocksAction() {
        XCTAssertTrue(SessionActionPolicies.result(action, receipt(["run_id": "other"])).blocksSending)
        var foreign = receipt()
        foreign["request_id"] = "other"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(action, foreign).status)
    }

    func testOriginalReceiptAndAuthoritativeResultTargetRemainDistinct() {
        let result = SessionActionPolicies.result(action, receipt(["result_target": ["name": "renamed", "run_id": "new-run", "conversation_id": "conversation"]]))
        XCTAssertEqual(target, result.target)
        XCTAssertEqual("new-run", result.resultTarget?.run)
        XCTAssertEqual("completed", result.status)
    }

    func testRecoveryPersistsAttemptWithoutReplayAndBlocksMessage() throws {
        let state = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(MessageState(sessionActions: [action]))).recovered()
        XCTAssertEqual("request", state.sessionActions.first?.requestId)
        XCTAssertEqual("uncertain", state.sessionActions.first?.status)
        XCTAssertThrowsError(try state.enqueue(Draft(target: target, text: "text"), requestId: "send", at: 1))
    }

    func testQueueReceiptMustMatchSelectedImmutableQueueItem() {
        let queue = String(repeating: "a", count: 64)
        var sendNow = action
        sendNow.operation = "send_now"
        sendNow.arguments = ["queue_id": .string(queue)]
        XCTAssertEqual("uncertain", SessionActionPolicies.result(sendNow, receipt(["status": "submitted", "queue_id": .string(String(repeating: "b", count: 64))])).status)
        XCTAssertEqual("completed", SessionActionPolicies.result(sendNow, receipt(["status": "submitted", "queue_id": .string(queue)])).status)
    }

    func testSettingsCannotAcknowledgeUnrequestedOrChangedModel() {
        var settings = action
        settings.operation = "settings"
        settings.arguments = ["model": "synthetic"]
        XCTAssertEqual("uncertain", SessionActionPolicies.result(settings, receipt(["status": "scheduled", "model": "different"])).status)
        let scheduled = SessionActionPolicies.result(settings, receipt(["status": "scheduled", "model": "synthetic", "pending_settings_id": "10000000-0000-4000-8000-000000000001"]))
        XCTAssertEqual("scheduled", scheduled.status)
        XCTAssertEqual("10000000-0000-4000-8000-000000000001", scheduled.pendingId)
    }

    func testArbitraryActionArgumentsAndMalformedQueueIdsRejected() {
        XCTAssertThrowsError(try SessionActionPolicies.arguments("pause", ["action": "terminate"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("send_now", ["queue_id": "../other"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("rename", ["new_name": "bad\nname"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("fork", ["tag": "a/b"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("settings", ["effort": "turbo"]))
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", ["model": "native-model", "effort": "high"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("terminal_input", ["terminal_binding_id": "b", "key": "F13"]))
        XCTAssertNoThrow(try SessionActionPolicies.arguments("terminal_input", ["terminal_binding_id": "b", "text": "ls", "enter": true]))
    }

    func testSettingsInspectionRequiresTheExactCatalogAndPendingIdentity() {
        let raw: JSONObject = ["settings_change_supported": true, "model": "a", "pending_settings_id": "p",
                               "model_options": [["id": "a", "effort_options": ["low", "high"]], ["id": "b"]], "effort_options": ["low"]]
        XCTAssertEqual("", SessionActionPolicies.inspectReason(target, "settings", ["model": "a", "effort": "high"], raw))
        XCTAssertEqual("The selected model is no longer in the native catalog.", SessionActionPolicies.inspectReason(target, "settings", ["model": "c"], raw))
        XCTAssertEqual("The selected effort is unavailable for this model.", SessionActionPolicies.inspectReason(target, "settings", ["model": "b", "effort": "high"], raw))
        XCTAssertEqual("The selected effort is unavailable for this model.", SessionActionPolicies.inspectReason(target, "settings", ["model": "a", "effort": "turbo"], raw))
        XCTAssertEqual("The pending settings changed.", SessionActionPolicies.inspectReason(target, "settings", ["model": "a", "expected_pending_id": "q"], raw))
        var unsupported = raw
        unsupported["settings_change_supported"] = false
        unsupported["settings_change_reason"] = "Native reason"
        XCTAssertEqual("Native reason", SessionActionPolicies.inspectReason(target, "settings", ["model": "a"], unsupported))
    }

    func testLifecycleRenameKeepsProviderAndProject() {
        let session = target.with(session: "codex/project/old")
        let raw: JSONObject = ["allowed_actions": ["rename"]]
        XCTAssertEqual("", SessionActionPolicies.inspectReason(session, "rename", ["new_name": "codex/project/new"], raw))
        XCTAssertFalse(SessionActionPolicies.inspectReason(session, "rename", ["new_name": "claude/project/new"], raw).isEmpty)
        XCTAssertFalse(SessionActionPolicies.inspectReason(session, "rename", ["new_name": "codex/project/new.tag"], raw).isEmpty)
        XCTAssertEqual("Not now", SessionActionPolicies.inspectReason(session, "pause", [:], ["allowed_actions": [], "action_reasons": ["pause": "Not now"]]))
    }

    func testProcessStopNeedsTheExactProcessAndGeneration() {
        let raw: JSONObject = ["processes": ["generation": "g1", "items": [["id": "p", "run_id": "run", "conversation_id": "conversation",
                                                                            "capabilities": ["stop": true]]]]]
        XCTAssertEqual("", SessionActionPolicies.inspectReason(target, "process_stop", ["process_id": "p", "generation": "g1"], raw))
        XCTAssertEqual("The process generation changed.", SessionActionPolicies.inspectReason(target, "process_stop", ["process_id": "p", "generation": "g2"], raw))
        XCTAssertEqual("This process is no longer present.", SessionActionPolicies.inspectReason(target, "process_stop", ["process_id": "x"], raw))
        let before = SessionActionPolicies.evidence("process_stop", raw, ["process_id": "p"])
        var changed = raw
        changed["processes"] = ["generation": "g2", "items": raw.object("processes")!["items"]!]
        XCTAssertNotEqual(before, SessionActionPolicies.evidence("process_stop", changed, ["process_id": "p"]))
    }

    func testLaunchResultRequiresCreatedTargetAndReportsUnconfirmedProject() {
        let launch = SessionAction(requestId: "request", target: Target(computerId: "computer", session: "", run: "", conversation: "", connectionId: "workspace"),
                                   operation: "launch", arguments: ["agent": "codex", "directory": "/work", "tag": "tag", "swarm_id": "s", "project_id": "p", "add_folder": false])
        let native: JSONObject = ["request_id": "request", "status": "created", "result_target": ["name": "codex/work/tag", "run_id": "r1", "conversation_id": ""]]
        let result = SessionActionPolicies.result(launch, ["request_id": "request", "state": "completed", "result": .object(native)])
        XCTAssertEqual("completed", result.status)
        XCTAssertEqual("codex/work/tag", result.resultTarget?.session)
        XCTAssertTrue(result.error.contains("project assignment was not confirmed"))
        XCTAssertTrue(result.needsProjectReview)
        var archived = native
        archived["result_target"] = ["name": "codex/work/tag", "run_id": "r1", "archive_id": "x"]
        XCTAssertEqual("uncertain", SessionActionPolicies.result(launch, ["request_id": "request", "state": "completed", "result": .object(archived)]).status)
    }
}

final class SessionFollowUpTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "codex/example/session", run: "run", conversation: "conversation", connectionId: "workspace")
    private var archive: Target { target.with(archiveId: "10000000-0000-4000-8000-000000000001") }
    private func action(_ operation: String, _ original: Target? = nil, _ destination: Target? = nil) -> SessionAction {
        SessionAction(requestId: "request", target: original ?? target, operation: operation, arguments: [:], status: "completed", resultTarget: destination)
    }

    func testRemovedBindingsReturnToSessionsOnlyForStillActiveConfirmation() {
        for operation in ["archive", "forget"] {
            let action = action(operation)
            XCTAssertEqual(.sessions, SessionActionFollowUp.kind(action, selected: target, sameNavigation: true))
            XCTAssertEqual(.none, SessionActionFollowUp.kind(action, selected: target, sameNavigation: false))
            XCTAssertEqual(.none, SessionActionFollowUp.kind(action, selected: target.with(session: "other"), sameNavigation: true))
            var uncertain = action
            uncertain.status = "uncertain"
            XCTAssertEqual(.refresh, SessionActionFollowUp.kind(uncertain, selected: target, sameNavigation: true))
        }
    }

    func testRestoreOpensVerifiedLiveResultWithoutPromotingArchiveDraft() {
        let restore = action("restore", archive, target)
        XCTAssertEqual(.openResult, SessionActionFollowUp.kind(restore, selected: archive, sameNavigation: true))
        let state = MessageState(drafts: [Draft(target: archive, text: "saved archive composition")], sessionActions: [restore])
        XCTAssertEqual(state, ActionDraftPolicies.promote(state, restore))
        var foreign = restore
        foreign.resultTarget = target.with(connectionId: "foreign")
        XCTAssertFalse(SessionActionFollowUp.openable(foreign))
        foreign.resultTarget = archive.with(run: "other")
        XCTAssertFalse(SessionActionFollowUp.openable(foreign))
    }

    func testArchiveReceiptMustConfirmTheExecutedImmutableVersion() {
        var restore = action("restore", archive, target)
        restore.status = "sending"
        var native: JSONObject = ["request_id": "request", "name": .string(archive.session), "run_id": .string(archive.run), "conversation_id": .string(archive.conversation),
                                  "status": "completed", "result_target": ["name": .string(target.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation)]]
        func receipt() -> JSONObject { ["request_id": "request", "state": "completed", "result": .object(native)] }
        XCTAssertEqual("uncertain", SessionActionPolicies.result(restore, receipt()).status)
        native["archive_id"] = "20000000-0000-4000-8000-000000000001"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(restore, receipt()).status)
        native["archive_id"] = .string(archive.archiveId)
        XCTAssertEqual("completed", SessionActionPolicies.result(restore, receipt()).status)
        var live = restore
        live.target = target
        XCTAssertEqual("uncertain", SessionActionPolicies.result(live, receipt()).status)
    }

    func testNativeAllowedActionsAreThePresentationAuthority() {
        let raw: JSONObject = ["allowed_actions": ["pause", "rename"], "action_reasons": ["resume": "Already running"]]
        XCTAssertEqual("", SessionControlPresentation.reason(target, "pause", raw))
        XCTAssertEqual("Already running", SessionControlPresentation.reason(target, "resume", raw))
        XCTAssertFalse(SessionControlPresentation.reason(target, "fork", raw).isEmpty)
        XCTAssertFalse(SessionControlPresentation.reason(target, "pause", ["activity": "idle", "state": "running", "resumable": true]).isEmpty)
        XCTAssertTrue(SessionControlPresentation.reason(target.with(agentId: "child", parentConversation: "conversation"), "pause", raw).contains("parent"))
    }
}

final class ScheduledSettingsTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private var source: SessionAction {
        SessionAction(requestId: "original", target: target, operation: "settings", arguments: ["model": "synthetic", "effort": "high"], status: "scheduled", pendingId: "pending")
    }
    private var attempt: SessionAction {
        SessionAction(requestId: "attempt", target: target, operation: "settings", arguments: ["model": "synthetic", "effort": "high", "expected_pending_id": "pending"])
    }

    func testOneAttemptPerAuthorizationSurvivesPruning() throws {
        var state = try ScheduledSettingsAttempt.begin(MessageState(sessionActions: [source]), authorized: source, attempt: attempt)
        for index in 0..<150 {
            var unrelated = attempt
            unrelated.requestId = "unrelated-\(index)"
            unrelated.status = "completed"
            state = state.action(unrelated)
        }
        XCTAssertEqual("attempt", state.sessionActions.first { $0.requestId == "original" }?.applyAttemptId)
        var second = attempt
        second.requestId = "second"
        XCTAssertThrowsError(try ScheduledSettingsAttempt.begin(state, authorized: source, attempt: second))
    }

    func testChangedPendingOrRequestedModelCannotStartAttempt() {
        var pending = attempt
        pending.arguments["expected_pending_id"] = "changed"
        XCTAssertThrowsError(try ScheduledSettingsAttempt.begin(MessageState(sessionActions: [source]), authorized: source, attempt: pending))
        var model = attempt
        model.arguments["model"] = "unseen"
        XCTAssertThrowsError(try ScheduledSettingsAttempt.begin(MessageState(sessionActions: [source]), authorized: source, attempt: model))
    }
}

final class RecoveryTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "codex/example/mobile", run: "run", conversation: "conversation", connectionId: "workspace")
    private func raw(_ job: JSONObject = [:]) -> JSONObject {
        var recovery: JSONObject = ["name": .string(target.session), "id": "job", "identity": [.string(target.run), .string(target.conversation)],
                                    "state": "waiting", "attempt": 2, "class_attempt": 1, "delays": [15, 30, -1], "due_at": 120, "not_before": 115]
        for key in job.keys { recovery[key] = job[key] }
        return ["name": .string(target.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation), "recovery": .object(recovery)]
    }

    func testNativeCountdownAndRateLimitAreDistinct() throws {
        let view = try XCTUnwrap(RecoveryPresentation.from(raw(), target, verified: true))
        XCTAssertEqual("Retry in 10 s", view.status(now: 110))
        XCTAssertFalse(view.retryTimeAllowed(now: 110))
        XCTAssertTrue(view.retryTimeAllowed(now: 115))
        XCTAssertEqual("1/2 attempts", view.attempts)
        XCTAssertFalse(view.recorded)
    }

    func testRepeatingScheduleAndAcknowledgmentAreReportedWithoutInventingSuccess() {
        let pending = RecoveryPresentation.from(raw(["delays": [15, 0], "state": "retrying", "acknowledged": false]), target, verified: true)
        XCTAssertEqual("2 attempts (repeats until stopped)", pending?.attempts)
        XCTAssertEqual("Waiting for delivery confirmation…", pending?.status(now: 0))
        XCTAssertEqual("Recovery in progress", RecoveryPresentation.from(raw(["state": "retrying", "acknowledged": true]), target, verified: true)?.status(now: 0))
    }

    func testCachedOrReusedJobIsReadOnly() {
        XCTAssertEqual(true, RecoveryPresentation.from(raw(), target, verified: false)?.recorded)
        XCTAssertEqual(true, RecoveryPresentation.from(raw(["identity": ["newrun", .string(target.conversation)]]), target, verified: true)?.recorded)
    }

    func testUnknownMetricsStayUnknownAndUnknownStateRemainsVisible() throws {
        var value = raw(["state": "custom_state", "due_at": "120"])
        var job = value.object("recovery")!
        job["attempt"] = nil
        value["recovery"] = .object(job)
        let view = try XCTUnwrap(RecoveryPresentation.from(value, target, verified: true))
        XCTAssertEqual("Attempts not reported", view.attempts)
        XCTAssertNil(view.dueAt)
        XCTAssertEqual("Custom state", view.status(now: 0))
    }

    private let job = "55555555-5555-4555-8555-555555555555"
    private let id = "66666666-6666-4666-8666-666666666666"
    private func args(_ choice: String = "now") -> JSONObject { ["job_id": .string(job), "action": .string(choice)] }
    private func waiting(_ change: JSONObject = [:]) -> JSONObject {
        var recovery: JSONObject = ["id": .string(job), "state": "waiting", "identity": [.string(target.run), .string(target.conversation), "native-model"], "not_before": 99999999]
        for key in change.keys { recovery[key] = change[key] }
        return ["recovery": .object(recovery)]
    }

    func testRecoveryOnlyAcceptsExactWaitingJobAndNativePair() {
        XCTAssertEqual("", SessionActionPolicies.inspectReason(target, "recovery_action", args(), waiting()))
        XCTAssertFalse(SessionActionPolicies.inspectReason(target, "recovery_action", args(), waiting(["id": .string(id)])).isEmpty)
        XCTAssertFalse(SessionActionPolicies.inspectReason(target, "recovery_action", args(), waiting(["state": "dispatching"])).isEmpty)
        XCTAssertFalse(SessionActionPolicies.inspectReason(target.with(conversation: "other"), "recovery_action", args(), waiting()).isEmpty)
        XCTAssertThrowsError(try SessionActionPolicies.arguments("recovery_action", args("arbitrary")))
    }

    func testRetryScheduledIsAcknowledgementOfSameJob() {
        let action = SessionAction(requestId: id, target: target, operation: "recovery_action", arguments: args())
        var native: JSONObject = ["request_id": .string(id), "name": .string(target.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation),
                                  "job_id": .string(job), "action": "now", "status": "scheduled"]
        func receipt() -> JSONObject { ["request_id": .string(id), "state": "completed", "result": .object(native)] }
        XCTAssertEqual("completed", SessionActionPolicies.result(action, receipt()).status)
        native["status"] = "executed"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(action, receipt()).status)
        native["status"] = "scheduled"
        native["job_id"] = "other"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(action, receipt()).status)
    }

    func testDelayedNativeRecoveryEvidenceChangeCancelsStaleChoice() {
        XCTAssertNotEqual(SessionActionPolicies.evidence("recovery_action", waiting(), args()),
                          SessionActionPolicies.evidence("recovery_action", waiting(["not_before": 100000000]), args()))
    }
}

final class ContextCommandTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "codex/project/session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let file = Attachment(id: "10000000-0000-4000-8000-000000000001", name: "synthetic.txt", mime: "text/plain", bytes: 4, sha256: String(repeating: "0", count: 64))
    private var original: Draft { Draft(target: target, text: "Captured message", updatedAt: 1_000, attachments: [file], revision: "revision", generation: "generation") }
    private var op: ContextOperation { ContextOperation(requestId: "compact-id", target: target, operation: "compact_context", status: "completed", createdAt: 1) }
    private func native(_ status: String = "completed", _ change: JSONObject = [:], request: JSONObject = [:]) -> JSONObject {
        var compact: JSONObject = ["request_id": "compact-id", "run_id": "run", "conversation_id": "conversation", "status": .string(status)]
        for key in request.keys { compact[key] = request[key] }
        var raw: JSONObject = ["name": .string(target.session), "run_id": "run", "conversation_id": "conversation", "activity": "idle", "phase": "idle",
                               "compact_context_request": .object(compact)]
        for key in change.keys { raw[key] = change[key] }
        return raw
    }

    func testSubmittedAcknowledgementNeverCountsAsCompletedCompaction() {
        let receipt: JSONObject = ["request_id": "compact-id", "state": "completed", "result": ["request_id": "compact-id", "name": .string(target.session),
                                                                                            "run_id": "run", "conversation_id": "conversation", "status": "submitted"]]
        var sending = op
        sending.status = "sending"
        XCTAssertEqual("submitted", ContextPolicies.result(sending, receipt).status)
        XCTAssertNil(ContextPolicies.compactStatus(op, native("submitted")))
        XCTAssertNil(ContextPolicies.compactStatus(op, native("completed", ["phase": "compacting"])))
        XCTAssertNil(ContextPolicies.compactStatus(op, native("completed", ["conversation_id": "different"])))
        XCTAssertNil(ContextPolicies.compactStatus(op, native("completed", request: ["request_id": "different"])))
        XCTAssertEqual("completed", ContextPolicies.compactStatus(op, native()))
    }

    func testMismatchedNativeOrOuterReceiptRemainsUncertain() {
        var receipt: JSONObject = ["request_id": "compact-id", "state": "completed", "result": ["request_id": "foreign", "name": .string(target.session),
                                                                                            "run_id": "run", "conversation_id": "conversation", "status": "submitted"]]
        XCTAssertEqual("uncertain", ContextPolicies.result(op, receipt).status)
        receipt["state"] = "failed"
        receipt["request_id"] = "foreign"
        XCTAssertEqual("uncertain", ContextPolicies.result(op, receipt).status)
    }

    func testInterruptedRecordsRecoverWithoutAuthorization() throws {
        var submitted = op
        submitted.status = "submitted"
        let pending = MessageState(drafts: [original]).context(submitted)
        let recovered = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(pending)).recovered()
        XCTAssertEqual("uncertain", recovered.contextOperations.first?.status)
        XCTAssertEqual([original], recovered.drafts)
        XCTAssertThrowsError(try recovered.enqueue(original, requestId: "message-id", at: 1))
    }

    func testContinuationPermitsTransportOnlyForTheClearedUnchangedComposer() throws {
        let lease = CompactContinuation(requestId: op.requestId, draft: original, navigation: 1, startedAt: 0)
        let written = try MessageState(drafts: [original], contextOperations: [op]).enqueue(original, requestId: "message-id", at: 1)
        let cleared = written.drafts[0]
        XCTAssertTrue(lease.permitsTransport(target: target, navigation: 1, now: 1, written: cleared, current: cleared))
        XCTAssertFalse(lease.permitsTransport(target: target, navigation: 2, now: 1, written: cleared, current: cleared))
        XCTAssertFalse(lease.permitsTransport(target: target, navigation: 1, now: 601, written: cleared, current: cleared))
        var typed = cleared
        typed.text = "New text"
        typed.revision = "later"
        XCTAssertFalse(lease.permitsTransport(target: target, navigation: 1, now: 1, written: cleared, current: typed))
    }

    func testClearPromotionRequiresConfirmedProofAndEmptyDestinationAndPreservesOtherRecords() {
        let clear = ContextOperation(requestId: "clear-id", target: target, operation: "clear_context", status: "completed", createdAt: 1)
        let to = target.with(conversation: "new-conversation")
        let question = Draft(target: target, text: "Answer draft", questionId: "q", questionHash: "hash")
        let outgoing = OutgoingMessage(requestId: "send-id", target: target, text: "Old sent text", attachments: [file], createdAt: 1, status: "submitted")
        let state = MessageState(drafts: [original, question], outgoing: [outgoing], contextOperations: [clear])
        var submitted = clear
        submitted.status = "submitted"
        XCTAssertEqual(state, ContextPolicies.promoteClearDraft(state, submitted, to: to))
        XCTAssertEqual(state, ContextPolicies.promoteClearDraft(state, clear, to: to.with(run: "different")))
        var occupied = state
        occupied.drafts.append(Draft(target: to, text: "Already here"))
        XCTAssertEqual(occupied, ContextPolicies.promoteClearDraft(occupied, clear, to: to))
        let moved = ContextPolicies.promoteClearDraft(state, clear, to: to)
        XCTAssertEqual(question, moved.drafts.first { $0.questionId == "q" })
        XCTAssertEqual([outgoing], moved.outgoing)
        XCTAssertEqual(to, moved.drafts.first { $0.questionId.isEmpty }?.target)
        XCTAssertTrue(ContextPolicies.clearMoved(moved, target, to))
        XCTAssertFalse(ContextPolicies.clearMoved(moved, target, to.with(conversation: "unverified")))
    }
}

/// Ported from Android SessionActionsMenuTest.
final class SessionActionsMenuTests: XCTestCase {
    private let target = Target(computerId: "00000000-0000-4000-8000-000000000001", session: "codex/project/review", run: "run", conversation: "conversation", connectionId: "workspace")
    private let supported = SessionActionPolicies.lifecycle.union(["terminal_snapshot"])
    private func row(_ allowed: String...) -> JSONObject {
        ["allowed_actions": JSON(allowed), "action_reasons": ["pause": "Wait for a confirmed idle live session before pausing"]]
    }
    private func facts(_ state: String = "running", raw: JSONObject? = nil, online: Bool = true, read: SessionMenuRead = SessionMenuRead(),
                       attention: Bool = false, target: Target? = nil) -> SessionMenuFacts {
        let t = target ?? self.target
        return SessionMenuFacts(target: t, row: raw ?? row("pause", "rename", "fork", "terminate", "forget"), state: state, archived: !t.archiveId.isEmpty, online: online,
                                supported: supported, needsAttention: attention, read: read, reference: "zerus://session?x", folder: "/work/project")
    }
    private func items(_ facts: SessionMenuFacts) -> [SessionMenuItem] { SessionActionsMenu.items(facts).flatMap { $0 } }
    private func item(_ facts: SessionMenuFacts, _ id: String) -> SessionMenuItem { items(facts).first { $0.id == id }! }

    func testLiveRunningSessionMirrorsDesktopOrder() {
        XCTAssertEqual(["terminal", "review_later", "session_notifications", "pause", "rename", "fork", "copy_name", "copy_reference", "copy_folder", "terminate"], items(facts()).map(\.id))
        XCTAssertTrue(items(facts()).filter { $0.id != SessionNotificationPreferences.operation }.allSatisfy(\.enabled))
        XCTAssertFalse(item(facts(),SessionNotificationPreferences.operation).enabled)
    }

    func testSavedSessionOffersResumeArchiveAndForgetButNotTerminate() {
        let saved = facts("paused", raw: row("resume", "archive", "rename", "fork", "forget"))
        XCTAssertEqual(["terminal", "review_later", "session_notifications", "resume", "rename", "fork", "archive", "copy_name", "copy_reference", "copy_folder", "forget"], items(saved).map(\.id))
        XCTAssertEqual("Forget saved session…", item(saved, "forget").label)
        XCTAssertEqual("Move to archive…", item(saved, "archive").label)
        XCTAssertTrue(items(facts("stopped", raw: row("resume", "archive"))).contains { $0.id == "archive" })
    }

    func testArchivedVersionHasNoTerminalOrReadItemsAndCannotPause() {
        let archived = facts("archived", raw: row("restore", "rename", "fork", "forget"), target: target.with(archiveId: "11111111-1111-4111-8111-111111111111"))
        XCTAssertEqual(["session_notifications", "restore", "rename", "fork", "copy_name", "copy_reference", "copy_folder", "forget"], items(archived).map(\.id))
        XCTAssertFalse(item(archived,SessionNotificationPreferences.operation).enabled)
        XCTAssertEqual("Forget archived version…", item(archived, "forget").label)
        XCTAssertEqual("This archived conversation is read-only.", SessionActionsMenu.operationReason(archived, "pause"))
        XCTAssertEqual("Select an archived version first.", SessionActionsMenu.operationReason(facts(), "restore"))
    }

    func testDisabledItemsExplainNativeAndPhoneGates() {
        XCTAssertEqual("Wait for a confirmed idle live session before pausing", item(facts(raw: row("rename")), "pause").reason)
        XCTAssertEqual("The machine is offline.", item(facts(online: false), "pause").reason)
        XCTAssertEqual("The machine is offline.", item(facts(online: false), "terminal").reason)
        var narrow = facts()
        narrow.supported = ["terminal_snapshot"]
        XCTAssertEqual("This gateway or computer does not support this action.", item(narrow, "rename").reason)
        var blocked = facts()
        blocked.blocked = "Unavailable in preview."
        XCTAssertEqual("Unavailable in preview.", item(blocked, "terminate").reason)
        var busy = facts()
        busy.busy = "Resolve the pending delivery or action first."
        XCTAssertEqual("Resolve the pending delivery or action first.", item(busy, "fork").reason)
        XCTAssertTrue(item(busy, "terminal").enabled)
        XCTAssertFalse(SessionActionsMenu.operationReason(facts(raw: [:]), "pause").isEmpty)
        XCTAssertEqual("Session controls belong to the parent conversation.",
                       SessionActionsMenu.operationReason(facts(target: target.with(agentId: "child", parentConversation: "conversation")), "pause"))
    }

    func testTerminalUsesNativeTerminalSupportAndAdapterCapability() {
        var unsupported = row("pause")
        unsupported["terminal_supported"] = false
        unsupported["terminal_reason"] = "Terminal requires this exact live tracked tmux pane"
        XCTAssertEqual("Terminal requires this exact live tracked tmux pane", item(facts(raw: unsupported), "terminal").reason)
        var lifecycleOnly = facts()
        lifecycleOnly.supported = SessionActionPolicies.lifecycle
        XCTAssertEqual("This native adapter does not provide remote Terminal.", item(lifecycleOnly, "terminal").reason)
        var supportedRow = row()
        supportedRow["terminal_supported"] = true
        XCTAssertTrue(item(facts(raw: supportedRow), "terminal").enabled)
    }

    func testCopyItemsNeedReportedValues() {
        var missing = facts()
        missing.reference = nil
        missing.folder = ""
        XCTAssertFalse(item(missing, "copy_reference").enabled)
        XCTAssertFalse(item(missing, "copy_folder").enabled)
        XCTAssertTrue(item(missing, "copy_name").enabled)
    }

    func testReadItemMarksOnlyWithReadAllEvidenceOtherwiseExplains() {
        func read(_ read: SessionMenuRead, attention: Bool = true) -> SessionMenuItem { SessionActionsMenu.readItem(facts(read: read, attention: attention)) }
        XCTAssertEqual(SessionMenuItem("review_later", "Review later", hint: "Keep a reminder on this phone to review this session later."), read(SessionMenuRead(), attention: false))
        XCTAssertEqual("Read state is still loading on this phone.", read(SessionMenuRead(unavailable: "Read state is still loading on this phone."), attention: false).reason)
        let complete = read(SessionMenuRead(canMark: true, replies: true))
        XCTAssertEqual("mark_read", complete.id)
        XCTAssertEqual("Mark as read", complete.label)
        XCTAssertTrue(complete.enabled)
        let partial = read(SessionMenuRead(canMark: true, partial: true, replies: true))
        XCTAssertEqual("Mark loaded replies read", partial.label)
        XCTAssertTrue(partial.enabled && !partial.hint.isEmpty)
        let reminder = read(SessionMenuRead(replies: true, reminder: true))
        XCTAssertEqual("clear_reminder", reminder.id)
        XCTAssertTrue(reminder.enabled && reminder.hint.contains("stay unread"))
        let unknown = read(SessionMenuRead(replies: true))
        XCTAssertFalse(unknown.enabled)
        XCTAssertTrue(unknown.reason.hasPrefix("Needs verified history"))
        XCTAssertTrue(read(SessionMenuRead(replies: true, checking: true)).reason.hasPrefix("Waiting for verified history"))
        XCTAssertTrue(read(SessionMenuRead()).reason.contains("covers replies only"))
        XCTAssertFalse(read(SessionMenuRead(unavailable: "Unavailable in preview.", canMark: true)).enabled)
    }
}

/// Ported from Android ConfirmedTargetResolutionTest.
final class ConfirmedTargetResolutionTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "codex/project/chat", run: "run", conversation: "conversation", connectionId: "workspace")

    func testMembershipDelayUsesOnlyReadRetriesAndPreservesOriginalDraftUntilProof() async throws {
        let original = Draft(target: target, text: "unsent text")
        let destination = target.with(session: "codex/project/renamed")
        let action = SessionAction(requestId: "id", target: target, operation: "rename", arguments: [:], status: "completed", resultTarget: destination)
        var state = MessageState(drafts: [original]).action(action)
        var reads = 0, pauses = 0
        let proof = try await ConfirmedTargetResolution.read(active: { true }, pause: {
            pauses += 1
            XCTAssertEqual([original], state.drafts)
        }, inspect: { () throws -> Target in
            reads += 1
            if reads < 3 { throw URLError(.timedOut) }
            return destination
        })
        XCTAssertEqual(3, reads)
        XCTAssertEqual(2, pauses)
        state = ActionDraftPolicies.promote(state, action)
        XCTAssertEqual(proof, state.drafts.first?.target)
        XCTAssertEqual(original.text, state.drafts.first?.text)
    }

    func testExplicitConfirmedRestoreAcceptsEmptyEditorButPreservesOccupiedAndChangedDraft() {
        let to = target.with(session: "codex/project/renamed")
        let action = SessionAction(requestId: "id", target: target, operation: "rename", arguments: [:], status: "completed", resultTarget: to)
        let draft = Draft(target: target, text: "saved text")
        let state = MessageState(drafts: [draft, Draft(target: to, text: "")], sessionActions: [action])
        let restored = ActionDraftPolicies.restore(state, action, draft)
        XCTAssertEqual(1, restored.drafts.count)
        XCTAssertEqual(to, restored.drafts.first?.target)
        XCTAssertEqual(draft.generation, restored.drafts.first?.generation)
        var occupied = state
        occupied.drafts = [draft, Draft(target: to, text: "already typed")]
        XCTAssertEqual(occupied, ActionDraftPolicies.restore(occupied, action, draft))
        var changedDraft = draft
        changedDraft.revision = "new"
        changedDraft.text = "later edit"
        var changed = state
        changed.drafts = [changedDraft]
        XCTAssertEqual(changed, ActionDraftPolicies.restore(changed, action, draft))
    }

    func testNavigationCancelsFurtherReads() async {
        var active = true, reads = 0
        do {
            _ = try await ConfirmedTargetResolution.read(active: { active }, pause: { active = false }, inspect: { () throws -> Int in
                reads += 1
                throw URLError(.notConnectedToInternet)
            })
            XCTFail("Expected cancellation")
        } catch is CancellationError { XCTAssertEqual(1, reads) } catch { XCTFail("Unexpected \(error)") }
    }

    func testOfflineOutcomeRemainsConfirmedWithoutMutationReplay() async {
        var reads = 0
        do {
            _ = try await ConfirmedTargetResolution.read(active: { true }, pause: {}, inspect: { () throws -> Int in
                reads += 1
                throw URLError(.notConnectedToInternet)
            })
            XCTFail("Expected unavailable result")
        } catch is URLError { XCTAssertEqual(4, reads) } catch { XCTFail("Unexpected \(error)") }
    }
}
