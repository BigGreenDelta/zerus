import XCTest
@testable import ZerusKit

/// Ported from Android ContextPresentationTest and the cache parts of QuestionSendConfirmationTest.
final class ContextPresentationTests: XCTestCase {
    private let now = 1800000000.0
    private func view(_ raw: String = "{}", recorded: Bool = false) -> ContextView {
        ContextPresentation.from(object(raw), recorded: recorded, now: now, locale: Locale(identifier: "en_US"), zone: TimeZone(identifier: "UTC")!)
    }
    private func raw(_ used: JSON, _ limit: JSON? = nil) -> String {
        var context: JSONObject = ["used": used]
        if let limit { context["limit"] = limit }
        return JSON.object(["session_usage": ["status": "ok", "context": .object(context)]]).compact
    }
    private let live = #"{"run_id":"run","conversation_id":"conversation","state":"running","runtime_state":"live","process_state":"running","phase":"idle","activity":"idle"}"#
    private func live(_ extra: String) -> String { String(live.dropLast()) + "," + extra + "}" }

    func testMissingUsageAndCacheNeverInventMetricsOrWarmth() {
        let result = view()
        XCTAssertEqual("Context: Not reported", result.summary)
        XCTAssertEqual("Cache: Unknown", result.cacheSummary)
        XCTAssertEqual("Not reported", result.value("Context window"))
        XCTAssertFalse(result.cold)
        XCTAssertEqual(.muted, result.tone)
    }

    func testOnlyNativeFiniteNonnegativeNumbersCountAndZeroIsValid() {
        for invalid: JSON in ["123", -1, 9007199254740992.0] {
            XCTAssertEqual("Context: Not reported", view(raw(invalid, 1000)).summary)
        }
        let zero = view(raw(0, 1000))
        XCTAssertEqual("Context 0% (0/1k)", zero.summary)
        XCTAssertEqual("0", zero.value("Context tokens"))
        XCTAssertEqual(.normal, zero.tone)
        // A mathematically overflowing ratio cannot become a fabricated infinity percentage.
        XCTAssertEqual(.muted, view(raw(9007199254740991.0, 1e-308)).tone)
    }

