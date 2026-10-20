package app.zerus.mobile

import app.zerus.mobile.replica.SessionRef
import app.zerus.mobile.replica.TaskPosition
import app.zerus.mobile.replica.TaskStage
import app.zerus.mobile.replica.TaskStore
import app.zerus.mobile.replica.TaskTarget
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import okhttp3.mockwebserver.SocketPolicy
import org.junit.Assert.assertFalse
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The phone's Send now, Cancel, Done pages and fetches through a relay whose computer answers from its replica. */
class TaskActionsTest {
    @get:Rule val folders = TemporaryFolder()
    private val key = ProjectKey("workspace-a", "swarm-1", "project-1")

    private class Computer(val store: TaskStore) : Dispatcher() {
        val envelopes = mutableListOf<JSONObject>()
        /** Receipts for the operations other than the exchange: state, result, error. */
        val receipts = mutableMapOf<String, (JSONObject) -> Triple<String, Any, Any>>()
        /** Whole responses for an operation instead of its receipt: a lost answer, a relay refusal. */
        val raw = mutableMapOf<String, () -> MockResponse>()
        @Volatile var exchangeFails = false
        override fun dispatch(request: RecordedRequest): MockResponse {
            val envelope = JSONObject(request.body.readUtf8())
            synchronized(this) { envelopes += envelope }
            val id = envelope.getString("request_id")
            val operation = envelope.getString("operation")
            raw[operation]?.let { return it() }
            val payload = envelope.getJSONObject("payload")
            val (state, result, error) = when {
                operation == "task_exchange" && exchangeFails -> Triple("failed", JSONObject.NULL, "the project's tasks are busy; try again")
                operation == "task_exchange" -> Triple("completed", JSONObject(store.answerExchange(payload.toString())), JSONObject.NULL)
                else -> receipts.getValue(operation)(payload)
            }
            return MockResponse().setBody(JSONObject().put("request_id", id).put("state", state).put("result", result)
                .put("error", error).toString())
        }
        fun operations() = synchronized(this) { envelopes.map { it.getString("operation") } }
    }

    private fun world(block: suspend (Computer, Connection, TaskReplicas, TaskSync) -> Unit) = runBlocking {
        MockWebServer().use { server ->
            val computer = Computer(TaskStore(folders.newFolder().path, key.id, "orchard"))
            server.dispatcher = computer
            server.start()
            val connection = Connection(key.connectionId, "Workspace", server.url("/").toString().trimEnd('/'), "token")
            val replicas = TaskReplicas(folders.newFolder())
            block(computer, connection, replicas, TaskSync({ RelayApi() }, replicas))
        }
    }

    private fun added(store: TaskStore, text: String) = store.add(text, TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)

    @Test fun aSendFirstBringsThePhonesEdits() = world { computer, connection, replicas, sync ->
        val task = added(computer.store, "Add rate limiting")
        sync.exchangeNow(connection, key, "orchard", "computer-1")
        replicas.store(key, "orchard").use { it.editText(task.uuid, "Add rate limiting per account") }
        var textAtSend = ""
        computer.receipts["task_send"] = { payload ->
            textAtSend = computer.store.get(payload.getString("task")).text
            Triple("completed", JSONObject().put("schema", 1).put("ok", true).put("task", task.uuid).put("stage", "active"), JSONObject.NULL)
        }
        computer.envelopes.clear()
        assertEquals(TaskOutcome.Done, sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api"))
        assertEquals("Add rate limiting per account", textAtSend)
        val operations = computer.operations()
        assertEquals("task_exchange", operations.first())
        assertEquals("task_send", operations.single { it != "task_exchange" })
        assertEquals("task_exchange", operations.last())
        val send = computer.envelopes.single { it.getString("operation") == "task_send" }
        assertEquals("", send.getString("session"))
        assertEquals("computer-1", send.getString("computer_id"))
        val payload = send.getJSONObject("payload")
        assertEquals(listOf("project-1", task.uuid, "codex/api"), listOf(payload.getString("project"), payload.getString("task"), payload.getString("session")))
    }

    @Test fun aFailedExchangeSendsNothing() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        computer.exchangeFails = true
        val outcome = sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api", computerName = "Studio")
        assertTrue(outcome is TaskOutcome.Refused)
        assertTrue((outcome as TaskOutcome.Refused).reason, outcome.reason.contains("busy"))
        assertTrue(outcome.reason, outcome.reason.startsWith("The tasks could not reach Studio"))
        assertTrue(computer.operations().none { it == "task_send" })
    }

    @Test fun anUncertainSendSaysSo() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        val unrecorded = "the prompt was sent, but the task could not record it: disk full"
        computer.receipts["task_send"] = { Triple("uncertain", JSONObject.NULL, unrecorded) }
        assertEquals(TaskOutcome.Uncertain(unrecorded), sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api"))
    }

