package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual private writer + native settings callbacks against an in-process synthetic gateway only. */
class ScheduledSettingsRetrySmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }; return value.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) { check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }; SystemClock.sleep(30) }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    override fun onStart() {
        val result = Bundle()
        try { smoke(); result.putString("stream", "Scheduled settings retry passed: acknowledged scheduled child + distinct fresh hidden-chooser NotReady/Ready creates one pinned attempt; repeated Ready and restored/check-receipt state grant no retry; drafts retained; synthetic settings only, zero other mutations.\n"); finish(Activity.RESULT_OK, result) }
        catch (error: Throwable) { result.putString("stream", "Scheduled settings retry failed: ${error.stackTraceToString()}\n"); finish(Activity.RESULT_CANCELED, result) }
    }
    private fun smoke() {
        check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
        val store = PrivateStore(targetContext)
        check(store.connections().isEmpty() && store.messageState() == MessageState())
        val target = Target("fixture-computer", "codex/fixture/settings", "fixture-run", "fixture-conversation", "fixture-workspace")
        val pending = "00000000-0000-4000-8000-000000000071"
        val source = SessionAction("00000000-0000-4000-8000-000000000072", target, "settings", JSONObject().put("model", "synthetic").toString(), status = "scheduled", pendingId = pending)
        val draft = Draft(target, "Keep this unsent composition", attachments = emptyList())
        store.saveMessageState(MessageState(drafts = listOf(draft), sessionActions = listOf(source)))
        val mode = AtomicInteger(0) // 0 ready, 1 hidden chooser (idle/idle but apply_when ready).
        val inspections = AtomicInteger()
        val settings = AtomicInteger()
        val unexpected = AtomicInteger()
        val bodies = java.util.Collections.synchronizedList(mutableListOf<JSONObject>())
        fun raw() = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("agent", "codex").put("activity", "idle").put("phase", "idle").put("tracked", true)
            .put("runtime_state", "live").put("process_state", "running").put("settings_change_supported", true)
            .put("model", "synthetic").put("effort", "high").put("pending_model", "synthetic").put("pending_effort", "high")
            .put("pending_settings_id", pending).put("settings_apply_when", if (mode.get() == 1) "ready" else "now")
            .put("model_options", JSONArray(listOf(JSONObject().put("id", "synthetic").put("effort_options", JSONArray(listOf("high"))))))
            .put("effort_options", JSONArray(listOf("high"))).put("events", JSONArray())
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request(); check(request.url.host == "relay.example.test")
            val response = if (request.method == "GET" && request.url.encodedPath == "/v1/capabilities") {
                JSONObject().put("operations", JSONArray(listOf("settings", "inspect")))
            } else if (request.method == "GET" && request.url.encodedPath == "/v1/computers") {
                val snapshot = JSONObject().put("sessions", JSONArray(listOf(raw()))).put("mobile_capabilities", JSONObject().put("protocol_version", 1).put("operations", JSONArray(listOf("settings", "inspect"))))
                JSONObject().put("computers", JSONArray(listOf(JSONObject().put("id", target.computerId).put("name", "Fixture").put("online", true).put("snapshot", snapshot))))
            } else if (request.method == "GET" && request.url.encodedPath.startsWith("/v1/requests/")) {
                val id = request.url.pathSegments.last()
                val action = bodies.firstOrNull { it.string("request_id") == id } ?: error("Unexpected receipt $id")
                JSONObject().put("request_id", id).put("state", "completed").put("result", JSONObject().put("request_id", id)
                    .put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation).put("status", "scheduled").put("apply_when", "ready")
                    .put("pending_settings_id", pending).put("model", action.getJSONObject("payload").string("model")).put("effort", "high"))
            } else {
                check(request.method == "POST" && request.url.encodedPath == "/v1/requests")
                val input = JSONObject(okio.Buffer().also { request.body!!.writeTo(it) }.readUtf8())
                check(input.string("computer_id") == target.computerId && input.string("session") == target.session)
                val native = when (input.string("operation")) {
                    "inspect" -> { inspections.incrementAndGet(); raw() }
                    "settings" -> {
                        val payload = input.getJSONObject("payload")
                        check(payload.string("request_id") == input.string("request_id") && payload.string("expected_run_id") == target.run && payload.string("expected_conversation_id") == target.conversation)
                        check(payload.string("expected_pending_id") == pending && payload.string("model") == "synthetic")
                        val count = settings.incrementAndGet(); check(count <= 2) { "Repeated settings attempt" }; bodies += JSONObject(input.toString())
                        if (count == 2) check(payload.opt("effort") is String && payload.string("effort") == "high") { "Resolved effort pin lost" }
                        JSONObject().put("request_id", input.string("request_id")).put("name", target.session).put("run_id", target.run)
                            .put("conversation_id", target.conversation).put("status", if (count == 1) "scheduled" else "applied")
                            .put("pending_settings_id", pending).put("model", "synthetic").put("effort", "high").put("apply_when", "ready")
                    }
                    else -> { unexpected.incrementAndGet(); error("Unexpected synthetic operation ${input.string("operation")}") }
                }
                JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", native)
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(response.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        fun start(): Pair<MainActivity, ZerusViewModel> {
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
            await("private storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture", true)))
                val session = Session(target, "Fixture", "codex", "idle", "fixture", "", 0, raw())
                set(model, "setSessions", listOf(session)); set(model, "setSelected", session)
                set(model, "setRelayOperations", mapOf(target.connectionId to setOf("inspect", "settings")))
                set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to setOf("inspect", "settings")))
            }
            return activity to model
        }
        fun refresh(model: ZerusViewModel) {
            val before = inspections.get(); main { model.refreshActivity(explicit = true) }
            await("fresh settings inspection") { inspections.get() > before && main { !model.detailBusy && model.activityVerified && model.activity?.string("settings_apply_when") == if (mode.get() == 1) "ready" else "now" } }
        }
        fun settled(model: ZerusViewModel) = main {
            @Suppress("UNCHECKED_CAST") val flights = model.javaClass.getDeclaredMethod("getActionFlights").apply { isAccessible = true }.invoke(model) as Set<String>
            flights.isEmpty() && model.sessionActions.none { it.blocksSending }
        }
        val (activity, model) = start()
        try {
            refresh(model)
            await("first scheduled child durably acknowledged") { settings.get() == 1 && settled(model) }
            repeat(2) { refresh(model); check(settings.get() == 1) { "Repeated Ready retried without NotReady" } }
            mode.set(1); refresh(model); check(settings.get() == 1)
            main { set(model, "setPreparingTargets", setOf(target.key)) }
            mode.set(0); refresh(model); check(settings.get() == 1) { "Preflight blocker was bypassed" }
            main { set(model, "setPreparingTargets", emptySet<String>()) }
            refresh(model); check(settings.get() == 1) { "Clearing a blocker reused the same Ready grant" }
            mode.set(1); refresh(model); mode.set(0); refresh(model)
            await("second pinned attempt after distinct NotReady/Ready") { settings.get() == 2 && settled(model) }
            check(main { model.sessionActions.first { it.requestId == source.requestId }.status == "applied" && model.draft(target).text == draft.text && model.outgoing.isEmpty() })
            check(bodies.map { it.string("request_id") }.distinct().size == 2)
            // Restore a prior scheduled relation without THIS-process submit/ack provenance.
            val firstId = bodies.first().string("request_id")
            main { model.javaClass.getDeclaredMethod("editState", kotlin.jvm.functions.Function1::class.java).apply { isAccessible = true }.invoke(model,
                { state: MessageState -> state.copy(sessionActions = listOf(source.copy(applyAttemptId = firstId), SessionAction(firstId, target, "settings", bodies.first().getJSONObject("payload").let { JSONObject().put("model", it.string("model")).put("expected_pending_id", pending).toString() }, status = "scheduled", pendingId = pending))) }) }
            main { model.flushDrafts() }
            await("restored scheduled proof durably saved") { store.messageState().sessionActions.any { it.requestId == source.requestId && it.applyAttemptId == firstId } && store.messageState().sessionActions.any { it.requestId == firstId && it.status == "scheduled" } }
            main { activity.finish() }; waitForIdleSync()
        } finally { main { activity.finish() } }
        val (restoredActivity, restored) = start()
        try {
            val child = main { restored.sessionActions.first { it.requestId != source.requestId } }
            main { restored.checkSessionAction(child) }; await("restored original receipt checked") { settled(restored) }
            mode.set(1); refresh(restored); mode.set(0); refresh(restored); refresh(restored)
            check(settings.get() == 2) { "Restored/check-receipt state created retry authority" }
            check(main { restored.draft(target).text == draft.text && restored.outgoing.isEmpty() })
            check(unexpected.get() == 0)
        } finally { main { restoredActivity.finish() } }
    }
}
