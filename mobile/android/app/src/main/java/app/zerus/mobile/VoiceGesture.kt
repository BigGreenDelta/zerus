package app.zerus.mobile

import android.view.KeyEvent

/** Telegram's short-press/hold/lock state machine; side effects remain with the exact capture owner. */
internal class VoiceGesture(private val cancelDistance: Float, private val lockDistance: Float = 57f) {
    enum class End { Send, Preview, Cancel }
    sealed interface Action {
        data object SwitchMode : Action
        data object Start : Action
        data class Lock(val captureId: Long) : Action
        data class Finish(val captureId: Long, val end: End) : Action
    }
    private var pressed = false
    private var captureId: Long? = null
    private var ended = false
    private var held = false
    var locked = false; private set
    var progress = 0f; private set
    var lockProgress = 0f; private set

    init { require(cancelDistance.isFinite() && cancelDistance > 0 && lockDistance.isFinite() && lockDistance > 0) }

    fun begin(): Boolean {
        if (pressed) return false
        pressed = true; captureId = null; ended = false; held = false; locked = false; progress = 0f; lockProgress = 0f
        return true
    }
    fun captured(id: Long?) { if (pressed && !ended) captureId = id }
    fun hold(): Action? {
        if (!pressed || ended || held) return null
        held = true
        return Action.Start
    }
    fun move(horizontalDistance: Float, verticalDistance: Float = 0f): Action? {
        if (!pressed || ended || locked || !horizontalDistance.isFinite() || !verticalDistance.isFinite()) return null
        progress = (-horizontalDistance / cancelDistance).coerceIn(0f, 1f)
        lockProgress = (-verticalDistance / lockDistance).coerceIn(0f, 1f)
        if (!held && (progress > .15f || lockProgress > .15f)) { ended = true; return null }
        if (captureId != null && lockProgress >= 1f) { locked = true; return Action.Lock(captureId!!) }
        return if (progress >= 1f) finish(End.Cancel) else null
    }
    fun release(): Action? {
        val result = if (locked) null else if (pressed && !held && !ended) Action.SwitchMode
            else finish(if (progress >= .55f) End.Cancel else End.Send)
        pressed = false
        return result
    }
    /** A lost pointer never switches mode or submits; an already locked capture keeps its own controls. */
    fun interrupt(): Action? {
        val result = if (locked) null else finish(End.Preview)
        pressed = false
        return result
    }
    private fun finish(end: End): Action? {
        if (!pressed || ended) return null
        ended = true
        return captureId?.let { Action.Finish(it, end) }
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
