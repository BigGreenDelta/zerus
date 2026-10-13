package app.zerus.mobile

import java.net.URLEncoder
import java.util.UUID

data class SessionReferenceValue(val uri: String? = null, val reason: String = "")

object SessionReference {
    fun identifiers(target: Target): List<String> = buildList {
        if (target.conversation.isNotBlank()) add("Session ID: ${target.conversation}")
        else if (target.agentId.isBlank() && target.run.isNotBlank()) add("Run ID: ${target.run}")
        if (target.agentId.isNotBlank()) add("Agent ID: ${target.agentId}")
    }

    /** Relay IDs and display labels route the phone; only native identity belongs in a portable reference. */
    fun resolve(target: Target, machines: List<Machine>): SessionReferenceValue {
        val machine = machines.filter { it.connectionId == target.connectionId && target.computerId.isNotBlank() &&
            (it.id == target.computerId || target.computerId in it.serverAliases) }.singleOrNull()
            ?: return SessionReferenceValue(reason = "Refresh the computer list to verify this session's computer.")
        val id = runCatching { UUID.fromString(machine.machineId) }.getOrNull()
        if (id == null || id.toString() != machine.machineId || id == UUID(0,0))
            return SessionReferenceValue(reason = "This computer has not reported its Zerus identity. Refresh its connection.")
        if (!Regex("[A-Za-z0-9_.][A-Za-z0-9_.-]{0,79}").matches(machine.nativeHost))
            return SessionReferenceValue(reason = "This computer has not reported its native host name. Refresh its connection.")
        val conversation = if(target.agentId.isBlank()) target.conversation else target.parentConversation
        if (target.agentId.isNotBlank() && conversation.isBlank())
            return SessionReferenceValue(reason = "The parent conversation identity is not available yet.")
        if (target.session.isBlank() || !target.session.contains('/') || (conversation.isBlank() && target.run.isBlank()))
            return SessionReferenceValue(reason = "The session identity is not available yet. Refresh the session.")
        val fields = listOf("machine" to machine.machineId, "host" to machine.nativeHost, "name" to target.session,
            "conversation" to conversation, "run" to target.run, "archive" to target.archiveId, "agent" to target.agentId)
        if(fields.any { (_,value) -> value.toByteArray(Charsets.UTF_8).size>512 || value.any(Char::isISOControl) })
            return SessionReferenceValue(reason = "The reported session identity cannot form a valid reference.")
        fun encode(value: String) = URLEncoder.encode(value,"UTF-8").replace("+","%20").replace("*","%2A")
        val uri = "zerus://session?" + fields.filter { it.second.isNotBlank() }.joinToString("&") { (key,value) -> "$key=${encode(value)}" }
        return if (uri.length <= 8192) SessionReferenceValue(uri)
            else SessionReferenceValue(reason = "The reported session identity is too long to copy as a reference.")
    }
}
