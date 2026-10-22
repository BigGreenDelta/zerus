package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicReference

/** Synthetic owned-emulator Terminal links only; nothing opens, and no relay or file read starts. */
class TerminalLinkSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>(); runOnMainSync { value.set(runCatching(action)) }; return value.get().getOrThrow()
    }
    private fun await(label: String, action: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 15_000
        while (!action()) {
            if (SystemClock.elapsedRealtime() >= until) {
                screenshot("terminal-link-failure.png")
                all().forEach { android.util.Log.i("TerminalLinkSmoke", "${it.className} text=${it.text?.toString()?.take(60)} desc=${it.contentDescription} click=${it.isClickable} actions=${it.actionList.map { a -> a.id }}") }
                error(label)
            }
            SystemClock.sleep(40)
        }
    }
    private fun nodes(root: AccessibilityNodeInfo?): List<AccessibilityNodeInfo> {
        root ?: return emptyList(); root.refresh()
        return listOf(root) + (0 until root.childCount).flatMap { nodes(root.getChild(it)) }
    }
    private fun all() = uiAutomation.windows.flatMap { nodes(it.root) }
    private fun find(label: String) = all().firstOrNull { it.isVisibleToUser && (it.text?.toString() == label || it.contentDescription?.toString() == label) }
    private fun screenshot(name: String) {
        uiAutomation.waitForIdle(100, 2_000); waitForIdleSync()
        val frame = checkNotNull(uiAutomation.takeScreenshot())
        try { File(targetContext.getExternalFilesDir(null), name).outputStream().use { check(frame.compress(Bitmap.CompressFormat.PNG, 100, it)) } } finally { frame.recycle() }
    }
    private fun tap(node: AccessibilityNodeInfo) {
        val rect = Rect().also(node::getBoundsInScreen)
        val down = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val property = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
            val coordinate = MotionEvent.PointerCoords().apply { x = rect.exactCenterX(); y = rect.exactCenterY(); pressure = 1f; size = .1f }
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 1, arrayOf(property), arrayOf(coordinate), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }; SystemClock.sleep(40)
        }
        uiAutomation.waitForIdle(100, 2_000); waitForIdleSync()
    }
    private fun terminalText() = all().firstOrNull { it.text?.toString()?.startsWith("see docs link") == true }
    /** Compose exposes each link as a ClickableSpan of the Terminal text node. */
    private fun linkRanges(): List<IntRange> {
        val text = terminalText()?.text as? android.text.Spanned ?: return emptyList()
        return text.getSpans(0, text.length, android.text.style.ClickableSpan::class.java).map { text.getSpanStart(it) until text.getSpanEnd(it) }.sortedBy { it.first }
    }
    /** Taps the middle character of a text range at its laid-out screen position. */
    private fun tapText(start: Int, length: Int) {
        val node = checkNotNull(terminalText())
        val key = AccessibilityNodeInfo.EXTRA_DATA_TEXT_CHARACTER_LOCATION_KEY
        check(node.refreshWithExtraData(key, Bundle().apply {
            putInt(AccessibilityNodeInfo.EXTRA_DATA_TEXT_CHARACTER_LOCATION_ARG_START_INDEX, start + length / 2)
            putInt(AccessibilityNodeInfo.EXTRA_DATA_TEXT_CHARACTER_LOCATION_ARG_LENGTH, 1)
        }))
        @Suppress("DEPRECATION") val box = checkNotNull(node.extras.getParcelableArray(key)?.firstOrNull() as? android.graphics.RectF) { "No character location" }
        val rect = Rect(box.left.toInt(), box.top.toInt(), box.right.toInt(), box.bottom.toInt())
        val down = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val property = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
            val coordinate = MotionEvent.PointerCoords().apply { x = rect.exactCenterX(); y = rect.exactCenterY(); pressure = 1f; size = .1f }
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 1, arrayOf(property), arrayOf(coordinate), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }; SystemClock.sleep(40)
        }
        uiAutomation.waitForIdle(100, 2_000); waitForIdleSync()
    }
    private fun press(label: String) {
        var owner = checkNotNull(find(label)) { "Missing $label" }
        while (!owner.isClickable) owner = checkNotNull(owner.parent)
        tap(owner)
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            uiAutomation.serviceInfo = uiAutomation.serviceInfo.apply {
                flags = flags or android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
            }
            check(PrivateStore(targetContext).connections().isEmpty())
            val esc = "\u001b"
            // tmux 3.8 capture-pane -e output with a web link and an ls --hyperlink style file link.
            val text = "see $esc]8;id=a;https://example.com/docs?q=1$esc\\docs link$esc]8;;$esc\\ and " +
                "$esc]8;;file://archlinux/tmp/a%20b.txt$esc\\a file$esc]8;;$esc\\ end\nplain row\n\n\n\nfar from every link"
            val screen = TerminalAnsi().parse(text)
            check(screen.text == "see docs link and a file end\nplain row\n\n\n\nfar from every link") { screen.text }
            val raw = JSONObject().put("cols", 40).put("rows", 6).put("screen", text).put("cursor", JSONObject().put("x", 0).put("y", 1))
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            try {
                val selecting = mutableStateOf(false)
                val tapped = mutableStateListOf<TerminalLink>()
                val choice = mutableStateOf<TerminalLinkChoice?>(null)
                val files = mutableStateListOf<ComputerFileReference>()
                main { activity.setContent { MaterialTheme(colorScheme = darkColorScheme()) { Surface(Modifier.fillMaxSize()) {
                    Box(Modifier.systemBarsPadding().padding(12.dp).fillMaxSize()) {
                        TerminalScreenView(screen, raw, 18f, selecting.value) { link ->
                            tapped += link
                            choice.value = TerminalLinkChoice(TerminalLinkTarget.of(link.uri), screen.text.substring(link.start, link.end))
                        }
                    }
                    choice.value?.let { chosen -> TerminalLinkDialog(chosen, "fixture-computer", onDismiss = { choice.value = null }) { files += it; choice.value = null } }
                } } } }
                // A web link asks first and shows its whole target.
                await("two link spans") { linkRanges() == listOf(4 until 13, 18 until 24) }
                tapText(4, 9)
                await("web link confirmation") { find("Open link?") != null && find("https://example.com/docs?q=1") != null }
                check(tapped.toList() == listOf(TerminalLink(4, 13, "https://example.com/docs?q=1"))) { tapped.toList().toString() }
                check(find("Open in browser") != null && find("Shown as: docs link") != null)
                screenshot("terminal-link-web.png")
                press("Copy link")
                await("web confirmation closed") { find("Open link?") == null }
                val clip = main { (targetContext.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager).primaryClip?.getItemAt(0)?.text?.toString() }
                check(clip == "https://example.com/docs?q=1") { "Clipboard: $clip" }
                // A file link names its own host and reads only after the next explicit choice.
                tapText(18, 6)
                await("file link confirmation") { find("Read file from fixture-computer?") != null && find("Read on fixture-computer") != null }
                check(all().any { it.text?.toString()?.contains("The link names host archlinux.") == true })
                screenshot("terminal-link-file.png")
                press("Read on fixture-computer")
                await("file choice handed to computer files") { files.toList() == listOf(ComputerFileReference("/tmp/a b.txt")) && find("Read file from fixture-computer?") == null }
                // Plain text and a selection never reach a link.
                // Compose widens a link's touch target, so plain text is tapped well away from both links.
                tapText(42, 19)
                main { selecting.value = true }
                await("a selection leaves links inert") { terminalText() != null && linkRanges().isEmpty() }
                tapText(4, 9)
                check(tapped.size == 2 && choice.value == null) { tapped.toList().toString() }
            } finally { main { activity.finish() } }
            result.putString("stream", "Terminal link smoke passed: OSC 8 web and file links are separate tappable ranges, the confirmation shows the whole target and host, Copy link copies it, Read on hands only the parsed path to computer files, selection leaves links inert. Nothing opened.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) { result.putString("stream", "Terminal link smoke failed: ${error.stackTraceToString()}\n"); finish(Activity.RESULT_CANCELED, result) }
    }
}
