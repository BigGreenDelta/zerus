package app.zerus.mobile

import android.Manifest
import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import kotlinx.coroutines.runBlocking
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.Base64
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Explicit fresh-emulator qualification: CameraX uses the emulator's synthetic cameras. */
class VideoMessageSmokeInstrumentation : Instrumentation() {
    private fun step(name: String) { sendStatus(0, Bundle().apply { putString("stream", "Video fixture ${SystemClock.elapsedRealtime()}: $name\n") }) }
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }; return value.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 30_000
        while (!main(condition)) { check(SystemClock.elapsedRealtime() < deadline) { description }; SystemClock.sleep(50) }
    }
    private fun find(label: String, root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        root ?: return null
        root.refresh()
        if (root.isVisibleToUser && (root.text?.toString() == label || root.contentDescription?.toString() == label)) return root
        for (i in 0 until root.childCount) find(label, root.getChild(i))?.let { return it }; return null
    }
    private fun node(label: String, enabled: Boolean = false): AccessibilityNodeInfo {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (true) { find(label)?.takeIf { !enabled || it.isEnabled }?.let { return it }; check(SystemClock.elapsedRealtime() < deadline) { "Missing ${if(enabled) "enabled " else ""}$label" }; SystemClock.sleep(50) }
    }
    private fun tap(label: String) {
        var target: AccessibilityNodeInfo? = node(label, enabled = true)
        while (target != null && !target.isClickable) target = target.parent
        check(target?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true) { "Cannot activate $label" }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private data class Press(val time: Long, val x: Float, val y: Float)
    private fun touch(press: Press, action: Int, dx: Float = 0f, dy: Float = 0f) {
        val event = MotionEvent.obtain(press.time, SystemClock.uptimeMillis(), action, press.x + dx, press.y + dy, 0)
        event.source = InputDevice.SOURCE_TOUCHSCREEN
        try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
    }
    private fun down(label: String): Press {
        // Model settlement can precede the frame that re-enables the microphone.
        // Never inject a new gesture into the previous disabled preview action.
        val bounds = Rect().also { node(label, enabled = true).getBoundsInScreen(it) }
        return Press(SystemClock.uptimeMillis(), bounds.exactCenterX(), bounds.exactCenterY()).also { touch(it, MotionEvent.ACTION_DOWN) }
    }
    override fun onStart() {
        val report = Bundle()
        try {
            check(android.os.Build.FINGERPRINT.contains("generic") || android.os.Build.MODEL.contains("sdk")) { "Synthetic emulator only" }
            check(PrivateStore(targetContext).connections().isEmpty() && PrivateStore(targetContext).voiceState() == VoiceState()) { "Fresh emulator data required" }
            uiAutomation.grantRuntimePermission(targetContext.packageName, Manifest.permission.RECORD_AUDIO)
            uiAutomation.grantRuntimePermission(targetContext.packageName, Manifest.permission.CAMERA)
            val target = Target("computer", "fixture", "run", "conversation", "gateway")
            val session = Session(target, "Video fixture", "codex", "idle", "", "", 0, JSONObject())
            val sends = AtomicInteger()
            val sent = AtomicReference<JSONObject>()
            val client = OkHttpClient.Builder().addInterceptor { chain ->
                val request = chain.request();check(request.url.host == "relay.example.test")
                val text = okio.Buffer().also { request.body?.writeTo(it) }.readUtf8()
                val input = if (text.isBlank()) JSONObject() else JSONObject(text)
                val payload = input.optJSONObject("payload") ?: JSONObject()
                val result = when (input.string("operation")) {
                    "inspect" -> JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
                        .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "idle").put("phase", "idle")
                    "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray().put(JSONObject()
                        .put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4").put("name", "Fixture")
                        .put("profiles", JSONArray().put(JSONObject().put("id", "fixture").put("name", "Fixture").put("available", true)))))
                    "voice_submit" -> {
                        check(sends.incrementAndGet() in 1..4);sent.set(payload)
                        val video = payload.optJSONObject("video");val pcm = Base64.getDecoder().decode(payload.getJSONObject("audio").getString("data_base64"))
                        val id = input.getString("request_id")
                        val metadata = video?.let {
                            val mp4 = Base64.getDecoder().decode(it.getString("data_base64"));check(mp4.size <= VideoProtocol.MAX_BYTES)
                            check(mp4.copyOfRange(4, 8).contentEquals("ftyp".toByteArray()))
                            JSONObject(it.toString()).apply { remove("format"); remove("data_base64") }
                                .put("request_id", id).put("index", 1).put("name", "video.mp4").put("mime", "video/mp4").put("bytes", mp4.size)
                        }
                        JSONObject().put("schema", 1).put("message", JSONObject().put("id", id).put("run_id", target.run)
                            .put("conversation_id", target.conversation).put("created_at", System.currentTimeMillis() / 1000.0).put("revision", 1).put("state", "submitted")
                            .put("audio", JSONObject().put("request_id", id).put("index", 0).put("name", "voice.wav").put("mime", "audio/wav").put("bytes", pcm.size + 44).put("duration_ms", pcm.size / 32))
                            .put("video", metadata))
                    }
                    else -> error("Unexpected operation ${input.string("operation")}")
                }
                Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                    .body(JSONObject().put("state", "completed").put("request_id", input.string("request_id")).put("result", result).toString().toResponseBody("application/json".toMediaType())).build()
            }.build()
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
            try {
                await("Private storage") { model.storageReady }
                main {
                    model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                    model.speech.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model.speech, RelayApi(client))
                    set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                    set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture", true)))
                    set(model, "setRelayOperations", mapOf(target.connectionId to VoiceProtocol.operations))
                    set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to VoiceProtocol.operations))
                    for ((field, value) in listOf("relayFeatures" to mapOf(target.connectionId to setOf("voice_video")),
                        "machineFeatures" to mapOf(MachineKey(target.connectionId, target.computerId) to setOf("voice_video"))))
                        model.javaClass.getDeclaredField(field).apply { isAccessible = true }.set(model, value)
                    set(model, "setSelected", session);model.refreshActivity()

                }
                await("Exact conversation") { model.speech.ready && model.activityVerified && model.voiceSendReason(target).isBlank() }
                main { model.speech.chooseDefault(SpeechSelection(MachineKey(target.connectionId, "b4075bf0-9c22-4a67-b768-55af2d2936a4"), "fixture")) }
                val short = down("Voice input");touch(short, MotionEvent.ACTION_UP)
                node("Video input");check(main { model.speech.videoMode && !model.speech.recording });check(sends.get() == 0)
                val held = down("Video input")
                await("Camera recording samples: ${main { model.speech.notice }}") { model.speech.recording && model.speech.seconds >= 2 }
                val up = -70f * targetContext.resources.displayMetrics.density
                touch(held, MotionEvent.ACTION_MOVE, dy = up);touch(held, MotionEvent.ACTION_UP, dy = up)
                await("Locked capture") { model.speech.recordingLocked }
                SystemClock.sleep(300);check(main { model.speech.recording });check(sends.get() == 0)
                step("switch locked recording camera");tap("Switch camera");await("Persistent recording after camera switch") { model.speech.seconds >= 3 }
                val stoppedCapture = main { checkNotNull(model.speech.activeCaptureId) }
                step("stop after camera switch");tap("Stop recording")
                main { model.speech.sendCapture(stoppedCapture) } // A late release after Stop cannot authorize Send.
                await("Durable video preview") { model.speech.draft?.video != null && !model.speech.busy }
                val draft = main { checkNotNull(model.speech.draft) };val video = checkNotNull(draft.video)
                check(PrivateStore(targetContext).voiceState().draft == draft)
                val original = ByteArrayOutputStream();runBlocking { AttachmentStore(targetContext).decrypt(video.file) { original.write(it) } }
                val audio = ByteArrayOutputStream();runBlocking { AttachmentStore(targetContext).decrypt(draft.audio) { audio.write(it) } }
                val pcm = VoiceProtocol.pcm(audio.toByteArray());check(kotlin.math.abs(pcm.size / 32 - video.durationMs) <= 1000)
                check(sends.get() == 0)
                uiAutomation.takeScreenshot()?.let { image ->
                    val destination = java.io.File(targetContext.getExternalFilesDir(null), "video-message-preview.png")
                    destination.outputStream().use { image.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
                }
                tap("Play video message")
                await("Tap-only video playback with audio") { model.attachments.playing && model.attachments.audioPositionMs > 0 }
                main { model.attachments.stopAudio() }
                try { await("Confirmed provider") { !model.speech.loading && model.speech.sendReason().isBlank() } }
                catch (error: Exception) { throw IllegalStateException(main { "Provider: ${model.speech.sendReason()}, notice=${model.speech.notice}, profile=${model.speech.profileNotice}, loading=${model.speech.loading}, draftProvider=${model.speech.draft?.machineId}/${model.speech.draft?.profileId}, computers=${model.speech.computers}" }, error) }
                // Exercise the retained preview's explicit Send through accessibility.
                step("send retained video");tap("Send video message")
                await("Single accepted video") { model.speech.outgoing.singleOrNull()?.state == "submitted" }
                val payload = checkNotNull(sent.get())
                check(Base64.getDecoder().decode(payload.getJSONObject("video").getString("data_base64")).contentEquals(original.toByteArray()))
                check(Base64.getDecoder().decode(payload.getJSONObject("audio").getString("data_base64")).contentEquals(pcm))
                check(sends.get() == 1);check(main { model.speech.draft == null })
                step("cancel next capture");val cancel = down("Video input");await("Second capture") { model.speech.seconds >= 1 && model.speech.recording }
                touch(cancel, MotionEvent.ACTION_MOVE, dx = -110f * targetContext.resources.displayMetrics.density)
                touch(cancel, MotionEvent.ACTION_UP)
                await("Cancelled media removed") { !model.speech.busy };check(main { model.speech.draft == null });check(sends.get() == 1)
                step("default release-send");val ordinaryRelease = down("Video input")
                await("Confirmed provider during held video") { model.speech.recording && model.speech.seconds >= 1 && !model.speech.loading }
                touch(ordinaryRelease, MotionEvent.ACTION_UP)
                await("Release sends once") { model.speech.outgoing.size == 2 && model.speech.outgoing.all { it.state == "submitted" } }
                check(sends.get() == 2);check(main { model.speech.draft == null && model.speech.outgoing.map { it.id }.distinct().size == 2 })
                main { model.speech.setPreviewBeforeSend(true) }
                for ((label, videoMode) in listOf("Video input" to true, "Voice input" to false)) {
                    step("preview option: $label")
                    if (!videoMode) { val switch = down("Video input");touch(switch, MotionEvent.ACTION_UP);node(label) }
                    val before = sends.get()
                    val held = down(label)
                    await("Preview-option capture with confirmed provider") { model.speech.recording && model.speech.seconds >= 1 && !model.speech.loading }
                    val capture = main { checkNotNull(model.speech.activeCaptureId) }
                    main { model.speech.setPreviewBeforeSend(false) }
                    check(main { model.speech.captureNeedsPreview && model.speech.previewBeforeSend })
                    touch(held, MotionEvent.ACTION_UP)
                    await("Release retains reviewable media") { !model.speech.busy && model.speech.draft != null }
                    check(sends.get() == before)
                    val retained = main { checkNotNull(model.speech.draft) }
                    check((retained.video != null) == videoMode)
                    check(PrivateStore(targetContext).voiceState().let { it.previewBeforeSend && it.draft == retained })
                    main { model.speech.sendCapture(capture) } // Late release cannot submit a saved preview.
                    check(sends.get() == before)
                    tap(if (videoMode) "Send video message" else "Send voice message")
                    await("Separate Send submits exact retained media once") { model.speech.draft == null && sends.get() == before + 1 && model.speech.activeRequests.isEmpty() }
                    check(sent.get().getString("request_id") == model.speech.outgoing.last().id)
                    check(sent.get().has("video") == videoMode)
                }
                check(sends.get() == 4 && model.speech.outgoing.size == 4)
            } finally { main { model.attachments.stopAudio();activity.finish() };client.dispatcher.executorService.shutdown() }
            report.putString("stream", "Video message smoke passed: synthetic CameraX MP4/audio extraction, short-tap mode, upward lock, stop/retained encrypted preview, default release-send, tap playback, camera switch and left cancellation. Preview preference retains BOTH video and audio after physical release despite a confirmed provider; stale release and mid-capture setting changes cannot send. Separate Send uploads the exact durable recording once.\n")
            finish(Activity.RESULT_OK, report)
        } catch (error: Throwable) { report.putString("stream", "Video smoke failed: ${error.stackTraceToString()}\n");finish(Activity.RESULT_CANCELED, report) }
    }
}
