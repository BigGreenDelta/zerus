package app.zerus.mobile

import android.view.HapticFeedbackConstants
import android.view.View

/** One microphone capture owns its feedback, including delayed recorder callbacks. */
internal class VoiceCaptureFeedback(private val emit: (Event) -> Unit = {}) {
    enum class Event { Start, Finish, Cancel }
    private var started = false
    private var ended = false
    private var released = false
    private var terminal: Event? = null

    fun started() {
        if (started || ended) return
        started = true
        pulse(Event.Start)
    }

    fun finish(userInitiated: Boolean, cancelled: Boolean = false) {
        if (released) return
        if (!userInitiated) { ended = true; terminal = null; return }
        if (ended) return
        ended = true
        if (started) terminal = if (cancelled) Event.Cancel else Event.Finish
    }

    /** Called only after AudioRecord has stopped and released, before saving the preview. */
    fun stopped() {
        if (released) return
        released = true; ended = true
        terminal?.let(::pulse)
        terminal = null
    }

    private fun pulse(event: Event) { runCatching { emit(event) } }
}

internal object VoiceHaptics {
    fun perform(view: View, event: VoiceCaptureFeedback.Event): Boolean = runCatching { when (event) {
        VoiceCaptureFeedback.Event.Start, VoiceCaptureFeedback.Event.Finish, VoiceCaptureFeedback.Event.Cancel ->
            // The regular View API respects both Android and per-view haptic settings.
            // A false result is final: there is no vibrator or ignore-settings fallback.
            view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
    } }.getOrDefault(false)
}

/** Based on captured PCM duration, so initialization never invents elapsed recording time. */
internal fun voiceRecordingTime(elapsedMs: Int): String {
    val tenths = elapsedMs.coerceAtLeast(0) / 100
    val seconds = tenths / 10
    return "${seconds / 60}:${(seconds % 60).toString().padStart(2, '0')}.${tenths % 10}"
}
