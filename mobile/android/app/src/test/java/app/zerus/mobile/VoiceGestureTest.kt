package app.zerus.mobile

import android.view.KeyEvent
import org.junit.Assert.*
import org.junit.Test

class VoiceGestureTest {
    @Test fun shortTapSwitchesAndHoldReleaseSendsOnce() {
        val g = VoiceGesture(80f)
        assertTrue(g.begin()); assertEquals(VoiceGesture.Action.SwitchMode, g.release()); assertNull(g.release())
        assertTrue(g.begin()); assertEquals(VoiceGesture.Action.Start, g.hold()); assertNull(g.hold()); g.captured(42)
        assertNull(g.move(-10f)); assertEquals(VoiceGesture.Action.Finish(42, VoiceGesture.End.Send), g.release())
        assertNull(g.interrupt()); assertNull(g.release())
    }
    @Test fun cancelCommitsAndPartialLeftReleaseCancels() {
        val g = VoiceGesture(80f)
        g.begin(); g.hold(); g.captured(7)
        assertEquals(VoiceGesture.Action.Finish(7, VoiceGesture.End.Cancel), g.move(-80f))
        assertNull(g.move(0f)); assertNull(g.release())
        g.begin(); g.hold(); g.captured(8); g.move(-48f)
        assertEquals(VoiceGesture.Action.Finish(8, VoiceGesture.End.Cancel), g.release())
    }
    @Test fun upwardLockSurvivesReleaseAndInterruption() {
        val g = VoiceGesture(80f, 57f)
        g.begin(); g.hold(); g.captured(3)
        assertEquals(VoiceGesture.Action.Lock(3), g.move(0f, -57f)); assertTrue(g.locked)
        assertNull(g.move(-100f)); assertNull(g.release()); assertNull(g.interrupt())
    }
    @Test fun interruptedAndPermissionPressNeverSendsOrAcquiresLateCapture() {
        val g = VoiceGesture(80f)
        g.begin(); g.hold(); assertNull(g.release()); g.captured(9); assertNull(g.release())
        g.begin(); g.hold(); g.captured(2)
        assertEquals(VoiceGesture.Action.Finish(2, VoiceGesture.End.Preview), g.interrupt())
        g.captured(3); assertNull(g.release())
        g.begin(); g.move(-20f); assertNull(g.hold()); assertNull(g.release())
        g.begin(); g.hold(); g.captured(4); assertNull(g.move(Float.NaN)); assertNull(g.move(Float.NEGATIVE_INFINITY))
        assertEquals(VoiceGesture.Action.Finish(4, VoiceGesture.End.Send), g.release())
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
