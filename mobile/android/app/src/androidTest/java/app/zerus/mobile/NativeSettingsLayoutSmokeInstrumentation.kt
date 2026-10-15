package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.width
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual settings sheet on an isolated emulator with an explicitly synthetic catalog. */
class NativeSettingsLayoutSmokeInstrumentation : Instrumentation() {
    private lateinit var screenBounds: Rect
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments);start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG);smoke()
            result.putString("stream", "Native settings layout passed: native catalog order, pinned effort entry, separate effort page, Back, staged exact IDs, per-model efforts, 48dp targets, narrow/large text and explicit Apply.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error: Throwable) {
            screenshot("model-effort-sheet-failure.png")
            result.putString("stream", "Native settings layout failed: $error\n${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun await(condition: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 10_000
        while(!condition()) {
            check(SystemClock.elapsedRealtime() < until) { "Settings fixture timed out" }
            SystemClock.sleep(50)
        }
    }
    private fun find(root: AccessibilityNodeInfo?,label: String): AccessibilityNodeInfo? {
        if(root == null) return null
        root.refresh()
        if(root.text?.toString() == label || root.contentDescription?.toString() == label) return root
        for(index in 0 until root.childCount) find(root.getChild(index),label)?.let { return it }
        return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await { found = find(uiAutomation.rootInActiveWindow,label);found != null }
        return checkNotNull(found)
    }
    private fun button(label: String): AccessibilityNodeInfo {
        var result = node(label)
        while(!result.isClickable && result.parent != null) result = result.parent
        check(result.isClickable) { "No clickable choice owns $label" }
        return result
    }
    private fun radio(label: String): AccessibilityNodeInfo {
        var result = node(label)
        while(!result.isCheckable && result.parent != null) result = result.parent
        check(result.isCheckable) { "No radio choice owns $label" }
        return result
    }
    private fun bounds(node: AccessibilityNodeInfo) = Rect().also(node::getBoundsInScreen)
    private fun visible(label: String) {
        await {
            val item = find(uiAutomation.rootInActiveWindow,label)
            item != null && item.isVisibleToUser && bounds(item).width() > 0 &&
                bounds(item).height() > 0 && screenBounds.contains(bounds(item))
        }
    }
    private fun click(label: String) {
        visible(label)
        check(button(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Cannot click $label" }
    }
    private fun screenshot(name: String) {
        uiAutomation.waitForIdle(100,3_000)
        // Accessibility can become idle during sheet motion. Wait for the full
        // footer to be inside the physical screen and its bounds to settle.
        visible("Cancel")
        var last = Rect();var stableSince = SystemClock.elapsedRealtime()
        await {
            val current = bounds(button("Cancel"))
            if(current != last) { last = current;stableSince = SystemClock.elapsedRealtime() }
            screenBounds.contains(current) && SystemClock.elapsedRealtime() - stableSince >= 300
        }
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null),name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
            bitmap.recycle()
        }
    }
    // The sheet has its own Compose window: scale inside its content, then verify
    // the actual heading grows rather than trusting an outer density override.
    @Composable private fun ScaledSlot(scale: Float,content: @Composable () -> Unit) {
        val current = LocalDensity.current
        CompositionLocalProvider(LocalDensity provides Density(current.density,scale),content = content)
    }
    @OptIn(ExperimentalMaterial3Api::class)
    private fun smoke() {
        val activity = startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        checkNotNull(uiAutomation.takeScreenshot()).let { bitmap ->
            screenBounds = Rect(0,0,bitmap.width,bitmap.height);bitmap.recycle()
        }
        val labels = listOf("Example Zeta","Example Alpha","Example Delta","Example Beta") + (5..12).map { "Example model $it" }
        val levels = listOf("low","medium","high","xhigh","max","ultra")
        val models = labels.mapIndexed { index,label -> NativeModelChoice("model-$index",label,if(index == 1) listOf("low","high") else levels) }
        val settings = NativeSettings("model-0","xhigh",models,levels,"","","","ready",true,"")
        val selectedModel = mutableStateOf("model-0");val selectedEffort = mutableStateOf("xhigh")
        val scene = mutableIntStateOf(0);val layoutScene = AtomicInteger(-1)
        val density = AtomicReference(1f);val applied = AtomicInteger();val dismissed = AtomicInteger()
        runOnMainSync {
            activity.setContent {
                MaterialTheme(colorScheme = darkColorScheme(primary = Color(0xFF67E8CB),onPrimary = Color(0xFF101517),
                    secondary = Color(0xFFFFCB7D),background = Color(0xFF101517),surface = Color(0xFF1A2225),
                    surfaceContainer = Color(0xFF1A2225),onSurface = Color(0xFFE9F0F2),
                    onSurfaceVariant = Color(0xFF9AACB2),outline = Color(0xFF354449))) {
                    density.set(LocalDensity.current.density)
                    key(scene.intValue) {
                        ModalBottomSheet(onDismissRequest = { dismissed.incrementAndGet() },
                            sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
                            modifier = Modifier.width(if(scene.intValue == 1) 280.dp else 320.dp)) {
                            ScaledSlot(if(scene.intValue == 1) 1.8f else 1f) {
                                NativeSettingsSheetContent(settings,"Layout preview / sample catalog",selectedModel.value,selectedEffort.value,"",
                                    onModelChange = { id ->
                                        selectedModel.value = id
                                        if(selectedEffort.value !in models.first { it.id == id }.efforts) selectedEffort.value = ""
                                    },onEffortChange = { selectedEffort.value = it },
                                    onApply = { applied.incrementAndGet() },onDismiss = { dismissed.incrementAndGet() },
                                    modifier = Modifier.onGloballyPositioned { layoutScene.set(scene.intValue) })
                            }
                        }
                    }
                }
            }
        }
        await { layoutScene.get() == 0 }
        visible("Choose effort, Extra high");visible("Select model");visible("Apply when ready");visible("Cancel")
        val regularHeadingHeight = bounds(node("Select model")).height()
        check(radio("Model, Example Zeta").isChecked)
        check(bounds(node("Model, Example Zeta")).top < bounds(node("Model, Example Alpha")).top) { "Model labels were sorted alphabetically" }
        screenshot("models-sheet.png")

        // A long catalog scrolls without moving the effort entry or actions.
        var list = node("Model, Example Zeta")
        while(!list.isScrollable && list.parent != null) list = list.parent
        check(list.isScrollable && list.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD))
        visible("Choose effort, Extra high");visible("Cancel")
        click("Choose effort, Extra high");visible("Back to models")
        for(label in listOf("Default","Low","Medium","High","Extra high","Max","Ultra")) {
            val choice = radio("Effort, $label");visible("Effort, $label")
            check(bounds(choice).height() >= density.get() * 48 - 1) { "Effort target is smaller than 48dp" }
            check(choice.isChecked == (label == "Extra high")) { "Wrong effort accessibility state for $label" }
        }
        visible("Apply when ready");visible("Cancel")
        screenshot("effort-sheet.png")
        click("Effort, Low");await { selectedEffort.value == "low" }
        check(applied.get() == 0) { "Selecting an effort submitted settings" }
        click("Back to models");visible("Choose effort, Low")
        check(selectedModel.value == "model-0" && applied.get() == 0 && dismissed.get() == 0)
        click("Choose effort, Low");click("Effort, Ultra");await { selectedEffort.value == "ultra" }
        sendKeyDownUpSync(KeyEvent.KEYCODE_BACK)
        visible("Choose effort, Ultra")
        check(dismissed.get() == 0) { "System Back dismissed the form instead of returning to models" }

        // Reopening the model page reveals the selected model without reordering.
        visible("Model, Example Alpha");click("Model, Example Alpha")
        await { selectedModel.value == "model-1" && selectedEffort.value.isBlank() }
        visible("Choose effort, Default");click("Choose effort, Default")
        visible("Effort, Default");visible("Effort, Low");visible("Effort, High")
        check(find(uiAutomation.rootInActiveWindow,"Effort, Ultra") == null) { "Unsupported effort remained selectable" }
        click("Effort, High");await { selectedEffort.value == "high" }
        click("Back to models");visible("Choose effort, High")
        check(applied.get() == 0) { "Page navigation submitted settings" }
        click("Apply when ready");await { applied.get() == 1 }

        runOnMainSync { selectedModel.value = "model-0";selectedEffort.value = "xhigh";scene.intValue = 1 }
        await { layoutScene.get() == 1 };visible("Choose effort, Extra high")
        check(bounds(node("Select model")).height() >= regularHeadingHeight * 1.4) { "Large-font fixture did not scale the actual sheet text" }
        visible("Model, Example Zeta");visible("Apply when ready");visible("Cancel")
        check(bounds(button("Choose effort, Extra high")).width() <= density.get() * 280) { "Narrow fixture exceeded 280dp" }
        screenshot("models-sheet-large-text.png")
        click("Choose effort, Extra high");visible("Back to models");visible("Effort, Extra high")
        check(radio("Effort, Extra high").isChecked)
        visible("Apply when ready");visible("Cancel")
        screenshot("effort-sheet-large-text.png")
        click("Back to models");visible("Choose effort, Extra high")
        check(applied.get() == 1 && dismissed.get() == 0) { "Large-text navigation submitted or dismissed settings" }
        click("Cancel");await { dismissed.get() == 1 }
        runOnMainSync { activity.finish() }
    }
}
