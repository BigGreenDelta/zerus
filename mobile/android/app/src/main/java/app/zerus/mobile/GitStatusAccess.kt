package app.zerus.mobile

import org.json.JSONObject

/** Route identity excludes captions and never resolves another hop or display alias. */
internal sealed interface GitStatusRouteIdentity {
    data object Unknown : GitStatusRouteIdentity
    data object Direct : GitStatusRouteIdentity
    data class Via(val gatewayId: String) : GitStatusRouteIdentity
}

/** Immutable transport and native identities, separate from frequently refreshed catalog labels. */
internal data class GitStatusEpoch(val connection: Connection, val machine: MachineKey,
    val nativeMachineId: String, val routeIdentity: GitStatusRouteIdentity)

/** A catalog refresh invalidates access without tying reads to one selected conversation. */
internal data class GitStatusCatalog(val connections: List<Connection>, val machines: List<Machine>,
    val relayOperations: Map<String, Set<String>>, val machineOperations: Map<MachineKey, Set<String>>,
    val demo: Boolean, val storageReady: Boolean) {
    fun access(key: MachineKey): GitStatusAccess? = if (demo || !storageReady) null else
        GitStatusAccessPolicies.access(key, connections, machines,
            relayOperations[key.connectionId].orEmpty(), machineOperations[key].orEmpty())
}

internal object GitStatusAccessPolicies {
    fun access(key: MachineKey, connections: List<Connection>, machines: List<Machine>,
        relayOperations: Set<String>, machineOperations: Set<String>): GitStatusAccess? {
        if (!GitStatus.validKey(GitStatusKey(key, "/"))) return null
        val connection = connections.singleOrNull { it.id == key.connectionId } ?: return null
        val machine = machines.singleOrNull { it.connectionId == key.connectionId && it.id == key.computerId } ?: return null
        if (!GitStatus.canonicalId(machine.machineId)) return null
        val route = when (val route = machine.route) {
            MachineRoute.Unknown -> GitStatusRouteIdentity.Unknown
            MachineRoute.Direct -> GitStatusRouteIdentity.Direct
            is MachineRoute.Via -> {
                if (!GitStatus.canonicalId(route.gatewayId)) return null
                GitStatusRouteIdentity.Via(route.gatewayId)
            }
        }
        return GitStatusAccess(GitStatusEpoch(connection, key, machine.machineId, route),
            machine.online && !machine.lastKnown, GitStatus.OPERATION in relayOperations,
            GitStatus.OPERATION in machineOperations, route != GitStatusRouteIdentity.Unknown)
    }

    /** Only the raw original cwd chooses the request argument. Missing checkout proof stays unknown. */
    fun candidate(session: Session): GitStatusCandidate? {
        val target = session.target
        val raw = session.raw
        if (target.archiveId.isNotEmpty() || target.agentId.isNotEmpty() || target.parentConversation.isNotEmpty() ||
            !absentMarker(raw, "archive_id") || !absentMarker(raw, "agent_id") ||
            !absentMarker(raw, "parent_conversation_id") || raw.opt("state") == "archived") return null
        val cwd = raw.opt("cwd") as? String ?: return null
        val key = GitStatusKey(MachineKey(target.connectionId, target.computerId), cwd)
        if (!GitStatus.validKey(key)) return null
        val metadata = raw.opt("git_metadata_state") as? String
        if (metadata == "not_repo") return null
        return GitStatusCandidate(key, GitCheckoutEvidence(raw.opt("git_root") as? String,
            raw.opt("git_branch") as? String, raw.opt("git_detached") as? Boolean,
            metadataState = metadata))
    }

    // Native optional marker fields may be omitted or null. Other JSON types cannot prove a
    // parent row; actual nonempty strings always refuse, without string coercion.
    private fun absentMarker(raw: JSONObject, key: String): Boolean {
        val value = raw.opt(key)
        return value == null || value == JSONObject.NULL || value is String && value.isEmpty()
    }
}
