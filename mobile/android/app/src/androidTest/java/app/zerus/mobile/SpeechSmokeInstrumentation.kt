package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.content.ActivityNotFoundException
import android.content.ContextWrapper
import android.os.Bundle
import android.os.SystemClock
import android.provider.Settings
import android.util.Base64
import android.view.accessibility.AccessibilityNodeInfo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import java.util.UUID
import java.io.FileInputStream

/** Explicit emulator-only smoke: real microphone and encrypted drafts, synthetic in-process gateway. */
class SpeechSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            require(BuildConfig.DEBUG) { "Run the speech smoke against the debug build." }
            runSmoke()
            result.putString("stream", "Speech smoke passed: denied microphone settings recovery, explicit retry, metadata-only ack, unmirrored IME edits, exact draft recovery, cancellation, background stop, encrypted persistence.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Speech smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!main(condition)) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $description" }
            SystemClock.sleep(50)
        }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun node(label: String): AccessibilityNodeInfo {
        fun find(root: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (root == null) return null
            if (root.text?.toString()?.replace('’', '\'') == label || root.contentDescription?.toString() == label) return root
            for (index in 0 until root.childCount) find(root.getChild(index))?.let { return it }
            return null
        }
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (true) {
            find(uiAutomation.rootInActiveWindow)?.let { return it }
            check(SystemClock.elapsedRealtime() < deadline) { "UI element unavailable: $label" }
            SystemClock.sleep(50)
        }
    }
    private fun tap(label: String) {
        var target = node(label)
        while (!target.isClickable) target = checkNotNull(target.parent) { "UI element is not clickable: $label" }
        check(target.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Could not click: $label" }
    }
    private fun back() {
        SystemClock.sleep(300) // Let the newly opened system page finish its transition.
        uiAutomation.executeShellCommand("input keyevent KEYCODE_BACK").use { FileInputStream(it.fileDescriptor).readBytes() }
        SystemClock.sleep(300)
    }
    private fun permissionRecovery(model: ZerusViewModel, target: Target) {
        var settingsIntent: Intent? = null
        check(SpeechMicrophoneAccess.openSettings(object : ContextWrapper(targetContext) {
            override fun startActivity(intent: Intent) { settingsIntent = intent }
        }))
        check(settingsIntent?.action == Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
        check(settingsIntent?.dataString == "package:app.zerus.mobile")
        check(!SpeechMicrophoneAccess.openSettings(object : ContextWrapper(targetContext) {
            override fun startActivity(intent: Intent) { throw ActivityNotFoundException("Synthetic unavailable system page") }
        }))
        check(!SpeechMicrophoneAccess.granted(targetContext))
        tap("Voice input")
        tap("Don't allow")
        node("Microphone access is off")
        check(main { !model.speech.busy && model.draft(target).text == "Исходный текст." })
        tap("Open app settings")
        node("App info"); node("Zerus"); node("Permissions")
        back()
        node("Voice input")
        check(main { !model.speech.busy })

        // Android treats the second explicit denial as permanent. The next tap must
        // recover to settings even when the native permission dialog no longer appears.
        tap("Voice input")
        tap("Don't allow")
        node("Microphone access is off")
        tap("Not now")
        tap("Voice input")
        node("Microphone access is off")
        tap("Open app settings")
        tap("Permissions")
        tap("Microphone")
        tap("Allow only while using the app")
        back(); SystemClock.sleep(150)
        back(); SystemClock.sleep(150)
        back()
        node("Voice input")
        SystemClock.sleep(400)
        check(SpeechMicrophoneAccess.granted(targetContext))
        check(main { !model.speech.busy && model.draft(target).text == "Исходный текст." }) {
            "Returning from microphone settings must preserve the draft and never start recording."
        }
        tap("Voice input")
    }
    private fun runSmoke() {
        val submitted = AtomicInteger()
        val statusReads = AtomicInteger()
        val complete = AtomicBoolean(true)
        val target = Target("session-host", "voice-fixture", "fixture-run", "fixture-conversation", "speech-fixture")
        val host = MachineKey(target.connectionId, "gpu-fixture")
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            val body = okio.Buffer().also { request.body?.writeTo(it) }.readUtf8()
            val input = if (body.isNotBlank()) JSONObject(body) else JSONObject()
            val result = when (input.string("operation")) {
                "inspect" -> JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
                    .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "idle").put("phase", "idle")
                "speech_profiles" -> JSONObject().put("schema", 1).put("available", true).put("profiles", JSONArray().put(
                    JSONObject().put("id", "fixture").put("name", "Fixture Russian").put("available", true).put("model", "synthetic").put("max_duration_ms", 120_000)))
                "speech_submit" -> {
                    val payload = input.getJSONObject("payload")
                    check(payload.getString("request_id") == input.getString("request_id"))
                    check(input.getString("session").isEmpty() && input.getString("computer_id") == host.computerId)
                    val audio = payload.getJSONObject("audio")
                    check(audio.getInt("sample_rate") == 16_000 && audio.getInt("channels") == 1 && audio.getString("format") == "pcm_s16le")
                    check(Base64.decode(audio.getString("data_base64"), Base64.NO_WRAP).size in 3_200..SpeechProtocol.maxBytes)
                    submitted.incrementAndGet()
                    JSONObject().put("schema", 1).put("job_id", input.getString("request_id")).put("state", if (complete.get()) "completed" else "queued")
                }
                "speech_status" -> {
                    statusReads.incrementAndGet()
                    JSONObject().put("schema", 1).put("job_id", input.getJSONObject("payload").getString("job_id"))
                        .put("state", if (complete.get()) "completed" else "running").also { if (complete.get()) it.put("text", "Проверка голоса.") }
                }
                "speech_cancel" -> JSONObject().put("schema", 1).put("job_id", input.getJSONObject("payload").getString("job_id")).put("state", "cancelled")
                else -> JSONObject()
            }
            val receipt = JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", result)
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("private store") { model.storageReady }
        main {
            model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
            set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")))
            set(model, "setMachines", listOf(Machine(target.connectionId, host.computerId, "Fixture GPU", true)))
            set(model, "setRelayOperations", mapOf(target.connectionId to SpeechProtocol.operations))
            set(model, "setMachineOperations", mapOf(host to SpeechProtocol.operations))
            set(model, "setSelected", Session(target, "Fixture", "codex", "idle", "", "", 0, JSONObject()))
            model.edit(target, "Исходный текст.")
        }
        await("voice private store") { model.speech.ready }
        main { model.speech.chooseHost(host) }
        await("voice profiles") { !model.speech.loading && model.speech.profile?.available == true }
        val offline = MachineKey(target.connectionId, "offline-fixture")
        main { model.speech.refreshProfiles(); model.speech.chooseHost(offline) }
        await("offline host cancels profile loading") { model.speech.selection.host == offline && !model.speech.loading && model.speech.profiles.isEmpty() }
        main { model.speech.chooseHost(host) }
        await("voice profiles after changing hosts") { !model.speech.loading && model.speech.profile?.available == true }
        await("composer bridge") { model.speechComposer?.target == target }
        main {
            val bridge = checkNotNull(model.speechComposer)
            check(bridge.editor.text.toString() == model.draft(target).text)
            val attempt = SpeechAttempt(UUID.randomUUID().toString(), host, "fixture", model.draft(target), "completed", "Late recognition.")
            // No main-loop turn separates this IME edit from insertion: snapshotFlow cannot mirror it yet.
            bridge.editor.edit { replace(0, length, "Unmirrored IME edit.") }
            var result: Result<Unit>? = null
            CoroutineScope(Dispatchers.Main.immediate).launch(start = CoroutineStart.UNDISPATCHED) {
                result = runCatching { model.insertSpeech(attempt, false) }
            }
            check(result?.exceptionOrNull()?.message?.contains("draft changed") == true) {
                "Automatic speech insertion accepted a stale draft before the IME edit was mirrored."
            }
            check(bridge.editor.text.toString() == "Unmirrored IME edit.")
            check(model.draft(target).text == "Unmirrored IME edit.")
            bridge.editor.edit { replace(0, length, "Исходный текст.") }
            model.edit(target, "Исходный текст.")
        }
        permissionRecovery(model, target)
        await("microphone samples") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("first transcription") { !model.speech.busy && model.speech.attempt == null && submitted.get() == 1 }
        check(main { model.draft(target).text } == "Исходный текст. Проверка голоса.")
        check(statusReads.get() >= 1) { "Completed submit acknowledgement was mistaken for a transcript." }

        complete.set(false)
        main { check(model.speech.startReason(target).isBlank()) { model.speech.startReason(target) }; model.speech.start(target) }
        await("second recording") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("second submission") { submitted.get() == 2 }
        main { checkNotNull(model.speechComposer).editor.edit { replace(0, length, "Edited during recognition.") } }
        await("IME mirror during recognition") { model.draft(target).text == "Edited during recognition." }
        complete.set(true)
        await("recovered text") { !model.speech.working && model.speech.attempt?.state == "completed" }
        check(main { model.draft(target).text } == "Edited during recognition.")
        val finalText = "Reviewed IME edit. Проверка голоса. Next IME edit."
        main {
            val bridge = checkNotNull(model.speechComposer)
            bridge.editor.edit { replace(0, length, "Reviewed IME edit.") }
            model.speech.insertReviewed()
            check(bridge.editor.text.toString() == "Reviewed IME edit. Проверка голоса.") {
                "Reviewed insertion did not synchronously reconcile the actual editor."
            }
            // A second keystroke arrives while the durable insertion checkpoint can still be pending.
            bridge.editor.edit { append(" Next IME edit.") }
        }
        await("reviewed insertion") { model.speech.attempt == null }
        await("IME edit after insertion") { model.draft(target).text == finalText }
        check(main { model.speechComposer?.editor?.text.toString() } == finalText)

        main { check(model.speech.startReason(target).isBlank()) { model.speech.startReason(target) }; model.speech.start(target) }
        await("background recording") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.background() }
        await("background stop") { !model.speech.recording && model.speech.recorded }
        check(submitted.get() == 2)
        main { model.speech.discardRecording() }

        complete.set(false)
        main { check(model.speech.startReason(target).isBlank()) { model.speech.startReason(target) }; model.speech.start(target) }
        await("cancel recording") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("cancel submission") { submitted.get() == 3 }
        main { model.speech.cancel() }
        await("cancellation") { !model.speech.working && model.speech.attempt?.state == "cancelled" }
        check(main { model.draft(target).text } == finalText)
        main { model.speech.dismiss() }
        await("dismissal") { model.speech.attempt == null }
        val saved = PrivateStore(targetContext).messageState()
        check(saved.drafts.single { it.target == target }.text == finalText)
        check(saved.speechAppliedIds.size == 2 && saved.outgoing.isEmpty())
        main { activity.finish() }
    }
}
