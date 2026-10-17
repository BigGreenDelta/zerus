package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.runBlocking
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.Base64
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference

/** Synthetic owned-emulator media only; opening menus never executes a command or contacts a relay. */
class RecordingMessageLayoutSmokeInstrumentation : Instrumentation() {
    private var narrow = false
    private var light = false
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments); narrow = arguments?.getString("narrow") == "true"; light = arguments?.getString("light") == "true"; start()
    }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>(); runOnMainSync { value.set(runCatching(action)) }; return value.get().getOrThrow()
    }
    private fun await(label: String, action: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 15_000
        while (!action()) { check(SystemClock.elapsedRealtime() < until) { label }; SystemClock.sleep(40) }
    }
    private fun find(label: String, root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        root ?: return null; root.refresh()
        if (root.isVisibleToUser && (root.text?.toString() == label || root.contentDescription?.toString() == label)) return root
        for (i in 0 until root.childCount) find(label, root.getChild(i))?.let { return it }; return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var result: AccessibilityNodeInfo? = null
        await(label) { result = find(label); result != null }; return checkNotNull(result)
    }
    private fun press(label: String) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var owner = node(label)
        while (!owner.isClickable) owner = checkNotNull(owner.parent)
        owner.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.id)
        var previous: Rect? = null
        var stable = 0
        var rect = Rect()
        // Recomposition/accessibility publication can precede the committed media layout.
        // Keep the original total deadline and tap only this action's fresh, settled bounds.
        while (stable < 3) {
            check(SystemClock.elapsedRealtime() < deadline) { "Unsettled $label" }
            var current = find(label)
            while (current != null && !current.isClickable) current = current.parent
            if (current != null && current.isEnabled) {
                owner = current
                rect = Rect().also(current::getBoundsInScreen)
                stable = if (rect == previous) stable + 1 else 0
                previous = Rect(rect)
            } else stable = 0
            if (stable < 3) SystemClock.sleep(40)
        }
        val display = targetContext.resources.displayMetrics
        check(rect.width() >= 48 * display.density - 2 && rect.height() >= 48 * display.density - 2) { "Small $label: $rect" }
        check(rect.left >= 0 && rect.right <= display.widthPixels && rect.top >= 0 && rect.bottom <= display.heightPixels) { "Offscreen $label: $rect" }
        val down = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val property = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
            val coordinate = MotionEvent.PointerCoords().apply { x = rect.exactCenterX(); y = rect.exactCenterY(); pressure = 1f; size = .1f }
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, 1, arrayOf(property), arrayOf(coordinate), 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }; SystemClock.sleep(40)
        }
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState() == MessageState() && store.voiceState() == VoiceState())
            val target = Target("fixture-computer", "codex/fixture/media", "fixture-run", "fixture-conversation", "fixture-workspace")
            val audio = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(ByteArray(32_000))) }
            val bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888).apply { eraseColor(Color.rgb(54, 115, 104)) }
            val poster = ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.JPEG, 85, it) }.toByteArray(); bitmap.recycle()
            val entries = listOf("submitted", "transcribing", "uncertain", "failed").map { state ->
                val id = UUID.randomUUID().toString()
                val voice = VoiceMessage(id, target.run, target.conversation, 1_791_019_000.0, 1, state,
                    audio.displayed().copy(messageId = id, index = 0), 1000, transcript = if (state == "submitted") "Literal recognized text.\nSecond line." else "",
                    error = if (state in setOf("failed", "uncertain")) "Delivery needs review. No automatic resend." else "",
                    waveform = List(64) { (it * 37) % 256 })
                VoiceOutgoing(id, target, "fixture-machine", "fixture-profile", audio, 1000, voice.createdAt, voice)
            }
            store.saveVoiceState(VoiceState(outgoing = entries))
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            try {
                val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
                await("private state") { main { model.storageReady && model.speech.ready } }
                val sample = mutableStateOf(entries[0].displayed())
                val scheme = if (light) lightColorScheme() else Class.forName("app.zerus.mobile.MainActivityKt")
                    .getDeclaredField("Scheme").apply { isAccessible = true }.get(null) as ColorScheme
                main { activity.setContent {
                    MaterialTheme(colorScheme = scheme) {
                        val density = LocalDensity.current
                        CompositionLocalProvider(LocalDensity provides Density(density.density, if (narrow) 1.8f else 1f)) {
                            Surface(Modifier.fillMaxSize()) {
                                Column(Modifier.systemBarsPadding().verticalScroll(rememberScrollState()).padding(12.dp)) {
                                    Surface(Modifier.width(if (narrow) 280.dp else 360.dp), shape = androidx.compose.foundation.shape.RoundedCornerShape(17.dp),
                                        color = MaterialTheme.colorScheme.surfaceVariant) {
                                        Column(Modifier.padding(12.dp)) { VoiceMessageContent(model, target, sample.value) }
                                    }
                                }
                            }
                        }
                    }
                } }
                for (video in listOf(false, true)) for (entry in entries) {
                    val voice = entry.displayed().let {
                        if (!video) it else it.copy(video = VideoMessage(DisplayedAttachment("video.mp4", "video/mp4", 4000, messageId = it.id, index = 1), 1000, 480, 480, Base64.getEncoder().encodeToString(poster)))
                    }
                    main { sample.value = voice }; waitForIdleSync()
                    node(if (voice.state == "transcribing") VoiceProtocol.label(voice.state) else RecordingDelivery.of(voice.state).description)
                    check(find("Sent") == null && find("Voice message") == null && find("Video message") == null)
                    check(find("Check status") == null && find("Cancel delivery") == null)
                    if (voice.transcript.isNotBlank()) {
                        check(find(voice.transcript) == null); press("Show recognized text"); node(voice.transcript)
                        press("Hide recognized text"); await("transcript collapsed") { find(voice.transcript) == null }
                    }
                    if (voice.state in setOf("transcribing", "uncertain")) {
                        press("Recording actions"); node("Check status"); node("Cancel delivery")
                        sendKeyDownUpSync(android.view.KeyEvent.KEYCODE_BACK); waitForIdleSync()
                        await("recording menu dismissed") { find("Check status") == null && find("Cancel delivery") == null }
                    }
                    if (voice.error.isNotBlank()) node(voice.error)
                    uiAutomation.takeScreenshot()?.let { image ->
                        File(targetContext.getExternalFilesDir(null), "media-${if (light) "light" else "dark"}-${if (narrow) "narrow" else "normal"}-${if (video) "video" else "audio"}-${voice.state}.png")
                            .outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }; image.recycle()
                    }
                }
                check(main { model.outgoing.isEmpty() && model.speech.activeRequests.isEmpty() && model.speech.outgoing.map { it.id } == entries.map { it.id } })
            } finally { main { activity.finish() } }
            result.putString("stream", "Recording media layout passed: audio/video, sent/processing/uncertain/failed, exact transcript expansion, physical48dp action menus, visible errors and no scattered command/status text. Synthetic ${if (light) "light" else "dark"}/${if (narrow) "narrow280dp-font1.8" else "normal"}; no dispatch or native mutations.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) { result.putString("stream", "Recording layout failed: ${error.stackTraceToString()}\n"); finish(Activity.RESULT_CANCELED, result) }
    }
}
