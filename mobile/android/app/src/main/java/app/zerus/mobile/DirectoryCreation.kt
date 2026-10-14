package app.zerus.mobile

import org.json.JSONObject
import java.util.UUID

/** Explicit folder creation is separate from browsing and native session launch. */
internal object DirectoryCreation {
    const val operation = "directory_create"
    const val uncertain = "Folder creation is unconfirmed. Check its original receipt; it has not been retried."
    fun pathError(path: String): String = when {
        !path.startsWith('/') || path == "/" -> "Enter an absolute path for a new folder."
        path.toByteArray(Charsets.UTF_8).size > 4096 || path.any(Char::isISOControl) -> "Enter a valid native folder path."
        path.split('/').drop(1).any { it.isEmpty() || it == "." || it == ".." } -> "Use a path without empty, dot, or parent components."
        else -> ""
    }
    fun child(parent: String, name: String): String = if(name.isBlank() || name == "." || name == ".." || name.contains('/') || name.any(Char::isISOControl)) ""
        else (if(parent == "/") "/" else "$parent/") + name
    fun computerTarget(action: SessionAction, machine: MachineKey) = action.operation == operation &&
        action.target == Target(machine.computerId,"","","",machine.connectionId)
    fun pending(actions: List<SessionAction>, machine: MachineKey) = actions.lastOrNull { computerTarget(it,machine) && it.blocksSending }
    fun result(action: SessionAction, receipt: JSONObject): SessionAction {
        val native = receipt.optJSONObject("result")
        val requested = runCatching { JSONObject(action.arguments).getString("path") }.getOrDefault("")
        val path = native?.string("path").orEmpty()
        val parent = native?.string("parent").orEmpty()
        val computerOnly = action.target.session.isBlank() && action.target.run.isBlank() && action.target.conversation.isBlank() &&
            action.target.archiveId.isBlank() && action.target.agentId.isBlank()
        if(!computerOnly || !runCatching { UUID.fromString(action.requestId).toString()==action.requestId }.getOrDefault(false) ||
            pathError(requested).isNotBlank() || receipt.string("state") != "completed" ||
            native == null || native.string("request_id") != action.requestId || native.string("status") != "created" ||
            native.string("requested_path") != requested || pathError(path).isNotBlank() || path.substringAfterLast('/') != requested.substringAfterLast('/') ||
            !LaunchPresentation.absoluteFolder(parent) || path.substringBeforeLast('/').ifBlank { "/" } != parent)
            return action.copy(status="uncertain",error=uncertain)
        return action.copy(status="completed",error="",resultPath=path)
    }
}
