package app.zerus.mobile

import app.zerus.mobile.replica.TaskPosition
import app.zerus.mobile.replica.TaskStage
import app.zerus.mobile.replica.TaskStore
import app.zerus.mobile.replica.TaskTarget
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The phone's task sync through a relay whose one computer answers from its own replica. */
class TaskSyncTest {
    @get:Rule val folders = TemporaryFolder()
    private val key = ProjectKey("workspace-a", "swarm-1", "project-1")
    private val digest = "ab".repeat(32)

    private class Computer(val store: TaskStore) : Dispatcher() {
        val envelopes = mutableListOf<JSONObject>()
        val paths = mutableListOf<String>()
        @Volatile var refusal: String? = null
        @Volatile var delayMs = 0L
        override fun dispatch(request: RecordedRequest): MockResponse {
            Thread.sleep(delayMs)
            val envelope = JSONObject(request.body.readUtf8())
            synchronized(this) { envelopes += envelope; paths += request.path.orEmpty() }
            val id = envelope.getString("request_id")
            val refused = refusal
            val receipt = if (refused != null) {
                JSONObject().put("request_id", id).put("state", "failed").put("result", JSONObject.NULL).put("error", refused)
            } else {
                val answer = store.answerExchange(envelope.getJSONObject("payload").toString())
                JSONObject().put("request_id", id).put("state", "completed").put("result", JSONObject(answer)).put("error", JSONObject.NULL)
            }
            return MockResponse().setBody(receipt.toString())
        }
    }

    private fun world(block: suspend (Computer, Connection, TaskReplicas) -> Unit) = runBlocking {
        MockWebServer().use { server ->
            val computer = Computer(TaskStore(folders.newFolder().path, key.id, "orchard"))
            server.dispatcher = computer
            server.start()
            val connection = Connection(key.connectionId, "Workspace", server.url("/").toString().trimEnd('/'), "token")
            block(computer, connection, TaskReplicas(folders.newFolder()))
        }
    }

    private fun peer(computer: Computer) = TaskPeer("computer-1", computer.store.liveDigest())
    private fun sha256(value: String) = java.security.MessageDigest.getInstance("SHA-256").digest(value.toByteArray())
        .joinToString("") { "%02x".format(it) }
    /** Text that does not compress. */
    private fun noise(seed: String, length: Int): String {
        val out = StringBuilder()
        while (out.length < length) out.append(sha256("$seed-${out.length}"))
        return out.substring(0, length)
    }

    @Test fun aPhonePullsAComputersTasksThroughTheRelay() = world { computer, connection, replicas ->
        listOf("Add rate limiting", "Measure first", "Document the limits").forEach {
            computer.store.add(it, TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        }
        val failures = TaskSync({ RelayApi() }, replicas).sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(emptyMap<String, String>(), failures)
        replicas.store(key, "orchard").use { phone ->
            assertEquals(computer.store.liveDigest(), phone.liveDigest())
            assertEquals(3, phone.list().tasks.size)
        }
        assertTrue(computer.envelopes.isNotEmpty())
        computer.envelopes.forEach {
            assertEquals("task_exchange", it.getString("operation"))
            assertEquals("", it.getString("session"))
            assertEquals("computer-1", it.getString("computer_id"))
            assertEquals("project-1", it.getJSONObject("payload").getString("project"))
        }
        assertTrue(computer.paths.all { it == "/v1/requests" })
    }

    @Test fun thePhonesEditsReachTheComputer() = world { computer, connection, replicas ->
        val task = computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val sync = TaskSync({ RelayApi() }, replicas)
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        replicas.store(key, "orchard").use { it.comment(task.uuid, "From the phone") }
        assertEquals(emptyMap<String, String>(), sync.sync(connection, key, "orchard", listOf(peer(computer))))
        assertEquals("From the phone", computer.store.get(task.uuid).log.last { it.kind == "comment" }.text)
    }

    @Test fun aMatchingDigestSendsNothing() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val sync = TaskSync({ RelayApi() }, replicas)
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        val sent = computer.envelopes.size
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(sent, computer.envelopes.size)
    }

