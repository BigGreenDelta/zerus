package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Pure dialog fixtures on an isolated emulator: no transport, microphone or agent input. */
class DialogActionsSmokeInstrumentation : Instrumentation() {
    private val regularLineHeight=AtomicReference(0f)
    private val largeLineHeight=AtomicReference(0f)
    private val regularBodyHeight=AtomicReference(0f)
    private val largeBodyHeight=AtomicReference(0f)
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG); smoke()
            result.putString("stream", "Dialog actions passed: Cancel rightmost, actual title line-height ${regularLineHeight.get()} -> ${largeLineHeight.get()}, body ${regularBodyHeight.get()} -> ${largeBodyHeight.get()}, wrapped large-text actions visible, Enter/Back cancellation and pending-operation locks.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Dialog actions failed: $error\n${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun await(condition: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 10_000
        while (!condition()) { check(SystemClock.elapsedRealtime() < until) { "Dialog fixture timed out" }; SystemClock.sleep(50) }
    }
    private fun node(label: String): AccessibilityNodeInfo {
        fun find(root: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (root == null) return null
            root.refresh()
            if (root.text?.toString() == label) return root
            for (index in 0 until root.childCount) find(root.getChild(index))?.let { return it }
            return null
        }
        var found: AccessibilityNodeInfo? = null
        await { found = find(uiAutomation.rootInActiveWindow); found != null }
        return checkNotNull(found)
    }
    private fun button(label: String): AccessibilityNodeInfo {
        var found = node(label)
        // Match ConversationNavigationSmoke: a fresh label can still have a cached action owner.
        while (true) {
            found.refresh()
            if(found.isClickable || !found.isEnabled || found.parent==null) break
            found=checkNotNull(found.parent)
        }
        check(found.isClickable || !found.isEnabled) { "No action owner for $label" }
        return found
    }
    private fun bounds(label: String) = Rect().also { button(label).getBoundsInScreen(it) }
    // Android dialogs have a separate Compose window. Scale each actual slot.
    @Composable private fun ScaledSlot(large:Boolean,content:@Composable () -> Unit) {
        val density=LocalDensity.current
        CompositionLocalProvider(LocalDensity provides Density(density.density,if(large)1.8f else 1f),content=content)
    }
    private fun smoke() {
        // Enter tests keyboard focus; an earlier owned fixture may have left touch mode on.
        setInTouchMode(false)
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val scene = mutableStateOf(0); val shown = mutableStateOf(true); val busy = mutableStateOf(false)
        val confirmed = AtomicInteger(); val dismissed = AtomicInteger()
        runOnMainSync {
            activity.setContent {
                MaterialTheme {
                    val density = LocalDensity.current
                    CompositionLocalProvider(LocalDensity provides Density(density.density, if (scene.value == 2) 1.8f else 1f)) {
                        if (shown.value) {
                            fun cancel() { if (!busy.value) { dismissed.incrementAndGet(); shown.value = false } }
                            ZerusAlertDialog(onDismissRequest = ::cancel, modifier = Modifier.width(if (scene.value == 2) 280.dp else 360.dp),
                                title = { ScaledSlot(scene.value==2) { Text("Fixture confirmation", onTextLayout = { layout ->
                                    val height=layout.getLineBottom(0)-layout.getLineTop(0)
                                    if(scene.value==2) largeLineHeight.set(height) else regularLineHeight.set(height)
                                }) } }, text = { ScaledSlot(scene.value==2) { Text("Review the selected action.",onTextLayout={ layout ->
                                    val height=layout.getLineBottom(0)-layout.getLineTop(0)
                                    if(scene.value==2) largeBodyHeight.set(height) else regularBodyHeight.set(height)
                                }) } },
                                confirmButton = { ScaledSlot(scene.value==2) {
                                    TextButton(enabled = !busy.value, onClick = { confirmed.incrementAndGet() }) {
                                        Text(if (busy.value) "Saving…" else if (scene.value == 2) "Submit with full conversation" else "Save")
                                    }
                                    if (scene.value == 2) TextButton(onClick = { confirmed.incrementAndGet() }) { Text("Check original receipt") }
                                } }, dismissButton = { ScaledSlot(scene.value==2) {
                                    TextButton(enabled = !busy.value, onClick = ::cancel) { Text("Cancel") }
                                } })
                        }
                    }
                }
            }
        }
        val save = bounds("Save"); val cancel = bounds("Cancel")
        await { regularLineHeight.get()>0 }
        check(save.right <= cancel.left && save.top == cancel.top) { "Cancel is not rightmost: $save / $cancel" }
        await { button("Cancel").isFocused }
        sendKeyDownUpSync(KeyEvent.KEYCODE_ENTER)
        await { !shown.value }; check(dismissed.get() == 1 && confirmed.get() == 0) { "Enter submitted instead of cancelling" }

        runOnMainSync { scene.value = 1; shown.value = true }
        node("Save")
        runOnMainSync { busy.value = true }; node("Saving…")
        await { !button("Saving…").isEnabled && !button("Cancel").isEnabled }
        check(!button("Saving…").isEnabled && !button("Cancel").isEnabled) { "Pending action controls stayed enabled" }
        button("Saving…").performAction(AccessibilityNodeInfo.ACTION_CLICK)
        sendKeyDownUpSync(KeyEvent.KEYCODE_BACK); SystemClock.sleep(200)
        check(shown.value && confirmed.get() == 0 && dismissed.get() == 1) { "A pending operation was submitted or dismissed" }
        runOnMainSync { busy.value = false }; node("Save")
        check(button("Cancel").performAction(AccessibilityNodeInfo.ACTION_CLICK)); await { !shown.value }

        runOnMainSync { scene.value = 2; shown.value = true }
        await { largeLineHeight.get()>0 && largeBodyHeight.get()>0 }
        check(largeBodyHeight.get()>=regularBodyHeight.get()*1.4f && largeLineHeight.get()>regularLineHeight.get()) {
            "Outer fixture did not scale actual dialog text: body ${regularBodyHeight.get()} -> ${largeBodyHeight.get()}, title ${regularLineHeight.get()} -> ${largeLineHeight.get()}"
        }
        val primary = bounds("Submit with full conversation"); val receipt = bounds("Check original receipt"); val wrappedCancel = bounds("Cancel")
        check(wrappedCancel.top >= primary.bottom && wrappedCancel.top >= receipt.bottom) { "Wrapped cancellation is not last" }
        check(primary.width() > 0 && receipt.width() > 0 && wrappedCancel.width() > 0)
        check(wrappedCancel.right >= primary.right && wrappedCancel.right >= receipt.right) { "Wrapped actions lost end alignment" }
        val screen = Rect(); uiAutomation.rootInActiveWindow.getBoundsInScreen(screen)
        check(listOf(primary, receipt, wrappedCancel).all(screen::contains)) { "An action lies outside the dialog" }
        sendKeyDownUpSync(KeyEvent.KEYCODE_BACK); await { !shown.value }
        check(confirmed.get() == 0 && dismissed.get() == 3) { "Back submitted an action" }
        runOnMainSync { activity.finish() }
    }
}
