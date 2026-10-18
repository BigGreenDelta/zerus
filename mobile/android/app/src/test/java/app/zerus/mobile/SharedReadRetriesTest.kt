package app.zerus.mobile

import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.junit.Assert.*
import org.junit.Test

class SharedReadRetriesTest {
    @Test fun theDelaySaturatesAtSixteenMinutes() {
        val retries=SharedReadRetries()
        var now=0L;val delays=mutableListOf<Long>()
        repeat(6) {
            val due=retries.unapplied("mark","workspace",now)!!
            delays+=due-now
            assertEquals(listOf("mark"),retries.release(due))
            now=due
        }
        assertEquals(listOf(120_000L,240_000L,480_000L,960_000L,960_000L,960_000L),delays)
        assertEquals(1,retries.size)
        assertEquals(120_000L,SharedReadRetries.delayMs(-1))
    }
    @Test fun nothingIsReleasedEarlyAndAnAppliedMarkIsNeverReleased() {
        val retries=SharedReadRetries()
        val due=retries.unapplied("mark","workspace",10)!!
        assertEquals(due,retries.nextDue)
        assertEquals(emptyList<String>(),retries.release(due-1))
        retries.applied("mark")
        assertNull(retries.nextDue)
        // A timer that was already sleeping finds nothing to release.
        assertEquals(emptyList<String>(),retries.release(due+1))
        assertEquals(0,retries.size)
    }
    @Test fun aStaleWakeupCannotReleaseARescheduledMark() {
        val retries=SharedReadRetries()
        val first=retries.unapplied("mark","workspace",0)!!
        assertEquals(listOf("mark"),retries.release(first))
        val second=retries.unapplied("mark","workspace",first)!!
        // The wakeup for the first attempt arrives late; the second attempt is not due yet.
        assertEquals(emptyList<String>(),retries.release(first+1))
        assertEquals(second,retries.nextDue)
        assertEquals(listOf("mark"),retries.release(second))
        // However late it wakes, a timer never hands out a released mark twice.
        assertEquals(emptyList<String>(),retries.release(Long.MAX_VALUE))
    }
    @Test fun aStaleTimerWakeupWaitsForTheNewerDueOfTheSameMark() = runBlocking {
        val retries=SharedReadRetries()
        // A scripted monotonic clock; each due is 20 ms of real sleep away when the timer reads it.
        val times=ArrayDeque(listOf(0L,0L,119_980L,120_000L));var last=0L
        val allowed=mutableListOf<Pair<String,Long>>()
        retries.unapplied("mark","workspace",20-SharedReadRetries.delayMs(0))
        val timer=launch { retries.releaseWhenDue({ times.removeFirst().also { last=it } }) { allowed+=it to last } }
        yield()
        // While the timer sleeps, the mark is applied and a later send completes unapplied again.
        retries.applied("mark")
        assertEquals(120_000L,retries.unapplied("mark","workspace",0))
        timer.join()
        assertEquals(listOf("mark" to 120_000L),allowed)
        assertTrue(times.isEmpty())
        assertNull(retries.nextDue)
    }
    @Test fun aCancelledTimerLeavesItsMarkForTheNextTimer() = runBlocking {
        val retries=SharedReadRetries();val allowed=mutableListOf<String>()
        retries.unapplied("mark","workspace",20-SharedReadRetries.delayMs(0))
        val timer=launch { retries.releaseWhenDue({ 0L }) { allowed+=it } }
        yield();timer.cancel();delay(60)
        assertEquals(emptyList<String>(),allowed)
        assertEquals(listOf("mark"),retries.release(20))
    }
    @Test fun capacityFailsClosedAndRetiredMarksMakeRoom() {
        val retries=SharedReadRetries()
        repeat(SharedReadRetries.CAPACITY) { assertNotNull(retries.unapplied("mark-$it","workspace",0)) }
        // At capacity a new mark is not scheduled: it stays sent, and no timer is added for it.
        assertNull(retries.unapplied("overflow","workspace",1))
        assertEquals(SharedReadRetries.CAPACITY,retries.size)
        assertEquals(120_000L,retries.nextDue)
        // A mark already in the table is still rescheduled at capacity.
        assertEquals(120_001L,retries.unapplied("mark-0","workspace",1))
        assertEquals(SharedReadRetries.CAPACITY-1,retries.release(120_000).size)
        assertEquals(listOf("mark-0"),retries.release(120_001))
        // Released marks are remembered for one maximum delay, then retired.
        assertNull(retries.unapplied("overflow","workspace",120_000L+960_000L-1))
        assertNotNull(retries.unapplied("overflow","workspace",120_000L+960_000L+1))
        assertEquals(1,retries.size)
    }
    @Test fun aRetiredMarkStartsOverAtTwoMinutes() {
        val retries=SharedReadRetries()
        val first=retries.unapplied("mark","workspace",0)!!
        retries.release(first)
        val second=retries.unapplied("mark","workspace",first+10)!!
        assertEquals(240_000L,second-(first+10))
        retries.release(second)
        val restarted=retries.unapplied("mark","workspace",second+960_000L+1)!!
        assertEquals(120_000L,restarted-(second+960_000L+1))
    }
    @Test fun forgettingAWorkspaceOrResettingReleasesNothingLater() {
        val retries=SharedReadRetries()
        val due=retries.unapplied("a","first",0)!!
        retries.unapplied("b","second",0)
        retries.forget("first")
        assertEquals(listOf("b"),retries.release(due))
        retries.unapplied("c","second",due)
        retries.reset()
        assertNull(retries.nextDue)
        assertEquals(emptyList<String>(),retries.release(Long.MAX_VALUE))
        assertEquals(0,retries.size)
    }
}