    @Test fun aLostAnswerToTheSendIsUncertainAndHoldsTheTask() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        // The relay may hold the request although its answer never arrived.
        computer.raw["task_send"] = { MockResponse().setSocketPolicy(SocketPolicy.DISCONNECT_AFTER_REQUEST) }
        val outcome = sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api")
        assertTrue(outcome.toString(), outcome is TaskOutcome.Uncertain)
        assertTrue(sync.held(task.uuid))
    }

    @Test fun aRelayRefusalBeforeTheSendIsRefused() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        computer.raw["task_send"] = { MockResponse().setResponseCode(400).setBody(JSONObject().put("error", "invalid task_send payload").toString()) }
        assertEquals(TaskOutcome.Refused("invalid task_send payload"), sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api"))
        assertFalse(sync.held(task.uuid))
    }

    @Test fun anUncertainSendHoldsUntilAnExchangeWithThatComputerEnds() = world { computer, connection, replicas, sync ->
        val task = added(computer.store, "Add rate limiting")
        computer.receipts["task_send"] = {
            // Its copy cannot come back now either.
            computer.exchangeFails = true
            Triple("uncertain", JSONObject.NULL, "delivery uncertain")
        }
        assertEquals(TaskOutcome.Uncertain("delivery uncertain"), sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api"))
        assertTrue(sync.held(task.uuid))
        // A sync that exchanges nothing shows no stage: the heartbeat names the phone's own digest.
        val mine = replicas.store(key, "orchard").use { it.liveDigest() }
        assertTrue(sync.sync(connection, key, "orchard", listOf(TaskPeer("computer-1", mine))).isEmpty())
        assertTrue(sync.held(task.uuid))
        sync.settleSends(connection) { "orchard" }
        assertTrue(sync.held(task.uuid))
        computer.exchangeFails = false
        sync.settleSends(connection) { "orchard" }
        assertFalse(sync.held(task.uuid))
    }

    @Test fun aRefusedSendGivesHgsReason() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        computer.receipts["task_send"] = { Triple("failed", JSONObject.NULL, "codex/api cannot take it now: a question is open") }
        assertEquals(TaskOutcome.Refused("codex/api cannot take it now: a question is open"),
            sync.send(connection, key, "orchard", "computer-1", task.uuid, "codex/api"))
    }

    @Test fun aCancelCarriesItsReason() = world { computer, connection, _, sync ->
        val task = added(computer.store, "Add rate limiting")
        var reason = ""
        computer.receipts["task_cancel"] = { payload ->
            reason = payload.optString("reason")
            Triple("completed", JSONObject().put("schema", 1).put("ok", true).put("stage", "review"), JSONObject.NULL)
        }
        assertEquals(TaskOutcome.Done, sync.cancel(connection, key, "orchard", "computer-1", task.uuid, "Wrong approach"))
        assertEquals("Wrong approach", reason)
    }

    @Test fun donePagesParseAndContinue() = world { computer, connection, _, sync ->
        val row = { n: Int -> JSONObject().put("uuid", "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e1$n").put("id", "T-aaaaa$n")
            .put("text", "Task $n").put("session", JSONObject().put("machine", "m").put("session", "codex/api")).put("accepted_at", 100L - n) }
        computer.receipts["task_done"] = { payload ->
            val first = !payload.has("before")
            val rows = if (first) JSONArray(listOf(row(1), row(2))) else JSONArray(listOf(row(3)))
            val next = if (first) JSONObject().put("at", 98).put("uuid", row(2).getString("uuid")) else JSONObject.NULL
            Triple("completed", JSONObject().put("schema", 1).put("tasks", rows).put("next", next).put("total", 3)
                .put("recent", 1).put("damaged", 0), JSONObject.NULL)
        }
        val first = sync.donePage(connection, key, "computer-1", null)
        assertEquals(listOf("Task 1", "Task 2"), first.rows.map { it.text })
        assertEquals(Triple(3, 1, 0), Triple(first.total, first.recent, first.damaged))
        assertEquals("codex/api", first.rows.first().session?.session)
        val rest = sync.donePage(connection, key, "computer-1", first.next)
        assertEquals(listOf("Task 3"), rest.rows.map { it.text })
        assertEquals(null, rest.next)
        val asked = computer.envelopes.last().getJSONObject("payload")
        assertEquals(98L, asked.getJSONObject("before").getLong("at"))
        assertEquals(50, asked.getInt("limit"))
    }

    @Test fun openingADoneTaskFetchesIt() = world { computer, connection, replicas, sync ->
        // Only hgs accepts a task into Done; the crate and bindings tests fetch a real one.
        val done = added(computer.store, "Document the limits")
        sync.exchangeNow(connection, key, "orchard", "computer-1", fetch = listOf(done.uuid))
        // The phone held nothing: its first request asks for that task by name.
        val first = computer.envelopes.first { it.getString("operation") == "task_exchange" }
        assertTrue(first.getJSONObject("payload").getJSONObject("inventory").getJSONObject("tasks").has(done.uuid))
        replicas.store(key, "orchard").use { assertEquals("Document the limits", it.get(done.uuid).text) }
    }

    @Test fun aDonePageForOneSessionNamesIt() = world { computer, connection, _, sync ->
        computer.receipts["task_done"] = {
            Triple("completed", JSONObject().put("schema", 1).put("tasks", JSONArray()).put("next", JSONObject.NULL)
                .put("total", 0).put("recent", 0).put("damaged", 0), JSONObject.NULL)
        }
        sync.donePage(connection, key, "computer-1", null, session = SessionRef("node-1", "codex/api"))
        val asked = computer.envelopes.last().getJSONObject("payload").getJSONObject("session")
        assertEquals(listOf("node-1", "codex/api"), listOf(asked.getString("machine"), asked.getString("session")))
    }

    @Test fun editsReachTheComputer() = world { computer, connection, replicas, sync ->
        val task = replicas.store(key, "orchard").use { added(it, "Filed on the phone") }
        sync.exchangeNow(connection, key, "orchard", "computer-1")
        assertEquals("Filed on the phone", computer.store.get(task.uuid).text)
    }
}
