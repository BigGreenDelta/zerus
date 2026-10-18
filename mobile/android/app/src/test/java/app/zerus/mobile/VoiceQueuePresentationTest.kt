package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** Native admission of a voice job does not consume OpenCode's transient inbox. */
class VoiceQueuePresentationTest {
    @Test fun submittedStatusDetailsExplainQueueAndNativeAdmissionOnly() {
        val status = RecordingDelivery.of("submitted")
        assertEquals("Accepted by native app", status.description)
        assertTrue(status.explanation.contains("native app accepted the recognized text"))
        assertTrue(status.explanation.contains("Queue status"))
        assertTrue(status.explanation.contains("does not confirm model delivery, reading or completion"))
    }

    private val id = "aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"
    private val target = Target("computer", "opencode/example/local", "run", "conversation", "workspace")
    private val message = VoiceMessage(id, target.run, target.conversation, 50.0, 2, "submitted",
        DisplayedAttachment("voice.wav", "audio/wav", 6444, messageId = id, index = 0), 200, "Repeated text")
    private fun voice(binding: JSONObject = JSONObject().put("provider_source", "opencode_native").put("provider_message_id", "msg_voice")) =
        JSONObject().put("source", "hgs_voice").put("type", "UserMessage").put("message_id", id).put("at", 50.0)
            .put("detail", message.transcript).put("attachments", JSONArray().put(VoiceProtocol.json(message).getJSONObject("audio")))
            .put("voice", VoiceProtocol.json(message).put("native_binding", binding))
    private fun native(state: String? = "queued", id: String = "msg_voice", source: String = "opencode_native") =
        JSONObject().put("source", source).put("type", "UserPromptSubmit").put("message_id", id).put("detail", "Repeated text")
            .put("at", 51.0).apply { if (state != null) put("delivery_state", state) }
    private fun raw(voice: JSONObject, vararg natives: JSONObject) = JSONObject().put("provider_messages", JSONArray(natives.toList()))
        .put("voice_messages", JSONArray().put(voice))

    @Test fun queueStopRemovalAndConsumptionKeepMediaAndExactNativeStatus() {
        val voice = voice()
        val original = voice.toString()
        val boundMessage = VoiceProtocol.message(voice.getJSONObject("voice"))
        val queued = NativeParser.events(raw(voice, native()))
        assertEquals(2, queued.size)
        val media = queued.single { it.voice != null }
        val pending = queued.single { it.voice == null }
        assertEquals("queued", pending.delivery)
        assertFalse(pending.id in media.originalIds)
        assertEquals(boundMessage, media.voice)
        val canonical = media.copy(id = "history:voice", historyId = "voice", historyCursor = "cursor", historyEpoch = "epoch",
            originalIds = listOf(media.id))
        val head = HistoryWindow(target, "epoch", listOf(canonical), null, "cursor", false, false, true)
        val waiting = head.inspection(queued)
        assertEquals(listOf(canonical), waiting.events) // inbox must not acquire canonical read/cursor proof
        assertEquals(listOf("queued"), waiting.displayed.filter { it.voice == null }.map { it.delivery })
        val stopped = NativeParser.events(raw(voice, native("not_delivered")))
        val afterStop = waiting.inspection(stopped, queued)
        assertEquals(listOf("not_delivered"), afterStop.displayed.filter { it.voice == null }.map { it.delivery })
        assertEquals(2, RollingConversationHistory.merge(target, target, queued, stopped).events.size)
        assertEquals(2, VoiceHistory.merge(target, stopped, emptyList()).size)
        val removed = NativeParser.events(raw(voice))
        assertEquals(listOf(canonical), afterStop.inspection(removed, stopped).displayed)
        val consumed = NativeParser.events(raw(voice, native(null)))
        assertEquals(1, consumed.size)
        assertEquals(boundMessage, consumed.single().voice)
        assertEquals(1, RollingConversationHistory.merge(target, target, stopped, consumed).events.size)
        assertEquals(original, voice.toString())
        assertEquals("Accepted by native app", RecordingDelivery.of("submitted").description)
        assertEquals("Accepted by native app", VoiceProtocol.label("submitted"))
    }

    @Test fun aBoundOpenCodeRowNamingTheVoiceRequestKeepsVoiceRowsAndIsNeverATypedMessage() {
        // hgs names the voice job's request on the OpenCode prompt it bound, queued or delivered.
        fun shape(rows: List<Event>) = rows.map { listOf(it.id, it.delivery, it.voice, it.originalIds, it.outgoingId, it.text) }
        val typedId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
        val unrelated = OutgoingMessage(typedId, target, "Something else", emptyList(), 50.5, "submitted", 51.0)
        val typed = unrelated.copy(text = message.transcript)
        for (state in listOf("queued", "not_delivered", null)) {
            val plain = NativeParser.events(raw(voice(), native(state)))
            val named = NativeParser.events(raw(voice(), native(state).put("request_id", id)))
            assertEquals(shape(plain), shape(named))
            assertEquals(shape(VoiceHistory.merge(target, plain, emptyList())), shape(VoiceHistory.merge(target, named, emptyList())))
            assertEquals(shape(RollingConversationHistory.merge(target, target, plain, plain).events),
                shape(RollingConversationHistory.merge(target, target, named, named).events))
            // An unrelated phone message merges the same either way.
            assertEquals(shape(ConversationMerge.merge(target, plain, listOf(unrelated)).events),
                shape(ConversationMerge.merge(target, named, listOf(unrelated)).events))
            // One typed with the transcript's text inside the window: the text fallback took the unnamed
            // inbox row; the row naming the voice request is never another message, and the voice rows stay.
            assertEquals(if (state == null) emptySet() else setOf(typedId), ConversationMerge.merge(target, plain, listOf(typed)).recordedRequestIds)
            val separate = ConversationMerge.merge(target, named, listOf(typed))
            assertTrue(separate.recordedRequestIds.isEmpty())
            assertEquals(shape(named) + listOf(listOf("outgoing:$typedId", "submitted", null, emptyList<String>(), typedId, message.transcript)),
                shape(separate.events))
            // Bringing the own send into view still lands on the voice card, not the inbox row after it.
            assertEquals("voice:$id", ConversationViewport.own(VoiceHistory.merge(target, named, emptyList()), id)?.id)
        }
    }

    @Test fun wrongTargetMessageSourceAndAmbiguousBindingsNeverSuppressQueueRows() {
        val foreign = native().put("agent_id", "other-child")
        assertEquals(1, NativeParser.events(raw(voice(), foreign)).size)
        for (other in listOf(native(id = "msg_other"), native(source = "other_native"))) {
            val rows = NativeParser.events(raw(voice(), other))
            assertEquals(2, rows.size)
            assertTrue(rows.single { it.voice != null }.originalIds.isEmpty())
        }
        // Equal text and an accepted unrelated row cannot substitute for the bound native ID.
        assertEquals(2, NativeParser.events(raw(voice(), native(null, "msg_other"))).size)
        val binding = JSONObject().put("provider_source", "opencode_native").put("provider_message_id", "msg_voice").put("journal_seq", 7)
        val ambiguous = raw(voice(binding), native()).put("events", JSONArray().put(native("not_delivered").put("seq", 7)))
        val rows = NativeParser.events(ambiguous)
        assertEquals(3, rows.size)
        assertTrue(rows.single { it.voice != null }.originalIds.isEmpty())
        assertEquals(3, RollingConversationHistory.merge(target, target, emptyList(), rows).events.size)
    }
}
