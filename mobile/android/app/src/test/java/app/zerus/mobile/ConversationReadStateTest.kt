package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ConversationReadStateTest {
    private val target=Target("computer","session","run","conversation","workspace")
    @Test fun openingAndViewportDoNotAcknowledgeMessages() {
        val state=ConversationReadState().viewport(target,"incoming",24)
        assertTrue(state.record(target)!!.seen.isEmpty())
        assertEquals(SavedConversationViewport("incoming",24),state.record(target)!!.viewport)
    }
    @Test fun viewedRowsAndAnchorSurviveRestartWithoutCrossConversationLeak() {
        val state=ConversationReadState().viewed(target,setOf("incoming:1","incoming:2")).viewport(target,"incoming:1",30)
        val restored=ConversationReadPolicies.decode(ConversationReadPolicies.encode(state))
        assertEquals(state,restored)
        assertNull(restored.record(target.copy(conversation="other")))
        assertNull(restored.record(target.copy(connectionId="other")))
        assertNull(restored.record(target.copy(agentId="child",parentConversation="conversation")))
        assertEquals(state.record(target),restored.record(target.copy(run="resumed")))
    }
    @Test fun capacityDoesNotEraseSeenRowsOrInventExactUnreadCounts() {
        val ids=(1..5000).map { "message:$it" }.toSet()
        val state=ConversationReadState().viewed(target,ids).viewed(target,setOf("new"))
        assertEquals(ids,state.record(target)!!.seen.toSet())
        assertTrue(state.record(target)!!.incomplete)
    }
    @Test fun toolsAndOwnRowsNeverAdvanceIncomingReadState() {
        assertFalse(ConversationReadPolicies.incoming(Event("own","You","text","",nativeType="UserPromptSubmit")))
        assertFalse(ConversationReadPolicies.incoming(Event("tool","Tool","text","",nativeType="ToolFinished")))
        assertTrue(ConversationReadPolicies.incoming(Event("reply","AgentMessage","text","",nativeType="AgentMessage")))
    }
    @Test fun canonicalEvidenceChangesRequireResamplingEvenWhenVisibleIdsStayTheSame() {
        val old=Event("reply","AgentMessage","Same visible text","",nativeType="AgentMessage",historyEpoch="old",incomingSeq=10)
        val state=ConversationReadState().head(target,"old",10,10,true).visible(target,listOf(old))
        val fresh=old.copy(historyEpoch="new",incomingSeq=12)
        val rebuilt=state.head(target,"new",12,12,true)
        assertEquals(12,ConversationReadPolicies.lowerBound(rebuilt.record(target),listOf(fresh)))
        assertNotEquals(ConversationReadPolicies.visibilityEvidence(listOf(old)),ConversationReadPolicies.visibilityEvidence(listOf(fresh)))
        val resampled=rebuilt.visible(target,listOf(fresh))
        assertEquals(0,ConversationReadPolicies.lowerBound(resampled.record(target),listOf(fresh)))
    }
    @Test fun sameEpochSequencePromotionIsReadEvidenceButStreamingTextAloneIsNot() {
        val row=Event("reply","AgentMessage","Streaming text","",nativeType="AgentMessage",historyEpoch="epoch")
        assertNotEquals(ConversationReadPolicies.visibilityEvidence(listOf(row)),ConversationReadPolicies.visibilityEvidence(listOf(row.copy(incomingSeq=12))))
        assertEquals(ConversationReadPolicies.visibilityEvidence(listOf(row)),ConversationReadPolicies.visibilityEvidence(listOf(row.copy(text="More streaming text",at=20.0))))
        assertTrue(ConversationReadPolicies.visibilityEvidence(listOf(row.copy(role="You"),row.copy(nativeType="ToolFinished"))).isEmpty())
    }
    @Test fun encryptedIndexContainsOnlyNavigationDisplayFields() {
        val raw=JSONObject().put("state","running").put("account_id","secret-account").put("token","secret-token").put("mobile_capabilities",JSONObject().put("operations","send"))
        val session=Session(target,"Title","codex","busy","Project","Preview",0,raw)
        val row=ConversationIndex.capture(listOf(session),listOf(Machine("workspace","computer","Computer",true))).single()
        val encoded=ConversationIndex.encode(row).toString()
        assertFalse(encoded.contains("secret"));assertFalse(encoded.contains("capabilities"))
        val restored=ConversationIndex.decode(JSONObject(encoded)).session()
        assertTrue(restored.raw.optBoolean("last_known"));assertEquals(target,restored.target)
    }
}
