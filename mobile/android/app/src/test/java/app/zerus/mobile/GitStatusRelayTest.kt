package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.SocketPolicy
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.util.concurrent.TimeUnit

class GitStatusRelayTest {
    private val request = GitStatusRequest("123e4567-e89b-42d3-a456-426614174000",
        GitStatusKey(MachineKey("fixture-workspace", "fixture-computer"), "/fixture/work tree/repo/../project"),
        0, 0, GitCheckoutEvidence("/fixture/canonical-root", "fixture-branch", false, "fixture-head"))

    private fun envelope(state: String, id: Any = request.requestId) = JSONObject().put("request_id", id)
        .put("state", state).put("result", if (state == "completed") JSONObject().put("opaque_result", "unchanged") else JSONObject.NULL)
        .put("error", JSONObject.NULL).put("opaque_envelope", JSONObject().put("retained", true))

    private fun assertEnvelope(expected: JSONObject, actual: JSONObject) {
        assertEquals(expected.keys().asSequence().toSet(), actual.keys().asSequence().toSet())
        assertEquals(expected.getString("request_id"), actual.opt("request_id"))
        assertEquals(expected.getString("state"), actual.opt("state"))
        assertEquals(JSONObject.NULL, actual.opt("error"))
        val opaque = actual.getJSONObject("opaque_envelope")
        assertEquals(setOf("retained"), opaque.keys().asSequence().toSet())
        assertEquals(true, opaque.opt("retained"))
        if (expected.getString("state") == "completed") {
            val result = actual.getJSONObject("result")
            assertEquals(setOf("opaque_result"), result.keys().asSequence().toSet())
            assertEquals("unchanged", result.opt("opaque_result"))
        } else assertEquals(JSONObject.NULL, actual.opt("result"))
    }

    private fun relayTest(block: suspend (MockWebServer, Connection, GitStatusRelay) -> Unit) = runBlocking {
        MockWebServer().use { server ->
            server.start(InetAddress.getByName("127.0.0.1"), 0)
            val connection = Connection(request.key.machine.connectionId, "Synthetic fixture", server.url("/").toString().trimEnd('/'), "synthetic-git-status-token")
            withTimeout(5_000) { block(server, connection, GitStatusRelay(RelayApi())) }
        }
    }

    @Test fun computerScopedWireUsesOnlyOriginalRequestAndPath() = relayTest { server, connection, relay ->
        val expected = envelope("completed")
        server.enqueue(MockResponse().setBody(expected.toString()))
        assertEnvelope(expected, relay.start(connection, request))
        val captured = checkNotNull(server.takeRequest(1, TimeUnit.SECONDS))
        assertEquals("POST", captured.method)
        assertEquals("/v1/requests", captured.path)
        assertEquals("Bearer synthetic-git-status-token", captured.getHeader("Authorization"))
        val body = JSONObject(captured.body.readUtf8())
        assertEquals(setOf("request_id", "computer_id", "session", "operation", "payload"), body.keys().asSequence().toSet())
        assertEquals(request.requestId, body.getString("request_id"))
        assertEquals(request.key.machine.computerId, body.getString("computer_id"))
        assertTrue(body.opt("session") is String)
        assertEquals("", body.getString("session"))
        assertEquals("git_status", body.getString("operation"))
        val payload = body.getJSONObject("payload")
        assertEquals(setOf("request_id", "path"), payload.keys().asSequence().toSet())
        assertEquals(request.requestId, payload.getString("request_id"))
        assertEquals(request.key.requestedPath, payload.getString("path"))
        assertEquals(1, server.requestCount)
    }

    private suspend fun pendingPassesThrough(state: String, server: MockWebServer, connection: Connection, relay: GitStatusRelay) {
        val expected = envelope(state)
        server.enqueue(MockResponse().setBody(expected.toString()))
        assertEnvelope(expected, relay.start(connection, request))
        assertEquals("POST", checkNotNull(server.takeRequest(1, TimeUnit.SECONDS)).method)
        assertNull(server.takeRequest(100, TimeUnit.MILLISECONDS))
        assertEquals(1, server.requestCount)
    }

    @Test fun queuedEnvelopeReturnsWithoutHelperPolling() = relayTest { server, connection, relay ->
        pendingPassesThrough("queued", server, connection, relay)
    }

    @Test fun claimedEnvelopeReturnsWithoutHelperPolling() = relayTest { server, connection, relay ->
        pendingPassesThrough("claimed", server, connection, relay)
    }

