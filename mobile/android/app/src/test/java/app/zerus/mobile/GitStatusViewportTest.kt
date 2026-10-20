package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class GitStatusViewportTest {
    private fun row(key: Any? = "session:fixture", offset: Int = 0, size: Int = 20) = GitViewportItem(key, offset, size)

    @Test fun exactTouchingEdgesAndUnplacedPrefetchItemsAreExcluded() {
        val items = listOf(row("session:above", -20), row("session:below", 100),
            row("session:unplaced", 20, 0), row("session:negative-size", 20, -1),
            row("session:first-pixel", -19), row("session:last-pixel", 99), row("session:inside", 20))
        assertEquals(linkedSetOf("session:first-pixel", "session:last-pixel", "session:inside"), GitStatusViewport.keys(items, 0, 100))
    }

    @Test fun actualNegativePaddingOffsetsRemainValidOnlyWhereTheyIntersect() {
        val items = listOf(row("session:before-padding", -40, 24), row("session:padding-edge", -20, 5),
            row("session:inside", 0, 20), row("session:after", 100, 20))
        assertEquals(linkedSetOf("session:padding-edge", "session:inside"), GitStatusViewport.keys(items, -16, 100))
    }

    @Test fun longArithmeticPreventsIntOverflowFromHidingARealIntersection() {
        assertEquals(setOf("session:fixture"), GitStatusViewport.keys(listOf(row(offset = Int.MAX_VALUE - 5, size = 20)),
            Int.MAX_VALUE - 10, Int.MAX_VALUE))
        assertEquals(setOf("session:fixture"), GitStatusViewport.keys(listOf(row(offset = Int.MIN_VALUE, size = Int.MAX_VALUE)), -2, 2))
        assertTrue(GitStatusViewport.keys(listOf(row(offset = Int.MAX_VALUE - 5, size = 20)), Int.MIN_VALUE, 0).isEmpty())
    }

    @Test fun onlyExactStringSessionPrefixKeysCanOwnRequests() {
        val items = listOf(null, 7, "group:fixture", "history:fixture", "session", "session:", "prefix:session:fixture", "session:fixture")
            .map { row(it) }
        assertEquals(setOf("session:fixture"), GitStatusViewport.keys(items, 0, 100))
    }

    @Test fun invalidOrEmptyViewportNeverAdmitsItems() {
        assertTrue(GitStatusViewport.keys(listOf(row()), 100, 100).isEmpty())
        assertTrue(GitStatusViewport.keys(listOf(row()), 100, 0).isEmpty())
        assertTrue(GitStatusViewport.keys(emptyList(), 0, 100).isEmpty())
    }

    @Test fun uniqueKeysAreBoundedToSixtyFourInActualVisibleItemOrder() {
        val items = List(10) { row("session:duplicate") } + (0..100).map { row("session:$it") }
        val keys = GitStatusViewport.keys(items, 0, 100)
        assertEquals(64, keys.size)
        assertEquals(listOf("session:duplicate") + (0..62).map { "session:$it" }, keys.toList())
        assertFalse("session:63" in keys)
    }

    @Test fun catalogSizedOffscreenInputCannotPromotePrefetchedRows() {
        val items = (0 until 200).map { index -> row("session:$index", index * 100, 100) }
        assertEquals(setOf("session:50"), GitStatusViewport.keys(items, 5_000, 5_100))
        assertEquals(setOf("session:49", "session:50"), GitStatusViewport.keys(items, 4_999, 5_001))
    }
}
