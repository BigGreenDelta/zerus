package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.RecordedRequest
import okhttp3.mockwebserver.SocketPolicy
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.TimeUnit

class OvermindOutboxTest {
    private val requestUuid = "33333333-3333-4333-8333-333333333333"
    private val request = requestUuid
    private val other = "44444444-4444-4444-8444-444444444444"
    private val image = Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "screen.png", "image/png", 4, "b".repeat(64))
    private val key = MachineKey("workspace", GATEWAY)
    private fun write(action: String = "create", body: JSONObject = JSONObject().put("project", "example").put("prompt", "Write the release notes"),
        files: List<Attachment> = emptyList(), id: String = request) = OvermindWrites.create(id, key, action, body, files, "Pixel 9", 1_000)
    private fun receipt(state: String, result: JSONObject? = null, error: String = "") = JSONObject().put("request_id", request).put("state", state)
        .put("result", result ?: JSONObject.NULL).put("error", error.ifEmpty { JSONObject.NULL })
    /** The private state and every reduction the outbox committed, in order. */
    private class Store { var state = MessageState(); val commits = mutableListOf<MessageState>(); var connected = true; var saves = Int.MAX_VALUE
        val durable: suspend ((MessageState) -> MessageState) -> MessageState = { reduce ->
            check(saves-- > 0) { "synthetic storage failure" }; reduce(state).also { state = it; commits += it } } }
    private var missingFiles = false
    private fun outbox(store: Store) = OvermindOutbox(OvermindRepository(RelayApi(OkHttpClient.Builder().retryOnConnectionFailure(false)
        .readTimeout(2, TimeUnit.SECONDS).build()), pollDelayMs = 50), { files -> if (missingFiles) throw AttachmentException("Saved attachment is missing or changed. Select the file again.")
            JSONArray(files.map { JSONObject().put("name", it.name).put("mime", it.mime).put("data_base64", "AAAA") }) }, { store.connected }, store.durable)

    @Test fun createUsesTheRequestUuidAsTheTaskAndKeepsOnlyItsRequest() {
        val created = write(body = JSONObject().put("project", "example").put("prompt", "Write it").put("request_id", other))
        assertEquals(request, created.task)
        assertEquals("sending", created.status)
        assertFalse(JSONObject(created.request).has("request_id"))
        assertEquals(other, write("move", JSONObject().put("task", other).put("to", "Done")).task)
        assertThrows(IllegalArgumentException::class.java) { write("advance", JSONObject().put("task", other)) }
        assertThrows(IllegalArgumentException::class.java) { write(id = "not-a-uuid") }
        assertThrows(IllegalArgumentException::class.java) { write("comment", JSONObject().put("task", other).put("text", "x".repeat(200 * 1024))) }
        assertThrows(IllegalArgumentException::class.java) { write("comment", JSONObject().put("task", other).put("text", "x"), listOf(image)) }
        assertThrows(AttachmentException::class.java) { write(files = List(9) { image.copy(id = "aaaaaaaa-aaaa-4aaa-8aaa-%012d".format(it)) }) }
    }

    @Test fun writeIsPersistedBeforeTheNetworkAndSettledFromTheReceipt() = runBlocking<Unit> {
        val store = Store()
        val relay = object : Dispatcher() {
            val bodies = mutableListOf<JSONObject>()
            override fun dispatch(request: RecordedRequest): MockResponse {
                val envelope = JSONObject(request.body.readUtf8()); bodies += envelope
                // The relay sees the write only after the outbox committed it as sending.
                assertEquals("sending", store.state.overmindWrites.single().status)
                return MockResponse().setBody(receipt("completed", ok().put("task", card(requestUuid, "Todo", "a"))).toString())
            }
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            val settled = outbox(store).submit(gateway, write(files = listOf(image)))
            assertEquals("completed", settled.status)
            assertEquals(request, JSONObject(settled.result).getString("uuid"))
            assertEquals(listOf("sending", "completed"), store.commits.map { it.overmindWrites.single().status })
            val envelope = relay.bodies.single()
            assertEquals("overmind_write" to "", envelope.getString("operation") to envelope.getString("session"))
            val payload = envelope.getJSONObject("payload")
            assertEquals(setOf("request_id", "action", "request", "device"), payload.keys().asSequence().toSet())
            assertEquals(request, payload.getString("request_id"))
            assertEquals(request, envelope.getString("request_id"))
            assertEquals("Pixel 9", payload.getString("device"))
            assertEquals("screen.png", payload.getJSONObject("request").getJSONArray("attachments").getJSONObject(0).getString("name"))
            assertFalse(payload.getJSONObject("request").has("request_id"))
        } }
    }

    @Test fun aLostConnectionLeavesTheWriteUncertainAndAnExplicitRetryReusesItsIdentity() = runBlocking<Unit> {
        val store = Store()
        val relay = object : Dispatcher() {
            val bodies = mutableListOf<String>()
            override fun dispatch(request: RecordedRequest): MockResponse {
                bodies += request.body.readUtf8()
                return if (bodies.size == 1) MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AFTER_REQUEST)
                // The relay already ran the first attempt: the same identity returns its first receipt.
                else MockResponse().setBody(receipt("completed", ok().put("task", card(requestUuid, "Todo", "a"))).toString())
            }
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            val box = outbox(store)
            val lost = box.submit(gateway, write())
            assertEquals("uncertain", lost.status)
            assertTrue(lost.retryable)
            assertEquals(1, server.requestCount)
            assertEquals("uncertain", store.state.overmindWrites.single().status)
            // A restart never resends; it only recovers what was in flight.
            val restarted = MessageCodec.state(JSONObject(MessageCodec.stateJson(store.state.copy(overmindWrites = listOf(lost.copy(status = "sending")))).toString()))
            assertEquals("uncertain", restarted.overmindWrites.single().status)
            assertEquals(1, server.requestCount)
            val retried = box.retry(gateway, restarted.overmindWrites.single())
            assertEquals("completed", retried.status)
            assertEquals(2, server.requestCount)
            assertEquals(JSONObject(relay.bodies[0]).toString(), JSONObject(relay.bodies[1]).toString())
            assertEquals(request, JSONObject(relay.bodies[1]).getString("request_id"))
            assertThrows(IllegalArgumentException::class.java) { runBlocking { box.retry(gateway, retried) } }
            assertEquals(2, server.requestCount)
        } }
    }

    @Test fun aRejectedSubmissionFailsAndIsNotRepeated() = runBlocking<Unit> {
        val store = Store()
        val relay = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest) = MockResponse().setResponseCode(409).setBody("{\"error\":\"request identity or route conflicts\"}")
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            val failed = outbox(store).submit(gateway, write())
            assertEquals("failed", failed.status)
            assertFalse(failed.retryable)
            assertEquals(1, server.requestCount)
            assertThrows(IllegalArgumentException::class.java) { runBlocking { outbox(store).submit(gateway, write()) } }
            assertEquals(1, server.requestCount)
        } }
    }

    @Test fun receiptsSettleRefusalsFailuresAndUncertainDeliveries() {
        val send = write("send", JSONObject().put("task", other).put("session", JSONObject().put("name", "codex/example/worker")))
        val sent = OvermindWrites.settle(send, receipt("completed", ok().put("delivery", "sent").put("reason", "").put("task", card(other, "Doing", "a"))))
        assertEquals("completed" to "sent", sent.status to sent.delivery)
        assertFalse(sent.needsResolution)
        val notSent = OvermindWrites.settle(send, receipt("completed", ok().put("delivery", "not_sent").put("reason", "the session is busy")))
        assertEquals("completed" to "the session is busy", notSent.status to notSent.error)
        val uncertain = OvermindWrites.settle(send, receipt("completed", ok().put("delivery", "uncertain").put("reason", "lost").put("task", card(other, "Doing", "a"))))
        assertEquals("uncertain", uncertain.status)
        assertTrue(uncertain.needsResolution)
        assertFalse(uncertain.retryable)
        assertEquals(other, uncertain.task)
        val unknown = OvermindWrites.settle(send, receipt("completed", JSONObject().put("ok", false).put("error", JSONObject().put("code", "delivery_unknown")
            .put("message", "the session's machine did not report the delivery").put("field", "").put("state", ""))))
        assertEquals("uncertain" to "delivery_unknown", unknown.status to unknown.code)
        assertTrue(unknown.needsResolution)
        assertEquals("the session's machine did not report the delivery", unknown.error)
        val refused = OvermindWrites.settle(send, receipt("completed", JSONObject().put("ok", false).put("error", JSONObject().put("code", "session_busy")
            .put("message", "the session is working").put("field", "confirm").put("state", "missing"))))
        assertEquals(listOf("refused", "session_busy", "confirm"), listOf(refused.status, refused.code, refused.field))
        assertFalse(refused.retryable)
        // Outside the delivering writes the same code is an ordinary refusal.
        val comment = write("comment", JSONObject().put("task", other).put("text", "Looks good"))
        assertEquals("refused", OvermindWrites.settle(comment, receipt("completed", JSONObject().put("ok", false).put("error", JSONObject().put("code", "delivery_unknown")))).status)
        assertEquals("failed", OvermindWrites.settle(send, receipt("failed", error = "validation failed")).status)
        // Still queued or claimed, or another identity: the same request can be sent again safely.
        listOf(receipt("claimed"), receipt("queued"), receipt("completed", ok()).put("request_id", other)).forEach {
            val settled = OvermindWrites.settle(send, it)
            assertEquals(it.toString(), "uncertain", settled.status)
            assertTrue(settled.retryable)
            assertFalse(settled.needsCheck)
        }
        // A terminal receipt without a result: the relay will not run it again, so the person checks the task.
        listOf(receipt("uncertain"), receipt("completed", JSONObject().put("ok", "yes")), receipt("completed"),
            receipt("completed", error = "request history expired; delivery must not be retried")).forEach {
            val settled = OvermindWrites.settle(send, it)
            assertEquals(it.toString(), "uncertain", settled.status)
            assertTrue(settled.needsCheck)
            assertFalse(settled.retryable)
            assertFalse(settled.needsResolution)
            assertEquals(other, settled.task)
            assertEquals(OvermindWrites.CHECK_TASK, settled.error)
        }
        assertEquals(request, OvermindWrites.settle(write(), receipt("uncertain")).task)
        val deleted = OvermindWrites.settle(write("delete", JSONObject().put("task", "#12")), receipt("completed", ok().put("deleted", other)))
        assertEquals("completed" to other, deleted.status to deleted.task)
    }

    @Test fun outboxSurvivesItsCodecAndRetainsPendingWritesBeforeOldResults() {
        var state = MessageState()
        repeat(30) { index -> state = state.overmind(write(id = "55555555-5555-4555-8555-%012d".format(index)).copy(status = "completed")) }
        state = state.overmind(write(files = listOf(image)).copy(status = "uncertain", error = "unconfirmed", code = "x", field = "y", delivery = "uncertain"))
        repeat(30) { index -> state = state.overmind(write(id = "66666666-6666-4666-8666-%012d".format(index)).copy(status = "refused")) }
        assertEquals(21, state.overmindWrites.size)
        assertTrue(state.overmindWrites.any { it.requestId == request })
        val restored = MessageCodec.state(JSONObject(MessageCodec.stateJson(state).toString()))
        assertEquals(state.overmindWrites, restored.overmindWrites)
        val reviewed = OvermindWrites.review(state, request)
        assertEquals("reviewed", reviewed.overmindWrites.single { it.requestId == request }.status)
        assertEquals(reviewed, OvermindWrites.result(reviewed, request) { it.copy(status = "completed") })
        val last = state.overmindWrites.last().requestId
        assertEquals("reviewed", OvermindWrites.review(reviewed, last).overmindWrites.single { it.requestId == last }.status)
        assertEquals("Wait for the current message or session command to finish before installing.",
            UpdateInstallGuard.reason(MessageState(overmindWrites = listOf(write())), true, false, false, false, false))
        assertEquals("", UpdateInstallGuard.reason(MessageState(overmindWrites = listOf(write().copy(status = "uncertain"))), true, false, false, false, false))
        // A saved image stays protected while any Overmind write still names it.
        assertTrue(image.id in MessageState(overmindWrites = listOf(write(files = listOf(image)))).attachmentIds())
        assertTrue(image.id in MessageState(drafts = listOf(Draft(Target("c", "s", "r", "v"), "", attachments = listOf(image)))).attachmentIds())
    }

    @Test fun aRetryThatMayNotHaveReachedTheRelayStaysUnconfirmed() = runBlocking<Unit> {
        var status = 401
        val relay = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest) = MockResponse().setResponseCode(status).setBody("{\"error\":\"synthetic\"}")
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            fun lost() = write(files = listOf(image)).copy(status = "uncertain")
            fun holding() = Store().apply { state = state.overmind(lost()) }
            // A first submission that never left the phone did not happen.
            missingFiles = true
            assertEquals("failed", outbox(Store()).submit(gateway, write(files = listOf(image))).status)
            // A retry's original may already be recorded: no attempt, or a refused credential, proves nothing.
            val noAttempt = outbox(holding()).retry(gateway, lost())
            assertEquals("uncertain" to true, noAttempt.status to noAttempt.retryable)
            assertEquals(0, server.requestCount)
            missingFiles = false
            // Rate limiting, capacity, authentication and body reading run before the relay's identity lookup.
            for (code in listOf(400, 401, 403, 408, 413, 429, 500, 503)) {
                status = code
                val retried = outbox(holding()).retry(gateway, lost())
                assertEquals("$code", "uncertain" to true, retried.status to retried.retryable)
            }
            // 404 and 409 come only after the lookup, so the relay never recorded this request_id.
            for (code in listOf(404, 409)) {
                status = code
                assertEquals("$code", "failed", outbox(holding()).retry(gateway, lost()).status)
            }
            // A first submission refused by the relay was not recorded either.
            status = 429
            assertEquals("failed", outbox(Store()).submit(gateway, write(id = other)).status)
            status = 503
            assertEquals("uncertain", outbox(Store()).submit(gateway, write(id = other)).status)
            assertEquals(12, server.requestCount)
        } }
    }

    @Test fun aReceiptThatCannotBeReadAfterTheRelayAcceptedTheWriteLeavesItUnconfirmed() = runBlocking<Unit> {
        var status = 429
        val relay = object : Dispatcher() {
            val posts = mutableListOf<String>()
            override fun dispatch(request: RecordedRequest): MockResponse = if (request.method == "POST") {
                posts += JSONObject(request.body.readUtf8()).getString("request_id")
                MockResponse().setResponseCode(202).setBody(JSONObject().put("request_id", posts.last()).put("state", "queued").toString())
            // Polling its receipt: rate limited, a lapsed credential or a receipt the relay no longer finds.
            } else MockResponse().setResponseCode(status).setBody("{\"error\":\"synthetic\"}")
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            for (code in listOf(429, 401, 403, 404)) {
                status = code
                val before = relay.posts.size
                val store = Store()
                val accepted = outbox(store).submit(gateway, write())
                assertEquals("$code", "uncertain" to true, accepted.status to accepted.retryable)
                assertEquals("$code", OvermindWrites.UNCONFIRMED, accepted.error)
                assertEquals("$code", "uncertain", store.state.overmindWrites.single().status)
                assertEquals("$code", listOf(request), relay.posts.drop(before))
                // A retry the relay accepts again is unconfirmed too, never failed.
                val retried = outbox(store).retry(gateway, accepted)
                assertEquals("$code", "uncertain" to true, retried.status to retried.retryable)
            }
            assertEquals(8, relay.posts.size)
            assertEquals(16, server.requestCount)
        } }
    }

    @Test fun aDisconnectedWorkspaceNeverGetsItsWritesBack() = runBlocking<Unit> {
        val store = Store()
        val relay = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse {
                // The workspace is disconnected while the write is in flight.
                store.connected = false
                store.state = store.state.copy(overmindWrites = emptyList())
                return MockResponse().setBody(receipt("completed", ok().put("task", card(requestUuid, "Todo", "a"))).toString())
            }
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            outbox(store).submit(gateway, write())
            assertTrue(store.state.overmindWrites.isEmpty())
            assertThrows(IllegalArgumentException::class.java) { runBlocking { outbox(store).submit(gateway, write(id = other)) } }
            assertTrue(store.state.overmindWrites.isEmpty())
            assertEquals(1, server.requestCount)
        } }
    }

    @Test fun aResultThatCouldNotBeSavedIsReportedAsPossiblyApplied() = runBlocking<Unit> {
        val relay = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest) = MockResponse().setBody(receipt("completed", ok()).toString())
        }
        overmindServer(relay) { server, gateway -> runBlocking {
            val store = Store().apply { saves = 1 }
            val error = runCatching { outbox(store).submit(gateway, write()) }.exceptionOrNull()
            assertTrue(error is OvermindUnsavedResult)
            assertTrue(error!!.message!!.contains("may have been applied"))
            assertEquals(1, server.requestCount)
            assertEquals("sending", store.state.overmindWrites.single().status)
            val unsaved = runCatching { outbox(Store().apply { saves = 0 }).submit(gateway, write(id = other)) }.exceptionOrNull()
            assertTrue(unsaved != null && unsaved !is OvermindUnsavedResult)
            assertEquals(1, server.requestCount)
        } }
    }
}
