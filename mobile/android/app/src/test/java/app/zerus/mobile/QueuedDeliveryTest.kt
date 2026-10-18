package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** OpenCode inbox rows: queued before the agent reads them, not delivered after Stop. */
class QueuedDeliveryTest {
    private val target = Target("computer","opencode/project/local","run","ses_fixture","workspace")
    private val request = "11111111-1111-4111-8111-111111111111"
    private fun prompt(id:String,text:String,state:String?,at:Int) = JSONObject().put("type","UserPromptSubmit").put("message_id",id)
        .put("detail",text).put("at",at).also { if(state != null) it.put("delivery_state",state) }
    private fun inspection(vararg rows:JSONObject) = JSONObject().put("events",JSONArray(rows.toList()))
        .put("allowed_actions",JSONArray(listOf("rename","forget","remove_queued")))

    @Test fun queuedAndUndeliveredUserRowsShowTheirNativeState() {
        val events = NativeParser.events(inspection(prompt("msg_delivered","Read",null,1),
            prompt("msg_waiting","Later",  "queued",2),prompt("msg_stopped","After stop","not_delivered",3)))
        assertEquals(listOf("","queued","not_delivered"),events.map { it.delivery })
        assertEquals(listOf("","Queued","Not delivered"),events.map { MessageDeliveryPresentation.label(it.delivery) })
        assertFalse(MessageDeliveryPresentation.warning("queued"));assertTrue(MessageDeliveryPresentation.warning("not_delivered"))
        // Unknown states and agent rows never invent a delivery label.
        val other = NativeParser.events(inspection(prompt("msg_x","Odd","delivered",4),
            JSONObject().put("type","AgentMessage").put("message_id","msg_y").put("detail","Reply").put("at",5).put("delivery_state","queued")))
        assertEquals(listOf("",""),other.map { it.delivery })
        // Existing outgoing labels are unchanged.
        assertEquals(listOf("Sending…","Delivery unknown","Not sent","Sent","Sent"),
            listOf("sending","uncertain","failed","submitted","recorded").map(MessageDeliveryPresentation::label))
    }

    @Test fun aMatchedPhoneMessageKeepsItsNativeQueueState() {
        val native = NativeParser.events(inspection(prompt("msg_waiting","From the phone","queued",100))).single()
        val sent = OutgoingMessage(request,target,"From the phone",emptyList(),99.0,"submitted",100.0)
        val merged = ConversationMerge.merge(target,listOf(native),listOf(sent))
        assertEquals(setOf(request),merged.recordedRequestIds)
        assertEquals("queued",merged.events.single().delivery)
        assertEquals("recorded",ConversationMerge.merge(target,listOf(native.copy(delivery = "")),listOf(sent)).events.single().delivery)
    }

    @Test fun phonesNeverOfferRemoveForUndeliveredMessages() {
        assertFalse("remove_queued" in SessionActionPolicies.operations)
        assertThrows(IllegalArgumentException::class.java) {
            SessionActionPolicies.arguments("remove_queued",JSONObject().put("message_id","msg_stopped"))
        }
    }
}
