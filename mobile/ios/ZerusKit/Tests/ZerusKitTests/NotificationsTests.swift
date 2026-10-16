import XCTest
@testable import ZerusKit

/// Ported from Android NotificationPolicyTest.
final class NotificationPolicyTests: XCTestCase {
    private let now = 1_000.0
    private let preferences = NotificationPreferences(generation: 0)
    private var optionalOn: NotificationPreferences { var p = preferences; p.optional = true; return p }

    private func slot(_ name: String = "review", workspace: String = "workspace") -> NotificationSlot {
        NotificationSlot(connection: workspace, computer: "computer", session: name)
    }
    private func current(_ name: String = "review", workspace: String = "workspace", target: String = "run/conversation", identity: String = "question-1",
                         kind: SessionAlertKind? = .input, idle: Bool = false) -> NotificationCurrent {
        NotificationCurrent(slot: slot(name, workspace: workspace), target: target, title: "Review requested", machine: "QA laptop", kind: kind,
                            identity: identity, idle: idle)
    }
    private func event(_ id: Int64 = 1, _ name: String = "review", kind: String = "attention", at: Double? = nil) -> NotificationEvent {
        NotificationEvent(id: id, computer: "computer", session: name, kind: kind, at: at ?? now)
    }
    private func alert(_ id: Int64 = 2, _ name: String = "review", kind: String = "attention", at: Double? = nil) -> NotificationEvent {
        NotificationEvent(id: id, computer: "computer", session: name, kind: "alert", at: at ?? now, alertKind: kind)
    }
    private func ready(_ values: [NotificationCurrent]? = nil) -> NotificationState {
        NotificationPolicy.plan(NotificationState(bootstrap: false, settingsGeneration: 0), current: values ?? [current()], events: [], preferences: preferences,
                                now: now - 10).state
    }
    /// nil uses the default event; `planWithout` passes no event at all.
    private func plan(_ state: NotificationState, _ value: NotificationCurrent, _ valueEvent: NotificationEvent? = nil,
                      _ prefs: NotificationPreferences? = nil, at: Double? = nil, alertsOnly: Bool = false) -> NotificationPlan {
        NotificationPolicy.plan(state, current: [value], events: [(value.slot, valueEvent ?? event())], preferences: prefs ?? preferences,
                                now: at ?? now, alertsOnly: alertsOnly)
    }
    private func roundtrip(_ state: NotificationState) throws -> NotificationState {
        try JSONDecoder().decode(NotificationState.self, from: JSONEncoder().encode(state)).restored()
    }
    private func optional(_ ids: String..., target: String = "run/conversation", kind: SessionAlertKind?? = .some(nil), idle: Bool = false,
                          live: Bool = true) -> NotificationCurrent {
        let resolved: SessionAlertKind? = kind == .some(nil) ? (ids.isEmpty ? nil : .optional) : kind!
        return NotificationCurrent(slot: slot(), target: target, title: "Review requested", machine: "QA laptop", kind: resolved, identity: "hash-\(ids.count)",
                                   idle: idle, live: live, conversation: "conversation", optional: ids)
    }
    private func posted(_ plan: NotificationPlan) throws -> NotificationState {
        try roundtrip(NotificationPolicy.reserve(plan.state, plan.desired[0], audible: true, elapsed: 10_000))
    }
    private func planWithout(_ state: NotificationState, _ value: NotificationCurrent, _ prefs: NotificationPreferences? = nil, at: Double? = nil,
                             alertsOnly: Bool = false) -> NotificationPlan {
        NotificationPolicy.plan(state, current: [value], events: [], preferences: prefs ?? preferences, now: at ?? now, alertsOnly: alertsOnly)
    }

    func testOptionalQuestionsStayQuietByDefaultAndAnnounceOncePerQuestionWhenEnabled() throws {
        XCTAssertFalse(preferences.allows(.optional))
        XCTAssertTrue(plan(ready(), optional("a")).desired.isEmpty)
        let first = plan(ready(), optional("a"), nil, optionalOn)
        XCTAssertEqual(.optional, first.desired.first?.kind)
        XCTAssertEqual(true, first.desired.first?.fresh)
        XCTAssertEqual("Optional question on QA laptop", first.desired.first?.body)
        let shown = try posted(first)
        let resumed = plan(shown, optional("a", target: "resumed/conversation"), event(2), optionalOn, at: now + 1)
        XCTAssertEqual(false, resumed.desired.first?.fresh)
        XCTAssertEqual(shown.records.first?.postedFingerprint, resumed.desired.first?.fingerprint)
        let empty = plan(try roundtrip(resumed.state), optional(), event(3), optionalOn, at: now + 2)
        XCTAssertTrue(empty.desired.isEmpty)
        XCTAssertTrue(empty.retained.isEmpty)
        let again = plan(shown, optional("a", "b"), event(4), optionalOn, at: now + 3)
        XCTAssertEqual(true, again.desired.first?.fresh)
        let answered = planWithout(try posted(again), optional("b"), optionalOn, at: now + 4)
        XCTAssertEqual(false, answered.desired.first?.fresh)
        XCTAssertEqual(again.desired.first?.fingerprint, answered.desired.first?.fingerprint)
        let emptied = plan(try roundtrip(answered.state), optional(), event(5), optionalOn, at: now + 5)
        XCTAssertTrue(emptied.desired.isEmpty)
        XCTAssertEqual(false, plan(try roundtrip(emptied.state), optional("b"), event(6), optionalOn, at: now + 6).desired.first?.fresh)
    }

