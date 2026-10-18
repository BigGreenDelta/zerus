package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** A session-owned voice message. Audio identity is independent of the ASR worker. */
data class VoiceMessage(val id: String, val runId: String, val conversationId: String, val createdAt: Double,
    val revision: Long, val state: String, val audio: DisplayedAttachment, val durationMs: Int,
    val transcript: String = "", val error: String = "", val errorCode: String = "", val nativeBinding: String = "",
    val waveform: List<Int> = emptyList(), val video: VideoMessage? = null) {
    val terminal get() = state in setOf("submitted", "failed", "cancelled", "uncertain")
}
internal data class VoiceComputer(val id: String, val name: String, val profiles: List<SpeechProfile>)
internal data class VoiceDraft(val target: Target, val machineId: String, val profileId: String,
    val audio: Attachment, val durationMs: Int, val createdAt: Double, val profileRevision: Long? = null, val video: VideoDraft? = null,
    val resubmissionId: String = "", val rejection: String = "") {
    val selection get() = SpeechSelection(machineId.takeIf { it.isNotBlank() }?.let { MachineKey(target.connectionId, it) }, profileId, profileRevision)
}
internal data class VoiceOutgoing(val id: String, val target: Target, val machineId: String, val profileId: String,
    val audio: Attachment?, val durationMs: Int, val createdAt: Double, val message: VoiceMessage? = null,
    val uploadState: String = "uploading", val error: String = "", val profileRevision: Long? = null, val video: VideoDraft? = null,
    val rejection: String = "", val attemptedRejectionGeneration: Long = 0, val operationId: String = id) {
    val state get() = message?.state ?: uploadState
    val terminal get() = state in setOf("submitted", "failed", "cancelled", "uncertain")
    // Submitted receipts remain a local history/playback cache, not unsent drafts.
    val needsDraftReview get() = message?.state != "submitted"
    val needsStatus get() = message == null && uploadState == "uncertain" || !terminal
    val canRestoreUnsent get() = message == null && uploadState == "failed" && audio != null
    val effectiveTarget get() = if (target.conversation.isBlank() && !message?.conversationId.isNullOrBlank()) target.copy(conversation = message!!.conversationId) else target
    fun belongsTo(other: Target) = target == other || target.resolvesTo(other)
    fun recover() = if (message == null && uploadState == "uploading") copy(uploadState = "uncertain", error = "Sending was interrupted. Check this message's status; it has not been resent.") else this
    fun displayed(): VoiceMessage {
        val result = message ?: VoiceMessage(id, target.run, target.conversation, createdAt, 0,
            uploadState, checkNotNull(audio).displayed().copy(messageId = id, index = 0), durationMs, error = error)
        val displayed = audio?.let { result.copy(audio = result.audio.copy(localId = it.id, sha256 = it.sha256)) } ?: result
        return video?.let { local -> displayed.copy(video = (displayed.video ?: local.displayed(id)).let {
            it.copy(file = it.file.copy(localId = local.file.id, sha256 = local.file.sha256))
        }) } ?: displayed
    }
}
internal data class VoiceState(val selection: SpeechSelection = SpeechSelection(), val draft: VoiceDraft? = null,
    val outgoing: List<VoiceOutgoing> = emptyList(), val videoMode: Boolean = false, val previewBeforeSend: Boolean = false)

