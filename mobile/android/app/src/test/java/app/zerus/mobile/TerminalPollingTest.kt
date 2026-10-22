package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class TerminalPollingTest {
    @Test fun heldKeyConfirmationKeepsReadonlyFreshnessWithoutAdoptingInputBinding() {
        assertTrue(TerminalPolling.readAllowed(true,false,false,false,true))
        assertFalse(TerminalPolling.readAllowed(true,false,false,false,false))
        assertFalse(TerminalPolling.readAllowed(false,false,false,false,true))
        assertFalse(TerminalPolling.readAllowed(true,false,true,false,true))
        assertFalse(TerminalPolling.readAllowed(true,false,false,true,true))
    }
    @Test fun idleReadsBackOffAndNewOutputRestoresResponsiveness() {
        var policy = TerminalPolling()
        assertEquals(250L,policy.delayMillis)
        policy = policy.observed(false);assertEquals(500L,policy.delayMillis)
        policy = policy.observed(false);assertEquals(1000L,policy.delayMillis)
        repeat(50) { policy = policy.observed(false) }
        assertEquals(2000L,policy.delayMillis)
        assertEquals(250L,policy.observed(true).delayMillis)
    }
}

class TerminalFollowTest {
    // A 50-row pane of 10 px lines in a 200 px viewport scrolls up to 300 px.
    @Test fun cursorNearTheBottomFollowsTheBottomOfThePane() {
        assertEquals(47,TerminalFollow.row(47,50))
        assertEquals(300f,TerminalFollow.vertical(47,10f,200f,300f))
        assertEquals(300f,TerminalFollow.vertical(30,10f,200f,300f))
    }
    @Test fun higherCursorSitsAtTheBottomEdgeWithTheRowsAboveIt() {
        assertEquals(100f,TerminalFollow.vertical(29,10f,200f,300f))
        assertEquals(10f,TerminalFollow.vertical(20,10f,200f,300f))
        assertEquals(0f,TerminalFollow.vertical(0,10f,200f,300f))
        assertEquals(0f,TerminalFollow.vertical(5,10f,200f,0f))
    }
    @Test fun missingCursorFollowsTheLastRowAndTheLeftEdge() {
        assertEquals(49,TerminalFollow.row(-1,50))
        assertEquals(49,TerminalFollow.row(50,50))
        assertEquals(300f,TerminalFollow.vertical(TerminalFollow.row(-1,50),10f,200f,300f))
        assertEquals(0f,TerminalFollow.horizontal(-1,200,5f,100f,900f))
    }
    @Test fun wideCursorColumnStaysAtTheRightEdge() {
        assertEquals(0f,TerminalFollow.horizontal(19,200,5f,100f,900f))
        assertEquals(5f,TerminalFollow.horizontal(20,200,5f,100f,900f))
        assertEquals(900f,TerminalFollow.horizontal(199,200,5f,100f,900f))
    }
    @Test fun onlyAReturnToTheFollowedPositionResumesFollowing() {
        assertTrue(TerminalFollow.atFollowed(296f,2f,300f,0f,10f,5f))
        assertFalse(TerminalFollow.atFollowed(290f,0f,300f,0f,10f,5f))
        assertFalse(TerminalFollow.atFollowed(300f,10f,300f,0f,10f,5f))
    }
}
