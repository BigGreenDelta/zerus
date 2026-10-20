package app.zerus.mobile

import app.zerus.mobile.replica.TaskStore
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
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
 * minute. The Rust exchange decides every request; this only carries its JSON.
 */
class TaskSync(private val api: () -> RelayApi, private val replicas: TaskReplicas,
               private val clock: () -> Long = System::currentTimeMillis) {
    private val locks = ConcurrentHashMap<ProjectKey, Mutex>()
    private val waiting = ConcurrentHashMap<Pair<ProjectKey, String>, Long>()

    /** Exchanges `key`'s tasks with each of `peers` whose digest is not the phone's; the failures by computer. */
    suspend fun sync(connection: Connection, key: ProjectKey, name: String, peers: List<TaskPeer>): Map<String, String> =
        locks.getOrPut(key) { Mutex() }.withLock {
            val failures = linkedMapOf<String, String>()
            val store = withContext(Dispatchers.IO) { replicas.store(key, name) }
            try {
                for (peer in peers.sortedBy { it.computerId }) {
                    val wait = key to peer.computerId
                    if (clock() < (waiting[wait] ?: 0L)) continue
                    if (withContext(Dispatchers.IO) { store.liveDigest() } == peer.digest) continue
                    try {
                        exchange(connection, store, peer.computerId)
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

    private suspend fun exchange(connection: Connection, store: TaskStore, computerId: String) {
        val exchange = withContext(Dispatchers.IO) { store.exchange() }
        try {
            val target = Target(computerId, "", "", "", connection.id)
            while (true) {
                val payload = withContext(Dispatchers.IO) { exchange.nextRequest() } ?: return
                val relay = api()
                val id = UUID.randomUUID().toString()
                val receipt = relay.await(connection, relay.submit(connection, target, TaskSyncPlan.OPERATION, JSONObject(payload), id),
                    maxPolls = 80, pollDelayMs = 400)
                check(receipt.string("state") == "completed") {
                    receipt.string("error").ifBlank { "The computer did not answer the task exchange." }
                }
                val result = receipt.optJSONObject("result") ?: error("The computer's answer has no result.")
                withContext(Dispatchers.IO) { exchange.accept(result.toString()) }
            }
        } finally { exchange.close() }
    }

    companion object { const val RETRY_MS = 60_000L }
}