    func testOptionalQuestionsSeenElsewhereOrStoppedDoNotAnnounceLater() throws {
        let disabled = plan(ready(), optional("a"))
        var enabling = optionalOn
        enabling.generation = 1
        enabling.optionalEnabledAt = 1
        let enabled = plan(try roundtrip(disabled.state), optional("a"), event(2), enabling, at: now + 1)
        XCTAssertEqual(false, enabled.desired.first?.fresh)
        XCTAssertTrue(enabled.restoreKinds.contains(.optional))
        let request = plan(ready(), optional("a", kind: .input), nil, optionalOn)
        XCTAssertEqual(.input, request.desired.first?.kind)
        let resolved = plan(try posted(request), optional("a"), event(2), optionalOn, at: now + 1)
        XCTAssertEqual(.optional, resolved.desired.first?.kind)
        XCTAssertEqual(false, resolved.desired.first?.fresh)
        let shown = try posted(plan(ready(), optional("a"), nil, optionalOn))
        let stopped = plan(shown, optional("a", live: false), event(2), optionalOn, at: now + 1)
        XCTAssertTrue(stopped.desired.isEmpty)
        XCTAssertEqual(["a"], try roundtrip(stopped.state).records.first?.optionalSeen)
        let restarted = plan(try roundtrip(stopped.state), optional("a", target: "resumed/conversation"), event(3), optionalOn, at: now + 2)
        XCTAssertEqual(false, restarted.desired.first?.fresh)
        XCTAssertEqual(true, plan(try roundtrip(restarted.state), optional("a", "b", target: "resumed/conversation"), event(4), optionalOn, at: now + 3).desired.first?.fresh)
    }

    func testOptionalQuestionsWaitForPresenceAlertsAndYieldToCompletion() throws {
        let held = plan(ready(), optional("a"), event(), optionalOn, alertsOnly: true)
        XCTAssertTrue(held.desired.isEmpty)
        let alerted = plan(try roundtrip(held.state), optional("a"), alert(3), optionalOn, at: now + 1, alertsOnly: true)
        XCTAssertEqual(true, alerted.desired.first?.fresh)
        XCTAssertTrue(plan(try posted(alerted), optional("a"), alert(4), optionalOn, at: now + 2, alertsOnly: true).desired.allSatisfy { !$0.fresh })
        let idle = optional("a", idle: true)
        var finishedOn = preferences
        finishedOn.finished = true
        let finished = plan(ready([idle]), idle, event(kind: "completed"), finishedOn)
        XCTAssertEqual(.finished, finished.desired.first?.kind)
        XCTAssertEqual(true, finished.desired.first?.fresh)
        XCTAssertFalse(SessionAlertKind.optional.overflows)
        XCTAssertEqual([.input, .error], Set(SessionAlertKind.allCases.filter(\.overflows)))
    }

    func testCodecToleratesUnknownKindsAndBoundsSeenQuestions() throws {
        let state = NotificationState(records: [NotificationRecord(slot: slot(), target: "target", observedAt: now, postedKind: .optional,
                                                                   optionalSeen: (1...80).map { "q\($0)" })])
        let record = try XCTUnwrap(roundtrip(state).records.first)
        XCTAssertEqual(.optional, record.postedKind)
        XCTAssertEqual((17...80).map { "q\($0)" }, record.optionalSeen)
        var raw = try XCTUnwrap(JSON.parse(JSONEncoder().encode(state)).object)
        var row = try XCTUnwrap(raw["records"]?.array?.first?.object)
        row["postedKind"] = "Future"
        row["optionalSeen"] = nil
        raw["records"] = .array([.object(row)])
        let future = try XCTUnwrap(JSONDecoder().decode(NotificationState.self, from: JSON.object(raw).data()).records.first)
        XCTAssertNil(future.postedKind)
        XCTAssertTrue(future.optionalSeen.isEmpty)
    }

    func testDefaultsAndIndependentTypesDoNotEnableFinished() {
        XCTAssertTrue(preferences.allows(.input))
        XCTAssertTrue(preferences.allows(.error))
        XCTAssertFalse(preferences.allows(.finished))
        var errorsOnly = preferences
        errorsOnly.input = false
        errorsOnly.finished = true
        XCTAssertFalse(errorsOnly.allows(.input))
        XCTAssertTrue(errorsOnly.allows(.error))
        XCTAssertTrue(errorsOnly.allows(.finished))
        errorsOnly.master = false
        for kind in SessionAlertKind.allCases { XCTAssertFalse(errorsOnly.allows(kind)) }
    }

    func testSettingAKindRecordsWhenItWasEnabled() {
        let enabled = preferences.setting(.finished, true)
        XCTAssertEqual(1, enabled.generation)
        XCTAssertEqual(1, enabled.finishedEnabledAt)
        let again = enabled.setting(.finished, true)
        XCTAssertEqual(1, again.finishedEnabledAt)
        let master = again.settingMaster(false).settingMaster(true)
        XCTAssertEqual(master.generation, master.inputEnabledAt)
        XCTAssertEqual(master.generation, master.finishedEnabledAt)
        XCTAssertEqual(0, master.optionalEnabledAt)
    }

    func testMoreThanOnePageRetainsCursorAndCoalescesExactSlot() throws {
        var state = NotificationState()
        for start in [1, 101, 201] {
            let end = min(start + 99, 250)
            state = try roundtrip(NotificationPolicy.ingest(state, (start...end).map { event(Int64($0)) }).state)
        }
        XCTAssertEqual(250, state.cursor)
        XCTAssertEqual([250], state.pending.map(\.id))
        let drained = try NotificationPolicy.ingest(state, []).state
        let baseline = plan(drained, current(), event(250))
        XCTAssertFalse(baseline.state.bootstrap)
        XCTAssertTrue(baseline.desired.allSatisfy { !$0.fresh })
    }

