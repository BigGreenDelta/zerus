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
    private class Submitted(val action: String, val request: JSONObject, val id: String, val settle: (OvermindWrite) -> Unit, val unsettled: (String) -> Unit)
    private fun holder(scope: CoroutineScope, api: OvermindRepository, gateway: () -> OvermindGateway?, submitted: MutableList<Submitted> = mutableListOf(),
        connected: () -> Boolean = { gateway() != null }) =
        OvermindTasks(scope, api, { gateway() }, { connected() }, { _, action, request, _, settle, unsettled ->
            UUID.randomUUID().toString().also { submitted += Submitted(action, JSONObject(request.toString()), it, settle, unsettled) } }) { _, _ -> }
    private val synthetic = OvermindGateway(Connection("workspace", "Example", "https://relay.example", "token"), GATEWAY)
    private fun settled(write: Submitted, status: String = "completed") =
        OvermindWrites.create(write.id, MachineKey("workspace", GATEWAY), write.action, write.request, emptyList(), "", 1).copy(status = status)
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
        tasks.openBoard("workspace", "example")
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
        assertEquals(OvermindOutcome("Sent om-1 to ${worker.name}.", OvermindTone.Info, connectionId = "workspace"), tasks.outcome)
    }

    @Test fun aDeliveryThatMayHaveArrivedStaysOnShowUntilTheNextDelivery() = runBlocking {
        val gateway = OvermindGateway(Connection("workspace", "Example", "https://relay.example", "token"), GATEWAY)
        val submitted = mutableListOf<Submitted>()
        val tasks = holder(this, OvermindRepository(RelayApi()), { gateway }, submitted)
        tasks.openTask("workspace", task, "example")
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

    @Test fun aFailedRefreshKeepsTheWorkspaceAndSettlesItsWrites() = runBlocking {
        val png = byteArrayOf(0x89.toByte(), 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a) + ByteArray(24) { it.toByte() }
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(png).joinToString("") { "%02x".format(it) }
        val relay = FakeOvermindRelay { payload ->
            if (payload.getString("action") != "attachment") answer(payload) else ok().put("task", task).put("sha256", sha).put("name", "screen.png")
                .put("mime", "image/png").put("size", png.size).put("offset", 0).put("length", png.size).put("data_base64", java.util.Base64.getEncoder().encodeToString(png))
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            // Only the gateway's confirmation is lost (the workspace stays connected), as after one failed catalog refresh.
            var reachable = true
            val submitted = mutableListOf<Submitted>()
            val tasks = holder(this, OvermindRepository(RelayApi()), { gateway.takeIf { reachable } }, submitted) { true }
            tasks.openTask("workspace", task, "example")
            tasks.editDrafts("workspace", task) { it.copy(answer = "REST") }
            var after = 0
            tasks.write("workspace", "label", JSONObject().put("task", task).put("label", "bug").put("present", true), OvermindWriteContext("om-1", done = "Labelled.")) { after++ }
            reachable = false
            submitted.single().settle(settled(submitted.single()))
            assertEquals(1, after)
            assertEquals(OvermindOutcome("Labelled.", OvermindTone.Info, connectionId = "workspace"), tasks.outcome)
            assertEquals("REST", tasks.drafts("workspace", task).answer)
            // A read without a confirmed gateway says so instead of spinning, and reads again on a tap once it is back.
            tasks.image("workspace", task, sha)
            assertEquals(OvermindTasks.UNREACHABLE, tasks.images[tasks.imageKey("workspace", task, sha)]?.error)
            tasks.load("workspace", "example")
            assertEquals(OvermindTasks.UNREACHABLE, tasks.board("workspace", "example")?.error)
            reachable = true
            tasks.image("workspace", task, sha, retry = true)
            withTimeout(10_000) { while (tasks.images[tasks.imageKey("workspace", task, sha)]?.loading != false) delay(10) }
            assertEquals("png", tasks.images[tasks.imageKey("workspace", task, sha)]?.format)
        } }
    }

    @Test fun draftsOutliveTheCardAndGoWithTheWorkspace() = runBlocking {
        var connected = true
        val tasks = holder(this, OvermindRepository(RelayApi()), { synthetic.takeIf { connected } })
        tasks.openTask("workspace", task, "example")
        tasks.editDrafts("workspace", task) { it.copy(prompt = "Edited", promptBase = "Do it", title = "New title", comment = "Note") }
        tasks.closeTask(); tasks.close()
        tasks.openTask("workspace", "bbbbbbbb-0000-4000-8000-000000000002", "example")
        assertEquals(OvermindCardDrafts(title = "New title", prompt = "Edited", promptBase = "Do it", comment = "Note"), tasks.drafts("workspace", task))
        assertEquals(OvermindCardDrafts(), tasks.drafts("workspace", "bbbbbbbb-0000-4000-8000-000000000002"))
        // An edit back to nothing keeps no entry.
        tasks.editDrafts("workspace", "bbbbbbbb-0000-4000-8000-000000000002") { it.copy(answer = "x") }
        tasks.editDrafts("workspace", "bbbbbbbb-0000-4000-8000-000000000002") { it.copy(answer = "") }
        assertEquals(1, tasks.drafts.size)
        connected = false
        tasks.purge("workspace")
        assertTrue(tasks.drafts.isEmpty())
        tasks.editDrafts("workspace", task) { it.copy(answer = "late") }
        assertTrue(tasks.drafts.isEmpty())
    }

    @Test fun oneAddPerComposerUntilItSettles() = runBlocking {
        val submitted = mutableListOf<Submitted>()
        val tasks = holder(this, OvermindRepository(RelayApi()), { synthetic }, submitted)
        val composer = JSONArray(listOf("workspace", GATEWAY, worker.name, "run", "conversation")).toString()
        var added = 0
        tasks.addAsTask(composer, "workspace", "example", "Fix it", emptyList(), OvermindDestination.Backlog, worker) { added++ }
        // Counted from the tap, before the board or the outbox answer.
        assertTrue(composer in tasks.adding)
        tasks.addAsTask(composer, "workspace", "example", "Fix it", emptyList(), OvermindDestination.Backlog, worker) { added++ }
        withTimeout(10_000) { while (submitted.isEmpty()) delay(10) }
        delay(50)
        assertEquals(1, submitted.size)
        assertTrue(composer in tasks.adding)
        submitted.single().settle(settled(submitted.single()))
        assertFalse(composer in tasks.adding)
        assertEquals(1, added)
        // A write that ended without a settlement (not saved, or its result not saved) frees the composer too.
        tasks.addAsTask(composer, "workspace", "example", "Fix it", emptyList(), OvermindDestination.Backlog, worker) { added++ }
        withTimeout(10_000) { while (submitted.size < 2) delay(10) }
        submitted.last().unsettled("Could not save the task change.")
        assertFalse(composer in tasks.adding)
        assertNull(tasks.notices[composer])
        assertEquals(1, added)
        // A disconnect drops the composer's add and notice.
        tasks.addAsTask(composer, "workspace", "example", "Fix it", emptyList(), OvermindDestination.Backlog, worker) { added++ }
        tasks.purge("workspace")
        assertTrue(tasks.adding.isEmpty()); assertTrue(tasks.notices.isEmpty())
    }

    @Test fun aMessageStaysWithItsWorkspace() = runBlocking {
        val submitted = mutableListOf<Submitted>()
        val tasks = holder(this, OvermindRepository(RelayApi()), { synthetic }, submitted)
        tasks.openBoard("workspace", "example")
        tasks.write("workspace", "send", OvermindRequests.send(task, worker), OvermindWriteContext("om-1", worker.name))
        submitted.last().settle(OvermindWrites.create(submitted.last().id, MachineKey("workspace", GATEWAY), "send", submitted.last().request, emptyList(), "", 1)
            .copy(status = "refused", code = "agent_busy", field = "confirm", error = "busy"))
        assertEquals("workspace", tasks.outcome?.connectionId)
        // Another workspace's board shows no message of this one, and a late settlement of this one does not reach it.
        tasks.openBoard("other", "example")
        assertNull(tasks.outcome)
        tasks.write("workspace", "label", JSONObject().put("task", task).put("label", "bug").put("present", true), OvermindWriteContext("om-1", done = "Labelled."))
        submitted.last().settle(settled(submitted.last()))
        assertNull(tasks.outcome)
    }
}
