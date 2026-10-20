package app.zerus.mobile

import org.json.JSONObject

data class NotificationAttention(val phase: String, val requestId: String, val startedAt: Double?, val run: String = "") {
    fun matches(current: NotificationAttention) = phase == current.phase && run == current.run &&
        if (requestId.isNotEmpty()) requestId == current.requestId else current.requestId.isEmpty() && startedAt != null && startedAt == current.startedAt
}
data class NotificationSuppression(val replyId: String?, val signature: String?, val optionalIds: Set<String>,
    val main: NotificationAttention?, val children: Map<String, NotificationAttention>)
data class SessionNotificationPreference(val conversation: String, val muted: Boolean, val revision: Long, val suppressed: NotificationSuppression)

/** Host-owned, conversation-scoped settings. Missing or invalid metadata never grants a local substitute. */
object SessionNotificationPreferences {
    const val operation = "session_notifications"
    const val unavailable = "Shared notification settings are unavailable. Update and refresh Zerus on this computer."
    private fun integer(value: Any?): Long? = when(value) { is Int -> value.toLong();is Long -> value;else -> null }?.takeIf { it >= 0 }
    private fun text(value: Any?, empty: Boolean = true, max: Int = 256, multiline: Boolean = false) = (value as? String)?.takeIf {
        (empty || it.isNotEmpty()) && it.toByteArray(Charsets.UTF_8).size <= max && it.none { c -> c.isISOControl() && !(multiline && c == '\n') }
    }
    private fun time(value: Any?): Double? = (value as? Number)?.toDouble()?.takeIf { it.isFinite() && it > 0 }
    private fun fields(value:JSONObject, keys:Set<String>)=value.length()==keys.size && keys.all(value::has)
    private fun fits(value:JSONObject):Boolean {
        var bytes=0
        // JSONObject has already escaped quotes, backslashes and ASCII controls.
        for(c in value.toString()) { bytes+=if(c.code<=127) 1 else 6;if(bytes>384*1024) return false }
        return true
    }
    private fun attention(value: JSONObject, main: Boolean): NotificationAttention? {
        if(!fields(value,if(main) setOf("run_id","phase","request_id","started_at") else setOf("phase","request_id","started_at"))) return null
        val phase = text(value.opt("phase")) ?: return null
        val request = text(value.opt("request_id"),max=65536,multiline=true) ?: return null
        if (!value.has("started_at") || value.opt("started_at") !== JSONObject.NULL && time(value.opt("started_at")) == null) return null
        if(request.isEmpty() && time(value.opt("started_at"))==null) return null
        val run = if(main) text(value.opt("run_id"),false,128) ?: return null else ""
        if(!main && phase !in setOf("approval","input","attention")) return null
        return NotificationAttention(phase,request,time(value.opt("started_at")),run)
    }
    fun parse(raw: JSONObject?, conversation: String): SessionNotificationPreference? {
        val value = raw?.optJSONObject("notification_preferences") ?: return null
        if(!fields(value,setOf("conversation_id","muted","revision","suppressed")) || !fits(value)) return null
        if(conversation.isEmpty() || text(value.opt("conversation_id"),false,256) != conversation || value.opt("muted") !is Boolean) return null
        val revision = integer(value.opt("revision")) ?: return null
        val suppressed = value.optJSONObject("suppressed") ?: return null
        if(!fields(suppressed,setOf("reply_id","attention_signature","optional_question_ids","main_attention","child_attention"))) return null
        fun nullable(key: String): String? = if(suppressed.opt(key) === JSONObject.NULL) null else text(suppressed.opt(key),false)
        if(listOf("reply_id","attention_signature","main_attention").any { !suppressed.has(it) }) return null
        val reply = nullable("reply_id");val signature = nullable("attention_signature")
        if(suppressed.opt("reply_id") !== JSONObject.NULL && reply == null || suppressed.opt("attention_signature") !== JSONObject.NULL && signature?.matches(Regex("[0-9a-f]{64}")) != true) return null
        val optional = suppressed.optJSONArray("optional_question_ids") ?: return null
        if(optional.length() > 64) return null
        val ids = (0 until optional.length()).map { text(optional.opt(it),false) ?: return null }
        if(ids.distinct().size!=ids.size || ids.zipWithNext().any { (a,b) ->
            val left=a.toByteArray(Charsets.UTF_8);val right=b.toByteArray(Charsets.UTF_8)
            val differing=left.indices.firstOrNull { it>=right.size || left[it]!=right[it] }
            if(differing==null) left.size>=right.size else differing>=right.size || (left[differing].toInt() and 255)>=(right[differing].toInt() and 255)
        }) return null
        val main = if(suppressed.opt("main_attention") === JSONObject.NULL) null else attention(suppressed.optJSONObject("main_attention") ?: return null,true) ?: return null
        val childrenRaw = suppressed.optJSONObject("child_attention") ?: return null
        if(childrenRaw.length() > 256) return null
        val children = mutableMapOf<String,NotificationAttention>()
        for(id in childrenRaw.keys()) {
            if(text(id,false) == null) return null
            children[id] = attention(childrenRaw.optJSONObject(id) ?: return null,false) ?: return null
        }
        return SessionNotificationPreference(conversation,value.getBoolean("muted"),revision,NotificationSuppression(reply,signature,ids.toSet(),main,children))
    }
    fun parse(raw: JSONObject?, target: Target) = parse(raw,target.conversation)
    fun muted(session: Session) = parse(session.raw,session.target)?.muted == true
    fun reason(target: Target, raw: JSONObject?): String = when {
        target.agentId.isNotBlank() || target.archiveId.isNotBlank() -> "Notification controls belong to the live parent conversation."
        target.run.isBlank() || text(target.run,false,128) == null || parse(raw,target) == null -> unavailable
        else -> ""
    }
    fun label(preference: SessionNotificationPreference?) = when(preference?.muted) { true -> "Unmute notifications";false -> "Mute notifications";else -> "Shared notification settings" }
    fun payload(target: Target, args: JSONObject) = JSONObject().put("run_id",target.run).put("conversation_id",target.conversation)
        .put("muted",args.get("muted")).put("expected_revision",args.get("expected_revision"))
    fun arguments(raw: JSONObject): JSONObject {
        require(raw.length()==2 && raw.has("muted") && raw.opt("muted") is Boolean && integer(raw.opt("expected_revision")) != null) { "Invalid shared notification setting." }
        return JSONObject(raw.toString())
    }
    fun result(action: SessionAction, receipt: JSONObject): SessionAction {
        val value = receipt.optJSONObject("result")
        val refusal = mapOf("conflict" to "Shared notification settings changed. Refresh and try again.",
            "baseline_unavailable" to "The computer could not capture a complete notification baseline. The setting was not changed.",
            "stale_identity" to "The conversation changed. Refresh its exact identity before applying.","not_found" to "This conversation is no longer available.",
            "capacity" to "The computer's notification preference storage is full. The setting was not changed.",
            "revision_exhausted" to "The notification preference revision is exhausted. The setting was not changed.")
        if(receipt.opt("request_id")==action.requestId && receipt.opt("state")=="completed" && value?.opt("ok")==false && !value.has("applied"))
            refusal[value.opt("error") as? String]?.let { return action.copy(status="failed",error=it) }
        val args = runCatching { arguments(JSONObject(action.arguments)) }.getOrNull()
        val pref = value?.optJSONObject("preferences")?.let { parse(JSONObject().put("notification_preferences",it),action.target) }
        val applied = value?.opt("applied") as? Boolean
        val revision = args?.let { integer(it.opt("expected_revision")) }
        val expected = if(applied == true && revision != null && revision < Long.MAX_VALUE) revision+1 else revision
        val valid = receipt.opt("request_id")==action.requestId && receipt.opt("state")=="completed" && value?.opt("ok")==true &&
            value.opt("run_id")==action.target.run && value.opt("conversation_id")==action.target.conversation && applied != null &&
            args != null && pref != null && pref.muted==args.getBoolean("muted") && pref.revision==expected && !(applied && revision==Long.MAX_VALUE)
        return if(valid) action.copy(status="completed",error="",confirmedNotificationPreference=value!!.getJSONObject("preferences").toString()) else action.copy(status="uncertain",error="The shared notification setting was not confirmed. Check its original receipt; it has not been retried.")
    }

