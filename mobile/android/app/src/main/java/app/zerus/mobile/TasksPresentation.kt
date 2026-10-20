package app.zerus.mobile

import app.zerus.mobile.replica.SessionRef
import app.zerus.mobile.replica.TaskInfo
import app.zerus.mobile.replica.TaskStage
import app.zerus.mobile.replica.TaskTarget

/** The stage tabs of the Tasks screen, in the order of the desktop's tiles. */
enum class TaskTab(val label: String) { REVIEW("In review"), ACTIVE("In progress"), TODO("To do"), BACKLOG("Backlog"), DONE("Done") }

/** The stage actions, the same list as on the desktop row and in the task settings. */
enum class TaskAction(val label: String) {
    EDIT("Edit"), TO_BACKLOG("Move to Backlog"), TO_TOP("Move to top"), SEND("Send now"), TO_TODO("Move to To do"),
    CANCEL("Cancel"), BACK_TO_TODO("Back to To do"), ACCEPT("Accept")
}

data class ActionState(val action: TaskAction, val enabled: Boolean, val reason: String = "")

/** One card: `note` is the agent's report or the cancellation, In review only. */
data class TaskCard(val uuid: String, val id: String, val text: String, val tab: TaskTab, val sessionLabel: String,
                    val highlighted: Boolean, val note: String, val actions: List<ActionState>)

/** The session Tasks is for: it receives new tasks and Any session tasks are sent to it. */
data class ChosenSession(val name: String, val computerId: String, val machineId: String)

enum class ChipDot { WORKING, ATTENTION, READY }
data class SessionChip(val session: ChosenSession, val dot: ChipDot)

/** The computer a Send now or Cancel goes to, and the session it names. */
data class SendRoute(val computerId: String, val session: String)
sealed interface RouteState {
    data class Ready(val route: SendRoute) : RouteState
    data class Blocked(val reason: String) : RouteState
}

/** What the phone's Tasks screen shows, from the replica's listing (docs/superpowers/specs/2026-10-13-project-task-queue-design.md, "Android interface"). */
object TasksPresentation {
    fun tab(stage: TaskStage) = when (stage) {
        TaskStage.REVIEW -> TaskTab.REVIEW
        TaskStage.ACTIVE -> TaskTab.ACTIVE
        TaskStage.TODO -> TaskTab.TODO
        TaskStage.BACKLOG -> TaskTab.BACKLOG
        TaskStage.DONE -> TaskTab.DONE
    }

    fun actions(tab: TaskTab): List<TaskAction> = when (tab) {
        TaskTab.TODO -> listOf(TaskAction.EDIT, TaskAction.TO_BACKLOG, TaskAction.TO_TOP, TaskAction.SEND)
        TaskTab.BACKLOG -> listOf(TaskAction.EDIT, TaskAction.TO_TODO, TaskAction.SEND)
        TaskTab.ACTIVE -> listOf(TaskAction.CANCEL)
        TaskTab.REVIEW -> listOf(TaskAction.BACK_TO_TODO, TaskAction.ACCEPT)
        TaskTab.DONE -> listOf(TaskAction.BACK_TO_TODO)
    }

    private fun same(ref: SessionRef?, chosen: ChosenSession?) =
        ref != null && chosen != null && ref.machine == chosen.machineId && ref.session == chosen.name

    /** Pinned to the chosen session in To do and Backlog; sent to it in the other stages. */
    fun highlighted(task: TaskInfo, chosen: ChosenSession?): Boolean = when (task.stage) {
        TaskStage.TODO, TaskStage.BACKLOG -> same((task.target as? TaskTarget.Session)?.session, chosen)
        else -> same(task.session, chosen)
    }

    /** Only this session: its own tasks, and the Any session tasks it can take. */
    fun visible(task: TaskInfo, chosen: ChosenSession?, onlyThisSession: Boolean) = !onlyThisSession ||
        highlighted(task, chosen) || (task.target is TaskTarget.AnySession && task.stage in listOf(TaskStage.TODO, TaskStage.BACKLOG))

    /** The counts of the replica's stages; Done counts come from a computer's list. */
    fun counts(tasks: List<TaskInfo>, chosen: ChosenSession?, onlyThisSession: Boolean): Map<TaskTab, Int> =
        listOf(TaskTab.REVIEW, TaskTab.ACTIVE, TaskTab.TODO, TaskTab.BACKLOG).associateWith { tab ->
            tasks.count { tab(it.stage) == tab && visible(it, chosen, onlyThisSession) }
        }

    fun defaultTab(counts: Map<TaskTab, Int>) = if ((counts[TaskTab.REVIEW] ?: 0) > 0) TaskTab.REVIEW else TaskTab.TODO

