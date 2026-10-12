package app.zerus.mobile

import android.content.Context
import android.util.Base64
import android.widget.Toast
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.UUID

internal class SpeechController(private val context: Context, private val model: ZerusViewModel,
    private val scope: CoroutineScope, private val api: RelayApi) {
    private val store = PrivateStore(context)
    private val saveLock = Mutex()
    private var value by mutableStateOf(SpeechState())
    val selection get() = value.selection
    val attempt get() = value.attempt
    var ready by mutableStateOf(false); private set
    var profiles by mutableStateOf<List<SpeechProfile>>(emptyList()); private set
    var loading by mutableStateOf(false); private set
    var recording by mutableStateOf(false); private set
    var seconds by mutableStateOf(0); private set
    var recorded by mutableStateOf(false); private set
    var working by mutableStateOf(false); private set
    var notice by mutableStateOf(""); private set
    var statistics by mutableStateOf(""); private set
    private var recorder: SpeechRecorder? = null
    private var audio: ByteArray? = null
    private var origin: Draft? = null
    private var navigation = -1L
    private var autoInsert = false
    private var submitWhenStopped = true
    private var discardWhenStopped = false
    private var profileJob: Job? = null
    private var profileRevision = 0L
    private var job: Job? = null
    val busy get() = recording || recorded || working
    val profile get() = profiles.find { it.id == selection.profileId }

    init {
        scope.launch {
            try {
                value = withContext(Dispatchers.IO) { store.speechState() }
                if (attempt != null) notice = if (attempt?.state == "completed") "A transcription is ready to review."
                    else "A previous voice request is saved. Check its status to recover the result."
                ready = true
            } catch (_: Exception) { notice = "Private voice settings could not be opened. Recording is unavailable." }
        }
    }
    private suspend fun persist() = saveLock.withLock {
        val snapshot = value
        withContext(Dispatchers.IO) { store.saveSpeechState(snapshot) }
    }
    fun chooseHost(host: MachineKey) {
        if (!ready || busy || model.updateInstallPreparing) return
        profileRevision++; profileJob?.cancel(); loading = false; profiles = emptyList(); statistics = ""
        value = value.copy(selection = SpeechSelection(host))
        scope.launch { try { persist(); refreshProfiles() } catch (_: Exception) { notice = "Could not save the voice computer." } }
    }
    fun chooseProfile(id: String) {
        if (!ready || busy || profiles.none { it.id == id } || model.updateInstallPreparing) return
        value = value.copy(selection = selection.copy(profileId = id))
        scope.launch { try { persist() } catch (_: Exception) { notice = "Could not save the voice profile." } }
    }
    fun refreshProfiles() {
        val host = selection.host ?: return
        if (!ready || busy) return
        val revision = ++profileRevision
        profileJob?.cancel()
        val reason = model.speechHostReason(host)
        if (reason.isNotBlank()) { loading = false; profiles = emptyList(); notice = reason; return }
        loading = true; notice = ""
        profileJob = scope.launch {
            try {
                val result = api.speech(connection(host), host, "speech_profiles", JSONObject())
                val received = SpeechProtocol.profiles(result)
                if (selection.host != host || profileRevision != revision) return@launch
                profiles = received
                if (selection.profileId.isBlank()) {
                    received.firstOrNull { it.available }?.let { value = value.copy(selection = selection.copy(profileId = it.id)); persist() }
                }
                notice = result.string("reason").ifBlank { if (received.isEmpty()) "No voice profiles are configured on this computer." else "" }
                if (model.supportsSpeechStats(host)) {
                    try {
                        val stats = api.speech(connection(host), host, "speech_stats", JSONObject())
                        check(stats.optInt("schema") == 1)
                        if (selection.host == host && profileRevision == revision) {
                            val totals = stats.optJSONObject("totals") ?: JSONObject()
                            statistics = if (!stats.optBoolean("enabled")) "Local voice statistics are disabled on this computer."
                            else "Last ${stats.optInt("retention_days", 90)} days: ${totals.optLong("completed")} transcribed, ${totals.optLong("failed")} failed, ${totals.optLong("audio_ms") / 60_000} audio minutes."
                        }
                    } catch (cancelled: CancellationException) { throw cancelled }
                    catch (_: Exception) { if (selection.host == host && profileRevision == revision) statistics = "Local voice statistics are unavailable." }
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { if (selection.host == host && profileRevision == revision) { profiles = emptyList(); notice = error.message ?: "Voice profiles are unavailable." } }
            finally { if (selection.host == host && profileRevision == revision) loading = false }
        }
    }
    fun startReason(target: Target): String = when {
        !ready || !model.storageReady -> "Private storage is not ready."
        model.updateInstallPreparing -> "Finish the app update first."
        model.demo -> "Connect a computer to use voice input."
        busy || attempt != null -> "Finish or review the current recording first."
        model.selected?.target != target || target.archiveId.isNotBlank() || model.draft(target).status != "editing" -> "Open an editable conversation first."
        selection.host == null -> "Choose a voice computer in Settings."
        model.speechHostReason(selection.host!!).isNotBlank() -> model.speechHostReason(selection.host!!)
        loading -> "Loading voice profiles…"
        profile == null -> "Choose an available voice profile in Settings."
        profile?.available != true -> profile?.reason?.ifBlank { "This voice profile is unavailable." }.orEmpty()
        else -> ""
    }
    fun permissionDenied() { notice = "Microphone access was denied. Allow microphone access in Android app settings to record." }
    fun start(target: Target) {
        val reason = startReason(target)
        if (reason.isNotBlank()) { notice = reason; return }
        origin = model.draft(target)
        navigation = model.speechNavigation
        autoInsert = true; submitWhenStopped = true; discardWhenStopped = false
        seconds = 0; notice = ""; recording = true
        val capture = SpeechRecorder(context); recorder = capture
        scope.launch {
            try {
                val pcm = capture.capture(profile!!.maxDurationMs) { seconds = it }
                recording = false; recorder = null
                if (discardWhenStopped) { notice = "Recording discarded."; return@launch }
                check(pcm.size >= 3_200) { "The recording was too short. Try again." }
                audio = pcm; recorded = true
                if (submitWhenStopped) submit() else notice = "Recording stopped when Zerus left the foreground. Transcribe or discard it."
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { notice = error.message ?: "Could not record audio."; Toast.makeText(context, notice, Toast.LENGTH_LONG).show() }
            finally { recording = false; recorder = null }
        }
    }
    fun stop() { submitWhenStopped = true; recorder?.stop() }
    fun background() { if (recording) { submitWhenStopped = false; recorder?.stop() } }
    fun discardRecording() {
        if (recording) { discardWhenStopped = true; recorder?.stop() }
        audio = null; recorded = false; autoInsert = false
    }
    private fun connection(host: MachineKey): Connection {
        check(model.speechHostReason(host).isBlank()) { model.speechHostReason(host) }
        return checkNotNull(model.connections.find { it.id == host.connectionId }) { "The voice workspace is disconnected." }
    }
    fun submit() {
        if (!recorded || working || attempt != null) return
        val pcm = audio ?: return
        val host = selection.host ?: return
        val captured = origin ?: return
        val pending = SpeechAttempt(UUID.randomUUID().toString(), host, selection.profileId, captured)
        working = true; notice = "Sending recording…"
        job = scope.launch {
            try {
                val route = connection(host)
                value = value.copy(attempt = pending)
                recorded = false; audio = null
                persist() // Durable request identity precedes the single upload attempt.
                val payload = withContext(Dispatchers.Default) {
                    JSONObject().put("request_id", pending.id).put("profile_id", pending.profileId).put("audio",
                        JSONObject().put("format", "pcm_s16le").put("sample_rate", 16_000).put("channels", 1)
                            .put("data_base64", Base64.encodeToString(pcm, Base64.NO_WRAP)))
                }
                accept(api.speech(route, host, "speech_submit", payload, pending.id), acknowledgment = true)
                poll()
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { notice = (error.message ?: "Voice submission was not confirmed.") + " Check status before recording again." }
            finally { working = false }
        }
    }
    fun checkStatus() {
        val pending = attempt ?: return
        if (working || pending.state == "completed") return
        working = true; notice = "Checking recording…"
        job = scope.launch {
            try { accept(api.speech(connection(pending.host), pending.host, "speech_status", JSONObject().put("job_id", pending.id))); poll() }
            catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { notice = error.message ?: "Status is unavailable. The original request remains saved." }
            finally { working = false }
        }
    }
    private suspend fun accept(result: JSONObject, acknowledgment: Boolean = false) {
        val previous = attempt ?: return
        val next = SpeechProtocol.job(result, previous)
        if (acknowledgment && next.state == "completed") {
            accept(api.speech(connection(previous.host), previous.host, "speech_status", JSONObject().put("job_id", previous.id)))
            return
        }
        value = value.copy(attempt = next)
        persist() // Save recovered text before attempting any composer change.
        when (next.state) {
            "completed" -> {
                if (next.text.isBlank()) notice = "No speech was recognized."
                else if (autoInsert && model.speechNavigation == navigation && model.selected?.target == next.origin.target &&
                    SpeechDrafts.unchanged(next, model.draft(next.origin.target))) insert(false)
                else notice = "Transcription is ready. Review it before inserting into the original conversation."
            }
            "failed", "uncertain" -> notice = next.error.ifBlank { "Recognition could not be completed." }
            "cancelled" -> notice = "Recognition cancelled."
            else -> notice = if (next.state == "running") "Transcribing…" else "Waiting for the voice computer…"
        }
    }
    private suspend fun poll() {
        repeat(300) {
            val pending = attempt ?: return
            if (pending.state !in setOf("queued", "running")) return
            delay(1_000)
            accept(api.speech(connection(pending.host), pending.host, "speech_status", JSONObject().put("job_id", pending.id)))
        }
        notice = "Recognition is taking longer than expected. Check status to continue waiting."
    }
    fun cancel() {
        val pending = attempt ?: return
        if (pending.state in setOf("completed", "failed", "cancelled")) return
        autoInsert = false
        val previous = job
        previous?.cancel()
        working = true
        job = scope.launch {
            previous?.join()
            working = true
            try {
                value = value.copy(attempt = pending.copy(state = "uncertain", error = "Cancellation requested. Check status.")); persist()
                accept(api.speech(connection(pending.host), pending.host, "speech_cancel", JSONObject().put("job_id", pending.id)), acknowledgment = true)
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { notice = error.message ?: "Cancellation was not confirmed. Check status." }
            finally { working = false }
        }
    }
    private suspend fun insert(explicit: Boolean) {
        val pending = attempt ?: return
        try {
            model.insertSpeech(pending, explicit)
            value = value.copy(attempt = null); persist()
            notice = "Transcription inserted into the draft."
        } catch (cancelled: CancellationException) { throw cancelled }
        catch (error: Exception) { notice = error.message ?: "The draft changed. Review the transcription." }
    }
    fun insertReviewed() { if (!working) scope.launch { insert(true) } }
    fun dismiss() {
        if (working || attempt?.state !in setOf("completed", "failed", "cancelled", "uncertain")) return
        value = value.copy(attempt = null); notice = ""; autoInsert = false
        scope.launch { try { persist() } catch (_: Exception) { notice = "Could not clear the saved voice result." } }
    }
}
