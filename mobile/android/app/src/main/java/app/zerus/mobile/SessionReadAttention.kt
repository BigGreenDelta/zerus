package app.zerus.mobile

import org.json.JSONObject

/** Read-state decoration can touch the entire catalog; keep JSON copies off the UI thread. */
object SessionReadAttention {
    fun prepare(sessions: List<Session>, state: ConversationReadState, ready: Boolean, heads: Set<String>,
        loaded: Map<Target, List<Event>>): List<Session> {
        val records = HashMap<String, ConversationReadRecord>().apply {
            state.records.forEach { putIfAbsent(ConversationReadPolicies.key(it.target), it) }
        }
        var changed = false
        val result = sessions.map { row ->
            val key = ConversationReadPolicies.key(row.target)
            val record = records[key]
            val unreadCount = if (ready && record?.headComplete == true && key in heads)
                record.headIncoming?.let { (it - record.readThrough).coerceIn(0, Int.MAX_VALUE.toLong()).toInt() } else null
            val loadedCount = if (!ready || record?.incomplete == true || state.records.size >= 500 && record == null) null
                else ConversationReadPolicies.lowerBound(record, loaded[row.target])
            val later = record?.reviewLater == true
            val unread = SharedReadPolicies.canShowReplyBadge(row) && (unreadCount ?: loadedCount ?: 0) > 0
            val acknowledged = SharedReadPolicies.attentionAcknowledged(row)
            if (row.raw.optBoolean("mobile_review_later") == later && row.raw.optBoolean("mobile_unread_reply") == unread &&
                row.raw.optBoolean("attention_acknowledged") == acknowledged) row
            else {
                changed = true
                row.copy(raw = JSONObject(row.raw.toString()).put("mobile_review_later", later)
                    .put("mobile_unread_reply", unread).put("attention_acknowledged", acknowledged))
            }
        }
        return if (changed) result else sessions
    }
}
