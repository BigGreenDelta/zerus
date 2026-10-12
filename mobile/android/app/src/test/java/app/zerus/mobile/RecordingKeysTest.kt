package app.zerus.mobile

import android.view.KeyEvent
import org.junit.Assert.*
import org.junit.Test

class RecordingKeysTest {
    @Test fun captureOwnsBothStopKeysUntilReleaseEvenAfterSaving() {
        for (code in listOf(KeyEvent.KEYCODE_SPACE, KeyEvent.KEYCODE_ESCAPE)) {
            val keys = RecordingKeys()
            assertEquals(RecordingKeys.Result.Stop, keys.handle(code, 7, KeyEvent.ACTION_DOWN, 0, true))
            assertEquals(RecordingKeys.Result.Consume, keys.handle(code, 7, KeyEvent.ACTION_DOWN, 1, true))
            assertEquals(RecordingKeys.Result.Consume, keys.handle(code, 7, KeyEvent.ACTION_DOWN, 2, false))
            assertEquals(RecordingKeys.Result.Consume, keys.handle(code, 7, KeyEvent.ACTION_UP, 0, false))
            assertEquals(RecordingKeys.Result.Pass, keys.handle(code, 7, KeyEvent.ACTION_DOWN, 0, false))
            assertEquals(RecordingKeys.Result.Pass, keys.handle(code, 7, KeyEvent.ACTION_UP, 0, false))
        }
    }
    @Test fun keyHeldBeforeCaptureCannotBecomeAnotherActivation() {
        val keys = RecordingKeys()
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_DOWN, 0, false))
        assertEquals(RecordingKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_DOWN, 1, true))
        assertEquals(RecordingKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_UP, 0, false))
        assertEquals(RecordingKeys.Result.Stop, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_DOWN, 0, true))
    }
    @Test fun independentKeysAndDevicesDoNotStealEachOthersRelease() {
        val keys = RecordingKeys()
        assertEquals(RecordingKeys.Result.Stop, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_DOWN, 0, true))
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_A, 7, KeyEvent.ACTION_DOWN, 0, true))
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_SPACE, 8, KeyEvent.ACTION_UP, 0, false))
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_ESCAPE, 7, KeyEvent.ACTION_UP, 0, false))
        assertEquals(RecordingKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_UP, 0, false))
    }
    @Test fun modifiedShortcutsPassButModifiersCannotReleaseAnOwnedPress() {
        val keys = RecordingKeys()
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_DOWN, 0, true, true))
        assertEquals(RecordingKeys.Result.Pass, keys.handle(KeyEvent.KEYCODE_SPACE, 7, KeyEvent.ACTION_UP, 0, true, true))
        assertEquals(RecordingKeys.Result.Stop, keys.handle(KeyEvent.KEYCODE_ESCAPE, 7, KeyEvent.ACTION_DOWN, 0, true))
        assertEquals(RecordingKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_ESCAPE, 7, KeyEvent.ACTION_DOWN, 1, false, true))
        assertEquals(RecordingKeys.Result.Consume, keys.handle(KeyEvent.KEYCODE_ESCAPE, 7, KeyEvent.ACTION_UP, 0, false, true))
    }
}
