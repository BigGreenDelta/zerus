package app.zerus.mobile

import android.view.KeyEvent
import org.junit.Assert.*
import org.junit.Test

class VoiceGestureTest {
    @Test fun releaseFinishesOnlyItsCaptureOnce() {
        val gesture = VoiceGesture(80f)
        assertTrue(gesture.begin()); assertFalse(gesture.begin()); gesture.captured(42)
        assertNull(gesture.move(-79f))
        assertEquals(VoiceGesture.Finish(42, false), gesture.release())
        assertNull(gesture.release()); assertNull(gesture.interrupt()); assertNull(gesture.move(-100f))
    }
    @Test fun physicalLeftCommitsCancelEvenIfFingerReturnsOrReleases() {
        val gesture = VoiceGesture(80f)
        gesture.begin(); gesture.captured(7)
        assertNull(gesture.move(100f)); assertEquals(0f, gesture.progress)
        assertEquals(VoiceGesture.Finish(7, true), gesture.move(-80f))
        assertNull(gesture.move(0f)); assertEquals(1f, gesture.progress)
        assertNull(gesture.release())
        assertTrue(gesture.begin()); gesture.captured(8)
        assertEquals(VoiceGesture.Finish(8, false), gesture.release())
    }
    @Test fun permissionWithoutCaptureAndInterruptedPressCannotAcquireLateCapture() {
        val gesture = VoiceGesture(80f)
        gesture.begin(); assertNull(gesture.release()); gesture.captured(1)
        assertNull(gesture.release())
        gesture.begin(); gesture.captured(2)
        assertEquals(VoiceGesture.Finish(2, false), gesture.interrupt())
        gesture.captured(3); assertNull(gesture.move(-100f)); assertNull(gesture.release())
    }
    @Test fun crossingCancelBeforeCaptureCannotCancelAnotherCapture() {
        val gesture = VoiceGesture(80f)
        gesture.begin(); assertNull(gesture.move(-81f)); gesture.captured(9)
        assertNull(gesture.release())
        gesture.begin(); gesture.captured(10)
        assertNull(gesture.move(Float.NaN)); assertNull(gesture.move(Float.NEGATIVE_INFINITY))
        assertEquals(VoiceGesture.Finish(10, false), gesture.release())
    }
    @Test fun keyboardActivationOwnsOneReleaseAndDoesNotRepeatOrSurviveFocusLoss() {
        val keys = VoiceActivationKeys()
        fun key(action: Int, repeat: Int = 0, modified: Boolean = false) =
            keys.handle(KeyEvent.KEYCODE_ENTER, 4, action, repeat, modified)
        assertEquals(VoiceActivationKeys.Result.Pass, key(KeyEvent.ACTION_DOWN, modified = true))
        assertEquals(VoiceActivationKeys.Result.Pass, key(KeyEvent.ACTION_UP))
        assertEquals(VoiceActivationKeys.Result.Consume, key(KeyEvent.ACTION_DOWN))
        assertEquals(VoiceActivationKeys.Result.Consume, key(KeyEvent.ACTION_DOWN, repeat = 2))
        assertEquals(VoiceActivationKeys.Result.Activate(null), key(KeyEvent.ACTION_UP, modified = true))
        assertEquals(VoiceActivationKeys.Result.Pass, key(KeyEvent.ACTION_UP))
        assertEquals(VoiceActivationKeys.Result.Consume, key(KeyEvent.ACTION_DOWN))
        keys.clear()
        assertEquals(VoiceActivationKeys.Result.Pass, key(KeyEvent.ACTION_UP))
        assertEquals(VoiceActivationKeys.Result.Pass, key(KeyEvent.ACTION_DOWN, repeat = 3))
    }
    @Test fun keyboardStopRetainsOriginalCaptureEvenIfItFinishesBeforeRelease() {
        val keys = VoiceActivationKeys()
        assertEquals(VoiceActivationKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_ENTER, 1, KeyEvent.ACTION_DOWN, 0, false, captureId = 5))
        assertEquals(VoiceActivationKeys.Result.Activate(5), keys.handle(KeyEvent.KEYCODE_ENTER, 1, KeyEvent.ACTION_UP, 0, false, captureId = null))
        assertEquals(VoiceActivationKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_SPACE, 1, KeyEvent.ACTION_DOWN, 0, false))
        assertEquals(VoiceActivationKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_SPACE, 1, KeyEvent.ACTION_UP, 0, false, cancelled = true))
    }
}