    private fun provider(target:Target)=target.session.substringBefore('/')
    fun key(target: Target) = org.json.JSONArray(listOf(target.connectionId,target.computerId,provider(target),target.conversation)).toString()
    fun sameConversation(left:Target,right:Target)=left.connectionId==right.connectionId && left.computerId==right.computerId && provider(left)==provider(right) && left.conversation==right.conversation
    /** Proofs live in the existing bounded action receipt history, never in an unbounded session cache. */
    fun known(sessions: List<Session> = emptyList(), actions: List<SessionAction> = emptyList()): Map<String,JSONObject> {
        val result=mutableMapOf<String,JSONObject>()
        fun retain(target: Target, raw: JSONObject) {
            val incoming=parse(raw,target) ?: return
            val previous=result[key(target)]?.let { parse(JSONObject().put("notification_preferences",it),target) }
            if(previous==null || incoming.revision>previous.revision) result[key(target)]=raw.getJSONObject("notification_preferences")
        }
        for(action in actions.asReversed()) {
            if(action.operation!=operation || action.status !in setOf("completed","reviewed") || action.confirmedNotificationPreference.isEmpty()) continue
            val pref=runCatching { JSONObject(action.confirmedNotificationPreference) }.getOrNull() ?: continue
            val args=runCatching { arguments(JSONObject(action.arguments)) }.getOrNull() ?: continue
            val raw=JSONObject().put("notification_preferences",pref)
            val parsed=parse(raw,action.target) ?: continue
            val revision=args.getLong("expected_revision")
            if(parsed.muted==args.getBoolean("muted") && (parsed.revision==revision || revision<Long.MAX_VALUE && parsed.revision==revision+1)) retain(action.target,raw)
        }
        sessions.forEach { retain(it.target,it.raw) }
        return result
    }
    fun merge(raw: JSONObject, target: Target, known: Map<String,JSONObject>): JSONObject {
        val previous=known[key(target)] ?: return raw
        val retained=parse(JSONObject().put("notification_preferences",previous),target) ?: return raw
        val incoming=parse(raw,target)
        if(incoming!=null && (incoming.revision>retained.revision || incoming==retained)) return raw
        return JSONObject(raw.toString()).put("notification_preferences",JSONObject(previous.toString()))
    }
    fun mergeRows(incoming: List<Session>, retained: List<Session>, actions: List<SessionAction>): List<Session> {
        val known=known(retained,actions)
        return incoming.map { row -> val raw=merge(row.raw,row.target,known);if(raw===row.raw) row else row.copy(raw=raw) }
    }
    fun mergeDetail(raw: JSONObject, target: Target, retained: List<JSONObject>, actions: List<SessionAction>): JSONObject {
        val proofs=known(actions=actions.filter { key(it.target)==key(target) })
        var merged=raw
        for(previous in retained) {
            val candidate=merge(previous,target,proofs)
            if(parse(candidate,target)!=null) merged=merge(merged,target,mapOf(key(target) to candidate.getJSONObject("notification_preferences")))
        }
        return merge(merged,target,proofs)
    }

