package app.zerus.mobile

import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.yield
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SharedReadTest {
    private val target=Target("computer","codex/project/review","run","conversation","gateway")
    private val signature="a".repeat(64)
    private fun row(read:JSONObject?=null,reply:String="7:100.5",attention:String?=signature,t:Target=target)=Session(t,"Review","codex","input","project","",0,
        JSONObject().put("run_id",t.run).put("conversation_id",t.conversation).put("reply_id",reply)
            .put("attention_signature",attention ?: JSONObject.NULL).put("read",read ?: JSONObject.NULL))
    private fun mark(reply:String?,attention:String?,conversation:String="conversation")=JSONObject().put("conversation_id",conversation)
        .put("reply_id",reply ?: JSONObject.NULL).put("attention_signature",attention ?: JSONObject.NULL).put("at",1.0)

    @Test fun expiredJournalReplyCannotResurrectOldPhoneHistoryAsNewDesktopReply() {
        val expired=row(mark("7:100.5",null),reply="",attention=null)
        assertFalse(SharedReadPolicies.replyRead(expired)) // No invented current reply identity.
        assertFalse(SharedReadPolicies.canShowReplyBadge(expired))
        assertTrue(SharedReadPolicies.canShowReplyBadge(row(reply="",attention=null))) // No shared read proof.
        assertFalse(ReadAllPolicies.listCandidate(expired,true))
        val reads=ConversationReadState().head(target,"epoch",20,20,true)
        val decorated=SessionReadAttention.prepare(listOf(expired),reads,true,setOf(ConversationReadPolicies.key(target)),emptyMap()).single()
        assertFalse(decorated.raw.optBoolean("mobile_unread_reply"))
        assertEquals(0L,reads.record(target)!!.readThrough) // No historical message is acknowledged.
        assertTrue(SharedReadPolicies.canShowReplyBadge(row(mark("7:100.5",null),reply="8:200.5")))
        assertFalse(SharedReadPolicies.canShowReplyBadge(row(mark("8:200.5",null),reply="8:200.5")))
        assertTrue(ReadAllPolicies.listCandidate(expired.copy(raw=JSONObject(expired.raw.toString()).put("mobile_review_later",true)),true))
        val legacy=expired.copy(raw=JSONObject(expired.raw.toString()).also { it.remove("attention_signature") })
        assertTrue(SharedReadPolicies.canShowReplyBadge(legacy))
    }

    @Test fun payloadCarriesExactIdentityOnlyAfterThisPhoneShowedTheReply() {
        val payload=SharedReadPolicies.payload(row(),true,0)!!
        assertEquals(setOf("run_id","conversation_id","reply_id","attention_signature"),payload.keys().asSequence().toSet())
        assertEquals("run",payload.getString("run_id"));assertEquals("conversation",payload.getString("conversation_id"))
        assertEquals("7:100.5",payload.getString("reply_id"));assertEquals(signature,payload.getString("attention_signature"))
        assertNull(SharedReadPolicies.payload(row(),false,0))   // no fresh head for this exact reply
        assertNull(SharedReadPolicies.payload(row(),true,2))    // newer messages not shown yet
        assertNull(SharedReadPolicies.payload(row(),true,null)) // unknown local count
    }
    @Test fun hostMarksCoverOnlyTheExactCurrentValues() {
        val read=row(mark("7:100.5",signature))
        assertTrue(SharedReadPolicies.replyRead(read));assertTrue(SharedReadPolicies.attentionAcknowledged(read))
        assertNull(SharedReadPolicies.payload(read,true,0))
        val newer=row(mark("7:100.5",signature),reply="8:200.5",attention="b".repeat(64))
        assertFalse(SharedReadPolicies.replyRead(newer));assertFalse(SharedReadPolicies.attentionAcknowledged(newer))
        val partial=SharedReadPolicies.payload(row(mark("7:100.5",null)),true,0)!!
        assertTrue(partial.isNull("reply_id"));assertEquals(signature,partial.getString("attention_signature"))
        assertFalse(SharedReadPolicies.replyRead(row(mark("7:100.5",signature,"other-conversation"))))
    }
    @Test fun readAllSharesRepliesButNeverAcknowledgesAttention() {
        val payload=SharedReadPolicies.payload(row(),true,0,acknowledge=false)!!
        assertEquals("7:100.5",payload.getString("reply_id"));assertTrue(payload.isNull("attention_signature"))
        // Only a request is pending: Read all leaves it for the person to view.
        assertNull(SharedReadPolicies.payload(row(mark("7:100.5",null)),true,0,acknowledge=false))
    }
    @Test fun selectedNewReplyCannotBorrowTheFleetPreviousReplyMark() {
        val fleet=row(mark("R1",signature),reply="R1")
        val selected=row(reply="R2")
        val before=ConversationReadState(listOf(ConversationReadRecord(target,epoch="epoch",readThrough=1,
            headIncoming=2,headTotal=2,headComplete=true,reviewLater=true)))
        val proof=ReadAttentionEvidence(target,"fresh-selected-R2","epoch",2,true,emptySet(),null)
        // The unrelated head callback must not adopt R1's mark over R2's authoritative head.
        val captures=if(SharedReadPolicies.canAdoptReply(fleet,target,selected.raw,true)) listOf(ReadAllCapture(target,proof)) else emptyList()
        val after=ReadAllPolicies.apply(before,captures,captures).state
        assertEquals(1L,after.record(target)!!.readThrough)
        assertTrue(after.record(target)!!.reviewLater)
        assertEquals(signature,selected.raw.string("attention_signature"))
    }
    @Test fun selectedAdoptionRequiresVerifiedExactRunConversationAndReply() {
        val fleet=row(mark("R2",signature),reply="R2")
        val selected=row(reply="R2")
        assertTrue(SharedReadPolicies.canAdoptReply(fleet,target,selected.raw,true))
        assertFalse(SharedReadPolicies.canAdoptReply(fleet,target,selected.raw,false))
        assertFalse(SharedReadPolicies.canAdoptReply(fleet,target,null,true))
        for(field in listOf("run_id","conversation_id","reply_id")) {
            val changed=JSONObject(selected.raw.toString()).put(field,"different")
            assertFalse(field,SharedReadPolicies.canAdoptReply(fleet,target,changed,true))
            changed.remove(field)
            assertFalse("missing $field",SharedReadPolicies.canAdoptReply(fleet,target,changed,true))
        }
        assertFalse(SharedReadPolicies.canAdoptReply(fleet,target.copy(run="new-run"),selected.raw,true))
        assertFalse(SharedReadPolicies.canAdoptReply(row(mark("R1",signature),reply="R2"),target,selected.raw,true))
        // Matching fleet hints do not independently bind a nonselected canonical prefix.
        assertFalse(SharedReadPolicies.canAdoptReply(fleet,target.copy(session="other"),null,false))
        assertFalse(SharedReadPolicies.canAdoptReply(fleet,null,null,false))
    }
    @Test fun readAllSendsOneMarkAtATimeAndSkipsTargetsThatNoLongerQualify() = runBlocking {
        val qualifying=mutableSetOf("a","b","c")
        val prepared=mutableListOf<String>();val sent=mutableListOf<String>()
        var inFlight=0;var most=0
        SharedReadPolicies.inOrder(listOf("a","b","c"),{ target -> prepared+=target;target.takeIf { it in qualifying } }) { target ->
            inFlight++;most=maxOf(most,inFlight)
            // A newer reply arrives for "b" while the first mark is in flight.
            if(target=="a") qualifying-="b"
            yield();delay(5);inFlight--
            sent+=target
        }
        assertEquals(1,most)
        assertEquals(listOf("a","b","c"),prepared)
        assertEquals(listOf("a","c"),sent)
    }
    @Test fun onlyAppliedMarksCountAsSharedAndUnappliedOnesWaitBeforeAnotherTry() {
        val payload=SharedReadPolicies.payload(row(),true,0)!!
        fun receipt(reply:Boolean,attention:Boolean,state:String="completed")=JSONObject().put("request_id","r").put("state",state)
            .put("result",JSONObject().put("ok",true).put("applied",JSONObject().put("reply_id",reply).put("attention_signature",attention)).put("read",JSONObject.NULL))
        assertTrue(SharedReadPolicies.applied(receipt(true,true),payload))
        // Completed without applying: the request is no longer current, or the computer's question index is not ready.
        assertFalse(SharedReadPolicies.applied(receipt(true,false),payload))
        assertFalse(SharedReadPolicies.applied(receipt(false,false),payload))
        assertFalse(SharedReadPolicies.applied(receipt(true,true,"failed"),payload))
        assertFalse(SharedReadPolicies.applied(JSONObject().put("state","completed").put("result",JSONObject().put("ok",false).put("error","not_found")),payload))
        // A field this phone did not send need not be applied.
        assertTrue(SharedReadPolicies.applied(receipt(true,false),SharedReadPolicies.payload(row(),true,0,acknowledge=false)!!))
        assertEquals(listOf(120_000L,240_000L,480_000L,960_000L,960_000L),(0..4).map(SharedReadPolicies::retryDelayMs))
    }
    @Test fun childArchivedOrChangedRowsAreNeverMarked() {
        assertNull(SharedReadPolicies.payload(row(t=target.copy(agentId="child",parentConversation="conversation")),true,0))
        assertNull(SharedReadPolicies.payload(row(t=target.copy(archiveId="55555555-5555-4555-8555-555555555555")),true,0))
        val changed=row();changed.raw.put("run_id","resumed-run")
        assertNull(SharedReadPolicies.payload(changed,true,0))
        assertNull(SharedReadPolicies.payload(row(reply="",attention="not-a-signature"),true,0))
    }
}
