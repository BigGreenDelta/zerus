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

/** Actual read writer and adoption callback, using only owned synthetic state and no transport. */
class SharedReadAdoptionSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }; return result.get().getOrThrow()
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun adopt(model: ZerusViewModel) {
        model.javaClass.getDeclaredMethod("adoptSharedReads").apply { isAccessible = true }.invoke(model)
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.readState() == ConversationReadState())
            val target = Target("fixture-computer", "codex/fixture/selected", "fixture-run", "fixture-conversation", "fixture-workspace")
            val other = target.copy(session = "codex/fixture/other", conversation = "other-conversation")
            val initial = ConversationReadState(listOf(ConversationReadRecord(target, epoch = "epoch", readThrough = 1,
                headIncoming = 1, headTotal = 1, headComplete = true, established = true, reviewLater = true)))
            store.saveReadState(initial)
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
            val until = SystemClock.elapsedRealtime() + 15_000
            while (!main { model.storageReady && model.javaClass.getDeclaredMethod("getReadTrackingReady").apply { isAccessible = true }.invoke(model) == true }) {
                check(SystemClock.elapsedRealtime() < until) { "Private read writer not ready" }; SystemClock.sleep(40)
            }
            try {
                main {
                    val mutations = AtomicInteger()
                    val client = OkHttpClient.Builder().addInterceptor {
                        mutations.incrementAndGet(); error("No fixture transport is allowed")
                    }.build()
                    model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                    set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                    set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                    set(model, "setRelayOperations", mapOf(target.connectionId to setOf("history")))
                    set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to setOf("history")))
                    fun raw(reply: String) = JSONObject().put("run_id", target.run).put("conversation_id", target.conversation)
                        .put("reply_id", reply).put("attention_signature", "a".repeat(64))
                        .put("read", JSONObject().put("conversation_id", target.conversation).put("reply_id", "R1"))
                    fun row(raw: JSONObject) = Session(target, "Selected", "codex", "input", "fixture", "", 1, raw)
                    val fleet = row(raw("R1"))
                    set(model, "setSessions", listOf(fleet))
                    set(model, "setPublishedSessions", listOf(fleet))
                    set(model, "setSelected", row(raw("R2")))
                    set(model, "setActivity", raw("R2")); set(model, "setActivityVerified", true)
                    val observe = model.javaClass.getDeclaredMethod("observeHistoryHead", HistoryPage::class.java, Boolean::class.javaPrimitiveType).apply { isAccessible = true }
                    fun head(t: Target, sequence: Long) = HistoryPage(t, "epoch", emptyList(), null, "cursor", false, false,
                        sequence, sequence, true, false, false, JSONObject())
                    observe.invoke(model, head(target, 2), true)
                    @Suppress("UNCHECKED_CAST")
                    val hints = model.javaClass.getDeclaredField("historySnapshotHints").apply { isAccessible = true }.get(model) as MutableMap<String, String>
                    hints[ConversationReadPolicies.key(target)] = ReadAllHeadCandidates.hint(fleet)
                    // A verified head for a different conversation triggers the global adoption callback.
                    observe.invoke(model, head(other, 1), true)
                    adopt(model)
                    fun state() = model.javaClass.getDeclaredMethod("getReadState").apply { isAccessible = true }.invoke(model) as ConversationReadState
                    check(state().record(target)!!.readThrough == 1L) { "Stale shared R1 mark adopted unread selected R2" }
                    check(model.unreadCount(target) == 1 && model.reviewLater(target))
                    check(fleet.raw.string("attention_signature") == "a".repeat(64))
                    // Current matching reply marks still adopt, but stale head hints never do.
                    val current = row(raw("R2").put("read", JSONObject().put("conversation_id", target.conversation).put("reply_id", "R2")))
                    set(model, "setSessions", listOf(current)); set(model, "setPublishedSessions", listOf(current))
                    set(model, "setSelected", current); set(model, "setActivity", current.raw)
                    adopt(model); check(state().record(target)!!.readThrough == 1L) { "Stale head hint bypassed" }
                    hints[ConversationReadPolicies.key(target)] = ReadAllHeadCandidates.hint(current)
                    set(model, "setActivityVerified", false)
                    adopt(model); check(state().record(target)!!.readThrough == 1L) { "Unverified selected reply adopted" }
                    set(model, "setActivityVerified", true)
                    adopt(model); check(state().record(target)!!.readThrough == 2L) { "Same-reply shared read was lost" }
                    check(model.unreadCount(target) == 0)
                    check(mutations.get() == 0 && model.outgoing.isEmpty() && model.connections.size == 1)
                }
            } finally { main { activity.finish() } }
            result.putString("stream", "Shared-read adoption passed: R1 read -> selected unread R2 -> unrelated head; stale mark/head and unverified selection refused, same-reply adoption retained, no transport.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Shared-read adoption failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
}
