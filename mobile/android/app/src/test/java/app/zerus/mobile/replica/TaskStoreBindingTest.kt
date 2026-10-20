package app.zerus.mobile.replica

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/** The task replica through its generated bindings, on the host library the build made. */
class TaskStoreBindingTest {
    @get:Rule val folders = TemporaryFolder()

    private fun store() = TaskStore(folders.newFolder().path, "project-1", "orchard")

    @Test
    fun tasksAreFiledListedAndChanged() {
        val store = store()
        val later = store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val first = store.add("Measure first", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.Top)
        assertEquals(listOf("Measure first", "Add rate limiting"), store.list().tasks.map { it.text })
        store.moveTask(later.uuid, TaskPosition.Top)
        store.setStage(first.uuid, TaskStage.BACKLOG)
        val target = TaskTarget.Session(SessionRef("4b810143-c06b-48bc-b665-9625e51357ae", "codex/api"))
        val pinned = store.setTarget(later.uuid, target)
        assertEquals(target, pinned.target)
        val commented = store.comment(later.uuid, "Measured: 40 rps.")
        assertEquals("Measured: 40 rps.", commented.log.last { it.kind == "comment" }.text)
        assertEquals(TaskActor.Person, commented.log.last().actor)
        assertEquals(listOf(TaskStage.TODO, TaskStage.BACKLOG), store.list().tasks.map { it.stage })
    }

    @Test
    fun refusalsArriveAsRefusedWithTheirMessage() {
        val store = store()
        val task = store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        try {
            store.editText(task.uuid, "  ")
            fail("an empty prompt was accepted")
        } catch (refused: ReplicaException.Refused) {
            assertTrue(refused.message.orEmpty(), refused.message.orEmpty().contains("empty"))
        }
        try {
            store.add("Nope", TaskStage.DONE, TaskTarget.AnySession, TaskPosition.End)
            fail("a task was filed in Done")
        } catch (refused: ReplicaException.Refused) {
            assertTrue(refused.message.orEmpty().contains("Backlog or To do"))
        }
    }

    @Test
    fun twoReplicasMeetThroughTheBindings() {
        val (a, b) = store() to store()
        a.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val page = a.updatesFor(b.inventory(), 1_048_576UL)
        assertFalse(page.more)
        assertTrue(b.importUpdates(page.updates).changed > 0u)
        assertEquals("Add rate limiting", b.list().tasks.single().text)
        assertEquals(a.inventory(), b.inventory())
    }

    @Test
    fun aPhoneExchangesWithAComputerThroughJson() {
        val (computer, phone) = store() to store()
        computer.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        phone.exchange().use { exchange ->
            var progress: ExchangeProgress? = null
            while (true) {
                val request = exchange.nextRequest() ?: break
                progress = exchange.accept(computer.answerExchange(request))
            }
            assertTrue(progress!!.finished)
        }
        assertEquals(computer.liveDigest(), phone.liveDigest())
        assertEquals("Add rate limiting", phone.list().tasks.single().text)
    }
}
