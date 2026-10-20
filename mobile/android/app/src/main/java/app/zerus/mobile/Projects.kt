package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class ProjectKey(val connectionId: String, val swarmId: String, val id: String) {
    val key: String get() = JSONArray(listOf(connectionId, swarmId, id)).toString()
}
data class ProjectFolder(val id: String, val name: String, val path: String, val computerId: String, val computerName: String)
data class ProjectSummary(val key: ProjectKey, val name: String, val color: String,
    val folders: List<ProjectFolder>, val computers: Map<String, String>, val stale: Boolean = false, val catalogued: Boolean = true)
/** [taskDigests] holds, for each catalogued project, every computer's tasks digest (by computer ID). */
data class ProjectCatalog(val projects: List<ProjectSummary>, val sessions: List<Session>, val unavailableComputers: List<String>,
    val taskDigests: Map<ProjectKey, Map<String, String>> = emptyMap())

private val taskDigest = Regex("[0-9a-f]{64}")

/** Canonical IDs own membership; paths are only an exact fallback for uncatalogued sessions. */
object ProjectParser {
    fun parse(connection: Connection, computers: List<JSONObject>, sessions: List<Session>): ProjectCatalog {
        val groups = linkedMapOf<ProjectKey, ProjectSummary>()
        val catalogs = computers.associate { computer -> computer.getString("id") to computer.optJSONObject("snapshot")?.optJSONObject("mobile_projects") }
        val assignments = mutableMapOf<Pair<String, String>, MutableSet<ProjectKey>>()
        val digests = linkedMapOf<ProjectKey, MutableMap<String, String>>()
        val unavailable = mutableListOf<String>()
        computers.forEach { computer ->
            val computerId = computer.getString("id")
            val computerName = computer.string("name", "id")
            val catalog = catalogs[computerId]
            // An offline peer has no snapshot; Machines already reports it as offline.
            if (computer.optJSONObject("snapshot") == null && !computer.optBoolean("online")) return@forEach
            if (catalog == null || catalog.optInt("schema") != 1 || !catalog.optBoolean("available")) {
                unavailable += computerName
                return@forEach
            }
            val swarm = catalog.string("swarm_id")
            if (swarm.isBlank()) { unavailable += computerName; return@forEach }
            catalog.optJSONArray("projects")?.objects().orEmpty().forEach projectLoop@ { project ->
                val id = project.string("id")
                if (id.isBlank()) return@projectLoop
                val key = ProjectKey(connection.id, swarm, id)
                // Every computer of the swarm names its own digest of the project's tasks.
                project.optString("tasks_digest").takeIf { taskDigest.matches(it) }?.let {
                    digests.getOrPut(key) { linkedMapOf() }[computerId] = it
                }
                val folders = project.optJSONArray("folders")?.objects().orEmpty().filter { it.optBoolean("local", true) }.map {
                    ProjectFolder(it.string("id"), it.string("name"), it.string("path"), computerId, computerName)
                }.filter { it.path.isNotBlank() }
                val existing = groups[key]
                val memberNames = project.optJSONArray("sessions")?.let { names -> (0 until names.length()).map { names.optString(it) } }.orEmpty()
                val archiveIds = project.optJSONArray("archives")?.let { ids -> (0 until ids.length()).map { ids.optString(it) } }.orEmpty()
                val participates = folders.isNotEmpty() || sessions.any { it.target.computerId == computerId &&
                    (if (it.target.archiveId.isNotBlank()) it.target.archiveId in archiveIds else it.target.session in memberNames) }
                groups[key] = ProjectSummary(key, project.string("name").ifBlank { "Project" }, project.string("color"),
                    (existing?.folders.orEmpty() + folders).distinctBy { it.computerId to it.id.ifBlank { it.path } },
                    existing?.computers.orEmpty() + if (participates) mapOf(computerId to computerName) else emptyMap(), existing?.stale == true || catalog.optBoolean("stale"))
                project.optJSONArray("sessions")?.let { names -> (0 until names.length()).forEach { index ->
                    names.optString(index).takeIf { it.isNotBlank() }?.let { name -> assignments.getOrPut(computerId to "live:$name") { mutableSetOf() }.add(key) }
                } }
                project.optJSONArray("archives")?.let { ids -> (0 until ids.length()).forEach { index ->
                    ids.optString(index).takeIf { it.isNotBlank() }?.let { id -> assignments.getOrPut(computerId to "archive:$id") { mutableSetOf() }.add(key) }
                } }
            }
        }
        val mapped = sessions.map { session ->
            val computer = computers.find { it.string("id") == session.target.computerId }
            val computerName = computer?.string("name", "id") ?: session.target.computerId
            val catalog = catalogs[session.target.computerId]
            val swarm = catalog?.string("swarm_id").orEmpty()
            val explicit = session.raw.string("mobile_project_id")
            val namedMembership = assignments[session.target.computerId to if (session.target.archiveId.isBlank()) "live:${session.target.session}" else "archive:${session.target.archiveId}"]?.singleOrNull()
            val canonical = if (explicit.isNotBlank() && swarm.isNotBlank()) ProjectKey(connection.id, swarm, explicit)
                else namedMembership
            val exactFolder = if (canonical == null) groups.values.filter { group ->
                group.folders.any { it.computerId == session.target.computerId && it.path == session.raw.string("cwd") }
            }.singleOrNull()?.key else null
            val nativeName = session.raw.opt("project") as? String ?: ""
            val cwd = session.raw.string("cwd")
            val key = canonical ?: exactFolder ?: if (nativeName.isNotBlank())
                ProjectKey(connection.id, "alias:${session.target.computerId}", "name:$nativeName")
            else ProjectKey(connection.id, "folder:${session.target.computerId}", cwd.ifBlank { "unassigned" })
            if (key !in groups) {
                val title = nativeName.ifBlank { cwd.trimEnd('/').substringAfterLast('/').ifBlank { "Unassigned sessions" } }
                groups[key] = ProjectSummary(key, title, "", if (cwd.isBlank()) emptyList() else listOf(
                    ProjectFolder("", title, cwd, session.target.computerId, computerName)), mapOf(session.target.computerId to computerName), catalogued = false)
            } else if (groups.getValue(key).catalogued) {
                val existing = groups.getValue(key)
                groups[key] = existing.copy(computers = existing.computers + (session.target.computerId to computerName))
            } else {
                val existing = groups.getValue(key)
                groups[key] = existing.copy(computers = existing.computers + (session.target.computerId to computerName),
                    folders = (existing.folders + if (cwd.isBlank()) emptyList() else listOf(ProjectFolder("", existing.name, cwd, session.target.computerId, computerName)))
                        .distinctBy { it.computerId to it.path })
            }
            session.copy(project = groups.getValue(key).name, projectKey = key)
        }
        return ProjectCatalog(groups.values.toList(), mapped, unavailable, digests)
    }
}
