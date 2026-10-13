package app.zerus.mobile

import android.accessibilityservice.AccessibilityService
import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Base64
import android.view.InputDevice
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowInsets
import android.view.accessibility.AccessibilityNodeInfo
import android.view.inputmethod.EditorInfo
import androidx.lifecycle.Lifecycle
import kotlinx.coroutines.runBlocking
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.ByteArrayOutputStream
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual pointer dispatch, emulator AudioRecord, encrypted previews, and an in-process gateway.
 * Run only on a fresh isolated emulator with synthetic audio and no microphone permission grant.
 * This runner never clears data, revokes permissions or contacts a relay. Its one
 * final explicit Send is accepted only by the isolated in-process fixture gateway.
 */
class VoiceGestureSmokeInstrumentation : Instrumentation() {
    private val viewportNotes = mutableListOf<String>()
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            require(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish")) {
                "Use a debug build on an isolated Android emulator with synthetic audio."
            }
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.voiceState() == VoiceState()) {
                "Use fresh emulator app data; this runner does not reset existing state."
            }
            require(!SpeechMicrophoneAccess.granted(targetContext)) { "Start with microphone permission ungranted." }
            // Deterministic capture-start race: stopping before dispatch must return
            // without touching Android's microphone, even before permission exists.
            val preStopped = SpeechRecorder(targetContext).apply { stop() }
            var progressCalls = 0
            var startedCalls = 0
            check(runBlocking { preStopped.capture(120_000, started = { startedCalls++ }) { progressCalls++ } }.isEmpty())
            check(progressCalls == 0 && startedCalls == 0 && !SpeechMicrophoneAccess.granted(targetContext))
            probePlatformFeedback()
            runGestures()
            result.putString("stream", "Voice gesture smoke passed: physical hold/release/left cancel, compact inline timer/cancel cue, open-IME and expanded editor/selection restoration, hidden input rejection, permission/lifecycle/late-event ownership, accessibility and Enter; controller lifecycle plus platform View haptic dispatch verified without a physical vibration claim; compact Pending preview, accessible ordered playback/delete/send targets, seek and discard confirmation; zero automatic uploads and one final explicit Send with exact durable audio identity.\n")
            if (viewportNotes.isNotEmpty()) result.putString("stream", result.getString("stream") + viewportNotes.joinToString("\n", postfix = "\n"))
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Voice gesture smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }

    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }
        return value.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!main(condition)) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $description" }
            SystemClock.sleep(40)
        }
    }
    private fun find(predicate: (AccessibilityNodeInfo) -> Boolean, root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (predicate(root)) return root
        for (index in 0 until root.childCount) find(predicate, root.getChild(index))?.let { return it }
        return null
    }
    private fun named(label: String) = find({ it.text?.toString()?.replace('’', '\'') == label || it.contentDescription?.toString() == label })
    private fun node(label: String): AccessibilityNodeInfo = uiNode(label) {
        it.text?.toString()?.replace('’', '\'') == label || it.contentDescription?.toString() == label
    }
    private fun uiNode(description: String, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (true) {
            find(predicate)?.let { return it }
            check(SystemClock.elapsedRealtime() < deadline) { "UI element unavailable: $description" }
            SystemClock.sleep(40)
        }
    }
    private fun tap(label: String) {
        var value: AccessibilityNodeInfo? = node(label)
        while (value != null && !value.isClickable) value = value.parent
        check(value?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true) { "Cannot activate $label" }
    }
    private fun tapProviderField(label: String) {
        waitForIdleSync()
        node("Transcription provider"); node("Done")
        fun visibleField() = named(label)?.let { it.isVisibleToUser && !Rect().also(it::getBoundsInScreen).isEmpty } == true
        fun bodySnapshot(root: AccessibilityNodeInfo?): List<String> {
            if (root == null) return emptyList()
            root.refresh()
            val bounds = Rect().also(root::getBoundsInScreen)
            val own = if (root.isVisibleToUser && (root.text != null || root.contentDescription != null))
                listOf("${root.text}/${root.contentDescription}:$bounds") else emptyList()
            return own + (0 until root.childCount).flatMap { bodySnapshot(root.getChild(it)) }
        }
        repeat(16) {
            if (visibleField()) { tap(label); return }
            // Compose may scroll without TYPE_VIEW_SCROLLED. Actual newly visible
            // controls or changed text/bounds prove progress without that event.
            val scroller = find({ it.isVisibleToUser && it.isScrollable })
            check(scroller != null) { "Provider dialog has no scrollable body to reach $label" }
            val before = bodySnapshot(scroller)
            check(scroller.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)) { "Provider dialog cannot scroll to $label" }
            val deadline = SystemClock.elapsedRealtime() + 2_000
            var previous = before
            var stableSince = SystemClock.elapsedRealtime()
            var moved = false
            while (true) {
                waitForIdleSync()
                if (visibleField()) { tap(label); return }
                val current = bodySnapshot(find({ it.isVisibleToUser && it.isScrollable }))
                val now = SystemClock.elapsedRealtime()
                if (current != before) moved = true
                if (current != previous) { previous = current; stableSince = now }
                if (moved && now - stableSince >= 200) break
                check(now < deadline) { "Provider dialog did not visibly scroll toward $label" }
                SystemClock.sleep(25)
            }
        }
        error("Provider dialog did not reveal $label after bounded scrolling")
    }
    private fun editor() = uiNode("composer editor") { it.className?.toString() == "android.widget.EditText" }
    private fun replaceText(value: String): Boolean = editor().performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
        putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, value)
    })
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun probePlatformFeedback() = main {
        val calls = mutableListOf<Pair<Int, Int>>()
        var accepted = true
        val view = object : View(targetContext) {
            override fun performHapticFeedback(feedbackConstant: Int, flags: Int): Boolean {
                calls += feedbackConstant to flags
                return accepted
            }
        }
        for (event in VoiceCaptureFeedback.Event.entries) {
            check(VoiceHaptics.perform(view, event))
            check(calls.last() == (HapticFeedbackConstants.KEYBOARD_TAP to 0))
        }
        check(calls.size == 3)
        // Android's refusal is authoritative; the production dispatcher must not
        // retry with ignore-settings flags or a different vibration mechanism.
        accepted = false
        view.isHapticFeedbackEnabled = false
        check(!VoiceHaptics.perform(view, VoiceCaptureFeedback.Event.Start))
        check(calls == List(4) { HapticFeedbackConstants.KEYBOARD_TAP to 0 })
    }
    private fun assertNoEditableControl() {
        check(find({ it.isVisibleToUser && it.actionList.any { action -> action.id == AccessibilityNodeInfo.ACTION_SET_TEXT } }) == null) {
            "Recording exposed a visible text replacement action"
        }
    }
    private fun interactionBounds(label: String, root: AccessibilityNodeInfo?, seek: Boolean = false): Pair<Rect?, String> {
        val minimum = 48 * targetContext.resources.displayMetrics.density - 2
        val ancestry = mutableListOf<String>()
        var current = find({ it.text?.toString() == label || it.contentDescription?.toString() == label }, root)
        while (current != null) {
            val bounds = Rect().also(current::getBoundsInScreen)
            val seeks = current.rangeInfo != null && current.actionList.any { it.id == AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.id }
            ancestry += "${current.className}(description=${current.contentDescription}, bounds=$bounds, visible=${current.isVisibleToUser}, clickable=${current.isClickable}, enabled=${current.isEnabled}, range=${current.rangeInfo != null}, seeks=$seeks)"
            if (current.isVisibleToUser && (if (seek) seeks else current.isClickable) && bounds.width() >= minimum && bounds.height() >= minimum)
                return bounds to ancestry.joinToString(" -> ")
            current = current.parent?.also { it.refresh() }
        }
        return null to "$label: $ancestry"
    }
    private fun compactPreviewBounds(): Rect {
        waitForIdleSync()
        try { uiAutomation.waitForIdle(100, 1_000) } catch (_: java.util.concurrent.TimeoutException) { /* Bounded geometry polling below still applies. */ }
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var previous: List<Rect>? = null
        var stableSince = SystemClock.elapsedRealtime()
        var stableBounds: List<Rect>
        var window = Rect()
        while (true) {
            val root = uiAutomation.rootInActiveWindow
            val candidates = listOf(interactionBounds("Play", root), interactionBounds("Recording playback position", root, seek = true),
                interactionBounds("Discard", root), interactionBounds("Send voice message", root))
            val bounds = candidates.mapNotNull { it.first }
            val pendingVisible = find({ it.text?.toString() == "Pending" && it.isVisibleToUser }, root) != null
            val now = SystemClock.elapsedRealtime()
            if (bounds.size != 4 || !pendingVisible) {
                previous = null; stableSince = now
            } else {
                if (bounds != previous) { previous = bounds; stableSince = now }
                if (now - stableSince >= 200) {
                    stableBounds = bounds
                    window = Rect().also { root?.getBoundsInScreen(it) }
                    break
                }
            }
            check(now < deadline) { "Compact preview never exposed stable visible 48dp action/range owners; Pending=$pendingVisible; ${candidates.joinToString("; ") { it.second }}" }
            SystemClock.sleep(25)
        }
        val (play, seek, discard, send) = stableBounds
        val tolerance = (2 * targetContext.resources.displayMetrics.density).toInt()
        check(play.right <= seek.left + tolerance && seek.right <= discard.left + tolerance && discard.right <= send.left + tolerance) {
            "Compact preview action order/targets overlap: Play=$play seek=$seek Discard=$discard Send=$send"
        }
        check(listOf(seek, discard, send).all { kotlin.math.abs(it.centerY() - play.centerY()) <= tolerance }) {
            "Compact preview actions do not share one row"
        }
        check(stableBounds.all { window.contains(it) }) { "Compact preview actions extend outside the window" }
        check(find({ it.text?.toString()?.contains("Recording saved", ignoreCase = true) == true }) == null)
        return seek
    }
    private fun inlineRecordingBounds(atRest: Boolean = true): Pair<Rect, Rect> {
        val timer = uiNode("inline recording timer") { it.contentDescription?.toString()?.matches(Regex("Recording [0-9]+:[0-9]{2}(\\.[0-9])?")) == true }
        // The visible cue adapts to "Cancel" at large fonts, while its complete
        // instruction stays accessible. Screenshots qualify both visual variants.
        val hint = uiNode("complete slide cancellation instruction") {
            it.isVisibleToUser && it.contentDescription?.toString() == "Slide to cancel"
        }
        val timerBounds = Rect().also(timer::getBoundsInScreen)
        val hintBounds = Rect().also(hint::getBoundsInScreen)
        val actionBounds = Rect().also { node("Stop recording").getBoundsInScreen(it) }
        val tolerance = (2 * targetContext.resources.displayMetrics.density).toInt()
        check(!timerBounds.isEmpty && !hintBounds.isEmpty)
        check(timerBounds.top >= actionBounds.top - tolerance && timerBounds.bottom <= actionBounds.bottom + tolerance &&
            hintBounds.top >= actionBounds.top - tolerance && hintBounds.bottom <= actionBounds.bottom + tolerance) {
            "Recording feedback grew beyond the action row: timer=$timerBounds hint=$hintBounds action=$actionBounds"
        }
        check(!atRest || timerBounds.right <= hintBounds.left + tolerance && hintBounds.right <= actionBounds.left + tolerance) {
            "Inline timer, cancellation cue and microphone overlap"
        }
        check(find({ it.isVisibleToUser && it.className?.toString() == "android.widget.ProgressBar" }) == null) {
            "Gesture recording exposed a separate progress bar"
        }
        return timerBounds to hintBounds
    }
    private data class Press(val time: Long, val x: Float, val y: Float)
    private fun touch(press: Press, action: Int, x: Float = press.x, y: Float = press.y) {
        val event = MotionEvent.obtain(press.time, SystemClock.uptimeMillis(), action, x, y, 0)
        event.source = InputDevice.SOURCE_TOUCHSCREEN
        try { check(uiAutomation.injectInputEvent(event, true)) { "Pointer injection failed: $action" } }
        finally { event.recycle() }
    }
    private fun settledMicrophoneBounds(): Rect {
        waitForIdleSync()
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var previous = Rect()
        var stableSince = SystemClock.elapsedRealtime()
        while (true) {
            val bounds = Rect().also { node("Voice input").getBoundsInScreen(it) }
            val now = SystemClock.elapsedRealtime()
            if (bounds != previous) { previous = bounds; stableSince = now }
            if (!bounds.isEmpty && now - stableSince >= 200) return bounds
            check(now < deadline) { "Microphone bounds did not settle: $bounds" }
            SystemClock.sleep(25)
        }
    }
    private fun down(): Press {
        val bounds = settledMicrophoneBounds()
        check(bounds.width() >= 32 * targetContext.resources.displayMetrics.density)
        return Press(SystemClock.uptimeMillis(), bounds.exactCenterX(), bounds.exactCenterY()).also { touch(it, MotionEvent.ACTION_DOWN) }
    }
    private fun settledEditorBounds(): Rect {
        waitForIdleSync()
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var previous = Rect()
        var stableSince = SystemClock.elapsedRealtime()
        while (true) {
            val bounds = Rect().also { editor().getBoundsInScreen(it) }
            val now = SystemClock.elapsedRealtime()
            if (bounds != previous) { previous = bounds; stableSince = now }
            if (!bounds.isEmpty && now - stableSince >= 200) return bounds
            check(now < deadline) { "Editor bounds did not settle: $bounds" }
            SystemClock.sleep(25)
        }
    }
    private fun closedKeyboardEditorBounds(activity: Activity): Rect {
        // Confirmation dialogs restore editor focus asynchronously. A single
        // isVisible=false sample during that transition is not a closed viewport.
        await("conversation window focus after dialog") { activity.hasWindowFocus() }
        waitForIdleSync()
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var previous = Rect()
        var stableSince = SystemClock.elapsedRealtime()
        var lastHide = 0L
        while (true) {
            val now = SystemClock.elapsedRealtime()
            val (focused, visible, bottom) = main {
                val insets = activity.window.decorView.rootWindowInsets
                Triple(activity.hasWindowFocus(), insets?.isVisible(WindowInsets.Type.ime()),
                    insets?.getInsets(WindowInsets.Type.ime())?.bottom)
            }
            if (focused && (lastHide == 0L || visible != false || bottom != 0) && now - lastHide >= 250) {
                main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }
                lastHide = now
            }
            val bounds = Rect().also { editor().getBoundsInScreen(it) }
            val closed = focused && visible == false && bottom == 0
            if (!closed || bounds != previous) { previous = bounds; stableSince = now }
            if (closed && !bounds.isEmpty && now - stableSince >= 500) return bounds
            check(now < deadline) { "Keyboard/editor did not settle closed: focus=$focused visible=$visible inset=$bottom editor=$bounds" }
            SystemClock.sleep(25)
        }
    }
    private fun screenshot(name: String) {
        waitForIdleSync()
        val bitmap = checkNotNull(uiAutomation.takeScreenshot())
        try {
            File(targetContext.getExternalFilesDir(null), name).outputStream().use {
                check(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it))
            }
        } finally { bitmap.recycle() }
    }

    private fun runGestures() {
        val target = Target("session-host", "gesture-fixture", "fixture-run", "fixture-conversation", "gesture-fixture")
        val session = Session(target, "Gesture fixture", "codex", "idle", "", "", 0, JSONObject())
        val mutations = AtomicInteger()
        val submissions = AtomicInteger()
        val authorizedPreview = AtomicReference<VoiceDraft?>()
        val authorizedPcm = AtomicReference<ByteArray?>()
        val profiles = AtomicInteger()
        val profilesGate = CountDownLatch(1)
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            check(request.url.host == "relay.example.test")
            val body = okio.Buffer().also { request.body?.writeTo(it) }.readUtf8()
            val input = if (body.isBlank()) JSONObject() else JSONObject(body)
            val operation = input.string("operation")
            val result = when (operation) {
                "inspect" -> JSONObject().put("name", target.session).put("run_id", target.run)
                    .put("conversation_id", target.conversation).put("tracked", true).put("runtime_state", "live")
                    .put("process_state", "running").put("activity", "idle").put("phase", "idle")
                "voice_profiles" -> {
                    profiles.incrementAndGet()
                    check(profilesGate.await(90, TimeUnit.SECONDS)) { "Discovery fixture was not released" }
                    JSONObject().put("schema", 1).put("computers", JSONArray().put(JSONObject()
                        .put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4").put("name", "Fixture computer")
                        .put("profiles", JSONArray().put(JSONObject().put("id", "fixture").put("name", "Fixture profile")
                            .put("available", true).put("max_duration_ms", 120_000)))))
                }
                "voice_submit" -> {
                    // This endpoint is armed only immediately before the final
                    // explicit UI Send. Any earlier upload remains a hard failure.
                    mutations.incrementAndGet()
                    val captured = checkNotNull(authorizedPreview.get()) { "Voice uploaded without explicit fixture Send" }
                    check(submissions.incrementAndGet() == 1) { "Explicit Send uploaded more than once" }
                    val id = input.getString("request_id")
                    check(UUID.fromString(id).toString() == id)
                    val payload = input.getJSONObject("payload")
                    check(input.string("computer_id") == target.computerId && input.string("session") == target.session)
                    check(payload.string("request_id") == id && payload.string("expected_run_id") == target.run &&
                        payload.string("expected_conversation_id") == target.conversation)
                    val provider = payload.getJSONObject("speech")
                    check(provider.string("machine_id") == captured.machineId && provider.string("profile_id") == captured.profileId)
                    val audio = payload.getJSONObject("audio")
                    check(audio.string("format") == "pcm_s16le" && audio.getInt("sample_rate") == 16_000 && audio.getInt("channels") == 1)
                    check(Base64.decode(audio.getString("data_base64"), Base64.NO_WRAP).contentEquals(checkNotNull(authorizedPcm.get())))
                    val persisted = PrivateStore(targetContext).voiceState()
                    check(persisted.draft == null && persisted.outgoing.single().let { it.id == id && it.audio == captured.audio && it.target == captured.target })
                    JSONObject().put("schema", 1).put("message", JSONObject().put("id", id).put("run_id", target.run)
                        .put("conversation_id", target.conversation).put("created_at", captured.createdAt).put("revision", 1).put("state", "submitted")
                        .put("transcript", "Synthetic explicit voice message.")
                        .put("audio", JSONObject().put("request_id", id).put("index", 0).put("name", "voice.wav")
                            .put("mime", "audio/wav").put("bytes", captured.audio.bytes).put("duration_ms", captured.durationMs)))
                }
                else -> {
                    // No delivery/status/attachment request is necessary to record and retain locally.
                    mutations.incrementAndGet()
                    throw java.io.IOException("Unexpected fixture operation: $operation")
                }
            }
            val receipt = JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", result)
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        try {
            await("private storage") { model.storageReady }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                model.attachments.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model.attachments, RelayApi(client))
                set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                val operations = VoiceProtocol.operations + "attachment_read"
                set(model, "setRelayOperations", mapOf(target.connectionId to operations))
                set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to operations))
                set(model, "setSelected", session)
                model.refreshActivity()
            }
            await("fresh conversation") { model.speech.ready && model.activityVerified && model.voiceSendReason(target).isBlank() }
            main { model.speech.refreshProfiles(target) }
            await("held discovery") { profiles.get() > 0 && model.speech.loading }

            fun noDelivery() {
                check(mutations.get() == 0) { "Gesture uploaded audio or performed another remote action" }
                check(main { model.speech.outgoing.isEmpty() && model.outgoing.isEmpty() })
                check(PrivateStore(targetContext).voiceState().outgoing.isEmpty())
                check(PrivateStore(targetContext).messageState().outgoing.isEmpty())
            }
            fun preview(description: String): VoiceDraft {
                await(description) { !model.speech.busy && model.speech.draft != null }
                val draft = main { checkNotNull(model.speech.draft) }
                check(draft.target == target && draft.durationMs >= 1_000)
                check(PrivateStore(targetContext).voiceState().draft == draft) { "Preview was not durable" }
                check(runBlocking { AttachmentStore(targetContext).payload(listOf(draft.audio)) }.length() == 1)
                noDelivery()
                return draft
            }
            fun revealPreviewDiscard(allowEmptyViewport: Boolean = false): Boolean {
                waitForIdleSync()
                try { uiAutomation.waitForIdle(100, 1_000) } catch (_: java.util.concurrent.TimeoutException) { /* Await fresh owners below. */ }
                fun discardVisible() = interactionBounds("Discard", uiAutomation.rootInActiveWindow).first != null
                val deadline = SystemClock.elapsedRealtime() + 15_000
                var scrollOwner: AccessibilityNodeInfo?
                var viewport: Rect
                while (true) {
                    if (discardVisible()) return true
                    val root = uiAutomation.rootInActiveWindow
                    scrollOwner = find({ it.text?.toString() in setOf("Pending", "Voice message for ${target.session}") ||
                        it.contentDescription?.toString() in setOf("Discard", "Transcription provider") }, root)
                    while (scrollOwner != null && !scrollOwner.isScrollable) scrollOwner = scrollOwner.parent?.also { it.refresh() }
                    viewport = Rect().also { scrollOwner?.getBoundsInScreen(it) }
                    if (!viewport.isEmpty) break
                    if (SystemClock.elapsedRealtime() >= deadline) {
                        if (allowEmptyViewport) return false
                        error("Preview did not publish a visible Discard action or scroll viewport")
                    }
                    SystemClock.sleep(40)
                }
                val scroller = checkNotNull(scrollOwner)
                var attempts = 0
                while (!discardVisible() && attempts++ < 32) {
                    if (!scroller.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)) break
                    waitForIdleSync(); SystemClock.sleep(50)
                    scroller.refresh()
                }
                // The viewport can finish scrolling before Compose publishes its
                // replacement virtual action node. Keep the final owner check strict.
                val publicationDeadline = SystemClock.elapsedRealtime() + 1_000
                while (!discardVisible() && SystemClock.elapsedRealtime() < publicationDeadline) SystemClock.sleep(25)
                check(discardVisible()) { "Preview has viewport $viewport but Discard was unreachable after $attempts scroll attempts" }
                return true
            }
            fun discard() {
                check(revealPreviewDiscard())
                tap("Discard"); tap("Discard recording")
                await("explicit preview discard") { !model.speech.busy && model.speech.draft == null }
                check(PrivateStore(targetContext).voiceState().draft == null)
            }
            fun hold(hideKeyboard: Boolean = true): Press {
                if (hideKeyboard) {
                    main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }
                    await("keyboard dismissed before hold") { activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) != true }
                }
                val press = down()
                try {
                    await("held microphone samples") { model.speech.recording && model.speech.seconds >= 1 }
                } catch (error: IllegalStateException) {
                    val currentBounds = Rect().also { (named("Voice input") ?: named("Stop recording"))?.getBoundsInScreen(it) }
                    throw IllegalStateException(main {
                        "Hold at (${press.x},${press.y}) failed; currentMic=$currentBounds; recording=${model.speech.recording}, busy=${model.speech.busy}, capture=${model.speech.activeCaptureId}, preview=${model.speech.draft != null}, notice=${model.speech.notice}"
                    }, error)
                }
                check(main { model.speech.gestureCaptureId != null }) { "Physical press did not own its capture" }
                return press
            }

            // The permission dialog outlives the original press. Granting access is not a new press.
            val permissionPress = down()
            val allow = uiNode("system microphone permission") {
                it.text?.toString() in setOf("While using the app", "Allow only while using the app")
            }
            touch(permissionPress, MotionEvent.ACTION_UP)
            tap(checkNotNull(allow.text).toString())
            await("permission result") { SpeechMicrophoneAccess.granted(targetContext) && activity.lifecycle.currentState == Lifecycle.State.RESUMED }
            node("Voice input"); waitForIdleSync()
            // Give the activity-result callback its main-loop turn, then require an explicit new press.
            SystemClock.sleep(300)
            check(main { !model.speech.busy && model.speech.draft == null && model.draft(target).text.isEmpty() })
            noDelivery()

            val lockedEditor = main { checkNotNull(model.speechComposer).editor }
            val editorSelection = main { lockedEditor.selection }
            val held = hold()
            check(main { model.speech.loading }) { "Local capture waited for discovery" }
            val capture = main { model.speech.gestureCaptureId }
            touch(held, MotionEvent.ACTION_MOVE, held.x - 5)
            touch(held, MotionEvent.ACTION_MOVE, held.x + 5)
            check(main { model.speech.gestureCaptureId == capture }) { "A move started another capture" }
            waitForIdleSync()
            // The inline row hides the editor, while its exact state and selection
            // remain mounted for restoration. Hidden semantics need not survive.
            check(main { model.speechComposer?.editor === lockedEditor && lockedEditor.text.isEmpty() && lockedEditor.selection == editorSelection })
            assertNoEditableControl()
            val (_, hintBeforeSlide) = inlineRecordingBounds()
            screenshot("voice-gesture-holding.png")
            touch(held, MotionEvent.ACTION_MOVE, held.x - 40 * targetContext.resources.displayMetrics.density)
            await("partial cancellation feedback") { model.speech.gestureCancelProgress > 0.3f && model.speech.gestureCancelProgress < 0.8f }
            val (_, hintDuringSlide) = inlineRecordingBounds(atRest = false)
            check(hintDuringSlide.left <= hintBeforeSlide.left) { "Leftward cancellation cue moved right" }
            screenshot("voice-gesture-sliding.png")
            touch(held, MotionEvent.ACTION_MOVE)
            check(main { model.speech.recording }) {
                main { "Capture stopped during locked-editor checks: busy=${model.speech.busy}, gesture=${model.speech.gestureCaptureId}, preview=${model.speech.draft != null}, notice=${model.speech.notice}" }
            }
            // Compose can expose the icon's click semantics separately from the
            // disabled IconButton. Follow the entire ownership chain, not merely
            // the first node with isClickable, to locate the disabled control.
            var attach: AccessibilityNodeInfo? = named("Attach files")?.takeIf { it.isVisibleToUser }
            val attachmentVisible = attach != null
            val attachAncestry = mutableListOf<String>()
            while (attach != null && attach.isEnabled) {
                attachAncestry += "${attach.className}(description=${attach.contentDescription}, clickable=${attach.isClickable}, enabled=${attach.isEnabled})"
                attach = attach.parent?.also { it.refresh() }
            }
            check(!attachmentVisible || attach != null) {
                main { "Recording left the attachment picker enabled: $attachAncestry; recording=${model.speech.recording}, busy=${model.speech.busy}, gesture=${model.speech.gestureCaptureId}" }
            }
            if (attach != null) check(!attach.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Disabled attachment control accepted activation" }
            var pickerOpened = false
            main { model.beginAttachmentPick(target) { pickerOpened = true } }
            runBlocking { main { model.sendMessage(target) }.join() }
            check(!pickerOpened && main { model.pickerDraft == null && !model.attachmentPreparing && model.draft(target).text.isEmpty() })
            touch(held, MotionEvent.ACTION_UP)
            val released = preview("release retained preview")
            check(main { model.speechComposer?.editor === lockedEditor && lockedEditor.text.isEmpty() && lockedEditor.selection == editorSelection })
            check(released.machineId.isEmpty() && released.profileId.isEmpty())
            profilesGate.countDown()
            await("discovery completed") { !model.speech.loading && model.speech.computers.size == 1 }
            check(main { model.speech.draft == released }) { "Late discovery changed the preview" }
            noDelivery()
            compactPreviewBounds()
            node("Transcription provider")
            screenshot("voice-gesture-preview.png")

            // The editor and trailing action recover without a second microphone gesture.
            check(replaceText("Restored typed draft."))
            await("restored editing") { model.draft(target).text == "Restored typed draft." }
            node("Send message"); check(named("Voice input") == null)
            check(replaceText(""))
            await("cleared fixture text") { model.draft(target).text.isEmpty() }
            node("Voice input")
            main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }
            discard()
            val attachment = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(ByteArray(6_400))) }
            main {
                model.javaClass.getDeclaredMethod("save", Draft::class.java).apply { isAccessible = true }
                    .invoke(model, model.draft(target).copy(attachments = listOf(attachment)))
            }
            node("Send message"); check(named("Voice input") == null)
            tap("Remove ${attachment.name}")
            await("attachment removed explicitly") { model.draft(target).attachments.isEmpty() }
            node("Voice input")

            // Reproduce the owner's keyboard-open case with an actual field tap.
            // Recording must not dismiss the IME or move the held microphone.
            val inputBounds = Rect().also { editor().getBoundsInScreen(it) }
            val focusPress = Press(SystemClock.uptimeMillis(), inputBounds.exactCenterX(), inputBounds.exactCenterY())
            touch(focusPress, MotionEvent.ACTION_DOWN); touch(focusPress, MotionEvent.ACTION_UP)
            await("real software keyboard") { activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true }
            val imeEditor = main { checkNotNull(model.speechComposer).editor }
            val imeSelection = main { imeEditor.selection }
            val micBeforeImeHold = settledMicrophoneBounds()
            val withKeyboard = hold(hideKeyboard = false)
            check(main { activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true }) { "Recording dismissed the open software keyboard" }
            check(main { model.speechComposer?.editor === imeEditor && imeEditor.selection == imeSelection && imeEditor.text.isEmpty() })
            val micDuringImeHold = Rect().also { node("Stop recording").getBoundsInScreen(it) }
            val positionTolerance = 2 * targetContext.resources.displayMetrics.density
            check(kotlin.math.abs(micDuringImeHold.exactCenterX() - micBeforeImeHold.exactCenterX()) <= positionTolerance &&
                kotlin.math.abs(micDuringImeHold.exactCenterY() - micBeforeImeHold.exactCenterY()) <= positionTolerance) {
                "Held microphone moved with keyboard open: $micBeforeImeHold -> $micDuringImeHold"
            }
            val (imeTimer, imeHint) = inlineRecordingBounds()
            val keyboardTop = main {
                val view = activity.window.decorView
                val location = IntArray(2).also(view::getLocationOnScreen)
                location[1] + view.height - checkNotNull(view.rootWindowInsets).getInsets(WindowInsets.Type.ime()).bottom
            }
            check(imeTimer.bottom <= keyboardTop && imeHint.bottom <= keyboardTop && micDuringImeHold.bottom <= keyboardTop) { "Recording row was covered by the IME" }
            assertNoEditableControl()
            main {
                activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A))
                activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A))
                // Exercise the retained platform input connection like an IME,
                // without a second touch that would intentionally end this hold.
                val connection = checkNotNull(activity.window.decorView.findFocus()?.onCreateInputConnection(EditorInfo())) {
                    "Recording lost the focused editor's input connection"
                }
                connection.setComposingText("Blocked composition", 1)
                connection.commitText("Blocked IME text", 1)
                connection.finishComposingText()
            }
            waitForIdleSync()
            check(main { model.speech.recording && model.draft(target).text.isEmpty() && imeEditor.text.isEmpty() && imeEditor.selection == imeSelection }) {
                "Recording accepted hardware or IME text into its concealed editor"
            }
            screenshot("voice-gesture-keyboard.png")
            touch(withKeyboard, MotionEvent.ACTION_UP)
            preview("keyboard-open retained preview")
            check(main { model.speechComposer?.editor === imeEditor && imeEditor.selection == imeSelection })
            discard()
            main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }

            val cancelled = hold()
            val left = cancelled.x - 120 * targetContext.resources.displayMetrics.density
            check(left > 0) { "Mic was not in the trailing action slot" }
            touch(cancelled, MotionEvent.ACTION_MOVE, left)
            await("left swipe cancelled capture") { !model.speech.busy }
            check(main { model.speech.draft == null })
            touch(cancelled, MotionEvent.ACTION_MOVE) // Crossing back cannot revive a cancelled press.
            touch(cancelled, MotionEvent.ACTION_UP)
            waitForIdleSync()
            check(main { !model.speech.busy && model.speech.draft == null && model.draft(target).text.isEmpty() })
            check(PrivateStore(targetContext).voiceState().draft == null)
            noDelivery()

            // Real fast release can produce <100ms on a fast emulator or a short
            // valid preview under load. Both must finish visibly and permit retry.
            val brief = down()
            touch(brief, MotionEvent.ACTION_UP)
            await("fast press terminal state") { !model.speech.busy && (model.speech.draft != null || model.speech.captureFailure != null) }
            if (main { model.speech.draft != null }) {
                check(PrivateStore(targetContext).voiceState().draft == main { model.speech.draft })
                discard()
            } else {
                node("Recording not saved")
                check(main { model.speech.captureFailure?.second?.isNotBlank() == true })
                tap("Done")
                node("Voice input")
                check(main { !model.speech.busy && model.speech.draft == null })
            }
            noDelivery()

            val interrupted = hold()
            touch(interrupted, MotionEvent.ACTION_CANCEL)
            preview("system pointer cancellation retained preview")
            discard()

            val stopped = hold()
            main {
                check(activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_SPACE)))
                check(activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_SPACE)))
            }
            val manual = preview("manual stop retained preview")
            touch(stopped, MotionEvent.ACTION_MOVE, stopped.x - 120 * targetContext.resources.displayMetrics.density)
            touch(stopped, MotionEvent.ACTION_UP)
            waitForIdleSync()
            check(main { model.speech.draft == manual && !model.speech.busy }) { "Late swipe discarded a manually stopped preview" }
            discard()

            val background = hold()
            check(uiAutomation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_HOME))
            await("activity background") { !activity.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED) }
            val retained = preview("background retained preview")
            main { targetContext.startActivity(Intent(targetContext, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)) }
            await("activity resumed") { activity.lifecycle.currentState == Lifecycle.State.RESUMED }
            node("Voice input")
            touch(background, MotionEvent.ACTION_UP)
            waitForIdleSync()
            check(main { model.speech.draft == retained && !model.speech.busy })
            discard()

            val navigation = hold()
            main { model.back() }
            val pinned = preview("navigation retained preview")
            touch(navigation, MotionEvent.ACTION_UP)
            main { set(model, "setSelected", session); model.refreshActivity() }
            await("returned to original conversation") { model.activityVerified && model.speechComposer != null }
            node("Voice input")
            check(main { model.speech.draft == pinned && !model.speech.busy && model.draft(target).text.isEmpty() })
            discard()

            // Accessibility activation is explicit start/stop, not an invented hold.
            val accessibleEditor = main { checkNotNull(model.speechComposer).editor }
            tap("Voice input")
            await("accessible recording") { model.speech.recording && model.speech.seconds >= 1 }
            check(main { model.speech.gestureCaptureId == null && model.speech.activeCaptureId != null })
            waitForIdleSync()
            check(main { model.speechComposer?.editor === accessibleEditor && accessibleEditor.text.isEmpty() })
            assertNoEditableControl()
            check(uiNode("accessible stop action") { it.contentDescription?.toString() == "Stop recording" }
                .performAction(AccessibilityNodeInfo.ACTION_CLICK))
            preview("accessible stop retained preview")
            discard()

            // Focused hardware activation travels through Android's event dispatcher.
            // Holding Enter must neither start early nor toggle the new recording off.
            check(node("Voice input").performAction(AccessibilityNodeInfo.ACTION_FOCUS))
            uiNode("focused voice action") { it.contentDescription?.toString() == "Voice input" && it.isFocused }
            val keyboardEditor = main { checkNotNull(model.speechComposer).editor }
            fun enter(action: Int, downTime: Long, repeat: Int = 0) {
                val event = KeyEvent(downTime, SystemClock.uptimeMillis(), action, KeyEvent.KEYCODE_ENTER,
                    repeat, 0, android.view.KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD)
                check(uiAutomation.injectInputEvent(event, true))
            }
            val startKey = SystemClock.uptimeMillis()
            enter(KeyEvent.ACTION_DOWN, startKey)
            enter(KeyEvent.ACTION_DOWN, startKey, repeat = 1)
            check(main { !model.speech.busy })
            enter(KeyEvent.ACTION_UP, startKey)
            await("focused Enter recording") { model.speech.recording && model.speech.seconds >= 1 }
            val keyboardCapture = main { model.speech.activeCaptureId }
            check(keyboardCapture != null && main { model.speech.gestureCaptureId == null })
            waitForIdleSync()
            check(main { model.speechComposer?.editor === keyboardEditor && keyboardEditor.text.isEmpty() })
            assertNoEditableControl()
            val stopKey = SystemClock.uptimeMillis()
            enter(KeyEvent.ACTION_DOWN, stopKey)
            enter(KeyEvent.ACTION_DOWN, stopKey, repeat = 1)
            check(main { model.speech.recording && model.speech.activeCaptureId == keyboardCapture })
            enter(KeyEvent.ACTION_UP, stopKey)
            preview("focused Enter retained preview")
            discard()
            check(main { model.draft(target).text.isEmpty() && model.draft(target).attachments.isEmpty() })
            noDelivery()

            // Observe the production capture callback and platform View dispatcher,
            // without pretending an emulator can confirm physical vibration.
            val feedbackEvents = mutableListOf<VoiceCaptureFeedback.Event>()
            val feedbackCalls = mutableListOf<Pair<Int, Int>>()
            val feedbackView = main { object : View(targetContext) {
                override fun performHapticFeedback(feedbackConstant: Int, flags: Int): Boolean {
                    check(android.os.Looper.myLooper() == android.os.Looper.getMainLooper())
                    feedbackCalls += feedbackConstant to flags
                    return true
                }
            } }
            val feedback: (VoiceCaptureFeedback.Event) -> Unit = { event ->
                check(android.os.Looper.myLooper() == android.os.Looper.getMainLooper())
                feedbackEvents += event
                VoiceHaptics.perform(feedbackView, event)
            }
            fun expectFeedback(vararg expected: VoiceCaptureFeedback.Event) {
                check(main { feedbackEvents == expected.toList() && feedbackCalls == List(expected.size) { HapticFeedbackConstants.KEYBOARD_TAP to 0 } }) {
                    main { "Unexpected capture feedback: events=$feedbackEvents calls=$feedbackCalls" }
                }
            }
            fun resetFeedback() = main { feedbackEvents.clear(); feedbackCalls.clear() }

            // Main stays occupied through stop, so an AudioRecord-ready callback
            // cannot win this deterministic release-before-ready race.
            main {
                val id = checkNotNull(model.speech.startGesture(target, feedback))
                check(model.speech.finishGesture(id, cancel = false))
            }
            await("release before microphone ready") { !model.speech.busy }
            check(main { model.speech.draft == null })
            expectFeedback()

            val acceptedId = main { checkNotNull(model.speech.startGesture(target, feedback)) }
            await("accepted capture feedback") { model.speech.seconds >= 1 && feedbackEvents == listOf(VoiceCaptureFeedback.Event.Start) }
            expectFeedback(VoiceCaptureFeedback.Event.Start)
            main {
                // A rejected second start and repeated motion have no feedback.
                check(model.speech.startGesture(target, feedback) == null)
                model.speech.moveGesture(acceptedId, 0.4f)
                model.speech.moveGesture(acceptedId, 0.6f)
                check(model.speech.finishGesture(acceptedId, cancel = false))
                check(!model.speech.finishGesture(acceptedId, cancel = true))
                check(feedbackEvents == listOf(VoiceCaptureFeedback.Event.Start) && feedbackCalls.size == 1) {
                    "Finish feedback ran before the recorder could return and release its hardware"
                }
            }
            preview("feedback capture retained preview")
            expectFeedback(VoiceCaptureFeedback.Event.Start, VoiceCaptureFeedback.Event.Finish)
            discard()
            resetFeedback()

            val cancelledId = main { checkNotNull(model.speech.startGesture(target, feedback)) }
            await("cancellable capture feedback") { model.speech.seconds >= 1 && feedbackEvents == listOf(VoiceCaptureFeedback.Event.Start) }
            main {
                check(!model.speech.finishGesture(acceptedId, cancel = true)) // Old ownership cannot pulse for this capture.
                check(model.speech.finishGesture(cancelledId, cancel = true))
                check(!model.speech.finishGesture(cancelledId, cancel = false))
                check(feedbackEvents == listOf(VoiceCaptureFeedback.Event.Start) && feedbackCalls.size == 1) {
                    "Cancel feedback ran before the recorder could return and release its hardware"
                }
            }
            await("feedback cancellation finished") { !model.speech.busy }
            check(main { model.speech.draft == null })
            expectFeedback(VoiceCaptureFeedback.Event.Start, VoiceCaptureFeedback.Event.Cancel)
            resetFeedback()

            val backgroundId = main { checkNotNull(model.speech.startGesture(target, feedback)) }
            await("background capture start feedback") { model.speech.seconds >= 1 && feedbackEvents == listOf(VoiceCaptureFeedback.Event.Start) }
            main { model.speech.background() }
            preview("silent background finish")
            main { check(!model.speech.finishGesture(backgroundId, cancel = true)) }
            expectFeedback(VoiceCaptureFeedback.Event.Start)
            discard()
            noDelivery()

            // With no held mic pointer, touch the concealed editor area itself.
            // It must not create a new IME session or steal the active capture.
            main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }
            await("closed keyboard before row tap") { activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) != true }
            val rowTapEditor = main { checkNotNull(model.speechComposer).editor }
            val rowTapSelection = main { rowTapEditor.selection }
            val rowTapId = main { checkNotNull(model.speech.startGesture(target)) }
            await("row tap recording") { model.speech.recording && model.speech.seconds >= 1 }
            val (_, rowTapBounds) = inlineRecordingBounds()
            val rowPress = Press(SystemClock.uptimeMillis(), rowTapBounds.exactCenterX(), rowTapBounds.exactCenterY())
            touch(rowPress, MotionEvent.ACTION_DOWN); touch(rowPress, MotionEvent.ACTION_UP)
            waitForIdleSync(); SystemClock.sleep(300)
            check(main { model.speech.activeCaptureId == rowTapId && model.speech.recording &&
                activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) != true &&
                model.speechComposer?.editor === rowTapEditor && rowTapEditor.text.isEmpty() && rowTapEditor.selection == rowTapSelection }) {
                "Tapping concealed input opened the keyboard, changed the editor, or ended capture"
            }
            main { model.speech.finishGesture(rowTapId, cancel = false) }
            preview("row tap retained preview")
            discard()

            // A nonempty editor uses Send, so start through the controller to
            // qualify overlay restoration without exposing a forbidden mic action.
            val expandedText = (1..6).joinToString("\n") { "Expanded fixture line $it" }
            check(replaceText(expandedText))
            await("expanded fixture draft") { model.draft(target).text == expandedText }
            tap("Expand message")
            node("Collapse message")
            fun focusExpandedEditor() {
                // This is setup for the next capture, after the previous restored
                // selection was asserted. Collapse it before tapping so the native
                // selection toolbar/handles cannot intercept the refocus gesture.
                check(editor().performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, Bundle().apply {
                    putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, 0)
                    putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, 0)
                }))
                await("collapsed selection before fixture refocus") {
                    model.speechComposer?.editor?.selection?.let { it.start == 0 && it.end == 0 } == true
                }
                val bounds = settledEditorBounds()
                val inset = 24 * targetContext.resources.displayMetrics.density
                val press = Press(SystemClock.uptimeMillis(), bounds.left + minOf(inset, bounds.width() / 4f),
                    bounds.top + minOf(inset, bounds.height() / 4f))
                touch(press, MotionEvent.ACTION_DOWN); touch(press, MotionEvent.ACTION_UP)
                await("expanded fixture input connection") {
                    activity.window.decorView.findFocus()?.onCreateInputConnection(EditorInfo()) != null
                }
                // The fixture previously hid IME without removing editor focus.
                // Refocusing can move its caret without issuing another show;
                // explicitly establish this cycle's open-keyboard precondition.
                main { activity.window.insetsController?.show(WindowInsets.Type.ime()) }
                await("focused expanded editor IME") { activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true }
                check(main { activity.window.decorView.findFocus()?.onCreateInputConnection(EditorInfo()) != null }) {
                    "Fixture did not focus the expanded text input before recording"
                }
            }
            focusExpandedEditor()
            check(editor().performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, Bundle().apply {
                putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, 3)
                putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, 14)
            }))
            val expandedEditor = main { checkNotNull(model.speechComposer).editor }
            await("fixture selection") { expandedEditor.selection.start == 3 && expandedEditor.selection.end == 14 }
            val expandedSelection = main { expandedEditor.selection }
            check(named("Voice input") == null)
            val expandedBounds = closedKeyboardEditorBounds(activity)
            val clipboard = targetContext.getSystemService(ClipboardManager::class.java)
            val originalClip = main { clipboard.primaryClip }
            main { clipboard.setPrimaryClip(ClipData.newPlainText("Voice input fixture", "Blocked paste")) }
            try {
                for (cancel in listOf(false, true)) {
                    // Open a real IME session for each input-rejection cycle. Size
                    // comparisons use the same closed-IME viewport on either side.
                    focusExpandedEditor()
                    check(editor().performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, Bundle().apply {
                        putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, expandedSelection.start)
                        putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, expandedSelection.end)
                    }))
                    await("refocused expanded selection") { expandedEditor.selection == expandedSelection }
                    val expandedCapture = main { checkNotNull(model.speech.startGesture(target)) }
                    await("expanded editor recording") { model.speech.recording && model.speech.seconds >= 1 }
                    assertNoEditableControl()
                    main {
                        val connection = checkNotNull(activity.window.decorView.findFocus()?.onCreateInputConnection(EditorInfo()))
                        connection.setComposingText("Blocked composition", 1)
                        connection.commitText("Blocked IME text", 1)
                        connection.setSelection(0, expandedText.length)
                        connection.performContextMenuAction(android.R.id.paste)
                        connection.finishComposingText()
                        activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A))
                        activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A))
                        val now = SystemClock.uptimeMillis()
                        activity.dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_V, 0, KeyEvent.META_CTRL_ON))
                        activity.dispatchKeyEvent(KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_V, 0, KeyEvent.META_CTRL_ON))
                    }
                    waitForIdleSync()
                    check(main { model.speechComposer?.editor === expandedEditor && expandedEditor.text.toString() == expandedText &&
                        expandedEditor.selection == expandedSelection && model.draft(target).text == expandedText }) {
                        "Concealed expanded editor accepted input, paste or selection changes"
                    }
                    main { check(model.speech.finishGesture(expandedCapture, cancel = cancel)) }
                    if (cancel) {
                        await("expanded capture cancelled") { !model.speech.busy }
                        check(main { model.speech.draft == null })
                    } else {
                        preview("expanded editor retained preview")
                    }
                    check(main { model.speechComposer?.editor === expandedEditor && expandedEditor.text.toString() == expandedText &&
                        expandedEditor.selection == expandedSelection && activity.window.decorView.rootWindowInsets?.isVisible(WindowInsets.Type.ime()) == true }) {
                        "Capture ending changed the expanded editor, selection or open IME"
                    }
                    node("Collapse message")
                    if (!cancel) {
                        settledEditorBounds()
                        screenshot("voice-gesture-expanded-preview-ime.png")
                        if (revealPreviewDiscard(allowEmptyViewport = true)) discard()
                        else {
                            screenshot("voice-gesture-expanded-preview-clipped.png")
                            viewportNotes += "Expanded editor with IME: preview controls had no exposed scroll viewport; explicitly closed the fixture keyboard to reach Discard (screenshot retained)."
                        }
                    }
                    // Compare restored dimensions with the same viewport as the
                    // baseline. When the IME leaves no preview viewport, closing it
                    // is an explicit fixture action, reported above rather than hidden.
                    closedKeyboardEditorBounds(activity)
                    if (!cancel && main { model.speech.draft != null }) discard()
                    // Discard's dialog can reopen the editor IME as window focus
                    // returns. Close it after that transition before comparing.
                    val restoredBounds = closedKeyboardEditorBounds(activity)
                    node("Collapse message")
                    check(main { model.speechComposer?.editor === expandedEditor && expandedEditor.text.toString() == expandedText && expandedEditor.selection == expandedSelection }) {
                        "Recording reset the expanded editor or its selection"
                    }
                    check(kotlin.math.abs(restoredBounds.width() - expandedBounds.width()) <= positionTolerance &&
                        kotlin.math.abs(restoredBounds.height() - expandedBounds.height()) <= positionTolerance) {
                        "Recording reset the expanded editor size: $expandedBounds -> $restoredBounds"
                    }
                    screenshot(if (cancel) "voice-gesture-expanded-cancelled.png" else "voice-gesture-expanded-restored.png")
                    noDelivery()
                }
            } finally {
                main {
                    if (originalClip != null) clipboard.setPrimaryClip(originalClip)
                    else if (Build.VERSION.SDK_INT >= 28) clipboard.clearPrimaryClip()
                }
            }
            tap("Collapse message")
            check(replaceText(""))
            await("empty restored fixture") { model.draft(target).text.isEmpty() }
            noDelivery()

            // Keep all gesture paths above strictly upload-free. Only this final
            // explicit Send arms the fixture gateway, with the exact retained PCM.
            val sendPress = hold()
            await("seekable compact preview capture") { model.speech.seconds >= 3 }
            touch(sendPress, MotionEvent.ACTION_UP)
            val pending = preview("compact pending send fixture")
            check(revealPreviewDiscard())
            compactPreviewBounds()
            tap("Transcription provider")
            await("compact preview provider discovery") { !model.speech.loading }
            tapProviderField("Choose voice computer"); tap("Fixture computer")
            tapProviderField("Choose voice profile"); tap("Fixture profile"); tap("Done")
            await("compact preview exact selected route") {
                model.speech.draft?.machineId == "b4075bf0-9c22-4a67-b768-55af2d2936a4" && model.speech.draft?.profileId == "fixture" &&
                    model.speech.sendReason().isBlank()
            }
            val ready = main { checkNotNull(model.speech.draft) }
            check(ready.audio == pending.audio && ready.target == pending.target)
            await("durable selected compact preview") { PrivateStore(targetContext).voiceState().draft == ready }
            closedKeyboardEditorBounds(activity)
            compactPreviewBounds()
            check(named("Transcription provider") == null) { "Healthy preview exposes routine provider settings" }
            tap("Discard"); node("Discard recording?")
            check(main { model.speech.draft == ready } && PrivateStore(targetContext).voiceState().draft == ready)
            tap("Keep recording")
            closedKeyboardEditorBounds(activity)
            check(main { model.speech.draft == ready }) { "Keeping the recording changed its identity" }
            val seek = compactPreviewBounds()
            val position = node("Recording playback position")
            check(position.rangeInfo != null && position.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.id,
                Bundle().apply { putFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE, .25f) }))
            await("compact preview accessible paused seek") {
                !model.attachments.playing && model.attachments.audioPositionMs in (ready.durationMs * .20).toInt()..(ready.durationMs * .30).toInt()
            }
            val seekPress = Press(SystemClock.uptimeMillis(), seek.left + seek.width() * .6f, seek.exactCenterY())
            touch(seekPress, MotionEvent.ACTION_DOWN); touch(seekPress, MotionEvent.ACTION_UP)
            await("compact preview physical seek") {
                !model.attachments.playing && model.attachments.audioPositionMs in (ready.durationMs * .4).toInt()..(ready.durationMs * .8).toInt()
            }
            tap("Play"); await("compact preview playback") { model.attachments.playing }
            tap("Pause"); await("compact preview paused") { !model.attachments.playing }
            check(main { model.speech.draft == ready })
            compactPreviewBounds()
            screenshot("voice-preview-ready.png")
            noDelivery()
            check(main { !model.voiceColdCache() && model.speech.sendReason().isBlank() }) {
                "Synthetic Send fixture unexpectedly needs additional consent or recovery"
            }
            val wav = ByteArrayOutputStream()
            runBlocking { AttachmentStore(targetContext).decrypt(ready.audio) { wav.write(it) } }
            authorizedPcm.set(VoiceProtocol.pcm(wav.toByteArray()))
            authorizedPreview.set(ready)
            tap("Send voice message")
            await("one explicit compact Send") {
                submissions.get() == 1 && model.speech.draft == null && model.speech.outgoing.singleOrNull()?.state == "submitted" && model.speech.activeRequests.isEmpty()
            }
            check(mutations.get() == 1 && main { model.outgoing.isEmpty() && model.draft(target).text.isEmpty() })
            check(PrivateStore(targetContext).voiceState().outgoing.single().audio == ready.audio)
        } catch (error: Throwable) {
            runCatching { screenshot("voice-gesture-failure.png") }
            throw error
        } finally {
            profilesGate.countDown()
            main { model.speech.stop(); activity.finish() }
        }
    }
}
