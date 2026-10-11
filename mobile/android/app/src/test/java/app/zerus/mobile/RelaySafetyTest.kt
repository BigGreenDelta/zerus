package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import okhttp3.mockwebserver.SocketPolicy
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.TimeUnit

class RelaySafetyTest {
    @Test fun destinationIncludesGatewayMachineRunAndConversation() {
        val target = Target("computer", "session", "run", "conversation", "gateway")
        assertNotEquals(target.key, target.copy(connectionId = "other").key)
        assertNotEquals(target.key, target.copy(computerId = "other").key)
        assertNotEquals(target.key, target.copy(run = "next").key)
        assertNotEquals(target.key, target.copy(conversation = "next").key)
        assertEquals("run", target.json().getString("expected_run_id"))
        assertEquals("conversation", target.json().getString("expected_conversation_id"))
    }
    @Test fun processRestartNeverPromotesSubmittingDraftToResendable() {
        val draft = Draft(Target("computer", "session", "run", "conversation"), "Important message").begin()
        val recovered = draft.recover()
        assertEquals("uncertain", recovered.status)
        assertEquals(draft.requestId, recovered.requestId)
        assertEquals(draft.text, recovered.text)
        assertEquals("editing", draft.copy(status = "editing").recover().status)
    }
    @Test fun serverUrlRequiresHttpsAndRejectsCredentialAndQueryInjection() {
        assertEquals("https://gateway.example", EndpointPolicy.normalize(" https://gateway.example/ "))
        listOf("http://gateway.example", "https://user:secret@gateway.example", "https://gateway.example?token=private", "https://gateway.example#fragment").forEach {
            assertThrows(IllegalArgumentException::class.java) { EndpointPolicy.normalize(it) }
        }
    }
    @Test fun lostTransportMakesOnlyOneNativeMutationAttempt() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AFTER_REQUEST))
            server.start()
            val api = RelayApi(OkHttpClient.Builder().retryOnConnectionFailure(false).readTimeout(1, TimeUnit.SECONDS).build())
            val connection = Connection("gateway", "Test", server.url("/").toString().trimEnd('/'), "synthetic-token")
            val target = Target("machine", "native-session", "exact-run", "exact-conversation", "gateway")
            val payload = target.json().put("request_id", "request-one").put("text", "synthetic message")
            val result = runCatching { api.submit(connection, target, "send", payload, "request-one") }
            assertTrue(result.isFailure)
            assertEquals(1, server.requestCount)
            val captured = server.takeRequest()
            assertEquals("/v1/requests", captured.path)
            assertEquals("Bearer synthetic-token", captured.getHeader("Authorization"))
            val request = JSONObject(captured.body.readUtf8())
            assertEquals("machine", request.getString("computer_id"))
            assertEquals("exact-run", request.getJSONObject("payload").getString("expected_run_id"))
        }
    }
    @Test fun pollingKnownReceiptDoesNotCreateReplacementRequest() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("{\"request_id\":\"known\",\"state\":\"completed\",\"result\":{\"status\":\"submitted\"}}"))
            server.start()
            val connection = Connection("gateway", "Test", server.url("/").toString().trimEnd('/'), "token")
            val receipt = RelayApi().await(connection, JSONObject("{\"request_id\":\"known\",\"state\":\"claimed\"}"), 1)
            assertEquals("completed", receipt.getString("state"))
            val captured = server.takeRequest()
            assertEquals("GET", captured.method)
            assertEquals("/v1/requests/known", captured.path)
            assertEquals(1, server.requestCount)
        }
    }
    @Test fun oversizedResponseIsRejected() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("x".repeat(1024 * 1024 + 1)))
            server.start()
            val result = runCatching { RelayApi().call(server.url("/").toString().trimEnd('/'), "", "/v1/computers") }
            assertTrue(result.exceptionOrNull() is IllegalArgumentException)
        }
    }
    @Test fun nativeActivityRetainsUserPromptAndDeduplicatesStopExcerpt() {
        val raw = JSONObject("""{"events":[{"type":"UserPromptSubmit","at":1,"seq":1,"detail":"Please review"},{"type":"Stop","at":3,"seq":2,"detail":"Review complete"}],"provider_messages":[{"type":"AgentMessage","at":2,"message_id":"m1","detail":"Review complete with details","agent_id":"main"}]}""")
        val events = NativeParser.events(raw)
        assertEquals(2, events.size)
        assertEquals("You", events[0].role)
        assertEquals("Please review", events[0].text)
        assertEquals("Review complete with details", events[1].text)
    }
    @Test fun attentionPhaseOverridesBusyAndArchiveIsHidden() {
        val machine = JSONObject("""{"id":"machine","snapshot":{"sessions":[{"name":"one","agent":"codex","run_id":"r","conversation_id":"c","activity":"busy","phase":"approval"},{"name":"one","archive_id":"old","state":"archived"}]}}""")
        val sessions = NativeParser.sessions(Connection("gateway", "Test", "https://gateway.example", "token"), machine)
        assertEquals(1, sessions.size)
        assertEquals("approval", sessions[0].status)
    }
    @Test fun messagingUsesVerifiedNativeReadiness() {
        val raw = JSONObject("""{"tracked":true,"run_id":"r","conversation_id":"c","runtime_state":"live","process_state":"running","activity":"idle","phase":"idle"}""")
        assertEquals("", NativeParser.messageBlockReason(raw))
        assertTrue(NativeParser.messageBlockReason(JSONObject(raw.toString()).put("phase", "approval")).isNotBlank())
        assertTrue(NativeParser.messageBlockReason(JSONObject(raw.toString()).put("run_id", "")).isNotBlank())
        assertTrue(NativeParser.messageBlockReason(JSONObject(raw.toString()).put("runtime_state", "dead")).isNotBlank())
    }
    @Test fun mismatchedSubmissionReceiptNeverPollsAnotherRequest() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("{\"request_id\":\"foreign\",\"state\":\"queued\"}"))
            server.start()
            val connection = Connection("gateway", "Test", server.url("/").toString().trimEnd('/'), "synthetic")
            val target = Target("machine", "session", "run", "conversation", "gateway")
            val result = runCatching { RelayApi().submit(connection, target, "send", target.json().put("request_id", "own"), "own") }
            assertTrue(result.isFailure)
            assertEquals(1, server.requestCount)
        }
    }
    @Test fun wrongReceiptBindingStaysUnconfirmed() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("{\"request_id\":\"foreign\",\"state\":\"failed\"}"))
            server.start()
            val connection = Connection("gateway", "Test", server.url("/").toString().trimEnd('/'), "synthetic")
            assertTrue(runCatching { RelayApi().receipt(connection, "own") }.isFailure)
            assertEquals("/v1/requests/own", server.takeRequest().path)
        }
    }


    @Test fun markReadSubmitsExactSessionScopedPayload() = runBlocking {
        MockWebServer().use { server ->
            var envelope = JSONObject()
            server.dispatcher = object : Dispatcher() {
                override fun dispatch(request: RecordedRequest): MockResponse {
                    envelope = JSONObject(request.body.readUtf8())
                    return MockResponse().setBody(JSONObject().put("request_id", envelope.getString("request_id")).put("state", "completed")
                        .put("result", JSONObject().put("ok", true)).put("error", JSONObject.NULL).toString())
                }
            }
            server.start()
            val connection = Connection("gateway", "Test", server.url("/").toString().trimEnd('/'), "token")
            val target = Target("machine", "codex/project/review", "exact-run", "exact-conversation", "gateway")
            val payload = JSONObject().put("run_id", "exact-run").put("conversation_id", "exact-conversation")
                .put("reply_id", "7:100.5").put("attention_signature", JSONObject.NULL)
            assertEquals("completed", RelayApi().markRead(connection, target, payload).getString("state"))
            assertEquals("mark_read", envelope.getString("operation"))
            assertEquals("machine", envelope.getString("computer_id"))
            assertEquals("codex/project/review", envelope.getString("session"))
            val sent = envelope.getJSONObject("payload")
            assertEquals(4, sent.length())
            for (key in listOf("run_id", "conversation_id", "reply_id", "attention_signature")) assertEquals(payload.opt(key), sent.opt(key))
            assertTrue(runCatching { RelayApi().markRead(connection, target.copy(agentId = "child"), payload) }.isFailure)
        }
    }
}
