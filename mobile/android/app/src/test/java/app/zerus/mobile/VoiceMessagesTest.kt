package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class VoiceMessagesTest {
    private val target = Target("owner", "example", "run", "conversation", "workspace")
    private val id = UUID.randomUUID().toString()
    private val audio = Attachment(UUID.randomUUID().toString(), "voice.wav", "audio/wav", 6444, "a".repeat(64))
    private fun message(state: String = "transcribing", revision: Long = 1) = VoiceMessage(id, target.run, target.conversation,
        50.0, revision, state, DisplayedAttachment("voice.wav", "audio/wav", 6444, messageId = id, index = 0), 200,
        if (state == "submitted") "Привет." else "")
    private fun entry(message: VoiceMessage? = null) = VoiceOutgoing(id, target, "b4075bf0-9c22-4a67-b768-55af2d2936a4", "russian", audio, 200, 50.0, message)
    private fun event(message: VoiceMessage = message()) = Event("native-voice", "You", message.transcript, "", 50.0, id,
        listOf(message.audio), "hgs_voice", nativeType = "UserMessage", voice = message)

    @Test fun audioIsLosslesslyWrappedAndTamperingRejected() {
        val pcm = ByteArray(6400) { (it % 127).toByte() }
        val wav = VoiceProtocol.wav(pcm)
        assertEquals(6444, wav.size)
        assertArrayEquals(pcm, VoiceProtocol.pcm(wav))
        for (offset in listOf(0, 4, 20, 22, 24, 32, 40)) {
            val changed = wav.copyOf(); changed[offset] = (changed[offset] + 1).toByte()
            assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.pcm(changed) }
        }
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.wav(ByteArray(3199)) }
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.wav(ByteArray(VoiceProtocol.MAX_PCM_BYTES + 2)) }
    }
    @Test fun waveformBackfillSurvivesHistoryAndReceiptCachesWithoutChangingAudioIdentity() {
        val previous = message("submitted", 5)
        val backfilled = previous.copy(revision = 6, waveform = List(64) { it * 4 })
        assertEquals(backfilled, VoiceProtocol.message(VoiceProtocol.json(backfilled)))
        val stored = VoiceState(outgoing = listOf(entry(backfilled)))
        assertEquals(stored, VoiceCodec.decode(VoiceCodec.encode(stored)))
        val cached = EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(event(backfilled)))).single()
        assertEquals(backfilled, cached.voice)
        val merged = RollingConversationHistory.merge(target, target, listOf(event(previous)), listOf(cached)).events.single()
        assertEquals(backfilled.waveform, merged.voice!!.waveform)
        assertEquals(previous.audio, merged.voice.audio)
        val malformed = VoiceProtocol.json(previous).apply { getJSONObject("audio").put("waveform", JSONArray().put(255)) }
        assertEquals(previous, VoiceProtocol.message(malformed))
    }
    @Test fun previewAndUploadIdentitySurviveRecoveryWithoutCreatingDelivery() {
        val preview = VoiceDraft(target, "b4075bf0-9c22-4a67-b768-55af2d2936a4", "russian", audio, 200, 50.0)
        val saved = VoiceState(SpeechSelection(MachineKey("workspace", "b4075bf0-9c22-4a67-b768-55af2d2936a4"), "russian"), preview)
        assertEquals(saved, VoiceCodec.decode(VoiceCodec.encode(saved)))
        val uploaded = saved.copy(draft = null, outgoing = listOf(entry()))
        val recovered = VoiceCodec.decode(VoiceCodec.encode(uploaded))
        assertNull(recovered.draft)
        assertEquals(id, recovered.outgoing.single().id)
        assertEquals(audio, recovered.outgoing.single().audio)
        assertEquals("uncertain", recovered.outgoing.single().state)
        assertTrue(recovered.outgoing.single().needsStatus)
        assertEquals("b4075bf0-9c22-4a67-b768-55af2d2936a4", recovered.outgoing.single().machineId)
        assertEquals("owner", recovered.outgoing.single().target.computerId)
    }
    @Test fun recordingWithoutDiscoveredProviderRemainsAnExactDurablePreview() {
        val preview = VoiceDraft(target, "", "", audio, 200, 50.0)
        val saved = VoiceState(draft = preview)
        assertEquals(saved, VoiceCodec.decode(VoiceCodec.encode(saved)))
        assertEquals(SpeechSelection(), preview.selection)
        assertTrue(VoiceCodec.decode(VoiceCodec.encode(saved)).outgoing.isEmpty())
    }
    @Test fun sendingRequiresFreshExactProviderAndConversationWithoutFallback() {
        val machine = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
        val draft = VoiceDraft(target, machine, "chosen", audio, 2_000, 50.0)
        val chosen = SpeechProfile("chosen", "Chosen", "synthetic", true)
        val fallback = SpeechProfile("other", "Other", "synthetic", true)
        val computers = listOf(VoiceComputer(machine, "GPU", listOf(chosen, fallback)))
        fun reason(value: VoiceDraft = draft, confirmed: Target? = target, loading: Boolean = false,
            profiles: List<VoiceComputer> = computers) = VoiceRoutes.providerReason(value, confirmed, loading, profiles)
        assertEquals("", reason())
        assertTrue(reason(loading = true).isNotBlank())
        for (other in listOf<Target?>(null, target.copy(run = "different"), target.copy(conversation = "different"),
            target.copy(computerId = "different"), target.copy(connectionId = "different"))) assertTrue(reason(confirmed = other).isNotBlank())
        assertTrue(reason(draft.copy(machineId = "")).isNotBlank())
        assertTrue(reason(draft.copy(profileId = "")).isNotBlank())
        assertTrue(reason(draft.copy(profileId = "missing")).isNotBlank())
        assertTrue(reason(draft.copy(machineId = UUID.randomUUID().toString())).isNotBlank())
        assertEquals("Chosen worker offline", reason(profiles = listOf(computers.single().copy(profiles =
            listOf(chosen.copy(available = false, reason = "Chosen worker offline"), fallback)))))
        assertTrue(reason(profiles = listOf(computers.single().copy(profiles = listOf(chosen.copy(maxDurationMs = 1_000))))).isNotBlank())
        assertEquals(SpeechSelection(MachineKey(target.connectionId, machine), "chosen"), draft.selection)
    }
    @Test fun providerRevisionPinsPreviewOutgoingAndSubmitWithoutAdoptingEdits() {
        val machine = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
        val preview = VoiceDraft(target, machine, "chosen", audio, 200, 50.0, profileRevision = 7)
        val saved = VoiceState(preview.selection, preview, listOf(entry().copy(profileRevision = 7, uploadState = "uncertain")))
        assertEquals(saved, VoiceCodec.decode(VoiceCodec.encode(saved)))
        assertEquals(7L, VoiceProtocol.provider(preview).getLong("profile_revision"))
        val profile = SpeechProfile("chosen", "Chosen", "synthetic", true, profileRevision = 7)
        fun reason(p: SpeechProfile) = VoiceRoutes.providerReason(preview, target, false, listOf(VoiceComputer(machine, "GPU", listOf(p))))
        assertEquals("", reason(profile))
        assertTrue(reason(profile.copy(profileRevision = 8)).contains("changed"))
        assertTrue(reason(profile.copy(profileRevision = null)).contains("changed"))
        val legacy = preview.copy(profileRevision = null)
        assertFalse(VoiceProtocol.provider(legacy).has("profile_revision"))
        assertTrue(VoiceRoutes.providerReason(legacy, target, false, listOf(VoiceComputer(machine, "GPU", listOf(profile)))).isNotBlank())
        val raw = JSONObject().put("schema", 1).put("profiles", JSONArray().put(JSONObject().put("id", "chosen").put("available", true).put("profile_revision", 7)))
        assertEquals(7L, SpeechProtocol.profiles(raw).single().profileRevision)
        raw.getJSONArray("profiles").getJSONObject(0).put("profile_revision", 7.5)
        assertThrows(IllegalArgumentException::class.java) { SpeechProtocol.profiles(raw) }
    }
    @Test fun rejectsForeignIdentityAndUnboundedAudio() {
        val raw = JSONObject().put("schema", 1).put("message", VoiceProtocol.json(message()))
        assertEquals(message(), VoiceProtocol.response(raw, target, id))
        for (other in listOf(target.copy(run = "other"), target.copy(conversation = "other")))
            assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.response(raw, other, id) }
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.response(raw, target, UUID.randomUUID().toString()) }
        val invalid = VoiceProtocol.json(message()); invalid.getJSONObject("audio").put("request_id", UUID.randomUUID().toString())
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.message(invalid) }
        val oversized = VoiceProtocol.json(message()); oversized.getJSONObject("audio").put("bytes", VoiceProtocol.MAX_PCM_BYTES + 45)
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.message(oversized) }
    }
    @Test fun onlyProvenLocalNonDeliveryCanBecomeANewRecording() {
        assertTrue(entry().copy(uploadState = "failed").canRestoreUnsent)
        assertFalse(entry().canRestoreUnsent)
        assertFalse(entry().recover().canRestoreUnsent)
        assertFalse(entry(message("failed", 3)).canRestoreUnsent)
        assertFalse(entry(message("uncertain", 3)).canRestoreUnsent)
        assertFalse(entry().copy(uploadState = "failed", audio = null).canRestoreUnsent)
    }
    @Test fun olderStatusCannotUndoSubmittedTranscript() {
        val submitted = message("submitted", 5)
        assertEquals(submitted, VoiceProtocol.newer(submitted, message("transcribing", 4)))
        val native = event(submitted)
        val merged = VoiceHistory.merge(target, listOf(native), listOf(entry(message())))
        assertEquals("submitted", merged.single().voice!!.state)
        assertEquals("Привет.", merged.single().text)
        assertEquals(audio.id, merged.single().voice!!.audio.localId)
        assertEquals(1, VoiceHistory.merge(target, listOf(event(), native), listOf(entry())).size)
    }
    @Test fun statusInHistoryAndDisplayCacheKeepsRevisionAndAudio() {
        val current = event(message("submitted", 5))
        assertEquals(current, EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(current))).single())
        val previous = RollingConversationHistory.merge(target, target, listOf(current), listOf(event())).events.single()
        assertEquals(5L, previous.voice!!.revision)
        assertEquals("Привет.", previous.text)
        val canonical = current.copy(historyId = "history-voice", historyCursor = "cursor", historyEpoch = "epoch")
        val window = HistoryWindow(target, "epoch", listOf(canonical), null, "cursor", false, false, true)
        assertEquals(5L, window.inspection(listOf(event())).events.single().voice!!.revision)
    }
    @Test fun voiceOverlayDoesNotMatchDifferentConversationOrEqualText() {
        val unrelated = Event("text", "You", "Привет.", "", 50.0)
        val merged = VoiceHistory.merge(target, listOf(unrelated), listOf(entry(message("submitted", 2))))
        assertEquals(2, merged.size)
        assertEquals(listOf(unrelated), VoiceHistory.merge(target.copy(conversation = "other"), listOf(unrelated), listOf(entry(message()))))
        assertEquals(listOf(unrelated), VoiceHistory.merge(target.copy(run = "other"), listOf(unrelated), listOf(entry(message()))))
    }
    @Test fun inspectionRequiresExplicitBindingToSuppressNativeUserText() {
        val native = JSONObject().put("seq", 7).put("type", "UserPromptSubmit").put("detail", "Привет.").put("at", 51.0)
        val voice = JSONObject().put("source", "hgs_voice").put("type", "UserMessage").put("message_id", id)
            .put("detail", "Привет.").put("at", 50.0).put("voice", VoiceProtocol.json(message("submitted", 2)))
        val raw = JSONObject().put("events", JSONArray().put(native)).put("voice_messages", JSONArray().put(voice))
        assertEquals(2, NativeParser.events(raw).size)
        voice.getJSONObject("voice").put("native_binding", JSONObject().put("journal_seq", 8))
        assertEquals(2, NativeParser.events(raw).size)
        voice.getJSONObject("voice").put("native_binding", JSONObject().put("journal_seq", 7))
        val parsed = NativeParser.events(raw)
        assertEquals(1, parsed.size)
        assertEquals(id, parsed.single().voice!!.id)
        val previous = NativeParser.events(JSONObject().put("events", JSONArray().put(native)))
        assertEquals(1, RollingConversationHistory.merge(target, target, previous, parsed).events.size)
    }
    @Test fun profilesKeepIndependentHostAndUnavailableSelections() {
        val raw = JSONObject().put("schema", 1).put("computers", JSONArray().put(JSONObject().put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4")
            .put("name", "GPU").put("profiles", JSONArray().put(JSONObject().put("id", "ru").put("name", "Russian").put("available", false)))))
        val computer = VoiceProtocol.computers(raw).single()
        assertEquals("b4075bf0-9c22-4a67-b768-55af2d2936a4", computer.id)
        assertFalse(computer.profiles.single().available)
        raw.getJSONArray("computers").getJSONObject(0).put("machine_id", "../../other-host")
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.computers(raw) }
        raw.getJSONArray("computers").getJSONObject(0).put("machine_id", "B4075BF0-9C22-4A67-B768-55AF2D2936A4")
        assertThrows(IllegalArgumentException::class.java) { VoiceProtocol.computers(raw) }
    }
    @Test fun statsAndLegacySelectionResolvePhysicalMachineThroughCurrentRelayIdentity() {
        val uuid = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
        val relay = "83a06c3e-16a0-4312-96ab-1a63ec75a6f6"
        val machine = MachineCatalog.parse("workspace", JSONObject().put("id", relay).put("machine_id", uuid).put("name", "GPU"))
        assertEquals(uuid, machine.machineId)
        assertEquals(MachineKey("workspace", relay), VoiceRoutes.statistics(MachineKey("workspace", uuid), listOf(machine)))
        assertNull(VoiceRoutes.statistics(MachineKey("other-workspace", uuid), listOf(machine)))
        val selected = SpeechSelection(MachineKey("workspace", relay), "ru")
        val computers = listOf(VoiceComputer(uuid, "GPU", emptyList()))
        assertEquals(SpeechSelection(MachineKey("workspace", uuid), "ru"), VoiceRoutes.migratedSelection(selected, computers, listOf(machine)))
        assertEquals(selected, VoiceRoutes.migratedSelection(selected, emptyList(), listOf(machine)))
        assertEquals("", MachineCatalog.parse("workspace", JSONObject().put("id", relay).put("machine_id", "broken")).machineId)
    }
    @Test fun voiceEnvelopeTargetsConversationComputerWithExactSessionIdentity() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            val receipt = JSONObject().put("request_id", id).put("state", "completed")
                .put("result", JSONObject().put("schema", 1).put("message", VoiceProtocol.json(message())))
            server.enqueue(MockResponse().setBody(receipt.toString()))
            val connection = Connection("workspace", "Fixture", server.url("/").toString().removeSuffix("/"), "fixture-token")
            RelayApi(OkHttpClient()).voice(connection, target, "voice_submit", VoiceProtocol.payload(target, id)
                .put("speech", JSONObject().put("machine_id", "b4075bf0-9c22-4a67-b768-55af2d2936a4").put("profile_id", "ru")), id)
            val envelope = JSONObject(server.takeRequest().body.readUtf8())
            assertEquals("owner", envelope.getString("computer_id"))
            assertEquals("example", envelope.getString("session"))
            assertEquals(id, envelope.getJSONObject("payload").getString("request_id"))
            assertEquals("b4075bf0-9c22-4a67-b768-55af2d2936a4", envelope.getJSONObject("payload").getJSONObject("speech").getString("machine_id"))
        }
    }
}
