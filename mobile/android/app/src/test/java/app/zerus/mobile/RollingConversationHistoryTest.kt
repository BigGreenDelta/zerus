package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RollingConversationHistoryTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private fun row(id: String, at: Double, text: String = id) = Event(id, "You", text, "", at)
    @Test fun toolStarvationRefreshKeepsConfirmedOwnRowAndReplacesStableNativeKeys() {
        val own = row("journal:1", 1.0, "Own confirmed message")
        val reply = row("provider:2", 2.0, "Partial")
        val result = RollingConversationHistory.merge(target, target, listOf(own, reply), listOf(reply.copy(text = "Complete"), row("provider:3", 3.0)))
        assertEquals(listOf("journal:1", "provider:2", "provider:3"), result.events.map { it.id })
        assertEquals("Complete", result.events[1].text)
        assertFalse(result.truncated)
    }
    @Test fun everyExactTargetIdentityPreventsCrossConversationHistoryReuse() {
        listOf(target.copy(connectionId = "other"), target.copy(computerId = "other"), target.copy(session = "other"),
            target.copy(run = "other"), target.copy(conversation = "other"), target.copy(archiveId = "archive")).forEach { previous ->
            val result = RollingConversationHistory.merge(target, previous, listOf(row("old", 1.0)), listOf(row("fresh", 2.0)))
            assertEquals(listOf("fresh"), result.events.map { it.id })
        }
    }
    @Test fun repeatedTextWithDistinctIdsSurvivesAndOldestRowsAreEvictedByCount() {
        val rows = (0..9).map { row("id$it", it.toDouble(), "Same text") }
        val result = RollingConversationHistory.merge(target, target, rows, emptyList(), maxEvents = 3)
        assertEquals(listOf("id7", "id8", "id9"), result.events.map { it.id }); assertTrue(result.truncated)
    }
    @Test fun serializedByteLimitIncludesUnicodeAndJsonEscapingAndKeepsWholeRows() {
        val rows = (0..9).map { row("id$it", it.toDouble(), "🦀\n\"".repeat(100)) }
        val result = RollingConversationHistory.merge(target, target, rows, emptyList(), maxBytes = 2000)
        assertTrue(result.truncated)
        assertTrue(EventHistoryCodec.encode(result.events).toString().toByteArray(Charsets.UTF_8).size <= 2000)
        assertTrue(result.events.all { it.text == rows.last().text })
    }
    @Test fun privateDisplayCodecPreservesImmutableReceiptAndAttachmentIdentity() {
        val event = row("outgoing:id", 1.0).copy(requestId = "receipt", source = "hgs_delivery", outgoingId = "receipt", delivery = "recorded",
            attachments = listOf(DisplayedAttachment("fixture.txt", "text/plain", 7, "[File #1]")))
        assertEquals(listOf(event), EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(event))))
    }
    @Test fun recordedOutboxDoesNotDisappearWhenNativeTailRollsPastIt() {
        val outgoing = OutgoingMessage("receipt", target, "Own message", emptyList(), 1.0, "recorded", 2.0)
        val result = ConversationMerge.merge(target, emptyList(), listOf(outgoing))
        assertEquals("outgoing:receipt", result.events.single().id)
        assertEquals("recorded", result.events.single().delivery)
        assertEquals(2.0, result.events.single().at, 0.0)
        assertTrue(result.recordedRequestIds.isEmpty())
    }
    @Test fun mainMessageWindowUsesJournalIdsAndDoesNotDuplicateOverlappingRows() {
        val own = JSONObject().put("type", "UserPromptSubmit").put("seq", 1).put("at", 1).put("detail", "Own message")
        val before = NativeParser.events(JSONObject().put("events", JSONArray().put(own)))
        val after = NativeParser.events(JSONObject().put("events", JSONArray().put(own)).put("message_events", JSONArray().put(own)))
        assertEquals(before, after)
        val cold = NativeParser.events(JSONObject().put("events", JSONArray()).put("message_events", JSONArray().put(own)))
        assertEquals(before, cold)
    }
    @Test fun confirmedAnswerIsDisplayedButCannotConfirmAnOrdinarySendByMatchingTextAndTime() {
        val answer = JSONObject().put("type", "QuestionAnswered").put("seq", 1).put("at", 2).put("detail", "Same text")
        val events = NativeParser.events(JSONObject().put("message_events", JSONArray().put(answer)))
        assertEquals("You (answer)", events.single().role)
        val outgoing = OutgoingMessage("receipt", target, "Same text", emptyList(), 1.0, "submitted", 2.0)
        val merged = ConversationMerge.merge(target, events, listOf(outgoing))
        assertEquals(2, merged.events.size)
        assertTrue(merged.recordedRequestIds.isEmpty())
    }
    @Test fun latePublicProviderReplySupersedesOnlyItsHookExcerpt() {
        val hook = JSONObject().put("type", "Stop").put("seq", 1).put("at", 2).put("detail", "Public reply")
        val before = NativeParser.events(JSONObject().put("events", JSONArray().put(hook)))
        val provider = JSONObject().put("type", "AgentMessage").put("message_id", "reply").put("at", 3).put("detail", "Public reply with full detail")
        val incoming = NativeParser.events(JSONObject().put("provider_messages", JSONArray().put(provider)))
        val result = RollingConversationHistory.merge(target, target, before, incoming)
        assertEquals(incoming, result.events)
        val unrelated = incoming.single().copy(id = "provider:other", at = 20.0)
        assertEquals(2, RollingConversationHistory.merge(target, target, before, listOf(unrelated)).events.size)
    }
    @Test fun lateAttachmentReceiptReplacesOnlyOneExactOriginalPromptAndPreservesRepeatedPrompts() {
        val prompt = JSONObject().put("type", "UserPromptSubmit").put("seq", 1).put("at", 2).put("detail", "Exact submitted native text")
        val previous = NativeParser.events(JSONObject().put("events", JSONArray().put(prompt)))
        val receipt = JSONObject().put("type", "UserPromptSubmit").put("source", "hgs_delivery").put("message_id", "receipt").put("at", 3)
            .put("submitted_text", "Exact submitted native text").put("detail", "Display without private native path")
            .put("attachments", JSONArray().put(JSONObject().put("name", "fixture.txt").put("mime", "text/plain").put("bytes", 7)))
        val incoming = NativeParser.events(JSONObject().put("attachment_messages", JSONArray().put(receipt)))
        assertEquals(incoming, RollingConversationHistory.merge(target, target, previous, incoming).events)
        val repeated = previous.single().copy(id = "journal:distinct-other-seq")
        assertEquals(3, RollingConversationHistory.merge(target, target, previous + repeated, incoming).events.size)
        val unrelated = previous.single().copy(at = 20.0)
        assertEquals(2, RollingConversationHistory.merge(target, target, listOf(unrelated), incoming).events.size)
    }
    @Test fun delayedAttachmentAcknowledgementUsesExactExpandedPathsAndPreservesIndependentSends() {
        val text = "Compare images"
        val submitted = text + "\n\nInspect the attached image at \"/synthetic/request-one/image.png\""
        val prompt = JSONObject().put("type", "UserPromptSubmit").put("seq", 1).put("at", 202).put("detail", submitted)
        val receipt = JSONObject().put("type", "UserPromptSubmit").put("source", "hgs_delivery").put("message_id", "receipt").put("at", 100)
            .put("submitted_text", submitted).put("detail", text)
            .put("attachments", JSONArray().put(JSONObject().put("name", "image.png").put("mime", "image/png").put("bytes", 120)))
        val previous = NativeParser.events(JSONObject().put("events", JSONArray().put(prompt)))
        val incoming = NativeParser.events(JSONObject().put("attachment_messages", JSONArray().put(receipt)))
        val rolling = RollingConversationHistory.merge(target,target,previous,incoming)
        assertEquals(listOf(text),rolling.events.map { it.text })
        assertEquals(mapOf(previous.single().id to incoming.single().id),rolling.supersededIds)
        val joint = NativeParser.events(JSONObject().put("events",JSONArray().put(prompt)).put("attachment_messages",JSONArray().put(receipt)))
        assertEquals(listOf(text),joint.map { it.text })
        assertEquals(listOf(previous.single().id),joint.single().originalIds)
        assertEquals(202.0,joint.single().at,0.0)
        assertEquals(1,RollingConversationHistory.merge(target,target,previous,joint).events.size)
        val independent = previous.single().copy(id="journal:other",text=submitted.replace("request-one","request-two"),at=203.0)
        assertEquals(2,RollingConversationHistory.merge(target,target,previous+independent,incoming).events.size)
        val ambiguous = previous.single().copy(id="journal:other")
        assertEquals(3,RollingConversationHistory.merge(target,target,previous+ambiguous,incoming).events.size)
        val otherReceipt = JSONObject(receipt.toString()).put("message_id","other-receipt")
        assertEquals(3,NativeParser.events(JSONObject().put("events",JSONArray().put(prompt))
            .put("attachment_messages",JSONArray().put(receipt).put(otherReceipt))).size)
    }

    @Test fun uniqueProviderSupersessionExportsReadIdentityButAmbiguousRepliesDoNot() {
        val target = Target("computer", "session", "run", "conversation", "workspace")
        val hook = Event("journal:hook:Stop", "AgentMessage", "Excerpt", "", 2.0, nativeType = "Stop")
        val provider = Event("provider:public:one", "AgentMessage", "Excerpt completed", "", 3.0, nativeType = "AgentMessage")
        val unique = RollingConversationHistory.merge(target, target, listOf(hook), listOf(provider))
        assertEquals(mapOf(hook.id to provider.id), unique.supersededIds)
        val repeated = RollingConversationHistory.merge(target, target, listOf(hook), listOf(provider, provider.copy(id = "provider:public:two")))
        assertTrue(repeated.supersededIds.isEmpty())
        val twoHooks = RollingConversationHistory.merge(target, target, listOf(hook, hook.copy(id = "journal:hook:second")), listOf(provider))
        assertTrue(twoHooks.supersededIds.isEmpty())
        val truncated = RollingConversationHistory.merge(target, target, listOf(hook), listOf(provider), maxEvents = 0)
        assertTrue(truncated.supersededIds.isEmpty())
    }
    @Test fun ambiguousHookProviderPairsRemainVisibleAndCanonicalAliasesAreExact() {
        val hook = Event("journal:Stop:1", "AgentMessage", "Repeated", "", 2.0, nativeType = "Stop")
        val provider = Event("provider:AgentMessage:1", "AgentMessage", "Repeated full", "", 2.0, nativeType = "AgentMessage")
        val ambiguous = RollingConversationHistory.merge(target,target,listOf(hook,hook.copy(id="journal:Stop:2")),listOf(provider))
        assertEquals(3, ambiguous.events.size); assertTrue(ambiguous.supersededIds.isEmpty())
        val canonical = hook.copy(id="history:canonical",historyId="canonical",originalId=provider.id,originalIds=listOf(hook.id),detailTruncated=true)
        val merged = RollingConversationHistory.merge(target,target,listOf(hook),listOf(canonical))
        assertEquals(listOf("history:canonical"),merged.events.map { it.id })
        assertEquals(mapOf(hook.id to canonical.id),merged.supersededIds)
        assertEquals(canonical,EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(canonical))).single())
    }

}
