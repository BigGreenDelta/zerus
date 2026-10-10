package app.zerus.mobile

import java.net.URLEncoder
import java.util.UUID

object SessionReference {
    fun create(target: Target, nativeMachineName: String): String? {
        val id = runCatching { UUID.fromString(target.computerId) }.getOrNull() ?: return null
        if (id.toString() != target.computerId || id == UUID(0,0) ||
            !Regex("[A-Za-z0-9_.][A-Za-z0-9_.-]{0,79}").matches(nativeMachineName) ||
            target.session.isBlank() || (target.conversation.isBlank() && target.run.isBlank())) return null
        val conversation = if(target.agentId.isBlank()) target.conversation else target.parentConversation
        if(target.agentId.isNotBlank() && conversation.isBlank()) return null
        val fields = listOf("machine" to target.computerId, "host" to nativeMachineName, "name" to target.session,
            "conversation" to conversation, "run" to target.run, "archive" to target.archiveId, "agent" to target.agentId)
        if(fields.any { (_,value) -> value.toByteArray(Charsets.UTF_8).size>512 || value.any(Char::isISOControl) }) return null
        fun encode(value: String) = URLEncoder.encode(value,"UTF-8").replace("+","%20").replace("*","%2A")
        return "zerus://session?" + fields.filter { it.second.isNotBlank() }.joinToString("&") { (key,value) -> "$key=${encode(value)}" }
    }
}
