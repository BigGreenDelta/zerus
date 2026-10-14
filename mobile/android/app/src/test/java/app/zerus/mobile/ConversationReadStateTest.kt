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
    private fun reply(sequence:Long)=Event("history:reply-$sequence","AgentMessage","Reply $sequence","",100.0-sequence,
        source="fixture",nativeType="AgentMessage",historyId="reply-$sequence",historyCursor="cursor-$sequence",
        historyEpoch="epoch",incomingSeq=sequence)
    private fun own()=Event("history:own","You","Tall outgoing summary","",0.0,source="fixture",nativeType="UserPromptSubmit",
        historyId="own",historyCursor="own-cursor",historyEpoch="epoch")
    @Test fun ownCanonicalPositionReadsItsIncomingPrefixWithoutCountingOwnMessage() {
        val replies=(1L..19L).map(::reply)
        val own=own()
        val evidence=ConversationReadPolicies.incomingThroughPosition("epoch",replies+own,listOf(own))
        val read=ConversationReadState().head(target,"epoch",19,19,true).visible(target,evidence)
        assertEquals(19,evidence.size)
        assertFalse(evidence.any { it.role=="You" })
        assertEquals(19L,read.record(target)!!.readThrough)
        assertEquals(0,ConversationReadPolicies.lowerBound(read.record(target),replies+own))
        // A current head may be newer than the visible window; never read it by inference.
        assertEquals(1,ConversationReadPolicies.lowerBound(read.head(target,"epoch",20,20,true).record(target),replies+own))
    }
    @Test fun canonicalOrderBeforeMiddleOwnRowLeavesLaterRepliesUnread() {
        val first=reply(1);val later=reply(2);val own=own()
        // Deliberately non-monotonic clocks: window order, not timestamps, proves position.
        val ordered=listOf(first,own,later)
        val evidence=ConversationReadPolicies.incomingThroughPosition("epoch",ordered,listOf(own))
        val read=ConversationReadState().head(target,"epoch",2,2,true).visible(target,evidence)
        assertEquals(listOf(first),evidence)
        assertEquals(1,ConversationReadPolicies.lowerBound(read.record(target),ordered))
    }
    @Test fun recordedOutgoingDisplayAliasKeepsProofButLocalOutboxDoesNot() {
        val first=reply(1);val own=own()
        val alias=own.copy(id="outgoing:request",outgoingId="request")
        assertEquals(listOf(first),ConversationReadPolicies.incomingThroughPosition("epoch",listOf(first,own),listOf(alias)))
        assertTrue(ConversationReadPolicies.incomingThroughPosition("epoch",listOf(first,own),
            listOf(alias.copy(historyId="",historyCursor=""))).isEmpty())
    }
    @Test fun ownPositionRequiresExactEpochCursorAndNativeIdentity() {
        val first=reply(1);val own=own();val ordered=listOf(first,own)
        listOf(own.copy(historyEpoch="other"),own.copy(historyCursor="other"),own.copy(historyId="other"),
            own.copy(source="other"),own.copy(nativeType="ToolFinished")).forEach { unproven ->
            assertTrue(ConversationReadPolicies.incomingThroughPosition("epoch",ordered,listOf(unproven)).isEmpty())
        }
        assertTrue(ConversationReadPolicies.incomingThroughPosition("",ordered,listOf(own)).isEmpty())
        assertTrue(ConversationReadPolicies.incomingThroughPosition("epoch",listOf(first,own,own),listOf(own)).isEmpty())
    }
    @Test fun legacyRowsAcknowledgeOnlyIncomingRowsActuallyVisible() {
        val first=reply(1).copy(historyId="",historyCursor="",historyEpoch="",incomingSeq=null)
        val own=own().copy(historyId="",historyCursor="",historyEpoch="")
        assertTrue(ConversationReadPolicies.incomingThroughPosition("epoch",listOf(first,own),listOf(own)).isEmpty())
        assertEquals(listOf(first),ConversationReadPolicies.incomingThroughPosition("epoch",listOf(first,own),listOf(first)))
    }
    @Test fun ownPositionResamplesPrefixMetadataWhileStreamingTextPreservesDwell() {
        val own=own();val provisional=reply(1).copy(incomingSeq=null)
        fun sample(rows:List<Event>,shown:Event=own)=ConversationReadPolicies.visibilityEvidence(
            ConversationReadPolicies.incomingThroughPosition("epoch",rows,listOf(shown)))
        val before=sample(listOf(provisional,own))
        val after=sample(listOf(reply(1),own))
        assertNotEquals(before,after)
        assertEquals(after,sample(listOf(reply(1).copy(text="Streaming text"),own.copy(text="More summary text")),own.copy(text="More summary text")))
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