    func testUnknownWindowCannotBeInferredFromProviderOrModel() {
        let result = view(#"{"session_usage":{"status":"ok","context":{"used":12345}},"agent":"codex","model":"model-with-an-unknown-window"}"#)
        XCTAssertEqual("Context 12.3k / Unknown window", result.summary)
        XCTAssertEqual("Not reported", result.value("Window used"))
        XCTAssertEqual(.muted, result.tone)
        XCTAssertTrue(result.notes.contains { $0.contains("Percentage is unavailable") })
    }

    func testThresholdsAreDesktopSeverityAndDoNotClaimNativeCompactionRequired() {
        XCTAssertEqual(.normal, view(raw(699, 1000)).tone)
        let growing = view(raw(700, 1000))
        XCTAssertEqual(.warning, growing.tone)
        XCTAssertTrue(growing.notes.contains { $0.hasPrefix("Context is growing") })
        let full = view(raw(900, 1000))
        XCTAssertEqual(.critical, full.tone)
        XCTAssertTrue(full.notes.contains { $0.hasPrefix("Context is nearly full") })
        XCTAssertEqual("", full.compactionSummary)
        XCTAssertEqual(.muted, view(raw(900, 1000), recorded: true).tone)
    }

    func testEstimatedUsageAndNativeModelConfigurationAreExplicit() {
        let result = view(#"{"session_usage":{"status":"ok","context":{"used":1500,"limit":2000,"estimated":true,"limit_source":"model_config"}}}"#)
        XCTAssertEqual("Context ≈75% (1.5k/2k)", result.summary)
        XCTAssertEqual("Approx. 1,500", result.value("Context tokens"))
        XCTAssertEqual("75.0%", result.value("Window used"))
        XCTAssertEqual("Native model configuration", result.value("Window source"))
    }

    func testLastRequestCacheHitsNeverProveCurrentWarmthOrExpiry() {
        let result = view(#"{"session_usage":{"status":"ok","prompt_cache":{"status":"unknown","source":"native_usage"},"last_request":{"input":100000,"cache_read":95000,"output":300}}}"#)
        XCTAssertEqual("Cache: Unknown", result.cacheSummary)
        XCTAssertFalse(result.cold)
        XCTAssertEqual("95,000", result.value("Last request cache read"))
        XCTAssertEqual("100,000", result.value("Last request input"))
        XCTAssertEqual("Not reported", result.value("Estimated cache expiry"))
        XCTAssertTrue(result.notes.contains { $0.contains("No countdown") })
    }

    func testWarmNativeDeadlineHasMinuteCountdownAndExactExpiryCrossing() {
        var data = object(#"{"session_usage":{"prompt_cache":{"status":"warm","ttl_seconds":300}}}"#)
        func setCache(_ key: String, _ value: JSON?) {
            var usage = data.object("session_usage")!
            var cache = usage.object("prompt_cache")!
            cache[key] = value
            usage["prompt_cache"] = .object(cache)
            data["session_usage"] = .object(usage)
        }
        setCache("expires_at", .double(now + 121))
        setCache("observed_at", .double(now - 179))
        let text = JSON.object(data).compact
        XCTAssertEqual("Cache ~3m", view(text).cacheSummary)
        XCTAssertEqual(3, ContextPresentation.countdownMinute(data, now: now))
        XCTAssertEqual(2, ContextPresentation.countdownMinute(data, now: now + 1))
        XCTAssertEqual(1, ContextPresentation.countdownMinute(data, now: now + 120))
        XCTAssertFalse(ContextPresentation.isCold(data, now: now + 120))
        XCTAssertTrue(ContextPresentation.isCold(data, now: now + 121))
        XCTAssertEqual(0, ContextPresentation.countdownMinute(data, now: now + 121))
        XCTAssertEqual("300 s", view(text).value("Reported cache TTL"))
        setCache("expires_at", nil)
        XCTAssertEqual("Warm cache / Expiry unknown", view(JSON.object(data).compact).cacheSummary)
        XCTAssertFalse(ContextPresentation.isCold(data, now: now + 100000))
        XCTAssertNil(ContextPresentation.countdownMinute(data, now: now))
    }

    func testSavingHintDoesNotProveColdButExplicitNativeColdOutranksWarmEstimate() {
        let saving = #"{"cache_hint":{"status":"saving_hint","tokens":50000},"session_usage":{"prompt_cache":{"status":"warm"}}}"#
        XCTAssertFalse(ContextPresentation.isCold(object(saving), now: now))
        XCTAssertTrue(view(saving).notes.contains { $0.contains("does not prove the cache is cold") })
        let result = view(#"{"cache_hint":{"status":"cold","tokens":50000},"session_usage":{"prompt_cache":{"status":"warm","expires_at":\#(now + 300)}}}"#)
        XCTAssertTrue(result.cold)
        XCTAssertEqual("Cold cache", result.cacheSummary)
        XCTAssertEqual("Cold (reported by native agent)", result.value("Provider cache"))
        XCTAssertEqual(0xFFFF9CA8, ContextPresentation.cacheColor(result.cacheTone))
    }

    func testStaleAndArchivedValuesRemainReadableButCannotOfferActions() {
        let supported = live(#""compact_context_supported":true,"clear_context_supported":true"#)
        let current = view(supported)
        XCTAssertEqual("", current.compactReason)
        XCTAssertEqual("", current.clearReason)
        let recorded = view(supported, recorded: true)
        XCTAssertTrue(recorded.compactReason.contains("Refresh"))
        XCTAssertTrue(recorded.notes.contains { $0.hasPrefix("Last known") })
        let archived = view(supported.replacingOccurrences(of: #""state":"running""#, with: #""state":"archived","archive_id":"saved-archive""#))
        XCTAssertTrue(archived.clearReason.contains("read-only"))
    }

    func testUnavailableActionsExplainKnownBusyStateWithoutProviderGuesses() {
        let busy = live.replacingOccurrences(of: #""phase":"idle","activity":"idle""#, with: #""phase":"working","activity":"busy""#)
        XCTAssertEqual("Wait until the session is idle.", view(busy).compactReason)
        XCTAssertEqual("Native compaction is not available for this session.", view(live).compactReason)
        XCTAssertEqual("Native provider-specific reason", view(live(#""compact_context_reason":"Native provider-specific reason""#)).compactReason)
    }

    func testOnlyMatchingNativeCompactionReceiptDescribesThisConversation() {
        func request(_ status: String, _ conversation: String = "conversation") -> String {
            #""compact_context_request":{"run_id":"run","conversation_id":"\#(conversation)","status":"\#(status)"}"#
        }
        let submitted = view(live(request("submitted")))
        XCTAssertEqual("Compaction requested", submitted.compactionSummary)
        XCTAssertFalse(submitted.compacting)
        XCTAssertEqual("Last compaction completed", view(live(request("completed"))).compactionSummary)
        let foreign = live(request("completed", "another-conversation"))
        XCTAssertEqual("", view(foreign).compactionSummary)
        let compacting = foreign.replacingOccurrences(of: #""phase":"idle","activity":"idle""#, with: #""phase":"compacting","activity":"busy","compaction_started":\#(now - 10)"#)
        XCTAssertTrue(view(compacting).compacting)
        XCTAssertFalse(view(compacting, recorded: true).compacting)
        XCTAssertFalse(view(compacting.replacingOccurrences(of: #""runtime_state":"live""#, with: #""runtime_state":"saved""#)).compacting)
    }

    func testUsageDetailsRetainNativeZeroCostTotalsAndIncompleteEvidence() {
        let result = view(#"{"session_usage":{"status":"ok","model":"native-model","totals":{"total":0,"input":0,"cache_read":0,"output":0},"requests":0,"cost_usd":0,"cost_incomplete":true,"partial":true,"extras":{"api_ms":1200,"tool_ms":500}}}"#)
        XCTAssertEqual("native-model", result.value("Model"))
        XCTAssertEqual("0", result.value("Total tokens"))
        XCTAssertEqual("0", result.value("Recorded requests"))
        XCTAssertEqual("$0.0000", result.value("Native cost estimate"))
        XCTAssertEqual("1.2 s", result.value("API time"))
        XCTAssertTrue(result.notes.contains { $0.contains("incomplete cost") })
        XCTAssertTrue(result.notes.contains { $0.contains("Totals are incomplete") })
    }

    func testNativeCacheHintAndElapsedWarmTtlRequireConsentWhileUnknownCacheDoesNot() {
        let nativeCold = object(#"{"cache_hint":{"status":"cold"}}"#)
        let expires = object(#"{"session_usage":{"prompt_cache":{"status":"warm","expires_at":100}}}"#)
        XCTAssertTrue(ContextPresentation.isCold(nativeCold, now: 100))
        XCTAssertTrue(ContextPresentation.isCold(expires, now: 100))
        XCTAssertFalse(ContextPresentation.isCold(expires, now: 99))
        XCTAssertFalse(ContextPresentation.isCold(JSONObject(), now: 100))
    }
}

/// Ported from Android ConversationSearchTest, SelectedSessionPresentationTest, ConversationLoadPresentationTest,
/// ComposerIdentityTest, AttachmentSelectionTest and the pure parts of PersistenceRaceTest.
final class ConversationPoliciesTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "codex/example/mobile", run: "run", conversation: "conversation", connectionId: "workspace")

    // MARK: Search

    func testRepeatedTextKeepsExactMessageIdentityWithoutGuessing() {
        let events = [Event(id: "one", role: "Agent", text: "same words", time: ""), Event(id: "two", role: "Agent", text: "same words", time: "")]
        let result = ConversationSearch.find(ConversationSearch.prepare(events), "SAME")
        XCTAssertEqual(["one", "two"], result.hits.map(\.eventId))
        XCTAssertEqual(events[1], ConversationSearch.resolve(events, "two"))
        XCTAssertNil(ConversationSearch.resolve(events, "missing"))
    }

    func testFullKnownQuestionAnswerIsSearchableBeyondListPreview() {
        let answer = String(repeating: "x", count: 400) + " important conclusion"
        let raw = "<send_user_message_question_reply>" + JSON.object(["questionItemId": "part", "question": "Which option?", "answer": .string(answer)]).compact +
            "</send_user_message_question_reply>"
        let event = Event(id: "answer", role: "You", text: raw, time: "", requestId: "request")
        let prepared = ConversationSearch.prepare([event])
        XCTAssertEqual("You (answer)", prepared.first?.role)
        XCTAssertEqual(["answer"], ConversationSearch.find(prepared, "important conclusion").hits.map(\.eventId))
        XCTAssertEqual(raw, event.text)
    }

    func testBoundedResultsDoNotInventFullHistoryTotals() {
        let messages = (0..<130).map { SearchableMessage(eventId: "id\($0)", role: "Agent", text: "word", at: 0) }
        let result = ConversationSearch.find(messages, "word")
        XCTAssertEqual(130, result.matches)
        XCTAssertEqual(100, result.hits.count)
        XCTAssertEqual(0, ConversationSearch.find(messages, "").matches)
    }

    func testSnippetsAreBoundedAndMarkTruncation() {
        let text = String(repeating: "a", count: 100) + "needle" + String(repeating: "b", count: 300)
        let hit = ConversationSearch.find([SearchableMessage(eventId: "id", role: "Agent", text: text, at: 0)], "needle").hits[0]
        XCTAssertTrue(hit.snippet.hasPrefix("…"))
        XCTAssertTrue(hit.snippet.hasSuffix("…"))
        XCTAssertEqual(182, hit.snippet.count)
        XCTAssertTrue(hit.snippet.contains("needle"))
    }

    // MARK: Selected session

    private var selected: Session {
        Session(target: target, title: "Mobile", agent: "codex", status: "", project: "example", preview: "", pendingCount: 0, raw: ["name": .string(target.session)])
    }
    private func inspected(_ extra: JSONObject = [:]) -> JSONObject {
        var raw: JSONObject = ["name": .string(target.session), "run_id": "run", "conversation_id": "conversation", "activity": "idle", "tracked": true, "state": "running"]
        for key in extra.keys { raw[key] = extra[key] }
        return raw
    }

    func testOlderInspectionCannotResurrectSettledWaitOrDropNewerWait() {
        func wait(revision: Int64, count: Int64) -> JSON {
            ["schema": 1, "status": "ok", "revision": .int(revision), "run_id": "run", "conversation_id": "conversation", "count": .int(count)]
        }
        var current = selected
        current.raw = inspected(["coordination_wait": wait(revision: 3, count: 0)])
        XCTAssertEqual(0, SelectedSessionPresentation.session(current, inspected(["coordination_wait": wait(revision: 2, count: 1)])).raw.object("coordination_wait")?.int("count"))
        var awaiting = selected
        awaiting.raw = inspected(["coordination_wait": wait(revision: 3, count: 1)])
        XCTAssertEqual(1, SelectedSessionPresentation.session(awaiting, inspected()).raw.object("coordination_wait")?.int("count"))
        XCTAssertEqual(0, SelectedSessionPresentation.session(awaiting, inspected(["coordination_wait": wait(revision: 4, count: 0)])).raw.object("coordination_wait")?.int("count"))
    }

    func testExactCachedInspectionPreservesReadyInsteadOfInventingUntracked() {
        XCTAssertEqual("Ready", SelectedSessionPresentation.status(selected, inspected()))
        XCTAssertEqual(.neutral, DesktopSessionPalette.badge(SelectedSessionPresentation.session(selected, inspected()), true).kind)
    }

    func testAnotherRunOrConversationCannotSupplyStatus() {
        XCTAssertEqual("Status not reported", SelectedSessionPresentation.status(selected, inspected(["run_id": "other"])))
        XCTAssertEqual("Status not reported", SelectedSessionPresentation.status(selected, inspected(["conversation_id": "other"])))
    }

    func testKnownUntrackedAndWorkingStillUseNativeSemantics() {
        XCTAssertEqual("Not tracked", SelectedSessionPresentation.status(selected, inspected(["tracked": false, "activity": ""])))
        XCTAssertEqual("Working", SelectedSessionPresentation.status(selected, inspected(["activity": "busy"])))
    }

    func testArchivedStatusNeverComesFromLiveInspection() {
        var archive = selected
        archive.target.archiveId = "archive"
        XCTAssertEqual(archive, SelectedSessionPresentation.session(archive, inspected()))
    }

    // MARK: Loading

    private func status(_ content: Bool, _ detail: Bool, _ detailProgress: Bool, _ history: Bool, _ historyProgress: Bool, _ demo: Bool,
                        resolved: Bool = false, opening: Bool = false) -> ConversationLoadPresentation.Status? {
        ConversationLoadPresentation.status(hasContent: content, detailLoading: detail, detailProgress: detailProgress, historyLoading: history,
                                            historyProgress: historyProgress, demo: demo, resolved: resolved, opening: opening)
    }

    func testOpeningCachedHistoryIsVisibleImmediatelyAcrossBothReadStages() {
        let inspection = status(true, true, false, false, false, false, opening: true)
        let history = status(true, false, false, true, false, false, resolved: true, opening: true)
        for item in [inspection, history] {
            XCTAssertEqual(false, item?.initial)
            XCTAssertEqual(true, item?.spinner)
            XCTAssertEqual("Updating messages…", item?.text)
        }
        XCTAssertNil(status(true, false, true, false, true, false, opening: true))
        // Once this opening has settled, fast background polls stay quiet.
        XCTAssertNil(status(true, true, false, false, false, false, opening: false))
        // A conversation without cached content keeps its initial placeholder.
        XCTAssertEqual("Loading conversation…", status(false, true, false, false, false, false, opening: true)?.text)
    }

    func testResolvedEmptyConversationStaysEmptyAcrossRepeatedBackgroundPolls() {
        for _ in 0..<4 {
            XCTAssertNil(status(false, true, false, false, false, false, resolved: true))
            XCTAssertNil(status(false, false, false, true, false, false, resolved: true))
            XCTAssertNil(status(false, false, false, false, false, false, resolved: true))
        }
        let explicit = status(false, true, true, false, false, false, resolved: true)
        XCTAssertEqual(false, explicit?.initial)
        XCTAssertEqual(true, explicit?.spinner)
        XCTAssertEqual("Refreshing conversation…", explicit?.text)
        XCTAssertEqual("Loading message history…", status(false, false, false, true, true, false, resolved: true)?.text)
        XCTAssertEqual(true, status(false, true, false, false, false, false)?.initial)
    }

    func testInitialLoadIsExplicitBeforeTheDelayedSpinner() {
        var time = 0.0
        let gate = LoadingGate(now: { time })
        _ = gate.begin("conversation-a")
        let initial = status(false, true, gate.visible(), false, false, false)
        XCTAssertEqual(true, initial?.initial)
        XCTAssertEqual(false, initial?.spinner)
        time = 5
        XCTAssertEqual(true, status(false, true, gate.visible(), false, false, false)?.spinner)
    }

    func testCachedContentUsesAnchoredStatusAndNeverTheInitialPlaceholder() {
        XCTAssertNil(status(true, true, false, false, false, false))
        let cached = status(true, true, true, false, false, false)
        XCTAssertEqual(false, cached?.initial)
        XCTAssertEqual(true, cached?.spinner)
        XCTAssertNil(status(true, false, true, true, false, false))
        let history = status(true, false, false, true, true, false)
        XCTAssertEqual(false, history?.initial)
        XCTAssertNotEqual(cached?.text, history?.text)
    }

    func testCompletionOrFailureRemovesPlaceholderAndStaleProgress() {
        XCTAssertNil(status(false, false, true, false, true, false))
        XCTAssertNil(status(true, false, true, false, true, false))
        XCTAssertNil(status(false, true, true, true, true, true))
    }

    func testReadAllPartialIsASubsetAndSkippedRowsAreNeverClaimedAsRead() {
        let result = ConversationLoadPresentation.readAllOutcome(marked: 3, skipped: 2, partial: 1)
        XCTAssertTrue(result.hasPrefix("3 conversations marked read"))
        XCTAssertTrue(result.contains("1 marked loaded replies only"))
        XCTAssertTrue(result.contains("2 could not be checked"))
        XCTAssertTrue(ConversationLoadPresentation.readAllOutcome(marked: 0, skipped: 2, partial: 0).hasPrefix("No conversations marked read"))
    }

    // MARK: Composer identity

    func testParentAndDistinctChildrenNeverShareEditorLane() {
        var child = target
        child.agentId = "child"
        child.parentConversation = target.conversation
        child.conversation = "conversation/child"
        XCTAssertNotEqual(ComposerIdentity.lane(target), ComposerIdentity.lane(child))
        var second = child
        second.agentId = "second"
        XCTAssertNotEqual(ComposerIdentity.lane(child), ComposerIdentity.lane(second))
        var reparented = child
        reparented.parentConversation = "new-parent"
        XCTAssertNotEqual(ComposerIdentity.lane(child), ComposerIdentity.lane(reparented))
    }

    func testInitialParentConversationPromotionPreservesEditor() {
        let initial = target.with(conversation: "")
        XCTAssertEqual(ComposerIdentity.lane(initial), ComposerIdentity.lane(initial.with(conversation: "created")))
        XCTAssertNotEqual(ComposerIdentity.lane(initial), ComposerIdentity.lane(initial.with(run: "reused")))
        var workspace = initial
        workspace.connectionId = "other-workspace"
        XCTAssertNotEqual(ComposerIdentity.lane(initial), ComposerIdentity.lane(workspace))
    }

    // MARK: Attachment selection and draft changes

    private let file = Attachment(id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "fixture.txt", mime: "text/plain", bytes: 1, sha256: String(repeating: "0", count: 64))
    private let request = "11111111-1111-4111-8111-111111111111"

    private func pending() -> MessageState {
        let origin = Draft(target: target, text: "Original text", attachments: [file])
        var latest = origin
        latest.text = "Latest text"
        latest.revision = "newer"
        let unrelated = Draft(target: target.with(session: "codex/fixture/other"), text: "Another saved draft")
        return MessageState(drafts: [latest, unrelated],
                            outgoing: [OutgoingMessage(requestId: request, target: unrelated.target, text: "Pending delivery", attachments: [file], createdAt: 1, status: "uncertain")],
                            pickerDraft: origin, pickerId: "selection")
    }
    private func cleared(_ state: MessageState, keepId: Bool = false) -> MessageState {
        var copy = state
        copy.pickerDraft = nil
        if !keepId { copy.pickerId = "" }
        return copy
    }

    func testColdRestartRetiresOnlyTheOrphanedMarkerAndPreservesNewestDraftAndOutbox() throws {
        let restored = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(pending()))
        let recovered = AttachmentSelection.recover(restored, "selection", launchedId: "", filesBusy: false)
        XCTAssertEqual(cleared(restored), recovered)
        XCTAssertEqual("Latest text", recovered.drafts.first?.text)
        XCTAssertEqual([file], recovered.drafts.first?.attachments)
        XCTAssertEqual("uncertain", recovered.outgoing.first?.status)
    }

    func testExactRestoredLaunchRetainsItsOriginalResultDestination() {
        let state = pending()
        XCTAssertEqual(state, AttachmentSelection.recover(state, "selection", launchedId: "selection", filesBusy: false))
        XCTAssertEqual(cleared(state), AttachmentSelection.recover(state, "selection", launchedId: "another-launch", filesBusy: false))
        XCTAssertEqual(state, AttachmentSelection.recover(state, "selection", launchedId: "", filesBusy: true))
    }

    func testDelayedRecoveryAndCancelCannotClearANewerSelection() {
        var newer = pending()
        newer.pickerId = "new-selection"
        XCTAssertEqual(newer, AttachmentSelection.recover(newer, "selection", launchedId: "", filesBusy: false))
        XCTAssertEqual(newer, AttachmentSelection.cancel(newer, "selection"))
        XCTAssertEqual(cleared(newer), AttachmentSelection.cancel(newer, "new-selection"))
    }

    func testLegacyMissingSelectionIdAndMissingOrigin() {
        var legacy = pending()
        legacy.pickerId = ""
        XCTAssertEqual(cleared(legacy, keepId: true), AttachmentSelection.recover(legacy, "", launchedId: "", filesBusy: false))
        var incomplete = pending()
        incomplete.pickerDraft = nil
        XCTAssertEqual(cleared(incomplete), AttachmentSelection.recover(incomplete, "selection", launchedId: "selection", filesBusy: false))
        let settled = cleared(pending())
        XCTAssertEqual(settled, AttachmentSelection.cancel(settled, ""))
    }

    func testLateTypingCannotRestoreAnEditingQuestionOverItsLockedDelivery() {
        let question = Draft(target: target, text: "Answer", questionId: "question", questionHash: "hash")
        let locked = question.begin()
        var late = question
        late.text = "Late edit"
        late.revision = "late"
        XCTAssertEqual([locked], MessageChanges.text(MessageState(drafts: [locked]), late).drafts)
    }

    func testAcknowledgedSendUsesWrittenGenerationAndPreservesNewFileOwnership() throws {
        let original = Draft(target: target, text: "First", attachments: [file])
        let initial = MessageState(drafts: [original])
        let written = try initial.enqueue(original, requestId: request, at: 1)
        var newFile = file
        newFile.id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
        var edited = original
        edited.text = "Next"
        edited.attachments = [file, newFile]
        edited.revision = "next"
        let result = MessageChanges.enqueueAcknowledged(MessageState(drafts: [edited]), original: original, written: written, requestId: request)
        XCTAssertEqual(written.drafts.first?.generation, result.drafts.first?.generation)
        XCTAssertEqual("Next", result.drafts.first?.text)
        XCTAssertEqual([newFile], result.drafts.first?.attachments)
        XCTAssertEqual(written.outgoing, result.outgoing)
    }

    func testReplacementAfterSelectionKeepsOriginalFilesAsDetachedRecovery() throws {
        let origin = Draft(target: target, text: "Original")
        let initial = MessageState(drafts: [origin], pickerDraft: origin, pickerId: "selection")
        let written = try initial.selectionResult(origin, [file])
        let replacement = Draft(target: target, text: "Replacement composition")
        let replaced = MessageChanges.draft(initial, replacement)
        let final = MessageChanges.selectionAcknowledged(replaced, origin: origin, written: written, addedIds: [file.id], selectionId: "selection")
        XCTAssertEqual([replacement], final.drafts.filter { $0.detachedId.isEmpty })
        XCTAssertTrue(final.drafts.contains { !$0.detachedId.isEmpty && $0.attachments.contains(file) })
        XCTAssertNil(final.pickerDraft)
    }

    func testDiscardCapturedBeforeSelectionCannotEraseItsUnseenCommittedFiles() throws {
        let origin = Draft(target: target, text: "Original")
        let initial = MessageState(drafts: [origin], pickerDraft: origin, pickerId: "selection")
        let written = try initial.selectionResult(origin, [file])
        let final = MessageChanges.selectionAcknowledged(MessageChanges.discard(initial, origin), origin: origin, written: written,
                                                         addedIds: [file.id], selectionId: "selection")
        XCTAssertTrue(final.drafts.contains { $0.attachments.contains(file) })
        XCTAssertFalse(final.drafts.contains { $0.key == origin.key && $0.revision == origin.revision })
    }

    func testCancelDuringSelectionKeepsCommittedAttachmentOwnership() throws {
        let origin = Draft(target: target, text: "Keep latest text")
        let initial = MessageState(drafts: [origin], pickerDraft: origin, pickerId: "selection")
        let written = try initial.selectionResult(origin, [file])
        let final = MessageChanges.selectionAcknowledged(AttachmentSelection.cancel(initial, "selection"), origin: origin, written: written,
                                                         addedIds: [file.id], selectionId: "selection")
        XCTAssertTrue(final.drafts.contains { $0.attachments.contains(file) })
        XCTAssertNil(final.pickerDraft)
    }

    func testCanceledPickerCannotAttachToTheDraft() {
        let origin = Draft(target: target, text: "Original")
        XCTAssertThrowsError(try MessageState(drafts: [origin]).selectionResult(origin, [file]))
    }
}

/// Ported from Android SessionDetailsPresentationTest.
final class SessionDetailsTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "provider/project/tag", run: "run", conversation: "conversation", connectionId: "workspace")
    private func task(_ run: String) -> JSON { ["run_id": .string(run), "source": "plan", "items": [["id": "task", "title": "Native task", "status": "in_progress"]]] }
    private let model = #"{"model":"native-model","effort":"high","settings_change_supported":true,"settings_apply_when":"now","model_options":[{"id":"native-model","label":"Native model","effort_options":["low","high"]}]}"#

    func testDetailsExposeExactIdentityIncludingArchives() {
        var archived = target
        archived.archiveId = "archive-one"
        let view = SessionDetailsPresentation.parse(nil, archived, verified: false)
        XCTAssertEqual("conversation", view.fields.first { $0.label == "Session ID" }?.value)
        XCTAssertEqual("archive-one", view.fields.first { $0.label == "Archive ID" }?.value)
        XCTAssertFalse(view.fields.contains { $0.label == "Run ID" })
    }

    func testTaskOwnersUseNativeRosterAndWrongRunIsLastRecorded() {
        let raw: JSON = ["task_lists": ["agent:child": task(target.run), "main": task("old")], "subagents": ["child": ["label": "Named child"]]]
        let view = SessionDetailsPresentation.parse(raw.object, target, verified: true)
        XCTAssertEqual("main", view.tasks.first?.owner)
        XCTAssertEqual(true, view.tasks.first?.recorded)
        XCTAssertEqual("Named child", view.tasks[1].label)
        XCTAssertFalse(view.tasks[1].recorded)
        XCTAssertEqual("In progress", SessionDetailsPresentation.taskStatus(view.tasks[1].tasks[0].status))
    }

    func testCachedAndArchivedTasksCannotAppearCurrent() {
        let raw: JSON = ["task_lists": ["main": task(target.run)]]
        XCTAssertEqual(true, SessionDetailsPresentation.parse(raw.object, target, verified: false).tasks.first?.recorded)
        var archived = target
        archived.archiveId = "archive"
        XCTAssertEqual(true, SessionDetailsPresentation.parse(raw.object, archived, verified: true).tasks.first?.recorded)
    }

    func testNativeReadyDoesNotCompleteActiveGoal() {
        let raw = object(#"{"activity":"idle","phase":"idle","goal":{"objective":"Finish native objective","status":"active","tokens_used":0,"token_budget":1000,"time_used_seconds":61}}"#)
        let view = SessionDetailsPresentation.parse(raw, target, verified: true)
        XCTAssertEqual("Pursuing goal", view.goal.first { $0.label == "Status" }?.value)
        XCTAssertEqual("0 / 1000", view.goal.first { $0.label == "Tokens" }?.value)
        XCTAssertEqual("1m", view.goal.first { $0.label == "Recorded active time" }?.value)
        XCTAssertTrue(view.goalNotice.contains("does not complete"))
    }

    func testMalformedGoalMetricsNeverBecomeNumbers() {
        let raw = object(#"{"goal":{"objective":"Goal","status":"mystery","tokens_used":"12","time_used_seconds":-1,"token_budget":999}}"#)
        let view = SessionDetailsPresentation.parse(raw, target, verified: true)
        XCTAssertEqual("Goal status unknown (mystery)", view.goal.first { $0.label == "Status" }?.value)
        XCTAssertFalse(view.goal.contains { ["Tokens", "Recorded active time"].contains($0.label) })
    }

    func testHookProfilesRemainGroupsNotIndividualAgents() {
        let raw = object(#"{"subagent_source":"hook_profiles","subagent_groups":{"reviewer":{"label":"Reviewer profile","active_count":2,"total_count":4}},"subagents":{"fake-individual":{"state":"input"}}}"#)
        let view = SessionDetailsPresentation.parse(raw, target, verified: true)
        XCTAssertEqual(["reviewer"], view.agents.map(\.id))
        XCTAssertEqual(true, view.agents.first?.group)
        XCTAssertEqual(2, view.agents.first?.active)
        XCTAssertTrue(view.agentNotice.contains("Individual agent identities are not reported"))
    }

    func testKnownChildIdentityAndStateRemainNative() throws {
        let raw = object(#"{"subagents":{"exact-child":{"name":"Worker","state":"finished","run_id":"run"}}}"#)
        let child = try XCTUnwrap(SessionDetailsPresentation.parse(raw, target, verified: true).agents.first)
        XCTAssertEqual("exact-child", child.id)
        XCTAssertEqual("Ready", SessionDetailsPresentation.agentStatus(child.state))
        XCTAssertFalse(child.group)
        XCTAssertFalse(child.recorded)
    }

    func testProcessIdentityAndCapabilitiesDoNotInferStopFromRunning() throws {
        let raw = object(#"{"generation":"opaque-generation","processes":{"items":[{"id":"exact","run_id":"run","conversation_id":"old","status":"running","capabilities":{"output":true,"stop":false}}],"complete":false}}"#)
        let view = SessionDetailsPresentation.parse(raw, target, verified: true)
        let process = try XCTUnwrap(view.processes.first)
        XCTAssertTrue(process.stale)
        XCTAssertTrue(process.output)
        XCTAssertFalse(process.stop)
        XCTAssertEqual("opaque-generation", view.generation)
        XCTAssertFalse(view.processComplete)
    }

    func testSettingsSnapshotIgnoresHistoryAndTimestampPolls() {
        let changed = String(model.dropLast()) + #","last_event_at":123,"journal":[{"detail":"next event"}]}"#
        XCTAssertEqual(SessionDetailsPresentation.settings(object(model)).signature, SessionDetailsPresentation.settings(object(changed)).signature)
    }

    func testSettingsNoticeAndPendingIdentityInvalidateSnapshot() {
        let before = SessionDetailsPresentation.settings(object(model))
        XCTAssertEqual("", before.notice)
        let noticed = SessionDetailsPresentation.settings(object(String(model.dropLast()) + #","settings_change_notice":"Claude re-reads the full history on the next message."}"#))
        XCTAssertEqual("Claude re-reads the full history on the next message.", noticed.notice)
        XCTAssertNotEqual(before.signature, noticed.signature)
        let pending = String(model.dropLast()) + #","pending_settings_id":"new-intent","pending_model":"native-model","pending_effort":"low"}"#
        XCTAssertNotEqual(before.signature, SessionDetailsPresentation.settings(object(pending)).signature)
        let catalog = model.replacingOccurrences(of: #"["low","high"]"#, with: #"["low"]"#)
        XCTAssertNotEqual(before.signature, SessionDetailsPresentation.settings(object(catalog)).signature)
    }

    func testUnreportedFieldsAreHonestAndNoModelWindowIsInvented() {
        let view = SessionDetailsPresentation.parse(JSONObject(), target, verified: false)
        XCTAssertTrue(view.recorded)
        XCTAssertEqual("Not reported", view.fields.first { $0.label == "Process" }?.value)
        XCTAssertTrue(view.goal.isEmpty)
        XCTAssertTrue(view.processes.isEmpty)
        XCTAssertEqual("No goal reported.", view.goalNotice)
    }

    func testNestedGenerationAndUnfamiliarStatesKeepNativeEvidence() {
        let view = SessionDetailsPresentation.parse(object(#"{"generation":"parent","processes":{"generation":"process-generation"}}"#), target, verified: true)
        XCTAssertEqual("process-generation", view.generation)
        XCTAssertEqual("Finished", SessionDetailsPresentation.agentStatus("completed"))
        XCTAssertEqual("native-custom-state", SessionDetailsPresentation.agentStatus("native-custom-state"))
        XCTAssertEqual("blocked", SessionDetailsPresentation.taskStatus("blocked"))
    }
}

/// Ported from Android ConversationViewportTest and ComposerReconcilerTest.
final class ConversationViewportTests: XCTestCase {
    private let keys = ["status", "event:first", "event:last", "tail"]

    func testFirstOpenGoesDirectlyToTail() {
        let request = ConversationViewport.content(ConversationFollowState(), keys: keys, ownSendId: "", ownItemKey: nil)
        XCTAssertEqual(3, request.index)
        XCTAssertTrue(request.state.opened)
        XCTAssertTrue(request.state.following)
    }

    func testReopeningRestoresExactMessageAndOffsetAcrossPrependedRows() {
        let anchor = ConversationAnchor(itemKey: "event:first", offset: 42, following: false)
        let request = ConversationViewport.content(ConversationFollowState(following: false, anchor: anchor),
                                                   keys: ["earlier"] + keys, ownSendId: "", ownItemKey: nil)
        XCTAssertEqual(2, request.index)
        XCTAssertEqual(42, request.offset)
        XCTAssertFalse(request.state.following)
    }

    func testMissingAnchorFallsBackToLatest() {
        let request = ConversationViewport.content(ConversationFollowState(anchor: ConversationAnchor(itemKey: "expired", offset: 42, following: false)),
                                                   keys: keys, ownSendId: "", ownItemKey: nil)
        XCTAssertEqual(3, request.index)
        XCTAssertEqual(0, request.offset)
        XCTAssertTrue(request.state.following)
    }

    func testIncomingMessagesDoNotMoveUserReadingOlderHistory() {
        let reading = ConversationFollowState(opened: true, following: false)
        XCTAssertNil(ConversationViewport.content(reading, keys: keys + ["new-tail"], ownSendId: "", ownItemKey: nil).index)
        XCTAssertEqual(reading, ConversationViewport.content(reading, keys: keys, ownSendId: "", ownItemKey: nil).state)
    }

    func testOwnSendForcesItsMessageIntoViewOnceItsRowExists() {
        let reading = ConversationFollowState(opened: true, following: false, ownSendId: "old")
        let request = ConversationViewport.content(reading, keys: keys, ownSendId: "new", ownItemKey: "event:last")
        XCTAssertEqual(2, request.index)
        XCTAssertTrue(request.state.following)
        XCTAssertEqual("new", request.state.ownSendId)
        let pending = ConversationViewport.content(reading, keys: keys, ownSendId: "new", ownItemKey: "not-yet-present")
        XCTAssertNil(pending.index)
        XCTAssertEqual("old", pending.state.ownSendId)
        XCTAssertNotNil(ConversationViewport.content(pending.state, keys: keys + ["not-yet-present"], ownSendId: "new", ownItemKey: "not-yet-present").index)
    }

    func testJumpScrollAndSettledFollowing() {
        let following = ConversationViewport.latest(ConversationFollowState(opened: true, following: false))
        XCTAssertEqual(3, ConversationViewport.content(following, keys: keys, ownSendId: "", ownItemKey: nil).index)
        XCTAssertFalse(ConversationViewport.userScrolled(following, atEnd: false).following)
        let reading = ConversationFollowState(opened: true, following: false)
        XCTAssertTrue(ConversationViewport.settled(reading, atEnd: true, hasNewer: false).following)
        XCTAssertFalse(ConversationViewport.settled(reading, atEnd: true, hasNewer: true).following)
        XCTAssertFalse(ConversationViewport.settled(ConversationFollowState(opened: false, following: false), atEnd: true, hasNewer: false).following)
        XCTAssertEqual(ConversationScroll(state: ConversationFollowState()), ConversationViewport.content(ConversationFollowState(), keys: [], ownSendId: "new", ownItemKey: "new"))
    }

    func testExplicitLatestReadOutcomes() {
        let reading = ConversationFollowState(opened: true, following: false, ownSendId: "retained-send",
                                              anchor: ConversationAnchor(itemKey: "event:first", offset: 42, following: false))
        let result = ConversationViewport.loadedLatest(reading, keys: keys, hasNewer: false, error: "")
        XCTAssertEqual(3, result.index)
        XCTAssertTrue(result.state.following)
        XCTAssertNil(result.state.anchor)
        XCTAssertEqual("retained-send", result.state.ownSendId)
        let middle = ConversationViewport.loadedLatest(reading, keys: keys, hasNewer: true, error: "")
        XCTAssertNil(middle.index)
        XCTAssertEqual(reading, middle.state)
        XCTAssertTrue(middle.failure.contains("Refresh"))
        let failed = ConversationViewport.loadedLatest(reading, keys: keys, hasNewer: true, error: "Message history is still indexing. Try again shortly.")
        XCTAssertTrue(failed.failure.hasSuffix("Try again shortly."))
        let empty = ConversationViewport.loadedLatest(ConversationFollowState(), keys: [], hasNewer: false, error: "")
        XCTAssertTrue(empty.failure.contains("No messages"))
        XCTAssertEqual(ConversationFollowState(), empty.state)
    }

    func testComposerEchoesNeverReplaceTheEditor() {
        for echo in ["", "w", "wo", "working"] {
            XCTAssertNil(ComposerReconciler.external(current: "composition", generation: "composition", text: echo).replacement)
        }
        let clear = ComposerReconciler.external(current: "sent", generation: "next", text: "")
        XCTAssertEqual("", clear.replacement)
        XCTAssertNil(ComposerReconciler.external(current: clear.generation, generation: "next", text: "Future typing echo").replacement)
        XCTAssertEqual("Saved\ncomposition", ComposerReconciler.external(current: "empty", generation: "restored", text: "Saved\ncomposition").replacement)
    }
}

/// Ported from Android UnreadPresentationTest.
final class OpeningPositionTests: XCTestCase {
    private let keys = ["history:earlier", "event:old", "history:unread", "event:new", "history:tail"]

    func testSavedPositionWinsOverUnreadDivider() {
        let position = UnreadPresentation.opening(memory: nil, saved: SavedConversationViewport(eventId: "old", offset: 37), boundary: "new", keys: keys)
        XCTAssertEqual("event:old", position.anchor?.itemKey)
        XCTAssertEqual(37, position.anchor?.offset)
        XCTAssertFalse(position.unavailableSaved)
    }

    func testFirstUnreadIsShownWhenNoReadingAnchorExists() {
        XCTAssertEqual("history:unread", UnreadPresentation.opening(memory: nil, saved: nil, boundary: "new", keys: keys).anchor?.itemKey)
        XCTAssertNil(UnreadPresentation.opening(memory: nil, saved: nil, boundary: "", keys: keys.filter { $0 != "history:unread" }).anchor)
    }

    func testDeliberateMemoryPositionWinsOverDurableOlderPosition() {
        let memory = ConversationAnchor(itemKey: "event:new", offset: 17, following: false)
        XCTAssertEqual(memory, UnreadPresentation.opening(memory: memory, saved: SavedConversationViewport(eventId: "old", offset: 10), boundary: "new", keys: keys).anchor)
    }

    func testMissingSavedPositionIsExplicitAndNeverFallsToTail() {
        let withUnread = UnreadPresentation.opening(memory: nil, saved: SavedConversationViewport(eventId: "missing", offset: 10), boundary: "new", keys: keys)
        XCTAssertTrue(withUnread.unavailableSaved)
        XCTAssertEqual("history:unread", withUnread.anchor?.itemKey)
        let noUnread = UnreadPresentation.opening(memory: nil, saved: SavedConversationViewport(eventId: "missing", offset: 10), boundary: "",
                                                  keys: keys.filter { $0 != "history:unread" })
        XCTAssertEqual("event:old", noUnread.anchor?.itemKey)
        XCTAssertTrue(noUnread.unavailableSaved)
    }
}
