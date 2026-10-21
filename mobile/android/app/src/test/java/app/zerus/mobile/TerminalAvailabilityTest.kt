package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class TerminalAvailabilityTest {
    private val target = Target("computer", "codex/project/session", "run", "conversation", "workspace")
    private val operations = setOf("terminal_snapshot", "terminal_input")
    private val live = JSONObject().put("terminal_supported", true)
    private fun reason(row: JSONObject? = live, online: Boolean = true, gateway: Set<String>? = operations,
        computer: Set<String>? = operations, selected: Target = target) = TerminalAvailability.reason(selected, row, online, gateway, computer)

    @Test fun actualNativeFalseRemainsARefusalEvenWhenBothRoutesSupportTerminal() {
        val refusal = JSONObject().put("terminal_supported", false).put("terminal_reason", "The exact live tracked pane is unavailable")
        assertEquals("The exact live tracked pane is unavailable", reason(refusal))
        assertEquals("This native adapter does not provide remote Terminal.", reason(JSONObject().put("terminal_supported", false)))
        // A transport capability cannot override a session-specific native refusal.
        assertEquals("The exact live tracked pane is unavailable", reason(refusal, gateway = null, computer = null))
    }

    @Test fun absentMalformedAndStaleNativeMetadataNeverBecomeBooleanFalse() {
        assertEquals("", reason(JSONObject())) // older native rows rely on the scoped operation and native binding check
        for (value in listOf<Any>("true", "false", 0, 1, JSONObject.NULL)) {
            val text = reason(JSONObject().put("terminal_supported", value))
            assertTrue(text.contains("metadata is unavailable"))
            assertFalse(text.contains("adapter does not provide"))
        }
        assertEquals("Refresh this session to check Terminal availability.", reason(JSONObject().put("last_known", true).put("terminal_supported", false)))
        val openCode = target.copy(session = "opencode/project/session")
        assertTrue(reason(JSONObject(), selected = openCode).contains("native server"))
    }

    @Test fun unknownGatewayAndComputerMetadataAreDifferentFromAnExplicitEmptyList() {
        assertEquals(TerminalAvailability.unknownGateway, reason(gateway = null))
        assertEquals(TerminalAvailability.unknownComputer, reason(computer = null))
        assertEquals("This gateway does not support remote Terminal. Update the gateway.", reason(gateway = emptySet()))
        assertEquals("This computer does not advertise remote Terminal. Refresh or update Zerus on this computer.", reason(computer = emptySet()))
        assertEquals("", reason())
        assertEquals("The machine is offline.", reason(online = false))
        assertTrue(reason(selected = target.copy(archiveId = "archive")).contains("live parent"))
        assertTrue(reason(selected = target.copy(agentId = "child")).contains("live parent"))
    }

    @Test fun operationParsingDoesNotCoerceMalformedDiscoveryIntoKnownUnsupportedOrSupported() {
        fun raw(value: Any) = JSONObject().put("protocol_version", 1).put("operations", value)
        assertNull(OperationCapabilities.parse(null))
        assertNull(OperationCapabilities.parse(JSONObject().put("protocol_version", 1)))
        for (value in listOf("terminal_snapshot", true, JSONObject.NULL, JSONArray(listOf("terminal_snapshot", false)), JSONArray(listOf(""))))
            assertNull(OperationCapabilities.parse(raw(value)))
        for (version in listOf<Any>(true, "1", 2))
            assertNull(OperationCapabilities.parse(raw(JSONArray(operations.toList())).put("protocol_version", version)))
        assertNull(OperationCapabilities.parse(raw(JSONArray(operations.toList())).put("protocol_version", 2), requireVersion = false))
        assertEquals(operations, OperationCapabilities.parse(JSONObject().put("operations", JSONArray(operations.toList())), requireVersion = false))
        assertEquals(emptySet<String>(), OperationCapabilities.parse(raw(JSONArray())))
        assertEquals(operations, OperationCapabilities.parse(raw(JSONArray(operations.toList()))))
    }

    @Test fun explicitDiscoveryReplacesCachedRefusalAndFailureRemovesStaleCapabilityOnlyForItsWorkspace() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            val connection = Connection(target.connectionId, "Fixture", server.url("/").toString().trimEnd('/'), "synthetic-token")
            val api = RelayApi()
            var known = mapOf(connection.id to emptySet<String>(), "other-workspace" to operations)
            fun response(value: String) { server.enqueue(MockResponse().setBody(value)) }
            response("""{"protocol_version":1,"operations":["terminal_snapshot","terminal_input"]}""")
            known = OperationCapabilities.replace(known, connection.id, OperationCapabilities.parse(api.capabilities(connection), requireVersion = false))
            assertEquals("", reason(gateway = known[connection.id]))
            // HTTP success with missing/malformed discovery must be retryable unknown, not a permanently cached empty set.
            response("""{"protocol_version":1,"operations":"terminal_snapshot"}""")
            known = OperationCapabilities.replace(known, connection.id, OperationCapabilities.parse(api.capabilities(connection), requireVersion = false))
            assertEquals(TerminalAvailability.unknownGateway, reason(gateway = known[connection.id]))
            assertFalse(known.containsKey(connection.id))
            response("""{"protocol_version":1,"operations":[]}""")
            known = OperationCapabilities.replace(known, connection.id, OperationCapabilities.parse(api.capabilities(connection), requireVersion = false))
            assertTrue(reason(gateway = known[connection.id]).contains("gateway does not support"))
            server.enqueue(MockResponse().setResponseCode(503).setBody("{}"))
            val failed = runCatching { api.capabilities(connection) }.getOrNull()
            known = OperationCapabilities.replace(known, connection.id, OperationCapabilities.parse(failed, requireVersion = false))
            assertEquals(TerminalAvailability.unknownGateway, reason(gateway = known[connection.id]))
            assertEquals(operations, known["other-workspace"])
            repeat(4) {
                val request = server.takeRequest()
                assertEquals("GET", request.method)
                assertEquals("/v1/capabilities", request.path)
                assertEquals(0L, request.bodySize)
            }
            assertEquals(4, server.requestCount)
        }
    }
}
