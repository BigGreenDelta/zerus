package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import android.view.InputDevice
import android.view.MotionEvent
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.runBlocking
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Base64
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.*

/** Two synthetic messages through the actual conversation renderer; no relay or capture mutations. */
class RoundVideoMessageLayoutSmokeInstrumentation : Instrumentation() {
    private var narrow = false
    private var light = false
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        narrow = arguments?.getString("narrow") == "true"
        light = arguments?.getString("light") == "true"
        start()
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) { check(SystemClock.elapsedRealtime() < end) { label }; SystemClock.sleep(40) }
    }
    private fun find(label: String, root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        root ?: return null
        if (root.isVisibleToUser && (root.text?.toString() == label || root.contentDescription?.toString() == label)) return root
        for (i in 0 until root.childCount) find(label, root.getChild(i))?.let { return it }
        return null
    }
    private fun group(index: Int): AccessibilityNodeInfo? = find("Fixture message $index")
    private fun node(index: Int, label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await("Message $index: $label") { found = find(label, group(index)); found != null }
        return checkNotNull(found)
    }
    private fun click(index: Int, label: String) {
        var owner = node(index, label)
        while (!owner.isClickable) owner = checkNotNull(owner.parent)
        owner.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.id)
        var previous: Rect? = null
        var stable = 0
        var bounds = Rect()
        // Accessibility publication can precede the committed frame after a card changes height.
        // Reacquire and pin the measured target; never tap a previous layout's coordinates.
        await("Stable message $index $label bounds") {
            var current = find(label, group(index))
            while (current != null && !current.isClickable) current = current.parent
            if (current == null || !current.isEnabled) false else {
                owner = current
                bounds = Rect().also(current::getBoundsInScreen)
                stable = if (bounds == previous) stable + 1 else 0
                previous = Rect(bounds)
                stable >= 3
            }
        }
        val display = targetContext.resources.displayMetrics
        val density = display.density
        check(bounds.width() >= 48 * density - 2 && bounds.height() >= 48 * density - 2) { "Small $label: $bounds" }
        check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= display.widthPixels && bounds.bottom <= display.heightPixels) {
            "Offscreen $label: $bounds"
        }
        android.util.Log.i("RoundLayoutFixture", "Tap message $index $label at $bounds")
        val down = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action,
                bounds.exactCenterX(), bounds.exactCenterY(), 0).apply { source = InputDevice.SOURCE_TOUCHSCREEN }
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
            SystemClock.sleep(40)
        }
        waitForIdleSync()
    }
    private fun screenshot(name: String) {
        waitForIdleSync()
        uiAutomation.takeScreenshot()?.let { image ->
            File(targetContext.getExternalFilesDir(null), "round-${if (light) "light" else "dark"}-${if (narrow) "narrow" else "normal"}-$name.png")
                .outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
            image.recycle()
        }
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState() == MessageState() && store.voiceState() == VoiceState())
            val target = Target("fixture-computer", "codex/fixture/round", "fixture-run", "fixture-conversation", "fixture-workspace")
            val samples = IntArray(96_000) { (sin(it * 2 * PI * 220 / 16_000) *
                (.1 + .7 * abs(sin(it / 6000.0))) * Short.MAX_VALUE).toInt() }
            val pcm = ByteBuffer.allocate(samples.size * 2).order(ByteOrder.LITTLE_ENDIAN)
                .also { buffer -> samples.forEach { buffer.putShort(it.toShort()) } }.array()
            val rms = (0 until 64).map { bin ->
                val start = bin * samples.size / 64; val end = (bin + 1) * samples.size / 64
                sqrt((start until end).sumOf { samples[it].toDouble().pow(2) } / (end - start))
            }
            val waveform = rms.map { (it / rms.max() * 255).roundToInt() }
            val audio = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(pcm)) }
            val movie = File(targetContext.getExternalFilesDir(null), "round-layout-fixture.mp4").readBytes()
            check(movie.copyOfRange(4, 8).contentEquals("ftyp".toByteArray()))
            val video = runBlocking { AttachmentStore(targetContext).importVideo(movie) }
            val entries = (1..2).map { index ->
                val bitmap = Bitmap.createBitmap(96, 96, Bitmap.Config.ARGB_8888)
                Canvas(bitmap).apply {
                    drawColor(if (index == 1) 0xff346f68.toInt() else 0xff3f587b.toInt())
                    drawCircle(48f, 42f, 25f, Paint().apply { color = 0xffb5d8c9.toInt() })
                    drawCircle(48f, 105f, 42f, Paint().apply { color = 0xff779e92.toInt() })
                }
                val poster = ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.JPEG, 85, it) }.toByteArray()
                bitmap.recycle()
                val id = UUID.randomUUID().toString()
                VoiceMessage(id, target.run, target.conversation, 1_791_019_000.0 + index * 60, 1, "submitted",
                    audio.displayed().copy(messageId = id, index = 0), 6000,
                    transcript = "Synthetic recording $index with recognized text.\nA second line stays exactly as recognized.", waveform = waveform,
                    video = VideoMessage(video.displayed().copy(messageId = id, index = 1), 6000, 256, 256,
                        Base64.getEncoder().encodeToString(poster)))
            }
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            try {
                val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
                await("Private storage ready") { main { model.storageReady && model.speech.ready } }
                val before = store.voiceState()
                val scheme = if (light) lightColorScheme(primary = androidx.compose.ui.graphics.Color(0xff086c58))
                    else Class.forName("app.zerus.mobile.MainActivityKt").getDeclaredField("Scheme")
                        .apply { isAccessible = true }.get(null) as ColorScheme
                main { activity.setContent {
                    MaterialTheme(colorScheme = scheme) {
                        val density = LocalDensity.current
                        CompositionLocalProvider(LocalDensity provides Density(density.density, if (narrow) 1.8f else 1f)) {
                            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                                Box(Modifier.fillMaxSize()) {
                                Column(Modifier.systemBarsPadding().width(if (narrow) 280.dp else 360.dp)
                                    .verticalScroll(rememberScrollState()).padding(12.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                    Text("Synthetic round video UI", style = MaterialTheme.typography.labelSmall)
                                    entries.forEachIndexed { index, voice ->
                                        Box(Modifier.fillMaxWidth().semantics { contentDescription = "Fixture message ${index + 1}" }) {
                                            ConversationMessage(target, Event(voice.id, "You", voice.transcript, "", at = voice.createdAt,
                                                voice = voice), "Fixture", null, model, {})
                                        }
                                    }
                                }
                                }
                            }
                        }
                    }
                } }
                node(1, "Play video message"); node(2, "Play video message")
                val measured = Rect().also(checkNotNull(group(1))::getBoundsInScreen)
                val widthDp = measured.width() / targetContext.resources.displayMetrics.density
                check(abs(widthDp - (if (narrow) 256f else 336f)) <= 2f) { "Wrong fixture viewport: $widthDp dp" }
                check(main { model.attachments.playingKey.isEmpty() && model.attachments.rows.isEmpty() })
                screenshot("collapsed")
                click(1, "Play video message")
                val videoKey = AttachmentIdentity.playbackKey(target, checkNotNull(entries[0].video).file)
                await("Exact first recording video") { main { model.attachments.playingKey == videoKey && model.attachments.playing } }
                click(1, "Show recognized text"); node(1, entries[0].transcript)
                check(find(entries[1].transcript, group(2)) == null)
                check(find("Show recognized text", group(1)) == null)
                node(1, "Play video message audio"); node(2, "Play video message")
                check(main { model.attachments.playingKey.isEmpty() && model.attachments.rows.isEmpty() })
                screenshot("first-expanded")
                click(1, "Hide recognized text"); node(1, "Play video message")
                click(2, "Show recognized text"); node(2, entries[1].transcript)
                check(find(entries[0].transcript, group(1)) == null)
                click(2, "Play video message audio")
                val key = AttachmentIdentity.playbackKey(target, entries[1].audio)
                await("Exact second recording audio") { main { model.attachments.playingKey == key && model.attachments.playing && model.attachments.audioPositionMs > 0 } }
                click(1, "Show recognized text")
                node(1, entries[0].transcript)
                check(main { model.attachments.playingKey == key && model.attachments.playing })
                click(1, "Hide recognized text")
                node(1, "Play video message")
                check(main { model.attachments.playingKey == key && model.attachments.playing })
                click(2, "Pause video message audio")
                await("Second audio paused") { main { !model.attachments.playing && model.attachments.playingKey == key } }
                val slider = node(2, "Recording playback position")
                check(slider.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.id,
                    Bundle().apply { putFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE, .4f) }))
                await("Exact paused seek") { main { model.attachments.audioPositionMs in 2200..2600 && !model.attachments.playing } }
                screenshot("second-expanded")
                click(2, "Hide recognized text")
                node(2, "Play video message")
                await("Second recording player retired") { main { model.attachments.playingKey.isEmpty() } }
                check(store.voiceState() == before && store.messageState() == MessageState())
                check(main { model.outgoing.isEmpty() && model.speech.activeRequests.isEmpty() })
            } catch (error: Throwable) {
                screenshot("failure")
                val lines = mutableListOf<String>()
                fun dump(node: AccessibilityNodeInfo?, depth: Int) {
                    if (node == null || depth > 30 || lines.size >= 256) return
                    lines.add("$depth text=${node.text} description=${node.contentDescription} clickable=${node.isClickable} " +
                        "visible=${node.isVisibleToUser} bounds=${Rect().also(node::getBoundsInScreen)}")
                    for (index in 0 until node.childCount) dump(node.getChild(index), depth + 1)
                }
                dump(uiAutomation.rootInActiveWindow, 0)
                File(targetContext.getExternalFilesDir(null), "round-failure-tree.txt").writeText(lines.joinToString("\n"))
                throw error
            } finally { main { activity.finish() } }
            result.putString("stream", "Round video layout passed: two independent compact circles/transcripts, icon-only disclosure, exact text,48dp controls, seekable same-message audio and other-message playback retained; no dispatch or persisted changes.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Round video layout failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
}
