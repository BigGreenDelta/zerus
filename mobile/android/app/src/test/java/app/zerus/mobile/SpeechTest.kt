package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class SpeechTest {
    private val target = Target("agent-host", "work-example", "run-a", "conversation-a", "workspace")
    private val host = MachineKey("workspace", "gpu-host")
    private val origin = Draft(target, "Existing draft")
    private fun attempt(text: String = "Распознанный текст.") = SpeechAttempt(UUID.randomUUID().toString(), host, "russian", origin, "completed", text)

    @Test fun insertsIntoExactDraftWithoutSendingOrChangingAttachments() {
        val recording = attempt()
        val state = MessageState(drafts = listOf(origin))
        val inserted = SpeechDrafts.insert(state, recording, origin)
        assertEquals("Existing draft Распознанный текст.", inserted.drafts.single().text)
        assertEquals(origin.attachments, inserted.drafts.single().attachments)
        assertEquals("editing", inserted.drafts.single().status)
        assertEquals(recording.id, inserted.drafts.single().generation)
        assertTrue(inserted.outgoing.isEmpty())
        assertEquals(inserted, SpeechDrafts.insert(inserted, recording, origin))
    }
    @Test fun editingOrSendingWhileRecognitionRunsRequiresReview() {
        val recording = attempt()
        val edited = origin.copy(text = "My edit", revision = UUID.randomUUID().toString())
        assertFalse(SpeechDrafts.unchanged(recording, edited))
        assertThrows(IllegalArgumentException::class.java) { SpeechDrafts.insert(MessageState(listOf(edited)), recording, origin) }
        val reviewed = SpeechDrafts.insert(MessageState(listOf(edited)), recording, edited)
        assertEquals("My edit Распознанный текст.", reviewed.drafts.single().text)
        assertFalse(SpeechDrafts.unchanged(recording, origin.copy(status = "submitting")))
    }
    @Test fun anotherSessionRunOrConversationCannotReceiveTheResult() {
        val recording = attempt()
        for (other in listOf(target.copy(run = "run-b"), target.copy(conversation = "conversation-b"),
            target.copy(computerId = "other"), target.copy(connectionId = "other"), target.copy(agentId = "child"))) {
            val wrong = origin.copy(target = other)
            assertFalse(SpeechDrafts.unchanged(recording, wrong))
            assertThrows(IllegalArgumentException::class.java) { SpeechDrafts.insert(MessageState(listOf(wrong)), recording, wrong) }
        }
    }
    @Test fun duplicateInsertionSurvivesMessageStateSerializationAndSendClear() {
        val recording = attempt()
        val inserted = SpeechDrafts.insert(MessageState(listOf(origin)), recording, origin)
        val restored = MessageCodec.state(MessageCodec.stateJson(inserted))
        val sent = restored.enqueue(restored.drafts.single(), UUID.randomUUID().toString(), 42.0)
        assertEquals(sent, SpeechDrafts.insert(sent, recording, origin))
    }
    @Test fun recoveredJobRetainsGpuHostSeparatelyFromDraftHost() {
        val original = SpeechState(SpeechSelection(host, "russian"), attempt())
        val recovered = SpeechProtocol.decode(SpeechProtocol.encode(original))
        assertEquals(original, recovered)
        assertNotEquals(recovered.attempt!!.host.computerId, recovered.attempt.origin.target.computerId)
    }
    @Test fun rejectsForeignJobAndUnknownProtocol() {
        val recording = attempt()
        val result = JSONObject().put("schema", 1).put("job_id", recording.id).put("state", "completed").put("text", "Текст")
        assertEquals("Текст", SpeechProtocol.job(result, recording).text)
        assertThrows(IllegalArgumentException::class.java) { SpeechProtocol.job(result.put("job_id", UUID.randomUUID().toString()), recording) }
        assertThrows(IllegalArgumentException::class.java) { SpeechProtocol.profiles(JSONObject().put("schema", 2)) }
    }
    @Test fun pcmConversionPreservesSignEndianAndPartialResamplingGroups() {
        val original = Pcm16Converter(16_000)
        original.add(shortArrayOf(-32768, -1, 0, 32767), 4)
        assertArrayEquals(byteArrayOf(0, -128, -1, -1, 0, 0, -1, 127), original.bytes())
        val fallback = Pcm16Converter(48_000)
        fallback.add(shortArrayOf(3, 6), 2)
        assertEquals(0, fallback.size)
        fallback.add(shortArrayOf(9, -3, -6, -9), 4)
        assertArrayEquals(byteArrayOf(6, 0, -6, -1), fallback.bytes())
    }
    @Test fun pcmLimitCannotBeExceededEvenWithOddRemainingByte() {
        val converter = Pcm16Converter(16_000)
        converter.add(ShortArray(100) { 100 }, 100, 9)
        assertEquals(8, converter.size)
    }
    @Test fun speechEnvelopeUsesIndependentComputerAndEmptySession() = runBlocking {
        MockWebServer().use { server ->
            val id = UUID.randomUUID().toString()
            server.enqueue(MockResponse().setBody(JSONObject().put("request_id", id).put("state", "completed")
                .put("result", JSONObject().put("schema", 1).put("job_id", id).put("state", "queued")).toString()))
            // Test the HTTP envelope on an isolated localhost endpoint, bypassing production HTTPS routing.
            val client = OkHttpClient.Builder().addInterceptor { chain ->
                chain.proceed(chain.request().newBuilder().url(server.url(chain.request().url.encodedPath)).build())
            }.build()
            val api = RelayApi(client)
            val connection = Connection("workspace", "Test", "https://relay.example.test", "fixture-token")
            api.speech(connection, host, "speech_submit", JSONObject().put("request_id", id), id)
            val envelope = JSONObject(server.takeRequest().body.readUtf8())
            assertEquals("gpu-host", envelope.getString("computer_id"))
            assertEquals("", envelope.getString("session"))
            assertEquals(id, envelope.getJSONObject("payload").getString("request_id"))
            assertEquals(1, server.requestCount)
        }
    }
}