    func testCapacityDoesNotConsumeUnadmittedEventAndRetrySurvivesRestart() throws {
        let first = try NotificationPolicy.ingest(NotificationState(bootstrap: false), (1...100).map { event(Int64($0), "session-\($0)") })
        let second = try NotificationPolicy.ingest(try roundtrip(first.state), (101...200).map { event(Int64($0), "session-\($0)") })
        XCTAssertTrue(second.full)
        XCTAssertEqual(128, second.state.cursor)
        XCTAssertEqual(128, second.state.pending.count)
        let observed = NotificationPolicy.plan(try roundtrip(second.state), current: (1...200).map { current("session-\($0)") }, events: [],
                                               preferences: preferences, now: now).state
        let retry = try NotificationPolicy.ingest(observed, (129...200).map { event(Int64($0), "session-\($0)") })
        XCTAssertEqual(200, retry.state.cursor)
        XCTAssertEqual(129, retry.state.pending.first?.id)
        XCTAssertEqual(72, retry.state.pending.count)
        XCTAssertThrowsError(try NotificationPolicy.ingest(retry.state, [event(5)]))
    }

    func testBootstrapStaysSilentAcrossBacklogPagesAndCatalogReconciliation() throws {
        let page = try NotificationPolicy.ingest(NotificationState(), [event()])
        let first = plan(page.state, current(), event())
        XCTAssertTrue(first.state.bootstrap)
        XCTAssertTrue(first.desired.allSatisfy { !$0.fresh })
        let final = plan(try NotificationPolicy.ingest(first.state, []).state, current(), event(2))
        XCTAssertFalse(final.state.bootstrap)
        XCTAssertTrue(final.desired.allSatisfy { !$0.fresh })
    }

    func testSameQuestionRestartAndDismissalDoNotBecomeFreshAgain() throws {
        let changed = current(identity: "question-2")
        let first = plan(ready(), changed)
        XCTAssertEqual(true, first.desired.first?.fresh)
        let reserved = try roundtrip(NotificationPolicy.reserve(first.state, first.desired[0], audible: true, elapsed: 10_000))
        let duplicate = plan(reserved, changed, event(2), at: now + 1)
        XCTAssertEqual(false, duplicate.desired.first?.fresh)
        XCTAssertEqual(reserved.records.first?.postedFingerprint, duplicate.desired.first?.fingerprint)
    }

    func testNewRunReplacesSameSlotAndTwoWorkspacesRemainSeparate() {
        let changed = current(target: "new-run/new-conversation")
        let next = plan(ready(), changed)
        XCTAssertEqual(true, next.desired.first?.fresh)
        XCTAssertEqual(slot(), next.desired.first?.current.slot)
        let foreign = current(workspace: "other")
        var foreignChanged = foreign
        foreignChanged.identity = "question-2"
        let combined = NotificationPolicy.plan(ready([current(), foreign]), current: [changed, foreignChanged],
                                               events: [(changed.slot, event()), (foreign.slot, event())], preferences: preferences, now: now)
        XCTAssertEqual(2, combined.desired.count)
        XCTAssertEqual(2, Set(combined.desired.map(\.current.slot.tag)).count)
    }

    func testOldCompletionCannotOverrideCurrentErrorOrAnotherRun() {
        var finishedOn = preferences
        finishedOn.finished = true
        let error = current(identity: "provider-error", kind: .error)
        XCTAssertEqual(.error, plan(ready(), error, event(kind: "completed"), finishedOn).desired.first?.kind)
        let idle = current(kind: nil, idle: true)
        XCTAssertTrue(plan(ready(), idle, event(kind: "completed", at: now - 121), finishedOn).desired.isEmpty)
        var otherRun = idle
        otherRun.target = "other-run"
        XCTAssertTrue(plan(ready(), otherRun, event(kind: "completed"), finishedOn).desired.isEmpty)
    }

    func testCurrentCompletionRequiresPriorObservationAndExplicitChoice() {
        let idle = current(kind: nil, idle: true)
        XCTAssertTrue(plan(ready(), idle, event(kind: "completed")).desired.isEmpty)
        var finishedOn = preferences
        finishedOn.finished = true
        let allowed = plan(ready(), idle, event(kind: "completed"), finishedOn)
        XCTAssertEqual(.finished, allowed.desired.first?.kind)
        XCTAssertEqual(true, allowed.desired.first?.fresh)
    }

    func testPreferenceChangesReconcileSilentlyAndDisabledOccurrenceIsNotReplayed() throws {
        let input = current(identity: "question-2")
        var off = preferences
        off.input = false
        off.generation = 1
        let disabled = plan(ready(), input, event(), off)
        XCTAssertTrue(disabled.desired.isEmpty)
        var on = preferences
        on.generation = 2
        let enabled = plan(try roundtrip(disabled.state), input, event(2), on)
        XCTAssertTrue(enabled.reconcile)
        XCTAssertEqual(false, enabled.desired.first?.fresh)
    }

    func testOfflineRetainsExistingCardButPausedAndMissingCancelIt() {
        let changed = current(identity: "question-2")
        let first = plan(ready(), changed)
        let posted = NotificationPolicy.reserve(first.state, first.desired[0], audible: false, elapsed: 0)
        var offline = changed
        offline.online = false
        let offlinePlan = plan(posted, offline)
        XCTAssertEqual([slot().key], offlinePlan.retained)
        XCTAssertTrue(offlinePlan.desired.isEmpty)
        var paused = changed
        paused.live = false
        XCTAssertTrue(plan(posted, paused).retained.isEmpty)
        XCTAssertTrue(NotificationPolicy.plan(posted, current: [], events: [], preferences: preferences, now: now).retained.isEmpty)
    }

    func testSustainedNonemptyStreamCanSoundAgainAfterCooldown() throws {
        let changed = current(identity: "question-2")
        let first = plan(try NotificationPolicy.ingest(ready(), [event()]).state, changed)
        XCTAssertTrue(NotificationPolicy.canSound(first.state, elapsed: 10_000))
        let posted = NotificationPolicy.reserve(first.state, first.desired[0], audible: true, elapsed: 10_000)
        let nextPage = try NotificationPolicy.ingest(posted, [event(2)])
        var third = changed
        third.identity = "question-3"
        let second = plan(nextPage.state, third, event(2), at: now + 31)
        XCTAssertFalse(NotificationPolicy.canSound(second.state, elapsed: 39_999))
        XCTAssertTrue(NotificationPolicy.canSound(second.state, elapsed: 40_000))
    }