    fun sessionLabel(task: TaskInfo): String {
        val target = (task.target as? TaskTarget.Session)?.session?.session
        return when (task.stage) {
            TaskStage.TODO, TaskStage.BACKLOG -> target ?: "any session"
            else -> task.session?.session ?: target ?: "any session"
        }
    }

    fun note(task: TaskInfo): String = when {
        task.stage != TaskStage.REVIEW -> ""
        task.cancelledAt != null -> if (task.cancelReason.isBlank()) "Cancelled" else "Cancelled: ${task.cancelReason}"
        else -> task.summary
    }

    fun cards(tasks: List<TaskInfo>, tab: TaskTab, chosen: ChosenSession?, onlyThisSession: Boolean,
              send: (TaskInfo) -> RouteState, cancel: (TaskInfo) -> RouteState): List<TaskCard> =
        tasks.filter { tab(it.stage) == tab && visible(it, chosen, onlyThisSession) }.map { task ->
            val states = actions(tab).map { action ->
                val route = when (action) {
                    TaskAction.SEND -> send(task)
                    TaskAction.CANCEL -> cancel(task)
                    else -> null
                }
                when {
                    !task.writable -> ActionState(action, false, "Update Zerus to change this task")
                    route is RouteState.Blocked -> ActionState(action, false, route.reason)
                    else -> ActionState(action, true)
                }
            }
            TaskCard(task.uuid, task.id, task.text, tab, sessionLabel(task), highlighted(task, chosen), note(task), states)
        }

    /** The computer of `ref` among this connection's machines. */
    private fun computer(ref: SessionRef, machines: List<Machine>, connectionId: String) =
        machines.firstOrNull { it.connectionId == connectionId && it.machineId == ref.machine }

    private fun reachable(machine: Machine) = machine.online && !machine.lastKnown

    /** Where Send now goes, or why it cannot: the pinned session's computer, or the chosen session for an Any session task. */
    fun route(task: TaskInfo, chosen: ChosenSession?, machines: List<Machine>, connectionId: String,
              operations: (Machine) -> Set<String>, sessions: List<Session>): RouteState {
        val ref = (task.target as? TaskTarget.Session)?.session
            ?: chosen?.let { SessionRef(it.machineId, it.name) }
            ?: return RouteState.Blocked("Choose a session for this task")
        val machine = computer(ref, machines, connectionId)
            ?: return RouteState.Blocked("${ref.session} is on a computer this phone does not reach")
        if (!reachable(machine)) return RouteState.Blocked("${machine.name} is offline")
        if ("task_send" !in operations(machine)) return RouteState.Blocked("${machine.name} needs an update to send tasks")
        if (sessions.none { it.target.computerId == machine.id && it.target.session == ref.session && it.target.archiveId.isBlank() })
            return RouteState.Blocked("${ref.session} no longer exists — choose another session")
        return RouteState.Ready(SendRoute(machine.id, ref.session))
    }

    /** Cancel goes to the session's computer, or, while it is out of reach, to any online computer of the project, which cancels it there as hgs does. */
    fun cancelRoute(task: TaskInfo, machines: List<Machine>, operations: (Machine) -> Set<String>): RouteState {
        val ref = task.session ?: return RouteState.Blocked("This task was not sent to a session")
        val able = machines.filter { reachable(it) && "task_cancel" in operations(it) }
        val machine = able.firstOrNull { it.machineId == ref.machine } ?: able.firstOrNull()
            ?: return RouteState.Blocked("No computer of this project is online")
        return RouteState.Ready(SendRoute(machine.id, ref.session))
    }

    /** One chip per live session of the project, with its status. */
    fun chips(sessions: List<Session>, machines: List<Machine>): List<SessionChip> =
        sessions.filter { it.target.archiveId.isBlank() && !SessionFilters.archived(it) }.map { session ->
            val machine = machines.firstOrNull { it.connectionId == session.target.connectionId && it.id == session.target.computerId }
            val online = SessionFilters.online(session, machines)
            val dot = when {
                SessionFilters.needsAction(session) -> ChipDot.ATTENTION
                SessionFilters.working(session, online) -> ChipDot.WORKING
                else -> ChipDot.READY
            }
            SessionChip(ChosenSession(session.target.session, session.target.computerId, machine?.machineId.orEmpty()), dot)
        }

    /** "Show older (N)": the older Done tasks, while some are not shown. */
    fun olderLabel(total: Int, recent: Int, shown: Int, expanded: Boolean, more: Boolean): String? {
        val older = if (expanded) total - shown else total - recent
        return if (older > 0 && (more || !expanded)) "Show older ($older)" else null
    }
}
