package app.zerus.mobile

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.RecordedRequest
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class OvermindTasksTest {
    private val task = "aaaaaaaa-0000-4000-8000-000000000001"
    private val worker = OvermindSession("22222222-2222-4222-8222-222222222222", "codex/example/worker")
    private fun answer(payload: JSONObject): JSONObject = when (payload.getString("action")) {
        "board" -> ok().put("project", "example").put("cursor", 9).put("workflow", workflow).put("tasks", JSONArray(listOf(card(task, "Todo", "a")))).put("next", JSONObject.NULL)
        "task" -> ok().put("task", card(task, "Todo", "a").put("prompt", "Do it").put("journal", JSONArray()))
        else -> ok()
    }
    private class Submitted(val action: String, val request: JSONObject, val id: String, val settle: (OvermindWrite) -> Unit)
    private fun holder(scope: CoroutineScope, api: OvermindRepository, gateway: () -> OvermindGateway?, submitted: MutableList<Submitted> = mutableListOf()) =
        OvermindTasks(scope, api, { gateway() }, { _, action, request, _, settle ->
            UUID.randomUUID().toString().also { submitted += Submitted(action, JSONObject(request.toString()), it, settle) } }) { _, _ -> }
    private suspend fun idle(tasks: OvermindTasks) = withTimeout(10_000) {
        while (tasks.boards.values.any { it.loading } || tasks.cards.values.any { it.loading }) delay(10)
    }

    @Test fun disconnectPurgesBoardsCardsAndTheOpenWindow() = runBlocking {
        overmindServer(FakeOvermindRelay(::answer)) { _, gateway -> runBlocking {
            var connected = true
            val tasks = holder(this, OvermindRepository(RelayApi()), { gateway.takeIf { connected } })
            tasks.openBoard("workspace", "example")
            tasks.load("workspace", "example")
            tasks.openTask("workspace", task)
            tasks.loadCard("workspace", task)
            idle(tasks)
            assertEquals(listOf(task), tasks.board("workspace", "example")?.board?.cards?.map { it.uuid })
            assertEquals("Do it", tasks.card("workspace", task)?.view?.prompt)
            connected = false
            tasks.purge("workspace")
            assertTrue(tasks.boards.isEmpty()); assertTrue(tasks.cards.isEmpty()); assertNull(tasks.screen)
            // Without the workspace nothing is read again.
            tasks.load("workspace", "example")
            assertTrue(tasks.boards.isEmpty())
        } }
    }

    @Test fun aReadThatEndsAfterDisconnectIsDropped() = runBlocking {
        val release = CountDownLatch(1)
        val received = CountDownLatch(1)
        val relay = object : Dispatcher() {
            val inner = FakeOvermindRelay(::answer)
            override fun dispatch(request: RecordedRequest): MockResponse {
                received.countDown(); release.await(10, TimeUnit.SECONDS); return inner.dispatch(request)
            }
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            var connected = true
            val tasks = holder(this, OvermindRepository(RelayApi()), { gateway.takeIf { connected } })
            tasks.load("workspace", "example")
            assertTrue(tasks.board("workspace", "example")?.loading == true)
            // The read is on the wire when the workspace is disconnected.
            withTimeout(10_000) { while (received.count > 0) delay(10) }
            connected = false
            tasks.purge("workspace")
            release.countDown()
            delay(300)
            assertTrue(tasks.boards.isEmpty())
        } }
    }

    private fun refused(write: Submitted, code: String) = OvermindWrites.create(write.id, MachineKey("workspace", GATEWAY), write.action, write.request,
        emptyList(), "Pixel", 1).copy(status = "refused", code = code, field = "confirm", error = "$code refused")

    @Test fun anywayIsAnExplicitNewWriteWithTheCodeConfirmed() = runBlocking {
        val gateway = OvermindGateway(Connection("workspace", "Example", "https://relay.example", "token"), GATEWAY)
        val submitted = mutableListOf<Submitted>()
        val tasks = holder(this, OvermindRepository(RelayApi()), { gateway }, submitted)
        tasks.write("workspace", "send", OvermindRequests.send(task, worker), OvermindWriteContext("om-1", worker.name, "Sent om-1."))
        submitted.single().settle(refused(submitted.single(), "agent_busy"))
        val offer = checkNotNull(tasks.outcome?.anyway)
        // Nothing is repeated on its own.
        assertEquals(1, submitted.size)
        tasks.anyway("workspace", offer)
        assertEquals(2, submitted.size)
        val again = submitted.last()
        assertNotEquals(submitted.first().id, again.id)
        assertEquals("send", again.action)
        assertEquals(JSONArray(listOf("agent_busy")).toString(), again.request.getJSONArray("confirm").toString())
        assertEquals(task, again.request.getString("task"))
        // A second confirmable refusal keeps both codes.
        again.settle(refused(again, "wip_exceeded"))
        tasks.anyway("workspace", checkNotNull(tasks.outcome?.anyway))
        assertEquals(JSONArray(listOf("agent_busy", "wip_exceeded")).toString(), submitted.last().request.getJSONArray("confirm").toString())
        submitted.last().let { it.settle(refused(it, "x").copy(status = "completed", code = "", field = "", error = "")) }
        assertEquals(OvermindOutcome("Sent om-1 to ${worker.name}.", OvermindTone.Info), tasks.outcome)
    }

    @Test fun aDeliveryThatMayHaveArrivedStaysOnShowUntilTheNextDelivery() = runBlocking {
        val gateway = OvermindGateway(Connection("workspace", "Example", "https://relay.example", "token"), GATEWAY)
        val submitted = mutableListOf<Submitted>()
        val tasks = holder(this, OvermindRepository(RelayApi()), { gateway }, submitted)
        tasks.write("workspace", "send", OvermindRequests.send(task, worker), OvermindWriteContext("om-1", worker.name))
        val send = submitted.single()
        send.settle(OvermindWrites.create(send.id, MachineKey("workspace", GATEWAY), "send", send.request, emptyList(), "", 1)
            .copy(status = "uncertain", delivery = "uncertain", error = OvermindWrites.RESOLVE))
        assertEquals(task, tasks.outcome?.resolve)
        tasks.write("workspace", "label", JSONObject().put("task", task).put("label", "bug").put("present", true), OvermindWriteContext("om-1", done = "Labelled."))
        assertEquals(task, tasks.outcome?.resolve)
        submitted.last().settle(OvermindWrites.create(submitted.last().id, MachineKey("workspace", GATEWAY), "label", submitted.last().request, emptyList(), "", 1)
            .copy(status = "completed"))
        assertEquals(task, tasks.outcome?.resolve)
        tasks.write("workspace", "send", OvermindRequests.send(task, worker), OvermindWriteContext("om-1", worker.name))
        assertNull(tasks.outcome)
    }
}