    func testRebootElapsedClockCannotBypassCooldown() {
        let guarded = NotificationPolicy.clockGuard(NotificationState(lastSoundElapsed: 80_000), elapsed: 100)
        XCTAssertFalse(NotificationPolicy.canSound(guarded, elapsed: 100))
        XCTAssertFalse(NotificationPolicy.canSound(guarded, elapsed: 30_099))
        XCTAssertTrue(NotificationPolicy.canSound(guarded, elapsed: 30_100))
    }

    func testBoundedRegistryDoesNotResurrectEvictedOccurrences() throws {
        let values = (1...300).map { current("session-\($0)") }
        let first = NotificationPolicy.plan(NotificationState(bootstrap: false, settingsGeneration: 0), current: values, events: [], preferences: preferences, now: now)
        XCTAssertEqual(256, first.state.records.count)
        XCTAssertTrue(first.state.truncated)
        XCTAssertTrue(first.overflow)
        let kept = Set(first.state.records.map(\.slot.key))
        let omitted = try XCTUnwrap(values.first { !kept.contains($0.slot.key) })
        let narrowed = planWithout(try roundtrip(first.state), omitted, at: now + 1)
        XCTAssertTrue(narrowed.desired.isEmpty)
        var changed = omitted
        changed.identity = "new-question"
        XCTAssertEqual(true, plan(narrowed.state, changed, event(301, omitted.slot.session), at: now + 2).desired.first?.fresh)
    }

    func testDurableStatePreservesReservedDeliveryAndRejectsOversizedState() throws {
        let first = plan(ready(), current(identity: "question-2"))
        var base = first.state
        base.cursor = 250
        base.pending = [event(251)]
        let state = NotificationPolicy.reserve(base, first.desired[0], audible: true, elapsed: 10_000)
        XCTAssertEqual(state, try roundtrip(state))
        var oversized = state
        oversized.records = Array(repeating: state.records[0], count: 257)
        XCTAssertThrowsError(try roundtrip(oversized))
    }

    func testInterruptedPlanRemainsDurableDeliveryWorkBeforeAnyReserve() throws {
        let changed = current(identity: "question-2")
        let planned = plan(try NotificationPolicy.ingest(ready(), [event()]).state, changed)
        XCTAssertTrue(planned.state.pending.isEmpty)
        XCTAssertTrue(planned.state.deliveryNeeded)
        let emptyPage = try NotificationPolicy.ingest(try roundtrip(planned.state), []).state
        XCTAssertTrue(emptyPage.deliveryNeeded)
        let retry = planWithout(emptyPage, changed)
        XCTAssertEqual(.input, retry.desired.first?.kind)
        XCTAssertEqual(false, retry.desired.first?.fresh)
        XCTAssertEqual("", retry.state.records.first?.postedFingerprint)
    }

    func testUnrelatedPreferenceChangeDoesNotRestoreDismissedOtherType() throws {
        let changed = current(identity: "question-2")
        let first = plan(ready(), changed)
        let posted = try roundtrip(NotificationPolicy.reserve(first.state, first.desired[0], audible: false, elapsed: 0))
        var finishedOn = preferences
        finishedOn.generation = 1
        finishedOn.finished = true
        finishedOn.finishedEnabledAt = 1
        let unrelated = planWithout(posted, changed, finishedOn)
        XCTAssertEqual([.finished], unrelated.restoreKinds)
        var inputBack = preferences
        inputBack.generation = 2
        inputBack.inputEnabledAt = 2
        let reenabled = planWithout(unrelated.state, changed, inputBack)
        XCTAssertEqual([.input], reenabled.restoreKinds)
        XCTAssertEqual(false, reenabled.desired.first?.fresh)
    }

    func testOfflineUnpostedReservationRetiresRetryWithoutDiscardingExistingActiveCard() throws {
        let changed = current(identity: "question-2")
        let first = plan(ready(), changed)
        let reserved = try roundtrip(NotificationPolicy.reserve(first.state, first.desired[0], audible: false, elapsed: 0))
        var offline = changed
        offline.online = false
        let retired = planWithout(reserved, offline)
        XCTAssertTrue(retired.state.pendingPosts.isEmpty)
        XCTAssertEqual("", retired.state.records.first?.postedFingerprint)
        let present = NotificationPolicy.plan(reserved, current: [offline], events: [], preferences: preferences, now: now, active: [changed.slot.key])
        XCTAssertEqual(reserved.records.first?.postedFingerprint, present.state.records.first?.postedFingerprint)
        XCTAssertTrue(present.retained.contains(changed.slot.key))
    }

    func testOverflowSummaryKindsAreDurableButFinishedNeverCreatesOverflowSummary() throws {
        let values = (1...300).map { current("session-\($0)") }
        let bounded = NotificationPolicy.plan(NotificationState(bootstrap: false, settingsGeneration: 0), current: values, events: [], preferences: preferences, now: now)
        XCTAssertEqual([.input], bounded.overflowKinds)
        var saved = bounded.state
        saved.overflowKinds = bounded.overflowKinds
        XCTAssertEqual(saved, try roundtrip(saved))
    }

    func testReenabledDismissedTypeSurvivesPlanCommitBeforeReserve() throws {
        let changed = current(identity: "question-2")
        let initial = plan(ready(), changed)
        var posted = NotificationPolicy.reserve(initial.state, initial.desired[0], audible: false, elapsed: 0)
        posted.pendingPosts = []
        posted.deliveryNeeded = false
        var enabled = preferences
        enabled.generation = 1
        enabled.inputEnabledAt = 1
        let committed = planWithout(posted, changed, enabled)
        XCTAssertTrue(committed.restoreKinds.contains(.input))
        let restarted = planWithout(try NotificationPolicy.ingest(try roundtrip(committed.state), []).state, changed, enabled)
        XCTAssertTrue(restarted.restoreKinds.contains(.input))
        XCTAssertEqual(false, restarted.desired.first?.fresh)
    }

