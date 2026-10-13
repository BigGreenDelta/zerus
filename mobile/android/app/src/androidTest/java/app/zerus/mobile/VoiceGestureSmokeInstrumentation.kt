package app.zerus.mobile

import android.accessibilityservice.AccessibilityService
import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowInsets
import android.view.accessibility.AccessibilityNodeInfo
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
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual pointer dispatch, emulator AudioRecord, encrypted previews, and an in-process gateway.
 * Run only on a fresh isolated emulator with synthetic audio and no microphone permission grant.
 * This runner never clears data, revokes permissions, contacts a relay, or presses Send.
 */
class VoiceGestureSmokeInstrumentation : Instrumentation() {
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
            check(runBlocking { preStopped.capture(120_000) { progressCalls++ } }.isEmpty())
            check(progressCalls == 0 && !SpeechMicrophoneAccess.granted(targetContext))
            runGestures()
            result.putString("stream", "Voice gesture smoke passed: physical hold/release/left cancel, permission grant after release, editor/send/attachment locks, text/attachment action switching, ACTION_CANCEL, keyboard-stop race, navigation and background retention; no uploads or automatic sends.\n")
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
    private fun editor() = uiNode("composer editor") { it.className?.toString() == "android.widget.EditText" }
    private fun replaceText(value: String): Boolean = editor().performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
        putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, value)
    })
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private data class Press(val time: Long, val x: Float, val y: Float)
    private fun touch(press: Press, action: Int, x: Float = press.x, y: Float = press.y) {
        val event = MotionEvent.obtain(press.time, SystemClock.uptimeMillis(), action, x, y, 0)
        event.source = InputDevice.SOURCE_TOUCHSCREEN
        try { check(uiAutomation.injectInputEvent(event, true)) { "Pointer injection failed: $action" } }
        finally { event.recycle() }
    }
    private fun down(): Press {
        waitForIdleSync()
        val bounds = Rect().also { node("Voice input").getBoundsInScreen(it) }
        check(!bounds.isEmpty && bounds.width() >= 32 * targetContext.resources.displayMetrics.density)
        return Press(SystemClock.uptimeMillis(), bounds.exactCenterX(), bounds.exactCenterY()).also { touch(it, MotionEvent.ACTION_DOWN) }
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
            fun discard() {
                tap("Discard"); tap("Discard recording")
                await("explicit preview discard") { !model.speech.busy && model.speech.draft == null }
                check(PrivateStore(targetContext).voiceState().draft == null)
            }
            fun hold(): Press {
                main { activity.window.insetsController?.hide(WindowInsets.Type.ime()) }
                val press = down()
                await("held microphone samples") { model.speech.recording && model.speech.seconds >= 1 }
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

            val lockedEditor = editor()
            val held = hold()
            check(main { model.speech.loading }) { "Local capture waited for discovery" }
            val capture = main { model.speech.gestureCaptureId }
            touch(held, MotionEvent.ACTION_MOVE, held.x - 5)
            touch(held, MotionEvent.ACTION_MOVE, held.x + 5)
            check(main { model.speech.gestureCaptureId == capture }) { "A move started another capture" }
            waitForIdleSync()
            // Refresh the same editor semantics node: Compose may change its Android
            // class while read-only, but capture must preserve the input's identity.
            check(lockedEditor.refresh()) { "Recording replaced the composer editor" }
            check(lockedEditor.actionList.none { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT })
            check(!lockedEditor.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
                putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, "Must not enter the recording draft")
            }))
            var attach = node("Attach files")
            while (attach.isEnabled && !attach.isClickable) attach = checkNotNull(attach.parent)
            check(!attach.isEnabled) { "Recording left the attachment picker enabled" }
            var pickerOpened = false
            main { model.beginAttachmentPick(target) { pickerOpened = true } }
            runBlocking { main { model.sendMessage(target) }.join() }
            check(!pickerOpened && main { model.pickerDraft == null && !model.attachmentPreparing && model.draft(target).text.isEmpty() })
            screenshot("voice-gesture-holding.png")
            touch(held, MotionEvent.ACTION_UP)
            val released = preview("release retained preview")
            check(released.machineId.isEmpty() && released.profileId.isEmpty())
            profilesGate.countDown()
            await("discovery completed") { !model.speech.loading && model.speech.computers.size == 1 }
            check(main { model.speech.draft == released }) { "Late discovery changed the preview" }
            noDelivery()
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
            noDelivery()
        } finally {
            profilesGate.countDown()
            main { model.speech.stop(); activity.finish() }
        }
    }
}
