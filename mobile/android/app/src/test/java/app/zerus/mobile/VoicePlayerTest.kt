package app.zerus.mobile

import org.json.JSONArray
import org.junit.Assert.*
import org.junit.Test

class VoicePlayerTest {
    @Test fun sharedEncryptedAudioDoesNotSharePlaybackAcrossMessagesOrConversations() {
        val target = Target("host", "session", "run", "conversation", "workspace")
        val attachment = Attachment("e1761172-d6de-42ad-bf01-c8b32e6f4847", "voice.wav", "audio/wav", 6444, "a".repeat(64)).displayed()
        val other = target.copy(conversation = "other")
        assertEquals(AttachmentIdentity.rowKey(target, attachment), AttachmentIdentity.rowKey(other, attachment))
        assertNotEquals(AttachmentIdentity.playbackKey(target, attachment), AttachmentIdentity.playbackKey(other, attachment))
        assertNotEquals(AttachmentIdentity.playbackKey(target, attachment), AttachmentIdentity.playbackKey(target, attachment.copy(messageId = "another-message", index = 0)))
        assertEquals(AttachmentIdentity.playbackKey(target, attachment), AttachmentIdentity.playbackKey(target, attachment.copy()))
    }
    @Test fun waveformRequiresExactly64IntegerAmplitudeBinsWithoutInventingMissingAudio() {
        val bins = List(64) { it * 4 }
        assertEquals(bins, VoiceWaveform.decode(JSONArray(bins)))
        assertEquals(List(64) { 0 }, VoiceWaveform.decode(JSONArray(List(64) { 0 })))
        for (bad in listOf(null, "waveform", JSONArray(), JSONArray(List(63) { 1 }), JSONArray(List(65) { 1 })))
            assertTrue(VoiceWaveform.decode(bad).isEmpty())
        for (bad in listOf(-1, 256, 2L shl 32, "12", 12.0, true)) {
            val input = JSONArray(bins).put(31, bad)
            assertTrue(VoiceWaveform.decode(input).isEmpty())
        }
    }
    @Test fun seekingClampsEndpointsAndRejectsInvalidDurationsAndNonFiniteInput() {
        assertEquals(0, VoiceWaveform.seekPosition(-1f, 16_000))
        assertEquals(4_000, VoiceWaveform.seekPosition(.25f, 16_000))
        assertEquals(16_000, VoiceWaveform.seekPosition(2f, 16_000))
        assertNull(VoiceWaveform.seekPosition(Float.NaN, 16_000))
        assertNull(VoiceWaveform.seekPosition(Float.POSITIVE_INFINITY, 16_000))
        assertNull(VoiceWaveform.seekPosition(.5f, 0))
        assertNull(VoiceWaveform.seekPosition(.5f, 120_001))
        assertEquals(.25f, VoiceWaveform.progress(4_000, 16_000), 0f)
        assertEquals(1f, VoiceWaveform.progress(20_000, 16_000), 0f)
        assertEquals(0f, VoiceWaveform.progress(-20, 16_000), 0f)
        assertEquals("2:00", VoiceWaveform.time(120_000))
    }
}