    func testOptedInCompletionSurvivesPlanCommitBeforeItsFirstReserve() throws {
        let idle = current(kind: nil, idle: true)
        var enabled = preferences
        enabled.finished = true
        let committed = plan(ready(), idle, event(kind: "completed"), enabled)
        XCTAssertEqual(.finished, committed.desired.first?.kind)
        let restarted = planWithout(try NotificationPolicy.ingest(try roundtrip(committed.state), []).state, idle, enabled, at: now + 1)
        XCTAssertEqual(.finished, restarted.desired.first?.kind)
        XCTAssertEqual(false, restarted.desired.first?.fresh)
    }

    func testRestoredFirstSlotKeepsItsEvidenceWhileRemainingSlotWaitsAcrossRestart() throws {
        let first = current("first", identity: "question-2"), second = current("second", identity: "question-2")
        var enabled = preferences
        enabled.generation = 7
        enabled.inputEnabledAt = 7
        let planned = NotificationPolicy.plan(ready([first, second]), current: [first, second], events: [], preferences: enabled, now: now)
        var partial = NotificationPolicy.reserve(planned.state, planned.desired[0], audible: false, elapsed: 0, enabledAt: enabled.inputEnabledAt)
        partial.pendingPosts = []
        let restarted = try roundtrip(partial)
        XCTAssertTrue(restarted.deliveryNeeded)
        XCTAssertEqual(7, restarted.records.first { $0.slot == planned.desired[0].current.slot }?.restoredAt)
        let retry = NotificationPolicy.plan(restarted, current: [first, second], events: [], preferences: enabled, now: now + 1)
        XCTAssertTrue(retry.restoreKinds.contains(.input))
        XCTAssertLessThan(try XCTUnwrap(retry.state.records.first { $0.slot != planned.desired[0].current.slot }).restoredAt, enabled.inputEnabledAt)
        XCTAssertTrue(retry.desired.allSatisfy { !$0.fresh })
    }

    func testInterruptedCompletionDoesNotFollowNewTargetExpireOrOverrideError() throws {
        let idle = current(kind: nil, idle: true)
        var enabled = preferences
        enabled.finished = true
        let pending = try roundtrip(plan(ready(), idle, event(kind: "completed"), enabled).state)
        var moved = idle
        moved.target = "new-run"
        XCTAssertTrue(planWithout(pending, moved, enabled, at: now + 1).desired.isEmpty)
        XCTAssertTrue(planWithout(pending, idle, enabled, at: now + 121).desired.isEmpty)
        var failed = idle
        failed.kind = .error
        failed.identity = "new-error"
        XCTAssertEqual(.error, planWithout(pending, failed, enabled, at: now + 1).desired.first?.kind)
    }

    func testPresenceAlertsPostOnlyForNeedAlertRows() {
        let changed = current(identity: "question-2")
        let waiting = plan(ready(), changed, event(), alertsOnly: true)
        XCTAssertTrue(waiting.desired.isEmpty)
        XCTAssertTrue(waiting.retained.isEmpty)
        XCTAssertTrue(planWithout(waiting.state, changed, at: now + 1, alertsOnly: true).desired.isEmpty)
        XCTAssertTrue(plan(waiting.state, changed, alert(kind: "completed"), alertsOnly: true).desired.isEmpty)
        let alerted = plan(waiting.state, changed, alert(3), alertsOnly: true)
        XCTAssertEqual(.input, alerted.desired.first?.kind)
        XCTAssertEqual(true, alerted.desired.first?.fresh)
    }

    func testPresenceAlertsAnnounceInputAndErrorCardsFromEitherNeedAlert() {
        let error = current(identity: "failed-recovery-with-question", kind: .error)
        let fromAttention = plan(ready(), error, alert(kind: "attention"), alertsOnly: true)
        XCTAssertEqual(.error, fromAttention.desired.first?.kind)
        XCTAssertEqual(true, fromAttention.desired.first?.fresh)
        let fromError = plan(ready(), current(identity: "question-2"), alert(kind: "error"), alertsOnly: true)
        XCTAssertEqual(.input, fromError.desired.first?.kind)
        XCTAssertEqual(true, fromError.desired.first?.fresh)
    }

