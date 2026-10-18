package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/**
 * hgs keeps OpenCode inbox rows (`delivery_state`) out of durable history, so with a paged head
 * window they exist only in the inspection: transient rows after the durable tail, never paged.
 */
class QueuedHistoryWindowTest {
    private val target = Target("computer","opencode/project/local","run","ses_fixture","workspace")
    private val request = "11111111-1111-4111-8111-111111111111"
    private fun row(type:String,id:String,text:String,at:Int,state:String? = null) = JSONObject().put("type",type).put("source","opencode_native")
        .put("message_id",id).put("detail",text).put("at",at).also { if(state != null) it.put("delivery_state",state) }
    private fun inspected(vararg rows:JSONObject) = NativeParser.events(JSONObject().put("events",JSONArray(rows.toList())))
    /** A canonical row as hgs history returns it, aliased to its native journal identity. */
    private fun durable(n:Int,role:String,type:String,id:String,text:String) = Event("history:h$n",role,text,"",n.toDouble(),source="opencode_native",
        nativeType=type,originalId="journal:opencode_native:$role:native:$id",originalIds=listOf("journal:opencode_native:$role:native:$id"),
        historyId="h$n",historyCursor="c$n",historyEpoch="epoch",incomingSeq=if(role=="You") null else n.toLong())
    private val reply = durable(1,"AgentMessage","AgentMessage","msg_A","Done")
    private val head = HistoryWindow(target,"epoch",listOf(reply),null,"c1",false,false,true)
    private val replyRow = row("AgentMessage","msg_A","Done",1)
    private val waiting = inspected(replyRow,row("UserPromptSubmit","msg_Q","Later",3,"queued"))

    @Test fun queuedRowFollowsTheDurableTailWithoutBecomingHistory() {
        val next = head.inspection(waiting)
        assertEquals(listOf("history:h1"),next.events.map { it.id })
        assertEquals(listOf("history:h1","journal:opencode_native:You:native:msg_Q"),next.displayed.map { it.id })
        assertEquals("queued",next.displayed.last().delivery)
        // The cursor, head flags and retained pages ignore it.
        assertEquals("c1",next.after);assertTrue(next.atHead);assertFalse(next.hasAfter)
        assertEquals("after" to "c1",HistoryRequests.refreshAtHead(next))
        val retained = checkNotNull(HistoryPage.retained(next,"c1"))
        assertEquals(1,retained.getJSONArray("rows").length());assertFalse(retained.getBoolean("has_after"))
        // It is neither new canonical history nor an unread reply.
        assertFalse(HistoryInspection.changed(head,waiting,emptyList()))
        assertEquals(listOf("history:h1"),next.displayed.filter(ConversationReadPolicies::incoming).map { it.id })
        // The same rows through the retained-window guard; a stopped session relabels it.
        assertEquals(next.displayed,HistoryInspection.retain(head,null,waiting,emptyList()).displayed)
        val stopped = next.inspection(inspected(replyRow,row("UserPromptSubmit","msg_Q","Later",3,"not_delivered")),waiting)
        assertEquals("not_delivered",stopped.displayed.last().delivery)
        // Away from the head the window cannot claim what follows its last page.
        assertEquals(listOf("history:h1"),next.copy(atHead = false,hasAfter = true).displayed.map { it.id })
        assertEquals(listOf("history:h1"),head.copy(atHead = false,hasAfter = true).inspection(waiting).displayed.map { it.id })
    }

    @Test fun deliveredRowReplacesTheTransientRowExactlyOnce() {
        val queued = head.inspection(waiting)
        val admitted = inspected(replyRow,row("UserPromptSubmit","msg_Q","Later",3))
        val afterDelivery = queued.inspection(admitted,waiting)
        assertEquals(listOf("history:h1"),afterDelivery.displayed.map { it.id })
        val delivered = durable(2,"You","UserPromptSubmit","msg_Q","Later")
        val page = HistoryPage(target,"epoch",listOf(delivered),"c2","c2",true,false,null,null,false,false,false,JSONObject())
        assertEquals(listOf("history:h1","history:h2"),afterDelivery.merge(page,"after").displayed.map { it.id })
        // The page can arrive while the inspection still reports the row queued.
        val race = queued.merge(page,"after")
        assertEquals(listOf("history:h1","history:h2"),race.displayed.map { it.id })
        assertEquals("",race.displayed.last().delivery)
    }

    @Test fun removedRowDisappearsWithTheNextInspection() {
        val queued = head.inspection(waiting)
        assertEquals(listOf("history:h1"),queued.inspection(inspected(replyRow),waiting).displayed.map { it.id })
    }

    @Test fun phoneMessageAndItsQueuedRowShowOneBubble() {
        val queued = head.inspection(waiting)
        val sent = OutgoingMessage(request,target,"Later",emptyList(),2.5,"submitted",3.0)
        val merged = ConversationMerge.merge(target,queued.displayed,listOf(sent))
        assertEquals(setOf(request),merged.recordedRequestIds)
        val own = merged.events.filter { it.role == "You" }
        assertEquals(1,own.size);assertEquals("queued",own.single().delivery)
    }
}
