package app.zerus.mobile

import android.app.Activity
import android.app.Application
import android.app.Instrumentation
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual ViewModels and private writers on a fresh owned emulator; no Activity, service or live gateway. */
class RevokedWorkspaceSmokeInstrumentation : Instrumentation() {
    private var selectedCase = "all"
    private val application get() = targetContext.applicationContext as Application
    private val retainedTarget = Target("fixture-computer", "codex/fixture/retained", "fixture-run", "fixture-conversation", "fixture-retained-workspace")
    private val retainedMessages = MessageState(
        drafts = listOf(Draft(retainedTarget, "Keep this original composer *exactly*", updatedAt = 1,
            revision = "fixture-composer-revision", generation = "fixture-composer-generation"),
            Draft(retainedTarget, "Original answer", questionId = "fixture-question", questionHash = "fixture-hash",
                answers = "[{\"question_id\":\"fixture-prompt\",\"selected_option_ids\":[],\"text\":\"Original answer\"}]",
                updatedAt = 1, revision = "fixture-answer-revision", generation = "fixture-answer-generation")),
        outgoing = listOf(OutgoingMessage("fixture-outgoing", retainedTarget, "Original delivery", emptyList(), 1.0,
            status = "recorded", submittedAt = 1.0, submittedText = "Original delivery")),
        contextOperations = listOf(ContextOperation("fixture-context", retainedTarget, "compact_context", status = "completed",
            createdAt = 1, error = "Keep the original context receipt")),
        sessionActions = listOf(SessionAction("fixture-action", retainedTarget, "rename", "{\"new_name\":\"codex/fixture/retained\"}",
            status = "completed", createdAt = 1, error = "Keep the original action receipt")))
    private val retainedReads = ConversationReadState(listOf(ConversationReadRecord(retainedTarget,
        seen = listOf("fixture-seen"), epoch = "fixture-epoch", readThrough = 7, headIncoming = 8, headTotal = 8,
        headComplete = true, established = true, reviewLater = true)))

