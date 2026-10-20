package app.zerus.mobile

import org.json.JSONObject
import java.security.MessageDigest
import java.util.UUID

internal data class SessionProjectChoice(val id: String, val name: String, val color: String)
internal data class SessionProjectCatalog(val swarmId: String, val currentProjectId: String,
    val membershipToken: String, val epoch: String, val projects: List<SessionProjectChoice>, val next: String?)

/** A route fingerprint is private evidence, never a credential or a replacement native identity. */
internal object SessionProjectProtocol {
    const val read = "project_assignment"
    const val assign = "project_assign"
    const val feature = "session_project_assignment_v1"
    val operations = setOf(read, assign)
    const val uncertain = "Project grouping is unconfirmed. Check the original request; nothing has been retried."
    private val digest = Regex("[0-9a-f]{64}")
    private fun uuid(value: String) = runCatching { UUID.fromString(value).let { it.toString() == value && it != UUID(0,0) } }.getOrDefault(false)
    private fun literal(value: Any?, max: Int, empty: Boolean = false) = value is String && (empty || value.isNotEmpty()) &&
        value.toByteArray(Charsets.UTF_8).size <= max && value.none(Char::isISOControl)
    fun fingerprint(connection: Connection, machine: Machine): String {
        val value = JSONObject().put("connection",connection.id).put("url",connection.url).put("endpoint",connection.endpoint).put("token",connection.token)
            .put("computer",machine.id).put("native",machine.machineId).put("route",when(val route=machine.route) {
                MachineRoute.Direct -> "direct"; is MachineRoute.Via -> "via:"+route.gatewayId; MachineRoute.Unknown -> "unknown"
            }).toString()
        return MessageDigest.getInstance("SHA-256").digest(value.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
    }
    fun readPayload(target: Target, nativeId: String, id: String, cursor: String? = null): JSONObject {
        require(target.archiveId.isEmpty() && target.agentId.isEmpty() && target.run.isNotEmpty() && uuid(nativeId) && uuid(id))
        return target.json().put("request_id",id).put("expected_machine_id",nativeId).also { if(cursor != null) it.put("cursor",cursor) }
    }
    fun catalog(receipt: JSONObject, target: Target, nativeId: String, id: String): SessionProjectCatalog {
        require(receipt.string("request_id")==id && receipt.string("state")=="completed") { "Project choices were not confirmed. Refresh to try again." }
        val raw = checkNotNull(receipt.optJSONObject("result"))
        val allowed = setOf("schema","request_id","status","name","run_id","conversation_id","machine_id","swarm_id","current_project_id","membership_token","catalog_epoch","projects","next_cursor")
        require(raw.keys().asSequence().toSet()==allowed && raw.opt("schema") is Int && raw.getInt("schema")==1 && raw.string("status")=="ok" &&
            raw.string("request_id")==id && raw.string("name")==target.session && raw.string("run_id")==target.run &&
            raw.opt("conversation_id") == target.conversation && raw.string("machine_id")==nativeId && uuid(raw.string("swarm_id")) &&
            digest.matches(raw.string("membership_token")) && digest.matches(raw.string("catalog_epoch")) && literal(raw.opt("current_project_id"),128)) {
            "Project choices belong to another session or computer. Refresh to try again."
        }
        val rows = raw.getJSONArray("projects")
        require(rows.length()<=100 && rows.objects().size==rows.length()) { "Project choices exceed the supported page." }
        val projects = rows.objects().map { row ->
            require(row.keys().asSequence().toSet()==setOf("id","name","color") && literal(row.opt("id"),128) && literal(row.opt("name"),320) &&
                row.string("color").matches(Regex("#[0-9a-fA-F]{6}"))) { "Invalid literal project choice." }
            SessionProjectChoice(row.getString("id"),row.getString("name"),row.getString("color"))
        }
        require(projects.distinctBy { it.id }.size==projects.size)
        val next=raw.opt("next_cursor").takeUnless { it==JSONObject.NULL }?.also { require(literal(it,512)) } as String?
        return SessionProjectCatalog(raw.getString("swarm_id"),raw.getString("current_project_id"),raw.getString("membership_token"),raw.getString("catalog_epoch"),projects,next)
    }
    fun arguments(raw: JSONObject): JSONObject {
        require(raw.keys().asSequence().toSet()==setOf("expected_machine_id","route_fingerprint","swarm_id","membership_token","catalog_epoch","project_id","project_name"))
        require(uuid(raw.string("expected_machine_id")) && uuid(raw.string("swarm_id")) && digest.matches(raw.string("route_fingerprint")) &&
            digest.matches(raw.string("membership_token")) && digest.matches(raw.string("catalog_epoch")) && literal(raw.opt("project_id"),128) && literal(raw.opt("project_name"),320))
        return JSONObject(raw.toString())
    }
    fun arguments(nativeId: String, route: String, catalog: SessionProjectCatalog, project: SessionProjectChoice) = arguments(JSONObject()
        .put("expected_machine_id",nativeId).put("route_fingerprint",route).put("swarm_id",catalog.swarmId).put("membership_token",catalog.membershipToken)
        .put("catalog_epoch",catalog.epoch).put("project_id",project.id).put("project_name",project.name))
    fun payload(action: SessionAction): JSONObject {
        require(action.operation==assign)
        val args=arguments(JSONObject(action.arguments))
        return readPayload(action.target,args.getString("expected_machine_id"),action.requestId).also { body ->
            listOf("swarm_id","membership_token","catalog_epoch","project_id").forEach { body.put(it,args.get(it)) }
        }
    }
    fun result(action: SessionAction, receipt: JSONObject): SessionAction {
        if(receipt.string("request_id")!=action.requestId) return action.copy(status="uncertain",error=uncertain)
        if(receipt.string("state")=="failed") return action.copy(status="failed",error=receipt.string("error").ifBlank { "The project move was refused." })
        val args=runCatching { arguments(JSONObject(action.arguments)) }.getOrNull() ?: return action.copy(status="uncertain",error=uncertain)
        val raw=receipt.optJSONObject("result") ?: return action.copy(status="uncertain",error=uncertain)
        val scope=listOf("swarm_id","membership_token","catalog_epoch","project_id")
        if(receipt.string("state")!="completed" || raw.opt("schema") !is Int || raw.optInt("schema")!=1 || raw.string("request_id")!=action.requestId ||
            raw.string("name")!=action.target.session || raw.string("run_id")!=action.target.run || raw.opt("conversation_id")!=action.target.conversation ||
            raw.string("machine_id")!=args.string("expected_machine_id") || scope.any { raw.opt(it)!=args.opt(it) }) return action.copy(status="uncertain",error=uncertain)
        return when(raw.string("status")) {
            "assigned" -> if(raw.string("current_project_id")==args.string("project_id")) action.copy(status="completed",error="") else action.copy(status="uncertain",error=uncertain)
            "failed" -> action.copy(status="failed",error=raw.string("error").ifBlank { "The project move was refused. Refresh and review current choices." })
            else -> action.copy(status="uncertain",error=uncertain)
        }
    }
}