    /** Keep receipts/outcomes, but retain at most one proof per identity and 1 MiB of encoded proof bytes. */
    fun boundedProofs(actions: List<SessionAction>): List<SessionAction> {
        val winners=mutableMapOf<String,Pair<Int,Long>>()
        for(index in actions.indices.reversed()) {
            val action=actions[index]
            val proof=known(actions=listOf(action))[key(action.target)] ?: continue
            val revision=parse(JSONObject().put("notification_preferences",proof),action.target)?.revision ?: continue
            val key=key(action.target)
            if(winners[key]==null || revision>winners.getValue(key).second) winners[key]=index to revision
        }
        var bytes=0L
        return actions.indices.reversed().map { index ->
            val action=actions[index]
            val proof=action.confirmedNotificationPreference
            if(proof.isEmpty()) action else {
                val cost=JSONObject.quote(proof).toByteArray(Charsets.UTF_8).size.toLong()+64
                if(winners[key(action.target)]?.first!=index || cost > 1024*1024-bytes) action.copy(confirmedNotificationPreference="")
                else { bytes+=cost;action }
            }
        }.asReversed()
    }
    fun deliveryAllowed(current: NotificationCurrent, actions: List<SessionAction>): Boolean {
        val target=Target(current.slot.computer,current.slot.session,"",current.conversation,current.slot.connection)
        if(actions.any { it.operation==operation && it.blocksSending && sameConversation(it.target,target) }) return false
        val value=known(actions=actions.filter { key(it.target)==key(target) })[key(target)] ?: return true
        val latest=parse(JSONObject().put("notification_preferences",value),target) ?: return true
        return current.notificationPreference?.let { !it.muted && (it.revision>latest.revision || it==latest) } == true
    }

