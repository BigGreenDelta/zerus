package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ReadAllTest {
    private val target=Target("computer","session","run","conversation","workspace")
    private fun evidence(t:Target=target,head:Long=10)=ReadAttentionEvidence(t,"fingerprint","epoch",head,true,emptySet())
    private fun capture(e:ReadAttentionEvidence)=ReadAllCapture(e.target,e)
    private fun session(t:Target=target,raw:JSONObject=JSONObject().put("reply_id","reply"))=Session(t,"Title","codex","idle","Project","reply",0,raw)
    @Test fun batchScopeDeduplicatesWithoutTouchingOtherConversationsOrViewport() {
        val other=target.copy(conversation="other");val outside=target.copy(connectionId="outside")
        val state=ConversationReadState().head(target,"epoch",10,10,true).review(target,true).viewport(target,"anchor",7)
            .head(other,"epoch",10,10,true).head(outside,"epoch",30,30,true)
        val rows=listOf(capture(evidence()),capture(evidence()),capture(evidence(other)))
        val next=ReadAllPolicies.apply(state,rows,rows)
        assertEquals(ReadAllResult(2,0,0),next.result)
        assertEquals(10L,next.state.record(target)!!.readThrough);assertFalse(next.state.record(target)!!.reviewLater)
        assertEquals(state.record(target)!!.viewport,next.state.record(target)!!.viewport)
        assertEquals(state.record(outside),next.state.record(outside))
    }
    @Test fun changedHeadEpochFingerprintAndRunAreSkipped() {
        val initial=ConversationReadState().head(target,"epoch",10,10,true).review(target,true)
        val rows=listOf(capture(evidence()))
        for(changed in listOf(initial.head(target,"epoch",11,11,true),initial.head(target,"new-epoch",10,10,true))) {
            val next=ReadAllPolicies.apply(changed,rows,rows)
            assertEquals(ReadAllResult(0,1,0),next.result);assertEquals(changed,next.state)
        }
        for(current in listOf(listOf(capture(evidence().copy(fingerprint="new"))),listOf(capture(evidence(target.copy(run="new-run")))))) {
            assertEquals(ReadAllResult(0,1,0),ReadAllPolicies.apply(initial,rows,current).result)
        }
    }
    @Test fun changedRowDoesNotPreventIndependentRowFromBeingMarked() {
        val other=target.copy(conversation="other")
        val state=ConversationReadState().head(target,"epoch",11,11,true).review(target,true)
            .head(other,"epoch",10,10,true).review(other,true)
        val rows=listOf(capture(evidence()),capture(evidence(other)))
        val next=ReadAllPolicies.apply(state,rows,rows)
        assertEquals(ReadAllResult(1,1,0),next.result)
        assertEquals(state.record(target),next.state.record(target))
        assertEquals(10L,next.state.record(other)!!.readThrough);assertFalse(next.state.record(other)!!.reviewLater)
    }
    @Test fun partialLoadedRepliesKeepUnknownAndEmptyReminderUnclaimed() {
        val unknown=target.copy(conversation="unknown")
        val reminder=target.copy(conversation="reminder")
        val state=ConversationReadState().review(target,true).review(reminder,true)
        val loaded=ReadAttentionEvidence(target,"","",null,false,setOf("loaded"))
        val empty=ReadAttentionEvidence(reminder,"","",null,false,emptySet())
        val rows=listOf(capture(loaded),ReadAllCapture(unknown,null),capture(empty))
        val next=ReadAllPolicies.apply(state,rows,rows)
        assertEquals(ReadAllResult(1,2,1),next.result)
        assertEquals(listOf("loaded"),next.state.record(target)!!.seen);assertEquals(0L,next.state.record(target)!!.readThrough)
        assertFalse(next.state.record(target)!!.reviewLater);assertTrue(next.state.record(reminder)!!.reviewLater)
        assertNull(next.state.record(unknown));assertFalse(ReadAllPolicies.available(empty))
    }
    @Test fun partialCanonicalUsesCapturedLoadedWatermarkAndNewerRepliesRemainUnread() {
        val state=ConversationReadState().head(target,"epoch",10,10,false)
        val loaded=evidence().copy(authoritative=false,loadedIds=setOf("loaded"),loadedThrough=8)
        val rows=listOf(capture(loaded));val next=ReadAllPolicies.apply(state,rows,rows)
        val record=next.state.record(target)!!
        assertEquals(ReadAllResult(1,0,1),next.result);assertEquals(8L,record.readThrough)
        assertTrue(ConversationReadPolicies.unread(record,Event("new","AgentMessage","reply","",nativeType="AgentMessage",historyEpoch="epoch",incomingSeq=11)))
        assertEquals(ReadAllResult(0,1,0),ReadAllPolicies.apply(state.head(target,"new",10,10,false),rows,rows).result)
    }
    @Test fun batchMarkersAndRemindersPersistAcrossEncodingAndNewHeadsStayUnread() {
        val state=ConversationReadState().head(target,"epoch",10,10,true).review(target,true).viewport(target,"anchor",23)
        val rows=listOf(capture(evidence()));val next=ReadAllPolicies.apply(state,rows,rows).state
        val restored=ConversationReadPolicies.decode(ConversationReadPolicies.encode(next))
        assertEquals(next,restored)
        val newHead=restored.head(target,"epoch",11,11,true).record(target)!!
        assertEquals(10L,newHead.readThrough);assertFalse(newHead.reviewLater);assertEquals("anchor",newHead.viewport!!.eventId)
    }
    private fun reply(id:String,seq:Long?=null,epoch:String="epoch")=Event(id,"AgentMessage","reply","",nativeType="AgentMessage",historyEpoch=if(seq==null) "" else epoch,incomingSeq=seq)
    /** What the phone shows for one conversation: its reminder and the unread lower bound. */
    private fun shown(state:ConversationReadState,rows:List<Event>)=state.record(target).let { (it?.reviewLater==true) to ConversationReadPolicies.lowerBound(it,rows) }
    /** The predicate must agree with what apply() would actually change. */
    private fun assertAgrees(state:ConversationReadState,evidence:ReadAttentionEvidence?,rows:List<Event>,expected:Boolean) {
        val record=state.record(target)
        assertEquals(expected,ReadAllPolicies.changes(record,evidence,rows))
        if(expected) assertTrue(ReadAllPolicies.pending(record,rows))
        val captured=listOf(ReadAllCapture(target,evidence))
        val applied=ReadAllPolicies.apply(state,captured,captured).state
        assertEquals(expected,shown(applied,rows)!=shown(state,rows))
    }
    @Test fun readAllIsActionableOnlyWhenItWouldClearSomethingShown() {
        val head=ConversationReadState().head(target,"epoch",10,10,true)
        val read=ReadAllPolicies.apply(head,listOf(capture(evidence())),listOf(capture(evidence()))).state
        assertAgrees(head,evidence(),emptyList(),true)
        assertAgrees(read,evidence(),emptyList(),false)
        assertFalse(ReadAllPolicies.pending(read.record(target),emptyList()))
        // A reminder alone is something Read all clears when it has evidence.
        assertAgrees(read.review(target,true),evidence(),emptyList(),true)
        // Evidence-less reminders remain: pending, but Read all cannot clear them.
        val reminder=ConversationReadState().review(target,true)
        assertTrue(ReadAllPolicies.pending(reminder.record(target),emptyList()))
        assertAgrees(reminder,null,emptyList(),false)
        assertAgrees(reminder,ReadAttentionEvidence(target,"","",null,false,emptySet()),emptyList(),false)
    }
    @Test fun partialEvidenceIsActionableOnlyForLoadedUnreadReplies() {
        val rows=listOf(reply("a"),reply("b"))
        val loaded=ReadAttentionEvidence(target,"","",null,false,setOf("a","b"))
        assertAgrees(ConversationReadState(),loaded,rows,true)
        val seen=ConversationReadState().viewed(target,setOf("a","b"))
        assertAgrees(seen,loaded,rows,false)
        assertFalse(ReadAllPolicies.pending(seen.record(target),rows))
        assertAgrees(ConversationReadState().viewed(target,setOf("a")),loaded,rows,true)
        // Canonical partial pages use the captured loaded watermark.
        val canonical=listOf(reply("c7",7),reply("c8",8))
        val partial=evidence().copy(authoritative=false,loadedIds=setOf("c7","c8"),loadedThrough=8,head=12)
        val window=ConversationReadState().head(target,"epoch",12,12,false)
        assertAgrees(window,partial,canonical,true)
        val through=ReadAllPolicies.apply(window,listOf(capture(partial)),listOf(capture(partial))).state
        assertEquals(8L,through.record(target)!!.readThrough)
        // Newer replies outside the loaded window stay pending, but these rows add nothing.
        assertTrue(ReadAllPolicies.pending(through.record(target),canonical))
        assertAgrees(through,partial,canonical,false)
    }
    @Test fun readAllPredicateFollowsApplySkipRules() {
        val state=ConversationReadState().head(target,"epoch",10,10,true)
        // Changed head, epoch or incomplete head: apply() skips, so nothing is actionable.
        assertFalse(ReadAllPolicies.changes(state.record(target),evidence(head=11),emptyList()))
        assertFalse(ReadAllPolicies.changes(state.record(target),evidence().copy(epoch="other"),emptyList()))
        val incomplete=ConversationReadState().head(target,"epoch",10,10,false)
        assertAgrees(incomplete,evidence(),emptyList(),false)
        assertTrue(ReadAllPolicies.pending(incomplete.record(target),emptyList()))
        // A new conversation the 500-record cap refuses is skipped as well.
        val rows=listOf(reply("a"));val loaded=ReadAttentionEvidence(target,"","",null,false,setOf("a"))
        assertTrue(ReadAllPolicies.changes(null,loaded,rows))
        assertFalse(ReadAllPolicies.changes(null,loaded,rows,full=true))
        assertFalse(ReadAllPolicies.unreadReplies(ConversationReadState().review(target,true).record(target),emptyList()))
    }
    @Test fun headCandidatesIncludeUnvisitedReplyEvidenceAndRecordedChatsButExcludeArchivesAndUnknown() {
        val recorded=target.copy(conversation="recorded")
        val state=ConversationReadState().review(recorded,true)
        val unknown=target.copy(conversation="unknown")
        val archived=target.copy(conversation="archive",archiveId="archive-id")
        val rows=listOf(session(),session(recorded,JSONObject()),session(unknown,JSONObject()),session(archived),session(target.copy(conversation="",run="")))
        assertEquals(listOf(target,recorded),ReadAllHeadCandidates.candidates(rows,state).map { it.target })
        assertNotEquals(ReadAllHeadCandidates.hint(session()),ReadAllHeadCandidates.hint(session(target.copy(run="new-run"))))
    }
    @Test fun metadataWarmupRespectsRecordCapAndDistinctConversationKeys() {
        val records=(0 until 499).map { ConversationReadRecord(target.copy(conversation="recorded-$it")) }
        val rows=listOf(session(),session(),session(target.copy(conversation="second")),session(records.first().target,JSONObject()))
        assertEquals(listOf(target,records.first().target),ReadAllHeadCandidates.candidates(rows,ConversationReadState(records)).map { it.target })
        val full=ConversationReadState(records+ConversationReadRecord(target.copy(conversation="last")))
        assertEquals(listOf(records.first().target),ReadAllHeadCandidates.candidates(rows,full).map { it.target })
    }
}