object VoiceProtocol {
    val operations = setOf("voice_profiles", "voice_submit", "voice_status", "voice_cancel")
    val states = setOf("queued", "transcribing", "ready", "waiting_agent", "delivering", "submitted", "failed", "cancelled", "uncertain")
    internal fun provider(draft: VoiceDraft) = JSONObject().put("machine_id", draft.machineId).put("profile_id", draft.profileId)
        .put("profile_revision", draft.profileRevision)
    const val MAX_PCM_BYTES = 3_840_000
    fun payload(target: Target, requestId: String) = JSONObject().put("request_id", requestId)
        .put("expected_run_id", target.run).put("expected_conversation_id", target.conversation)
    internal fun computers(value: JSONObject): List<VoiceComputer> {
        require(value.optInt("schema") == 1) { "This computer uses an unsupported voice message protocol." }
        val rows = value.optJSONArray("computers")?.objects().orEmpty()
        require(rows.size <= 100 && rows.map { it.string("machine_id") }.distinct().size == rows.size)
        return rows.map { row ->
            val id = row.getString("machine_id"); require(AttachmentPolicy.validId(id))
            VoiceComputer(id, row.string("name").ifBlank { id }, SpeechProtocol.profiles(JSONObject()
                .put("schema", 1).put("profiles", row.optJSONArray("profiles") ?: JSONArray())))
        }
    }
    fun message(raw: JSONObject): VoiceMessage {
        val id = raw.getString("id"); require(AttachmentPolicy.validId(id))
        val state = raw.getString("state"); require(state in states)
        val run = raw.getString("run_id"); require(run.isNotBlank() && run.length <= 256)
        val conversation = raw.getString("conversation_id"); require(conversation.length <= 256)
        val created = raw.getDouble("created_at"); require(created.isFinite() && created > 0)
        val revision = raw.getLong("revision"); require(revision >= 0)
        val audio = raw.getJSONObject("audio")
        require(audio.getString("request_id") == id && audio.getInt("index") == 0 && audio.getString("mime") == "audio/wav")
        val name = audio.getString("name"); require(name == AttachmentPolicy.filename(name))
        val bytes = audio.getLong("bytes"); require(bytes in 45..(MAX_PCM_BYTES + 44L))
        val duration = audio.getInt("duration_ms"); require(duration in 1..120_000)
        val transcript = raw.string("transcript"); require(transcript.toByteArray(Charsets.UTF_8).size <= 65_536)
        return VoiceMessage(id, run, conversation, created, revision, state,
            DisplayedAttachment(name, "audio/wav", bytes, messageId = id, index = 0),
            duration, transcript, raw.string("error").take(2000), raw.string("error_code").take(128),
            raw.optJSONObject("native_binding")?.toString()?.also { require(it.length <= 4096) }.orEmpty(),
            VoiceWaveform.decode(audio.opt("waveform")), raw.optJSONObject("video")?.let { VideoProtocol.message(it, id) })
    }
    fun response(raw: JSONObject, target: Target, id: String): VoiceMessage {
        require(raw.optInt("schema") == 1) { "Unsupported voice message response." }
        return message(raw.getJSONObject("message")).also {
            require(it.id == id && it.runId == target.run && (target.conversation.isBlank() || it.conversationId == target.conversation)) {
                "The computer returned a different voice message."
            }
        }
    }
    fun json(message: VoiceMessage): JSONObject = JSONObject().put("id", message.id).put("run_id", message.runId)
        .put("conversation_id", message.conversationId).put("created_at", message.createdAt).put("revision", message.revision)
        .put("state", message.state).put("transcript", message.transcript).put("error", message.error).put("error_code", message.errorCode)
        .put("native_binding", message.nativeBinding.takeIf { it.isNotBlank() }?.let(::JSONObject))
        .put("audio", JSONObject().put("request_id", message.id).put("index", 0).put("name", message.audio.name)
            .put("mime", "audio/wav").put("bytes", message.audio.bytes).put("duration_ms", message.durationMs)
            .put("waveform", message.waveform.takeIf(VoiceWaveform::valid)?.let(::JSONArray)))
        .put("video", message.video?.let(VideoProtocol::json))
    fun newer(previous: VoiceMessage?, next: VoiceMessage): VoiceMessage {
        if (previous == null) return next
        require(previous.id == next.id && previous.runId == next.runId &&
            (previous.conversationId.isBlank() || next.conversationId == previous.conversationId))
        return if (next.revision > previous.revision) next else previous
    }
    fun wav(pcm: ByteArray): ByteArray {
        require(pcm.size in 3_200..MAX_PCM_BYTES && pcm.size % 2 == 0)
        return ByteBuffer.allocate(pcm.size + 44).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("RIFF".toByteArray()); putInt(pcm.size + 36); put("WAVEfmt ".toByteArray()); putInt(16)
            putShort(1); putShort(1); putInt(16_000); putInt(32_000); putShort(2); putShort(16)
            put("data".toByteArray()); putInt(pcm.size); put(pcm)
        }.array()
    }
    fun pcm(wav: ByteArray): ByteArray {
        require(wav.size in 3244..MAX_PCM_BYTES + 44)
        val pcm = wav.copyOfRange(44, wav.size)
        require(wav.contentEquals(wav(pcm))) { "The saved recording is invalid." }
        return pcm
    }
    fun label(state: String) = when (state) {
        "uploading" -> "Sending recording…"
        "queued" -> "Waiting to transcribe…"
        "transcribing" -> "Transcribing…"
        "ready", "waiting_agent" -> "Waiting for the agent…"
        "delivering" -> "Delivering to the agent…"
        "submitted" -> "Accepted by native app"
        "failed" -> "Voice message failed"
        "cancelled" -> "Cancelled"
        else -> "Delivery unconfirmed"
    }
}