    private fun currentMain(raw: JSONObject, target: Target) = NotificationAttention(raw.string("phase"),raw.string("attention_id"),time(raw.opt("attention_started")),target.run)
    private fun currentChildren(raw: JSONObject): List<Pair<String,NotificationAttention>> {
        val result = mutableListOf<Pair<String,NotificationAttention>>()
        val values = raw.optJSONObject("subagents")?.let { roster -> roster.keys().asSequence().mapNotNull { id -> roster.optJSONObject(id)?.let { id to it } }.toList() }.orEmpty() +
            raw.optJSONArray("subagent_previews")?.objects().orEmpty().map { it.string("agent_id","id") to it }
        for((id,value) in values) {
            val phase = value.string("display_state","state")
            if(id.isNotEmpty() && phase in setOf("approval","input","attention")) result += id to NotificationAttention(phase,value.string("attention_id","question_id"),
                time(value.opt("attention_started")) ?: time(value.opt("last_event_at")))
        }
        return result
    }
    fun hasChildAttention(raw: JSONObject) = currentChildren(raw).isNotEmpty()
    fun attentionIdentity(raw:JSONObject,target:Target):org.json.JSONArray {
        fun edge(value:NotificationAttention)=org.json.JSONArray(listOf(value.run,value.phase,value.requestId,if(value.requestId.isEmpty()) value.startedAt else null))
        val children=currentChildren(raw).map { (id,value) -> org.json.JSONArray(listOf(id,edge(value))).toString() }.distinct().sorted()
        return org.json.JSONArray(listOf(edge(currentMain(raw,target)),org.json.JSONArray(children)))
    }
    fun attentionSuppressed(raw: JSONObject, target: Target, pref: SessionNotificationPreference): Boolean {
        val main = currentMain(raw,target)
        val children = currentChildren(raw)
        if(children.any { (id,value) -> pref.suppressed.children[id]?.matches(value) != true }) return false
        val signature = raw.opt("attention_signature") as? String
        val aggregate=signature != null && signature == pref.suppressed.signature
        // Recovery identities are not part of the host baseline ABI or its attention signature.
        if(raw.optJSONObject("recovery")?.string("state") in setOf("uncertain","blocked","exhausted")) return false
        val questions=raw.optJSONArray("mobile_attention")?.objects().orEmpty().any { !QuestionPolicies.optionalAttention(it) }
        if(questions || raw.optInt("pending_question_count")>0) return aggregate
        if(main.phase in setOf("approval","input","error")) return pref.suppressed.main?.matches(main) == true
        // Removing a covered sibling or optional question cannot make an old child new again.
        return children.isNotEmpty() || aggregate
    }
    fun alertKind(raw:JSONObject,target:Target,kind:SessionAlertKind?,optional:List<String>):SessionAlertKind? {
        val pref=parse(raw,target) ?: return kind
        return if(!pref.muted && kind in setOf(SessionAlertKind.Input,SessionAlertKind.Error) && attentionSuppressed(raw,target,pref) &&
            optional.any { it !in pref.suppressed.optionalIds }) SessionAlertKind.Optional else kind
    }
    fun suppressed(raw: JSONObject, target: Target, kind: SessionAlertKind?, optional: List<String>): Boolean {
        val pref = parse(raw,target) ?: return false
        if(pref.muted) return true
        return when(kind) {
            SessionAlertKind.Input,SessionAlertKind.Error -> attentionSuppressed(raw,target,pref)
            SessionAlertKind.Optional -> optional.isNotEmpty() && optional.all { it in pref.suppressed.optionalIds }
            else -> pref.suppressed.replyId != null && raw.opt("reply_id") == pref.suppressed.replyId
        }
    }
}
