package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import okhttp3.OkHttpClient
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual ViewModel callbacks/read writer; owned synthetic state, no remote read mark or transport. */
class NonselectedSharedReadSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }; return result.get().getOrThrow()
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.readState() == ConversationReadState())
            val target = Target("fixture-computer", "codex/fixture/navigation", "fixture-run", "fixture-conversation", "fixture-workspace")
            val other = target.copy(session = "codex/fixture/nonselected", conversation = "other-conversation")
            val positive = target.copy(session = "codex/fixture/current", conversation = "current-conversation")
            val targets = listOf(target, other, positive)
            store.saveReadState(ConversationReadState(targets.map { ConversationReadRecord(it, epoch = "epoch", readThrough = 1,
                headIncoming = 1, headTotal = 1, headComplete = true, established = true, reviewLater = true) }))
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
            val transport = AtomicInteger()
            val until = SystemClock.elapsedRealtime() + 15_000
            while (!main { model.storageReady && model.javaClass.getDeclaredMethod("getReadTrackingReady").apply { isAccessible = true }.invoke(model) == true }) {
                check(SystemClock.elapsedRealtime() < until) { "Private read writer not ready" }; SystemClock.sleep(40)
            }
            try {
                main {
                    val api = RelayApi(OkHttpClient.Builder().addInterceptor {
                        transport.incrementAndGet(); error("No fixture transport is allowed")
                    }.build())
                    model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, api)
                    set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                    set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                    set(model, "setRelayOperations", mapOf(target.connectionId to setOf("history")))
                    set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to setOf("history")))
                    fun raw(t: Target, reply: String) = JSONObject().put("run_id", t.run).put("conversation_id", t.conversation)
                        .put("reply_id", reply).put("attention_signature", "a".repeat(64))
                        .put("read", JSONObject().put("conversation_id", t.conversation).put("reply_id", reply))
                    fun row(t: Target, reply: String) = Session(t, "Fixture", "codex", "idle", "fixture", "", 0, raw(t, reply))
                    val fleet = listOf(row(target, "R1"), row(other, "R1"), row(positive, "R2"))
                    set(model, "setSessions", fleet); set(model, "setPublishedSessions", fleet)
                    val selected = row(target, "R2")
                    set(model, "setSelected", selected); set(model, "setActivity", selected.raw); set(model, "setActivityVerified", true)
                    val observe = model.javaClass.getDeclaredMethod("observeHistoryHead", HistoryPage::class.java, Boolean::class.javaPrimitiveType).apply { isAccessible = true }
                    fun head(t: Target) = HistoryPage(t, "epoch", emptyList(), null, "cursor", false, false, 2, 2, true, false, false, JSONObject())
                    @Suppress("UNCHECKED_CAST")
                    val hints = model.javaClass.getDeclaredField("historySnapshotHints").apply { isAccessible = true }.get(model) as MutableMap<String, String>
                    fleet.forEach { hints[ConversationReadPolicies.key(it.target)] = ReadAllHeadCandidates.hint(it) }
                    val adopt = model.javaClass.getDeclaredMethod("adoptSharedReads").apply { isAccessible = true }
                    fun state() = model.javaClass.getDeclaredMethod("getReadState").apply { isAccessible = true }.invoke(model) as ConversationReadState
                    observe.invoke(model, head(target), true)
                    check(state().record(target)!!.readThrough == 1L) { "Selected R2 guard regressed" }
                    observe.invoke(model, head(other), true); adopt.invoke(model)
                    val failures = mutableListOf<String>()
                    if (state().record(other)!!.readThrough != 1L || !model.reviewLater(other)) failures += "nonselected R1 mark consumed canonical R2"
                    model.back(); adopt.invoke(model)
                    if (state().record(target)!!.readThrough != 1L || !model.reviewLater(target)) failures += "navigation away granted R1 authority over R2"
                    // Exact selected same-reply adoption stays available.
                    val current = fleet.last()
                    set(model, "setSelected", current); set(model, "setActivity", current.raw); set(model, "setActivityVerified", true)
                    observe.invoke(model, head(positive), true); adopt.invoke(model)
                    check(state().record(positive)!!.readThrough == 2L && !model.reviewLater(positive)) { "Selected same-reply adoption lost" }
                    // Explicit manual Read all retains its existing head/consent contract.
                    check(model.canMarkRead(target) && model.canMarkRead(other))
                    model.markAllRead(listOf(target, other))
                    check(listOf(target, other).all { state().record(it)!!.readThrough == 2L && !model.reviewLater(it) }) { "Explicit Read all changed" }
                    check(model.outgoing.isEmpty() && transport.get() == 0)
                    check(failures.isEmpty()) { failures.joinToString("; ") }
                }
            } finally { main { activity.finish() } }
            waitForIdleSync()
            check(transport.get() == 0)
            result.putString("stream", "Nonselected shared-read passed: canonical R2/stale fleet R1 and navigation-away refused; exact selected same-reply and explicit manual Read all retained; no transport/read-mark send.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Nonselected shared-read failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
}
