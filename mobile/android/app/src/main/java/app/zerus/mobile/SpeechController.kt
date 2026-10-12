package app.zerus.mobile

import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.util.Base64
import java.util.UUID

/** The phone owns capture and one explicit upload; the conversation computer owns delivery. */
internal class SpeechController(private val context: Context, private val model: ZerusViewModel,
    private val scope: CoroutineScope, private val api: RelayApi) {
    private val store = PrivateStore(context)
    private val audioStore = AttachmentStore(context)
    private val saveLock = Mutex()
    private var value by mutableStateOf(VoiceState())
    val selection get() = value.selection
    val draft get() = value.draft
    val outgoing get() = value.outgoing
    val recorded get() = draft != null
    var ready by mutableStateOf(false); private set
    var computers by mutableStateOf<List<VoiceComputer>>(emptyList()); private set
    val profiles get() = computers.find { it.id == selection.host?.computerId }?.profiles.orEmpty()
    val profile get() = profiles.find { it.id == selection.profileId }
    var loading by mutableStateOf(false); private set
    var recording by mutableStateOf(false); private set
    var savingRecording by mutableStateOf(false); private set
    var seconds by mutableStateOf(0); private set
    var notice by mutableStateOf(""); private set
    var statistics by mutableStateOf(""); private set
    var activeRequests by mutableStateOf<Set<String>>(emptySet()); private set
    var legacyResult by mutableStateOf<SpeechAttempt?>(null); private set
    val busy get() = recording || savingRecording || activeRequests.isNotEmpty()
    private var recorder: SpeechRecorder? = null
    val recordingKeys = RecordingKeys()
    private var discardWhenStopped = false
    private var profileJob: Job? = null
    private var profileRevision = 0L
    private var profileTarget: Target? = null
    private var statsJob: Job? = null
    private val statusJobs = mutableMapOf<String, Job>()

    init {
        scope.launch {
            try {
                value = withContext(Dispatchers.IO) { store.voiceState() }
                legacyResult = withContext(Dispatchers.IO) { store.speechState().attempt }
                ready = true
                if (draft != null) notice = "Your recording is saved. Play it, send it, or discard it."
            } catch (_: Exception) { notice = "Private voice storage could not be opened. Recording is unavailable." }
        }
    }
    private suspend fun persist() = saveLock.withLock {
        val snapshot = value
        withContext(Dispatchers.IO) { store.saveVoiceState(snapshot) }
    }
    private fun update(id: String, change: (VoiceOutgoing) -> VoiceOutgoing) {
        value = value.copy(outgoing = value.outgoing.map { if (it.id == id) change(it) else it })
    }
    fun chooseHost(host: MachineKey) {
        if (!ready || recording || savingRecording || model.updateInstallPreparing || computers.none { it.id == host.computerId }) return
        val profiles = computers.first { it.id == host.computerId }.profiles
        value = value.copy(selection = SpeechSelection(host, profiles.firstOrNull { it.available }?.id.orEmpty()))
        scope.launch { try { persist(); refreshStatistics() } catch (_: Exception) { notice = "Could not save the voice computer." } }
    }
    fun chooseProfile(id: String) {
        if (!ready || recording || savingRecording || profiles.none { it.id == id } || model.updateInstallPreparing) return
        value = value.copy(selection = selection.copy(profileId = id))
        scope.launch { try { persist() } catch (_: Exception) { notice = "Could not save the voice profile." } }
    }
    fun refreshProfiles() {
        val target = model.selected?.target
        if (!ready || recording || savingRecording) return
        val revision = ++profileRevision
        profileJob?.cancel(); loading = false
        if (target == null || target.archiveId.isNotBlank() || target.agentId.isNotBlank()) {
            computers = emptyList(); profileTarget = null
            notice = "Open a conversation to see the voice computers it can reach. Your selection is saved."
            return
        }
        val reason = model.voiceHostReason(target)
        if (reason.isNotBlank()) { computers = emptyList(); profileTarget = null; notice = reason; return }
        loading = true
        profileJob = scope.launch {
            try {
                val id = UUID.randomUUID().toString()
                val result = api.voice(connection(target), target, "voice_profiles", VoiceProtocol.payload(target, id), id)
                val received = VoiceProtocol.computers(result)
                if (model.selected?.target != target || profileRevision != revision) return@launch
                computers = received; profileTarget = target
                val migrated = VoiceRoutes.migratedSelection(selection, received, model.machines)
                if (migrated != selection) { value = value.copy(selection = migrated); persist() }
                selection.host?.takeIf { it.connectionId != target.connectionId && received.any { machine -> machine.id == it.computerId } }?.let { host ->
                    value = value.copy(selection = selection.copy(host = host.copy(connectionId = target.connectionId)))
                    persist()
                }
                if (selection.host == null) received.firstOrNull { machine -> machine.profiles.any { it.available } }?.let { machine ->
                    value = value.copy(selection = SpeechSelection(MachineKey(target.connectionId, machine.id), machine.profiles.first { it.available }.id))
                    persist()
                }
                notice = if (received.isEmpty()) "No voice profiles are available to this conversation computer." else ""
                refreshStatistics()
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { if (profileRevision == revision) { computers = emptyList(); profileTarget = null; notice = error.message ?: "Voice profiles are unavailable." } }
            finally { if (profileRevision == revision) loading = false }
        }
    }
    private fun refreshStatistics() {
        statsJob?.cancel(); statistics = ""
        val selected = selection.host ?: return
        val host = VoiceRoutes.statistics(selected, model.machines) ?: run {
            statistics = "Local voice statistics are unavailable through this gateway."; return
        }
        if (!model.supportsSpeechStats(host)) return
        val route = model.connections.find { it.id == host.connectionId } ?: return
        statsJob = scope.launch {
            try {
                val stats = api.speech(route, host, "speech_stats", JSONObject())
                check(stats.optInt("schema") == 1)
                if (selection.host == selected) {
                    val totals = stats.optJSONObject("totals") ?: JSONObject()
                    statistics = if (!stats.optBoolean("enabled")) "Local voice statistics are disabled on this computer."
                        else "Last ${stats.optInt("retention_days", 90)} days: ${totals.optLong("completed")} transcribed, ${totals.optLong("failed")} failed, ${totals.optLong("audio_ms") / 60_000} audio minutes."
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (selection.host == selected) statistics = "Local voice statistics are unavailable." }
        }
    }
    fun startReason(target: Target): String = when {
        !ready || !model.storageReady -> "Private storage is not ready."
        model.updateInstallPreparing -> "Finish the app update first."
        recording || savingRecording || draft != null -> "Send or discard the saved recording first."
        model.sendingBlocked(target) -> "Finish the pending message or session action before recording."
        target.archiveId.isNotBlank() || target.agentId.isNotBlank() -> "Voice messages are available in live parent conversations."
        outgoing.sumOf { it.audio?.bytes ?: 0 } > 100L * 1024 * 1024 - VoiceProtocol.MAX_PCM_BYTES - 44 -> "Remove an old local voice copy in Drafts to make room for another recording."
        model.selected?.target != target -> "Open the conversation first."
        model.voiceHostReason(target).isNotBlank() -> model.voiceHostReason(target)
        loading -> "Loading voice profiles…"
        profileTarget != target -> "Refresh voice profiles for this conversation."
        selection.host == null || profile == null -> "Choose an available voice computer and profile."
        profile?.available != true -> profile?.reason?.ifBlank { "This voice profile is unavailable." }.orEmpty()
        else -> ""
    }
    fun permissionDenied() { notice = "Microphone access was denied. Allow microphone access in Android app settings to record." }
    fun start(target: Target) {
        val reason = startReason(target)
        if (reason.isNotBlank()) { notice = reason; return }
        val machine = selection.host!!.computerId
        val profileId = selection.profileId
        val duration = profile!!.maxDurationMs
        model.attachments.stopAudio()
        discardWhenStopped = false; seconds = 0; notice = ""; recording = true
        val capture = SpeechRecorder(context); recorder = capture
        scope.launch {
            var imported: Attachment? = null
            try {
                val pcm = capture.capture(duration) { seconds = it }
                recording = false; recorder = null
                if (discardWhenStopped) { notice = "Recording discarded."; return@launch }
                check(pcm.size >= 3_200) { "The recording was too short. Try again." }
                savingRecording = true
                imported = audioStore.importRecording(VoiceProtocol.wav(pcm))
                value = value.copy(draft = VoiceDraft(target, machine, profileId, imported, pcm.size / 32, System.currentTimeMillis() / 1000.0))
                persist()
                imported = null
                notice = "Recording saved. Send it when ready."
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { notice = error.message ?: "Could not save the recording." }
            finally {
                recording = false; savingRecording = false; recorder = null
                // A committed preview remains recoverable if the Activity or coroutine stops.
                imported?.takeIf { draft?.audio?.id != it.id }?.let { file ->
                    withContext(NonCancellable) { runCatching { audioStore.remove(file.id, protectedAudioIds()) } }
                }
            }
        }
    }
    fun stop() { recorder?.stop() }
    fun background() { recorder?.stop() }
    fun protectedAudioIds() = (outgoing.mapNotNull { it.audio?.id } + listOfNotNull(draft?.audio?.id)).toSet()
    fun discardRecording() {
        if (recording) { discardWhenStopped = true; recorder?.stop(); return }
        val captured = draft ?: return
        if (savingRecording) return
        savingRecording = true
        scope.launch {
            try {
                value = value.copy(draft = null); persist()
                audioStore.remove(captured.audio.id, protectedAudioIds())
                notice = "Recording discarded."
            } catch (_: Exception) { value = value.copy(draft = captured); notice = "Could not discard the saved recording." }
            finally { savingRecording = false }
        }
    }
    private fun connection(target: Target): Connection {
        check(model.voiceHostReason(target).isBlank()) { model.voiceHostReason(target) }
        return checkNotNull(model.connections.find { it.id == target.connectionId }) { "The voice workspace is disconnected." }
    }
    fun sendReason(): String {
        val captured = draft ?: return "Record a voice message first."
        return when {
            savingRecording -> "Saving the recording…"
            outgoing.size >= 100 -> "Remove an old local voice receipt in Drafts before sending another."
            outgoing.any { it.belongsTo(captured.target) && !it.terminal } -> "Wait for the previous voice message to finish."
            outgoing.any { it.belongsTo(captured.target) && it.state == "uncertain" } -> "Check the previous voice message before sending another."
            else -> model.voiceSendReason(captured.target)
        }
    }
    fun submit(allowColdCache: Boolean = false) {
        val captured = draft ?: return
        val reason = sendReason()
        if (reason.isNotBlank()) { notice = reason; return }
        if (model.voiceColdCache() && !allowColdCache) { notice = "Confirm sending with full context first."; return }
        val id = UUID.randomUUID().toString()
        val pending = VoiceOutgoing(id, captured.target, captured.machineId, captured.profileId, captured.audio, captured.durationMs, captured.createdAt)
        savingRecording = true; activeRequests = activeRequests + id
        scope.launch {
            var attempted = false
            try {
                val route = connection(captured.target)
                value = value.copy(draft = null, outgoing = outgoing + pending)
                persist() // UUID and immutable audio are durable before the only upload.
                savingRecording = false
                model.voiceSent(captured.target, id)
                check(model.voiceSendReason(captured.target).isBlank()) { "The conversation changed before sending. The recording remains saved." }
                check(allowColdCache || !model.voiceColdCache()) { "The prompt cache expired. The recording remains saved without sending." }
                val bytes = ByteArrayOutputStream()
                audioStore.decrypt(captured.audio) { bytes.write(it) }
                val payload = withContext(Dispatchers.Default) {
                    VoiceProtocol.payload(captured.target, id)
                        .put("speech", JSONObject().put("machine_id", captured.machineId).put("profile_id", captured.profileId))
                        .put("audio", JSONObject().put("format", "pcm_s16le").put("sample_rate", 16_000).put("channels", 1)
                            .put("data_base64", Base64.getEncoder().encodeToString(VoiceProtocol.pcm(bytes.toByteArray()))))
                }
                attempted = true
                accept(pending, api.voice(route, captured.target, "voice_submit", payload, id))
            } catch (error: Exception) {
                withContext(NonCancellable) {
                    update(id) { it.copy(uploadState = if (attempted) "uncertain" else "failed",
                        error = if (attempted) "Sending was not confirmed. Check this message's status; it has not been resent." else error.message.orEmpty()) }
                    try { persist() } catch (_: Exception) { notice = "Could not save the outcome. The original voice request remains recoverable." }
                }
                if (error is CancellationException) throw error
            } finally { savingRecording = false; activeRequests = activeRequests - id }
            outgoing.find { it.id == id }?.takeIf { it.needsStatus }?.let { poll(it.id) }
        }
    }
    private suspend fun accept(previous: VoiceOutgoing, raw: JSONObject) {
        val next = VoiceProtocol.response(raw, previous.effectiveTarget, previous.id)
        previous.audio?.let { require(next.audio.bytes == it.bytes && next.durationMs == previous.durationMs) { "The computer returned different audio metadata." } }
        update(previous.id) { it.copy(message = VoiceProtocol.newer(it.message, next), error = "") }
        persist()
    }
    private suspend fun status(entry: VoiceOutgoing) {
        val id = UUID.randomUUID().toString()
        val target = entry.effectiveTarget
        accept(entry, api.voice(connection(target), target, "voice_status", VoiceProtocol.payload(target, id).put("message_id", entry.id), id))
    }
    fun checkStatus(entry: VoiceOutgoing) { poll(entry.id, force = true) }
    private fun poll(id: String, force: Boolean = false) {
        if (statusJobs[id]?.isActive == true || id in activeRequests) return
        statusJobs[id] = scope.launch {
            activeRequests = activeRequests + id
            try {
                repeat(300) { index ->
                    val entry = outgoing.find { it.id == id } ?: return@launch
                    if (!entry.needsStatus && !(force && index == 0)) return@launch
                    if (index > 0) delay(1500)
                    status(entry)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) {
                update(id) { it.copy(error = error.message ?: "Reconnect to check this voice message. No message has been resent.") }
                try { persist() } catch (_: Exception) { notice = "Could not save voice status." }
            } finally { activeRequests = activeRequests - id; statusJobs.remove(id) }
        }
    }
    /** Reconnect only performs reads. Other devices' in-progress rows use the same status path. */
    fun observe(target: Target, events: List<Event>) {
        if (!ready || target.archiveId.isNotBlank() || target.agentId.isNotBlank()) return
        var changed = false
        events.mapNotNull { it.voice }.forEach { voice ->
            if (voice.runId != target.run || voice.conversationId != target.conversation) return@forEach
            val previous = outgoing.find { it.id == voice.id && it.belongsTo(target) }
            if (previous != null) {
                val next = VoiceProtocol.newer(previous.message, voice)
                if (next != previous.message) { update(voice.id) { it.copy(message = next) }; changed = true }
            } else if (!voice.terminal && outgoing.size < 100) {
                value = value.copy(outgoing = outgoing + VoiceOutgoing(voice.id, target, "", "", null, voice.durationMs, voice.createdAt, voice))
                changed = true
            }
        }
        if (changed) scope.launch { try { persist() } catch (_: Exception) { notice = "Could not save voice status." } }
        outgoing.filter { it.belongsTo(target) && it.needsStatus }.forEach { poll(it.id) }
    }
    fun cancel(entry: VoiceOutgoing) {
        if (entry.state in setOf("submitted", "failed", "cancelled")) return
        val previous = statusJobs.remove(entry.id); previous?.cancel()
        scope.launch {
            previous?.join()
            if (entry.id in activeRequests) return@launch
            activeRequests = activeRequests + entry.id
            try {
                val id = UUID.randomUUID().toString(); val target = entry.effectiveTarget
                accept(entry, api.voice(connection(target), target, "voice_cancel", VoiceProtocol.payload(target, id).put("message_id", entry.id), id))
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { update(entry.id) { it.copy(error = error.message ?: "Cancellation was not confirmed. Check status.") }; persist() }
            finally { activeRequests = activeRequests - entry.id }
        }
    }
    fun dismiss(entry: VoiceOutgoing) {
        if (!entry.terminal || entry.id in activeRequests) return
        scope.launch {
            try {
                value = value.copy(outgoing = outgoing.filterNot { it.id == entry.id }); persist()
                entry.audio?.let { audioStore.remove(it.id, protectedAudioIds()) }
            } catch (_: Exception) { notice = "Could not remove the local voice receipt." }
        }
    }
    /** Only a locally proven pre-transport failure may become a new editable recording. */
    fun restoreUnsent(entry: VoiceOutgoing) {
        val current = outgoing.find { it.id == entry.id } ?: return
        if (current != entry || !current.canRestoreUnsent || current.audio == null ||
            current.id in activeRequests || draft != null || recording || savingRecording) return
        val restored = VoiceDraft(current.target, current.machineId, current.profileId, current.audio, current.durationMs, current.createdAt)
        savingRecording = true
        scope.launch {
            try {
                value = value.copy(draft = restored, outgoing = outgoing.filterNot { it.id == current.id }); persist()
                notice = "The unsent recording is ready. Open its original conversation before sending."
            } catch (_: Exception) { notice = "Could not restore the recording. Its saved receipt remains recoverable after restart." }
            finally { savingRecording = false }
        }
    }
    fun dismissLegacy() {
        scope.launch {
            try { withContext(Dispatchers.IO) { store.saveSpeechState(SpeechState(selection)) }; legacyResult = null }
            catch (_: Exception) { notice = "Could not dismiss the old transcription." }
        }
    }
}