    override fun onCreate(arguments: Bundle?) {
        selectedCase = arguments?.getString("case") ?: "all"
        super.onCreate(arguments); start()
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && !BuildConfig.FIREBASE_ENABLED && Build.HARDWARE in setOf("ranchu", "goldfish"))
            check(!LiveConnectionService.running)
            val store = PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState()) {
                "Use fresh owned emulator app data; existing data is never cleared."
            }
            store.saveMessageState(retainedMessages); store.saveReadState(retainedReads)
            val cases = linkedMapOf<String, (Fixture) -> Unit>(
                "owned-clear" to { ownedClear(it, 200) },
                "already-revoked" to { ownedClear(it, 401) },
                "late-read" to ::lateRead,
                "mutation-error" to { unownedError(it, "Delivery is unconfirmed. Check its original receipt; nothing has been retried.") },
                "storage-error" to { unownedError(it, "Could not save private changes. Your text remains available here; a message is sent only after its outgoing record is saved.") },
                "other-workspace" to ::otherWorkspace,
                "revoke-refused" to ::revokeRefused)
            require(selectedCase == "all" || selectedCase in cases) { "Unknown synthetic case" }
            val passed = mutableListOf<String>(); val failures = mutableListOf<String>()
            for ((name, test) in cases.filterKeys { selectedCase == "all" || it == selectedCase }) {
                val fixture = Fixture()
                try { fixture.start(); test(fixture); fixture.assertPreserved(); passed += name }
                catch (error: Throwable) { failures += "$name: ${error.stackTraceToString()}" }
                finally { fixture.close() }
            }
            check(failures.isEmpty()) { failures.joinToString("\n") }
            result.putString("stream", "Revoked workspace ViewModel checks passed: ${passed.joinToString()}; actual pair/revoke/catalog/detail callbacks and private writers, original drafts/outbox/context/action receipts and other-workspace reads retained. Synthetic pair/revoke only; zero send/read-mark/provider/service operations. No Activity or UI rendering claim.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Revoked workspace ViewModel checks failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }
        return value.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 10_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }
            SystemClock.sleep(30)
        }
    }
    private fun errorOwner(model: ZerusViewModel) = main {
        model.javaClass.getDeclaredField("catalogErrorOwner").apply { isAccessible = true }.get(model) as String?
    }
    private fun replaceError(model: ZerusViewModel, value: String) = main {
        // Use the production setter: unowned storage/mutation errors remove the old read ownership.
        model.javaClass.getDeclaredMethod("setError", String::class.java).apply { isAccessible = true }.invoke(model, value)
    }
    private fun flushPrivate() = runBlocking {
        withTimeout(10_000) {
            MessagePersistence.get(application).open().flush()
            ConversationReadPersistence.get(application).open().flush()
            AccountsPersistence.get(application).open().flush()
        }
    }
    private inner class Fixture {
        val gateway = Gateway()
        private val models = ViewModelStore()
        private val ownedConnections = mutableSetOf<String>()
        val model = main {
            ViewModelProvider(models, ViewModelProvider.AndroidViewModelFactory.getInstance(application))[ZerusViewModel::class.java].also {
                it.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(it, RelayApi(gateway.client))
            }
        }
        fun start() {
            await("private storage") { main {
                model.storageReady && model.javaClass.getDeclaredMethod("getReadTrackingReady").apply { isAccessible = true }.invoke(model) == true
            } }
            check(main { model.connections.isEmpty() })
        }
        fun pair(): Connection {
            val before = main { model.connections.map { it.id }.toSet() }
            val done = AtomicInteger()
            main { model.pair("https://relay.example.test", "fixture-invitation") { done.incrementAndGet() } }
            await("synthetic pairing saved") { done.get() == 1 && main { !model.busy } }
            val value = main { model.connections.single { it.id !in before } }
            ownedConnections += value.id
            check(PrivateStore(targetContext).connections().contains(value))
            return value
        }
        fun fresh(connection: Connection) {
            await("fresh credential catalog") { main {
                !model.catalogBusy && model.machines.any { it.connectionId == connection.id && it.online && !it.lastKnown } &&
                    model.sessions.any { it.target.connectionId == connection.id }
            } }
        }
        fun rejectedRead(connection: Connection): String {
            fresh(connection); gateway.rejected += connection.token
            main { model.back(); model.refresh(explicit = true) }
            await("credential-owned catalog refusal") { main { !model.catalogBusy && model.error.contains("Synthetic credential unauthorized") } && errorOwner(model) == connection.id }
            return main { model.error }
        }
        fun disconnect(connection: Connection) {
            main { model.disconnect(connection) }
            await("disconnect settled") { main { !model.busy } }
        }
        fun assertPreserved() {
            flushPrivate()
            val store = PrivateStore(targetContext)
            check(store.messageState().copy(conversationIndex = emptyList()) == retainedMessages) { "Original composition or mutation receipt changed" }
            check(store.readState().records.filter { it.target.connectionId == retainedTarget.connectionId } == retainedReads.records) { "Other workspace read identities changed" }
            check(main { model.drafts == retainedMessages.drafts && model.outgoing == retainedMessages.outgoing &&
                model.contextOperations == retainedMessages.contextOperations && model.sessionActions == retainedMessages.sessionActions })
            check(gateway.unexpected.get() == 0 && gateway.sends.get() == 0 && gateway.readMarks.get() == 0)
            check(!LiveConnectionService.running)
        }
        fun close() {
            gateway.release.countDown()
            val job = main { model.viewModelScope.coroutineContext[Job] }
            main { models.clear() }
            gateway.client.dispatcher.cancelAll()
            runBlocking { withTimeout(10_000) { job?.join() } }
            flushPrivate()
            val store = PrivateStore(targetContext)
            check(store.connections().all { it.id in ownedConnections }) { "Refusing to clean a connection not created by this fixture" }
            // Only this fixture's synthetic connections are removed between independent ViewModels.
            store.saveConnections(emptyList())
            gateway.client.connectionPool.evictAll()
            gateway.client.dispatcher.executorService.shutdown()
        }
    }
    private inner class Gateway {
        val rejected = ConcurrentHashMap.newKeySet<String>()
        val holdToken = AtomicReference<String?>(null)
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val revokeStatus = AtomicInteger(200)
        val unexpected = AtomicInteger()
        val sends = AtomicInteger()
        val readMarks = AtomicInteger()
        private val pairs = AtomicInteger()
        private val issued = ConcurrentHashMap.newKeySet<String>()
        private val calls = AtomicInteger()
        val details = AtomicInteger()
        private fun raw() = JSONObject().put("name", "codex/fixture/ready").put("run_id", "fixture-run")
            .put("conversation_id", "fixture-ready-conversation").put("agent", "codex").put("activity", "idle").put("phase", "idle")
            .put("state", "running").put("tracked", true).put("runtime_state", "live").put("process_state", "running")
            .put("events", JSONArray())
        fun session(connection: Connection) = Session(Target("fixture-computer", "codex/fixture/ready", "fixture-run",
            "fixture-ready-conversation", connection.id), "Fixture ready", "codex", "idle", "fixture", "", 0, raw())
        val client: OkHttpClient = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            check(request.url.host == "relay.example.test" && calls.incrementAndGet() <= 64)
            val path = request.url.encodedPath
            val token = request.header("Authorization").orEmpty().removePrefix("Bearer ")
            if (!(request.method == "POST" && path == "/v1/pair")) check(token in issued) { "Unknown synthetic credential" }
            var status = 200
            val response = when {
                request.method == "POST" && path == "/v1/pair" -> {
                    val body = JSONObject(okio.Buffer().also { request.body!!.writeTo(it) }.readUtf8())
                    check(body.string("code") == "fixture-invitation")
                    val created = "fixture-token-${pairs.incrementAndGet()}"
                    issued += created
                    JSONObject().put("workspace_name", "Fixture workspace").put("device_token", created)
                }
                request.method == "DELETE" && path == "/v1/device" -> {
                    status = revokeStatus.get()
                    if (status in setOf(200, 401)) rejected += token
                    JSONObject().put("error", "Synthetic revocation status $status")
                }
                request.method == "GET" && path == "/v1/capabilities" ->
                    JSONObject().put("operations", JSONArray(listOf("inspect"))).put("features", JSONArray())
                request.method == "GET" && path == "/v1/computers" -> {
                    if (holdToken.get() == token) {
                        entered.countDown(); check(release.await(10, TimeUnit.SECONDS)) { "Held synthetic read was not released" }
                    }
                    if (token in rejected) {
                        status = 401; JSONObject().put("error", "Synthetic credential unauthorized")
                    } else JSONObject().put("computers", JSONArray(listOf(JSONObject().put("id", "fixture-computer")
                        .put("name", "Fixture computer").put("online", true).put("snapshot", JSONObject()
                            .put("sessions", JSONArray(listOf(raw()))).put("mobile_capabilities", JSONObject()
                                .put("protocol_version", 1).put("operations", JSONArray(listOf("inspect"))).put("features", JSONArray()))))))
                }
                request.method == "POST" && path == "/v1/requests" -> {
                    val body = JSONObject(okio.Buffer().also { request.body!!.writeTo(it) }.readUtf8())
                    when (body.string("operation")) {
                        "inspect" -> {
                            check(body.string("computer_id") == "fixture-computer" && body.string("session") == "codex/fixture/ready")
                            if (token in rejected) {
                                status = 401; JSONObject().put("error", "Synthetic credential unauthorized")
                            } else {
                                details.incrementAndGet()
                                JSONObject().put("request_id", body.string("request_id")).put("state", "completed").put("result", raw())
                            }
                        }
                        else -> {
                            if (body.string("operation") in setOf("send", "answer", "voice_submit")) sends.incrementAndGet()
                            if (body.string("operation") == "mark_read") readMarks.incrementAndGet()
                            unexpected.incrementAndGet(); throw IOException("Unexpected synthetic operation")
                        }
                    }
                }
                else -> { unexpected.incrementAndGet(); throw IOException("Unexpected synthetic request") }
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(status).message("Synthetic")
                .body(response.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
    }
    private fun ownedClear(fixture: Fixture, revokeStatus: Int) {
        val first = fixture.pair(); fixture.rejectedRead(first)
        fixture.gateway.revokeStatus.set(revokeStatus)
        fixture.disconnect(first)
        check(main { fixture.model.connections.none { it.id == first.id } } && PrivateStore(targetContext).connections().none { it.id == first.id })
        check(main { fixture.model.error.isEmpty() } && errorOwner(fixture.model) == null) { "Disconnected credential kept its owned catalog banner" }
        val next = fixture.pair(); check(next.id != first.id && next.url == first.url && next.label == first.label && next.token != first.token)
        fixture.fresh(next)
        check(main { fixture.model.error.isEmpty() }) { "Fresh re-pair retained the revoked credential's banner" }
    }
    private fun lateRead(fixture: Fixture) {
        val first = fixture.pair(); fixture.fresh(first)
        fixture.gateway.holdToken.set(first.token); fixture.gateway.rejected += first.token
        main { fixture.model.refresh(explicit = true) }
        check(fixture.gateway.entered.await(10, TimeUnit.SECONDS)) { "Old catalog request was not held" }
        fixture.disconnect(first)
        val next = fixture.pair(); check(next.id != first.id && next.url == first.url)
        // The old catalog still owns LoadingGate. Actual B detail can finish independently.
        val session = fixture.gateway.session(next)
        main { fixture.model.open(session) }
        await("fresh re-paired detail") { main { fixture.model.activityVerified && !fixture.model.detailBusy && fixture.model.selected?.target == session.target } }
        check(fixture.gateway.details.get() > 0)
        main { fixture.model.back() }
        check(main { fixture.model.selected == null })
        fixture.gateway.release.countDown()
        fixture.fresh(next)
        check(main { fixture.model.error.isEmpty() } && errorOwner(fixture.model) == null) { "Late disconnected credential restored its catalog banner after fresh detail/navigation" }
    }
    private fun unownedError(fixture: Fixture, expected: String) {
        val first = fixture.pair(); fixture.rejectedRead(first)
        replaceError(fixture.model, expected); check(errorOwner(fixture.model) == null)
        fixture.disconnect(first)
        check(main { fixture.model.error == expected }) { "Disconnect erased an unrelated error" }
        val next = fixture.pair(); fixture.fresh(next)
        check(main { fixture.model.error == expected }) { "Fresh catalog erased an unrelated error" }
    }
    private fun otherWorkspace(fixture: Fixture) {
        val removed = fixture.pair(); fixture.fresh(removed)
        val other = fixture.pair(); fixture.fresh(other)
        val expected = fixture.rejectedRead(other)
        fixture.disconnect(removed)
        check(main { fixture.model.connections == listOf(other) && fixture.model.error == expected } && errorOwner(fixture.model) == other.id) {
            "Removing another credential erased the current workspace's read error"
        }
    }
    private fun revokeRefused(fixture: Fixture) {
        val first = fixture.pair(); fixture.rejectedRead(first)
        fixture.gateway.revokeStatus.set(403)
        fixture.disconnect(first)
        check(main { fixture.model.connections == listOf(first) && fixture.model.error.contains("connection is preserved") })
        check(PrivateStore(targetContext).connections() == listOf(first)) { "Failed revocation removed the saved connection" }
    }
}
