package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
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
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Real settings controls with no selected conversation; only synthetic computer-scoped reads. */
class DefaultVoiceProvidersSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            smoke()
            result.putString("stream", "Default voice providers passed: no conversation, direct relay routes, actual Refresh and computer/profile choices, changed revisions require selection, retained recording route, stale response rejection, explicit failure/retry and capability recovery. Zero mutations.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Default voice providers failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }
        return value.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }
            SystemClock.sleep(35)
        }
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (root.isVisibleToUser && predicate(root)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), predicate)?.let { return it }
        return null
    }
    private fun text(value: String) = find(uiAutomation.rootInActiveWindow) { it.text?.toString() == value }
    private fun tap(label: String) {
        await("clickable $label") {
            var node = text(label)
            while (node != null && !node.isClickable) node = node.parent
            node?.performAction(AccessibilityNodeInfo.ACTION_CLICK) == true
        }
    }
    private fun choose(label: String, option: String) {
        val owner = checkNotNull(uiAutomation.rootInActiveWindow).windowId
        tap(label)
        await("$label menu in its own window") { uiAutomation.rootInActiveWindow?.windowId != owner && text(option) != null }
        tap(option)
        await("$label menu dismissed") { uiAutomation.rootInActiveWindow?.windowId == owner }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun smoke() {
        val store = PrivateStore(targetContext)
        check(store.connections().isEmpty() && store.messageState() == MessageState()) { "Use fresh task-owned emulator app data" }
        val nativeA = "00000000-0000-4000-8000-000000000031"
        val nativeB = "00000000-0000-4000-8000-000000000032"
        val machines = listOf(Machine("fixture", "route-a", "Fixture computer A", true, machineId = nativeA),
            Machine("fixture", "route-b", "Fixture computer B", true, machineId = nativeB))
        val target = Target("route-a", "codex/fixture/voice", "fixture-run", "fixture-conversation", "fixture")
        val audio = runBlocking { AttachmentStore(targetContext).importRecording(VoiceProtocol.wav(ByteArray(3_200))) }
        val saved = VoiceDraft(target, nativeA, "local", audio, 100, System.currentTimeMillis() / 1000.0, 7)
        store.saveVoiceState(VoiceState(SpeechSelection(MachineKey("fixture", nativeA), "local", 7), saved))
        val requests = AtomicInteger()
        val mutations = AtomicInteger()
        val revision = AtomicInteger(7)
        val fail = AtomicBoolean(false)
        val held = AtomicReference<CountDownLatch?>(null)
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            check(request.url.host == "relay.example.test" && request.url.encodedPath == "/v1/requests")
            val input = JSONObject(okio.Buffer().also { request.body?.writeTo(it) }.readUtf8())
            if (input.string("operation") != "speech_profiles") { mutations.incrementAndGet(); error("Unexpected fixture operation") }
            check(input.string("session").isEmpty() && input.getJSONObject("payload").length() == 0)
            val route = input.getString("computer_id"); check(route in setOf("route-a", "route-b"))
            val gate = held.get()
            requests.incrementAndGet()
            gate?.let { check(it.await(20, TimeUnit.SECONDS)) }
            if (fail.get()) throw java.io.IOException("Synthetic profiles unavailable")
            val profile = JSONObject().put("id", "local").put("name", if (route == "route-a") "Fixture profile A v${revision.get()}" else "Fixture profile B")
                .put("model", "fixture-model").put("available", true).put("profile_revision", if (route == "route-a") revision.get() else 9)
            val result = JSONObject().put("schema", 1).put("available", true).put("profiles", JSONArray(listOf(profile)))
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", result)
                    .toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        try {
            await("private state") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection("fixture", "Fixture workspace", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", machines)
                set(model, "setRelayOperations", mapOf("fixture" to setOf("speech_profiles")))
                set(model, "setMachineOperations", machines.associate { MachineKey(it.connectionId, it.id) to setOf("speech_profiles") })
                activity.setContent { MaterialTheme { Surface { Column(Modifier.fillMaxSize().safeDrawingPadding().verticalScroll(rememberScrollState()).padding(16.dp)) {
                    Text("Voice settings / synthetic fixture")
                    SpeechSettingsContent(model, target = null, showPreview = false)
                } } } }
            }
            await("saved default and computer profiles without a conversation") { text("Fixture computer A") != null && text("Fixture profile A v7") != null }
            check(main { model.selected == null && model.speech.computers.isEmpty() && model.speech.draft == saved })
            check(text("Open a conversation to see the voice computers it can reach. Your selection is saved.") == null)
            val first = requests.get()
            val refresh = CountDownLatch(1); held.set(refresh)
            tap("Refresh voice profiles")
            await("actual refresh request and visible progress") { requests.get() > first && main { model.defaultVoiceProviders.loading } &&
                find(uiAutomation.rootInActiveWindow) { it.className?.toString() == "android.widget.ProgressBar" } != null }
            revision.set(8); refresh.countDown(); held.set(null)
            await("updated profile") { text("Fixture profile A v8") != null && main { !model.defaultVoiceProviders.loading } }
            check(main { model.speech.selection.profileRevision == 7L && model.speech.draft == saved })
            choose("Fixture profile A v8", "Fixture profile A v8")
            await("explicit default revision selection") { main { model.speech.selection.profileRevision == 8L && model.speech.draft == saved } }
            choose("Fixture computer A", "Fixture computer B")
            await("second computer") { text("Choose voice profile") != null && main { model.defaultVoiceProviders.profiles.singleOrNull()?.name == "Fixture profile B" } }
            choose("Choose voice profile", "Fixture profile B")
            await("second default with original recording retained") { main { model.speech.selection.host == MachineKey("fixture", nativeB) &&
                model.speech.selection.profileRevision == 9L && model.speech.draft == saved && model.selected == null } }
            val late = CountDownLatch(1); held.set(late)
            val previous = requests.get(); tap("Refresh voice profiles")
            await("held old computer read") { requests.get() > previous }
            held.set(null)
            choose("Fixture computer B", "Fixture computer A")
            await("new computer profiles") { main { model.defaultVoiceProviders.profiles.singleOrNull()?.name == "Fixture profile A v8" } }
            late.countDown()
            uiAutomation.waitForIdle(200, 3_000)
            check(main { model.defaultVoiceProviders.host?.machineId == nativeA && model.defaultVoiceProviders.profiles.single().name == "Fixture profile A v8" })
            fail.set(true); tap("Refresh voice profiles")
            await("read failure explained") { text("Could not connect to this computer. Refresh voice profiles to try again.") != null }
            fail.set(false); tap("Refresh voice profiles")
            await("retry loads profiles") { main { !model.defaultVoiceProviders.loading && model.defaultVoiceProviders.profiles.isNotEmpty() } }
            main { set(model, "setRelayOperations", mapOf("fixture" to emptySet<String>())) }
            await("unsupported gateway explained") { text("Update this gateway to load voice profiles.") != null }
            main { set(model, "setRelayOperations", mapOf("fixture" to setOf("speech_profiles"))) }
            await("capability refresh recovers") { text("Update this gateway to load voice profiles.") == null &&
                main { !model.defaultVoiceProviders.loading && model.defaultVoiceProviders.profiles.isNotEmpty() } }
            choose("Choose voice profile", "Fixture profile A v8")
            await("recovered profile visible and selected") { text("Fixture profile A v8") != null && main { model.speech.selection.profileRevision == 8L } }
            check(main { model.speech.draft == saved && model.speech.outgoing.isEmpty() && model.speech.computers.isEmpty() && model.selected == null })
            check(mutations.get() == 0)
            uiAutomation.waitForIdle(200, 3_000)
            uiAutomation.takeScreenshot()?.let { bitmap ->
                File(targetContext.getExternalFilesDir(null), "o77o-global-providers.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }; bitmap.recycle()
            }
        } finally { held.get()?.countDown(); main { activity.finish() } }
    }
}
