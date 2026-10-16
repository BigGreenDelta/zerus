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
import java.util.Base64
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Explicit fresh-emulator qualification: CameraX uses the emulator's synthetic cameras. */
class VideoBackgroundStopInstrumentation : Instrumentation() {
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
    private fun settledAction(label: String): Rect {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var previous: Rect? = null
        var stableSince = SystemClock.elapsedRealtime()
        while (true) {
            val bounds = find(label)?.takeIf { it.isEnabled }?.let { Rect().also(it::getBoundsInScreen) }
            val now = SystemClock.elapsedRealtime()
            if (bounds == null || bounds != previous) stableSince = now
            if (bounds != null && now - stableSince >= 300) return bounds
            check(now < deadline) { "Unsettled $label" }
            previous = bounds; SystemClock.sleep(40)
        }
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
        // Model settlement and IME dismissal can precede committed action geometry.
        // The same15s deadline covers enabled publication and stable bounds before a new press.
        // This adds no delay between camera switching and the already-pinned physical Stop.
        val bounds = settledAction(label)
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
                val request = chain.request();check(request.url.host == "relay.example.test") { "Unexpected fixture host ${request.url.host}" }
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
                        check(sends.incrementAndGet() in 1..4) { "Unexpected voice_submit count ${sends.get()}" };sent.set(payload)
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
                main { model.speech.setPreviewBeforeSend(false) }
                fun noSend(label: String) { check(sends.get() == 0) { "$label dispatched ${sends.get()} voice_submit" } }
                fun discard() {
                    main { model.speech.discardRecording() }
                    await("Synthetic preview discarded") { !model.speech.busy && model.speech.draft == null }
                }
                fun cancelHeld(label: String, video: Boolean) {
                    val press = down(label)
                    await("Held cancellation capture") { model.speech.recording && model.speech.seconds >= 1 && !model.speech.loading }
                    val capture = main { checkNotNull(model.speech.activeCaptureId) }
                    touch(press, MotionEvent.ACTION_CANCEL)
                    await("Cancelled held pointer retains preview") { !model.speech.busy }
                    noSend(label)
                    check(main { model.speech.draft != null && (model.speech.draft?.video != null) == video })
                    main { model.speech.sendCapture(capture) }
                    noSend("Late cancelled release")
                    discard()
                }
                step("cancel short press before hold")
                val shortCancel = down("Voice input");touch(shortCancel, MotionEvent.ACTION_CANCEL)
                SystemClock.sleep(250) // Outlive the150ms hold deadline; cancellation cannot revive it.
                check(main { !model.speech.recording && !model.speech.videoMode });noSend("Short cancellation")
                step("cancel held audio")
                cancelHeld("Voice input", video = false)
                val short = down("Voice input");touch(short, MotionEvent.ACTION_UP);node("Video input")
                step("cancel held video")
                cancelHeld("Video input", video = true)
                step("cancel second click on locked video")
                val lock = down("Video input")
                await("Capture before lock") { model.speech.recording && model.speech.seconds >= 1 }
                val up = -70f * targetContext.resources.displayMetrics.density
                touch(lock, MotionEvent.ACTION_MOVE, dy = up);touch(lock, MotionEvent.ACTION_UP, dy = up)
                await("Locked capture") { model.speech.recordingLocked }
                val lockedId = main { checkNotNull(model.speech.activeCaptureId) }
                val before = main { model.speech.seconds }
                val second = down("Send recording");touch(second, MotionEvent.ACTION_CANCEL)
                await("Cancelled locked click keeps capture running") { model.speech.recording && model.speech.recordingLocked &&
                    model.speech.activeCaptureId == lockedId && model.speech.seconds > before }
                noSend("Locked second-click cancellation");check(main { model.speech.draft == null })
                tap("Stop recording")
                await("Explicit locked Stop retains preview") { !model.speech.busy && model.speech.draft?.video != null }
                noSend("Locked Stop");discard()
                step("background physical held video with preview off")
                down("Video input")
                await("Background capture with confirmed provider") { model.speech.recording && model.speech.seconds >= 1 && !model.speech.loading && !model.speech.captureNeedsPreview }
                check(sends.get() == 0)
                val capture = main { checkNotNull(model.speech.activeCaptureId) }
                main { check(activity.moveTaskToBack(true)) }
                await("Background capture finishes") { !model.speech.busy }
                check(sends.get() == 0) { "Background dispatched ${sends.get()} voice_submit without a release or Send" }
                check(main { model.speech.draft?.video != null })
                main { model.speech.sendCapture(capture) }
                check(sends.get() == 0)
            } finally { main { model.attachments.stopAudio();activity.finish() };client.dispatcher.executorService.shutdown() }
            report.putString("stream", "Video background stop passed: physical ACTION_CANCEL before hold, during held audio/video and on locked second-click; real UP still switches/locks, Stop retains preview; physical held video with confirmed provider and preview disabled survives actual Activity background with retained draft and zero voice_submit, no late-release replay.\n")
            finish(Activity.RESULT_OK, report)
        } catch (error: Throwable) { report.putString("stream", "Video background stop failed: ${error.stackTraceToString()}\n");finish(Activity.RESULT_CANCELED, report) }
    }
}
