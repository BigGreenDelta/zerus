package app.zerus.mobile

import java.io.IOException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class HistoryReadPathsTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val canonical = Event("history:1", "AgentMessage", "reply", "", 1.0,
        historyId="1",historyCursor="cursor:1",historyEpoch="epoch",incomingSeq=1)
    private fun saved() = JSONObject().put("zerus_page_window",1).put("target",MessageCodec.targetJson(target))
        .put("epoch","epoch").put("rows",EventHistoryCodec.encode(listOf(canonical)))
        .put("before","cursor:1").put("after","cursor:1").put("has_before",false).put("has_after",false)
    private fun oldMixed() = saved().put("rows",EventHistoryCodec.encode(listOf(canonical,
        canonical.copy(id="journal:fixture:AgentMessage:native:2",historyId="",historyCursor="",historyEpoch="",incomingSeq=null))))

    @Test fun previouslyWrittenMixedPageIsMissWithoutWeakeningStrictDecoder() = runBlocking {
        val old = oldMixed()
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.cached(target,old) }
        assertNull(HistoryReadPaths.saved(target) { old })
        val fresh = HistoryReadPaths.saved(target) { saved() }!!
        assertEquals(listOf(canonical),fresh.events)
        assertFalse(fresh.complete)
        assertNull(fresh.latestIncoming)
        assertNull(fresh.totalIncoming)
    }
    @Test fun foreignScopeMalformedJsonAndCacheReadFailureAreSafeMisses() = runBlocking {
        assertNull(HistoryReadPaths.saved(target.copy(connectionId="other")) { saved() })
        assertNull(HistoryReadPaths.saved(target.copy(run="other")) { saved() })
        assertNull(HistoryReadPaths.saved(target) { saved().put("epoch","foreign epoch") })
        assertNull(HistoryReadPaths.saved(target) { JSONObject().put("zerus_page_window",1) })
        assertNull(HistoryReadPaths.saved(target) { throw IOException("cache unavailable") })
    }
    @Test fun offlineInvalidCachePreservesOriginalNetworkFailureAndDoesNotRetry() = runBlocking {
        val original = IOException("fixture offline")
        var attempts = 0
        try {
            HistoryReadPaths.read(target,network={ attempts++;throw original },cached={oldMixed()})
            fail("Invalid retained page cannot satisfy this read")
        } catch(error:IOException) { assertSame(original,error) }
        assertEquals(1,attempts)
    }
    @Test fun resetIndexReopensVerifiedHeadInsteadOfReplayingOldEpoch() = runBlocking {
        val head = HistoryPage(target,"new epoch",emptyList(),null,null,false,false,null,null,true,false,false,JSONObject())
        val stale = HistoryReadPaths.saved(target) { saved() }!!
        var probes = 0
        val failed = HistoryReadPaths.reopening("epoch",{ throw IOException("native command failed") }) { probes++;head }
        assertTrue(failed.head);assertTrue(failed.result.verified);assertSame(head,failed.result.page)
        val cached = HistoryReadPaths.reopening("epoch",{ HistoryReadPaths.Result(stale,false) }) { probes++;head }
        assertTrue(cached.head);assertSame(head,cached.result.page)
        assertEquals(2,probes)
    }
    @Test fun sameEpochOfflineOrVerifiedReadsKeepTheirOriginalOutcome() = runBlocking {
        val stale = HistoryReadPaths.saved(target) { saved() }!!
        val original = IOException("fixture offline")
        var probes = 0
        val verified = HistoryReadPaths.reopening("epoch",{ HistoryReadPaths.Result(stale,true) }) { probes++;stale }
        assertFalse(verified.head);assertEquals(0,probes)
        val cached = HistoryReadPaths.reopening("epoch",{ HistoryReadPaths.Result(stale,false) }) { stale }
        assertFalse(cached.head);assertFalse(cached.result.verified)
        for(head in listOf<suspend ()->HistoryPage>({ stale },{ throw IOException("head offline") })) {
            try { HistoryReadPaths.reopening("epoch",{ throw original },head);fail("Original failure must remain") }
            catch(error:IOException) { assertSame(original,error) }
        }
        try { HistoryReadPaths.reopening(null,{ throw original }) { probes++;stale };fail("A head read has no cursor epoch") }
        catch(error:IOException) { assertSame(original,error) }
        assertEquals(0,probes)
    }
    @Test fun cacheCancellationPropagatesInsteadOfBeingTreatedAsAMiss() = runBlocking {
        val cancellation = CancellationException("navigation changed")
        try {
            HistoryReadPaths.saved(target) { throw cancellation }
            fail("Cancellation must propagate")
        } catch(error:CancellationException) { assertSame(cancellation,error) }
    }
}
