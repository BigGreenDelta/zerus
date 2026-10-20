package app.zerus.mobile

import app.zerus.mobile.replica.TaskStore
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap

/**
 * The phone's task replicas (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md,
 * "Phone"): one folder per relay connection and swarm below [root], so two relays never share
 * documents. The documents are not encrypted further; [root] lies in `noBackupFilesDir`.
 */
class TaskReplicas(private val root: File) {
    /** The project's tasks; close it when done. */
    fun store(key: ProjectKey, name: String) = TaskStore(folder(key).path, key.id, name)
    fun folder(key: ProjectKey) = File(File(root, hex(key.connectionId)), hex(key.swarmId))
    /** Drops every replica the connection holds. */
    fun purge(connectionId: String) { File(root, hex(connectionId)).deleteRecursively() }
    private fun hex(value: String) = MessageDigest.getInstance("SHA-256").digest(value.toByteArray())
        .joinToString("") { "%02x".format(it) }
}

/** A computer of a project that can exchange its tasks now, and the digest its heartbeat names. */
data class TaskPeer(val computerId: String, val digest: String)

/** How a Send now or Cancel ended on the session's computer. */
sealed interface TaskOutcome {
    data object Done : TaskOutcome
    /** Nothing happened: hgs's reason, or why the phone's edits could not reach that computer. */
    data class Refused(val reason: String) : TaskOutcome
    /** It may have happened: the next exchange shows the task's stage. */
    data class Uncertain(val reason: String) : TaskOutcome
}

/** Where the next Done page starts. */
data class DoneCursor(val at: Long, val uuid: String)
data class DoneRow(val uuid: String, val id: String, val text: String, val session: app.zerus.mobile.replica.SessionRef?, val acceptedAt: Long)
/** One page of a computer's Done list, newest first; `recent` counts the last 7 days. */
data class DonePage(val rows: List<DoneRow>, val next: DoneCursor?, val total: Int, val recent: Int, val damaged: Int)

object TaskSyncPlan {
    const val OPERATION = "task_exchange"

    /** Each catalogued project's computers that are online, not last known, and offer the exchange, as their relay does. */
    fun peers(digests: Map<ProjectKey, Map<String, String>>, relayOperations: Map<String, Set<String>>,
              machineOperations: Map<MachineKey, Set<String>>, machines: List<Machine>): Map<ProjectKey, List<TaskPeer>> =
        digests.mapNotNull { (key, byComputer) ->
            if (OPERATION !in relayOperations[key.connectionId].orEmpty()) return@mapNotNull null
            val peers = byComputer.filter { (computer, _) ->
                OPERATION in machineOperations[MachineKey(key.connectionId, computer)].orEmpty() &&
                    machines.any { it.connectionId == key.connectionId && it.id == computer && it.online && !it.lastKnown }
            }.map { (computer, digest) -> TaskPeer(computer, digest) }.sortedBy { it.computerId }
            if (peers.isEmpty()) null else key to peers
        }.toMap()
}

/**
 * Exchanges a project's tasks with its computers through the relay: one project at a time
 * under its lock, one computer after another, and a computer whose exchange failed waits a
 * minute. After an exchange, the phone and the computer stay quiet until either digest
 * changes: a heartbeat may still name the digest from before, and some projects cannot
 * meet (a document the computer refuses, or one too large to send). The Rust exchange
 * decides every request; this only carries its JSON.
 */
