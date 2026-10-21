package app.zerus.mobile

import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Test

class InspectionFlightsTest {
    private fun checkReads(block: suspend CoroutineScope.() -> Unit) = runBlocking { withTimeout(5_000) { block() } }
    private class Reads(private val scope: CoroutineScope) {
        val flights = mutableMapOf<String, Pair<String, Deferred<Boolean>>>()
        val requested = CompletableDeferred<Unit>()
        val response = CompletableDeferred<Boolean>()
        val effects = mutableListOf<String>()
        var current = true
        var ownerCurrent: () -> Boolean = { true }
        var onCurrent: () -> Unit = {}

        fun terminal() = scope.async(start = CoroutineStart.UNDISPATCHED) {
            // Same admission helper and immediate map insertion used by inspectOnce.
            val shared = InspectionFlights.existing(flights,"target",fresh=true,current={ onCurrent();current && ownerCurrent() })
            val task = shared ?: response.also {
                effects += "new exact inspection"
                flights["target"] = "target" to it
                requested.complete(Unit)
            }
            task.await()
        }
    }

    @Test fun sideFlightOutsideDetailBusyCannotSupplyTerminalEvidenceAndDelayedCleanupCannotRemoveNewFlight() = checkReads {
        val reads = Reads(this)
        val old = CompletableDeferred<Boolean>()
        val unrelated = CompletableDeferred<Boolean>()
        reads.flights["target"] = "target" to old
        reads.flights["other"] = "other" to unrelated
        // No detailBusy job owns this shared flight (scheduled settings/context inspection).
        val ordinaryReader = InspectionFlights.existing(reads.flights,"target")
        assertSame(old,ordinaryReader)
        val terminal = reads.terminal()
        assertTrue(reads.effects.isEmpty())
        assertFalse(old.isCancelled)
        old.complete(false)
        reads.requested.await()
        assertFalse(ordinaryReader!!.await())
        assertFalse(terminal.isCompleted)
        assertSame(reads.response,reads.flights["target"]?.second)
        InspectionFlights.retire(reads.flights,"target",old) // delayed invokeOnCompletion callback
        assertSame(reads.response,reads.flights["target"]?.second)
        assertSame(unrelated,reads.flights["other"]?.second)
        reads.response.complete(true)
        assertTrue(terminal.await())
        assertEquals(listOf("new exact inspection"),reads.effects)
    }

    @Test fun replacementIsNeitherDeletedNorAcceptedAsTheExplicitFreshRead() = checkReads {
        val reads = Reads(this)
        val old = CompletableDeferred<Boolean>()
        val replacement = CompletableDeferred<Boolean>()
        val replacementObserved = CompletableDeferred<Unit>()
        reads.flights["target"] = "target" to old
        val terminal = reads.terminal()
        reads.flights["target"] = "target" to replacement
        reads.onCurrent = { if (reads.flights["target"]?.second === replacement) replacementObserved.complete(Unit) }
        old.complete(false)
        // An old completion must not remove the replacement while the fresh reader resumes.
        InspectionFlights.retire(reads.flights,"target",old)
        replacementObserved.await()
        assertSame(replacement,reads.flights["target"]?.second)
        val oldReader = async(start=CoroutineStart.UNDISPATCHED) { replacement.await() }
        assertTrue(reads.effects.isEmpty())
        assertFalse(terminal.isCompleted)
        replacement.complete(false)
        reads.requested.await()
        assertFalse(oldReader.await())
        assertFalse(terminal.isCompleted)
        reads.response.complete(true)
        assertTrue(terminal.await())
        assertEquals(listOf("new exact inspection"),reads.effects)
    }

    @Test fun capacityWaitRechecksNewSameTargetFlightBeforeStartingFreshInspection() = checkReads {
        val reads = Reads(this)
        val capacity = CompletableDeferred<Boolean>()
        reads.flights["first"] = "first" to capacity
        repeat(3) { reads.flights["other-$it"] = "other-$it" to CompletableDeferred<Boolean>() }
        val terminal = reads.terminal()
        val sideFlight = CompletableDeferred<Boolean>()
        val sideFlightObserved = CompletableDeferred<Unit>()
        capacity.complete(false)
        reads.flights.remove("first")
        reads.flights["target"] = "target" to sideFlight
        reads.onCurrent = { if (reads.flights["target"]?.second === sideFlight) sideFlightObserved.complete(Unit) }
        sideFlightObserved.await()
        assertTrue(reads.effects.isEmpty())
        assertFalse(terminal.isCompleted)
        sideFlight.complete(false)
        reads.requested.await()
        assertFalse(terminal.isCompleted)
        reads.response.complete(true)
        assertTrue(terminal.await())
        assertEquals(4,reads.flights.size)
        assertEquals(listOf("new exact inspection"),reads.effects)
    }

    @Test fun failedOrCancelledPriorRpcSettlesBeforeNewExplicitInspection() = checkReads {
        for (cancelled in listOf(false,true)) {
            val reads = Reads(this)
            val old = CompletableDeferred<Boolean>()
            reads.flights["target"] = "target" to old
            val terminal = reads.terminal()
            if (cancelled) old.cancel() else old.completeExceptionally(IOException("Old RPC failed"))
            reads.requested.await()
            assertFalse(terminal.isCompleted)
            reads.response.complete(true)
            assertTrue(terminal.await())
            assertEquals(listOf("new exact inspection"),reads.effects)
        }
    }

    @Test fun cancellingFreshReaderDoesNotCancelSharedReadersOrStartAnotherInspection() = checkReads {
        val reads = Reads(this)
        val old = CompletableDeferred<Boolean>()
        reads.flights["target"] = "target" to old
        val terminal = reads.terminal()
        terminal.cancelAndJoin()
        assertFalse(old.isCancelled)
        assertSame(old,reads.flights["target"]?.second)
        old.complete(false)
        assertFalse(old.await())
        assertTrue(reads.effects.isEmpty())
    }

    @Test fun changedNavigationAfterSharedReadSettlesPreventsFreshNativeRequest() = checkReads {
        val reads = Reads(this)
        val old = CompletableDeferred<Boolean>()
        reads.flights["target"] = "target" to old
        val terminal = reads.terminal()
        reads.current = false
        old.complete(false)
        terminal.join()
        assertTrue(terminal.isCancelled)
        assertTrue(reads.effects.isEmpty())
        assertFalse(old.isCancelled)
    }

    @Test fun closingOuterAvailabilityOwnerInvalidatesIndependentDetailWaiterWithoutCancellingSharedRead() = checkReads {
        val reads = Reads(this)
        val old = CompletableDeferred<Boolean>()
        reads.flights["target"] = "target" to old
        var availabilitySequence = 1L
        val capturedSequence = availabilitySequence
        reads.ownerCurrent = { availabilitySequence == capturedSequence }
        val detailStarted = CompletableDeferred<Deferred<Boolean>>()
        val availability = async(start=CoroutineStart.UNDISPATCHED) {
            // Like refreshActivity's viewModelScope job: the detail reader has an independent owner.
            val detail = reads.terminal()
            detailStarted.complete(detail)
            detail.await()
        }
        val detail = detailStarted.await()
        availabilitySequence++ // Close retires the exact availability preparation before cancelling its outer job.
        availability.cancelAndJoin()
        assertFalse(detail.isCancelled)
        assertFalse(old.isCancelled)
        assertTrue(reads.effects.isEmpty())
        old.complete(false)
        detail.join()
        assertTrue(detail.isCancelled)
        assertFalse(old.await())
        assertTrue(reads.effects.isEmpty())
    }
}
