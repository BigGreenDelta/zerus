package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.json.JSONObject

/** Global defaults use computer reads; they never authorize a conversation or alter a saved recording. */
internal class DefaultVoiceProviders(private val model: ZerusViewModel, private val scope: CoroutineScope,
    private val api: RelayApi) {
    val host get() = DefaultVoiceRoutes.resolve(model.speech.selection.host, model.machines, model.connections)
    fun hosts(query: String = "") = DefaultVoiceRoutes.hosts(model.machines, model.connections, query)
    private var receivedProfiles by mutableStateOf<List<SpeechProfile>>(emptyList())
    val profiles get() = receivedProfiles.takeIf { confirmed == host && host?.let { reason(it).isBlank() && connection(it) == confirmedConnection } == true }.orEmpty()
    var loading by mutableStateOf(false); private set
    var notice by mutableStateOf(""); private set
    private var confirmed: SpeechConfigHost? = null
    private var confirmedConnection: Connection? = null
    private var requested: SpeechConfigHost? = null
    private var requestedConnection: Connection? = null
    private var generation = 0L
    private var job: Job? = null

    fun reason(captured: SpeechConfigHost) = model.defaultVoiceProfilesReason(captured)
    private fun connection(captured: SpeechConfigHost) = model.connections.singleOrNull { it.id == captured.key.connectionId }
    fun refresh() {
        if (!model.speech.ready) return
        val captured = host
        val route = captured?.let(::connection)
        if (loading && requested == captured && requestedConnection == route && captured != null && reason(captured).isBlank()) return
        val version = ++generation
        job?.cancel(); requested = captured; requestedConnection = route; confirmed = null; confirmedConnection = null
        loading = false; receivedProfiles = emptyList()
        if (captured == null) {
            notice = if (model.speech.selection.host != null) "The saved voice computer is unavailable. Choose an online computer."
                else if (hosts().isEmpty()) "No verified online voice computers are available. Refresh Machines and try again."
                else "Choose a voice computer to load its profiles."
            return
        }
        notice = reason(captured)
        if (notice.isNotBlank()) return
        val connection = checkNotNull(route)
        loading = true
        job = scope.launch {
            try {
                val result = api.speech(connection, captured.key, "speech_profiles", JSONObject())
                val received = SpeechProtocol.profiles(result)
                if (version != generation || host != captured || connection(captured) != connection || reason(captured).isNotBlank()) return@launch
                receivedProfiles = received; confirmed = captured; confirmedConnection = connection
                notice = when {
                    !result.optBoolean("available", true) -> result.string("reason").ifBlank { "The speech service is unavailable on this computer." }
                    received.isEmpty() -> "No voice profiles are configured on this computer. Manage speech providers to add one."
                    else -> ""
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) {
                if (version == generation && host == captured) notice = if (error is java.io.IOException)
                    "Could not connect to this computer. Refresh voice profiles to try again."
                else error.message.orEmpty().ifBlank { "Could not load voice profiles. Refresh to try again." }
            } finally { if (version == generation) loading = false }
        }
    }
    fun chooseHost(captured: SpeechConfigHost): Boolean {
        if (reason(captured).isNotBlank()) return false
        val current = model.speech.selection
        val sameHost = host == captured
        if (!model.speech.chooseDefault(SpeechSelection(MachineKey(captured.key.connectionId, captured.machineId),
                current.profileId.takeIf { sameHost }.orEmpty(), current.profileRevision.takeIf { sameHost }))) return false
        refresh()
        return true
    }
    fun chooseProfile(profile: SpeechProfile): Boolean {
        val captured = host ?: return false
        if (loading || confirmed != captured || reason(captured).isNotBlank() || profile !in profiles) return false
        return model.speech.chooseDefault(SpeechSelection(MachineKey(captured.key.connectionId, captured.machineId), profile.id, profile.profileRevision))
    }
}

internal object DefaultVoiceRoutes {
    private fun eligible(machine: Machine, connections: List<Connection>) = machine.online && !machine.lastKnown &&
        AttachmentPolicy.validId(machine.machineId) && connections.any { it.id == machine.connectionId }
    fun resolve(saved: MachineKey?, machines: List<Machine>, connections: List<Connection>): SpeechConfigHost? {
        if (saved == null) return null
        val machine = machines.singleOrNull { eligible(it, connections) && it.connectionId == saved.connectionId &&
            (it.machineId == saved.computerId || it.id == saved.computerId) } ?: return null
        return SpeechConfigHost(MachineKey(machine.connectionId, machine.id), machine.machineId, machine.name)
    }
    fun hosts(machines: List<Machine>, connections: List<Connection>, query: String = ""): List<SpeechConfigHost> = machines.asSequence()
        .filter { eligible(it, connections) && it.name.contains(query.trim(), ignoreCase = true) }
        .map { SpeechConfigHost(MachineKey(it.connectionId, it.id), it.machineId, it.name) }
        .distinctBy { MachineKey(it.key.connectionId, it.machineId) }.take(100).toList()
}
