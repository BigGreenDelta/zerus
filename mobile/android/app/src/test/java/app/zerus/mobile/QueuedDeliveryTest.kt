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

    // OpenCode dates a queued message by its delivery, not by when it was sent: a phone
    // message that waited 12 s in its inbox arrives 12 s after the phone submitted it.
    private val other = "22222222-2222-4222-8222-222222222222"
    private val waited = "Waited in the queue"
    private val phone = OutgoingMessage(request,target,waited,emptyList(),99.0,"submitted",100.0)
    /** An OpenCode row as hgs inspects it; `requestId` names the Zerus send that admitted the prompt. */
    private fun opencode(text:String,at:Int,requestId:Any? = null,state:String? = null,type:String = "UserPromptSubmit",id:String = "msg_waited") =
        prompt(id,text,state,at).put("type",type).put("source","opencode_native").also { if(requestId != null) it.put("request_id",requestId) }
    private fun merged(native:JSONObject,sent:OutgoingMessage = phone) = ConversationMerge.merge(target,NativeParser.events(inspection(native)),listOf(sent))

    @Test fun aLongQueuedPhoneMessageMatchesTheOpenCodeRowNamingItsRequest() {
        val delivered = merged(opencode(waited,112,request))
        assertEquals(setOf(request),delivered.recordedRequestIds)
        assertEquals(listOf("outgoing:$request"),delivered.events.map { it.id })
        assertEquals(listOf("recorded"),delivered.events.map { it.delivery })
        assertEquals(request,delivered.events.single().outgoingId)
        assertFalse(ConversationReadPolicies.incoming(delivered.events.single()))
        // The native inbox state stays visible until OpenCode reads the message.
        assertEquals(listOf("queued"),merged(opencode(waited,112,request,"queued")).events.map { it.delivery })
        assertEquals(setOf(request),merged(opencode(waited,112,request,"queued")).recordedRequestIds)
        assertEquals(listOf("not_delivered"),merged(opencode(waited,112,request,"not_delivered")).events.map { it.delivery })
        // Like a delivery receipt, the exact request settles an uncertain send.
        assertEquals(setOf(request),merged(opencode(waited,112,request),phone.copy(status = "uncertain")).recordedRequestIds)
        // A receipt row naming the same request still wins, as before.
        val both = ConversationMerge.merge(target,NativeParser.events(inspection(opencode(waited,112,request),
            prompt(request,waited,null,113).put("source","hgs_delivery"))),listOf(phone)).events
        assertEquals(listOf("opencode_native" to "","hgs_delivery" to request),both.map { it.source to it.outgoingId })
    }

    @Test fun anOpenCodeRowNamingAnotherRequestIsNeverThisMessage() {
        // Not even with equal text inside the ten-second window.
        for(at in listOf(100,112)) {
            val result = merged(opencode(waited,at,other))
            assertTrue(result.recordedRequestIds.isEmpty())
            assertEquals(2,result.events.size)
            assertEquals("submitted",result.events.single { it.source == "mobile_outbox" }.delivery)
            assertEquals(other,result.events.single { it.source == "opencode_native" }.requestId)
        }
    }

    @Test fun withoutARequestOnlyTheUnchangedTextAndTimeFallbackApplies() {
        // Older computers and prompts typed in OpenCode name no request; a long-queued message
        // then still shows twice.
        val late = merged(opencode(waited,112))
        assertTrue(late.recordedRequestIds.isEmpty())
        assertEquals(listOf("mobile_outbox","opencode_native"),late.events.map { it.source })
        assertEquals(setOf(request),merged(opencode(waited,103)).recordedRequestIds)
    }

    @Test fun aNonCanonicalRequestIdIsIgnored() {
        val lettered = "abcdef12-3456-4789-8abc-def123456789"
        val sent = phone.copy(requestId = lettered)
        for(id in listOf(lettered.uppercase(),"{$lettered}",lettered.replace("-",""),"not-a-uuid","1-1-1-1-1"," $lettered","",42)) {
            assertEquals("",NativeParser.events(inspection(opencode(waited,112,id))).single().requestId)
            assertEquals(2,merged(opencode(waited,112,id),sent).events.size)
            // Treated as a row without a request, so the unchanged fallback still applies.
            assertEquals(setOf(lettered),merged(opencode(waited,103,id),sent).recordedRequestIds)
        }
        assertEquals(lettered,NativeParser.events(inspection(opencode(waited,112,lettered))).single().requestId)
    }

    @Test fun onlyAnOpenCodeUserPromptTakesItsRequestId() {
        assertEquals(request,NativeParser.events(inspection(opencode(waited,1,request))).single().requestId)
        // Other OpenCode rows and other sources ignore it; receipts keep their message ID.
        val rows = NativeParser.events(inspection(
            opencode("Reply",2,request,type = "AgentMessage",id = "msg_reply"),
            opencode("Answer",3,request,type = "QuestionAnswered",id = "msg_answer"),
            prompt("msg_claude","Elsewhere",null,4).put("source","claude_native").put("request_id",request),
            prompt("msg_untagged","Untagged",null,5).put("request_id",request),
            prompt("receipt-id","Delivered",null,6).put("source","hgs_delivery").put("request_id",request),
            prompt("voice-id","Spoken",null,7).put("source","hgs_voice").put("request_id",request)))
        assertEquals(listOf("","","","","receipt-id","voice-id"),rows.map { it.requestId })
    }

    // OpenCode refuses attachments today; generic receipt handling must still treat its rows as before.
    private val expanded = "Compare\n\nRead the attached file at \"/synthetic/request-one/notes.txt\""
    private val receipt = JSONObject().put("type","UserPromptSubmit").put("source","hgs_delivery").put("message_id",request).put("at",101)
        .put("submitted_text",expanded).put("detail","Compare")
        .put("attachments",JSONArray().put(JSONObject().put("name","notes.txt").put("mime","text/plain").put("bytes",7)))

    @Test fun anAttachmentReceiptStandsOnlyForTheOpenCodeRowOfItsOwnRequest() {
        fun parsed(native:JSONObject) = NativeParser.events(inspection(native).put("attachment_messages",JSONArray().put(receipt)))
        val own = parsed(opencode(expanded,102,request))
        assertEquals(listOf("hgs_delivery"),own.map { it.source })
        assertEquals(parsed(opencode(expanded,102)),own)
        assertEquals(listOf("hgs_delivery","opencode_native"),parsed(opencode(expanded,102,other)).map { it.source }.sorted())
    }

    @Test fun rollingHistoryLetsAReceiptSupersedeOnlyTheOpenCodeRowOfItsOwnRequest() {
        val incoming = NativeParser.events(JSONObject().put("attachment_messages",JSONArray().put(receipt)))
        val own = NativeParser.events(inspection(opencode(expanded,102,request)))
        val rolling = RollingConversationHistory.merge(target,target,own,incoming)
        assertEquals(incoming,rolling.events)
        assertEquals(mapOf(own.single().id to incoming.single().id),rolling.supersededIds)
        val foreign = NativeParser.events(inspection(opencode(expanded,102,other)))
        assertEquals(2,RollingConversationHistory.merge(target,target,foreign,incoming).events.size)
        // The private display cache keeps the request a native row names.
        assertEquals(own,EventHistoryCodec.decode(EventHistoryCodec.encode(own)))
    }

    @Test fun phonesNeverOfferRemoveForUndeliveredMessages() {
        assertFalse("remove_queued" in SessionActionPolicies.operations)
        assertThrows(IllegalArgumentException::class.java) {
            SessionActionPolicies.arguments("remove_queued",JSONObject().put("message_id","msg_stopped"))
        }
    }
}
