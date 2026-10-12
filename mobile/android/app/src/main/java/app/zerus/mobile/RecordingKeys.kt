package app.zerus.mobile

import android.view.KeyEvent

/** Keep the whole physical press owned by recording even after capture has stopped. */
internal class RecordingKeys {
    enum class Result { Pass, Consume, Stop }
    private val presses = mutableMapOf<Pair<Int, Int>, Boolean>()

    fun handle(keyCode: Int, deviceId: Int, action: Int, repeatCount: Int, recording: Boolean, modified: Boolean = false): Result {
        if (keyCode != KeyEvent.KEYCODE_SPACE && keyCode != KeyEvent.KEYCODE_ESCAPE) return Result.Pass
        val key = deviceId to keyCode
        if (action == KeyEvent.ACTION_UP) {
            val owned = presses.remove(key) == true
            return if (owned || recording && !modified) Result.Consume else Result.Pass
        }
        if (action != KeyEvent.ACTION_DOWN) return if (recording && !modified || presses[key] == true) Result.Consume else Result.Pass
        if (repeatCount == 0) {
            presses[key] = recording && !modified
            return if (recording && !modified) Result.Stop else Result.Pass
        }
        // A key held before capture began must not become a second activation.
        if (recording && !modified) presses[key] = true
        return if (presses[key] == true) Result.Consume else Result.Pass
    }
}