    func testPresenceAlertsUpdatePostedCardSilentlyAndHoldNewNeedWithoutAlert() throws {
        let changed = current(identity: "question-2")
        let alerted = plan(ready(), changed, alert(), alertsOnly: true)
        let posted = try roundtrip(NotificationPolicy.reserve(alerted.state, alerted.desired[0], audible: true, elapsed: 10_000))
        let refresh = plan(posted, changed, event(3), at: now + 1, alertsOnly: true)
        XCTAssertEqual([changed.slot.key], refresh.retained)
        XCTAssertEqual(false, refresh.desired.first?.fresh)
        var detail = changed
        detail.identity = "question-2-details"
        for batch in [nil, event(4, kind: "completed")] {
            let updated = batch.map { plan(refresh.state, detail, $0, at: now + 2, alertsOnly: true) } ?? planWithout(refresh.state, detail, at: now + 2, alertsOnly: true)
            XCTAssertEqual([changed.slot.key], updated.retained)
            XCTAssertEqual(.input, updated.desired.first?.kind)
            XCTAssertEqual(false, updated.desired.first?.fresh)
            XCTAssertNotEqual(posted.records.first?.postedFingerprint, updated.desired.first?.fingerprint)
        }
        let errorCard = current(identity: "error-1", kind: .error)
        let errorAlerted = plan(ready(), errorCard, alert(kind: "error"), alertsOnly: true)
        let errorPosted = NotificationPolicy.reserve(errorAlerted.state, errorAlerted.desired[0], audible: true, elapsed: 10_000)
        XCTAssertEqual(.input, planWithout(errorPosted, changed, at: now + 1, alertsOnly: true).desired.first?.kind)
        let updated = planWithout(refresh.state, detail, at: now + 2, alertsOnly: true)
        let reposted = try roundtrip(NotificationPolicy.reserve(updated.state, updated.desired[0], audible: false, elapsed: 20_000))
        var next = changed
        next.identity = "question-3"
        let replaced = plan(reposted, next, event(5), at: now + 3, alertsOnly: true)
        XCTAssertTrue(replaced.desired.isEmpty)
        XCTAssertTrue(replaced.retained.isEmpty)
        var newRun = changed
        newRun.target = "new-run/conversation"
        newRun.identity = "question-3"
        XCTAssertTrue(planWithout(reposted, newRun, at: now + 3, alertsOnly: true).desired.isEmpty)
        let announced = plan(replaced.state, next, alert(6), at: now + 4, alertsOnly: true)
        XCTAssertEqual(.input, announced.desired.first?.kind)
        XCTAssertEqual(true, announced.desired.first?.fresh)
    }

    func testPresenceAlertsAnnounceCompletionOnlyFromCompletedAlert() {
        let idle = current(kind: nil, idle: true)
        var enabled = preferences
        enabled.finished = true
        XCTAssertTrue(plan(ready(), idle, event(kind: "completed"), enabled, alertsOnly: true).desired.isEmpty)
        XCTAssertEqual(.finished, plan(ready(), idle, alert(kind: "completed"), enabled, alertsOnly: true).desired.first?.kind)
    }

    func testWithoutPresenceAlertsAlertRowsNeitherAnnounceNorHideCompletion() {
        let idle = current(kind: nil, idle: true)
        var enabled = preferences
        enabled.finished = true
        let both = [(idle.slot, event(1, kind: "completed")), (idle.slot, alert(2, kind: "completed"))]
        XCTAssertEqual(.finished, NotificationPolicy.plan(ready(), current: [idle], events: both, preferences: enabled, now: now).desired.first?.kind)
        XCTAssertTrue(NotificationPolicy.plan(ready(), current: [idle], events: [(idle.slot, alert(kind: "completed"))], preferences: enabled, now: now).desired.isEmpty)
        XCTAssertEqual(.input, plan(ready(), current(identity: "question-2"), alert()).desired.first?.kind)
    }

    func testAlertRowsSurviveLaterRefreshRowsAndRoundtrip() throws {
        var page = try NotificationPolicy.ingest(NotificationState(bootstrap: false), [event(1), alert(2), event(3, kind: "completed")]).state
        page.presenceAlerts = true
        let state = try roundtrip(page)
        XCTAssertEqual([2, 3], state.pending.map(\.id).sorted())
        XCTAssertEqual("attention", state.pending.first { $0.kind == "alert" }?.alertKind)
        XCTAssertNil(state.pending.first { $0.kind == "completed" }?.alertKind)
        XCTAssertTrue(state.presenceAlerts)
    }

    func testNeedAndCompletionAlertsSurvivePagesInEitherOrder() throws {
        for need in ["attention", "error"] {
            for completionFirst in [false, true] {
                let signals = completionFirst ? ["completed", need] : [need, "completed"]
                var state = ready()
                state.presenceAlerts = true
                for (index, signal) in signals.enumerated() { state = try roundtrip(NotificationPolicy.ingest(state, [alert(Int64(index + 1), kind: signal)]).state) }
                state = try roundtrip(NotificationPolicy.ingest(state, [event(3, kind: "completed")]).state)
                XCTAssertEqual(3, state.pending.count)
                let value = current(identity: "question-2")
                let result = NotificationPolicy.plan(state, current: [value], events: state.pending.map { (value.slot, $0) }, preferences: preferences, now: now, alertsOnly: true)
                XCTAssertEqual(.input, result.desired.first?.kind)
                XCTAssertEqual(true, result.desired.first?.fresh)
                XCTAssertEqual(Int64(signals.firstIndex(of: need)! + 1), result.desired.first?.eventId)
            }
        }
    }

    func testDelayedNeedAlertRemainsFreshAfterSilentDetailDelivery() throws {
        let value = current(identity: "question-2")
        let first = plan(ready(), value, alert(), alertsOnly: true)
        var posted = NotificationPolicy.reserve(first.state, first.desired[0], audible: true, elapsed: 10_000)
        posted.pendingPosts = []
        var changed = value
        changed.identity = "question-3"
        let detail = plan(posted, changed, event(3, kind: "completed"), at: now + 1, alertsOnly: true)
        XCTAssertEqual(false, detail.desired.first?.fresh)
        var silent = NotificationPolicy.reserve(detail.state, detail.desired[0], audible: false, elapsed: 40_000)
        silent.pendingPosts = []
        silent = try roundtrip(silent)
        XCTAssertEqual(posted.records.first?.observedFingerprint, silent.records.first?.observedFingerprint)
        let nextBatch = try NotificationPolicy.ingest(silent, [alert(4)]).state
        let delivered = plan(nextBatch, changed, alert(4), at: now + 31, alertsOnly: true)
        XCTAssertEqual(true, delivered.desired.first?.fresh)
        XCTAssertTrue(NotificationPolicy.canSound(delivered.state, elapsed: 40_000))
        var reserved = NotificationPolicy.reserve(delivered.state, delivered.desired[0], audible: true, elapsed: 40_000)
        reserved.pendingPosts = []
        XCTAssertEqual(false, plan(try roundtrip(reserved), changed, alert(5), at: now + 32, alertsOnly: true).desired.first?.fresh)
    }
}