    @Test fun receiptIsAnExplicitGetForTheSameOriginalId() = relayTest { server, connection, relay ->
        server.enqueue(MockResponse().setBody(envelope("queued").toString()))
        assertEquals("queued", relay.start(connection, request).getString("state"))
        val expected = envelope("completed")
        server.enqueue(MockResponse().setBody(expected.toString()))
        assertEnvelope(expected, relay.receipt(connection, request))
        val post = checkNotNull(server.takeRequest(1, TimeUnit.SECONDS))
        val get = checkNotNull(server.takeRequest(1, TimeUnit.SECONDS))
        assertEquals("POST", post.method)
        assertEquals(request.requestId, JSONObject(post.body.readUtf8()).getString("request_id"))
        assertEquals("GET", get.method)
        assertEquals("/v1/requests/${request.requestId}", get.path)
        assertEquals(0L, get.bodySize)
        assertEquals("Bearer synthetic-git-status-token", get.getHeader("Authorization"))
        assertEquals(2, server.requestCount)
    }

    @Test fun mismatchedSubmissionEnvelopeNeverPollsOrReplacesIdentity() = relayTest { server, connection, relay ->
        for (state in listOf("queued", "claimed", "completed")) {
            val before = server.requestCount
            server.enqueue(MockResponse().setBody(envelope(state, "123e4567-e89b-42d3-a456-426614174001").toString()))
            assertTrue(runCatching { relay.start(connection, request) }.exceptionOrNull() is IllegalStateException)
            assertEquals(before + 1, server.requestCount)
            assertEquals(request.requestId, JSONObject(checkNotNull(server.takeRequest(1, TimeUnit.SECONDS)).body.readUtf8()).getString("request_id"))
        }
    }

    @Test fun outerSubmissionIdRequiresAnActualMatchingString() = relayTest { server, connection, relay ->
        val malformed = listOf(envelope("queued").also { it.remove("request_id") }, envelope("queued", JSONObject.NULL),
            envelope("queued", 7), envelope("queued", true), envelope("queued", JSONArray(listOf(request.requestId))),
            envelope("queued", JSONObject().put("value", request.requestId)))
        for (body in malformed) {
            val before = server.requestCount
            server.enqueue(MockResponse().setBody(body.toString()))
            assertTrue(runCatching { relay.start(connection, request) }.exceptionOrNull() is IllegalStateException)
            assertEquals(before + 1, server.requestCount)
            assertEquals("POST", checkNotNull(server.takeRequest(1, TimeUnit.SECONDS)).method)
        }
    }

    @Test fun mismatchedReceiptIsRefusedWithoutAnyPost() = relayTest { server, connection, relay ->
        server.enqueue(MockResponse().setBody(envelope("completed", "123e4567-e89b-42d3-a456-426614174001").toString()))
        assertTrue(runCatching { relay.receipt(connection, request) }.exceptionOrNull() is IllegalStateException)
        val get = checkNotNull(server.takeRequest(1, TimeUnit.SECONDS))
        assertEquals("GET", get.method)
        assertEquals("/v1/requests/${request.requestId}", get.path)
        assertEquals(1, server.requestCount)
    }

    @Test fun wrongConnectionIsRefusedBeforePostOrReceiptHttp() = relayTest { server, connection, relay ->
        val foreign = connection.copy(id = "foreign-workspace")
        assertTrue(runCatching { relay.start(foreign, request) }.exceptionOrNull() is IllegalArgumentException)
        assertTrue(runCatching { relay.receipt(foreign, request) }.exceptionOrNull() is IllegalArgumentException)
        assertEquals(0, server.requestCount)
    }

    @Test fun existingInvalidRequestPinsAreRefusedBeforeAnyHttp() = relayTest { server, connection, relay ->
        val invalid = listOf(request.copy(requestId = request.requestId.uppercase()),
            request.copy(key = request.key.copy(requestedPath = "fixture/relative")),
            request.copy(key = request.key.copy(machine = request.key.machine.copy(computerId = ""))))
        for (value in invalid) {
            assertTrue(runCatching { relay.start(connection, value) }.exceptionOrNull() is IllegalArgumentException)
            assertTrue(runCatching { relay.receipt(connection, value) }.exceptionOrNull() is IllegalArgumentException)
        }
        assertEquals(0, server.requestCount)
    }

    @Test fun lostPostTransportMakesOneAttemptAndDoesNotFetchOrReplay() = relayTest { server, connection, relay ->
        server.enqueue(MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AFTER_REQUEST))
        assertTrue(runCatching { relay.start(connection, request) }.exceptionOrNull() is IOException)
        val post = checkNotNull(server.takeRequest(1, TimeUnit.SECONDS))
        assertEquals("POST", post.method)
        assertEquals(request.requestId, JSONObject(post.body.readUtf8()).getString("request_id"))
        assertEquals(1, server.requestCount)
    }
}