    @Test fun aStaleHeartbeatDigestSendsNothingAfterAnExchange() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val sync = TaskSync({ RelayApi() }, replicas)
        val heartbeat = peer(computer)
        assertEquals(emptyMap<String, String>(), sync.sync(connection, key, "orchard", listOf(heartbeat)))
        replicas.store(key, "orchard").use { it.comment(it.list().tasks.single().uuid, "From the phone") }
        sync.sync(connection, key, "orchard", listOf(heartbeat))
        val sent = computer.envelopes.size
        // The heartbeat still names the digest from before the exchange; nothing changed since.
        sync.sync(connection, key, "orchard", listOf(heartbeat))
        assertEquals(sent, computer.envelopes.size)
    }

    @Test fun anUnconvergedProjectWaitsUntilADigestChanges() = world { computer, connection, replicas ->
        val task = computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val sync = TaskSync({ RelayApi() }, replicas)
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        // More than one request can carry: the phone keeps it, and the digests stay apart.
        replicas.store(key, "orchard").use { phone ->
            repeat(10) { n -> phone.comment(task.uuid, noise("comment-$n", 60_000)) }
        }
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        replicas.store(key, "orchard").use { assertNotEquals(computer.store.liveDigest(), it.liveDigest()) }
        val sent = computer.envelopes.size
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(sent, computer.envelopes.size)
        // Something new on the computer: the phone exchanges again.
        computer.store.add("Measure first", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        sync.sync(connection, key, "orchard", listOf(peer(computer)))
        assertTrue(computer.envelopes.size > sent)
        replicas.store(key, "orchard").use { assertEquals(2, it.list().tasks.size) }
    }

    @Test fun aReplicaThatCannotBeReadFailsOnlyItsExchange() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        // A damaged project document: the phone cannot read its own digest.
        val project = java.io.File(replicas.folder(key), sha256(key.id))
        project.mkdirs()
        java.io.File(project, "project.loro").writeText("damaged")
        val failures = TaskSync({ RelayApi() }, replicas).sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(setOf("computer-1"), failures.keys)
        assertTrue(computer.envelopes.isEmpty())
    }

    @Test fun aSlowExchangeStopsAtItsDeadline() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        computer.delayMs = 1_000
        val failures = TaskSync({ RelayApi() }, replicas, exchangeTimeoutMs = 300).sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(setOf("computer-1"), failures.keys)
        assertTrue(failures.getValue("computer-1"), failures.getValue("computer-1").contains("took longer"))
    }

    @Test fun aFailedExchangeWaitsAMinute() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        var now = 0L
        val sync = TaskSync({ RelayApi() }, replicas) { now }
        computer.refusal = "the project's tasks are busy; try again"
        val failures = sync.sync(connection, key, "orchard", listOf(peer(computer)))
        assertEquals(mapOf("computer-1" to "the project's tasks are busy; try again"), failures)
        assertEquals(1, computer.envelopes.size)
        computer.refusal = null
        now = 59_000
        assertEquals(emptyMap<String, String>(), sync.sync(connection, key, "orchard", listOf(peer(computer))))
        assertEquals(1, computer.envelopes.size)
        now = 61_000
        assertEquals(emptyMap<String, String>(), sync.sync(connection, key, "orchard", listOf(peer(computer))))
        replicas.store(key, "orchard").use { assertEquals(computer.store.liveDigest(), it.liveDigest()) }
    }

    @Test fun replicasAreKeptPerConnectionAndSwarm() {
        val replicas = TaskReplicas(folders.newFolder())
        val elsewhere = key.copy(connectionId = "workspace-b")
        assertNotEquals(replicas.folder(key), replicas.folder(elsewhere))
        assertNotEquals(replicas.folder(key), replicas.folder(key.copy(swarmId = "swarm-2")))
        replicas.store(key, "orchard").use { it.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End) }
        replicas.store(elsewhere, "orchard").use { it.add("Measure first", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End) }
        replicas.purge("workspace-a")
        assertFalse(replicas.folder(key).exists())
        replicas.store(elsewhere, "orchard").use { assertEquals("Measure first", it.list().tasks.single().text) }
    }

    @Test fun purgeWaitsForARunningExchangeAndDropsItsReplica() = world { computer, connection, replicas ->
        computer.store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val sync = TaskSync({ RelayApi() }, replicas)
        computer.delayMs = 400
        coroutineScope {
            val running = async { sync.sync(connection, key, "orchard", listOf(peer(computer))) }
            delay(150)
            sync.purge(key.connectionId)
            running.await()
        }
        assertFalse(replicas.folder(key).exists())
    }

    @Test fun onlyComputersThatCanExchangeNowAreKept() {
        val digests = mapOf(key to listOf("a", "b", "c", "d").associateWith { digest })
        val machines = listOf(Machine("workspace-a", "a", "A", true), Machine("workspace-a", "b", "B", false),
            Machine("workspace-a", "c", "C", true, lastKnown = true), Machine("workspace-a", "d", "D", true))
        val exchange = setOf("task_exchange")
        val operations = mapOf(MachineKey("workspace-a", "a") to exchange, MachineKey("workspace-a", "b") to exchange,
            MachineKey("workspace-a", "c") to exchange, MachineKey("workspace-a", "d") to setOf("inspect"))
        val relay = mapOf("workspace-a" to exchange)
        assertEquals(mapOf(key to listOf(TaskPeer("a", digest))), TaskSyncPlan.peers(digests, relay, operations, machines))
        assertEquals(emptyMap<ProjectKey, List<TaskPeer>>(), TaskSyncPlan.peers(digests, mapOf("workspace-a" to setOf("inspect")), operations, machines))
        assertEquals(emptyMap<ProjectKey, List<TaskPeer>>(), TaskSyncPlan.peers(digests, emptyMap(), operations, machines))
    }
}
