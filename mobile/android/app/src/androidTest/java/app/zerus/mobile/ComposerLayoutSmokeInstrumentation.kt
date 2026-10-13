package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.AttachFile
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import java.io.File
import java.util.concurrent.atomic.AtomicReference

/** Native text geometry and editing fixtures; no transport, microphone or agent input. */
class ComposerLayoutSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG); smoke()
            result.putString("stream", "Composer layout passed: compact single-line input, wider editing, top-right expansion, selection retention, narrow/large text and read-only editing.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Composer layout failed: $error\n${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun await(condition: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 10_000
        while (!condition()) { check(SystemClock.elapsedRealtime() < until) { "Composer fixture timed out" }; SystemClock.sleep(50) }
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh(); if (predicate(root)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), predicate)?.let { return it }
        return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await { found = find(uiAutomation.rootInActiveWindow) { it.contentDescription?.toString() == label }; found != null }
        var button = checkNotNull(found)
        while (!button.isClickable && button.parent != null) button = button.parent
        return button
    }
    private fun bounds(label: String) = Rect().also { node(label).getBoundsInScreen(it) }
    private fun screenshot(name: String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null), name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
            bitmap.recycle()
        }
    }
    private fun smoke() {
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val sample = "Сообщение читается лучше, когда буквы крупнее, строки ближе, а кнопки не забирают лишнее место. "
        val editor = TextFieldState(initialText = sample.repeat(7))
        val scene = mutableIntStateOf(0); val locked = mutableStateOf(false)
        val surface = AtomicReference(Rect()); val density = AtomicReference(1f)
        val focus = FocusRequester()
        runOnMainSync {
            activity.setContent {
                MaterialTheme(colorScheme = darkColorScheme(primary = Color(0xFF67E8CB), background = Color(0xFF101517),
                    surfaceContainer = Color(0xFF1A2225), onSurface = Color(0xFFE9F0F2), onSurfaceVariant = Color(0xFF9AACB2), outline = Color(0xFF354449))) {
                    val original = LocalDensity.current; density.set(original.density)
                    CompositionLocalProvider(LocalDensity provides Density(original.density, if (scene.intValue == 1) 1.8f else 1f)) {
                        Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).systemBarsPadding().imePadding().padding(8.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Text("Conversation preview", color = MaterialTheme.colorScheme.onSurface, style = MaterialTheme.typography.titleMedium)
                            Surface(color = MaterialTheme.colorScheme.surfaceContainer, shape = RoundedCornerShape(18.dp)) {
                                Text(sample, Modifier.padding(horizontal = 12.dp, vertical = 8.dp), style = conversationTextStyle())
                            }
                            Spacer(Modifier.weight(1f))
                            // A system font/configuration change recreates the Activity.
                            // Give the independent configuration fixture fresh native nodes too.
                            key(scene.intValue) { CompactMessageInput(state = editor, placeholder = "Message", readOnly = locked.value,
                                inputTransformation = InputTransformation { if (locked.value) revertAllChanges() },
                                focusRequester = focus, onFocusChanged = {},
                                modifier = Modifier.width(if (scene.intValue == 1) 280.dp else 360.dp).onGloballyPositioned {
                                    val b = it.boundsInWindow(); surface.set(Rect(b.left.toInt(), b.top.toInt(), b.right.toInt(), b.bottom.toInt()))
                                }, leading = { IconButton(onClick = {}) { Icon(Icons.Default.AttachFile, "Attach files") } },
                                action = { FilledIconButton(onClick = {}, modifier = Modifier.size(48.dp)) { Icon(Icons.AutoMirrored.Filled.Send, "Send message") } }) }
                        }
                    }
                }
            }
        }
        fun geometry() {
            val attach = bounds("Attach files"); val send = bounds("Send message"); val expand = bounds("Expand message")
            val outer = surface.get(); val tolerance = (density.get() * 2).toInt()
            check(kotlin.math.abs(send.bottom - attach.bottom) <= tolerance) { "Bottom actions drifted" }
            check(expand.right == send.right && expand.bottom <= send.top) { "Expansion is not separate at top-right: $expand / $send" }
            val envelope = Rect(outer).apply { inset(-1, -1) }
            if (!(envelope.contains(attach) && envelope.contains(send) && envelope.contains(expand))) screenshot("composer-layout-failure.png")
            check(envelope.contains(attach) && envelope.contains(send) && envelope.contains(expand)) {
                "A control is outside the field: $outer / $attach / $send / $expand"
            }
            val editable = find(uiAutomation.rootInActiveWindow) { it.isEditable }
            checkNotNull(editable); val text = Rect(); editable.getBoundsInScreen(text)
            check(text.width() >= outer.width() * .60) { "Too little width left for message text: $text / $outer" }
        }
        geometry(); screenshot("composer-compact.png")
        val collapsed = surface.get().height()
        runOnMainSync { editor.edit { selection = TextRange(9, 16) } }
        check(node("Expand message").performAction(AccessibilityNodeInfo.ACTION_CLICK))
        node("Collapse message"); await { surface.get().height() > collapsed }
        runOnMainSync { check(editor.selection == TextRange(9, 16)) { "Expanding changed the selection" } }
        check(node("Collapse message").performAction(AccessibilityNodeInfo.ACTION_CLICK)); node("Expand message")
        await { surface.get().height() == collapsed }

        runOnMainSync { scene.intValue = 1 }
        await { surface.get().width() <= density.get() * 281 }
        uiAutomation.waitForIdle(100, 3_000); geometry(); screenshot("composer-large-text.png")
        runOnMainSync { scene.intValue = 0; editor.edit { replace(0, length, "Short draft") } }
        await { surface.get().height() <= density.get() * 50 }
        runOnMainSync { focus.requestFocus() }
        var editable: AccessibilityNodeInfo? = null
        await { editable = find(uiAutomation.rootInActiveWindow) { it.isEditable }; editable != null }
        val replacement = Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, "Edited through native input") }
        check(checkNotNull(editable).performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, replacement))
        await { editor.text.toString() == "Edited through native input" }
        runOnMainSync { locked.value = true }
        SystemClock.sleep(150)
        editable = find(uiAutomation.rootInActiveWindow) { it.className?.toString() == "android.widget.EditText" }
        editable?.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, "Must stay rejected")
        })
        SystemClock.sleep(150)
        runOnMainSync { check(editor.text.toString() == "Edited through native input") { "Recording guard accepted an edit" }; activity.finish() }
    }
}
