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
import android.view.KeyEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
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
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.UUID
import java.io.FileInputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.*

/** Explicit emulator-only smoke: real microphone and encrypted drafts, synthetic in-process gateway. */
class SpeechSmokeInstrumentation : Instrumentation() {
    private var playerOnly = false
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); playerOnly = arguments?.getString("waveform_only") == "true"; start() }
    override fun onStart() {
        val result = Bundle()
        try {
            require(BuildConfig.DEBUG) { "Run the speech smoke against the debug build." }
            if (!playerOnly) runSmoke()
            runPlayerSmoke()
            result.putString("stream", if (playerOnly) "Voice player smoke passed: waveform tap/drag/accessibility seeking, paused/end/replay/playing seek, scoped playback, retained drafts and narrow light/dark screenshots.\n"
                else "Voice message smoke passed: immediate PCM capture during delayed/failed/unavailable discovery, exact preview provider selection without fallback, microphone settings recovery, recording editor/send lock, hardware Space/Escape with held-key and modifier handling, durable preview, explicit send only, local and authenticated remote playback, transcript history, typed draft preservation, status-only recovery, cancellation, background stop, waveform tap/drag/accessibility seeking, scoped playback and narrow light/dark player screenshots.\n")
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
            root.refresh()
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
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (true) {
            var target: AccessibilityNodeInfo? = node(label)
            while (target != null && !target.isClickable) target = target.parent
            if (target?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true) return
            // A closing dialog can expose its same-named title for one accessibility frame.
            check(SystemClock.elapsedRealtime() < deadline) { "UI element is not clickable: $label" }
            SystemClock.sleep(50)
        }
    }
    private fun back() {
        SystemClock.sleep(300) // Let the newly opened system page finish its transition.
        uiAutomation.executeShellCommand("input keyevent KEYCODE_BACK").use { FileInputStream(it.fileDescriptor).readBytes() }
        SystemClock.sleep(300)
    }
    private fun keyboardStop(activity: MainActivity, model: ZerusViewModel, target: Target, code: Int) {
        val original = main { model.draft(target) }
        val editorState = main { checkNotNull(model.speechComposer).editor }
        val editorSelection = main { editorState.selection }
        waitForIdleSync()
        fun findWritable(root: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (root == null) return null
            root.refresh()
            if (root.isVisibleToUser && root.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }) return root
            for (index in 0 until root.childCount) findWritable(root.getChild(index))?.let { return it }
            return null
        }
        // Inline recording feedback may hide the input's semantics, but must keep
        // the exact editor/draft mounted and reject ordinary keyboard entry.
        check(findWritable(uiAutomation.rootInActiveWindow) == null) { "Recording exposes a writable field" }
        main {
            activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A))
            activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A))
            check(model.speechComposer?.editor === editorState && editorState.text.toString() == original.text && editorState.selection == editorSelection)
        }
        fun findSend(root: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (root == null) return null
            root.refresh()
            if (root.contentDescription?.toString() == "Send message") return root
            for (index in 0 until root.childCount) findSend(root.getChild(index))?.let { return it }
            return null
        }
        // The trailing action becomes Stop while recording, including programmatic
        // captures that retain typed text. An ordinary Send may be absent or disabled.
        node("Stop recording")
        findSend(uiAutomation.rootInActiveWindow)?.let { found ->
            var send = found
            while (send.isEnabled && !send.isClickable) send = checkNotNull(send.parent)
            check(!send.isEnabled) { "Ordinary Send stayed enabled during recording" }
        }
        runBlocking { main { model.sendMessage(target) }.join() }
        var pickerOpened = false
        main { model.beginAttachmentPick(target) { pickerOpened = true } }
        for (file in original.attachments) runBlocking { main { model.removeAttachment(target, file.id) }.join() }
        check(!pickerOpened && main { model.pickerDraft == null && !model.attachmentPreparing }) { "Recording opened attachment selection" }
        check(runBlocking { AttachmentStore(targetContext).payload(original.attachments) }.length() == original.attachments.size)
        check(main { model.outgoing.isEmpty() && model.draft(target) == original }) { "Recording allowed ordinary delivery or changed its draft" }
        if (code == KeyEvent.KEYCODE_SPACE) {
            val screenshot = checkNotNull(uiAutomation.takeScreenshot())
            File(targetContext.getExternalFilesDir(null), "voice-recording-locked.png").outputStream().use {
                screenshot.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)
            }
            screenshot.recycle()
        }
        val down = SystemClock.uptimeMillis()
        fun key(action: Int, repeat: Int = 0, modifiers: Int = 0) = main {
            activity.dispatchKeyEvent(KeyEvent(down, SystemClock.uptimeMillis(), action, code, repeat, modifiers, 12, 0))
        }
        // Modified shortcuts must not stop recording or mutate the read-only editor.
        key(KeyEvent.ACTION_DOWN, modifiers = KeyEvent.META_CTRL_ON)
        key(KeyEvent.ACTION_UP, modifiers = KeyEvent.META_CTRL_ON)
        check(main { model.speech.recording && model.draft(target) == original })
        check(key(KeyEvent.ACTION_DOWN)) { "Recording did not own the stop key" }
        await("hardware stop durable preview") { !model.speech.busy && model.speech.draft != null }
        check(main { model.speechComposer?.editor === editorState && editorState.text.toString() == original.text && editorState.selection == editorSelection })
        val preview = main { model.speech.draft }
        node("Play").performAction(AccessibilityNodeInfo.ACTION_FOCUS)
        check(key(KeyEvent.ACTION_DOWN, repeat = 1))
        check(key(KeyEvent.ACTION_DOWN, repeat = 2, modifiers = KeyEvent.META_SHIFT_ON))
        check(key(KeyEvent.ACTION_UP, modifiers = KeyEvent.META_SHIFT_ON))
        check(main { model.speech.draft == preview && !model.attachments.playing && model.outgoing.isEmpty() && model.draft(target) == original }) {
            "Held stop key changed the preview, started playback, sent, or changed the typed draft"
        }
        waitForIdleSync()
        val restored = node(original.text)
        check(restored.isEditable && restored.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }) { "Editing did not resume after capture" }
        val edited = original.text + " Restored."
        check(restored.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, edited)
        }))
        await("post-recording draft edit") { model.draft(target).text == edited }
        main { check(model.speechComposer?.editor?.text.toString() == edited) { "Resumed editor differs from its draft" }; model.flushDrafts() }
        await("resumed draft checkpoint") { PrivateStore(targetContext).messageState().drafts.any { it.target == target && it.text == edited } }
        check(main { model.draft(target).text == edited && model.speechComposer?.editor?.text.toString() == edited }) {
            main { "Resumed text reverted: editor=${model.speechComposer?.editor?.text}; draft=${model.draft(target).text}; generation=${model.draft(target).generation}" }
        }
        waitForIdleSync(); SystemClock.sleep(500) // Inspect the fully expanded IME viewport.
        val editedNode = node(edited)
        val editedBounds = android.graphics.Rect(); editedNode.getBoundsInScreen(editedBounds)
        check(editedNode.isVisibleToUser && editedBounds.height() > 0)
        if (code == KeyEvent.KEYCODE_ESCAPE) check(editedBounds.height() >= 46 * targetContext.resources.displayMetrics.scaledDensity) {
            "Resumed two-line editor was clipped by the voice preview"
        }
        val keyboardTop = main { activity.window.decorView.let { it.height - (it.rootWindowInsets?.getInsets(android.view.WindowInsets.Type.ime())?.bottom ?: 0) } }
        check(editedBounds.bottom <= keyboardTop) { "Resumed editor is covered by the software keyboard" }
        val editingScreenshot = checkNotNull(uiAutomation.takeScreenshot())
        File(targetContext.getExternalFilesDir(null), "voice-resumed-editor-$code.png").outputStream().use {
            editingScreenshot.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it)
        }
        editingScreenshot.recycle()
        // Restore the fixture's exact prior text for the remaining transport/recovery checks.
        check(node(edited).performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, original.text)
        }))
        await("restored original typed draft") { model.draft(target).text == original.text }
        // Editing was visibly verified above; dismiss IME for the following preview actions.
        main { activity.window.insetsController?.hide(android.view.WindowInsets.Type.ime()) }
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
        check(main { !model.speech.busy && model.draft(target).text.isEmpty() })
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
        check(main { !model.speech.busy && model.draft(target).text.isEmpty() }) {
            "Returning from microphone settings must preserve the draft and never start recording."
        }
    }
    private fun runSmoke() {
        val submitted = AtomicInteger()
        val statusReads = AtomicInteger()
        val audioReads = AtomicInteger()
        val ordinarySends = AtomicInteger()
        val complete = AtomicBoolean(false)
        val interruptSubmit = AtomicBoolean(false)
        val profilesGate = CountDownLatch(1)
        val profileRequests = AtomicInteger()
        val failProfiles = AtomicBoolean(false)
        val unavailableProfile = AtomicBoolean(false)
        val lastProfileConversation = AtomicReference<String>()
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
                check(payload.string("expected_run_id") == target.run)
                check(payload.string("expected_conversation_id") == target.conversation ||
                    operation == "voice_profiles" && payload.string("expected_conversation_id") == "other-conversation")
            }
            val result = when (operation) {
                "inspect" -> inspect()
                "voice_profiles" -> {
                    profileRequests.incrementAndGet()
                    lastProfileConversation.set(payload.string("expected_conversation_id"))
                    check(profilesGate.await(90, TimeUnit.SECONDS)) { "Held discovery fixture was not released" }
                    if (failProfiles.get()) throw java.io.IOException("Synthetic discovery failure")
                    JSONObject().put("schema", 1).put("computers", JSONArray().put(
                        JSONObject().put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4").put("name", "Fixture GPU").put("profiles", JSONArray().put(
                            JSONObject().put("id", "fixture").put("name", "Fixture Russian").put("available", !unavailableProfile.get())
                                .put("reason", "Chosen fixture unavailable").put("model", "synthetic").put("max_duration_ms", 120_000))))
                        .put(JSONObject().put("machine_id", "c4075bf0-9c22-4a67-b768-55af2d2936a4").put("name", "Other GPU").put("profiles", JSONArray().put(
                            JSONObject().put("id", "other").put("name", "Other profile").put("available", true)))))
                }
                "voice_submit" -> {
                    val id = input.getString("request_id")
                    val audio = payload.getJSONObject("audio")
                    check(payload.getJSONObject("speech").string("machine_id") == "b4075bf0-9c22-4a67-b768-55af2d2936a4")
                    check(payload.getJSONObject("speech").string("profile_id") == "fixture")
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
                "send" -> { ordinarySends.incrementAndGet(); error("The phone must never submit a second native send after recognizing voice") }
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
            model.refreshActivity()
        }
        await("voice private store") { model.speech.ready }
        main { model.speech.refreshProfiles() }
        await("held voice discovery") { model.speech.loading && profileRequests.get() > 0 }
        await("fresh session") { model.activityVerified && model.voiceSendReason(target).isBlank() }
        // Only an empty composer exposes the microphone. Permission recovery is
        // independent of the retained text/attachment keyboard-lock fixture below.
        permissionRecovery(model, target)
        // The editor already exists after permission recovery. Enter this fixture
        // through its real editing action: model.edit deliberately cannot overwrite
        // an active editor that still owns the same draft generation.
        fun editable(root: AccessibilityNodeInfo?): AccessibilityNodeInfo? {
            if (root == null) return null
            root.refresh()
            if (root.actionList.any { it.id == AccessibilityNodeInfo.ACTION_SET_TEXT }) return root
            for (index in 0 until root.childCount) editable(root.getChild(index))?.let { return it }
            return null
        }
        waitForIdleSync()
        check(checkNotNull(editable(uiAutomation.rootInActiveWindow)).performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, "Исходный текст.")
        }))
        try {
            await("retained typed editor") {
                model.draft(target).text == "Исходный текст." && model.speechComposer?.editor?.text.toString() == "Исходный текст."
            }
        } catch (error: IllegalStateException) {
            throw IllegalStateException(main {
                "Fixture edit did not synchronize: selected=${model.selected?.target}; draft=${model.draft(target)}; bridge=${model.speechComposer?.target}; editor=${model.speechComposer?.editor?.text}"
            }, error)
        }
        val retainedAttachment = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(ByteArray(6_400))) }
        main {
            model.javaClass.getDeclaredMethod("save", Draft::class.java).apply { isAccessible = true }
                .invoke(model, model.draft(target).copy(attachments = listOf(retainedAttachment)))
        }
        main { model.speech.start(target) }
        await("microphone samples") { model.speech.recording && model.speech.seconds >= 1 }
        check(main { model.speech.loading && model.speech.selection == SpeechSelection() }) { "Recording waited for provider discovery" }
        keyboardStop(activity, model, target, KeyEvent.KEYCODE_SPACE)
        await("durable voice preview") { !model.speech.busy && model.speech.draft != null }
        check(submitted.get() == 0) { "Stopping the recorder uploaded without explicit Send" }
        val unbound = checkNotNull(PrivateStore(targetContext).voiceState().draft)
        check(unbound.durationMs >= 1_000 && unbound.machineId.isBlank() && unbound.profileId.isBlank())
        val capturedBytes = runBlocking { AttachmentStore(targetContext).payload(listOf(unbound.audio)) }
        check(capturedBytes.length() == 1 && unbound.audio.bytes >= 32_044) { "Delayed discovery did not capture durable PCM" }
        main { model.speech.submit() }
        check(submitted.get() == 0 && main { model.speech.draft } == unbound)
        profilesGate.countDown()
        await("discovered profiles without automatic selection") { !model.speech.loading && model.speech.computers.size == 2 }
        check(main { model.speech.selection == SpeechSelection() && model.speech.draft == unbound })
        tap("Transcription provider")
        await("preview profile discovery") { !model.speech.loading }
        tap("Choose voice computer"); tap("Fixture GPU")
        tap("Choose voice profile"); tap("Fixture Russian"); tap("Done")
        await("explicit bound preview") { model.speech.draft?.machineId == "b4075bf0-9c22-4a67-b768-55af2d2936a4" && model.speech.draft?.profileId == "fixture" }
        // Changing the global preference must not replace a retained preview's tuple.
        main {
            model.speech.chooseHost(MachineKey(target.connectionId, "c4075bf0-9c22-4a67-b768-55af2d2936a4"), target)
            model.speech.chooseProfile("other", target)
        }
        check(main { model.speech.selection.profileId == "other" && model.speech.draft?.profileId == "fixture" })
        tap("Transcription provider")
        await("pinned preview profiles") { !model.speech.loading }
        node("Fixture GPU"); tap("Fixture Russian"); node("synthetic"); tap("Fixture Russian")
        await("preview host owns explicit profile choice") { model.speech.selection.profileId == "fixture" }
        tap("Done")
        check(main { model.speech.selection.profileId == "fixture" && model.speech.draft?.audio == unbound.audio })
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
        check(node("Edited while voice is processing.").performAction(AccessibilityNodeInfo.ACTION_FOCUS))
        main {
            model.speech.start(target)
            // A physical edit in this same UI dispatch precedes read-only recomposition.
            activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A))
            activity.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A))
            check(model.speechComposer?.editor?.text.toString() == "Edited while voice is processing.")
        }
        await("second recording") { model.speech.recording && model.speech.seconds >= 1 }
        keyboardStop(activity, model, target, KeyEvent.KEYCODE_ESCAPE)
        await("second preview") { model.speech.draft != null && !model.speech.savingRecording }
        main { model.speech.submit() }
        await("status-only recovery") { submitted.get() == 2 && model.speech.outgoing.last().state == "transcribing" }
        val second = main { model.speech.outgoing.last() }
        main { model.speech.cancel(second) }
        await("cancellation") { model.speech.outgoing.last().state == "cancelled" && model.speech.activeRequests.isEmpty() }
        check(submitted.get() == 2)

        failProfiles.set(true)
        main { model.speech.start(target) }
        await("background recording") { model.speech.recording && model.speech.seconds >= 1 }
        await("failed discovery while microphone continues") { !model.speech.loading && model.speech.profileNotice.isNotBlank() }
        check(main { model.speech.recording && model.speech.notice.isBlank() })
        main { model.speech.background() }
        await("background saved preview") { model.speech.draft != null && !model.speech.busy }
        check(submitted.get() == 2)
        val failedDiscoveryPreview = main { model.speech.draft }
        main { model.speech.submit() }
        check(main { model.speech.draft == failedDiscoveryPreview && model.speech.outgoing.size == 2 })
        failProfiles.set(false); unavailableProfile.set(true)
        main { model.speech.refreshProfiles(target) }
        await("unavailable chosen profile") { !model.speech.loading && model.speech.computers.size == 2 }
        check(main { model.speech.selection.profileId == "fixture" && model.speech.sendReason() == "Chosen fixture unavailable" })
        main { model.speech.submit() }
        check(submitted.get() == 2 && main { model.speech.draft == failedDiscoveryPreview })
        tap("Discard"); tap("Discard recording")
        await("discard saved fixture") { model.speech.draft == null && !model.speech.busy }
        main { model.speech.start(target) }
        await("capture with unavailable chosen provider") { model.speech.recording && model.speech.seconds >= 1 }
        main { model.speech.stop() }
        await("unavailable provider retained preview") { model.speech.draft != null && !model.speech.busy }
        val persisted = PrivateStore(targetContext).voiceState()
        check(persisted.draft != null && persisted.outgoing.size == 2)
        tap("Discard"); node("Discard recording?")
        check(main { model.speech.draft != null }) { "Opening discard confirmation removed the recording" }
        tap("Keep recording")
        check(main { model.speech.draft == persisted.draft })
        check(PrivateStore(targetContext).messageState().drafts.single { it.target == target }.text == "Edited while voice is processing.")
        check(main { model.draft(target).attachments } == listOf(retainedAttachment))
        check(main { model.outgoing.isEmpty() })
        check(ordinarySends.get() == 0) { "Recording dispatched an ordinary send" }
        val pinned = checkNotNull(main { model.speech.draft })
        main {
            set(model, "setSelected", checkNotNull(model.selected).copy(target = target.copy(conversation = "other-conversation")))
            model.speech.refreshProfiles(target.copy(conversation = "other-conversation"))
        }
        await("other conversation profile discovery") { !model.speech.loading && lastProfileConversation.get() == "other-conversation" }
        main { activity.setContent { MaterialTheme { Surface { SpeechSettingsContent(model, target, pinned, showPreview = false) } } } }
        await("preview settings query original conversation") { !model.speech.loading && lastProfileConversation.get() == target.conversation }
        node("Fixture GPU"); node("Fixture Russian")
        main { model.speech.chooseHost(MachineKey(target.connectionId, "c4075bf0-9c22-4a67-b768-55af2d2936a4"), target, UUID.randomUUID().toString()) }
        check(main { model.speech.draft == pinned }) { "A stale preview changed the retained recording" }
        main { activity.finish() }
    }
    private fun runPlayerSmoke() {
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("player private store") { model.storageReady && model.speech.ready }
        val before = PrivateStore(targetContext).voiceState()
        val draftBefore = PrivateStore(targetContext).messageState().drafts
        // A deterministic audible fixture; displayed RMS bins derive from these actual PCM samples.
        val duration = 16_000
        val samples = IntArray(duration * 16) { sample ->
            val time = sample / 16_000.0
            if (sample / 4000 % 13 == 0) 0 else (sin(time * 2 * PI * 220) * (0.08 + .82 * abs(sin(time * 2 * PI * .713))) * 22_000).toInt()
        }
        val pcm = ByteBuffer.allocate(samples.size * 2).order(ByteOrder.LITTLE_ENDIAN).apply { samples.forEach { putShort(it.toShort()) } }.array()
        val rms = List(64) { bin ->
            val start = bin * samples.size / 64; val end = (bin + 1) * samples.size / 64
            sqrt((start until end).sumOf { samples[it].toDouble().pow(2) } / (end - start))
        }
        val bins = rms.map { (it / rms.max() * 255).roundToInt() }
        check(bins.distinct().size > 20 && bins.contains(0))
        val audio = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(pcm)) }
        val target = Target("player-host", "voice-player-fixture", "player-run", "player-conversation", "player-workspace")
        val id = UUID.randomUUID().toString()
        val voice = VoiceMessage(id, target.run, target.conversation, 100.0, 1, "submitted",
            audio.displayed().copy(messageId = id, index = 0), duration, "Synthetic audio used to verify the waveform player.", waveform = bins)
        val dark = mutableStateOf(true)
        main { activity.setContent {
            MaterialTheme(colorScheme = if (dark.value) darkColorScheme(primary = Color(0xFF67E8CB), onPrimary = Color(0xFF101517),
                surface = Color(0xFF1A2225), onSurface = Color(0xFFE9F0F2), onSurfaceVariant = Color(0xFF9AACB2))
            else lightColorScheme(primary = Color(0xFF086C58), onPrimary = Color.White)) {
                Surface(Modifier.fillMaxSize()) { Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Column(Modifier.widthIn(max = 320.dp).fillMaxWidth().padding(16.dp)) {
                        Text("You (voice)", style = MaterialTheme.typography.titleMedium)
                        VoiceMessageContent(model, target, voice)
                    }
                } }
            }
        } }
        val position = node("Recording playback position")
        check(position.rangeInfo != null)
        check(position.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.id,
            Bundle().apply { putFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE, .25f) }))
        await("accessible paused seek") { model.attachments.audioPositionMs in 3_800..4_200 && !model.attachments.playing && model.attachments.state(target, voice.audio)?.loading != true }
        val bounds = android.graphics.Rect(); node("Recording playback position").getBoundsInScreen(bounds)
        fun touch(fraction: Float) = (bounds.left + bounds.width() * fraction).roundToInt()
        uiAutomation.executeShellCommand("input tap ${touch(.6f)} ${bounds.centerY()}").use { FileInputStream(it.fileDescriptor).readBytes() }
        await("waveform tap seek") { model.attachments.audioPositionMs in 9_000..10_200 && !model.attachments.playing }
        uiAutomation.executeShellCommand("input swipe ${touch(.2f)} ${bounds.centerY()} ${touch(.8f)} ${bounds.centerY()} 350").use { FileInputStream(it.fileDescriptor).readBytes() }
        await("waveform drag seek") { model.attachments.audioPositionMs in 12_000..13_500 && !model.attachments.playing }
        tap("Play"); await("seeked playback") { model.attachments.playing }
        tap("Pause"); await("paused playback") { !model.attachments.playing }
        main { model.attachments.seekAudio(target, voice.audio, 1f, duration) }
        await("paused end seek") { !model.attachments.playing && model.attachments.audioPositionMs >= 15_900 }
        tap("Play"); await("play from completed position restarts at beginning") { model.attachments.playing && model.attachments.audioPositionMs < 2_000 }
        tap("Pause"); await("paused after replay") { !model.attachments.playing }
        main { model.attachments.seekAudio(target, voice.audio, .35f, duration); model.attachments.toggleAudio(target, voice.audio) }
        await("play waits for seek completion") { model.attachments.playing && model.attachments.audioPositionMs in 5_400..6_600 }
        main { model.attachments.seekAudio(target, voice.audio, .5f, duration) }
        await("playing seek continues playback") { model.attachments.playing && model.attachments.audioPositionMs in 7_800..9_000 }
        val other = target.copy(conversation = "other-conversation")
        main { model.attachments.seekAudio(other, voice.audio, .1f, duration) }
        await("seek remains scoped and never autoplays a different message") {
            model.attachments.playingKey == AttachmentIdentity.playbackKey(other, voice.audio) && !model.attachments.playing && model.attachments.audioPositionMs in 1_400..1_800
        }
        main { model.attachments.seekAudio(target, voice.audio, .4f, duration) }
        await("screenshot progress") { !model.attachments.playing && model.attachments.audioPositionMs in 6_200..6_600 }
        tap("Show recognized text"); node(voice.transcript); tap("Hide recognized text")
        for ((name, isDark) in listOf("dark" to true, "light" to false)) {
            main { dark.value = isDark }; waitForIdleSync(); SystemClock.sleep(300)
            val screenshot = checkNotNull(uiAutomation.takeScreenshot())
            File(targetContext.getExternalFilesDir(null), "voice-player-$name.png").outputStream().use { screenshot.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
            screenshot.recycle()
        }
        check(PrivateStore(targetContext).voiceState() == before)
        check(PrivateStore(targetContext).messageState().drafts == draftBefore)
        main { model.attachments.stopAudio(); activity.finish() }
    }
}
