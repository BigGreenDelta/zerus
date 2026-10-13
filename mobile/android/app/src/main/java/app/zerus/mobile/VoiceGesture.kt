package app.zerus.mobile

import android.view.KeyEvent

/** One pointer press owns at most one capture and one terminal action. No delivery actions exist here. */
internal class VoiceGesture(private val cancelDistance: Float) {
    data class Finish(val captureId: Long, val cancel: Boolean)
    private var pressed = false
    private var captureId: Long? = null
    private var ended = false
    var progress = 0f; private set

    init { require(cancelDistance.isFinite() && cancelDistance > 0) }

    fun begin(): Boolean {
        if (pressed) return false
        pressed = true; captureId = null; ended = false; progress = 0f
        return true
    }
    fun captured(id: Long?) { if (pressed && !ended) captureId = id }
    fun move(horizontalDistance: Float): Finish? {
        if (!pressed || ended || !horizontalDistance.isFinite()) return null
        progress = (-horizontalDistance / cancelDistance).coerceIn(0f, 1f)
        return if (progress >= 1f) finish(cancel = true) else null
    }
    fun release(): Finish? {
        val result = finish(cancel = false)
        pressed = false
        return result
    }
    /** A system cancellation/second pointer/navigation ends capture into a preview, never sends. */
    fun interrupt(): Finish? = release()
    private fun finish(cancel: Boolean): Finish? {
        if (!pressed || ended) return null
        ended = true
        return captureId?.let { Finish(it, cancel) }
    }
}

/** Keyboard activation happens on release; pointer release never calls this path. */
internal class VoiceActivationKeys {
    sealed interface Result {
        data object Pass : Result
        data object Consume : Result
        data class Activate(val captureId: Long?) : Result
    }
    private data class Press(val key: Pair<Int, Int>, val captureId: Long?)
    private var owned: Press? = null
    fun clear() { owned = null }
    fun handle(code: Int, device: Int, action: Int, repeat: Int, modified: Boolean, cancelled: Boolean = false, captureId: Long? = null): Result {
        if (code !in setOf(KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_SPACE)) return Result.Pass
        val key = device to code
        if (action == KeyEvent.ACTION_UP) {
            val press = owned?.takeIf { it.key == key } ?: return Result.Pass
            owned = null
            return if (cancelled) Result.Consume else Result.Activate(press.captureId)
        }
        if (action != KeyEvent.ACTION_DOWN) return if (owned?.key == key) Result.Consume else Result.Pass
        if (owned?.key == key) return Result.Consume
        if (owned != null || repeat != 0 || modified) return Result.Pass
        owned = Press(key, captureId)
        return Result.Consume
    }
}