/// Ported from Android NotificationCatalogTest.
final class NotificationCatalogTests: XCTestCase {
    private let computer = "22222222-2222-4222-8222-222222222222"
    private let otherComputer = "33333333-3333-4333-8333-333333333333"
    private let alias = "44444444-4444-4444-8444-444444444444"
    private let connection = Connection(id: "workspace", label: "Example workspace", url: "https://relay.example", token: "synthetic-token")

    private func session(_ name: String = "raw/session/opaque") -> JSONObject {
        ["name": .string(name), "tag": "Review requested", "title": "Provider title", "run_id": "run", "conversation_id": "conversation", "phase": "input",
         "state": "running", "process_state": "running", "activity": "idle"]
    }
    private func machine(_ sessions: [JSONObject]? = nil, id: String? = nil, online: Bool = true) -> JSONObject {
        ["id": .string(id ?? computer), "name": "Native machine", "online": .bool(online), "via": .null,
         "snapshot": ["sessions": .array((sessions ?? [session()]).map(JSON.object))]]
    }
    private func parse(_ rows: [JSONObject]? = nil, workspace: Connection? = nil) -> NotificationCatalog {
        NotificationCatalogParser.parse(workspace ?? connection, records: rows ?? [machine()],
                                        aliases: [MachineAlias(key: MachineKey(connectionId: connection.id, computerId: computer), name: "QA laptop")])
    }
    private func event(_ id: Int64 = 1, computer: String? = nil) -> NotificationEvent {
        NotificationEvent(id: id, computer: computer ?? self.computer, session: "raw/session/opaque", kind: "attention", at: 1_000)
    }

    func testRawTagAndPrivateMachineNameAppearWithoutOpaqueSlotIds() throws {
        let current = try XCTUnwrap(parse().sessions.first)
        XCTAssertEqual("Review requested", current.title)
        XCTAssertEqual("QA laptop", current.machine)
        XCTAssertEqual("raw/session/opaque", current.slot.session)
        XCTAssertEqual(.input, current.kind)
        XCTAssertEqual("Needs input on QA laptop", SessionNotice(current: current, kind: .input, fingerprint: "fingerprint", fresh: false).body)
    }

    func testPrivateLabelDoesNotCrossWorkspaceBoundary() throws {
        let other = Connection(id: "other", label: connection.label, url: connection.url, token: connection.token)
        let foreign = try XCTUnwrap(parse(workspace: other).sessions.first)
        XCTAssertEqual("Native machine", foreign.machine)
        XCTAssertNotEqual(parse().sessions.first?.slot.tag, foreign.slot.tag)
    }

    func testAliasesResolveOnlyOneCanonicalMachineAndAmbiguityFailsClosed() {
        let first = machine().putting("aliases", .array([.string(alias)]))
        XCTAssertEqual(computer, parse([first]).resolve(connection.id, event(computer: alias))?.0.computer)
        let second = machine(id: otherComputer).putting("aliases", .array([.string(alias)]))
        XCTAssertNil(parse([first, second]).resolve(connection.id, event(computer: alias)))
        XCTAssertNil(parse().resolve(connection.id, event(computer: "missing")))
        XCTAssertTrue(parse([first, first]).sessions.isEmpty)
    }

    func testArchivePausedExitedAndOfflineDoNotCreateFreshLiveEligibility() {
        let archive = session().putting("state", "archived").putting("archive_id", "55555555-5555-4555-8555-555555555555")
        XCTAssertTrue(parse([machine([archive])]).sessions.isEmpty)
        XCTAssertEqual(false, parse([machine([session().putting("state", "paused")])]).sessions.first?.live)
        XCTAssertEqual(false, parse([machine([session().putting("process_state", "exited")])]).sessions.first?.live)
        XCTAssertEqual(false, parse([machine(online: false)]).sessions.first?.online)
        XCTAssertTrue(parse([machine([])]).sessions.isEmpty)
    }

    func testSameQuestionFactsKeepIdentityAndChangedQuestionChangesIt() {
        let first = session().putting("mobile_attention", [["question_id": "q1", "question_hash": "h1"]])
        let original = parse([machine([first])]).sessions.first?.identity
        XCTAssertEqual(original, parse([machine([first])]).sessions.first?.identity)
        let changed = first.putting("mobile_attention", [["question_id": "q2", "question_hash": "h2"]])
        XCTAssertNotEqual(original, parse([machine([changed])]).sessions.first?.identity)
    }

    func testProviderErrorIdentityChangesForNewFailureButRecoveryWaitingIsNotError() throws {
        var row = session().putting("phase", "error").putting("provider_error", ["source": "example", "message_id": "error-1", "code": "unavailable",
                                                                                   "message": "Synthetic failure"])
        let first = try XCTUnwrap(parse([machine([row])]).sessions.first)
        XCTAssertEqual(.error, first.kind)
        XCTAssertEqual(first.identity, parse([machine([row])]).sessions.first?.identity)
        row["provider_error"] = .object(row.object("provider_error")!.putting("message_id", "error-2"))
        XCTAssertNotEqual(first.identity, parse([machine([row])]).sessions.first?.identity)
        row["recovery"] = ["state": "waiting"]
        XCTAssertNil(try XCTUnwrap(parse([machine([row])]).sessions.first).kind)
        row["recovery"] = ["state": "exhausted", "request_id": "retry-1"]
        XCTAssertEqual(.error, parse([machine([row])]).sessions.first?.kind)
    }

