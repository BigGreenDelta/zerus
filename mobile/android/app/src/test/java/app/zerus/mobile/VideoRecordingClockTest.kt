package app.zerus.mobile

import org.junit.Assert.assertEquals
import org.junit.Test

class VideoRecordingClockTest {
    @Test fun countdownMatchesCaptureLimitAndRoundsUp() {
        assertEquals(59, VideoRecordingClock.remainingSeconds(0))
        assertEquals(59, VideoRecordingClock.remainingSeconds(1))
        assertEquals(58, VideoRecordingClock.remainingSeconds(1_000))
        assertEquals(1, VideoRecordingClock.remainingSeconds(58_999))
        assertEquals(0, VideoRecordingClock.remainingSeconds(59_000))
    }
    @Test fun staleOrOutOfRangeStatisticsCannotWrapTheClock() {
        assertEquals(59, VideoRecordingClock.remainingSeconds(Int.MIN_VALUE))
        assertEquals(0, VideoRecordingClock.remainingSeconds(Int.MAX_VALUE))
        assertEquals(0f, VideoRecordingClock.progress(-1), 0f)
        assertEquals(1f, VideoRecordingClock.progress(60_000), 0f)
        assertEquals(.5f, VideoRecordingClock.progress(29_500), .0001f)
    }
}
