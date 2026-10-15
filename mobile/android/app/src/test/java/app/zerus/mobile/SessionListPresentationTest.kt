package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionListPresentationTest {
    private val host = Machine("workspace", "computer", "Workstation", true)
    private val project = ProjectSummary(ProjectKey("workspace", "swarm", "project"), "Project", "", emptyList(), emptyMap())
    private fun row(index: Int) = Session(Target(host.id, "codex/project/$index", "run", "conversation-$index", host.connectionId),
        "Session $index", "codex", "", "Project", "", 0,
        JSONObject().put("state", "running").put("activity", "idle").put("phase", "idle"), project.key)

    @Test fun projectionKeepsDraftsMachineCountsSearchAndReadAllScopeTogether() {
        val idle = row(1)
        val working = row(2).copy(raw = JSONObject("""{"activity":"busy","phase":"tool"}"""))
        val offline = row(3).copy(target = row(3).target.copy(connectionId = "offline"))
        val archive = row(4).copy(target = row(4).target.copy(archiveId = "archive"))
        val rows = listOf(idle, working, offline, archive)
        val hosts = listOf(host, host.copy(connectionId = "offline", online = false))
        val result = SessionListPresentation.prepare(rows, hosts, listOf(project), setOf(MachineKey(host.connectionId, host.id)),
            "", null, SessionFilter.Active, listOf(Draft(idle.target, "Unsent"), Draft(offline.target, "Unsent")))
        assertEquals(listOf(idle, working), result.groups.single().sessions)
        assertEquals(2, result.visibleCount)
        assertEquals(2, result.counts[SessionFilter.Active])
        assertEquals(1, result.machineCounts[MachineKey("offline", host.id)])
        assertEquals(listOf(idle.target), result.readAllTargets)
        val search = SessionListPresentation.prepare(rows, hosts, listOf(project), emptySet(), "no match", project.key,
            SessionFilter.Active, listOf(Draft(idle.target, "Unsent")))
        assertTrue(search.groups.isEmpty())
        assertTrue(search.readAllTargets.isEmpty())
        assertEquals(2, search.machineCounts[MachineKey(host.connectionId, host.id)])
    }

    @Test fun thousandsOfProjectGroupsUseAnIndexInsteadOfRepeatedCatalogScans() {
        var reads = 0
        val projects = object : AbstractList<ProjectSummary>() {
            override val size = 5_000
            override fun get(index: Int): ProjectSummary {
                reads++
                return project.copy(key = project.key.copy(id = "project-$index"), name = "Project $index")
            }
        }
        val rows = (0 until projects.size).map { row(it).copy(projectKey = project.key.copy(id = "project-$it")) }
        val result = SessionListPresentation.prepare(rows, listOf(host), projects, emptySet(), "", null, SessionFilter.All, emptyList())
        assertEquals(5_000, result.groups.size)
        assertEquals(rows.map { it.target }, result.groups.flatMap { it.sessions }.map { it.target })
        assertTrue("Repeated project scans: $reads", reads <= projects.size * 3)
    }
}
