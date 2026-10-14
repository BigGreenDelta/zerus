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
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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

/** Actual settings controls on an isolated emulator; never sends native settings. */
class NativeSettingsLayoutSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG); smoke()
            result.putString("stream", "Native settings layout passed: long model catalog, visible effort/selection, 48dp targets, exact choice IDs, per-model efforts, narrow/large text and explicit Apply.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error: Throwable) {
            screenshot("model-effort-failure.png")
            result.putString("stream", "Native settings layout failed: $error\n${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun await(condition: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 10_000
        while(!condition()) { check(SystemClock.elapsedRealtime() < until) { "Settings fixture timed out" };SystemClock.sleep(50) }
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
        return result
    }
    private fun radio(label: String): AccessibilityNodeInfo {
        var result = node(label)
        while(!result.isCheckable && result.parent != null) result = result.parent
        check(result.isCheckable) { "No radio choice owns $label" }
        return result
    }
    private fun bounds(node: AccessibilityNodeInfo) = Rect().also(node::getBoundsInScreen)
    private fun screenshot(name: String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null),name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
            bitmap.recycle()
        }
    }
    // Dialog owns another Compose root: override density inside its slots,
    // otherwise its Android window can replace an outer fixture's font scale.
    @Composable private fun ScaledSlot(scale: Float,content: @Composable () -> Unit) {
        val current = LocalDensity.current
        CompositionLocalProvider(LocalDensity provides Density(current.density,scale),content = content)
    }
    private fun smoke() {
        val activity = startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val labels = listOf("GPT-6.1-Sol","GPT-6-Astra","GPT-6-Sol","GPT-6-Luna","GPT-5.6-Sol","GPT-5.6-Terra","GPT-5.6-Luna")
        val levels = listOf("low","medium","high","xhigh","max","ultra")
        val models = labels.mapIndexed { index,label -> NativeModelChoice("model-$index",label,if(index == 1) listOf("low","high") else levels) }
        val selectedModel = mutableStateOf("model-0");val selectedEffort = mutableStateOf("xhigh")
        val scene = mutableIntStateOf(0);val layoutScene = AtomicInteger(-1)
        val density = AtomicReference(1f);val applied = AtomicInteger()
        runOnMainSync {
            activity.setContent {
                MaterialTheme(colorScheme = darkColorScheme(primary = Color(0xFF67E8CB),onPrimary = Color(0xFF101517),
                    secondary = Color(0xFFFFCB7D),background = Color(0xFF101517),surface = Color(0xFF1A2225),
                    surfaceContainer = Color(0xFF1A2225),onSurface = Color(0xFFE9F0F2),
                    onSurfaceVariant = Color(0xFF9AACB2),outline = Color(0xFF354449))) {
                    val original = LocalDensity.current;density.set(original.density)
                    CompositionLocalProvider(LocalDensity provides Density(original.density,if(scene.intValue == 1) 1.8f else 1f)) {
                        key(scene.intValue) {
                            ZerusAlertDialog(onDismissRequest = {},modifier = Modifier.width(if(scene.intValue == 1) 280.dp else 320.dp),
                                title = { ScaledSlot(if(scene.intValue == 1) 1.8f else 1f) { Text("Model and effort") } },text = {
                                    ScaledSlot(if(scene.intValue == 1) 1.8f else 1f) {
                                    Column(Modifier.heightIn(max = 440.dp).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                        Text("Settings apply to codex/example/session.",style = MaterialTheme.typography.bodySmall)
                                        NativeSettingsChoices(models,selectedModel.value,models.first { it.id == selectedModel.value }.efforts,selectedEffort.value,
                                            onModelChange = { id -> selectedModel.value = id
                                                if(selectedEffort.value !in models.first { it.id == id }.efforts) selectedEffort.value = "" },
                                            onEffortChange = { selectedEffort.value = it },
                                            modifier = Modifier.onGloballyPositioned { layoutScene.set(scene.intValue) })
                                        Text("Apply when the agent is ready.",style = MaterialTheme.typography.bodySmall)
                                    }
                                    }
                                },confirmButton = { ScaledSlot(if(scene.intValue == 1) 1.8f else 1f) {
                                    TextButton(onClick = { applied.incrementAndGet() }) { Text("Apply when ready") }
                                } },dismissButton = { ScaledSlot(if(scene.intValue == 1) 1.8f else 1f) {
                                    TextButton(onClick = {}) { Text("Cancel") }
                                } })
                        }
                    }
                }
            }
        }
        fun visible(label: String) {
            val item = node(label);val screen = bounds(uiAutomation.rootInActiveWindow)
            check(item.isVisibleToUser && bounds(item).width() > 0 && bounds(item).height() > 0 && screen.contains(bounds(item))) {
                "$label is hidden below the dialog viewport: ${bounds(item)} / $screen"
            }
        }
        await { layoutScene.get() == 0 }
        val regularHeadingHeight = bounds(node("Model")).height()
        visible("Model, GPT-6.1-Sol");visible("Effort: Extra high")
        for(label in listOf("Default","Low","Medium","High","Extra high","Max","Ultra")) {
            // The label is a child node; an already checked radio can be
            // non-clickable, so resolve the checkable owner independently.
            val choice = radio("Effort, $label");visible("Effort, $label")
            check(bounds(choice).height() >= density.get() * 48 - 1) { "Effort target is smaller than 48dp" }
            check(choice.isCheckable && choice.isChecked == (label == "Extra high")) {
                "Wrong effort accessibility state: $label (${choice.className}, checked=${choice.isChecked})"
            }
        }
        screenshot("model-effort-compact.png")
        check(button("Effort, Low").performAction(AccessibilityNodeInfo.ACTION_CLICK))
        await { selectedEffort.value == "low" };node("Effort: Low")
        check(applied.get() == 0) { "Selecting an effort submitted settings" }
        check(button("Effort, Ultra").performAction(AccessibilityNodeInfo.ACTION_CLICK));await { selectedEffort.value == "ultra" }
        check(button("Model, GPT-6.1-Sol").performAction(AccessibilityNodeInfo.ACTION_CLICK))
        check(button("GPT-6-Astra").performAction(AccessibilityNodeInfo.ACTION_CLICK))
        await { selectedModel.value == "model-1" && selectedEffort.value.isBlank() }
        node("Effort: Default")
        check(find(uiAutomation.rootInActiveWindow,"Effort, Ultra") == null) { "Unsupported effort remained selectable" }
        check(button("Effort, High").performAction(AccessibilityNodeInfo.ACTION_CLICK));await { selectedEffort.value == "high" }
        check(applied.get() == 0)
        check(button("Apply when ready").performAction(AccessibilityNodeInfo.ACTION_CLICK));await { applied.get() == 1 }

        runOnMainSync { selectedModel.value = "model-0";selectedEffort.value = "xhigh";scene.intValue = 1 }
        await { layoutScene.get() == 1 };uiAutomation.waitForIdle(100,3_000)
        check(bounds(node("Model")).height() >= regularHeadingHeight * 1.4) { "Large-font fixture did not scale the actual dialog text" }
        visible("Model, GPT-6.1-Sol");visible("Effort: Extra high");visible("Effort, Default")
        visible("Apply when ready");visible("Cancel")
        screenshot("model-effort-large-text.png")
        check(button("Effort, Default").performAction(AccessibilityNodeInfo.ACTION_CLICK));await { selectedEffort.value.isBlank() }
        check(applied.get() == 1) { "Default submitted settings" }
        runOnMainSync { activity.finish() }
    }
}
