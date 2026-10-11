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
    @Test fun childArchivedOrChangedRowsAreNeverMarked() {
        assertNull(SharedReadPolicies.payload(row(t=target.copy(agentId="child",parentConversation="conversation")),true,0))
        assertNull(SharedReadPolicies.payload(row(t=target.copy(archiveId="55555555-5555-4555-8555-555555555555")),true,0))
        val changed=row();changed.raw.put("run_id","resumed-run")
        assertNull(SharedReadPolicies.payload(changed,true,0))
        assertNull(SharedReadPolicies.payload(row(reply="",attention="not-a-signature"),true,0))
    }
}
