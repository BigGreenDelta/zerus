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

/** Management is pinned to an authorized processing computer, independent of open conversations. */
internal class SpeechConfigurationController(context: Context, private val model: ZerusViewModel,
    private val scope: CoroutineScope, private val api: RelayApi) {
    private val store = PrivateStore(context)
    private val saves = Mutex()
    var ready by mutableStateOf(false); private set
    var host by mutableStateOf<SpeechConfigHost?>(null); private set
    var configuration by mutableStateOf<SpeechConfiguration?>(null); private set
    var loading by mutableStateOf(false); private set
    var notice by mutableStateOf(""); private set
    var pending by mutableStateOf<List<SpeechConfigOperation>>(emptyList()); private set
    var active by mutableStateOf<Set<String>>(emptySet()); private set
    var result by mutableStateOf<SpeechConfigResult?>(null); private set
    val busy get() = active.isNotEmpty()
    private var generation = 0L
    private var refreshJob: Job? = null
    init {
        scope.launch {
            try { pending = withContext(Dispatchers.IO) { store.speechConfigOperations() }; ready = true }
            catch (_: Exception) { notice = "Private configuration recovery could not be opened." }
        }
    }
    private suspend fun persist() = saves.withLock {
        val captured = pending
        withContext(Dispatchers.IO) { store.saveSpeechConfigOperations(captured) }
    }
    fun reason(captured: SpeechConfigHost) = model.speechConfigurationReason(captured)
    fun select(selected: SpeechConfigHost) {
        if (host == selected && configuration != null) return
        host = selected; configuration = null; result = null; notice = ""
        refresh()
    }
    fun refresh() {
        val captured = host ?: return
        if (!ready) return
        val version = ++generation
        refreshJob?.cancel(); configuration = null; loading = false
        val reason = reason(captured)
        if (reason.isNotBlank()) { notice = reason; return }
        loading = true
        refreshJob = scope.launch {
            try {
                val config = SpeechConfigProtocol.parse(api.speechConfiguration(connection(captured), captured, "speech_config", JSONObject()), captured)
                if (generation == version && host == captured && reason(captured).isBlank()) {
                    configuration = config; notice = ""
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (generation == version && host == captured) notice = "Could not load this computer's speech configuration. Refresh to try again." }
            finally { if (generation == version) loading = false }
        }
    }
    private fun connection(captured: SpeechConfigHost): Connection {
        check(reason(captured).isBlank()) { reason(captured) }
        return checkNotNull(model.connections.find { it.id == captured.key.connectionId })
    }
    fun canMutate(captured: SpeechConfigHost) = ready && host == captured && !loading && reason(captured).isBlank() &&
        configuration?.machineId == captured.machineId && pending.none { it.host.key == captured.key && it.host.machineId == captured.machineId }
    fun useForNewRecordings(profile: SpeechConfigProfile): Boolean {
        val captured = host ?: return false
        if (!canMutate(captured) || !profile.available || configuration?.profiles?.none { it == profile } != false) return false
        return model.speech.chooseDefault(SpeechSelection(MachineKey(captured.key.connectionId, captured.machineId), profile.id, profile.revision))
    }
    fun apply(operation: SpeechConfigOperation): Boolean {
        val config = configuration ?: return false
        if (!canMutate(operation.host) || pending.size >= 32 || operation.operation !in SpeechConfigProtocol.mutations) return false
        val body = JSONObject(operation.payload)
        if (operation.operation == "speech_profile_test") {
            if (config.profiles.none { it.id == body.string("profile_id") && it.revision == body.optLong("expected_profile_revision") }) return false
        } else if (body.optLong("expected_revision", -1) != config.revision) return false
        pending = pending + operation; active = active + operation.id; result = null; notice = ""
        scope.launch {
            var submitted = false
            var running = false
            try {
                persist() // Exact UUID and sealed envelope are durable before the only mutation.
                val route = connection(operation.host)
                submitted = true
                running = accept(operation, api.speechConfiguration(route, operation.host, operation.operation, body, operation.id)).state == "running"
            } catch (error: Exception) {
                withContext(NonCancellable) {
                    if (!submitted) {
                        pending = pending.filterNot { it.id == operation.id }
                        runCatching { persist() }
                    }
                    if (host == operation.host) notice = if (submitted)
                        "The outcome is unconfirmed. Check this operation's status; it has not been repeated."
                    else "The operation was not sent. Refresh the computer and try again."
                }
                if (error is CancellationException) throw error
            } finally { active = active - operation.id }
            if (pending.any { it.id == operation.id } && running) checkStatus(operation)
        }
        return true
    }
    private suspend fun accept(operation: SpeechConfigOperation, raw: JSONObject): SpeechConfigResult {
        val receipt = SpeechConfigProtocol.result(raw, operation)
        if (receipt.terminal && receipt.errorCode != "operation_not_found") {
            pending = pending.filterNot { it.id == operation.id }; persist()
        }
        if (host == operation.host) {
            result = receipt
            notice = when (receipt.state) {
                "completed" -> if (operation.operation == "speech_profile_test") "Provider test succeeded." else "Speech configuration saved."
                "failed" -> when (receipt.errorCode) {
                    "revision_conflict" -> "The configuration changed on another device. Refresh and review your edits before saving again."
                    "invalid_credential" -> "The API key could not be accepted. Refresh and enter it again."
                    "operation_not_found" -> "The computer has not seen this operation yet. Its original request may still arrive; check status again."
                    else -> receipt.error.ifBlank { "The computer could not complete this operation." }
                }
                "running" -> "Testing the saved provider…"
                else -> "The outcome is unconfirmed. Check its status; it has not been repeated."
            }
            if (receipt.state == "completed") refreshAfterSuccess()
        }
        return receipt
    }
    private fun refreshAfterSuccess() {
        val savedNotice = notice
        refresh()
        // Voice selection and any retained preview remain unchanged; only discovery refreshes.
        val voiceTarget = model.speech.draft?.target ?: model.selected?.target
        if (voiceTarget != null) model.speech.refreshProfiles(voiceTarget)
        notice = savedNotice
    }
    fun checkStatus(operation: SpeechConfigOperation) {
        if (!ready || operation.id in active || pending.none { it.id == operation.id } || reason(operation.host).isNotBlank()) return
        active = active + operation.id
        scope.launch {
            try {
                repeat(60) {
                    val receipt = accept(operation, api.speechConfiguration(connection(operation.host), operation.host, "speech_config_status",
                        JSONObject().put("operation_id", operation.id).put("expected_machine_id", operation.host.machineId)))
                    if (pending.none { it.id == operation.id } || receipt.state != "running") return@launch
                    delay(1_000)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { if (host == operation.host) notice = "Status could not be confirmed. The original operation has not been repeated." }
            finally { active = active - operation.id }
        }
    }
}