class TaskSync(private val api: () -> RelayApi, private val replicas: TaskReplicas,
               private val exchangeTimeoutMs: Long = EXCHANGE_MS,
               private val clock: () -> Long = System::currentTimeMillis) {
    private val locks = ConcurrentHashMap<ProjectKey, Mutex>()
    private val waiting = ConcurrentHashMap<Pair<ProjectKey, String>, Long>()
    /** After an exchange: the phone's digest, the heartbeat's it answered, and the computer's after it. */
    private val quiet = ConcurrentHashMap<Pair<ProjectKey, String>, Triple<String, String, String>>()
    /** Tasks whose Send now may have happened, with their project and computer. */
    private val sends = ConcurrentHashMap<String, Pair<ProjectKey, String>>()

    /** Exchanges `key`'s tasks with each of `peers` whose digest is not the phone's; the failures by computer. */
    suspend fun sync(connection: Connection, key: ProjectKey, name: String, peers: List<TaskPeer>): Map<String, String> =
        locks.getOrPut(key) { Mutex() }.withLock {
            val failures = linkedMapOf<String, String>()
            val store = withContext(Dispatchers.IO) { replicas.store(key, name) }
            try {
                for (peer in peers.sortedBy { it.computerId }) {
                    val wait = key to peer.computerId
                    if (clock() < (waiting[wait] ?: 0L)) continue
                    try {
                        val mine = withContext(Dispatchers.IO) { store.liveDigest() }
                        if (mine == peer.digest) continue
                        val known = quiet[wait]
                        if (known != null && known.first == mine && peer.digest in setOf(known.second, known.third)) continue
                        val theirs = bounded { exchange(connection, store, peer.computerId) }
                        quiet[wait] = Triple(withContext(Dispatchers.IO) { store.liveDigest() }, peer.digest, theirs)
                        waiting.remove(wait)
                    } catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        waiting[wait] = clock() + RETRY_MS
                        failures[peer.computerId] = e.message.orEmpty().ifBlank { "The tasks could not be exchanged." }
                    }
                }
            } finally { store.close() }
            failures
        }

    /** One exchange with this computer now, whatever the digests: the phone's edits reach it, and `fetch` brings tasks it does not hold. */
    suspend fun exchangeNow(connection: Connection, key: ProjectKey, name: String, computerId: String, fetch: List<String> = emptyList()) {
        project(key, name) { store -> bounded { exchange(connection, store, computerId, fetch) } }
    }

    /** Send now: the phone's edits reach the session's computer first, then it sends, then its copy comes back. */
    suspend fun send(connection: Connection, key: ProjectKey, name: String, computerId: String, task: String, session: String,
                     computerName: String = computerId): TaskOutcome =
        act(connection, key, name, computerId, computerName, "task_send",
            JSONObject().put("schema", 1).put("project", key.id).put("task", task).put("session", session))
            .also { if (it is TaskOutcome.Uncertain) sends[task] = key to computerId }

    /** A Send now that may have happened holds the task until an exchange with that computer shows its stage. */
    fun held(task: String) = sends.containsKey(task)
    fun heldTasks(): Set<String> = sends.keys.toSet()

    /** Exchanges once with each computer of this connection that holds a task back; one that completes releases them. */
    suspend fun settleSends(connection: Connection, names: (ProjectKey) -> String) {
        for (pending in sends.values.filter { it.first.connectionId == connection.id }.toSet()) {
            try {
                exchangeNow(connection, pending.first, names(pending.first), pending.second)
                sends.values.removeAll { it == pending }
            } catch (e: CancellationException) { throw e }
            catch (_: Exception) { /* The next sync tries again. */ }
        }
    }

    suspend fun cancel(connection: Connection, key: ProjectKey, name: String, computerId: String, task: String, reason: String,
                       computerName: String = computerId): TaskOutcome =
        act(connection, key, name, computerId, computerName, "task_cancel", JSONObject().put("schema", 1).put("project", key.id).put("task", task)
            .also { if (reason.isNotBlank()) it.put("reason", reason) })

    /** One page of that computer's Done list; with `session`, of the tasks sent to it. */
    suspend fun donePage(connection: Connection, key: ProjectKey, computerId: String, before: DoneCursor?, limit: Int = 50,
                         session: app.zerus.mobile.replica.SessionRef? = null): DonePage {
        val payload = JSONObject().put("schema", 1).put("project", key.id).put("limit", limit)
        before?.let { payload.put("before", JSONObject().put("at", it.at).put("uuid", it.uuid)) }
        session?.let { payload.put("session", JSONObject().put("machine", it.machine).put("session", it.session)) }
        val receipt = request(connection, computerId, "task_done", payload)
        check(receipt.string("state") == "completed") { receipt.string("error").ifBlank { "The computer did not list its Done tasks." } }
        val result = receipt.optJSONObject("result") ?: error("The computer's Done list has no result.")
        val rows = result.optJSONArray("tasks")?.objects().orEmpty().map { row ->
            val session = row.optJSONObject("session")?.let { app.zerus.mobile.replica.SessionRef(it.string("machine"), it.string("session")) }
            DoneRow(row.string("uuid"), row.string("id"), row.string("text"), session, row.optLong("accepted_at"))
        }
        val next = result.optJSONObject("next")?.let { DoneCursor(it.optLong("at"), it.string("uuid")) }
        return DonePage(rows, next, result.optInt("total"), result.optInt("recent"), result.optInt("damaged"))
    }

    /** The exchange, then `operation` on that computer, then the exchange that shows its result. */
    private suspend fun act(connection: Connection, key: ProjectKey, name: String, computerId: String, computerName: String,
                            operation: String, payload: JSONObject): TaskOutcome = project(key, name) { store ->
        try { bounded { exchange(connection, store, computerId) } }
        catch (e: CancellationException) { throw e }
        catch (e: Exception) { return@project TaskOutcome.Refused("The tasks could not reach $computerName: ${e.message.orEmpty()}") }
        val outcome = try {
            val receipt = request(connection, computerId, operation, payload, maxPolls = 150)
            val reason = receipt.string("error")
            when (receipt.string("state")) {
                "completed" -> TaskOutcome.Done
                "failed" -> TaskOutcome.Refused(reason.ifBlank { "The computer refused it." })
                "uncertain" -> TaskOutcome.Uncertain(reason.ifBlank { "The computer could not confirm it." })
                else -> TaskOutcome.Uncertain("The computer has not answered yet.")
            }
        } catch (e: CancellationException) { throw e }
        // The relay refused the request itself: nothing was queued.
        catch (e: RelayException) {
            if (e.status in 400..499 && e.status != 408) TaskOutcome.Refused(e.message.orEmpty().ifBlank { "The relay refused it." })
            else TaskOutcome.Uncertain("The relay did not confirm it: ${e.message.orEmpty()}")
        }
        // The request may be queued although its answer was lost.
        catch (e: Exception) { TaskOutcome.Uncertain("The relay did not confirm it: ${e.message.orEmpty().ifBlank { e.javaClass.simpleName }}") }
        // Its copy comes back now; when that fails the next sync brings it.
        try { bounded { exchange(connection, store, computerId) } }
        catch (e: CancellationException) { throw e }
        catch (_: Exception) { }
        outcome
    }

    private suspend fun <T> project(key: ProjectKey, name: String, block: suspend (TaskStore) -> T): T =
        locks.getOrPut(key) { Mutex() }.withLock {
            val store = withContext(Dispatchers.IO) { replicas.store(key, name) }
            try { block(store) } finally { store.close() }
        }

    /** Besides its request budget, an exchange ends after its time. */
    private suspend fun <T> bounded(block: suspend () -> T): T = withTimeoutOrNull(exchangeTimeoutMs) { block() }
        ?: error("The task exchange took longer than ${exchangeTimeoutMs / 1000} seconds.")

    private suspend fun request(connection: Connection, computerId: String, operation: String, payload: JSONObject, maxPolls: Int = 80): JSONObject {
        val relay = api()
        val target = Target(computerId, "", "", "", connection.id)
        return relay.await(connection, relay.submit(connection, target, operation, payload, UUID.randomUUID().toString()),
            maxPolls = maxPolls, pollDelayMs = 400)
    }

    /** Drops the connection's replicas once its running exchanges end, so none writes them again. */
    suspend fun purge(connectionId: String) {
        val held = locks.filterKeys { it.connectionId == connectionId }.toSortedMap(compareBy { it.key }).values.toList()
        held.forEach { it.lock() }
        try { withContext(Dispatchers.IO) { replicas.purge(connectionId) } }
        finally { held.forEach { it.unlock() } }
    }

    /** One whole exchange with the computer; its live digest after the last answer. */
    private suspend fun exchange(connection: Connection, store: TaskStore, computerId: String, fetch: List<String> = emptyList()): String {
        val exchange = withContext(Dispatchers.IO) { if (fetch.isEmpty()) store.exchange() else store.exchangeFetching(fetch) }
        try {
            var digest = ""
            while (true) {
                val payload = withContext(Dispatchers.IO) { exchange.nextRequest() } ?: return digest
                val receipt = request(connection, computerId, TaskSyncPlan.OPERATION, JSONObject(payload))
                check(receipt.string("state") == "completed") {
                    receipt.string("error").ifBlank { "The computer did not answer the task exchange." }
                }
                val result = receipt.optJSONObject("result") ?: error("The computer's answer has no result.")
                digest = withContext(Dispatchers.IO) { exchange.accept(result.toString()) }.digest
            }
        } finally { exchange.close() }
    }

    companion object {
        const val RETRY_MS = 60_000L
        const val EXCHANGE_MS = 5 * 60_000L
    }
}
