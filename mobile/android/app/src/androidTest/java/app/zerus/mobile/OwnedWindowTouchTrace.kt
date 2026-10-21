package app.zerus.mobile

import android.os.Looper
import android.view.InputDevice
import android.view.MotionEvent
import android.view.Window

/** Test-only observation. A consumed window event never proves that a particular Compose button clicked. */
internal class OwnedWindowTouchTrace(private val window: Window, private val state: () -> String) {
    private data class EventRecord(val kind: String, val metadata: String, val detail: String, val before: String,
        var after: String = "UNKNOWN", var outcome: String = "UNKNOWN")
    private data class Tap(val id: Int, val expectedWindow: Int, val ownedWindow: Int, val down: Long,
        val x: Float, val y: Float, val records: MutableList<EventRecord> = mutableListOf(), var ended: Boolean = false,
        var overflow: Boolean = false)
    private val original = checkNotNull(window.callback)
    private val taps = mutableListOf<Tap>()
    private var pending: Tap? = null
    private var installed = false
    private var restored = false
    private var tokenOverflow = false
    @Volatile private var unknown: String? = null
    private fun uncertain(reason: String) { if (unknown == null) unknown = reason }
    fun observationFailed() { uncertain("observation-failed") }
    private fun onMain() = Looper.myLooper() == Looper.getMainLooper()
    private fun current(): Tap? {
        if (!onMain()) { uncertain("not-main"); return null }
        return pending
    }
    private fun append(tap: Tap, kind: String, metadata: String, detail: String = ""): EventRecord? {
        if (tap.records.size >= 4) { tap.overflow = true; uncertain("record-overflow"); return null }
        return EventRecord(kind, metadata.take(128), detail.take(128), state().take(160)).also { tap.records.add(it) }
    }
    private fun touch(event: MotionEvent): EventRecord? {
        val tap = current() ?: return null
        if (event.actionMasked !in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL)) return null
        val matches = tap.expectedWindow == tap.ownedWindow && event.downTime == tap.down && event.eventTime >= tap.down &&
            event.pointerCount == 1 && event.source == InputDevice.SOURCE_TOUCHSCREEN && event.rawX == tap.x && event.rawY == tap.y
        if (!matches) uncertain("touch-metadata-mismatch")
        return append(tap, "touch", "identity=${System.identityHashCode(event)} action=${event.actionMasked} down=${event.downTime} time=${event.eventTime}",
            "x=${event.rawX} y=${event.rawY} source=${event.source} pointers=${event.pointerCount} match=$matches")
    }
    private fun focus(value: Boolean): EventRecord? {
        val tap = current() ?: return null
        return append(tap, "focus", "value=$value")
    }
    private fun finish(record: EventRecord?, result: Any?, error: Throwable?) {
        try {
            if (record == null) return
            record.outcome = if (error != null) "throw" else when (result) { is Boolean -> result.toString(); Unit -> "unit"; else -> "UNKNOWN" }
            record.after = state().take(160)
        } catch (_: Throwable) { uncertain("after-record-failed") }
    }
    private fun <T> forward(record: () -> EventRecord?, dispatch: () -> T): T {
        val before = try { record() } catch (_: Throwable) { uncertain("before-record-failed"); null }
        return try {
            val result = dispatch()
            finish(before, result, null)
            result
        } catch (error: Throwable) {
            finish(before, null, error)
            throw error
        }
    }
    private val wrapper = object : Window.Callback by original {
        override fun dispatchTouchEvent(event: MotionEvent): Boolean = forward({ touch(event) }) { original.dispatchTouchEvent(event) }
        override fun onWindowFocusChanged(hasFocus: Boolean) {
            forward({ focus(hasFocus) }) { original.onWindowFocusChanged(hasFocus) }
        }
    }
    fun install() {
        check(onMain() && window.callback === original)
        window.callback = wrapper
        installed = true
    }
    private fun ownedWindowId(): Int? {
        val node = window.decorView.createAccessibilityNodeInfo()
        if (node == null) { uncertain("owned-window-unavailable"); return null }
        return try {
            val id = node.windowId
            if (id < 0) { uncertain("owned-window-invalid"); null } else id
        } finally { node.recycle() }
    }
    fun begin(label: String, expectedWindow: Int, down: Long, x: Float, y: Float): Int? {
        try {
            check(onMain())
            if (label != "Send answer") return null
            if (pending != null) { uncertain("nested-tap"); return null }
            if (taps.size >= 8) { tokenOverflow = true; uncertain("tap-overflow"); return null }
            if (!installed || window.callback !== wrapper) { uncertain("callback-changed"); return null }
            // This owned View/window observation happens outside dispatch, never in the callback.
            val ownedWindow = ownedWindowId() ?: return null
            val tap = Tap(taps.size + 1, expectedWindow, ownedWindow, down, x, y)
            if (tap.expectedWindow != tap.ownedWindow) uncertain("window-mismatch")
            taps.add(tap); pending = tap
            return tap.id
        } catch (_: Throwable) { uncertain("begin-failed"); return null }
    }
    fun end(id: Int) {
        try {
            check(onMain())
            val tap = pending
            if (tap == null || tap.id != id) { uncertain("token-mismatch"); return }
            tap.ended = true; pending = null
            if (window.callback !== wrapper) uncertain("callback-changed")
        } catch (_: Throwable) { uncertain("end-failed") }
    }
    fun evidence(): String {
        check(onMain())
        if (taps.isEmpty()) uncertain("no-send-token")
        if (pending != null || taps.any { !it.ended }) uncertain("unclosed-token")
        val lines = mutableListOf<String>()
        fun line(text: String) { if (lines.size < 184) lines.add(text.take(160)) }
        line("owned-touch status=${if (unknown == null) "observed" else "UNKNOWN"} reason=${unknown ?: "none"} restored=$restored scope=window-consumption-only reviewGuard=UNKNOWN")
        line("owned-touch coverage=recorded-window-events-only tokens=${taps.size} tokenOverflow=$tokenOverflow")
        for (tap in taps) {
            line("owned-touch token=${tap.id} label=Send answer expectedWindow=${tap.expectedWindow} ownedWindow=${tap.ownedWindow}")
            line("owned-touch token=${tap.id} ended=${tap.ended} records=${tap.records.size} touch=${tap.records.count { it.kind == "touch" }} focus=${tap.records.count { it.kind == "focus" }} overflow=${tap.overflow}")
            for (record in tap.records) {
                line("owned-touch token=${tap.id} kind=${record.kind} outcome=${record.outcome}")
                line("owned-touch metadata ${record.metadata}")
                if (record.detail.isNotEmpty()) line("owned-touch detail ${record.detail}")
                line("owned-touch before ${record.before}")
                line("owned-touch after ${record.after}")
            }
        }
        return lines.joinToString("\n")
    }
    fun close() {
        check(onMain())
        if (!installed) return
        if (window.callback !== wrapper) { uncertain("callback-changed"); error("Owned touch callback changed; restoration UNKNOWN") }
        window.callback = original
        if (window.callback !== original) { uncertain("restore-unverified"); error("Owned touch callback restoration UNKNOWN") }
        installed = false
        restored = true
    }
}
