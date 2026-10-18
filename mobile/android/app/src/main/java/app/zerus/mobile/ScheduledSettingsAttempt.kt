package app.zerus.mobile

/** Original scheduled authorization keeps its one attempt proof even if terminal receipts are pruned. */
object ScheduledSettingsAttempt {
    /** A malformed/legacy value keeps its original retention semantics. */
    internal fun expectedPendingId(action:SessionAction):String?=runCatching {
        val args=org.json.JSONObject(action.arguments)
        val id=(args.opt("expected_pending_id") as? String)?.takeIf { it.isNotBlank() } ?: return@runCatching null
        require(listOf("model","effort").all { !args.has(it) || args.opt(it) is String })
        SessionActionPolicies.arguments("settings",args,true)
        id
    }.getOrNull()
    internal fun begin(state:MessageState,authorized:SessionAction,attempt:SessionAction,retry:ScheduledSettingsRetry.ConsumedGrant?=null):MessageState {
        val source=state.sessionActions.find { it.requestId == authorized.requestId && it.status == "scheduled" && it.pendingId == authorized.pendingId }
            ?: error("This authorized settings intent already has an attempt.")
        if(retry==null) check(source.applyAttemptId.isBlank()) { "This authorized settings intent already has an attempt." }
        else {
            check(source==retry.original && source==authorized && state.sessionActions.any { it==retry.child } && source.applyAttemptId==retry.child.requestId) { "The scheduled settings proof changed." }
            require(attempt.target==retry.target && expectedPendingId(attempt)==retry.pendingId)
            val pinned=org.json.JSONObject(attempt.arguments)
            require(pinned.opt("model")==retry.model && pinned.opt("effort")==retry.effort)
        }
        require(source.operation == "settings" && attempt.operation == "settings")
        require(source.target.connectionId == attempt.target.connectionId && source.target.computerId == attempt.target.computerId && source.target.session == attempt.target.session &&
            source.target.conversation.isNotBlank() && source.target.conversation == attempt.target.conversation && attempt.target.agentId.isBlank() && attempt.target.archiveId.isBlank())
        val requested=org.json.JSONObject(source.arguments)
        require(listOf("model","effort").all { !requested.has(it) || requested.get(it) == org.json.JSONObject(attempt.arguments).opt(it) })
        require(org.json.JSONObject(attempt.arguments).string("expected_pending_id") == source.pendingId)
        return state.action(source.copy(applyAttemptId=attempt.requestId)).action(attempt)
    }
}
