package app.zerus.mobile

import org.json.JSONArray

/** marked includes partial; skipped and duplicate handling count distinct conversation keys. */
data class ReadAllResult(val marked:Int=0,val skipped:Int=0,val partial:Int=0)
data class ReadAllCapture(val target:Target,val evidence:ReadAttentionEvidence?)
data class ReadAllUpdate(val state:ConversationReadState,val result:ReadAllResult)

object ReadAllPolicies {
    /** List-level Read all follows visible reply badges; an explicit review reminder stays actionable. */
    fun listCandidate(session:Session,online:Boolean,reviewLater:Boolean=false):Boolean =
        !SessionFilters.archived(session) && (reviewLater || session.raw.optBoolean("review_later") ||
            session.raw.optBoolean("mobile_review_later") ||
            SharedReadPolicies.canShowReplyBadge(session) && !SessionFilters.replyInProgress(session,online))
    fun available(evidence:ReadAttentionEvidence?)=evidence!=null &&
        (evidence.authoritative && evidence.epoch.isNotBlank() && evidence.head!=null || evidence.loadedIds.isNotEmpty())
    /** A known head past the watermark, or a loaded reply this phone counts as unread. */
    fun unreadReplies(record:ConversationReadRecord?,rows:List<Event>)=
        record!=null && record.epoch.isNotBlank() && (record.headIncoming ?: 0)>record.readThrough ||
        rows.any { ConversationReadPolicies.unread(record,it) }
    /** Cheap check before capturing evidence: a local reminder or replies counted unread. */
    fun pending(record:ConversationReadRecord?,rows:List<Event>)=record?.reviewLater==true || unreadReplies(record,rows)
    /**
     * Whether [apply] with this evidence would clear something this phone shows: a reminder or replies counted unread.
     * Uses apply's own skip rules; [full] is a missing record that the 500-record cap would refuse.
     */
    fun changes(record:ConversationReadRecord?,evidence:ReadAttentionEvidence?,rows:List<Event>,full:Boolean=false):Boolean {
        if(!available(evidence) || record==null && full || record?.epoch.orEmpty()!=evidence!!.epoch || record?.headIncoming!=evidence.head ||
            evidence.authoritative && record?.headComplete!=true) return false
        if(record?.reviewLater==true) return true
        return if(evidence.authoritative) evidence.head!!>record!!.readThrough
            else rows.any { it.id in evidence.loadedIds && ConversationReadPolicies.unread(record,it) }
    }
    /** All evidence is captured before the single ordered edit; reducers never fetch or inspect UI state. */
    fun apply(state:ConversationReadState,captured:List<ReadAllCapture>,current:List<ReadAllCapture>):ReadAllUpdate {
        var next=state;var marked=0;var skipped=0;var partial=0
        captured.distinctBy { ConversationReadPolicies.key(it.target) }.forEach { capture ->
            val evidence=capture.evidence
            val matching=current.find { it.target==capture.target }?.evidence
            val old=next.record(capture.target)
            if(!available(evidence) || evidence!!.target!=capture.target || evidence!=matching ||
                old?.epoch.orEmpty()!=evidence.epoch || old?.headIncoming!=evidence.head ||
                evidence.authoritative && old?.headComplete!=true) { skipped++;return@forEach }
            val updated=ReadAttentionPolicies.mark(next,evidence)
            val record=updated.record(capture.target)
            if(record==null) { skipped++;return@forEach }
            next=updated;marked++
            if(!evidence.authoritative || record.incomplete) partial++
        }
        return ReadAllUpdate(next,ReadAllResult(marked,skipped,partial))
    }
}

/** Seed metadata heads without opening chats, retaining bounded storage and scheduler ownership. */
object ReadAllHeadCandidates {
    fun candidates(sessions:List<Session>,state:ConversationReadState):List<Session> {
        var slots=(500-state.records.size).coerceAtLeast(0)
        val recorded=state.records.mapTo(hashSetOf()) { ConversationReadPolicies.key(it.target) }
        return sessions.distinctBy { ConversationReadPolicies.key(it.target) }.filter { row ->
            val t=row.target
            if(SessionFilters.archived(row) || t.run.isBlank() || t.conversation.isBlank()) false
            else if(ConversationReadPolicies.key(t) in recorded) true
            else if(slots>0 && (row.raw.string("reply_id").isNotBlank() || row.raw.optDouble("reply_at",0.0)>0 ||
                row.raw.optBoolean("unread_reply") || row.raw.optBoolean("mobile_unread_reply"))) { slots--;true } else false
        }
    }
    fun hint(row:Session)=JSONArray(listOf(row.target.key,HistoryHeadHints.signature(row.preview,row.raw),
        row.raw.string("reply_id"),HistoryHeadHints.value(row.raw,"reply_at"))).toString()
}
