package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.content.ActivityNotFoundException
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import kotlinx.coroutines.runBlocking
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.spec.ECGenParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** Synthetic transport and credentials only; no real provider calls or microphone access. */
class SpeechConfigurationSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG); smoke()
            result.putString("stream", "Speech configuration smoke passed: capability gate, no active conversation, native encrypted-key form, provider-specific explicit browser links and visible opening failures without changing unsaved fields/key, conflict edit retention, lost-response status-only recovery including missing receipts, saved-profile test, explicit default without preview retargeting, confirmed removal, unchanged pinned voice/typed drafts and Android P256 interoperability.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            uiAutomation.takeScreenshot()?.let { bitmap ->
                File(targetContext.getExternalFilesDir(null), "speech-configuration-failure.png").outputStream().use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }; bitmap.recycle()
            }
            result.putString("stream", "Speech configuration smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }; return result.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!main(condition)) { check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }; SystemClock.sleep(50) }
    }
    private fun nodes(): List<AccessibilityNodeInfo> {
        fun visit(node: AccessibilityNodeInfo?, into: MutableList<AccessibilityNodeInfo>) {
            if (node == null) return; node.refresh(); into.add(node)
            for (index in 0 until node.childCount) visit(node.getChild(index), into)
        }
        return mutableListOf<AccessibilityNodeInfo>().also { visit(uiAutomation.rootInActiveWindow, it) }
    }
    private fun node(label: String): AccessibilityNodeInfo {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        var nextScroll = 0L
        var scrollAction = AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
        while (true) {
            val all = nodes()
            all.find { it.text?.toString() == label || it.contentDescription?.toString() == label || it.hintText?.toString() == label }?.let { return it }
            val now = SystemClock.elapsedRealtime()
            check(now < deadline) { "UI element unavailable: $label" }
            if (now >= nextScroll) {
                val moved = all.firstOrNull { it.isScrollable }?.performAction(scrollAction)
                if (moved != true) scrollAction = if (scrollAction == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD else AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
                nextScroll = now + 500
            }
            SystemClock.sleep(50)
        }
    }
    private fun tap(label: String) {
        var item: AccessibilityNodeInfo? = node(label)
        while (item != null && !item.isClickable) item = item.parent
        check(item?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true) { "Could not click: $label" }
        waitForIdleSync(); SystemClock.sleep(150)
    }
    private fun text(label: String, value: String) {
        var item: AccessibilityNodeInfo? = node(label)
        while (item != null && !item.isEditable) item = item.parent
        check(item?.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply {
            putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, value)
        }) == true) { "Could not edit: $label" }
        waitForIdleSync(); SystemClock.sleep(150)
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun smoke() {
        val host = SpeechConfigHost(MachineKey("config-workspace", "config-relay-computer"), "b4075bf0-9c22-4a67-b768-55af2d2936a4", "Fixture GPU")
        val target = Target(host.key.computerId, "configuration-fixture", "fixture-run", "fixture-conversation", host.key.connectionId)
        val audio = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(ByteArray(6_400))) }
        val before = VoiceState(SpeechSelection(MachineKey(host.key.connectionId, host.machineId), "saved", 7),
            VoiceDraft(target, host.machineId, "saved", audio, 200, 100.0, 7))
        PrivateStore(targetContext).saveVoiceState(before)
        PrivateStore(targetContext).saveMessageState(MessageState(drafts = listOf(Draft(target, "Typed before provider setup."))))
        val capabilitiesEnabled = AtomicBoolean(false)
        val configReads = AtomicInteger(); val writes = AtomicInteger(); val tests = AtomicInteger(); val deletes = AtomicInteger()
        val revision = AtomicInteger(7); val conflict = AtomicBoolean(true); val loseReply = AtomicBoolean(true); val foreign = AtomicBoolean(false)
        val missingReceipt = AtomicBoolean(true)
        val receipts = ConcurrentHashMap<String, JSONObject>()
        val profiles = ConcurrentHashMap<String, JSONObject>()
        val keyPair = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }.generateKeyPair()
        val key = SpeechConfigKey(MessageDigest.getInstance("SHA-256").digest(keyPair.public.encoded).joinToString("") { "%02x".format(it) }, SpeechCredential.algorithm,
            Base64.getEncoder().encodeToString(keyPair.public.encoded))
        profiles["saved"] = JSONObject().put("id", "saved").put("revision", 7).put("name", "Saved fixture").put("adapter", "openai_http")
            .put("model", "gigaam").put("language", "ru").put("base_url", "https://saved.example.test/v1").put("allow_insecure_http", false)
            .put("credential", JSONObject().put("configured", true)).put("available", true).put("max_duration_ms", 120000)
        fun configuration() = JSONObject().put("schema", 1).put("machine_id", if (foreign.get()) "c4075bf0-9c22-4a67-b768-55af2d2936a4" else host.machineId)
            .put("revision", revision.get()).put("service", JSONObject().put("available", true)).put("credential_key", JSONObject()
                .put("key_id", key.id).put("algorithm", key.algorithm).put("public_key_spki", key.publicSpki))
            .put("adapters", JSONArray().put(JSONObject().put("id", "gigaam").put("models", JSONArray().put("v3_e2e_rnnt")).put("devices", JSONArray().put("cpu")))
                .put(JSONObject().put("id", "openai_http"))).put("profiles", JSONArray(profiles.values.sortedBy { it.string("id") }))
        fun receipt(id: String, state: String) = JSONObject().put("schema", 1).put("machine_id", host.machineId).put("request_id", id).put("state", state)
        fun decrypt(payload: JSONObject): String {
            val sealed = payload.getJSONObject("credential").getJSONObject("sealed_value"); val decoder = Base64.getDecoder()
            val public = KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(decoder.decode(sealed.getString("ephemeral_public_key"))))
            val shared = KeyAgreement.getInstance("ECDH").run { init(keyPair.private); doPhase(public, true); generateSecret() }
            fun hmac(key: ByteArray, value: ByteArray) = Mac.getInstance("HmacSHA256").run { init(SecretKeySpec(key, "HmacSHA256")); doFinal(value) }
            val context = SpeechCredential.context(payload)
            val aes = hmac(hmac(MessageDigest.getInstance("SHA-256").digest(keyPair.public.encoded), shared), context + byteArrayOf(1))
            return Cipher.getInstance("AES/GCM/NoPadding").run {
                init(Cipher.DECRYPT_MODE, SecretKeySpec(aes, "AES"), GCMParameterSpec(128, decoder.decode(sealed.getString("nonce"))))
                updateAAD(context); String(doFinal(decoder.decode(sealed.getString("ciphertext"))), Charsets.UTF_8)
            }
        }
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request(); val body = okio.Buffer().also { request.body?.writeTo(it) }.readUtf8()
            check(!body.contains("synthetic-private-key")) { "Plaintext key reached transport" }
            if (body.isBlank()) {
                val capabilities = JSONObject().put("protocol_version", 1).put("operations", JSONArray(if (capabilitiesEnabled.get()) (SpeechConfigProtocol.operations + VoiceProtocol.operations).toList() else emptyList<String>()))
                val result = if (request.url.encodedPath == "/v1/computers") JSONObject().put("computers", JSONArray().put(JSONObject()
                    .put("id", host.key.computerId).put("machine_id", host.machineId).put("name", host.name).put("online", true)
                    .put("snapshot", JSONObject().put("mobile_capabilities", capabilities).put("sessions", JSONArray())))) else capabilities
                return@addInterceptor Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                    .body(result.toString().toResponseBody("application/json".toMediaType())).build()
            }
            val input = JSONObject(body); val payload = input.getJSONObject("payload"); val operation = input.string("operation"); val id = input.getString("request_id")
            if (operation in SpeechConfigProtocol.operations) {
                check(input.string("computer_id") == host.key.computerId && input.string("session").isEmpty())
                if (operation != "speech_config") check(payload.string("expected_machine_id") == host.machineId)
                if (operation in SpeechConfigProtocol.mutations) {
                    check(payload.string("request_id") == id)
                    check(PrivateStore(targetContext).speechConfigOperations().any { it.id == id }) { "Mutation preceded durable checkpoint" }
                }
            }
            val result = when (operation) {
                "speech_config" -> { configReads.incrementAndGet(); configuration() }
                "speech_profile_upsert" -> {
                    writes.incrementAndGet(); check(decrypt(payload) == "synthetic-private-key")
                    if (conflict.getAndSet(false)) {
                        revision.incrementAndGet()
                        receipt(id, "failed").put("error_code", "revision_conflict").also { receipts[id] = it }
                    } else {
                        check(payload.getInt("expected_revision") == revision.get())
                        val profile = JSONObject(payload.getJSONObject("profile").toString()).put("revision", revision.incrementAndGet())
                            .put("credential", JSONObject().put("configured", true)).put("available", true).put("max_duration_ms", 120000)
                        profiles[profile.getString("id")] = profile
                        val saved = receipt(id, "completed").put("revision", revision.get()).put("profile_id", profile.getString("id")); receipts[id] = saved
                        if (loseReply.getAndSet(false)) throw java.io.IOException("Synthetic lost configuration response")
                        saved
                    }
                }
                "speech_config_status" -> if (missingReceipt.getAndSet(false)) receipt(payload.getString("operation_id"), "failed").put("error_code", "operation_not_found")
                    else checkNotNull(receipts[payload.getString("operation_id")])
                "speech_profile_test" -> {
                    tests.incrementAndGet(); val profile = checkNotNull(profiles[payload.getString("profile_id")])
                    check(payload.getInt("expected_profile_revision") == profile.getInt("revision"))
                    receipts[id] = receipt(id, "completed"); receipt(id, "running")
                }
                "speech_profile_remove" -> {
                    deletes.incrementAndGet(); check(payload.getInt("expected_revision") == revision.get())
                    profiles.remove(payload.getString("profile_id")); receipt(id, "completed").put("revision", revision.incrementAndGet()).also { receipts[id] = it }
                }
                "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray().put(JSONObject().put("machine_id", host.machineId)
                    .put("name", host.name).put("profiles", JSONArray(profiles.values.map { JSONObject(it.toString()).put("profile_revision", it.getLong("revision")) }))))
                else -> JSONObject()
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(JSONObject().put("request_id", id).put("state", "completed").put("result", result).toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("private storage") { model.storageReady }
        main {
            model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
            set(model, "setConnections", listOf(Connection(host.key.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")))
            set(model, "setMachines", listOf(Machine(host.key.connectionId, host.key.computerId, host.name, true, machineId = host.machineId)))
        }
        val manager = main { model.speechConfiguration.also { controller ->
            controller.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(controller, RelayApi(client))
        } }
        await("configuration store") { manager.ready && model.speech.ready }
        main { manager.select(host) }
        check(configReads.get() == 0 && main { manager.notice.contains("gateway") }) { "Old gateway received guessed operations" }
        main {
            capabilitiesEnabled.set(true)
            val capabilities = SpeechConfigProtocol.operations + VoiceProtocol.operations
            set(model, "setRelayOperations", mapOf(host.key.connectionId to capabilities)); set(model, "setMachineOperations", mapOf(host.key to capabilities))
            activity.setContent { MaterialTheme { SpeechConfigurationScreen(model, before.selection) {} } }
        }
        await("configuration without active conversation") { manager.configuration != null }
        check(main { model.selected == null })
        fun screenshot(name: String) {
            waitForIdleSync(); SystemClock.sleep(500) // Capture after the native dialog enter animation.
            val bitmap = checkNotNull(uiAutomation.takeScreenshot())
            File(targetContext.getExternalFilesDir(null), name).outputStream().use { bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }; bitmap.recycle()
        }
        val openedDocuments = CopyOnWriteArrayList<String>()
        val failOpening = AtomicBoolean(false)
        val browserMonitor = object : ActivityMonitor() {
            override fun onStartActivity(intent: Intent): ActivityResult? {
                if (intent.action != Intent.ACTION_VIEW) return null
                openedDocuments.add(intent.dataString.orEmpty())
                if (failOpening.getAndSet(false)) throw ActivityNotFoundException("Synthetic missing browser")
                return ActivityResult(Activity.RESULT_CANCELED, null) // Never launch a browser from the fixture.
            }
        }
        addMonitor(browserMonitor)
        val document = "https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"
        val readsBeforeHelp = configReads.get()
        check(openedDocuments.isEmpty())
        tap("Speech provider documentation")
        check(openedDocuments.toList() == listOf(document))
        check(configReads.get() == readsBeforeHelp && writes.get() == 0 && tests.get() == 0 && deletes.get() == 0)
        tap("Add provider"); node("Processing computer: ${host.name}")
        node(speechServiceReadiness(host, main { manager.configuration }, false))
        check(openedDocuments.size == 1) { "Opening the editor launched documentation" }
        tap("Local GigaAM setup")
        check(openedDocuments.last() == "$document#local-gigaam")
        screenshot("speech-provider-help-local.png")
        text("Name", "Remote fixture")
        tap("Type: Local GigaAM"); tap("OpenAI-compatible server")
        node("OpenAI-compatible server setup")
        check(openedDocuments.size == 2) { "Changing type launched documentation" }
        check(nodes().none { it.text?.toString() == "Local GigaAM setup" })
        text("Server base URL", "https://remote.example.test/v1"); text("Model", "gigaam")
        tap("API key: No key"); tap("Enter new key"); text("New API key", "synthetic-private-key")
        tap("OpenAI-compatible server setup")
        check(openedDocuments.last() == "$document#openai-compatible-server")
        node("Remote fixture"); node("https://remote.example.test/v1")
        failOpening.set(true)
        tap("OpenAI-compatible server setup")
        node("Could not open the documentation. Check that a browser is available and try again.")
        node("Remote fixture"); node("https://remote.example.test/v1")
        screenshot("speech-provider-help-http-failure.png")
        tap("OpenAI-compatible server setup")
        check(openedDocuments.toList() == listOf(document, "$document#local-gigaam",
            "$document#openai-compatible-server", "$document#openai-compatible-server", "$document#openai-compatible-server"))
        check(nodes().none { it.text?.toString()?.startsWith("Could not open the documentation.") == true })
        screenshot("speech-provider-help-http.png")
        check(writes.get() == 0 && tests.get() == 0 && deletes.get() == 0)
        removeMonitor(browserMonitor)
        // The same unsaved key must decrypt after both successful and failed browser dispatch.
        tap("Save provider")
        await("conflict retained editor") { manager.result?.errorCode == "revision_conflict" && manager.pending.isEmpty() }
        node("Remote fixture")
        tap("Refresh configuration")
        await("refreshed revision") { manager.configuration?.revision == 8L }
        tap("Use refreshed revision"); text("New API key", "synthetic-private-key"); tap("Save provider")
        await("unconfirmed saved operation") {
            if (writes.get() > 2 || manager.result?.state == "completed") error("Unexpected save receipt: writes=${writes.get()}, pending=${manager.pending.size}, result=${manager.result}")
            writes.get() == 2 && manager.pending.size == 1 && !manager.busy
        }
        check(!SpeechConfigProtocol.encode(PrivateStore(targetContext).speechConfigOperations()).toString().contains("synthetic-private-key"))
        val pendingBeforeStatus = main { manager.pending.single() }
        tap("Check operation status")
        await("missing receipt keeps original operation locked") { manager.result?.errorCode == "operation_not_found" && !manager.busy }
        check(main { manager.pending.single() == pendingBeforeStatus && !manager.canMutate(host) })
        check(PrivateStore(targetContext).speechConfigOperations().single() == pendingBeforeStatus)
        check(writes.get() == 2)
        tap("Check operation status")
        await("status-only save recovery") { manager.pending.isEmpty() && manager.configuration?.profiles?.size == 2 }
        check(writes.get() == 2)
        val created = main { manager.configuration!!.profiles.single { it.name == "Remote fixture" } }
        tap("Use for new recordings")
        await("explicit default selection") { model.speech.selection.profileId == created.id && model.speech.selection.profileRevision == created.revision }
        check(main { model.speech.draft == before.draft })
        main { check(manager.useForNewRecordings(manager.configuration!!.profiles.single { it.id == "saved" })) }
        await("default checkpoint") { PrivateStore(targetContext).voiceState().selection == before.selection }
        main { check(manager.apply(SpeechConfigProtocol.test(host, created))) }
        await("saved-profile synthetic test") { tests.get() == 1 && manager.pending.isEmpty() && !manager.loading }
        tap("Remove"); node("Remove speech provider?")
        check(deletes.get() == 0); tap("Keep provider"); check(deletes.get() == 0)
        tap("Remove"); tap("Remove provider")
        await("confirmed removal") { deletes.get() == 1 && manager.pending.isEmpty() && manager.configuration?.profiles?.size == 1 }
        foreign.set(true); main { manager.refresh() }
        await("foreign response rejected") { !manager.loading && manager.configuration == null && manager.notice.isNotBlank() }
        foreign.set(false); main { manager.refresh() }; await("correct host refreshed") { manager.configuration != null }
        check(main { model.speech.draft == before.draft && model.speech.selection == before.selection })
        check(PrivateStore(targetContext).voiceState() == before)
        check(PrivateStore(targetContext).messageState().drafts.single { it.target == target }.text == "Typed before provider setup.")
        check(runBlocking { AttachmentStore(targetContext).payload(listOf(audio)) }.length() == 1)
        val screenshot = checkNotNull(uiAutomation.takeScreenshot())
        File(targetContext.getExternalFilesDir(null), "speech-configuration.png").outputStream().use { screenshot.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        screenshot.recycle(); main { activity.finish() }
    }
}
