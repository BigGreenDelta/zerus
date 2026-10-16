package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class VideoMessagesTest {
    private val id = UUID.randomUUID().toString()
    private val target = Target("owner", "fixture", "run", "conversation", "gateway")
    private val video = VideoDraft(Attachment(UUID.randomUUID().toString(), "video.mp4", "video/mp4", 100, "b".repeat(64)), 1000, 480, 480)
    private val audio = Attachment(UUID.randomUUID().toString(), "voice.wav", "audio/wav", 32044, "a".repeat(64))
    @Test fun mediaHaveOneMessageIdentityAcrossDurableRecoveryAndNativeReceipts() {
        val draft = VoiceDraft(target, "machine", "profile", audio, 1000, 42.0, video = video)
        val pending = VoiceOutgoing(id, target, "machine", "profile", audio, 1000, 42.0, uploadState = "failed", video = video)
        val state = VoiceState(draft = draft, outgoing = listOf(pending), videoMode = true)
        assertEquals(state, VoiceCodec.decode(VoiceCodec.encode(state)))
        val displayed = pending.displayed()
        assertEquals(id, displayed.audio.messageId); assertEquals(0, displayed.audio.index)
        assertEquals(id, displayed.video!!.file.messageId); assertEquals(1, displayed.video.file.index)
        val native = VoiceProtocol.message(VoiceProtocol.json(displayed.copy(state = "queued")))
        assertEquals(id, native.video!!.file.messageId)
        assertEquals(video.durationMs, native.video.durationMs)
        assertEquals(100L, native.video.file.bytes)
    }
    @Test fun nativeHistoryAdoptsLocalVideoWithoutDuplicatingTheMessage() {
        val own = VoiceOutgoing(id, target, "machine", "profile", audio, 1000, 42.0, video = video)
        val native = own.displayed().copy(state = "submitted", revision = 2, video = video.displayed(id).let {
            it.copy(file = it.file.copy(localId = "", sha256 = ""))
        })
        val row = Event("native", "You", "", "", 42.0, id, listOf(native.audio, native.video!!.file), "hgs_voice", nativeType = "UserMessage", voice = native)
        val merged = VoiceHistory.merge(target, listOf(row), listOf(own)).single()
        assertEquals(id, merged.voice!!.id); assertEquals(video.file.id, merged.voice.video!!.file.localId)
        assertEquals(video.file.sha256, merged.voice.video.file.sha256); assertEquals(listOf(0, 1), merged.attachments.map { it.index })
    }
    @Test fun optionalVideoDoesNotChangeLegacyAudioOrDefaultMode() {
        val old = VoiceCodec.decode(JSONObject())
        assertFalse(old.videoMode); assertNull(old.draft)
        val audioOnly = VoiceOutgoing(id, target, "machine", "profile", audio, 1000, 42.0)
        assertNull(audioOnly.displayed().video)
        assertFalse(VoiceProtocol.json(audioOnly.displayed()).has("video"))
    }
    @Test fun videoMetadataAndWireAreBoundedAndExact() {
        val meta = VideoProtocol.json(video.displayed(id))
        assertEquals(video.displayed(id).copy(file = DisplayedAttachment("video.mp4", "video/mp4", 100, messageId = id, index = 1)), VideoProtocol.message(meta, id))
        for ((field, replacement) in listOf("duration_ms" to 60001, "width" to 4000, "index" to 0, "request_id" to UUID.randomUUID().toString(), "bytes" to 10_485_761)) {
            val bad = JSONObject(meta.toString()).put(field, replacement)
            assertThrows(IllegalArgumentException::class.java) { VideoProtocol.message(bad, id) }
        }
        assertThrows(IllegalArgumentException::class.java) { VideoProtocol.wire(video, ByteArray(99)) }
        val wire = VideoProtocol.wire(video, ByteArray(100))
        assertEquals("mp4", wire.getString("format")); assertFalse(wire.has("poster_base64"))
    }
    @Test fun extractedAudioConversionHandlesStereoAndAacRatesWithoutChunkDependence() {
        for (rate in listOf(8000, 16000, 44100, 48000)) {
            val converter = MediaPcm16Converter(rate, 2)
            repeat(rate) { converter.sample(10000); converter.sample(-2000) }
            val bytes = converter.bytes()
            assertEquals(32000, bytes.size)
            for (index in bytes.indices step 2) assertEquals(4000, ((bytes[index].toInt() and 255) or (bytes[index + 1].toInt() shl 8)).toShort().toInt())
        }
    }
    @Test fun extractedAudioHasAHardSixtySecondBound() {
        val converter = MediaPcm16Converter(8000, 1)
        repeat(8000 * 61) { converter.sample(200) }
        assertTrue(converter.full); assertEquals(1_920_000, converter.bytes().size)
    }
}
