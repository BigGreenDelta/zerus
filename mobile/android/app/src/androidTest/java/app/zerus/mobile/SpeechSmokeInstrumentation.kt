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
            result.putString("stream", "Voice message smoke passed: microphone settings recovery, durable preview, explicit send only, local and authenticated remote playback, transcript history, typed draft preservation, status-only recovery, cancellation, background stop.\n")
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
            check(SystemClock.elapsedRealtime() < deadline) {
                fun labels(root: AccessibilityNodeInfo?): List<String> = if (root == null) emptyList() else
                    listOfNotNull(root.text?.toString(), root.contentDescription?.toString()) + (0 until root.childCount).flatMap { labels(root.getChild(it)) }
                "UI element unavailable: $label; fixture labels: ${labels(uiAutomation.rootInActiveWindow)}"
            }
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
        val audioReads = AtomicInteger()
        val complete = AtomicBoolean(false)
        val interruptSubmit = AtomicBoolean(false)
        val target = Target("session-host", "voice-fixture", "fixture-run", "fixture-conversation", "speech-fixture")
        val owner = MachineKey(target.connectionId, target.computerId)
        val messages = java.util.concurrent.ConcurrentHashMap<String, JSONObject>()
        val recordings = java.util.concurrent.ConcurrentHashMap<String, ByteArray>()
        val revision = AtomicInteger(1)
        fun inspect() = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "idle").put("phase", "idle")
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            val body = okio.Buffer().also { request.body?.writeTo(it) }.readUtf8()
            val input = if (body.isNotBlank()) JSONObject(body) else JSONObject()
            val payload = input.optJSONObject("payload") ?: JSONObject()
            val operation = input.string("operation")
            if (operation in VoiceProtocol.operations) {
                check(input.string("computer_id") == target.computerId && input.string("session") == target.session)
                check(payload.string("request_id") == input.string("request_id"))
                check(payload.string("expected_run_id") == target.run && payload.string("expected_conversation_id") == target.conversation)
            }
            val result = when (operation) {
                "inspect" -> inspect()
                "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray().put(
                    JSONObject().put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4").put("name", "Fixture GPU").put("profiles", JSONArray().put(
                        JSONObject().put("id", "fixture").put("name", "Fixture Russian").put("available", true).put("model", "synthetic").put("max_duration_ms", 120_000)))))
                "voice_submit" -> {
                    val id = input.getString("request_id")
                    val audio = payload.getJSONObject("audio")
                    check(payload.getJSONObject("speech").string("machine_id") == "b4075bf0-9c22-4a67-b768-55af2d2936a4")
                    val pcm = Base64.decode(audio.getString("data_base64"), Base64.NO_WRAP)
                    check(pcm.size in 3_200..VoiceProtocol.MAX_PCM_BYTES)
                    val stored = PrivateStore(targetContext).voiceState()
                    check(stored.draft == null && stored.outgoing.any { it.id == id && it.audio != null }) { "Voice upload preceded durable outbox storage" }
                    submitted.incrementAndGet()
                    val wav = VoiceProtocol.wav(pcm); recordings[id] = wav
                    val message = JSONObject().put("id", id).put("run_id", target.run).put("conversation_id", target.conversation)
                        .put("created_at", System.currentTimeMillis() / 1000.0).put("revision", revision.incrementAndGet()).put("state", "queued")
                        .put("audio", JSONObject().put("request_id", id).put("index", 0).put("name", "voice.wav").put("mime", "audio/wav")
                            .put("bytes", wav.size).put("duration_ms", pcm.size / 32))
                    messages[id] = message
                    if (interruptSubmit.getAndSet(false)) throw java.io.IOException("Synthetic lost acceptance")
                    JSONObject().put("schema", 1).put("message", message)
                }
                "voice_status", "voice_cancel" -> {
                    statusReads.incrementAndGet()
                    val message = JSONObject(checkNotNull(messages[payload.getString("message_id")]).toString())
                    message.put("revision", revision.incrementAndGet()).put("state", when {
                        operation == "voice_cancel" -> "cancelled"
                        complete.get() -> "submitted"
                        else -> "transcribing"
                    })
                    if (complete.get()) message.put("transcript", "Проверка голоса.")
                    messages[message.getString("id")] = message
                    JSONObject().put("schema", 1).put("message", message)
                }
                "attachment_read" -> {
                    audioReads.incrementAndGet()
                    val id = payload.getString("message_id"); val bytes = checkNotNull(recordings[id])
                    val offset = payload.getInt("offset"); val length = payload.getInt("length")
                    JSONObject().put("request_id", input.getString("request_id")).put("name", target.session).put("message_id", id)
                        .put("index", 0).put("bytes", bytes.size).put("sha256", java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) })
                        .put("offset", offset).put("length", length).put("data_base64", Base64.encodeToString(bytes.copyOfRange(offset, offset + length), Base64.NO_WRAP))
                }
                "send" -> error("The phone must never submit a second native send after recognizing voice")
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
            model.attachments.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model.attachments, RelayApi(client))
            set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")))
            set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture session host", true)))
            val capabilities = VoiceProtocol.operations + "attachment_read"
            set(model, "setRelayOperations", mapOf(target.connectionId to capabilities))
            set(model, "setMachineOperations", mapOf(owner to capabilities))
            set(model, "setSelected", Session(target, "Fixture", "codex", "idle", "", "", 0, JSONObject()))
            model.edit(target, "Исходный текст.")
            model.refreshActivity()
        }
        await("voice private store") { model.speech.ready }
        main { model.speech.refreshProfiles() }
        await("voice profiles") { !model.speech.loading && model.speech.profile?.available == true }
        await("fresh session") { model.activityVerified && model.voiceSendReason(target).isBlank() }
        permissionRecovery(model, target)
        await("microphone samples") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("durable voice preview") { !model.speech.busy && model.speech.draft != null }
        check(submitted.get() == 0) { "Stopping the recorder uploaded without explicit Send" }
        check(PrivateStore(targetContext).voiceState().draft != null)
        tap("Play")
        await("local audio playback") { model.attachments.playing }
        main { model.attachments.stopAudio() }
        tap("Send voice message")
        await("server transcription") { submitted.get() == 1 && model.speech.outgoing.single().state == "transcribing" }
        main { checkNotNull(model.speechComposer).editor.edit { replace(0, length, "Edited while voice is processing.") } }
        await("typed draft mirror") { model.draft(target).text == "Edited while voice is processing." }
        complete.set(true)
        await("voice delivery") { model.speech.outgoing.single().state == "submitted" && model.speech.activeRequests.isEmpty() }
        check(main { model.draft(target).text } == "Edited while voice is processing.")
        node("Voice message"); tap("Show recognized text"); node("Проверка голоса.")
        check(submitted.get() == 1 && statusReads.get() > 0)
        val first = main { model.speech.outgoing.single() }
        // Download without this phone's encrypted local ID to exercise another device's history playback.
        main { model.attachments.toggleAudio(target, first.message!!.audio) }
        await("authenticated remote playback") { model.attachments.playing && audioReads.get() > 0 }
        main { model.attachments.stopAudio() }

        // Lost acceptance recovers by stable message ID, never a second create/upload.
        complete.set(false); interruptSubmit.set(true)
        main { model.speech.start(target) }
        await("second recording") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("second preview") { model.speech.draft != null && !model.speech.savingRecording }
        main { model.speech.submit() }
        await("status-only recovery") { submitted.get() == 2 && model.speech.outgoing.last().state == "transcribing" }
        val second = main { model.speech.outgoing.last() }
        main { model.speech.cancel(second) }
        await("cancellation") { model.speech.outgoing.last().state == "cancelled" && model.speech.activeRequests.isEmpty() }
        check(submitted.get() == 2)

        main { model.speech.start(target) }
        await("background recording") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.background() }
        await("background saved preview") { model.speech.draft != null && !model.speech.busy }
        check(submitted.get() == 2)
        val persisted = PrivateStore(targetContext).voiceState()
        check(persisted.draft != null && persisted.outgoing.size == 2)
        tap("Discard"); node("Discard recording?")
        check(main { model.speech.draft != null }) { "Opening discard confirmation removed the recording" }
        tap("Keep recording")
        check(main { model.speech.draft == persisted.draft })
        check(PrivateStore(targetContext).messageState().drafts.single { it.target == target }.text == "Edited while voice is processing.")
        check(main { model.outgoing.isEmpty() })
        main { activity.finish() }
    }
}
