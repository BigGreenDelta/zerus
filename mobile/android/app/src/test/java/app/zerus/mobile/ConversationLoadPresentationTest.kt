package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class ConversationLoadPresentationTest {
    @Test fun resolvedEmptyConversationStaysEmptyAcrossRepeatedBackgroundPolls() {
        repeat(4) {
            assertNull(ConversationLoadPresentation.status(false, true, false, false, false, false, resolved = true))
            assertNull(ConversationLoadPresentation.status(false, false, false, true, false, false, resolved = true))
            assertNull(ConversationLoadPresentation.status(false, false, false, false, false, false, resolved = true))
        }
        val explicit = ConversationLoadPresentation.status(false, true, true, false, false, false, resolved = true)!!
        assertFalse(explicit.initial)
        assertTrue(explicit.spinner)
        assertEquals("Refreshing conversation…", explicit.text)
        val slowHistory = ConversationLoadPresentation.status(false, false, false, true, true, false, resolved = true)!!
        assertFalse(slowHistory.initial)
        assertEquals("Loading message history…", slowHistory.text)
        // A different, unopened conversation still gets its own cold placeholder.
        assertTrue(ConversationLoadPresentation.status(false, true, false, false, false, false, resolved = false)!!.initial)
    }

    @Test fun initialLoadIsExplicitBeforeTheDelayedSpinner() {
        var time = 0L
        val gate = LoadingGate({ time })
        gate.begin("conversation-a")
        val initial = ConversationLoadPresentation.status(false, true, gate.visible(), false, false, false)!!
        assertTrue(initial.initial)
        assertFalse(initial.spinner)
        assertTrue(initial.text.isNotBlank())
        time = 5_000
        assertTrue(ConversationLoadPresentation.status(false, true, gate.visible(), false, false, false)!!.spinner)
    }

    @Test fun cachedContentUsesAnchoredStatusAndNeverTheInitialPlaceholder() {
        assertNull(ConversationLoadPresentation.status(true, true, false, false, false, false))
        val cached = ConversationLoadPresentation.status(true, true, true, false, false, false)!!
        assertFalse(cached.initial)
        assertTrue(cached.spinner)
        assertNull(ConversationLoadPresentation.status(true, false, true, true, false, false))
        val history = ConversationLoadPresentation.status(true, false, false, true, true, false)!!
        assertFalse(history.initial)
        assertTrue(history.spinner)
        assertNotEquals(cached.text, history.text)
    }

    @Test fun emptyHeaderOnlyHistoryLoadRemainsExplicitBeforeAnimation() {
        val pending = ConversationLoadPresentation.status(false, false, false, true, false, false)!!
        assertTrue(pending.initial)
        assertFalse(pending.spinner)
        assertTrue(ConversationLoadPresentation.status(false, false, false, true, true, false)!!.spinner)
    }

    @Test fun fastCachedPollStaysHiddenButManualRefreshPromotesTheSameRead() {
        var time = 0L
        val gate = LoadingGate({ time })
        val read = gate.begin("conversation-a").operation
        time = 200
        assertNull(ConversationLoadPresentation.status(true, true, gate.visible(), false, false, false))
        gate.begin("conversation-a", explicit = true)
        assertTrue(ConversationLoadPresentation.status(true, true, gate.visible(), false, false, false)!!.spinner)
        gate.finish(read.id)
        assertNull(ConversationLoadPresentation.status(true, false, gate.visible(), false, false, false))
    }

    @Test fun completionOrFailureRemovesPlaceholderAndStaleProgress() {
        assertNull(ConversationLoadPresentation.status(false, false, true, false, true, false))
        assertNull(ConversationLoadPresentation.status(true, false, true, false, true, false))
        assertNull(ConversationLoadPresentation.status(false, true, true, true, true, true))
    }

    @Test fun historyNavigationDoesNotInheritOldSpinnerDeadline() {
        var time = 0L
        val gate = LoadingGate({ time })
        val previous = gate.begin("conversation-a").operation
        time = 5_000
        assertTrue(gate.visible())
        val next = gate.begin("conversation-b").operation
        assertFalse(gate.finish(previous.id))
        assertFalse(ConversationLoadPresentation.status(false, false, false, true, gate.visible(), false)!!.spinner)
        assertEquals(5_000L, gate.remaining(next.id))
    }

    @Test fun readAllPartialIsASubsetAndSkippedRowsAreNeverClaimedAsRead() {
        val result = ConversationLoadPresentation.readAllOutcome(3, 2, 1)
        assertTrue(result.startsWith("3 conversations marked read"))
        assertTrue(result.contains("1 marked loaded replies only"))
        assertTrue(result.contains("2 could not be checked"))
        assertFalse(result.contains("4 conversations"))
        assertTrue(ConversationLoadPresentation.readAllOutcome(0, 2, 0).startsWith("No conversations marked read"))
    }
}
