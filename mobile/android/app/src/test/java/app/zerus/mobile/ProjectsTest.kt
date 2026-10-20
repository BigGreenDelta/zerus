package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ProjectsTest {
    private val connection = Connection("workspace-a", "A", "https://gateway.example", "synthetic")
    private fun project(id: String, name: String, names: List<String> = emptyList(), paths: List<String> = emptyList()) = JSONObject()
        .put("id", id).put("name", name).put("sessions", JSONArray(names)).put("folders", JSONArray(paths.map {
            JSONObject().put("id", it).put("name", "Folder").put("path", it).put("local", true)
        }))
    private fun computer(id: String, groups: List<JSONObject>, native: List<JSONObject> = emptyList(), available: Boolean = true) = JSONObject()
        .put("id", id).put("name", id.uppercase()).put("snapshot", JSONObject().put("sessions", JSONArray(native))
            .put("mobile_projects", JSONObject().put("schema", 1).put("available", available).put("swarm_id", "swarm").put("projects", JSONArray(groups))))
    private fun session(name: String, cwd: String, project: String = "", id: String = "") = JSONObject()
        .put("name", name).put("cwd", cwd).put("project", project).put("mobile_project_id", id).put("run_id", "run-$name").put("conversation_id", "conversation")
    private fun parse(computers: List<JSONObject>, workspace: Connection = connection) = ProjectParser.parse(workspace, computers,
        computers.flatMap { NativeParser.sessions(workspace, it) })

    @Test fun sharedCanonicalIdsMergeAcrossMachinesButDuplicateNamesDoNot() {
        val a = computer("a", listOf(project("p", "Same", listOf("one")), project("q", "Same")), listOf(session("one", "/a", id = "p")))
        val b = computer("b", listOf(project("p", "Same", listOf("two")), project("q", "Same")), listOf(session("two", "/b", id = "p")))
        val result = parse(listOf(a, b))
        assertEquals(2, result.projects.size)
        assertEquals(setOf("a", "b"), result.projects.first { it.key.id == "p" }.computers.keys)
        assertEquals(1, result.sessions.map { it.projectKey }.distinct().size)
        val another = parse(listOf(a), connection.copy(id = "workspace-b"))
        assertNotEquals(result.sessions.first().projectKey, another.sessions.first().projectKey)
    }
    @Test fun taskDigestsAreReadPerComputer() {
        val digest = { c: Char -> c.toString().repeat(64) }
        val a = computer("a", listOf(project("p", "Orchard", listOf("one")).put("tasks_digest", digest('a'))), listOf(session("one", "/a", id = "p")))
        // A computer that lists the project without folders or sessions still names its digest.
        val b = computer("b", listOf(project("p", "Orchard").put("tasks_digest", digest('b')), project("q", "Meadow").put("tasks_digest", "not a digest")))
        val result = parse(listOf(a, b))
        assertEquals(mapOf("a" to digest('a'), "b" to digest('b')), result.taskDigests[result.projects.first { it.key.id == "p" }.key])
        assertNull(result.taskDigests[result.projects.first { it.key.id == "q" }.key])
    }
    @Test fun projectsAndSessionGroupsKeepTheSharedCatalogOrder() {
        // hgs lists projects in the swarm's shared order, the order desktops show.
        val result = parse(listOf(computer("a", listOf(project("z-first", "Zeta", listOf("one")), project("m", "Alpha", listOf("two")),
            project("b-last", "Mid", listOf("three"))), listOf(session("three", "/c"), session("one", "/a"), session("two", "/b")))))
        assertEquals(listOf("z-first", "m", "b-last"), result.projects.map { it.key.id })
        assertEquals(listOf("Zeta", "Alpha", "Mid"), SessionFilters.grouped(result.sessions, result.projects).map { it.name })
    }
    @Test fun offlinePeerWithoutSnapshotIsNotReportedAsCatalogFailure() {
        val online = computer("a", listOf(project("p", "Project"))).put("online", true)
        val offline = JSONObject().put("id", "mac").put("name", "Mac").put("online", false).put("snapshot", JSONObject.NULL)
        val pending = JSONObject().put("id", "new").put("name", "New").put("online", true).put("snapshot", JSONObject.NULL)
        val result = parse(listOf(online, offline, pending))
        assertEquals(listOf("New"), result.unavailableComputers)
        assertEquals(listOf("p"), result.projects.map { it.key.id })
    }
    @Test fun emptyHeadersAreVisibleWithZeroComputersEvenWhenReplicated() {
        val result = parse(listOf(computer("a", listOf(project("empty", "Empty"))), computer("b", listOf(project("empty", "Empty")))))
        assertEquals(1, result.projects.size)
        assertEquals("Empty", result.projects.single().name)
        assertTrue(result.projects.single().computers.isEmpty())
        assertTrue(result.sessions.isEmpty())
    }
    @Test fun canonicalMembershipWinsOverFolderAndLegacyName() {
        val result = parse(listOf(computer("a", listOf(project("assigned", "Logical", listOf("one")), project("folder", "Folder", paths = listOf("/work"))),
            listOf(session("one", "/work", "Legacy")))))
        assertEquals("assigned", result.sessions.single().projectKey!!.id)
        assertEquals("Logical", result.sessions.single().project)
    }
    @Test fun explicitCanonicalIdWinsEvenWhenExactNameMembershipDisagrees() {
        val result = parse(listOf(computer("a", listOf(project("explicit", "Selected"), project("named", "Other", listOf("one"))),
            listOf(session("one", "/work", id = "explicit")))))
        assertEquals("explicit", result.sessions.single().projectKey!!.id)
    }
    @Test fun pathFallbackIsExactCaseSensitiveAndNeverAncestorGuess() {
        val result = parse(listOf(computer("a", listOf(project("p", "Project", paths = listOf("/Work"))),
            listOf(session("exact", "/Work"), session("case", "/work"), session("child", "/Work/child")))))
        assertEquals("p", result.sessions.first { it.target.session == "exact" }.projectKey!!.id)
        assertNotEquals("p", result.sessions.first { it.target.session == "case" }.projectKey!!.id)
        assertNotEquals("p", result.sessions.first { it.target.session == "child" }.projectKey!!.id)
        assertEquals(3, result.projects.size)
    }
    @Test fun uncataloguedAliasesStayMachineLocalAndPreserveCase() {
        val result = parse(listOf(computer("a", emptyList(), listOf(session("one", "/a", "Named"), session("lower", "/lower", "named")), false),
            computer("b", emptyList(), listOf(session("two", "/b", "Named")), false)))
        assertEquals(3, result.projects.size)
        assertTrue(result.projects.all { !it.catalogued })
        assertNotEquals(result.sessions[0].projectKey, result.sessions[2].projectKey)
        assertEquals(setOf("A", "B"), result.unavailableComputers.toSet())
    }
    @Test fun ordinaryFoldersAndUnassignedSessionsRemainAccessibleWithoutCatalog() {
        val raw = JSONObject().put("id", "a").put("snapshot", JSONObject().put("sessions", JSONArray(listOf(session("plain", "/ordinary"), session("unknown", "")))))
        val result = parse(listOf(raw))
        assertEquals(2, result.sessions.size)
        assertEquals(2, result.projects.size)
        assertTrue(result.projects.any { it.name == "ordinary" && it.folders.single().path == "/ordinary" })
        assertTrue(result.projects.any { it.name == "Unassigned sessions" })
    }
}
