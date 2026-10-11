package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class NotificationPolicyTest {
    private val now = 1_000.0
    private val preferences = NotificationPreferences(generation = 0)
    private fun slot(name: String = "review", workspace: String = "workspace") = NotificationSlot(workspace, "computer", name)
    private fun current(name: String = "review", workspace: String = "workspace", target: String = "run/conversation",
        identity: String = "question-1", kind: SessionAlertKind? = SessionAlertKind.Input, idle: Boolean = false) =
        NotificationCurrent(slot(name, workspace), target, "Review requested", "QA laptop", kind, identity, idle)
    private fun event(id: Long = 1, name: String = "review", kind: String = "attention", at: Double = now) =
        NotificationEvent(id, "computer", name, kind, at)
    private fun ready(values: List<NotificationCurrent> = listOf(current())) =
        NotificationPolicy.plan(NotificationState(bootstrap = false, settingsGeneration = 0), values, emptyList(), preferences, now - 10).state
    private fun plan(state: NotificationState, value: NotificationCurrent, valueEvent: NotificationEvent? = event(),
        prefs: NotificationPreferences = preferences, at: Double = now) =
        NotificationPolicy.plan(state, listOf(value), valueEvent?.let { listOf(value.slot to it) }.orEmpty(), prefs, at)
    private fun roundtrip(state: NotificationState) = NotificationStateCodec.decode(JSONObject(NotificationStateCodec.encode(state).toString()))
    private fun alert(id: Long = 2, name: String = "review", kind: String = "attention", at: Double = now) =
        NotificationEvent(id, "computer", name, "alert", at, kind)
    private fun alertPlan(state: NotificationState, value: NotificationCurrent, valueEvent: NotificationEvent?,
        prefs: NotificationPreferences = preferences, at: Double = now) =
        NotificationPolicy.plan(state, listOf(value), valueEvent?.let { listOf(value.slot to it) }.orEmpty(), prefs, at, alertsOnly = true)

    @Test fun defaultsAndIndependentTypesDoNotEnableFinished() {
        assertTrue(preferences.allows(SessionAlertKind.Input))
        assertTrue(preferences.allows(SessionAlertKind.Error))
        assertFalse(preferences.allows(SessionAlertKind.Finished))
        val errorsOnly = preferences.copy(input = false, finished = true)
        assertFalse(errorsOnly.allows(SessionAlertKind.Input))
        assertTrue(errorsOnly.allows(SessionAlertKind.Error))
        assertTrue(errorsOnly.allows(SessionAlertKind.Finished))
        SessionAlertKind.entries.forEach { assertFalse(errorsOnly.copy(master = false).allows(it)) }
    }

    @Test fun moreThanOnePageRetainsCursorAndCoalescesExactSlot() {
        var state = NotificationState()
        for (start in listOf(1, 101, 201)) {
            val end = minOf(start + 99, 250)
            state = roundtrip(NotificationPolicy.ingest(state, (start..end).map { event(it.toLong()) }).state)
        }
        assertEquals(250L, state.cursor)
        assertEquals(1, state.pending.size)
        assertEquals(250L, state.pending.single().id)
        val drained = NotificationPolicy.ingest(state, emptyList()).state
        val baseline = plan(drained, current(), event(250))
        assertFalse(baseline.state.bootstrap)
        assertTrue(baseline.desired.none { it.fresh })
    }

    @Test fun capacityDoesNotConsumeUnadmittedEventAndRetrySurvivesRestart() {
        val first = NotificationPolicy.ingest(NotificationState(bootstrap = false),
            (1L..100L).map { event(it, "session-$it") })
        val second = NotificationPolicy.ingest(roundtrip(first.state),
            (101L..200L).map { event(it, "session-$it") })
        assertTrue(second.full)
        assertEquals(128L, second.state.cursor)
        assertEquals(128, second.state.pending.size)
        val observed = NotificationPolicy.plan(roundtrip(second.state),
            (1..200).map { current("session-$it") }, emptyList(), preferences, now).state
        val retry = NotificationPolicy.ingest(observed, (129L..200L).map { event(it, "session-$it") })
        assertEquals(200L, retry.state.cursor)
        assertEquals(129L, retry.state.pending.first().id)
        assertEquals(72, retry.state.pending.size)
    }

    @Test fun bootstrapStaysSilentAcrossBacklogPagesAndCatalogReconciliation() {
        val page = NotificationPolicy.ingest(NotificationState(), listOf(event()))
        val first = plan(page.state, current(), event())
        assertTrue(first.state.bootstrap)
        assertTrue(first.desired.none { it.fresh })
        val final = plan(NotificationPolicy.ingest(first.state, emptyList()).state, current(), event(2))
        assertFalse(final.state.bootstrap)
        assertTrue(final.desired.none { it.fresh })
    }

    @Test fun sameQuestionRestartAndDismissalDoNotBecomeFreshAgain() {
        val changed = current(identity = "question-2")
        val first = plan(ready(), changed)
        assertTrue(first.desired.single().fresh)
        val reserved = roundtrip(NotificationPolicy.reserve(first.state, first.desired.single(), true, 10_000))
        val duplicate = plan(reserved, changed, event(2), at = now + 1)
        assertFalse(duplicate.desired.single().fresh)
        // Absence from Android's active set is a dismissal, not a new occurrence.
        assertEquals(reserved.records.single().postedFingerprint, duplicate.desired.single().fingerprint)
    }

    @Test fun newRunReplacesSameSlotAndTwoWorkspacesRemainSeparate() {
        val changed = current(target = "new-run/new-conversation")
        val next = plan(ready(), changed)
        assertTrue(next.desired.single().fresh)
        assertEquals(slot(), next.desired.single().current.slot)
        val foreign = current(workspace = "other")
        val combined = NotificationPolicy.plan(ready(listOf(current(), foreign)), listOf(changed, foreign.copy(identity = "question-2")),
            listOf(changed.slot to event(), foreign.slot to event()), preferences, now)
        assertEquals(2, combined.desired.size)
        assertEquals(2, combined.desired.map { it.current.slot.tag }.toSet().size)
    }

    @Test fun oldCompletionCannotOverrideCurrentErrorOrAnotherRun() {
        val error = current(kind = SessionAlertKind.Error, identity = "provider-error")
        assertEquals(SessionAlertKind.Error, plan(ready(), error, event(kind = "completed"), preferences.copy(finished = true)).desired.single().kind)
        val idle = current(kind = null, idle = true)
        assertTrue(plan(ready(), idle, event(kind = "completed", at = now - 121), preferences.copy(finished = true)).desired.isEmpty())
        assertTrue(plan(ready(), idle.copy(target = "other-run"), event(kind = "completed"), preferences.copy(finished = true)).desired.isEmpty())
    }

    @Test fun currentCompletionRequiresPriorObservationAndExplicitChoice() {
        val idle = current(kind = null, idle = true)
        assertTrue(plan(ready(), idle, event(kind = "completed")).desired.isEmpty())
        val allowed = plan(ready(), idle, event(kind = "completed"), preferences.copy(finished = true))
        assertEquals(SessionAlertKind.Finished, allowed.desired.single().kind)
        assertTrue(allowed.desired.single().fresh)
    }

    @Test fun preferenceChangesReconcileSilentlyAndDisabledOccurrenceIsNotReplayed() {
        val input = current(identity = "question-2")
        val disabled = plan(ready(), input, event(), preferences.copy(input = false, generation = 1))
        assertTrue(disabled.desired.isEmpty())
        val enabled = plan(roundtrip(disabled.state), input, event(2), preferences.copy(generation = 2))
        assertTrue(enabled.reconcile)
        assertFalse(enabled.desired.single().fresh)
    }

    @Test fun offlineRetainsExistingCardButPausedAndMissingCancelIt() {
        val changed = current(identity = "question-2")
        val first = plan(ready(), changed)
        val posted = NotificationPolicy.reserve(first.state, first.desired.single(), false, 0)
        val offline = plan(posted, changed.copy(online = false))
        assertEquals(setOf(slot().key), offline.retained)
        assertTrue(offline.desired.isEmpty())
        assertTrue(plan(posted, changed.copy(live = false)).retained.isEmpty())
        assertTrue(NotificationPolicy.plan(posted, emptyList(), emptyList(), preferences, now).retained.isEmpty())
    }

    @Test fun sustainedNonemptyStreamCanSoundAgainAfterCooldown() {
        val changed = current(identity = "question-2")
        val first = plan(NotificationPolicy.ingest(ready(), listOf(event())).state, changed)
        assertTrue(NotificationPolicy.canSound(first.state, 10_000))
        val posted = NotificationPolicy.reserve(first.state, first.desired.single(), true, 10_000)
        val nextPage = NotificationPolicy.ingest(posted, listOf(event(2)))
        val second = plan(nextPage.state, changed.copy(identity = "question-3"), event(2), at = now + 31)
        assertFalse(NotificationPolicy.canSound(second.state, 39_999))
        assertTrue(NotificationPolicy.canSound(second.state, 40_000))
    }

    @Test fun rebootElapsedClockCannotBypassCooldown() {
        val state = NotificationState(lastSoundElapsed = 80_000)
        val guarded = NotificationPolicy.clockGuard(state, 100)
        assertFalse(NotificationPolicy.canSound(guarded, 100))
        assertFalse(NotificationPolicy.canSound(guarded, 30_099))
        assertTrue(NotificationPolicy.canSound(guarded, 30_100))
    }

    @Test fun boundedRegistryDoesNotResurrectEvictedOccurrences() {
        val values = (1..300).map { current("session-$it") }
        val first = NotificationPolicy.plan(NotificationState(bootstrap = false, settingsGeneration = 0), values, emptyList(), preferences, now)
        assertEquals(256, first.state.records.size)
        assertTrue(first.state.truncated)
        assertTrue(first.overflow)
        val omitted = values.first { it.slot.key !in first.state.records.map { record -> record.slot.key }.toSet() }
        val narrowed = plan(roundtrip(first.state), omitted, null, at = now + 1)
        assertTrue(narrowed.desired.isEmpty())
        val changed = plan(narrowed.state, omitted.copy(identity = "new-question"), event(301, omitted.slot.session), at = now + 2)
        assertTrue(changed.desired.single().fresh)
    }

    @Test fun durableCodecPreservesReservedDeliveryAndRejectsOversizedState() {
        val first = plan(ready(), current(identity = "question-2"))
        val state = NotificationPolicy.reserve(first.state.copy(cursor = 250, pending = listOf(event(251))), first.desired.single(), true, 10_000)
        assertEquals(state, roundtrip(state))
        val raw = NotificationStateCodec.encode(state.copy(records = List(257) { state.records.single() }))
        assertThrows(IllegalArgumentException::class.java) { NotificationStateCodec.decode(raw) }
    }
    @Test fun interruptedPlanRemainsDurableDeliveryWorkBeforeAnyReserve() {
        val changed = current(identity = "question-2")
        val planned = plan(NotificationPolicy.ingest(ready(), listOf(event())).state, changed)
        assertTrue(planned.state.pending.isEmpty())
        assertTrue(planned.state.deliveryNeeded)
        val restart = roundtrip(planned.state)
        val emptyPage = NotificationPolicy.ingest(restart, emptyList()).state
        assertTrue(emptyPage.deliveryNeeded)
        val retry = plan(emptyPage, changed, null)
        assertEquals(SessionAlertKind.Input, retry.desired.single().kind)
        assertFalse(retry.desired.single().fresh)
        assertTrue(retry.state.records.single().postedFingerprint.isEmpty())
    }

    @Test fun unrelatedPreferenceChangeDoesNotRestoreDismissedOtherType() {
        val changed = current(identity = "question-2")
        val first = plan(ready(), changed)
        val posted = roundtrip(NotificationPolicy.reserve(first.state, first.desired.single(), false, 0))
        val unrelated = plan(posted, changed, null, preferences.copy(generation = 1, finished = true, finishedEnabledAt = 1))
        assertFalse(SessionAlertKind.Input in unrelated.restoreKinds)
        assertEquals(setOf(SessionAlertKind.Finished), unrelated.restoreKinds)
        val reenabled = plan(unrelated.state, changed, null, preferences.copy(generation = 2, inputEnabledAt = 2))
        assertEquals(setOf(SessionAlertKind.Input), reenabled.restoreKinds)
        assertFalse(reenabled.desired.single().fresh)
    }

    @Test fun offlineUnpostedReservationRetiresRetryWithoutDiscardingExistingActiveCard() {
        val changed = current(identity = "question-2")
        val first = plan(ready(), changed)
        val reserved = roundtrip(NotificationPolicy.reserve(first.state, first.desired.single(), false, 0))
        val offline = plan(reserved, changed.copy(online = false), null)
        assertTrue(offline.state.pendingPosts.isEmpty())
        assertTrue(offline.state.records.single().postedFingerprint.isEmpty())
        val present = NotificationPolicy.plan(reserved, listOf(changed.copy(online = false)), emptyList(), preferences, now, setOf(changed.slot.key))
        assertEquals(reserved.records.single().postedFingerprint, present.state.records.single().postedFingerprint)
        assertTrue(present.retained.contains(changed.slot.key))
    }

    @Test fun overflowSummaryKindsAreDurableButFinishedNeverCreatesOverflowSummary() {
        val values = (1..300).map { current("session-$it") }
        val bounded = NotificationPolicy.plan(NotificationState(bootstrap = false, settingsGeneration = 0), values, emptyList(), preferences, now)
        assertEquals(setOf(SessionAlertKind.Input), bounded.overflowKinds)
        val saved = bounded.state.copy(overflowKinds = bounded.overflowKinds)
        assertEquals(saved, roundtrip(saved))
    }

    @Test fun reenabledDismissedTypeSurvivesPlanCommitBeforeReserve() {
        val changed = current(identity = "question-2")
        val initial = plan(ready(), changed)
        val posted = NotificationPolicy.reserve(initial.state, initial.desired.single(), false, 0).copy(pendingPosts = emptySet(), deliveryNeeded = false)
        val enabled = preferences.copy(generation = 1, inputEnabledAt = 1)
        val committed = plan(posted, changed, null, enabled)
        assertTrue(SessionAlertKind.Input in committed.restoreKinds)
        val restarted = plan(NotificationPolicy.ingest(roundtrip(committed.state), emptyList()).state, changed, null, enabled)
        assertTrue(SessionAlertKind.Input in restarted.restoreKinds)
        assertFalse(restarted.desired.single().fresh)
    }

    @Test fun optedInCompletionSurvivesPlanCommitBeforeItsFirstReserve() {
        val idle = current(kind = null, idle = true)
        val enabled = preferences.copy(finished = true)
        val committed = plan(ready(), idle, event(kind = "completed"), enabled)
        assertEquals(SessionAlertKind.Finished, committed.desired.single().kind)
        val restarted = plan(NotificationPolicy.ingest(roundtrip(committed.state), emptyList()).state, idle, null, enabled, now + 1)
        assertEquals(SessionAlertKind.Finished, restarted.desired.single().kind)
        assertFalse(restarted.desired.single().fresh)
    }

    @Test fun restoredFirstSlotKeepsItsEvidenceWhileRemainingSlotWaitsAcrossRestart() {
        val first = current("first", identity = "question-2")
        val second = current("second", identity = "question-2")
        val enabled = preferences.copy(generation = 7, inputEnabledAt = 7)
        val planned = NotificationPolicy.plan(ready(listOf(first, second)), listOf(first, second), emptyList(), enabled, now)
        val partiallyDelivered = NotificationPolicy.reserve(planned.state, planned.desired.first(), false, 0, enabled.inputEnabledAt)
            .copy(pendingPosts = emptySet())
        val restarted = roundtrip(partiallyDelivered)
        assertTrue(restarted.deliveryNeeded)
        assertEquals(7L, restarted.records.first { it.slot == planned.desired.first().current.slot }.restoredAt)
        val retry = NotificationPolicy.plan(restarted, listOf(first, second), emptyList(), enabled, now + 1)
        assertTrue(SessionAlertKind.Input in retry.restoreKinds)
        assertTrue(retry.state.records.first { it.slot != planned.desired.first().current.slot }.restoredAt < enabled.inputEnabledAt)
        assertTrue(retry.desired.none { it.fresh })
    }

    @Test fun interruptedCompletionDoesNotFollowNewTargetExpireOrOverrideError() {
        val idle = current(kind = null, idle = true)
        val enabled = preferences.copy(finished = true)
        val pending = roundtrip(plan(ready(), idle, event(kind = "completed"), enabled).state)
        assertTrue(plan(pending, idle.copy(target = "new-run"), null, enabled, now + 1).desired.isEmpty())
        assertTrue(plan(pending, idle, null, enabled, now + 121).desired.isEmpty())
        val error = plan(pending, idle.copy(kind = SessionAlertKind.Error, identity = "new-error"), null, enabled, now + 1)
        assertEquals(SessionAlertKind.Error, error.desired.single().kind)
    }

    @Test fun presenceAlertsPostOnlyForNeedAlertRows() {
        val changed = current(identity = "question-2")
        val waiting = alertPlan(ready(), changed, event())
        assertTrue(waiting.desired.isEmpty())
        assertTrue(waiting.retained.isEmpty())
        // A need observed at a computer stays held across later batches without its alert.
        assertTrue(alertPlan(waiting.state, changed, null, at = now + 1).desired.isEmpty())
        assertTrue(alertPlan(waiting.state, changed, alert(kind = "completed")).desired.isEmpty())
        val alerted = alertPlan(waiting.state, changed, alert(3))
        assertEquals(SessionAlertKind.Input, alerted.desired.single().kind)
        assertTrue(alerted.desired.single().fresh)
    }

    @Test fun presenceAlertsAnnounceInputAndErrorCardsFromEitherNeedAlert() {
        // The relay names a need attention before error; the phone shows error first.
        val error = current(kind = SessionAlertKind.Error, identity = "failed-recovery-with-question")
        val fromAttention = alertPlan(ready(), error, alert(kind = "attention"))
        assertEquals(SessionAlertKind.Error, fromAttention.desired.single().kind)
        assertTrue(fromAttention.desired.single().fresh)
        val input = current(identity = "question-2")
        val fromError = alertPlan(ready(), input, alert(kind = "error"))
        assertEquals(SessionAlertKind.Input, fromError.desired.single().kind)
        assertTrue(fromError.desired.single().fresh)
    }

    @Test fun presenceAlertsUpdatePostedCardSilentlyAndHoldNewNeedWithoutAlert() {
        val changed = current(identity = "question-2")
        val alerted = alertPlan(ready(), changed, alert())
        val posted = roundtrip(NotificationPolicy.reserve(alerted.state, alerted.desired.single(), true, 10_000))
        val refresh = alertPlan(posted, changed, event(3), at = now + 1)
        assertEquals(setOf(changed.slot.key), refresh.retained)
        assertFalse(refresh.desired.single().fresh)
        // Details the relay never announces (a shorter question list, recovery or
        // provider error text) update the posted card silently, as without the feature.
        val detail = changed.copy(identity = "question-2-details")
        for (batch in listOf(null, event(4, kind = "completed"))) {
            val updated = alertPlan(refresh.state, detail, batch, at = now + 2)
            assertEquals(setOf(changed.slot.key), updated.retained)
            assertEquals(SessionAlertKind.Input, updated.desired.single().kind)
            assertFalse(updated.desired.single().fresh)
            assertNotEquals(posted.records.single().postedFingerprint, updated.desired.single().fingerprint)
        }
        // A posted error card that turns back into a pending question stays as well.
        val errorCard = current(kind = SessionAlertKind.Error, identity = "error-1")
        val errorAlerted = alertPlan(ready(), errorCard, alert(kind = "error"))
        val errorPosted = NotificationPolicy.reserve(errorAlerted.state, errorAlerted.desired.single(), true, 10_000)
        assertEquals(SessionAlertKind.Input, alertPlan(errorPosted, changed, null, at = now + 1).desired.single().kind)
        // A new need the relay announced while the owner is at a computer withdraws
        // the card until its alert, and so does another run without one.
        val updated = alertPlan(refresh.state, detail, null, at = now + 2)
        val reposted = roundtrip(NotificationPolicy.reserve(updated.state, updated.desired.single(), false, 20_000))
        val next = changed.copy(identity = "question-3")
        val replaced = alertPlan(reposted, next, event(5), at = now + 3)
        assertTrue(replaced.desired.isEmpty())
        assertTrue(replaced.retained.isEmpty())
        val newRun = alertPlan(reposted, changed.copy(target = "new-run/conversation", identity = "question-3"), null, at = now + 3)
        assertTrue(newRun.desired.isEmpty())
        val announced = alertPlan(replaced.state, next, alert(6), at = now + 4)
        assertEquals(SessionAlertKind.Input, announced.desired.single().kind)
        assertTrue(announced.desired.single().fresh)
    }

    @Test fun presenceAlertsAnnounceCompletionOnlyFromCompletedAlert() {
        val idle = current(kind = null, idle = true)
        val enabled = preferences.copy(finished = true)
        assertTrue(alertPlan(ready(), idle, event(kind = "completed"), enabled).desired.isEmpty())
        assertEquals(SessionAlertKind.Finished, alertPlan(ready(), idle, alert(kind = "completed"), enabled).desired.single().kind)
    }

    @Test fun withoutPresenceAlertsAlertRowsNeitherAnnounceNorHideCompletion() {
        val idle = current(kind = null, idle = true)
        val enabled = preferences.copy(finished = true)
        val both = listOf(idle.slot to event(1, kind = "completed"), idle.slot to alert(2, kind = "completed"))
        assertEquals(SessionAlertKind.Finished, NotificationPolicy.plan(ready(), listOf(idle), both, enabled, now).desired.single().kind)
        val onlyAlert = listOf(idle.slot to alert(kind = "completed"))
        assertTrue(NotificationPolicy.plan(ready(), listOf(idle), onlyAlert, enabled, now).desired.isEmpty())
        val input = current(identity = "question-2")
        assertEquals(SessionAlertKind.Input, plan(ready(), input, alert()).desired.single().kind)
    }

    @Test fun alertRowsSurviveLaterRefreshRowsAndCodecRoundtrip() {
        val page = NotificationPolicy.ingest(NotificationState(bootstrap = false), listOf(event(1), alert(2), event(3, kind = "completed")))
        val state = roundtrip(page.state.copy(presenceAlerts = true))
        assertEquals(listOf(2L, 3L), state.pending.map { it.id }.sorted())
        assertEquals("attention", state.pending.single { it.kind == "alert" }.alertKind)
        assertNull(state.pending.single { it.kind == "completed" }.alertKind)
        assertTrue(state.presenceAlerts)
    }

    @Test fun needAndCompletionAlertsSurvivePagesInEitherOrder() {
        for (need in listOf("attention", "error")) for (completionFirst in listOf(false, true)) {
            val signals = if (completionFirst) listOf("completed", need) else listOf(need, "completed")
            var state = ready().copy(presenceAlerts = true)
            for ((index, signal) in signals.withIndex()) {
                state = roundtrip(NotificationPolicy.ingest(state, listOf(alert(index + 1L, kind = signal))).state)
            }
            state = roundtrip(NotificationPolicy.ingest(state, listOf(event(3, kind = "completed"))).state)
            assertEquals(3, state.pending.size)
            val value = current(identity = "question-2")
            val result = NotificationPolicy.plan(state, listOf(value), state.pending.map { value.slot to it },
                preferences, now, alertsOnly = true)
            assertEquals(SessionAlertKind.Input, result.desired.single().kind)
            assertTrue(result.desired.single().fresh)
            assertEquals(signals.indexOf(need) + 1L, result.desired.single().eventId)
        }
    }

    @Test fun delayedNeedAlertRemainsFreshAfterSilentDetailDelivery() {
        val value = current(identity = "question-2")
        val first = alertPlan(ready(), value, alert())
        val posted = NotificationPolicy.reserve(first.state, first.desired.single(), true, 10_000)
            .copy(pendingPosts = emptySet())
        val changed = value.copy(identity = "question-3")
        // Coalescing can leave only a refresh row while the new need is held.
        val detail = alertPlan(posted, changed, event(3, kind = "completed"), at = now + 1)
        assertFalse(detail.desired.single().fresh)
        val silent = roundtrip(NotificationPolicy.reserve(detail.state, detail.desired.single(), false, 40_000)
            .copy(pendingPosts = emptySet()))
        assertEquals(posted.records.single().observedFingerprint, silent.records.single().observedFingerprint)
        val nextBatch = NotificationPolicy.ingest(silent, listOf(alert(4))).state
        val delivered = alertPlan(nextBatch, changed, alert(4), at = now + 31)
        assertTrue(delivered.desired.single().fresh)
        assertTrue(NotificationPolicy.canSound(delivered.state, 40_000))
        val reserved = roundtrip(NotificationPolicy.reserve(delivered.state, delivered.desired.single(), true, 40_000)
            .copy(pendingPosts = emptySet()))
        assertFalse(alertPlan(reserved, changed, alert(5), at = now + 32).desired.single().fresh)
    }
}
