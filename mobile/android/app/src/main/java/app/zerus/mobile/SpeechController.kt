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
    var profileNotice by mutableStateOf(""); private set
    var recording by mutableStateOf(false); private set
    var savingRecording by mutableStateOf(false); private set
    var seconds by mutableStateOf(0); private set
    var notice by mutableStateOf(""); private set
    var statistics by mutableStateOf(""); private set
    var activeRequests by mutableStateOf<Set<String>>(emptySet()); private set
    var legacyResult by mutableStateOf<SpeechAttempt?>(null); private set
    val busy get() = recording || savingRecording || activeRequests.isNotEmpty()
    private class Capture(val id: Long, val recorder: SpeechRecorder) { var discarded = false }
    private var capture: Capture? = null
    private var captureSequence = 0L
    val activeCaptureId get() = capture?.id.takeIf { recording }
    var gestureCaptureId by mutableStateOf<Long?>(null); private set
    var gestureCancelProgress by mutableStateOf(0f); private set
    var captureFailure by mutableStateOf<Pair<Long, String>?>(null); private set
    val recordingKeys = RecordingKeys()
    private var profileJob: Job? = null
    private var profileRevision = 0L
    private var profileTarget: Target? = null
    private var requestedProfileTarget: Target? = null
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
    private fun choose(selection: SpeechSelection, target: Target, previewId: String?): Boolean {
        if (!ready || recording || savingRecording || model.updateInstallPreparing || loading || profileTarget != target) return false
        val captured = draft
        if (previewId != null && (captured?.audio?.id != previewId || captured.target != target)) return false
        value = value.copy(selection = selection, draft = captured?.let {
            if (previewId != null) it.copy(machineId = selection.host?.computerId.orEmpty(), profileId = selection.profileId, profileRevision = selection.profileRevision) else it
        })
        return true
    }
    private fun choiceSelection(target: Target, previewId: String?): SpeechSelection? =
        if (previewId == null) selection else draft?.takeIf { it.audio.id == previewId && it.target == target }?.selection
    fun chooseHost(host: MachineKey, target: Target? = model.selected?.target, previewId: String? = null) {
        if (target == null || host.connectionId != target.connectionId || computers.none { it.id == host.computerId }) return
        val current = choiceSelection(target, previewId) ?: return
        val sameHost = current.host?.computerId == host.computerId
        val next = SpeechSelection(host, current.profileId.takeIf { sameHost }.orEmpty(), current.profileRevision.takeIf { sameHost })
        if (!choose(next, target, previewId)) return
        scope.launch { try { persist(); refreshStatistics() } catch (_: Exception) { notice = "Could not save the voice computer." } }
    }
    fun chooseProfile(id: String, target: Target? = model.selected?.target, previewId: String? = null) {
        if (target == null) return
        val current = choiceSelection(target, previewId) ?: return
        val computer = computers.find { it.id == current.host?.computerId } ?: return
        val selectedProfile = computer.profiles.find { it.id == id } ?: return
        if (!choose(current.copy(host = current.host?.copy(connectionId = target.connectionId), profileId = id,
            profileRevision = selectedProfile.profileRevision), target, previewId)) return
        scope.launch { try { persist() } catch (_: Exception) { notice = "Could not save the voice profile." } }
    }
    fun chooseDefault(selection: SpeechSelection): Boolean {
        if (!ready || recording || savingRecording || model.updateInstallPreparing) return false
        value = value.copy(selection = selection) // Retained recordings keep their independently pinned route.
        scope.launch { try { persist(); refreshStatistics() } catch (_: Exception) { notice = "Could not save the default voice provider." } }
        return true
    }
    fun refreshProfiles(target: Target? = model.selected?.target) {
        if (!ready || loading && requestedProfileTarget == target) return
        val revision = ++profileRevision
        profileJob?.cancel(); loading = false; requestedProfileTarget = target; profileTarget = null
        if (target == null || target.archiveId.isNotBlank() || target.agentId.isNotBlank()) {
            computers = emptyList()
            profileNotice = "Open a conversation to see the voice computers it can reach. Your selection is saved."
            return
        }
        val reason = model.voiceHostReason(target)
        if (reason.isNotBlank()) { computers = emptyList(); profileNotice = reason; return }
        loading = true; profileNotice = ""
        profileJob = scope.launch {
            try {
                val id = UUID.randomUUID().toString()
                val result = api.voice(connection(target), target, "voice_profiles", VoiceProtocol.payload(target, id), id)
                val received = VoiceProtocol.computers(result)
                if (requestedProfileTarget != target || profileRevision != revision) return@launch
                computers = received; profileTarget = target
                val migrated = VoiceRoutes.migratedSelection(selection, received, model.machines)
                profileNotice = if (received.isEmpty()) "No voice profiles are available to this conversation computer." else ""
                if (migrated != selection) {
                    value = value.copy(selection = migrated)
                    persist()
                }
                if (requestedProfileTarget == target && profileRevision == revision) refreshStatistics()
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { if (requestedProfileTarget == target && profileRevision == revision) { computers = emptyList(); profileTarget = null; profileNotice = error.message ?: "Voice profiles are unavailable." } }
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
        model.demo -> "Open a connected conversation to record a voice message."
        model.updateInstallPreparing -> "Finish the app update first."
        recording || savingRecording || draft != null -> "Send or discard the saved recording first."
        model.sendingBlocked(target) -> "Finish the pending message or session action before recording."
        target.archiveId.isNotBlank() || target.agentId.isNotBlank() -> "Voice messages are available in live parent conversations."
        target.run.isBlank() || target.conversation.isBlank() -> "Wait for the conversation to be ready before recording."
        outgoing.sumOf { it.audio?.bytes ?: 0 } > 100L * 1024 * 1024 - VoiceProtocol.MAX_PCM_BYTES - 44 -> "Remove an old local voice copy in Drafts to make room for another recording."
        model.selected?.target != target -> "Open the conversation first."
        else -> ""
    }
    fun permissionDenied() { notice = "Microphone access was denied. Allow microphone access in Android app settings to record." }
    fun start(target: Target): Long? = start(target, gesture = false)
    fun startGesture(target: Target): Long? = start(target, gesture = true)
    private fun start(target: Target, gesture: Boolean): Long? {
        val reason = startReason(target)
        if (reason.isNotBlank()) { notice = reason; return null }
        val machine = selection.host?.computerId.orEmpty()
        val profileId = selection.profileId
        val profileRevision = if (profileTarget == target && !loading && profile != null) profile?.profileRevision else selection.profileRevision
        model.attachments.stopAudio()
        seconds = 0; notice = ""; captureFailure = null; recording = true
        val owned = Capture(++captureSequence, SpeechRecorder(context)); capture = owned
        gestureCaptureId = owned.id.takeIf { gesture }; gestureCancelProgress = 0f
        scope.launch {
            var imported: Attachment? = null
            try {
                val pcm = owned.recorder.capture(120_000) { if (capture === owned) seconds = it }
                // Natural completion and the duration limit retire this touch press before saving.
                retireGesture(owned.id)
                recording = false
                if (owned.discarded) { notice = "Recording discarded."; return@launch }
                check(pcm.size >= 3_200) { "The recording was too short. Try again." }
                savingRecording = true
                imported = audioStore.importRecording(VoiceProtocol.wav(pcm))
                value = value.copy(draft = VoiceDraft(target, machine, profileId, imported, pcm.size / 32, System.currentTimeMillis() / 1000.0, profileRevision))
                persist()
                imported = null
                notice = "Recording saved. Send it when ready."
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) {
                notice = if (owned.discarded) "Recording discarded." else error.message ?: "Could not save the recording."
                if (!owned.discarded) captureFailure = owned.id to notice
            }
            finally {
                if (capture === owned) {
                    retireGesture(owned.id)
                    recording = false; savingRecording = false; capture = null
                }
                // A committed preview remains recoverable if the Activity or coroutine stops.
                imported?.takeIf { draft?.audio?.id != it.id }?.let { file ->
                    withContext(NonCancellable) { runCatching { audioStore.remove(file.id, protectedAudioIds()) } }
                }
            }
        }
        refreshProfiles(target)
        return owned.id
    }
    private fun retireGesture(id: Long) {
        if (gestureCaptureId == id) { gestureCaptureId = null; gestureCancelProgress = 0f }
    }
    fun moveGesture(id: Long, progress: Float) {
        if (gestureCaptureId == id && capture?.id == id) gestureCancelProgress = progress.coerceIn(0f, 1f)
    }
    fun finishGesture(id: Long, cancel: Boolean): Boolean {
        val owned = capture?.takeIf { it.id == id && gestureCaptureId == id } ?: return false
        retireGesture(id)
        if (cancel) owned.discarded = true
        owned.recorder.stop()
        return true
    }
    fun stopCapture(id: Long) { capture?.takeIf { it.id == id }?.let { retireGesture(it.id); it.recorder.stop() } }
    fun cancelCapture(id: Long) {
        capture?.takeIf { it.id == id && recording }?.let { it.discarded = true; stopCapture(id) }
    }
    fun stop() { capture?.let { stopCapture(it.id) } }
    fun background() { stop() }
    fun protectedAudioIds() = (outgoing.mapNotNull { it.audio?.id } + listOfNotNull(draft?.audio?.id)).toSet()
    fun discardRecording() {
        if (recording) { activeCaptureId?.let(::cancelCapture); return }
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
    private fun providerReason(captured: VoiceDraft) = VoiceRoutes.providerReason(captured, profileTarget, loading, computers)
    private fun validateSend(captured: VoiceDraft, allowColdCache: Boolean) {
        check(model.voiceSendReason(captured.target).isBlank()) { "The conversation changed before sending. The recording remains saved." }
        check(providerReason(captured).isBlank()) { providerReason(captured) }
        check(allowColdCache || !model.voiceColdCache()) { "The prompt cache expired. The recording remains saved without sending." }
    }
    fun sendReason(): String {
        val captured = draft ?: return "Record a voice message first."
        return when {
            savingRecording -> "Saving the recording…"
            outgoing.size >= 100 -> "Remove an old local voice receipt in Drafts before sending another."
            outgoing.any { it.belongsTo(captured.target) && !it.terminal } -> "Wait for the previous voice message to finish."
            outgoing.any { it.belongsTo(captured.target) && it.state == "uncertain" } -> "Check the previous voice message before sending another."
            else -> model.voiceSendReason(captured.target).ifBlank { providerReason(captured) }
        }
    }
    fun submit(allowColdCache: Boolean = false) {
        val captured = draft ?: return
        val reason = sendReason()
        if (reason.isNotBlank()) { notice = reason; return }
        if (model.voiceColdCache() && !allowColdCache) { notice = "Confirm sending with full context first."; return }
        val id = UUID.randomUUID().toString()
        val pending = VoiceOutgoing(id, captured.target, captured.machineId, captured.profileId, captured.audio, captured.durationMs, captured.createdAt, profileRevision = captured.profileRevision)
        savingRecording = true; activeRequests = activeRequests + id
        scope.launch {
            var attempted = false
            try {
                val route = connection(captured.target)
                value = value.copy(draft = null, outgoing = outgoing + pending)
                persist() // UUID and immutable audio are durable before the only upload.
                savingRecording = false
                model.voiceSent(captured.target, id)
                validateSend(captured, allowColdCache)
                val bytes = ByteArrayOutputStream()
                audioStore.decrypt(captured.audio) { bytes.write(it) }
                val payload = withContext(Dispatchers.Default) {
                    VoiceProtocol.payload(captured.target, id)
                        .put("speech", VoiceProtocol.provider(captured))
                        .put("audio", JSONObject().put("format", "pcm_s16le").put("sample_rate", 16_000).put("channels", 1)
                            .put("data_base64", Base64.getEncoder().encodeToString(VoiceProtocol.pcm(bytes.toByteArray()))))
                }
                validateSend(captured, allowColdCache)
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
        val restored = VoiceDraft(current.target, current.machineId, current.profileId, current.audio, current.durationMs, current.createdAt, current.profileRevision)
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
