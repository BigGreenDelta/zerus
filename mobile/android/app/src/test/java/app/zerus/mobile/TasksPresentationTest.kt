package app.zerus.mobile

import app.zerus.mobile.replica.SessionRef
import app.zerus.mobile.replica.TaskActor
import app.zerus.mobile.replica.TaskInfo
import app.zerus.mobile.replica.TaskStage
import app.zerus.mobile.replica.TaskTarget
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TasksPresentationTest {
    private val here = Machine("workspace-a", "computer-1", "Studio", true, machineId = "node-1")
    private val laptop = Machine("workspace-a", "computer-2", "Laptop", true, machineId = "node-2")
    private val chosen = ChosenSession("codex/api", "computer-1", "node-1")
    private val exchange = setOf("task_exchange", "task_send", "task_cancel", "task_done")
    private var serial = 0

    private fun task(stage: TaskStage, target: TaskTarget = TaskTarget.AnySession, session: SessionRef? = null,
                     summary: String = "", cancelledAt: Long? = null, reason: String = "") = TaskInfo(
        "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e%02d".format(++serial), "T-a1b2%02d".format(serial), "project-1", 1, true, 1, 1,
        TaskActor.Person, stage, target, "V", session, summary, cancelledAt, reason, "Task $serial", emptyList())

    private fun pinned(name: String, machine: String = "node-1") = TaskTarget.Session(SessionRef(machine, name))
    private fun session(name: String, computer: String = "computer-1", raw: String = """{"activity":"idle"}""") =
        Session(Target(computer, name, "run", "conversation", "workspace-a"), name, name.substringBefore('/'), "", "orchard", "", 0, JSONObject(raw))

    private fun route(task: TaskInfo, machines: List<Machine> = listOf(here, laptop), ops: Set<String> = exchange,
                      sessions: List<Session> = listOf(session("codex/api"), session("claude/web", "computer-2"))) =
        TasksPresentation.route(task, chosen, machines, "workspace-a", { ops }, sessions)

    @Test fun stagesKeepTheDesktopOrderAndDefaultToReviewWhenItHasTasks() {
        assertEquals(listOf("In review", "In progress", "To do", "Backlog", "Done"), TaskTab.entries.map { it.label })
        val tasks = listOf(task(TaskStage.TODO), task(TaskStage.BACKLOG))
        assertEquals(TaskTab.TODO, TasksPresentation.defaultTab(TasksPresentation.counts(tasks, chosen, false)))
        val review = tasks + task(TaskStage.REVIEW)
        assertEquals(TaskTab.REVIEW, TasksPresentation.defaultTab(TasksPresentation.counts(review, chosen, false)))
    }

    @Test fun eachStageHasTheSpecsActionsInOrder() {
        assertEquals(listOf(TaskAction.EDIT, TaskAction.TO_BACKLOG, TaskAction.TO_TOP, TaskAction.SEND), TasksPresentation.actions(TaskTab.TODO))
        assertEquals(listOf(TaskAction.EDIT, TaskAction.TO_TODO, TaskAction.SEND), TasksPresentation.actions(TaskTab.BACKLOG))
        assertEquals(listOf(TaskAction.CANCEL), TasksPresentation.actions(TaskTab.ACTIVE))
        assertEquals(listOf(TaskAction.BACK_TO_TODO, TaskAction.ACCEPT), TasksPresentation.actions(TaskTab.REVIEW))
        assertEquals(listOf(TaskAction.BACK_TO_TODO), TasksPresentation.actions(TaskTab.DONE))
    }

    @Test fun onlyThisSessionKeepsItsOwnAndAnySessionTasksInTodoAndBacklog() {
        val mine = task(TaskStage.TODO, pinned("codex/api"))
        val other = task(TaskStage.TODO, pinned("claude/web", "node-2"))
        val any = task(TaskStage.BACKLOG)
        val sentHere = task(TaskStage.ACTIVE, session = SessionRef("node-1", "codex/api"))
        val sentElsewhere = task(TaskStage.REVIEW, session = SessionRef("node-2", "claude/web"))
        val anyReview = task(TaskStage.REVIEW)
        val all = listOf(mine, other, any, sentHere, sentElsewhere, anyReview)
        assertEquals(mapOf(TaskTab.REVIEW to 2, TaskTab.ACTIVE to 1, TaskTab.TODO to 2, TaskTab.BACKLOG to 1),
            TasksPresentation.counts(all, chosen, false))
        assertEquals(mapOf(TaskTab.REVIEW to 0, TaskTab.ACTIVE to 1, TaskTab.TODO to 1, TaskTab.BACKLOG to 1),
            TasksPresentation.counts(all, chosen, true))
        val todo = TasksPresentation.cards(all, TaskTab.TODO, chosen, false, { route(it) }, { route(it) })
        assertEquals(listOf(true, false), todo.map { it.highlighted })
        assertTrue(TasksPresentation.cards(all, TaskTab.ACTIVE, chosen, false, { route(it) }, { route(it) }).single().highlighted)
    }

    @Test fun cardsNameTheirSessionAndInReviewShowsTheReportOrTheCancellation() {
        val any = task(TaskStage.TODO)
        val reported = task(TaskStage.REVIEW, session = SessionRef("node-1", "codex/api"), summary = "Token bucket; 6 tests.")
        val cancelled = task(TaskStage.REVIEW, session = SessionRef("node-1", "codex/api"), summary = "Started",
            cancelledAt = 5, reason = "Wrong approach")
        val all = listOf(any, reported, cancelled)
        assertEquals("any session", TasksPresentation.cards(all, TaskTab.TODO, chosen, false, { route(it) }, { route(it) }).single().sessionLabel)
        val review = TasksPresentation.cards(all, TaskTab.REVIEW, chosen, false, { route(it) }, { route(it) })
        assertEquals(listOf("codex/api", "codex/api"), review.map { it.sessionLabel })
        assertEquals(listOf("Token bucket; 6 tests.", "Cancelled: Wrong approach"), review.map { it.note })
    }

    @Test fun sendNowGoesToThePinnedSessionsComputerOrTheChosenSession() {
        assertEquals(RouteState.Ready(SendRoute("computer-2", "claude/web")), route(task(TaskStage.TODO, pinned("claude/web", "node-2"))))
        assertEquals(RouteState.Ready(SendRoute("computer-1", "codex/api")), route(task(TaskStage.BACKLOG)))
        assertEquals(RouteState.Blocked("Choose a session for this task"),
            TasksPresentation.route(task(TaskStage.TODO), null, listOf(here), "workspace-a", { exchange }, emptyList()))
    }

    @Test fun sendNowIsBlockedWithTheReason() {
        val elsewhere = task(TaskStage.TODO, pinned("claude/web", "node-2"))
        assertEquals(RouteState.Blocked("Laptop is offline"), route(elsewhere, listOf(here, laptop.copy(online = false))))
        assertEquals(RouteState.Blocked("Laptop is offline"), route(elsewhere, listOf(here, laptop.copy(lastKnown = true))))
        assertEquals(RouteState.Blocked("Laptop needs an update to send tasks"), route(elsewhere, ops = setOf("task_exchange")))
        assertEquals(RouteState.Blocked("claude/web no longer exists — choose another session"),
            route(elsewhere, sessions = listOf(session("codex/api"))))
        assertEquals(RouteState.Blocked("claude/web is on a computer this phone does not reach"), route(elsewhere, listOf(here)))
        val card = TasksPresentation.cards(listOf(elsewhere), TaskTab.TODO, chosen, false,
            { route(it, listOf(here, laptop.copy(online = false))) }, { route(it) }).single()
        val send = card.actions.single { it.action == TaskAction.SEND }
        assertFalse(send.enabled)
        assertEquals("Laptop is offline", send.reason)
        assertTrue(card.actions.filter { it.action != TaskAction.SEND }.all { it.enabled })
    }

    @Test fun cancelGoesToTheSessionsComputerOrAnyOnlineComputerOfTheProject() {
        val active = task(TaskStage.ACTIVE, session = SessionRef("node-2", "claude/web"))
        assertEquals(RouteState.Ready(SendRoute("computer-2", "claude/web")),
            TasksPresentation.cancelRoute(active, listOf(here, laptop), { exchange }))
        // That computer is offline: another one cancels it there, as hgs does.
        assertEquals(RouteState.Ready(SendRoute("computer-1", "claude/web")),
            TasksPresentation.cancelRoute(active, listOf(here, laptop.copy(online = false)), { exchange }))
        assertEquals(RouteState.Blocked("No computer of this project is online"),
            TasksPresentation.cancelRoute(active, listOf(here.copy(online = false)), { exchange }))
    }

    @Test fun chipsShowEachSessionsStatus() {
        val sessions = listOf(session("codex/api", raw = """{"activity":"busy"}"""), session("claude/web", "computer-2", """{"phase":"input"}"""),
            session("codex/docs"))
        assertEquals(listOf(ChipDot.WORKING, ChipDot.ATTENTION, ChipDot.READY),
            TasksPresentation.chips(sessions, listOf(here, laptop)).map { it.dot })
        assertEquals(ChosenSession("claude/web", "computer-2", "node-2"), TasksPresentation.chips(sessions, listOf(here, laptop))[1].session)
    }

    @Test fun doneShowsTheLastSevenDaysThenOlderOnRequest() {
        assertEquals("Show older (7)", TasksPresentation.olderLabel(total = 10, recent = 3, shown = 3, expanded = false, more = true))
        assertEquals(null, TasksPresentation.olderLabel(total = 3, recent = 3, shown = 3, expanded = false, more = false))
        assertEquals("Show older (2)", TasksPresentation.olderLabel(total = 10, recent = 3, shown = 8, expanded = true, more = true))
        assertEquals(null, TasksPresentation.olderLabel(total = 10, recent = 3, shown = 10, expanded = true, more = false))
    }
}