    func testPresenceAlertsKeepPostedErrorCardAcrossFailedRecoveryStates() throws {
        var row = session().putting("phase", "working").putting("recovery", ["state": "uncertain", "request_id": "retry-1"])
        let uncertain = try XCTUnwrap(parse([machine([row])]).sessions.first)
        row["recovery"] = ["state": "exhausted", "request_id": "retry-1"]
        let exhausted = try XCTUnwrap(parse([machine([row])]).sessions.first)
        XCTAssertEqual([.error, .error], [uncertain.kind, exhausted.kind])
        XCTAssertNotEqual(uncertain.identity, exhausted.identity)
        let alerted = NotificationPolicy.plan(NotificationState(bootstrap: false, settingsGeneration: 0), current: [uncertain],
                                              events: [(uncertain.slot, NotificationEvent(id: 2, computer: computer, session: "raw/session/opaque", kind: "alert",
                                                                                           at: 1_000, alertKind: "error"))],
                                              preferences: NotificationPreferences(), now: 1_000, alertsOnly: true)
        let posted = NotificationPolicy.reserve(alerted.state, alerted.desired[0], audible: true, elapsed: 10_000)
        let later = NotificationPolicy.plan(posted, current: [exhausted], events: [], preferences: NotificationPreferences(), now: 1_030, alertsOnly: true)
        XCTAssertEqual([exhausted.slot.key], later.retained)
        XCTAssertEqual(.error, later.desired.first?.kind)
        XCTAssertEqual(false, later.desired.first?.fresh)
    }

    func testOptionalOnlyQuestionsAreTheirOwnKindAndNeverChangeARequest() throws {
        func item(_ id: String, _ optional: Bool? = true) -> JSON {
            var raw: JSONObject = ["question_id": .string(id), "question_hash": .string("h-\(id)")]
            if let optional { raw["optional"] = .bool(optional) }
            return .object(raw)
        }
        func parsed(_ phase: String, _ items: JSON..., extra: (inout JSONObject) -> Void = { _ in }) -> NotificationCurrent {
            var row = session().putting("phase", .string(phase)).putting("mobile_attention", .array(items))
            extra(&row)
            return parse([machine([row])]).sessions[0]
        }
        let optional = parsed("tool", item("codex-async:b:0"), item("codex-async:a:0"), item("codex-async:a:0"))
        XCTAssertEqual(.optional, optional.kind)
        XCTAssertEqual(["codex-async:a:0", "codex-async:b:0"], optional.optional)
        XCTAssertEqual("conversation", optional.conversation)
        XCTAssertEqual("Optional question on QA laptop", SessionNotice(current: optional, kind: .optional, fingerprint: "fingerprint", fresh: false).body)
        XCTAssertEqual(.optional, parsed("tool", item("codex-async:a:0", nil)).kind)
        XCTAssertEqual(.input, parsed("tool", item("tool-1", nil)).kind)
        XCTAssertEqual(.input, parsed("tool", item("codex-async:a:0", false)).kind)
        XCTAssertEqual(.input, parsed("tool", item("codex-async:a:0"), item("tool-1", nil)).kind)
        XCTAssertEqual(.input, parsed("input", item("codex-async:a:0")).kind)
        XCTAssertEqual(.input, parsed("tool", item("codex-async:a:0")) { $0["pending_question_count"] = 1 }.kind)
        XCTAssertEqual(.error, parsed("error", item("codex-async:a:0")).kind)
        XCTAssertNil(parsed("tool").kind)
        let request = parsed("input", item("tool-1", nil))
        let withOptional = parsed("input", item("tool-1", nil), item("codex-async:a:0"))
        XCTAssertEqual(request.identity, withOptional.identity)
        XCTAssertEqual(["codex-async:a:0"], withOptional.optional)
    }

    func testDisplayTextIsBoundedAndControlsCannotSpoofNotificationLines() throws {
        let row = session().putting("tag", .string("Review\n\u{202e}requested" + String(repeating: "x", count: 300)))
        let current = try XCTUnwrap(parse([machine([row])]).sessions.first)
        XCTAssertLessThanOrEqual(current.title.count, 256)
        XCTAssertFalse(current.title.contains("\n"))
        XCTAssertFalse(current.title.contains("\u{202e}"))
    }

    func testPagesRequireOrderedIdsAndBoundedValidEventMetadata() {
        func raw(_ ids: [Int]) -> JSONObject {
            ["events": .array(ids.map { .object(["id": .int(Int64($0)), "computer_id": .string(computer), "session": "review", "kind": "attention", "created_at": 1_000.0]) })]
        }
        XCTAssertEqual(100, try NotificationCatalogParser.events(raw(Array(1...100))).count)
        XCTAssertThrowsError(try NotificationCatalogParser.events(raw(Array(1...101))))
        XCTAssertThrowsError(try NotificationCatalogParser.events(raw([2, 1])))
        XCTAssertThrowsError(try NotificationCatalogParser.events(raw([1, 1])))
        let invalid: JSONObject = ["events": [["id": 1, "computer_id": .string(computer), "session": "review\nspoof", "kind": "attention", "created_at": 1_000.0]]]
        XCTAssertThrowsError(try NotificationCatalogParser.events(invalid))
    }

    func testAlertRowsCarryOnlyKnownKindsAndCapabilitiesEnableAlertMode() throws {
        func row(_ id: Int64, _ kind: String, _ alertKind: String? = nil) -> JSON {
            var raw: JSONObject = ["id": .int(id), "computer_id": .string(computer), "session": "review", "kind": .string(kind), "created_at": 1_000.0]
            if let alertKind { raw["alert_kind"] = .string(alertKind) }
            return .object(raw)
        }
        let events = try NotificationCatalogParser.events(["events": [row(1, "attention"), row(2, "alert", "completed"), row(3, "alert", "future"), row(4, "attention", "error")]])
        XCTAssertEqual([nil, "completed", nil, nil], events.map(\.alertKind))
        XCTAssertTrue(NotificationCatalogParser.presenceAlerts(["features": ["inspect_after", "presence_alerts"]]))
        XCTAssertFalse(NotificationCatalogParser.presenceAlerts(["features": ["inspect_after"]]))
        XCTAssertFalse(NotificationCatalogParser.presenceAlerts([:]))
    }
}