internal object VoiceRoutes {
    fun providerReason(draft: VoiceDraft, confirmedTarget: Target?, loading: Boolean, computers: List<VoiceComputer>): String {
        if (loading) return "Loading voice profiles. Your recording is saved."
        if (confirmedTarget != draft.target) return "Refresh voice profiles for this recording's conversation before sending."
        if (draft.machineId.isBlank() || draft.profileId.isBlank()) return "Choose a voice computer and profile before sending."
        val computer = computers.find { it.id == draft.machineId } ?: return "The selected voice computer is unavailable. Choose a provider or refresh."
        val profile = computer.profiles.find { it.id == draft.profileId } ?: return "The selected voice profile is unavailable. Choose a provider or refresh."
        if (profile.profileRevision != draft.profileRevision) return "This provider has changed. Select it again to confirm where this recording will be sent."
        if (!profile.available) return profile.reason.ifBlank { "The selected voice profile is unavailable." }
        if (draft.durationMs > profile.maxDurationMs) return "This recording exceeds the selected profile's duration limit. Choose another profile."
        return ""
    }
    fun statistics(selection: MachineKey, machines: List<Machine>): MachineKey? = machines
        .filter { it.connectionId == selection.connectionId && it.machineId == selection.computerId }
        .singleOrNull()?.let { MachineKey(it.connectionId, it.id) }
    fun migratedSelection(selection: SpeechSelection, computers: List<VoiceComputer>, machines: List<Machine>): SpeechSelection {
        val saved = selection.host ?: return selection
        if (computers.any { it.id == saved.computerId }) return selection
        val native = machines.singleOrNull { it.connectionId == saved.connectionId && it.id == saved.computerId }?.machineId
        if (native.isNullOrBlank() || computers.none { it.id == native }) return selection
        return selection.copy(host = saved.copy(computerId = native))
    }
}

