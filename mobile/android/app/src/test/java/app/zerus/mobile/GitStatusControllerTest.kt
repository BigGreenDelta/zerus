package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class GitStatusControllerTest {
    private class Harness : AutoCloseable {
        data class Call(val request: GitStatusRequest, val response: CompletableDeferred<JSONObject> = CompletableDeferred())
        data class Sleep(val at: Long, val resumed: CompletableDeferred<Unit> = CompletableDeferred())
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
        var now = GitStatusTestData.SECOND
        var available: GitStatusAccess? = GitStatusAccess(
            Pair(Connection("fixture-connection", "Fixture", "https://relay.example.test", "fixture-token"), "fixture-native-machine"), true, true, true)
        val overrides = mutableMapOf<MachineKey, GitStatusAccess?>()
        val starts = mutableListOf<Call>()
        val receipts = mutableListOf<Call>()
        val sleeps = mutableListOf<Sleep>()
        var uncancellable = false
        val controller = GitStatusController(scope, { now }, { if (overrides.containsKey(it)) overrides[it] else available },
            start = { request ->
                val call = Call(request).also(starts::add)
                if (uncancellable) withContext(NonCancellable) { call.response.await() } else call.response.await()
            },
            receipt = { request -> Call(request).also(receipts::add).response.await() },
            pause = { milliseconds ->
                val sleep = Sleep(now + milliseconds * 1_000_000L).also(sleeps::add)
                try { sleep.resumed.await() } finally { sleeps.remove(sleep) }
            })
        fun watch(candidate: GitStatusCandidate = GitStatusTestData.candidate()) = controller.watch(candidate, true)!!
        fun complete(index: Int = starts.lastIndex, snapshot: JSONObject = GitStatusTestData.snapshot()) {
            val call = starts[index]; call.response.complete(GitStatusTestData.completed(call.request, snapshot))
        }
        fun advanceNanos(delta: Long) {
            require(delta >= 0)
            now += delta
            var completed: Boolean
            do {
                val due = sleeps.filter { it.at <= now && !it.resumed.isCompleted }
                due.forEach { it.resumed.complete(Unit) }
                completed = due.isNotEmpty()
            } while (completed)
        }
        fun advanceMillis(value: Long) = advanceNanos(value * 1_000_000L)
        override fun close() {
            controller.setForeground(false)
            scope.cancel()
            (starts + receipts).forEach { it.response.complete(JSONObject().put("state", "failed")) }
            sleeps.toList().forEach { it.resumed.cancel() }
        }
    }

    @Test fun foregroundAndActualViewportAreBothRequiredAndNoRestoreAuthorityExists() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        assertNull(h.controller.watch(candidate, false))
        h.controller.setForeground(true)
        h.advanceMillis(60_000)
        assertEquals(0, h.starts.size)
        h.controller.setForeground(false)
        h.watch(candidate)
        assertEquals(0, h.starts.size)
        assertNull(h.controller.badge(candidate))
        h.controller.setForeground(true)
        assertEquals(1, h.starts.size)
        h.complete()
        assertTrue(h.controller.badge(candidate)!!.synced)
    }

    @Test fun queueAndActualOutstandingCallsAreBoundedAndEachVisibleRegistrationOwnsItsLease() = Harness().use { h ->
        val leases = (0 until 64).map { h.watch(GitStatusTestData.candidate("/fixture/$it")) }
        assertNull(h.controller.watch(GitStatusTestData.candidate("/fixture/65"), true))
        h.controller.setForeground(true)
        assertEquals(64, h.controller.counts.watched)
        assertEquals(2, h.starts.size); assertEquals(2, h.controller.counts.inFlight); assertEquals(62, h.controller.counts.queued)
        h.complete(0)
        assertEquals(3, h.starts.size); assertEquals(2, h.controller.counts.inFlight); assertEquals(61, h.controller.counts.queued)
        leases.forEach { it.close(); it.close() }
        assertEquals(0, h.controller.counts.watched)
        assertEquals(0, h.controller.counts.queued)
        assertEquals(0, h.controller.counts.inFlight)
        val after = h.starts.size
        h.advanceMillis(120_000)
        assertEquals(after, h.starts.size)
    }

    @Test fun identicalPathsDeduplicateOnlyWithinTheExactComputerConnectionAndEvidence() = Harness().use { h ->
        h.controller.setForeground(true)
        val candidate = GitStatusTestData.candidate()
        val first = h.watch(candidate); val second = h.watch(candidate)
        assertEquals(1, h.starts.size)
        first.close(); first.close()
        assertEquals(1, h.controller.counts.inFlight)
        h.complete(0)
        assertTrue(h.controller.badge(candidate)!!.synced)
        h.watch(GitStatusTestData.candidate(connection = "other-connection"))
        h.watch(GitStatusTestData.candidate(computer = "other-computer"))
        assertEquals(3, h.starts.size)
        assertEquals(3, h.starts.map { it.request.key.machine }.toSet().size)
        second.close()
        assertEquals(2, h.controller.counts.watched)
    }

    @Test fun readCadenceIsFifteenSecondsFromStartAndNeverFromReceiptCompletion() = Harness().use { h ->
        h.controller.setForeground(true); h.watch()
        val first = h.starts.single().request
        h.advanceMillis(10_000); h.complete()
        assertEquals(1, h.starts.size)
        h.advanceNanos(5_000_000_000L - 1)
        assertEquals(1, h.starts.size)
        h.advanceNanos(1)
        assertEquals(2, h.starts.size)
        assertEquals(first.originalStartNanos + 15_000_000_000L, h.starts.last().request.originalStartNanos)
        assertNotEquals(first.requestId, h.starts.last().request.requestId)
        h.controller.refresh(); h.controller.refresh()
        assertEquals(2, h.starts.size)
    }

    @Test fun pollingKeepsTheSameRequestAndOriginalClockEvenWhenReceiptArrivesAfterThirtySeconds() = Harness().use { h ->
        h.controller.setForeground(true); val candidate = GitStatusTestData.candidate(); h.watch(candidate)
        val first = h.starts.single()
        h.advanceMillis(20_000)
        first.response.complete(GitStatusTestData.queued(first.request))
        h.advanceMillis(800)
        assertEquals(1, h.receipts.size)
        h.advanceMillis(10_000)
        h.receipts.single().response.complete(GitStatusTestData.completed(first.request))
        val badge = h.controller.badge(candidate)!!
        assertFalse(badge.current); assertFalse(badge.synced)
        assertEquals(first.request, h.receipts.single().request)
        // The next read is allowed by the cadence, but cannot freshen the completed old result.
        assertEquals(2, h.starts.size)
        assertNotEquals(first.request.requestId, h.starts.last().request.requestId)
    }

    @Test fun failedRefreshPreservesThePreviousObservationAgeInsteadOfResettingIt() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); h.watch(candidate); h.complete()
        h.advanceMillis(15_000)
        h.starts.last().response.completeExceptionally(IOException("fixture offline response"))
        assertTrue(h.controller.badge(candidate)!!.current)
        h.advanceMillis(15_000)
        val badge = h.controller.badge(candidate)!!
        assertFalse(badge.current); assertFalse(badge.synced)
        assertTrue(badge.description.contains("Last known"))
    }

    @Test fun wrongOuterRequestInnerRequestAndOriginalPathCannotPublishAnyObservation() {
        for (kind in 0..2) Harness().use { h ->
            val candidate = GitStatusTestData.candidate()
            h.controller.setForeground(true); h.watch(candidate)
            val request = h.starts.single().request
            val result = GitStatusTestData.completed(request)
            when (kind) {
                0 -> result.put("request_id", GitStatusTestData.ID)
                1 -> result.getJSONObject("result").put("request_id", GitStatusTestData.ID)
                2 -> result.getJSONObject("result").put("requested_path", "/fixture/other")
            }
            h.starts.single().response.complete(result)
            assertNull("mismatch=$kind", h.controller.badge(candidate))
            assertEquals(0, h.controller.counts.origins)
        }
    }

    @Test fun knownMetadataChangesAndMissingProofCannotBorrowAFormerCheckoutObservation() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); val first = h.watch(candidate); h.complete()
        assertTrue(h.controller.badge(candidate)!!.current)
        first.close()
        val replacement = candidate.copy(evidence = candidate.evidence.copy(branch = "replacement"))
        h.watch(replacement)
        assertNull(h.controller.badge(replacement))
        h.advanceMillis(15_000) // The same host/path cadence also applies after an evidence change.
        h.complete(snapshot = GitStatusTestData.snapshot()) // old branch, despite matching request/path
        assertFalse(h.controller.badge(replacement)!!.current)
        assertFalse(h.controller.badge(replacement)!!.synced)
        assertNotEquals(h.starts.first().request.generation, h.starts.last().request.generation)
    }

    @Test fun missingOfflineOrUnsupportedComputerAndRelayNeverStartARead() {
        Harness().use { h ->
            h.controller.setForeground(true)
            for (state in listOf(null, h.available!!.copy(online = false), h.available!!.copy(relaySupported = false),
                h.available!!.copy(computerSupported = false))) {
                h.available = state; val lease = h.watch(); h.controller.refresh()
                assertEquals(0, h.starts.size); assertEquals(0, h.controller.counts.queued)
                lease.close()
                h.available = GitStatusAccess("fixture-epoch", true, true, true)
            }
        }
    }

    @Test fun capabilityWithdrawalDuringFlightRejectsResultAndCachedValuesCannotStayCurrent() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); h.watch(candidate); h.complete()
        assertTrue(h.controller.badge(candidate)!!.current)
        h.available = h.available!!.copy(computerSupported = false)
        assertFalse(h.controller.badge(candidate)!!.current)
        h.controller.refresh()
        h.advanceMillis(15_000)
        assertEquals(1, h.starts.size)
        h.available = h.available!!.copy(computerSupported = true); h.controller.refresh()
        assertEquals(2, h.starts.size)
        h.available = null // check response-time access even without a separate refresh callback
        h.complete()
        assertFalse(h.controller.badge(candidate)!!.current)
        assertEquals(1, h.controller.counts.origins)
    }

    @Test fun fullConnectionAndNativeMachineEpochChangesInvalidateEvidenceEvenAtSamePublicIds() {
        for (changed in listOf("url", "token", "native-machine", "missing")) Harness().use { h ->
            val candidate = GitStatusTestData.candidate()
            h.controller.setForeground(true); h.watch(candidate); h.complete()
            val old = h.available!!
            val pair = old.epoch as Pair<*, *>
            val connection = pair.first as Connection
            h.available = when (changed) {
                "url" -> old.copy(epoch = Pair(connection.copy(url = "https://changed.example.test"), pair.second))
                "token" -> old.copy(epoch = Pair(connection.copy(token = "changed-fixture-token"), pair.second))
                "native-machine" -> old.copy(epoch = Pair(connection, "replacement-machine"))
                else -> null
            }
            assertFalse(changed, h.controller.badge(candidate)!!.current)
            h.controller.refresh()
            h.available = old; h.controller.refresh()
            assertFalse("Restoring IDs must not restore old generation authority", h.controller.badge(candidate)!!.current)
            h.advanceMillis(15_000); h.complete()
            assertTrue(h.controller.badge(candidate)!!.synced)
        }
    }

    @Test fun foregroundLossNeverPromotesPreviousCacheOnResumeAndOnlyFreshResultCanRestoreIt() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); h.watch(candidate); h.complete()
        assertTrue(h.controller.badge(candidate)!!.synced)
        h.controller.setForeground(false)
        assertFalse(h.controller.badge(candidate)!!.current)
        h.advanceMillis(5_000)
        assertEquals(1, h.starts.size)
        h.controller.setForeground(true)
        assertFalse(h.controller.badge(candidate)!!.current)
        h.advanceMillis(10_000)
        assertEquals(2, h.starts.size)
        h.complete()
        assertTrue(h.controller.badge(candidate)!!.synced)
    }

    @Test fun canceledUncooperativeCallsRetainTheirSlotsAndLateResponsesNeverReachNewGeneration() = Harness().use { h ->
        h.uncancellable = true
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); h.watch(candidate)
        h.watch(GitStatusTestData.candidate("/fixture/second"))
        h.watch(GitStatusTestData.candidate("/fixture/third"))
        assertEquals(2, h.starts.size)
        h.controller.setForeground(false); h.advanceMillis(20_000); h.controller.setForeground(true)
        assertEquals(2, h.starts.size); assertEquals(2, h.controller.counts.inFlight)
        h.complete(0)
        assertNull(h.controller.badge(candidate))
        assertEquals(3, h.starts.size); assertEquals(2, h.controller.counts.inFlight)
        h.complete(1)
        assertEquals(4, h.starts.size); assertEquals(2, h.controller.counts.inFlight)
        h.complete(2)
        assertNotEquals(h.starts[0].request.generation, h.starts[2].request.generation)
        assertTrue(h.controller.counts.inFlight <= 2)
    }

    @Test fun cacheAndOriginalClockMapRemainBoundedWhileVisibilityMovesAcrossManyPaths() = Harness().use { h ->
        h.controller.setForeground(true)
        repeat(140) { index ->
            val lease = h.watch(GitStatusTestData.candidate("/fixture/$index"))
            h.complete()
            lease.close()
            assertTrue(h.controller.counts.cached <= 128)
            assertTrue(h.controller.counts.origins <= 130)
            assertEquals(0, h.controller.counts.inFlight)
        }
        assertEquals(128, h.controller.counts.cached)
        assertEquals(128, h.controller.counts.origins)
        assertNull(h.controller.badge(GitStatusTestData.candidate("/fixture/0")))
        val retained = GitStatusTestData.candidate("/fixture/139")
        // No active registration means even a retained process observation is last known.
        assertFalse(h.controller.badge(retained)!!.current)
        h.watch(retained)
        assertFalse(h.controller.badge(retained)!!.current)
        assertEquals(140, h.starts.size) // Removing/recreating a card cannot bypass cadence.
    }

    @Test fun receiptPollingIsBoundedAndEveryPollUsesOriginalIdentityWithoutRepostingIt() = Harness().use { h ->
        h.controller.setForeground(true); h.watch()
        val original = h.starts.single()
        original.response.complete(GitStatusTestData.queued(original.request))
        repeat(35) { index ->
            h.advanceMillis(800)
            assertEquals(index + 1, h.receipts.size)
            h.receipts.last().response.complete(GitStatusTestData.queued(original.request))
        }
        assertEquals(35, h.receipts.size)
        assertTrue(h.receipts.all { it.request == original.request })
        assertEquals(1, h.starts.count { it.request.requestId == original.request.requestId })
        assertTrue(h.controller.counts.inFlight <= 2)
        assertTrue(h.starts.map { it.request.requestId }.distinct().size == h.starts.size)
    }

    @Test fun expiryPublishesARevisionWithoutUsingAnyHostSampleTimestamp() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true); h.watch(candidate)
        h.complete(snapshot = GitStatusTestData.snapshot().put("sampled_at", 9E15).put("remote_checked_at", 9E15).put("remote_age_seconds", 59))
        assertTrue(h.controller.badge(candidate)!!.verified)
        val revision = h.controller.revision.value
        h.advanceMillis(1_000)
        assertTrue(h.controller.revision.value > revision)
        assertTrue(h.controller.badge(candidate)!!.current)
        assertFalse(h.controller.badge(candidate)!!.verified)
        assertEquals(1, h.starts.size)
    }

    @Test fun invalidAndKnownNonrepositoryCandidatesNeverEnterTheQueue() = Harness().use { h ->
        h.controller.setForeground(true)
        val candidate = GitStatusTestData.candidate()
        assertNull(h.controller.watch(candidate.copy(key = candidate.key.copy(requestedPath = "relative")), true))
        assertNull(h.controller.watch(candidate.copy(evidence = candidate.evidence.copy(metadataState = "not_repo")), true))
        assertEquals(GitStatusController.Counts(0, 0, 0, 0, 0), h.controller.counts)
        assertEquals(0, h.starts.size)
    }

    @Test fun alreadyCanceledScopeCannotLeaveNeverStartedFlightsOrOriginsReserved() = Harness().use { h ->
        h.scope.cancel()
        h.controller.setForeground(true)
        h.watch()
        h.watch(GitStatusTestData.candidate("/fixture/second"))
        h.watch(GitStatusTestData.candidate("/fixture/third"))
        assertEquals(0, h.starts.size)
        assertEquals(0, h.controller.counts.inFlight)
        assertEquals(0, h.controller.counts.origins)
    }

    @Test fun canceledRequestCreationRetainsBoundedCadenceWithoutCreatingAnObservation() = Harness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true)
        h.watch(candidate).close() // start entered, but no native receipt was observed
        assertEquals(0, h.controller.counts.inFlight)
        assertNull(h.controller.badge(candidate))
        h.watch(candidate)
        assertEquals("A fresh card must not bypass the existing request-start cadence", 1, h.starts.size)
        h.advanceMillis(14_999)
        assertEquals(1, h.starts.size)
        h.advanceMillis(1)
        assertEquals(2, h.starts.size)
        assertNotEquals(h.starts[0].request.requestId, h.starts[1].request.requestId)
    }

    @Test fun checkoutEvidenceChurnCannotBypassHostPathCadenceOrBorrowOldObservation() {
        val first = GitStatusTestData.candidate()
        val replacements = listOf(first.evidence.copy(branch = "replacement"), first.evidence.copy(root = "/fixture/replacement"),
            first.evidence.copy(detached = true, branch = ""), first.evidence.copy(metadataState = "unavailable"))
        for (evidence in replacements) for (completed in listOf(false, true)) Harness().use { h ->
            h.controller.setForeground(true)
            val lease = h.watch(first)
            if (completed) h.complete()
            lease.close()
            val replacement = first.copy(evidence = evidence)
            h.watch(replacement)
            assertNull("A different evidence generation cannot borrow the old result", h.controller.badge(replacement))
            assertEquals("Same host/path must retain cadence through $evidence, completed=$completed", 1, h.starts.size)
            h.advanceMillis(14_999)
            assertEquals(1, h.starts.size)
            h.advanceMillis(1)
            assertEquals(2, h.starts.size)
            assertEquals(first.key, h.starts.last().request.key)
            assertEquals(evidence, h.starts.last().request.evidence)
            h.complete(snapshot = GitStatusTestData.snapshot().put("root", evidence.root).put("branch", evidence.branch).put("detached", evidence.detached))
            assertEquals(evidence.metadataState == "ok", h.controller.badge(replacement)!!.current)
            if (completed) assertFalse("Old generation cannot become Current after fresh evidence", h.controller.badge(first)!!.current)
        }
    }

    @Test fun simultaneousDifferentEvidenceForSameHostPathCannotDrainQueueIntoDuplicateStarts() = Harness().use { h ->
        val first = GitStatusTestData.candidate()
        val second = first.copy(evidence = first.evidence.copy(branch = "replacement"))
        h.watch(first); h.watch(second)
        h.controller.setForeground(true)
        assertEquals("Queued candidates must recheck shared host/path cadence when starting", 1, h.starts.size)
        assertEquals(1, h.controller.counts.inFlight)
        assertNull(h.controller.badge(second))
        // Different connections and computers remain independent, including identical path text.
        h.watch(GitStatusTestData.candidate(connection = "other-connection"))
        assertEquals(2, h.starts.size)
        h.watch(GitStatusTestData.candidate(computer = "other-computer"))
        h.complete(1)
        assertEquals(3, h.starts.size)
        assertEquals(3, h.starts.map { it.request.key.machine }.toSet().size)
    }

    @Test fun conflictingVisibleEvidenceTakesTurnsAtHostPathCadenceWithoutBorrowingObservations() = Harness().use { h ->
        val first = GitStatusTestData.candidate()
        val second = first.copy(evidence = first.evidence.copy(branch = "second"))
        val third = first.copy(evidence = first.evidence.copy(branch = "third"))
        h.watch(first); h.watch(second); h.watch(third)
        h.controller.setForeground(true)
        assertEquals(1, h.starts.size)
        assertEquals(first.evidence, h.starts[0].request.evidence)
        h.complete(0)
        assertNull(h.controller.badge(second)); assertNull(h.controller.badge(third))
        h.advanceMillis(15_000)
        assertEquals(2, h.starts.size)
        assertEquals("Second visible proof must eventually receive its own request", second.evidence, h.starts[1].request.evidence)
        assertNull(h.controller.badge(second)); assertNull(h.controller.badge(third))
        h.complete(1, GitStatusTestData.snapshot().put("branch", "second"))
        assertTrue(h.controller.badge(second)!!.current)
        assertNull(h.controller.badge(third))
        h.advanceMillis(15_000)
        assertEquals(3, h.starts.size)
        assertEquals("Third visible proof must receive the next bounded turn", third.evidence, h.starts[2].request.evidence)
        h.complete(2, GitStatusTestData.snapshot().put("branch", "third"))
        assertTrue(h.controller.badge(third)!!.current)
        assertTrue(h.starts.all { it.request.key == first.key })
        assertEquals(15_000_000_000L, h.starts[1].request.originalStartNanos - h.starts[0].request.originalStartNanos)
        assertEquals(15_000_000_000L, h.starts[2].request.originalStartNanos - h.starts[1].request.originalStartNanos)
    }
}
