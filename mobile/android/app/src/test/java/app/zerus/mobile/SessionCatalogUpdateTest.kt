package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionCatalogUpdateTest {
    private val target=Target("computer","codex/project/session","run","conversation","workspace")
    private fun row(t:Target=target)=Session(t,"Session","codex","","Project","Preview",0,
        JSONObject().put("run_id",t.run).put("conversation_id",t.conversation).put("activity","idle").put("phase","idle"))

    @Test fun pollPublishesReadDecorationOnceAndRetainsUnchangedCards() {
        val reads=ConversationReadState().head(target,"epoch",8,8,true).review(target,true)
        val heads=setOf(ConversationReadPolicies.key(target))
        val old=SessionReadAttention.prepare(listOf(row(),row(target.copy(session="other",conversation="other"))),reads,true,heads,emptyMap())
        val fresh=old.map { it.copy(raw=JSONObject(it.raw.toString()).also { json ->
            json.remove("mobile_review_later");json.remove("mobile_unread_reply");json.remove("attention_acknowledged")
        }) }
        val published=SessionCatalogUpdate.prepare(fresh,old,emptyList(),reads,true,heads,emptyMap())
        assertSame(old,published)
        assertTrue(published.first().raw.optBoolean("mobile_review_later"))
        assertTrue(published.first().raw.optBoolean("mobile_unread_reply"))
        val changed=SessionCatalogUpdate.prepare(fresh.mapIndexed { i,it -> if(i==1) it.copy(preview="A new message") else it },old,emptyList(),reads,true,heads,emptyMap())
        assertSame(old.first(),changed.first())
        assertEquals("A new message",changed.last().preview)
        val nextRun=SessionCatalogUpdate.prepare(listOf(row(target.copy(run="new-run",conversation="new-conversation"))),old,emptyList(),reads,true,heads,emptyMap())
        assertFalse(nextRun.single().raw.optBoolean("mobile_unread_reply"))
        assertFalse(nextRun.single().raw.optBoolean("mobile_review_later"))
    }
}
