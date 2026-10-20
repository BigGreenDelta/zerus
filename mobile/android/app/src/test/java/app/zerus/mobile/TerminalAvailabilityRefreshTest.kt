package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class TerminalAvailabilityRefreshTest {
    /** Held read responses expose requests and their actual published effects separately. */
    private class Reads(private val scope: CoroutineScope) {
        val target = Target("computer", "codex/project/session", "run", "conversation", "workspace")
        val automaticCatalog = CompletableDeferred<Unit>()
        val catalogResponse = CompletableDeferred<Unit>()
        val detailResponse = CompletableDeferred<Unit>()
        val catalogRequested = CompletableDeferred<Unit>()
        val detailRequested = CompletableDeferred<Unit>()
        var catalogFlight: Deferred<Unit> = automaticCatalog
        var detailFlight: Deferred<Unit> = CompletableDeferred<Unit>().apply { complete(Unit) }
        var current = true
        var gateway = emptySet<String>() // cached refusal before the explicit open
        var computer = emptySet<String>()
        var row = JSONObject().put("terminal_supported", false).put("terminal_reason", "Previous pane was unavailable")
        val effects = mutableListOf<String>()

        fun open() = scope.async(start = CoroutineStart.UNDISPATCHED) {
            val verified = TerminalAvailabilityRefresh.run(current = { current }, catalogIdle = { catalogFlight.await() },
                refreshCatalog = {
                    effects += "GET capabilities";effects += "GET computers"
                    catalogFlight = scope.async(start = CoroutineStart.UNDISPATCHED) {
                        catalogResponse.await()
                        gateway = setOf("terminal_snapshot", "terminal_input");computer = gateway
                        effects += "publish discovery"
                    }
                    catalogRequested.complete(Unit)
                }, detailIdle = { detailFlight.await() }, refreshDetail = {
                    effects += "inspect exact session"
                    detailFlight = scope.async(start = CoroutineStart.UNDISPATCHED) {
                        detailResponse.await()
                        row = JSONObject().put("terminal_supported", true)
                        effects += "publish native support"
                    }
                    detailRequested.complete(Unit)
                })
            // This is the UI's read-only screen admission after preparation; refresh itself sends no Terminal input.
            if (verified && TerminalAvailability.reason(target, row, true, gateway, computer).isBlank()) effects += "read screen"
            verified
        }
    }

    @Test fun firstOpenWaitsForAutomaticReadThenNewExplicitDiscoveryAndFreshNativeEvidence() = runBlocking {
        val reads = Reads(this)
        val opening = reads.open()
        assertFalse(opening.isCompleted)
        assertTrue(reads.effects.isEmpty())
        reads.automaticCatalog.complete(Unit)
        reads.catalogRequested.await()
        assertEquals(listOf("GET capabilities", "GET computers"), reads.effects)
        assertFalse(opening.isCompleted)
        assertTrue(reads.gateway.isEmpty())
        reads.catalogResponse.complete(Unit)
        reads.detailRequested.await()
        assertEquals(listOf("GET capabilities", "GET computers", "publish discovery", "inspect exact session"), reads.effects)
        assertFalse(opening.isCompleted)
        assertEquals(false, reads.row.opt("terminal_supported"))
        reads.detailResponse.complete(Unit)
        assertTrue(opening.await())
        assertEquals(listOf("GET capabilities", "GET computers", "publish discovery", "inspect exact session", "publish native support", "read screen"), reads.effects)
        assertFalse(reads.effects.any { it.contains("input") })
    }

    @Test fun navigationDuringAutomaticReadPreventsEvenExplicitDiscovery() = runBlocking {
        val reads = Reads(this)
        val opening = reads.open()
        reads.current = false
        reads.automaticCatalog.complete(Unit)
        assertFalse(opening.await())
        assertTrue(reads.effects.isEmpty())
    }

    @Test fun navigationDuringExplicitDiscoveryPreventsDetailAndScreenReads() = runBlocking {
        val reads = Reads(this)
        val opening = reads.open()
        reads.automaticCatalog.complete(Unit)
        reads.catalogRequested.await()
        reads.current = false
        reads.catalogResponse.complete(Unit)
        assertFalse(opening.await())
        assertEquals(listOf("GET capabilities", "GET computers", "publish discovery"), reads.effects)
    }

    @Test fun cancellationDuringDiscoveryCannotContinueWhenItsReadLaterCompletes() = runBlocking {
        val reads = Reads(this)
        val opening = reads.open()
        reads.automaticCatalog.complete(Unit)
        reads.catalogRequested.await()
        opening.cancelAndJoin()
        reads.catalogResponse.complete(Unit)
        reads.catalogFlight.await()
        assertEquals(listOf("GET capabilities", "GET computers", "publish discovery"), reads.effects)
        assertTrue(opening.isCancelled)
    }

    @Test fun pendingOldInspectionMustFinishBeforeAnExplicitInspectionAndNavigationCanStillCancelIt() = runBlocking {
        val reads = Reads(this)
        val oldDetail = CompletableDeferred<Unit>()
        reads.detailFlight = oldDetail
        val opening = reads.open()
        reads.automaticCatalog.complete(Unit)
        reads.catalogRequested.await()
        reads.catalogResponse.complete(Unit)
        reads.catalogFlight.await()
        assertFalse(reads.detailRequested.isCompleted)
        reads.current = false
        oldDetail.complete(Unit)
        assertFalse(opening.await())
        assertEquals(listOf("GET capabilities", "GET computers", "publish discovery"), reads.effects)
    }
}
