package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeInfo.AccessibilityAction
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicReference

/** Synthetic owned-emulator Terminal snapshots only; no relay, native pane or input. */
class TerminalFollowSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>(); runOnMainSync { value.set(runCatching(action)) }; return value.get().getOrThrow()
    }
    private fun await(label: String, action: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 15_000
        while (!action()) {
            if (SystemClock.elapsedRealtime() >= until) { screenshot("terminal-follow-failure.png"); error(label) }
            SystemClock.sleep(40)
        }
    }
    private fun nodes(root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): List<AccessibilityNodeInfo> {
        root ?: return emptyList(); root.refresh()
        return listOf(root) + (0 until root.childCount).flatMap { nodes(root.getChild(it)) }
    }
    private fun find(label: String) = nodes().firstOrNull { it.isVisibleToUser && (it.text?.toString() == label || it.contentDescription?.toString() == label) }
    /** Directional scroll actions of the vertical and horizontal Terminal scrollers. */
    private fun scroll(): Map<String, Boolean> {
        val ids = nodes().filter { it.isScrollable }.flatMap { node -> node.actionList.map { it.id } }.toSet()
        return mapOf("up" to (AccessibilityAction.ACTION_SCROLL_UP.id in ids), "down" to (AccessibilityAction.ACTION_SCROLL_DOWN.id in ids),
            "left" to (AccessibilityAction.ACTION_SCROLL_LEFT.id in ids), "right" to (AccessibilityAction.ACTION_SCROLL_RIGHT.id in ids))
    }
    private fun at(vertical: String) = scroll().let { it[vertical] == false && it[if (vertical == "down") "up" else "down"] == true }
    private fun screenshot(name: String) {
        uiAutomation.waitForIdle(100, 2_000); waitForIdleSync()
        val frame = checkNotNull(uiAutomation.takeScreenshot())
        try { File(targetContext.getExternalFilesDir(null), name).outputStream().use { check(frame.compress(Bitmap.CompressFormat.PNG, 100, it)) } } finally { frame.recycle() }
    }
    private fun gesture(points: List<Pair<Float, Float>>) {
        val down = SystemClock.uptimeMillis()
        points.forEachIndexed { index, (x, y) ->
            val action = when (index) { 0 -> MotionEvent.ACTION_DOWN; points.lastIndex -> MotionEvent.ACTION_UP; else -> MotionEvent.ACTION_MOVE }
            val property = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
            val coordinate = MotionEvent.PointerCoords().apply { this.x = x; this.y = y; pressure = 1f; size = .1f }
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 1, arrayOf(property), arrayOf(coordinate), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
            SystemClock.sleep(16)
        }
        uiAutomation.waitForIdle(100, 2_000); waitForIdleSync()
    }
    private fun tap(label: String) {
        var owner = checkNotNull(find(label)) { "Missing $label" }
        while (!owner.isClickable) owner = checkNotNull(owner.parent)
        val rect = Rect().also(owner::getBoundsInScreen)
        gesture(listOf(rect.exactCenterX() to rect.exactCenterY(), rect.exactCenterX() to rect.exactCenterY()))
    }
    private fun snapshot(generation: Int, cursorY: Int): Pair<TerminalScreen, JSONObject> {
        val text = (0 until 60).joinToString("\n") { row -> if (row == cursorY) "$ prompt $generation" else "line %02d of snapshot %d".format(row, generation) }
        val raw = JSONObject().put("cols", 80).put("rows", 60).put("screen", text).put("cursor", JSONObject().put("x", 2).put("y", cursorY))
        return TerminalAnsi().parse(text) to raw
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            check(PrivateStore(targetContext).connections().isEmpty())
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            try {
                val shown = mutableStateOf(snapshot(1, 57))
                val selecting = mutableStateOf(false)
                main { activity.setContent { MaterialTheme(colorScheme = darkColorScheme()) { Surface(Modifier.fillMaxSize()) {
                    Box(Modifier.systemBarsPadding().padding(12.dp).size(320.dp, 360.dp)) {
                        val (screen, raw) = shown.value
                        TerminalScreenView(screen, raw, 13f, selecting.value)
                    }
                } } } }
                // Opening a tall pane shows its bottom, where the cursor is, and the left edge.
                await("opening follows the cursor at the bottom") { at("down") && scroll()["left"] == false && find("Follow cursor") == null }
                screenshot("terminal-follow-open.png")
                // A higher cursor in the next snapshot is followed to the bottom edge with the rows above it.
                main { shown.value = snapshot(2, 5) }
                await("following tracks a new cursor row") { at("up") }
                main { shown.value = snapshot(3, 57) }
                await("following returns to the bottom") { at("down") }
                // A user scroll stops following; new snapshots then keep the user's position.
                val terminal = checkNotNull(nodes().firstOrNull { it.isScrollable }).let { Rect().also(it::getBoundsInScreen) }
                gesture((0..12).map { terminal.exactCenterX() to terminal.top + 40f + it * 25f })
                await("a user scroll stops following") { find("Follow cursor") != null && scroll()["down"] == true }
                main { shown.value = snapshot(4, 57) }
                SystemClock.sleep(600); waitForIdleSync()
                check(find("Follow cursor") != null && scroll()["down"] == true) { "A new snapshot moved a user scroll" }
                screenshot("terminal-follow-paused.png")
                tap("Follow cursor")
                await("Follow cursor resumes following") { at("down") && find("Follow cursor") == null }
                // A selection never moves, and its end resumes following.
                main { selecting.value = true; shown.value = snapshot(5, 5) }
                SystemClock.sleep(600); waitForIdleSync()
                check(at("down")) { "Following moved a selection" }
                main { selecting.value = false }
                await("following resumes after selection") { at("up") }
                screenshot("terminal-follow-resumed.png")
            } finally { main { activity.finish() } }
            result.putString("stream", "Terminal follow smoke passed: opens at the bottom cursor row, tracks a higher cursor and back, a user scroll pauses following across snapshots, Follow cursor resumes it, selection never moves. Synthetic 60x80 snapshots; no relay or native input.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) { result.putString("stream", "Terminal follow smoke failed: ${error.stackTraceToString()}\n"); finish(Activity.RESULT_CANCELED, result) }
    }
}
