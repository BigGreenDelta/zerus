package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

internal enum class AnalyticsReadState(val label: String) {
    Ready("Ready to read"), Current("Reply received"), Offline("Offline"), Unsupported("Update the gateway and computer for analytics"),
    Unavailable("Read unavailable"), Invalid("Response rejected"), Unverified("Computer identity unavailable")
}
internal fun privateAnalyticsAccess(machineId: String, online: Boolean, ready: Boolean,
    relayOperations: Set<String>, computerOperations: Set<String>): AnalyticsReadState = when {
    !PrivateAnalyticsProtocol.validMachine(machineId) -> AnalyticsReadState.Unverified
    !online || !ready -> AnalyticsReadState.Offline
    PrivateAnalyticsProtocol.operation !in relayOperations || PrivateAnalyticsProtocol.operation !in computerOperations -> AnalyticsReadState.Unsupported
    else -> AnalyticsReadState.Ready
}
internal data class AnalyticsRoute(val connection: Connection, val key: MachineKey, val machineId: String, val name: String,
    val state: AnalyticsReadState, val speech: Boolean)
internal data class AnalyticsComputer(val machineId: String, val name: String, val state: AnalyticsReadState,
    val snapshot: AnalyticsSnapshot? = null, val speech: SpeechAnalytics? = null, val speechAt: Double? = null,
    val speechCurrent: Boolean = false)
internal data class PrivateAnalyticsState(val range: AnalyticsRange = AnalyticsRange.recent(7),
    val computers: List<AnalyticsComputer> = emptyList(), val loading: Boolean = false, val omitted: Int = 0)

internal suspend fun RelayApi.analytics(connection: Connection, host: MachineKey, machineId: String,
    range: AnalyticsRange): AnalyticsSnapshot {
    require(host.connectionId == connection.id && PrivateAnalyticsProtocol.validMachine(machineId))
    val id = UUID.randomUUID().toString()
    val initial = post(connection,host.computerId,"",PrivateAnalyticsProtocol.operation,range.json(),id)
    check(initial.string("request_id") == id)
    val receipt = await(connection, initial)
    check(receipt.string("request_id") == id && receipt.string("state") == "completed")
    return withContext(Dispatchers.Default) { PrivateAnalyticsProtocol.parse(receipt.getJSONObject("result"), machineId, range) }
}

