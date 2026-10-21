package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionCatalogTest {
    @Test fun preparedCatalogKeepsMachineCapabilitiesAndCanonicalMembershipInOneWorkspace() {
        val connection = Connection("workspace", "Fixture", "https://relay.example.test", "fixture")
        val session = JSONObject().put("name", "codex/project/session").put("run_id", "run")
            .put("conversation_id", "conversation").put("mobile_project_id", "project").put("cwd", "/workspace/project")
        fun computer(id: String, version: Int) = JSONObject().put("id", id).put("name", "Fixture $id").put("online", true)
            .put("snapshot", JSONObject().put("sessions", JSONArray(listOf(session)))
                .put("mobile_capabilities", JSONObject().put("protocol_version", version)
                    .put("operations", JSONArray(listOf("history"))).put("features", JSONArray(listOf("inspect_agent"))))
                .put("mobile_projects", JSONObject().put("schema", 1).put("available", true).put("swarm_id", "swarm")
                    .put("projects", JSONArray(listOf(JSONObject().put("id", "project").put("name", "Canonical project"))))))
        val prepared = SessionCatalogParser.parse(connection, listOf(computer("one", 1), computer("two", 2)))
        assertEquals(listOf("one", "two"), prepared.machines.map { it.id })
        assertTrue(prepared.machines.all { it.connectionId == connection.id })
        assertEquals(setOf("history"), prepared.operations[MachineKey(connection.id, "one")])
        assertNull(prepared.operations[MachineKey(connection.id, "two")])
        assertEquals(setOf("inspect_agent"), prepared.features[MachineKey(connection.id, "two")])
        assertEquals(2, prepared.accounts.size)
        assertEquals(setOf("one", "two"), prepared.projects.projects.single().computers.keys)
        assertTrue(prepared.projects.sessions.all { it.project == "Canonical project" && it.projectKey == ProjectKey(connection.id, "swarm", "project") })
        assertEquals(setOf("one", "two"), prepared.projects.sessions.map { it.target.computerId }.toSet())
    }

    @Test fun missingMalformedAndUnsupportedComputerOperationsRemainDistinct() {
        val connection = Connection("workspace", "Fixture", "https://relay.example.test", "fixture")
        fun computer(id: String, capabilities: Any?) = JSONObject().put("id", id).put("online", true)
            .put("snapshot", JSONObject().put("mobile_capabilities", capabilities))
        val unsupported = JSONObject().put("protocol_version", 1).put("operations", JSONArray())
        val supported = JSONObject().put("protocol_version", 1).put("operations", JSONArray(listOf("terminal_snapshot", "terminal_input")))
        val malformed = JSONObject().put("protocol_version", 1).put("operations", "terminal_snapshot")
        val catalog = SessionCatalogParser.parse(connection, listOf(computer("missing", null), computer("bad", malformed),
            computer("unsupported", unsupported), computer("supported", supported)))
        assertNull(catalog.operations[MachineKey(connection.id, "missing")])
        assertNull(catalog.operations[MachineKey(connection.id, "bad")])
        assertEquals(emptySet<String>(), catalog.operations[MachineKey(connection.id, "unsupported")])
        assertEquals(setOf("terminal_snapshot", "terminal_input"), catalog.operations[MachineKey(connection.id, "supported")])
        assertTrue(catalog.machines.all { it.online })
    }
}
