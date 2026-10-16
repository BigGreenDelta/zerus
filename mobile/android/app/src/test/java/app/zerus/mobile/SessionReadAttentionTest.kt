package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionReadAttentionTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private fun row(target: Target = this.target) = Session(target, "Session", "codex", "", "Project", "", 0, JSONObject())

    @Test fun decorationKeepsExactReadHeadsLoadedFallbackAndSharedReadIdentity() {
        val reply = Event("reply", "AgentMessage", "Reply", "", nativeType = "AgentMessage")
        val state = ConversationReadState().head(target, "epoch", 8, 8, true).review(target, true)
        val current = row()
        val other = row(target.copy(connectionId = "another-workspace"))
        val rows = SessionReadAttention.prepare(listOf(current, other), state, true, setOf(ConversationReadPolicies.key(target)), emptyMap())
        assertTrue(rows.first().raw.optBoolean("mobile_review_later"))
        assertTrue(rows.first().raw.optBoolean("mobile_unread_reply"))
        assertSame(other, rows.last())
        assertFalse(current.raw.has("mobile_unread_reply"))
        val loaded = SessionReadAttention.prepare(listOf(current), ConversationReadState(), true, emptySet(), mapOf(target to listOf(reply)))
        assertTrue(loaded.single().raw.optBoolean("mobile_unread_reply"))
        assertSame(loaded.single(), SessionReadAttention.prepare(loaded, ConversationReadState(), true, emptySet(), mapOf(target to listOf(reply))).single())
        val read = current.copy(raw = JSONObject().put("conversation_id", target.conversation).put("reply_id", "reply")
            .put("read", JSONObject().put("conversation_id", target.conversation).put("reply_id", "reply")))
        assertFalse(SessionReadAttention.prepare(listOf(read), state, true, emptySet(), mapOf(target to listOf(reply))).single().raw.optBoolean("mobile_unread_reply"))
    }

    @Test fun incompleteOrUnopenedReadStateDoesNotClaimUnreadKnowledge() {
        val record = ConversationReadRecord(target, epoch = "epoch", headIncoming = 8, headComplete = false, incomplete = true)
        val state = ConversationReadState(listOf(record))
        val original = row()
        assertSame(original, SessionReadAttention.prepare(listOf(original), state, true, emptySet(), emptyMap()).single())
        assertSame(original, SessionReadAttention.prepare(listOf(original), state, false, emptySet(), emptyMap()).single())
    }
}