/** Reads only while the page is open. No persistence, telemetry, transcript reads or collection controls. */
internal class PrivateAnalyticsController(private val routes: () -> List<AnalyticsRoute>,
    private val routeCurrent: (AnalyticsRoute, Boolean) -> Boolean, private val scope: CoroutineScope, private val api: RelayApi) {
    constructor(model: ZerusViewModel, scope: CoroutineScope, api: RelayApi) :
        this(model::privateAnalyticsRoutes, model::privateAnalyticsRouteCurrent, scope, api)
    var value by mutableStateOf(PrivateAnalyticsState()); private set
    private var job: Job? = null
    private var generation = 0L
    // Keep only source metadata across range/catalog changes. Never evict a purge guard to admit
    // another machine: after 32 accepted identities, new identities fail closed until disposal.
    private val sources = mutableMapOf<String, AnalyticsSourceGuard>()

    fun stop() { generation++; job?.cancel(); job = null; value = value.copy(loading = false) }

    fun reconcileAvailability() {
        val routes = routes().groupBy { it.machineId }
        value = value.copy(computers = value.computers.map { computer ->
            val available = routes[computer.machineId].orEmpty()
            if (available.any { it.state == AnalyticsReadState.Ready }) computer
            else computer.copy(state = available.minByOrNull { it.state.ordinal }?.state ?: AnalyticsReadState.Offline, speechCurrent = false)
        })
    }

    fun refresh(range: AnalyticsRange = value.range) {
        stop()
        val attempt = generation
        val all = routes().groupBy { it.machineId.ifBlank { "unverified:${it.key}" } }
        val groups = all.entries.take(PrivateAnalyticsProtocol.maxMachines)
        val previous = value.takeIf { it.range == range }?.computers.orEmpty().associateBy { it.machineId }
        value = PrivateAnalyticsState(range, groups.map { (_, routes) ->
            val route = routes.first()
            val old = previous[route.machineId]
            AnalyticsComputer(route.machineId, route.name, routes.minBy { it.state.ordinal }.state,
                old?.snapshot, old?.speech, old?.speechAt)
        }, loading = true, omitted = (all.size - groups.size).coerceAtLeast(0))
        job = scope.launch {
            try {
                val gate = Semaphore(2)
                coroutineScope {
                    groups.forEachIndexed { index, (_, routes) -> launch {
                        gate.withPermit {
                            var entry = value.computers[index]
                            var speechRead = false
                            for (route in routes.sortedBy { it.state.ordinal }.take(PrivateAnalyticsProtocol.maxRoutes)) {
                                val agentReadable = route.state == AnalyticsReadState.Ready && routeCurrent(route, true)
                                val speechReadable = route.speech && routeCurrent(route, false)
                                if (!agentReadable && !speechReadable) continue
                                if (agentReadable) try {
                                    val snapshot = api.analytics(route.connection, route.key, route.machineId, range)
                                    if (attempt != generation || !routeCurrent(route, true)) continue
                                    val source = sources[snapshot.machineId]
                                    require(source != null || sources.size < PrivateAnalyticsProtocol.maxMachines)
                                    val accepted = AnalyticsFleet.acceptSource(source, snapshot)
                                    if (accepted != null) {
                                        sources[snapshot.machineId] = accepted
                                        entry = entry.copy(snapshot = snapshot.copy(retiredEpochs = accepted.retiredEpochs), state = AnalyticsReadState.Current)
                                    } else if (entry.state != AnalyticsReadState.Current) {
                                        entry = entry.copy(state = AnalyticsReadState.Invalid)
                                    }
                                } catch (cancelled: CancellationException) { throw cancelled }
                                catch (_: IllegalArgumentException) { if (entry.state != AnalyticsReadState.Current) entry = entry.copy(state = AnalyticsReadState.Invalid) }
                                catch (_: Exception) { if (entry.state != AnalyticsReadState.Current) entry = entry.copy(state = AnalyticsReadState.Unavailable) }
                                if (!speechRead && speechReadable) {
                                    try {
                                        val raw = api.speech(route.connection, route.key, "speech_stats", JSONObject())
                                        val speech = withContext(Dispatchers.Default) { PrivateAnalyticsProtocol.speech(raw, range) }
                                        if (attempt == generation && routeCurrent(route, false)) {
                                            entry = entry.copy(speech = speech, speechAt = System.currentTimeMillis() / 1000.0, speechCurrent = true)
                                            speechRead = true
                                        }
                                    } catch (cancelled: CancellationException) { throw cancelled }
                                    catch (_: Exception) { /* Preserve last known separately, without echoing server errors. */ }
                                }
                                if (attempt == generation) value = value.copy(computers = value.computers.toMutableList().also { it[index] = entry })
                            }
                        }
                    } }
                }
            } finally {
                if (attempt == generation) value = value.copy(loading = false)
            }
        }
    }
}

/** A separate diagnostic schema, built from fixed local fields; never serializes a server reply or label. */
internal object PrivateAnalyticsDiagnostics {
    fun preview(state: PrivateAnalyticsState, versionCode: Int, now: Double): String {
        require(state.computers.size <= PrivateAnalyticsProtocol.maxMachines)
        return JSONObject().put("schema", 1).put("component", "android_private_analytics").put("android_build_code", versionCode)
            .put("from", state.range.from.toString()).put("through", state.range.through.toString())
            .put("omitted_computers", state.omitted).put("computers", JSONArray(state.computers.map { computer ->
                JSONObject().put("read_state", computer.state.name.lowercase()).put("has_agent_summary", computer.snapshot != null)
                    .put("has_speech_summary", computer.speech != null).put("speech_current", computer.speechCurrent).also { row ->
                        computer.snapshot?.let { snapshot ->
                            row.put("schema", 1).put("collection_enabled", snapshot.enabled)
                                .put("stale", AnalyticsFleet.stale(snapshot, now) || computer.state != AnalyticsReadState.Current)
                                .put("partial", snapshot.coverage.partial).put("bucket_count", snapshot.buckets.size)
                                .put("gaps", JSONArray(snapshot.coverage.gaps.map { it.takeIf { code -> code in PrivateAnalyticsProtocol.gapCodes } ?: "unknown" }.sorted()))
                                .put("unsupported", JSONArray(snapshot.coverage.unsupported.map { it.takeIf { code -> code in PrivateAnalyticsProtocol.measures } ?: "unknown" }.sorted()))
                        }
                    }
            })).toString(2)
    }
}