internal object VoiceCodec {
    private fun draft(value: VoiceDraft) = MessageCodec.targetJson(value.target).put("machine", value.machineId)
        .put("profile", value.profileId).put("profile_revision", value.profileRevision).put("audio", value.audio.toJson()).put("duration_ms", value.durationMs).put("created", value.createdAt)
        .put("video", value.video?.let(VideoProtocol::draftJson)).put("resubmission_id", value.resubmissionId).put("rejection", value.rejection)
    private fun draft(raw: JSONObject) = VoiceDraft(MessageCodec.target(raw), raw.getString("machine"), raw.getString("profile"),
        Attachment.fromJson(raw.getJSONObject("audio")), raw.getInt("duration_ms"), raw.getDouble("created"), SpeechProtocol.revision(raw), raw.optJSONObject("video")?.let(VideoProtocol::draft), raw.string("resubmission_id"), raw.string("rejection"))
    fun encode(value: VoiceState) = JSONObject().put("schema", 1)
        .put("selection", SpeechProtocol.encode(SpeechState(value.selection)).getJSONObject("selection")).put("video_mode", value.videoMode)
        .put("preview_before_send", value.previewBeforeSend)
        .put("draft", value.draft?.let(::draft)).put("outgoing", JSONArray(value.outgoing.map { entry ->
            MessageCodec.targetJson(entry.target).put("id", entry.id).put("machine", entry.machineId).put("profile", entry.profileId).put("profile_revision", entry.profileRevision)
                .put("audio", entry.audio?.toJson()).put("duration_ms", entry.durationMs).put("created", entry.createdAt)
                .put("message", entry.message?.let(VoiceProtocol::json)).put("upload_state", entry.uploadState).put("error", entry.error)
                .put("video", entry.video?.let(VideoProtocol::draftJson)).put("rejection", entry.rejection)
                .put("attempted_rejection_generation", entry.attemptedRejectionGeneration).put("operation_id", entry.operationId)
        }))
    fun decode(raw: JSONObject): VoiceState {
        require(raw.optInt("schema", 1) == 1)
        val entries = raw.optJSONArray("outgoing")?.objects().orEmpty(); require(entries.size <= 100)
        return VoiceState(SpeechProtocol.decode(JSONObject().put("selection", raw.optJSONObject("selection"))).selection,
            raw.optJSONObject("draft")?.let(::draft), entries.map { row ->
                val id = row.getString("id"); require(AttachmentPolicy.validId(id))
                val target = MessageCodec.target(row)
                val message = row.optJSONObject("message")?.let { VoiceProtocol.response(JSONObject().put("schema", 1).put("message", it), target, id) }
                VoiceOutgoing(id, target, row.getString("machine"), row.getString("profile"), row.optJSONObject("audio")?.let(Attachment::fromJson),
                    row.getInt("duration_ms"), row.getDouble("created"), message, row.getString("upload_state"), row.string("error"), SpeechProtocol.revision(row), row.optJSONObject("video")?.let(VideoProtocol::draft), row.string("rejection"), row.optLong("attempted_rejection_generation"), row.string("operation_id").ifBlank { id }).recover()
            }.also { require(it.map(VoiceOutgoing::id).distinct().size == it.size) }, raw.optBoolean("video_mode"), raw.optBoolean("preview_before_send"))
    }
}

internal object VoiceHistory {
    fun merge(target: Target, native: List<Event>, outgoing: List<VoiceOutgoing>): List<Event> {
        val local = outgoing.filter { it.belongsTo(target) }.associateBy { it.id }
        val seen = mutableSetOf<String>()
        val authoritative = native.filter { it.voice != null }.groupBy { it.voice!!.id }.mapValues { (_, rows) ->
            rows.maxWith(compareBy<Event> { it.voice!!.revision }.thenBy { it.historyId.isNotBlank() })
        }
        val rows = native.filter { it.voice == null || authoritative[it.voice.id] === it }.map { event ->
            val voice = event.voice ?: return@map event
            seen += voice.id
            val own = local[voice.id]
            val latest = own?.message?.let { VoiceProtocol.newer(voice, it) } ?: voice
            val audio = own?.audio?.let { latest.audio.copy(localId = it.id, sha256 = it.sha256) } ?: latest.audio
            val video = latest.video?.let { remote -> own?.video?.let { local ->
                remote.copy(file = remote.file.copy(localId = local.file.id, sha256 = local.file.sha256))
            } ?: remote } ?: own?.video?.displayed(voice.id)
            event.copy(id = "voice:${voice.id}", text = latest.transcript, voice = latest.copy(audio = audio, video = video),
                attachments = listOfNotNull(audio, video?.file), requestId = voice.id, outgoingId = voice.id)
        }.toMutableList()
        local.values.filter { it.id !in seen }.forEach { own ->
            val voice = own.displayed().let { message -> own.audio?.let { message.copy(audio = message.audio.copy(localId = it.id, sha256 = it.sha256)) } ?: message }
            val event = Event("voice:${own.id}", "You", voice.transcript, "", own.createdAt, own.id,
                listOfNotNull(voice.audio, voice.video?.file), "hgs_voice", outgoingId = own.id, nativeType = "UserMessage", voice = voice)
            val position = rows.indexOfFirst { it.at > event.at }
            if (position < 0) rows += event else rows.add(position, event)
        }
        return rows
    }
}
