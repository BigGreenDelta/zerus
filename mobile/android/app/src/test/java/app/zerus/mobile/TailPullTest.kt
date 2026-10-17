package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class TailPullTest {
    // Material3's 80 dp threshold and the lists' 28 dp resistance cap at density 1.
    private fun pull() = TailPull(threshold = 80f, maximum = 28f)

    @Test fun travelWithoutAPointerDownNeverPulls() {
        val tail = pull()
        assertEquals(0f, tail.pull(200f))
        assertEquals(0f, tail.distance)
        assertFalse(tail.release())
    }
    @Test fun refreshNeedsTravelPastTheListsThreshold() {
        val tail = pull()
        tail.begin()
        assertEquals(160f, tail.pull(160f))
        assertEquals(1f, tail.fraction)
        assertFalse(tail.armed)
        assertFalse(tail.release())

        tail.begin()
        tail.pull(1f)
        assertTrue(tail.armed)
        assertTrue(tail.release())
        // One release refreshes once; later travel needs a new touch.
        assertFalse(tail.release())
        assertEquals(0f, tail.pull(40f))
    }
    @Test fun contentFollowsTheFingerWithBoundedResistance() {
        val tail = pull()
        tail.begin()
        tail.pull(40f)
        assertEquals(10f, tail.offset)
        tail.pull(400f)
        assertEquals(28f, tail.offset)
    }
    @Test fun downwardTravelReturnsThePullBeforeTheListScrolls() {
        val tail = pull()
        tail.begin()
        tail.pull(100f)
        assertEquals(60f, tail.unwind(60f))
        assertEquals(40f, tail.distance)
        // Only the remaining pull is consumed; the rest scrolls the list.
        assertEquals(40f, tail.unwind(70f))
        assertEquals(0f, tail.unwind(10f))
        assertEquals(0f, tail.distance)
        assertFalse(tail.release())
    }
    @Test fun upwardTravelIsNeverUnwound() {
        val tail = pull()
        tail.begin()
        tail.pull(50f)
        assertEquals(0f, tail.unwind(-20f))
        assertEquals(0f, tail.pull(-20f))
        assertEquals(50f, tail.distance)
    }
    @Test fun canceledTouchDoesNotRefresh() {
        val tail = pull()
        tail.begin()
        tail.pull(400f)
        tail.cancel()
        assertFalse(tail.release())
    }
    @Test fun aNewTouchContinuesFromTheReturningPull() {
        val tail = pull()
        tail.begin()
        tail.pull(150f)
        assertFalse(tail.release())
        tail.settle(120f)
        tail.begin()
        tail.pull(41f)
        assertTrue(tail.armed)
        tail.settle(-5f)
        assertEquals(0f, tail.distance)
    }
}
