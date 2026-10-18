package app.zerus.mobile

import org.json.JSONObject

/** Durable identity of one native context command; it never authorizes an automatic retry. */
data class ContextOperation(val requestId: String, val target: Target, val operation: String,
    val status: String = "sending", val createdAt: Long = System.currentTimeMillis(), val error: String = "", val resolvedConversation: String = "", val dismissPrompt: Boolean? = null) {
    val blocksSending get() = status in listOf("sending", "submitted", "uncertain")
    fun recover() = if (status in listOf("sending", "submitted")) copy(status = "uncertain", error = "Context command was interrupted. Check its original receipt; it has not been retried.") else this
}

/** A continuation is an in-memory user intent, never reconstructed from durable records. */
data class CompactContinuation(val requestId: String, val draft: Draft, val navigation: Long, val startedAt: Long) {
    fun unchanged(current: Draft) = ContextPolicies.sameDraft(draft, current)
    fun allowed(target: Target?, navigation: Long, now: Long) = target == draft.target && navigation == this.navigation && now - startedAt in 0..600_000
    fun permitsTransport(target: Target?, navigation: Long, now: Long, written: Draft, current: Draft) =
        allowed(target, navigation, now) && written.key == draft.key && current.key == written.key &&
            current.generation == written.generation && current.revision == written.revision && current.text.isEmpty() && current.attachments.isEmpty()
}

object ContextPolicies {
    const val DISMISS_FEATURE = "clear_context_dismiss"

    /** The caller owns selected-frame freshness and pending-delivery checks. Native eligibility is explicit. */
    fun dismissReason(target: Target, raw: JSONObject?, relayFeatures: Set<String>, machineFeatures: Set<String>): String {
        fun marked(key: String): Boolean = raw?.opt(key).let { it != null && it != JSONObject.NULL && (it !is String || it.isNotEmpty()) }
        if (target.archiveId.isNotEmpty() || target.agentId.isNotEmpty() || target.parentConversation.isNotEmpty() ||
            target.connectionId.isBlank() || target.computerId.isBlank() || target.session.isBlank() || target.run.isBlank() || target.conversation.isBlank() ||
            raw == null || raw.opt("name") != target.session || raw.opt("run_id") != target.run || raw.opt("conversation_id") != target.conversation ||
            marked("archive_id") || marked("agent_id") || marked("parent_conversation_id") || raw.opt("state") == "archived")
            return "Refresh the live conversation before dismissing its prompt."
        if (DISMISS_FEATURE !in relayFeatures || DISMISS_FEATURE !in machineFeatures)
            return "This gateway or computer does not support dismissing native prompts."
        if (raw.opt("runtime_state") != "live" || raw.opt("process_state") != "running") return "The native agent is not running."
        if ((raw.opt(DISMISS_FEATURE) as? Boolean) != true) return "The computer has not offered to dismiss this prompt before clearing context."
        return ""
    }

    fun payload(operation: ContextOperation): JSONObject {
        require(operation.dismissPrompt == null || operation.operation == "clear_context") { "Only clear context can dismiss a prompt." }
        return operation.target.json().put("request_id", operation.requestId).also { payload ->
            operation.dismissPrompt?.let { payload.put("dismiss_prompt", it) }
        }
    }

    fun sameSession(a: Target, b: Target) = a.agentId.isBlank() && b.agentId.isBlank() && a.archiveId.isBlank() && b.archiveId.isBlank() && a.connectionId == b.connectionId && a.computerId == b.computerId && a.session == b.session
    fun sameSessionRun(a: Target, b: Target) = sameSession(a, b) && a.run == b.run

    fun clearMoved(state: MessageState, from: Target, to: Target) = from != to && sameSessionRun(from, to) &&
        state.contextOperations.any { it.target == from && it.operation == "clear_context" && it.status == "completed" && it.resolvedConversation.isNotBlank() && it.resolvedConversation == to.conversation }
    fun sameDraft(expected: Draft, current: Draft) = expected.target == current.target && expected.key == current.key &&
        expected.status == "editing" && current.status == "editing" && expected.generation == current.generation &&
        expected.revision == current.revision && expected.text == current.text && expected.answers == current.answers && expected.attachments == current.attachments
    fun compactStatus(operation: ContextOperation, raw: JSONObject): String? {
        if (operation.operation != "compact_context" || operation.target.archiveId.isNotBlank() || raw.string("archive_id").isNotBlank() || raw.string("state") == "archived" || raw.string("run_id") != operation.target.run ||
            raw.string("conversation_id") != operation.target.conversation || raw.string("name") != operation.target.session) return null
        val request = raw.optJSONObject("compact_context_request") ?: return null
        if (request.string("request_id") != operation.requestId || request.string("run_id") != operation.target.run ||
            request.string("conversation_id") != operation.target.conversation) return null
        val status = request.string("status")
        if (status == "completed" && (raw.string("activity") != "idle" || raw.string("phase") != "idle")) return null
        return status.takeIf { it in listOf("completed", "failed", "cancelled", "unchanged", "uncertain") }
    }
    fun result(operation: ContextOperation, receipt: JSONObject): ContextOperation {
        if (receipt.string("request_id") != operation.requestId) return operation.copy(status = "uncertain", error = "Gateway returned a different request. Nothing has been retried.")
        if (receipt.string("state") == "failed") return operation.copy(status = "failed", error = "The computer rejected the context command.")
        val native = receipt.optJSONObject("result")
        if (receipt.string("state") != "completed" || native == null || native.string("request_id") != operation.requestId ||
            native.string("name") != operation.target.session || native.string("run_id") != operation.target.run || native.string("conversation_id") != operation.target.conversation)
            return operation.copy(status = "uncertain", error = "Context delivery is unconfirmed. Check the original receipt.")
        return when (native.string("status")) {
            "submitted" -> operation.copy(status = "submitted", error = "")
            "confirmed" -> if (operation.operation == "clear_context") operation.copy(status = "completed", error = "") else operation.copy(status = "uncertain")
            else -> operation.copy(status = "uncertain", error = "The native context command did not confirm its outcome.")
        }
    }
    fun promoteClearDraft(state: MessageState, operation: ContextOperation, to: Target): MessageState {
        val from = operation.target
        val confirmed = state.contextOperations.find { it.requestId == operation.requestId && it.status == "completed" } ?: return state
        if (operation.operation != "clear_context" || operation.status != "completed" || from.archiveId.isNotBlank() || to.archiveId.isNotBlank() ||
            from.connectionId != to.connectionId || from.computerId != to.computerId || from.session != to.session || from.run != to.run ||
            to.conversation.isBlank() || from.conversation == to.conversation) return state
        val editor = state.drafts.find { it.target == from && it.questionId.isBlank() && it.detachedId.isBlank() && it.status == "editing" } ?: return state
        if (state.drafts.any { it.target == to && it.questionId.isBlank() && it.detachedId.isBlank() }) return state
        return state.context(confirmed.copy(resolvedConversation = to.conversation)).copy(drafts = state.drafts.map { if (it.key == editor.key) it.copy(target = to) else it })
    }
}
