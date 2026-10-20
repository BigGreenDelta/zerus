package app.zerus.mobile

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.util.LinkedHashMap
import java.util.UUID

/** epoch must be an immutable full Connection plus native machine identity snapshot. */
internal data class GitStatusAccess(val epoch: Any, val online: Boolean, val relaySupported: Boolean, val computerSupported: Boolean,
    val routeKnown: Boolean = true) {
    val available: Boolean get() = online && relaySupported && computerSupported && routeKnown
}

/**
 * Main-confined, process-local reads. Only actual viewport registrations and a resumed, focused
 * session list enable transport. Canceled calls keep their slot until completion: an uncooperative
 * transport cannot cause more than two outstanding calls or publish a late result.
 */
internal class GitStatusController(private val scope: CoroutineScope, private val nowNanos: () -> Long,
    private val access: (MachineKey) -> GitStatusAccess?,
    private val start: suspend (GitStatusRequest) -> JSONObject,
    private val receipt: suspend (GitStatusRequest) -> JSONObject,
    private val pause: suspend (Long) -> Unit = { delay(it) }) {
    companion object {
        const val QUEUE_LIMIT = 64
        const val IN_FLIGHT_LIMIT = 2
        const val CACHE_LIMIT = 128
        const val CADENCE_NANOS = 15_000_000_000L
        private const val POLLS = 35
    }
    private data class Watch(val token: Long, val candidate: GitStatusCandidate)
    private data class Slot(val candidate: GitStatusCandidate, var generation: Long, var access: GitStatusAccess? = null)
    private data class Original(val request: GitStatusRequest, val access: GitStatusAccess, val foreground: Long)
    private data class Observation(val snapshot: GitStatusSnapshot, val requestId: String)
    private data class Cached(val observation: Observation?)
    private class Flight(val original: Original) { var retired = false; var job: Job? = null }
    internal data class Counts(val watched: Int, val queued: Int, val inFlight: Int, val cached: Int, val origins: Int)
    private val watches = LinkedHashMap<Long, Watch>()
    private val slots = LinkedHashMap<GitStatusCandidate, Slot>()
    private val queue = ArrayDeque<GitStatusCandidate>()
    private val flights = LinkedHashMap<String, Flight>()
    private val cache = LinkedHashMap<GitStatusCandidate, Cached>(16, .75f, true)
    // Cadence follows host + original path across checkout evidence changes. This separate
    // bounded LRU stores at most 128 timestamps; it never supplies an observation's origin.
    private val startClocks = LinkedHashMap<GitStatusKey, Long>(16, .75f, true)
    private val origins = LinkedHashMap<String, Original>()
    private val mutableRevision = MutableStateFlow(0L)
    val revision: StateFlow<Long> = mutableRevision.asStateFlow()
    private var foreground = false
    private var foregroundGeneration = 0L
    private var sequence = 0L
    private var timer: Job? = null
    private var timerGeneration = 0L
    private var reconciling = false
    private var again = false
    val counts: Counts get() = Counts(watches.size, queue.size, flights.size, cache.size, origins.size)

    inner class Lease internal constructor(private val token: Long) {
        private var closed = false
        fun close() {
            if (closed) return
            closed = true
            watches.remove(token)
            reconcile()
        }
    }

    fun watch(candidate: GitStatusCandidate, visible: Boolean): Lease? {
        if (!visible || !GitStatus.validKey(candidate.key) || candidate.evidence.metadataState == "not_repo" || watches.size >= QUEUE_LIMIT) return null
        val token = ++sequence
        watches[token] = Watch(token, candidate)
        reconcile()
        return Lease(token)
    }

    fun setForeground(value: Boolean) {
        if (foreground == value) return
        foreground = value
        foregroundGeneration++
        if (!value) {
            cancelTimer(); queue.clear()
            flights.values.toList().forEach(::retire)
            slots.values.forEach { it.generation = ++sequence }
        }
        reconcile()
    }

    /** Reevaluate capability/connection changes; it creates no request identity by itself. */
    fun refresh() = reconcile()
    private fun currentAccess(candidate: GitStatusCandidate) = runCatching { access(candidate.key.machine) }.getOrNull()
    private fun publish() { mutableRevision.value++ }
    private fun cancelTimer() { timerGeneration++; timer?.cancel(); timer = null }
    private fun retire(flight: Flight) { flight.retired = true; flight.job?.cancel() }
    private fun referenced(id: String) = flights.containsKey(id) || cache.values.any { it.observation?.requestId == id }
    private fun forgetOrigin(id: String) { if (!referenced(id)) origins.remove(id) }
    private fun remember(candidate: GitStatusCandidate, observation: Observation?) {
        val previous = cache.put(candidate, Cached(observation))
        if (previous?.observation?.requestId != observation?.requestId) previous?.observation?.requestId?.let(::forgetOrigin)
        while (cache.size > CACHE_LIMIT) {
            val key = cache.entries.first().key
            cache.remove(key)?.observation?.requestId?.let(::forgetOrigin)
        }
    }
    private fun due(key: GitStatusKey, at: Long) = startClocks[key]?.let { at >= it && at - it >= CADENCE_NANOS } ?: true
    private fun pending(key: GitStatusKey) = flights.values.any { !it.retired && it.original.request.key == key }
    private fun rememberStart(key: GitStatusKey, started: Long) {
        startClocks[key] = started
        while (startClocks.size > CACHE_LIMIT) startClocks.remove(startClocks.entries.first().key)
    }
    private fun release(flight: Flight) {
        val id = flight.original.request.requestId
        if (flights[id] !== flight) return
        flights.remove(id)
        forgetOrigin(id)
        reconcile()
    }

    fun badge(candidate: GitStatusCandidate): GitStatusBadge? {
        val cached = cache[candidate]?.observation ?: return null
        val observed = origins[cached.requestId]
        val slot = slots[candidate]
        val current = currentAccess(candidate)
        val proven = observed?.takeIf { foreground && slot != null && it.foreground == foregroundGeneration &&
            it.request.generation == slot.generation && current?.available == true && it.access == current }
        return GitStatus.present(cached.snapshot, proven?.let { GitStatusOrigin(it.request.requestId, it.request.originalStartNanos) },
            candidate, current?.online == true, nowNanos())
    }

    private fun reconcile() {
        if (reconciling) { again = true; return }
        reconciling = true
        try {
            do { again = false; reconcileOnce() } while (again)
        } finally { reconciling = false }
    }

    private fun reconcileOnce() {
        cancelTimer()
        val visible = watches.values.mapTo(linkedSetOf()) { it.candidate }
        for (candidate in slots.keys.toList()) if (candidate !in visible) {
            slots.remove(candidate); queue.remove(candidate)
            flights.values.filter { it.original.request.candidate() == candidate }.toList().forEach(::retire)
        }
        for (candidate in visible) slots.getOrPut(candidate) { Slot(candidate, ++sequence) }
        if (!foreground) { queue.clear(); publish(); return }
        val at = nowNanos()
        for ((candidate, slot) in slots) {
            val current = currentAccess(candidate)
            if (current != slot.access) {
                slot.access = current; slot.generation = ++sequence
                flights.values.filter { it.original.request.candidate() == candidate }.toList().forEach(::retire)
            }
            if (current?.available != true) { queue.remove(candidate); continue }
            if (!pending(candidate.key) && due(candidate.key, at) && candidate !in queue && queue.size < QUEUE_LIMIT) queue.addLast(candidate)
        }
        while (flights.size < IN_FLIGHT_LIMIT && queue.isNotEmpty()) {
            val candidate = queue.removeFirst()
            val slot = slots[candidate] ?: continue
            val current = currentAccess(candidate)?.takeIf { it.available && it == slot.access } ?: continue
            val started = nowNanos().takeIf { it > 0 } ?: continue
            // A preceding queued candidate may have just started this same host/path with
            // different checkout evidence. Discard it here; the shared clock schedules it later.
            if (pending(candidate.key) || !due(candidate.key, started)) continue
            val request = GitStatusRequest(UUID.randomUUID().toString(), candidate.key, slot.generation, started, candidate.evidence)
            rememberStart(candidate.key, started)
            // Preserve cadence even if the card disappears before any receipt. Never assign
            // this new clock to an old observation: that retains its own request-ID origin.
            remember(candidate, cache[candidate]?.observation)
            val original = Original(request, current, foregroundGeneration)
            val flight = Flight(original)
            origins[request.requestId] = original; flights[request.requestId] = flight
            flight.job = scope.launch(start = CoroutineStart.LAZY) { run(flight) }
            // A LAZY job canceled before entry never executes run.finally. Completion runs
            // only once actual execution also ends, retaining uncooperative calls' slots.
            flight.job!!.invokeOnCompletion { release(flight) }
            flight.job!!.start()
            // Conflicting visible metadata for one path still needs each candidate's own
            // proof. Rotate only served slots so the next cadence gives its sibling a turn.
            if (slots[candidate] === slot) { slots.remove(candidate); slots[candidate] = slot }
        }
        scheduleTimer(at)
        publish()
    }

    private fun GitStatusRequest.candidate() = GitStatusCandidate(key, evidence)
    private fun valid(flight: Flight): Boolean {
        val original = flight.original; val request = original.request; val slot = slots[request.candidate()]
        return foreground && !flight.retired && flights[request.requestId] === flight &&
            original.foreground == foregroundGeneration && slot?.generation == request.generation &&
            currentAccess(request.candidate()) == original.access && original.access.available
    }

    private suspend fun run(flight: Flight) {
        val request = flight.original.request
        var accepted: GitStatusSnapshot? = null
        try {
            if (!valid(flight)) return
            var response = start(request)
            var polls = 0
            while (valid(flight)) {
                if (response.opt("request_id") as? String != request.requestId) return
                when (response.opt("state") as? String) {
                    "completed" -> {
                        accepted = (response.opt("result") as? JSONObject)?.let { GitStatus.parse(it, request) }
                        return
                    }
                    "queued", "claimed" -> {
                        if (polls++ >= POLLS) return
                        pause(800)
                        if (!valid(flight)) return
                        response = receipt(request)
                    }
                    else -> return
                }
            }
        } catch (error: CancellationException) { throw error }
        catch (_: Exception) { /* The previous observation keeps its original age. */ }
        finally {
            if (valid(flight)) {
                val previous = cache[request.candidate()]
                val observation = accepted?.let { Observation(it, request.requestId) } ?: previous?.observation
                remember(request.candidate(), observation)
            }
            release(flight)
        }
    }

    private fun scheduleTimer(at: Long) {
        if (!foreground) return
        var next: Long? = null
        fun consider(deadline: Long) { if (deadline > at && (next == null || deadline < next!!)) next = deadline }
        for ((candidate, slot) in slots) {
            if (currentAccess(candidate)?.available != true) continue
            startClocks[candidate.key]?.takeIf { it <= Long.MAX_VALUE - CADENCE_NANOS }?.let { consider(it + CADENCE_NANOS) }
            val observation = cache[candidate]?.observation ?: continue
            val original = origins[observation.requestId] ?: continue
            if (original.foreground != foregroundGeneration || original.request.generation != slot.generation || original.access != currentAccess(candidate)) continue
            val start = original.request.originalStartNanos
            if (start <= Long.MAX_VALUE - 30_000_000_000L) consider(start + 30_000_000_000L)
            observation.snapshot.remoteAgeSeconds?.takeIf { it < 60.0 }?.let { age ->
                val remaining = ((60.0 - age) * 1_000_000_000.0).toLong()
                if (start <= Long.MAX_VALUE - remaining) consider(start + remaining)
            }
        }
        val deadline = next ?: return
        val expectedTimer = timerGeneration
        timer = scope.launch(start = CoroutineStart.LAZY) {
            pause(((deadline - at + 999_999) / 1_000_000).coerceAtLeast(1))
            if (expectedTimer != timerGeneration) return@launch
            timer = null
            reconcile()
        }.also { it.start() }
    }
}
