package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ScheduledSettingsRetryTest {
    private val target = Target("fixture-computer", "codex/fixture/settings", "fixture-run", "fixture-conversation", "fixture-workspace")
    private val pending = "11111111-1111-4111-8111-111111111111"
    private val ack = 1_000_000_000L
    private val second = 1_000_000_000L
    private fun args(model: Boolean = true, effort: Boolean = true) = JSONObject().also {
        if (model) it.put("model", "fixture-model")
        if (effort) it.put("effort", "high")
    }
    private fun original(arguments: JSONObject = args(), t: Target = target, id: String = "original", childId: String = "child") =
        SessionAction(id, t, "settings", arguments.toString(), status = "scheduled", pendingId = pending, applyAttemptId = childId)
    private fun child(arguments: JSONObject = args(), t: Target = target, id: String = "child") =
        SessionAction(id, t, "settings", JSONObject(arguments.toString()).put("expected_pending_id", pending).toString(),
            status = "scheduled", pendingId = pending)
    private fun receipt(c: SessionAction = child()) = JSONObject().put("request_id", c.requestId).put("name", c.target.session)
        .put("run_id", c.target.run).put("conversation_id", c.target.conversation).put("status", "scheduled")
        .put("pending_settings_id", pending).put("model", "fixture-model").put("effort", "high").put("apply_when", "ready")
    private fun raw(whenReady: String, t: Target = target) = JSONObject().put("name", t.session).put("run_id", t.run)
        .put("conversation_id", t.conversation).put("pending_settings_id", pending).put("pending_model", "fixture-model")
        .put("pending_effort", "high").put("settings_change_supported", true).put("settings_apply_when", whenReady)
        .put("phase", "idle").put("activity", "idle")
    private data class Cycle(val tracker: ScheduledSettingsRetry, val original: SessionAction, val child: SessionAction)
    private fun cycle(o: SessionAction = original(), c: SessionAction = child(), result: JSONObject = receipt(c)): Cycle {
        val tracker = ScheduledSettingsRetry()
        assertTrue(tracker.register(o, c, ack, result))
        return Cycle(tracker, o, c)
    }
    private fun Cycle.inspect(value: JSONObject, start: Long = ack + 1, now: Long = start + 1,
        verified: Boolean = true, online: Boolean = true, t: Target = target) =
        tracker.inspect(original, child, t, value, start, now, verified, online)
    private fun Cycle.grant(): ScheduledSettingsRetry.Grant {
        assertNull(inspect(raw("ready")))
        return inspect(raw("now"), ack + second) ?: error("Fresh NotReady -> Ready did not grant one retry")
    }

    @Test fun registrationAndRepeatedReadyNeverGrantAnotherAttempt() {
        val cycle = cycle()
        assertTrue(cycle.tracker.tracks(cycle.original))
        repeat(5) { assertNull(cycle.inspect(raw("now"), ack + second * (it + 1))) }
        assertTrue(cycle.tracker.tracks(cycle.original))
    }

    @Test fun hiddenChooserThenReadyGrantsFullTupleOnceAndConsumesBeforeDurableTransition() {
        val cycle = cycle()
        // apply_when=ready is authoritative even if a chooser leaves both status strings idle.
        val grant = cycle.grant()
        assertEquals(cycle.original, grant.original)
        assertEquals(cycle.child, grant.child)
        assertEquals(target, grant.target)
        assertEquals(pending, grant.pendingId)
        assertEquals("fixture-model", grant.model)
        assertEquals("high", grant.effort)
        assertEquals(cycle.original.arguments, grant.originalArguments)
        assertEquals(cycle.child.arguments, grant.childArguments)
        assertNull(cycle.inspect(raw("now"), ack + second * 2))
        val consumed = cycle.tracker.consume(grant)!!
        assertSame(grant, consumed.grant)
        assertEquals(cycle.original, consumed.original)
        assertEquals(cycle.child, consumed.child)
        assertEquals("fixture-model", consumed.model)
        assertEquals("high", consumed.effort)
        assertFalse(cycle.tracker.tracks(cycle.original))
        assertNull(cycle.tracker.consume(grant))
        assertNull(cycle.inspect(raw("ready"), ack + second * 3))
        assertNull(cycle.inspect(raw("now"), ack + second * 4))
    }

    @Test fun blockedPreflightDiscardsReadyAndRequiresANewDistinctTransition() {
        val cycle=cycle();val grant=cycle.grant()
        assertTrue(cycle.tracker.discard(grant))
        assertNull(cycle.tracker.consume(grant))
        assertFalse(cycle.tracker.discard(grant))
        repeat(3) { assertNull(cycle.inspect(raw("now"),ack+second*(it+2))) }
        assertNull(cycle.inspect(raw("ready"),ack+second*5))
        val fresh=cycle.inspect(raw("now"),ack+second*6)!!
        assertNotSame(grant,fresh)
        assertNotNull(cycle.tracker.consume(fresh))
        assertFalse(cycle.tracker.discard(fresh))
        assertNull(cycle.inspect(raw("ready"),ack+second*7))
        assertNull(cycle.inspect(raw("now"),ack+second*8))
    }

    @Test fun busyCanLastLongerThanTenSecondsWhileEachObservedFrameMustBeFresh() {
        val cycle = cycle()
        assertNull(cycle.inspect(raw("ready").put("phase", "working").put("activity", "busy")))
        assertNotNull(cycle.inspect(raw("now"), ack + second * 60))
    }

    @Test fun freshFrameAgeIncludesTheTenSecondBoundaryButRejectsOlderOrInvalidTimes() {
        for ((start, now) in listOf(0L to ack + 2, ack to ack + 1, ack - 1 to ack + 1,
            ack + 2 to ack + 1, ack + 1 to ack + 1 + second * 10 + 1)) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready"), start, now))
            assertNull(cycle.inspect(raw("now"), ack + second * 20))
        }
        val boundary = cycle()
        assertNull(boundary.inspect(raw("ready"), ack + 1, ack + 1 + second * 10))
        assertNotNull(boundary.inspect(raw("now"), ack + second * 11))
        val staleReady = cycle()
        assertNull(staleReady.inspect(raw("ready")))
        assertNull(staleReady.inspect(raw("now"), ack + second, ack + second * 12))
        assertNotNull(staleReady.inspect(raw("now"), ack + second * 13))
    }

    @Test fun observationsMustBeDistinctSequentialAndStartedAfterTheScheduledAck() {
        val cycle = cycle()
        assertNull(cycle.inspect(raw("ready"), ack + 100, ack + 200))
        for (start in listOf(ack - 1, ack, ack + 100, ack + 150, ack + 200))
            assertNull(cycle.inspect(raw("now"), start, ack + 300))
        assertNotNull(cycle.inspect(raw("now"), ack + 301, ack + 302))
    }

    @Test fun unverifiedOrOfflineFramesCannotArmOrGrantARepeat() {
        for ((verified, online) in listOf(false to true, true to false, false to false)) {
            val notArmed = cycle()
            assertNull(notArmed.inspect(raw("ready"), verified = verified, online = online))
            assertNull(notArmed.inspect(raw("now"), ack + second))
            val notGranted = cycle()
            assertNull(notGranted.inspect(raw("ready")))
            assertNull(notGranted.inspect(raw("now"), ack + second, verified = verified, online = online))
        }
    }

    @Test fun noRestoreApiMeansPersistedScheduledRowsAloneNeverBecomeRetryAuthority() {
        val restored = ScheduledSettingsRetry()
        val o = original(); val c = child()
        assertFalse(restored.tracks(o))
        assertNull(restored.inspect(o, c, target, raw("ready"), ack + 1, ack + 2, true, true))
        assertNull(restored.inspect(o, c, target, raw("now"), ack + second, ack + second + 1, true, true))
    }

    @Test fun registrationRequiresTheExactScheduledOriginalToChildDurableRelation() {
        val o = original(); val c = child()
        val badOriginals = listOf(o.copy(status = "sending"), o.copy(status = "uncertain"), o.copy(status = "failed"),
            o.copy(status = "applied"), o.copy(operation = "resume"), o.copy(requestId = ""), o.copy(requestId = c.requestId),
            o.copy(applyAttemptId = ""), o.copy(applyAttemptId = "other-child"), o.copy(pendingId = ""),
            o.copy(pendingId = "different"), o.copy(target = target.copy(run = "different")))
        for (changed in badOriginals) assertFalse(changed.toString(), ScheduledSettingsRetry().register(changed, c, ack, receipt(c)))
        val badChildren = listOf(c.copy(status = "sending"), c.copy(status = "uncertain"), c.copy(status = "failed"),
            c.copy(status = "applied"), c.copy(operation = "resume"), c.copy(applyAttemptId = "later-child"),
            c.copy(pendingId = "different"), c.copy(target = target.copy(conversation = "different")))
        for (changed in badChildren) assertFalse(changed.toString(), ScheduledSettingsRetry().register(o, changed, ack, receipt(changed)))
        assertFalse(ScheduledSettingsRetry().register(o, c, 0, receipt(c)))
    }

    @Test fun nativeScheduledResultMustConfirmExactIdentityAndActualStringFullTuple() {
        val changes: List<Pair<String, Any>> = listOf("request_id" to "other", "name" to "other", "run_id" to "other",
            "conversation_id" to "other", "pending_settings_id" to "other", "status" to "uncertain", "status" to "applied",
            "archive_id" to "archive", "agent_id" to "child", "parent_conversation_id" to "parent",
            "model" to true, "model" to 7, "model" to JSONObject.NULL, "model" to "", "effort" to false,
            "effort" to 1, "effort" to JSONObject.NULL, "apply_when" to "resume", "apply_when" to "now")
        for ((field, value) in changes) assertFalse("$field=$value", ScheduledSettingsRetry().register(original(), child(), ack, receipt().put(field, value)))
        for (field in listOf("request_id", "name", "run_id", "conversation_id", "pending_settings_id", "status", "model", "effort", "apply_when")) {
            val result = receipt().also { it.remove(field) }
            assertFalse("missing $field", ScheduledSettingsRetry().register(original(), child(), ack, result))
        }
    }

    @Test fun partialOriginalConsentStillPinsTheWholeActualNativeResultTuple() {
        for (requested in listOf(args(model = true, effort = false), args(model = false, effort = true), args())) {
            val cycle = cycle(original(requested), child(requested))
            val grant = cycle.grant()
            assertEquals("fixture-model", grant.model)
            assertEquals("high", grant.effort)
        }
        val empty = JSONObject().put("model", "fixture-model").put("effort", "")
        val emptyEffort = cycle(original(empty), child(empty), receipt(child(empty)).put("effort", ""))
        assertNull(emptyEffort.inspect(raw("ready").put("pending_effort", "")))
        assertEquals("", emptyEffort.inspect(raw("now").put("pending_effort", ""), ack + second)!!.effort)
    }

    @Test fun requestedComponentsCannotBeDroppedCoercedChangedOrExpandedOutsideSettings() {
        val o = original(); val c = child()
        val attempts = listOf(JSONObject(c.arguments).put("expected_pending_id", "other"),
            JSONObject(c.arguments).put("model", "other"), JSONObject(c.arguments).put("effort", "low"),
            JSONObject(c.arguments).put("model", true), JSONObject(c.arguments).put("effort", 1),
            JSONObject(c.arguments).put("unrequested_field", "value"), JSONObject(c.arguments).also { it.remove("model") },
            JSONObject(c.arguments).also { it.remove("effort") }, JSONObject(c.arguments).also { it.remove("expected_pending_id") })
        for (changed in attempts) assertFalse(changed.toString(), ScheduledSettingsRetry().register(o, c.copy(arguments = changed.toString()), ack, receipt(c)))
        for (changed in listOf(JSONObject(o.arguments).put("model", "other"), JSONObject(o.arguments).put("effort", false),
            JSONObject(o.arguments).put("expected_pending_id", pending), JSONObject(), JSONObject().put("unknown", "value")))
            assertFalse(changed.toString(), ScheduledSettingsRetry().register(o.copy(arguments = changed.toString()), c, ack, receipt(c)))
        assertFalse(ScheduledSettingsRetry().register(o.copy(arguments = "invalid"), c, ack, receipt(c)))
        assertFalse(ScheduledSettingsRetry().register(o, c.copy(arguments = "invalid"), ack, receipt(c)))
    }

    @Test fun freshNativePendingTupleReplacementRevokesTheCycleEvenIfItLaterReappears() {
        val changes: List<Pair<String, Any>> = listOf("name" to "other", "run_id" to "other", "conversation_id" to "other",
            "pending_settings_id" to "other", "pending_model" to "other", "pending_effort" to "low",
            "pending_model" to true, "pending_effort" to false, "pending_effort" to JSONObject.NULL,
            "archive_id" to "archive", "agent_id" to "child", "parent_conversation_id" to "parent",
            "settings_change_supported" to false, "settings_change_supported" to "true")
        for ((field, value) in changes) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            assertNull("$field=$value", cycle.inspect(raw("now").put(field, value), ack + second))
            assertFalse(cycle.tracker.tracks(cycle.original))
            assertNull(cycle.inspect(raw("now"), ack + second * 2))
        }
        for (field in listOf("name", "run_id", "conversation_id", "pending_settings_id", "pending_model", "pending_effort", "settings_change_supported")) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            assertNull(cycle.inspect(raw("now").also { it.remove(field) }, ack + second))
        }
    }

    @Test fun targetScopeCannotMoveAcrossConnectionComputerSessionRunConversationArchiveOrChild() {
        for (changed in listOf(target.copy(connectionId = "other"), target.copy(computerId = "other"), target.copy(session = "other"),
            target.copy(run = "other"), target.copy(conversation = "other"), target.copy(archiveId = "archive"),
            target.copy(agentId = "child"), target.copy(parentConversation = "parent"))) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            assertNull(changed.toString(), cycle.inspect(raw("now", changed), ack + second, t = changed))
            val o = original(t = changed); val c = child(t = changed)
            if (changed.archiveId.isNotBlank() || changed.agentId.isNotBlank() || changed.parentConversation.isNotBlank())
                assertFalse(ScheduledSettingsRetry().register(o, c, ack, receipt(c)))
        }
    }

    @Test fun latestDurableOriginalAndChildMustStillMatchTheRegisteredProof() {
        for (changeOriginal in listOf(true, false)) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            val o = if (changeOriginal) cycle.original.copy(applyAttemptId = "newer-child") else cycle.original
            val c = if (changeOriginal) cycle.child else cycle.child.copy(status = "uncertain")
            assertNull(cycle.tracker.inspect(o, c, target, raw("now"), ack + second, ack + second + 1, true, true))
            assertFalse(cycle.tracker.tracks(cycle.original))
            assertNull(cycle.inspect(raw("now"), ack + second * 2))
        }
        val cycle = cycle()
        assertFalse(cycle.tracker.tracks(cycle.original.copy(arguments = args(effort = false).toString())))
        assertFalse(cycle.tracker.tracks(cycle.original.copy(applyAttemptId = "other-child")))
    }

    @Test fun resumeMissingAndUnknownReadinessNeverArmOrPreserveARetryCycle() {
        for (whenReady in listOf("resume", "", "unknown")) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            assertNull(cycle.inspect(raw(whenReady), ack + second))
            assertNull(cycle.inspect(raw("now"), ack + second * 2))
            assertFalse(cycle.tracker.tracks(cycle.original))
        }
        val missing = cycle()
        assertNull(missing.inspect(raw("ready").also { it.remove("settings_apply_when") }))
        assertNull(missing.inspect(raw("now"), ack + second))
        for (field in listOf("phase", "activity")) {
            val cycle = cycle()
            assertNull(cycle.inspect(raw("ready")))
            assertNull(cycle.inspect(raw("now").also { it.remove(field) }, ack + second))
        }
    }

    @Test fun repeatedRegistrationCannotResetOrReplayAChildButANewDurableChildStartsANewCycle() {
        val cycle = cycle()
        assertNull(cycle.inspect(raw("ready")))
        assertFalse(cycle.tracker.register(cycle.original, cycle.child, ack + 1, receipt(cycle.child)))
        val grant = cycle.inspect(raw("now"), ack + second)!!
        assertNotNull(cycle.tracker.consume(grant))
        assertFalse(cycle.tracker.register(cycle.original, cycle.child, ack + second * 2, receipt(cycle.child)))
        val nextChild = child(id = "second-child")
        val nextOriginal = cycle.original.copy(applyAttemptId = nextChild.requestId)
        assertFalse(cycle.tracker.register(nextOriginal, nextChild, ack, receipt(nextChild)))
        assertTrue(cycle.tracker.register(nextOriginal, nextChild, ack + second * 2, receipt(nextChild)))
        assertFalse(cycle.tracker.tracks(cycle.original))
        assertTrue(cycle.tracker.tracks(nextOriginal))
        val next = Cycle(cycle.tracker, nextOriginal, nextChild)
        assertNull(next.inspect(raw("now"), ack + second * 3))
        assertNull(next.inspect(raw("ready"), ack + second * 4))
        val nextGrant = next.inspect(raw("now"), ack + second * 5)!!
        assertNull(cycle.tracker.consume(grant))
        assertNotNull(cycle.tracker.consume(nextGrant))
    }

    @Test fun unrequestedResultComponentCannotDriftOnTheNextScheduledChild() {
        val requested = args(effort = false)
        val cycle = cycle(original(requested), child(requested))
        assertNotNull(cycle.tracker.consume(cycle.grant()))
        val nextArgs = args().put("effort", "low")
        val nextChild = child(nextArgs, id = "second-child")
        val nextOriginal = cycle.original.copy(applyAttemptId = nextChild.requestId)
        assertFalse(cycle.tracker.register(nextOriginal, nextChild, ack + second * 2, receipt(nextChild).put("effort", "low")))
    }

    @Test fun processLocalCapacityIsSixtyFourAndEvictedGrantsCannotBeConsumed() {
        val first = cycle()
        val evictedGrant = first.grant()
        val retained = mutableListOf<SessionAction>()
        repeat(64) { index ->
            val t = target.copy(session = "codex/fixture/$index", conversation = "fixture-conversation-$index")
            val o = original(t = t, id = "original-$index", childId = "child-$index")
            val c = child(t = t, id = "child-$index")
            assertTrue(first.tracker.register(o, c, ack, receipt(c)))
            retained += o
        }
        assertFalse(first.tracker.tracks(first.original))
        assertEquals(64, retained.count(first.tracker::tracks))
        assertNull(first.tracker.consume(evictedGrant))
    }

    @Test fun aGrantFromAnotherTrackerCannotBeConsumedEvenWithIdenticalDurableActions() {
        val first = cycle(); val second = cycle()
        val firstGrant = first.grant(); val secondGrant = second.grant()
        assertNull(second.tracker.consume(firstGrant))
        assertNotNull(first.tracker.consume(firstGrant))
        assertNotNull(second.tracker.consume(secondGrant))
    }
}
