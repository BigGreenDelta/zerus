package app.zerus.mobile

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.yield
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.junit.Assert.*
import org.junit.Test

class MarkdownDocumentCacheTest {
    @Test fun identicalContentCoalescesOffCallerThreadAndSurvivesViewerCancellation() = runBlocking {
        val calls = AtomicInteger()
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val caller = Thread.currentThread()
        var parseThread: Thread? = null
        val cache = MarkdownDocumentCache(parse = { source ->
            calls.incrementAndGet()
            parseThread = Thread.currentThread()
            entered.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            MarkdownParser().parse(source)
        })
        try {
            val first = async { cache.document("**same**") }
            val second = async { cache.document("**same**") }
            withContext(Dispatchers.IO) { assertTrue(entered.await(5, TimeUnit.SECONDS)) }
            first.cancel()
            release.countDown()
            val document = second.await()
            assertSame(document, cache.document("**same**"))
            assertEquals(1, calls.get())
            assertNotSame(caller, parseThread)
        } finally { release.countDown(); cache.close() }
    }

    @Test fun parserConcurrencyNeverExceedsTwoAcrossDifferentDocuments() = runBlocking {
        val active = AtomicInteger()
        val maximum = AtomicInteger()
        val firstPair = CountDownLatch(2)
        val release = CountDownLatch(1)
        val cache = MarkdownDocumentCache(parse = { source ->
            val count = active.incrementAndGet()
            maximum.updateAndGet { maxOf(it, count) }
            firstPair.countDown()
            try {
                check(release.await(5, TimeUnit.SECONDS))
                MarkdownDocument.literal(source)
            } finally { active.decrementAndGet() }
        })
        try {
            val tasks = (1..12).map { async { cache.document("source $it") } }
            withContext(Dispatchers.IO) { assertTrue(firstPair.await(5, TimeUnit.SECONDS)) }
            assertEquals(2, maximum.get())
            release.countDown()
            assertEquals(12, tasks.awaitAll().size)
            assertEquals(2, maximum.get())
        } finally { release.countDown(); cache.close() }
    }


    @Test fun cancelledWaitingAdmissionRegistersNoJobAndReturnsItsPermit() = runBlocking {
        val started = java.util.Collections.synchronizedList(mutableListOf<String>())
        val activePair = CountDownLatch(2)
        val release = CountDownLatch(1)
        val cache = MarkdownDocumentCache(parse = { source ->
            started.add(source)
            activePair.countDown()
            check(release.await(5, TimeUnit.SECONDS))
            MarkdownDocument.literal(source)
        })
        try {
            val first = async { cache.document("first") }
            val second = async { cache.document("second") }
            withContext(Dispatchers.IO) { assertTrue(activePair.await(5, TimeUnit.SECONDS)) }
            val waiting = async { cache.document("cancelled while waiting") }
            yield()
            assertEquals(2, cache.inFlightCount())
            waiting.cancelAndJoin()
            assertEquals(2, cache.inFlightCount())
            assertEquals(setOf("first", "second"), started.toSet())
            // A same-source viewer joins its active job even when both admission permits are held.
            val shared = async { cache.document("first") }
            release.countDown()
            assertSame(first.await(), shared.await())
            second.await()
            assertEquals("after cancellation", cache.document("after cancellation").source)
            assertEquals(setOf("first", "second", "after cancellation"), started.toSet())
        } finally { release.countDown(); cache.close() }
    }

    @Test fun exactStringsWithTheSameHashDoNotShareModels() = runBlocking {
        assertEquals("Aa".hashCode(), "BB".hashCode())
        val calls = AtomicInteger()
        val cache = MarkdownDocumentCache(parse = { source -> calls.incrementAndGet(); MarkdownParser().parse(source) })
        try {
            assertEquals("Aa", cache.document("Aa").source)
            assertEquals("BB", cache.document("BB").source)
            assertEquals("Aa", cache.document("Aa").source)
            assertEquals(2, calls.get())
        } finally { cache.close() }
    }

    @Test fun entryBudgetEvictsTheLeastRecentlyUsedExactDocument() = runBlocking {
        val calls = AtomicInteger()
        val cache = MarkdownDocumentCache(maxEntries = 2, parse = { source ->
            calls.incrementAndGet(); MarkdownDocument.literal(source)
        })
        try {
            cache.document("a"); cache.document("b"); cache.document("a"); cache.document("c")
            assertEquals(2, cache.snapshot().first)
            // A finished request leaves no flight behind for an evicted source to join.
            assertEquals(0, cache.inFlightCount())
            cache.document("b")
            assertEquals(4, calls.get())
        } finally { cache.close() }
    }

    @Test fun byteBudgetCountsModelWeightAndDoesNotRetainOversizedDocuments() = runBlocking {
        val calls = AtomicInteger()
        val cache = MarkdownDocumentCache(maxBytes = 12, parse = { source ->
            calls.incrementAndGet()
            MarkdownDocument(source, emptyList(), estimatedBytes = if (source == "large") 13 else 8)
        })
        try {
            cache.document("a"); cache.document("b")
            assertEquals(1 to 8L, cache.snapshot())
            cache.document("large")
            // Not retained, and not joinable either: the next request parses it again.
            assertEquals(0, cache.inFlightCount())
            cache.document("large")
            assertEquals(4, calls.get())
            assertEquals(1 to 8L, cache.snapshot())
        } finally { cache.close() }
    }

    @Test fun parserFailureIsSharedAsFullLiteralSourceInsteadOfPartialContent() = runBlocking {
        val source = "# Full original\n\n**tail**"
        val calls = AtomicInteger()
        val cache = MarkdownDocumentCache(parse = { calls.incrementAndGet(); error("synthetic parser failure") })
        try {
            val result = cache.document(source)
            assertTrue(result.literalFallback)
            assertEquals(source, result.source)
            assertSame(result, cache.document(source))
            assertEquals(1, calls.get())
        } finally { cache.close() }
    }
}
