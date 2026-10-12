package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

/** Stage roles of an Overmind workflow, as hgs names them. */
object OvermindRoles {
    const val BACKLOG = "backlog"
    const val READY = "ready"
    const val ACTIVE = "active"
    const val REVIEW = "review"
    const val YOUR_CALL = "needs_human"
    const val DONE = "done"
    const val PARKED = "parked"
}

data class OvermindColumn(val stage: OvermindStage, val cards: List<OvermindCard>)
/** A board as the phone shows it: one column per stage in workflow order, Your Call apart as a strip. */
data class OvermindBoardView(val columns: List<OvermindColumn>, val yourCall: List<OvermindCard>)
/** Asked before a change; [accept] names the confirming button. */
data class OvermindQuestion(val title: String, val text: String, val accept: String)

/** The desktop board's rules (tray/src/TasksView.cpp); hgs stays the authority and its refusals are shown. */
object OvermindWorkflow {
    /** Labels the workflow sets; a person does not remove them, and cards show the verdicts as markers. */
    val reserved = setOf("reviewed", "review-failed", "epic", "epic-ready")
    private val verdicts = setOf("reviewed", "review-failed")
    private val markerText = mapOf("working" to "working", "reviewed" to "reviewed", "needs_work" to "needs work", "conflict" to "conflict",
        "uncertain" to "unconfirmed delivery", "orphaned" to "orphaned")

    fun role(board: OvermindBoard, stage: String) = board.stages.find { it.name == stage }?.role.orEmpty()
    fun first(stages: List<OvermindStage>, role: String) = stages.firstOrNull { it.role == role }
    fun labels(card: OvermindCard) = card.labels.filterNot { it in verdicts }
    fun markers(markers: List<String>) = markers.map { markerText[it] ?: it }

    fun view(board: OvermindBoard): OvermindBoardView {
        val byStage = board.cards.groupBy { it.stage }
        val named = board.stages.filter { it.role != OvermindRoles.YOUR_CALL }.map { OvermindColumn(it, byStage[it.name].orEmpty()) }
        val unknown = board.cards.map { it.stage }.distinct().filter { stage -> board.stages.none { it.name == stage } }
            .map { OvermindColumn(OvermindStage(it, ""), byStage[it].orEmpty()) }
        return OvermindBoardView(named + unknown, board.cards.filter { role(board, it.stage) == OvermindRoles.YOUR_CALL })
    }

    private fun rules(board: OvermindBoard): List<JSONObject> =
        runCatching { JSONObject(board.workflow).optJSONArray("rules")?.objects() }.getOrNull().orEmpty()
    private fun JSONObject.names(key: String) = (opt(key) as? JSONArray)?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String } }.orEmpty()

    /** Whether a person may `move` a task between the stages, with the action hgs derives from their roles. */
    fun humanMove(board: OvermindBoard, from: String, to: String): Boolean {
        val fromRole = role(board, from); val toRole = role(board, to)
        if (fromRole.isEmpty() || toRole.isEmpty()) return false
        val action = when {
            fromRole == OvermindRoles.ACTIVE && toRole == OvermindRoles.READY -> "unclaim"
            fromRole == OvermindRoles.REVIEW && toRole == OvermindRoles.DONE -> "done"
            fromRole == OvermindRoles.DONE && toRole == OvermindRoles.READY -> "reopen"
            toRole == OvermindRoles.ACTIVE && fromRole != OvermindRoles.ACTIVE -> return false   // send it instead
            fromRole == OvermindRoles.YOUR_CALL && toRole != OvermindRoles.PARKED -> return false   // answer it instead
            else -> "move"
        }
        val rules = rules(board)
        // A workflow without rules did not say: the move is offered and hgs decides.
        if (rules.isEmpty()) return true
        return rules.any { rule -> rule.opt("action") == action && rule.opt("to") == to && "human" in rule.names("actors") &&
            from in rule.names("from") && rule.names("requires").isEmpty() }
    }

    /** The stages a person may move the task to; Your Call is where agents ask, never a target. */
    fun moveTargets(board: OvermindBoard, card: OvermindCard): List<OvermindStage> = if (card.readOnly) emptyList() else
        board.stages.filter { it.name != card.stage && it.role != OvermindRoles.YOUR_CALL && humanMove(board, card.stage, it.name) }

    /** A person may send a task from [stage]: a claim rule for people, or a ready stage when the workflow names no rules. */
    fun claimable(board: OvermindBoard, stage: String): Boolean {
        val rules = rules(board)
        if (rules.isEmpty()) return role(board, stage) == OvermindRoles.READY
        return rules.any { it.opt("action") == "claim" && stage in it.names("from") && "human" in it.names("actors") }
    }

    /** Taking a task from its session, out of Your Call or review, or out of Done asks first. */
    fun moveQuestion(board: OvermindBoard, card: OvermindCard, to: String): OvermindQuestion? {
        val from = role(board, card.stage); val target = role(board, to)
        val holder = card.assignee?.name ?: "its session"
        val title = "Move ${card.ref}"
        return when {
            from == OvermindRoles.ACTIVE -> OvermindQuestion(title, "Take ${card.ref} back from $holder and move it to $to? The session is not told and may keep working on it.",
                if (target == OvermindRoles.READY) "Take back" else "Move to $to")
            from == OvermindRoles.YOUR_CALL -> OvermindQuestion(title, "Move ${card.ref} to $to without an answer? $holder keeps waiting for one.", "Move to $to")
            from == OvermindRoles.REVIEW && card.reviewer != null && target != OvermindRoles.DONE ->
                OvermindQuestion(title, "Move ${card.ref} to $to? ${card.reviewer.name} was asked to review it and is not told.", "Move to $to")
            from == OvermindRoles.DONE && target == OvermindRoles.READY ->
                OvermindQuestion(title, "Reopen ${card.ref}? It moves back to $to, and its review verdict is cleared.", "Reopen")
            from == OvermindRoles.DONE -> OvermindQuestion(title, "Move the finished task ${card.ref} to $to?", "Move to $to")
            else -> null
        }
    }

    /** `reorder` for a card dropped at [index] of its column (in display order): before the card that follows it, or at the end.
     * A drop where the card already is writes nothing. */
    fun reorder(column: List<String>, uuid: String, index: Int): JSONObject? {
        val at = column.indexOf(uuid)
        if (at < 0) return null
        val order = column.filterNot { it == uuid }.toMutableList()
        val position = index.coerceIn(0, order.size)
        order.add(position, uuid)
        if (order == column) return null
        return JSONObject().put("task", uuid).also { request -> order.getOrNull(position + 1)?.let { request.put("before", it) } }
    }
}

/** A session as Overmind names it: the Overmind machine of its computer and its session name. */
object OvermindSessions {
    fun same(a: OvermindSession?, b: OvermindSession?) = a != null && b != null && a.name.isNotEmpty() && a.name == b.name && a.machine == b.machine
    /** A pin follows its session's renames on its host: `pinned_current` names it now. */
    fun pinned(card: OvermindCard) = card.pinned?.let { if (card.pinnedCurrent.isNotEmpty()) it.copy(name = card.pinnedCurrent) else it }
    /** The session a card names: its assignee, else the queue's or pin's session, else the reviewer. */
    fun tag(card: OvermindCard) = card.assignee?.name ?: card.queueOf?.name ?: pinned(card)?.name ?: card.reviewer?.name ?: ""
    /** Only a live parent session on a computer whose Overmind machine is known can be named; a name alone may be another machine's. */
    fun ref(session: Session, machines: List<Machine>): OvermindSession? {
        val target = session.target
        if (target.archiveId.isNotBlank() || target.agentId.isNotBlank() || target.session.isBlank()) return null
        val node = machines.find { it.connectionId == target.connectionId && it.id == target.computerId }?.nodeId.orEmpty()
        return if (node.isEmpty()) null else OvermindSession(node, target.session)
    }
}

data class OvermindBadge(val queue: Long, val yourCall: Long) { val empty get() = queue == 0L && yourCall == 0L }

object OvermindBadges {
    /** From the gateway digest: tasks waiting in Queue (ready stages) and in Your Call. No digest is unknown, not zero. */
    fun project(digest: OvermindDigest?, project: String): OvermindBadge? = digest?.let {
        val roles = it.projects[project].orEmpty()
        OvermindBadge(roles[OvermindRoles.READY] ?: 0, roles[OvermindRoles.YOUR_CALL] ?: 0)
    }
    /** From a read board: the session's queue and the tasks it asks a person about. */
    fun session(board: OvermindBoard?, session: OvermindSession?): OvermindBadge? {
        if (board == null || session == null) return null
        val queue = board.cards.count { OvermindWorkflow.role(board, it.stage) == OvermindRoles.READY && OvermindSessions.same(it.queueOf, session) }
        val asks = board.cards.count { OvermindWorkflow.role(board, it.stage) == OvermindRoles.YOUR_CALL && OvermindSessions.same(it.assignee, session) }
        return OvermindBadge(queue.toLong(), asks.toLong())
    }
}

/** Where Add as task puts a draft: this session's queue (Queue, pinned to it), the project pool (Queue) or Backlog. */
enum class OvermindDestination(val label: String) { Queue("To this session's queue"), Pool("To the project pool"), Backlog("To Backlog") }

object OvermindRequests {
    const val UNKNOWN_MACHINE = "Overmind does not know this session's computer yet."
    private const val NO_PROMPT = "Write the task's prompt first."
    /** Why a draft cannot become a task, or "": hgs needs a prompt and takes only images. */
    fun addReason(prompt: String, attachments: List<Attachment>): String = when {
        prompt.isBlank() -> NO_PROMPT
        else -> attachments.firstOrNull { !it.mime.startsWith("image/") }?.let { "Only images can be added to a task. Remove ${it.name} first." }.orEmpty()
    }
    /** The `create` request for a draft; the outbox adds the request_id, which becomes the task UUID. */
    fun addAsTask(project: String, prompt: String, destination: OvermindDestination, session: OvermindSession?, stages: List<OvermindStage>): JSONObject {
        require(prompt.isNotBlank()) { NO_PROMPT }
        val request = JSONObject().put("project", project).put("prompt", prompt)
        // hgs puts a task without a stage into the first Backlog stage.
        if (destination == OvermindDestination.Backlog) return request
        val ready = requireNotNull(OvermindWorkflow.first(stages, OvermindRoles.READY)) { "The project's workflow has no Ready stage." }
        request.put("stage", ready.name)
        if (destination == OvermindDestination.Queue) request.put("pinned", session(requireNotNull(session) { UNKNOWN_MACHINE }))
        return request
    }
    fun session(session: OvermindSession): JSONObject = JSONObject().put("machine", session.machine).put("name", session.name)
    fun send(task: String, session: OvermindSession): JSONObject = JSONObject().put("task", task).put("session", session(session))
}

enum class OvermindTone { Info, Warning, Error }
/** "<action> anyway": the same request with the refusal code confirmed, sent as a new write with a new request_id.
 * [context] names the task and the receiving session for its own message. */
data class OvermindAnyway(val label: String, val action: String, val request: JSONObject, val context: OvermindWriteContext = OvermindWriteContext(""))
/** What a settled write says, and what the person may do next: confirm anyway, resolve a delivery, retry, or check the task. */
data class OvermindOutcome(val text: String, val tone: OvermindTone, val anyway: OvermindAnyway? = null, val resolve: String = "",
    val retry: OvermindWrite? = null, val check: String = "")
/** Names for the message: the task's ref, the receiving session and the text shown on success ("" shows nothing). */
data class OvermindWriteContext(val ref: String, val session: String = "", val done: String = "")

object OvermindOutcomes {
    /** The session a delivery names in its request, for messages about a retried write. */
    fun recipient(write: OvermindWrite): String =
        runCatching { JSONObject(write.request).optJSONObject("session")?.optString("name") }.getOrNull().orEmpty()
    fun of(write: OvermindWrite, context: OvermindWriteContext): OvermindOutcome? {
        val ref = context.ref.ifBlank { write.task.take(8) }
        val name = context.session.ifBlank { "its session" }
        return when {
            write.status == "sending" || write.status == "reviewed" -> null
            write.needsResolution -> OvermindOutcome(when (write.action) {
                "send" -> "$ref may have been sent to $name."
                "review-send" -> "The review request for $ref may have been sent to $name."
                "rework-send" -> "The rework of $ref may have been sent to $name."
                "answer" -> "Your answer on $ref may have been sent to $name."
                else -> "The delivery of $ref may have arrived."
            } + " Check the session, then resolve the delivery. It is never sent again on its own.", OvermindTone.Warning, resolve = write.task)
            write.needsCheck -> OvermindOutcome(write.error.ifBlank { OvermindWrites.CHECK_TASK }, OvermindTone.Warning, check = write.task)
            write.retryable -> OvermindOutcome(write.error.ifBlank { OvermindWrites.UNCONFIRMED }, OvermindTone.Warning, retry = write)
            write.status == "completed" -> when {
                write.delivery == "not_sent" -> OvermindOutcome(write.error.ifBlank { "Saved, but not sent to the session." }, OvermindTone.Warning)
                context.done.isNotBlank() -> OvermindOutcome(context.done, OvermindTone.Info)
                else -> null
            }
            write.status == "refused" && write.action == "answer" && write.code == "not_delivered" -> OvermindOutcome(
                "${write.error} The answer is recorded but was not typed into the session; the phone cannot repeat it. Type it in the session, or repeat it from the desktop.",
                OvermindTone.Error)
            write.status == "refused" -> OvermindOutcome(write.error, OvermindTone.Error, anyway = anyway(write, ref, name))
            else -> OvermindOutcome(write.error.ifBlank { "The change was not made." }, OvermindTone.Error)
        }
    }

    /** Only a delivery's `confirm` refusal is confirmable, as on the desktop. The relay answers a changed body under the same
     * request_id with 409, and hgs stores no receipt for a refusal, so the confirmed request goes as a new write. */
    private fun anyway(write: OvermindWrite, ref: String, name: String): OvermindAnyway? {
        if (write.action !in setOf("send", "review-send", "rework-send", "answer") || write.field != "confirm" || write.code.isBlank()) return null
        val request = runCatching { JSONObject(write.request) }.getOrNull() ?: return null
        request.remove("request_id")
        val confirmed = (request.opt("confirm") as? JSONArray)?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String } }.orEmpty()
        request.put("confirm", JSONArray((confirmed + write.code).distinct()))
        val (label, done) = when (write.action) {
            "send" -> "Send $ref to $name anyway" to "Sent $ref to $name."
            "review-send" -> "Send $ref to $name for review anyway" to "Sent $ref to $name for review."
            "rework-send" -> "Send the rework of $ref anyway" to "Sent the rework of $ref to $name."
            else -> "Send the answer anyway" to "Sent your answer on $ref to $name."
        }
        return OvermindAnyway(label, write.action, request, OvermindWriteContext(ref, name, done))
    }
}

enum class OvermindOpenStep { Open, Wait, Drop }

/** A decision card names a task. It opens where Tasks are shown; without them the tap was only a refresh. */
object OvermindRouting {
    fun step(request: OvermindTaskKey?, available: Boolean, refreshing: Boolean, connected: Boolean): OvermindOpenStep? = when {
        request == null -> null
        !connected -> OvermindOpenStep.Drop
        available -> OvermindOpenStep.Open
        // The first refresh after a cold start learns the relay and gateway operations.
        refreshing -> OvermindOpenStep.Wait
        else -> OvermindOpenStep.Drop
    }
}

/** Where Tasks appear: only while a gateway serves both operations, and only for a project of the swarm catalog. */
object OvermindEntry {
    fun project(available: Boolean, project: ProjectSummary?): String? =
        project?.takeIf { available && it.catalogued && it.key.id.isNotBlank() }?.key?.id
    fun project(available: Boolean, session: Session, projects: List<ProjectSummary>): String? =
        project(available, projects.find { it.key == session.projectKey })
    /** The long-press item: null hides it; "" offers it; anything else disables it with that reason. */
    fun sendTaskReason(available: Boolean, session: Session, projects: List<ProjectSummary>, machines: List<Machine>): String? {
        if (session.target.archiveId.isNotBlank() || session.target.agentId.isNotBlank() || project(available, session, projects) == null) return null
        return if (OvermindSessions.ref(session, machines) == null) OvermindRequests.UNKNOWN_MACHINE else ""
    }
}

data class OvermindChoice(val card: OvermindCard, val note: String)

/** Send a task here: the Queue tasks of the session's project, its own queue first, then the pool, then tasks reserved for others. */
object OvermindSendChoices {
    fun of(board: OvermindBoard, session: OvermindSession): List<OvermindChoice> {
        val sendable = board.cards.filter { OvermindWorkflow.role(board, it.stage) == OvermindRoles.READY && !it.readOnly && "epic" !in it.labels }
        val mine = sendable.filter { OvermindSessions.same(OvermindSessions.pinned(it), session) || OvermindSessions.same(it.queueOf, session) }
        val pool = sendable.filter { it !in mine && it.pinned == null }
        val reserved = sendable.filter { it !in mine && it !in pool }
        return mine.map { OvermindChoice(it, "In this session's queue") } + pool.map { OvermindChoice(it, "Project pool") } +
            reserved.map { OvermindChoice(it, "Reserved for " + OvermindSessions.tag(it)) }
    }
}

enum class OvermindPurpose { Send, Review, Pin }
data class OvermindRecipient(val session: Session, val ref: OvermindSession?, val note: String, val enabled: Boolean, val checked: Boolean)

/** The live sessions of a project a card can be sent to, reviewed by or pinned to. A session whose computer Overmind does not
 * know yet is shown but cannot be chosen; review skips the session that did the work. */
object OvermindRecipients {
    fun of(sessions: List<Session>, machines: List<Machine>, project: ProjectKey, card: OvermindCard, purpose: OvermindPurpose): List<OvermindRecipient> =
        sessions.filter { it.projectKey == project && it.target.archiveId.isBlank() && it.target.agentId.isBlank() }
            .distinctBy { it.target.connectionId to it.target.computerId to it.target.session }
            .map { session ->
                val ref = OvermindSessions.ref(session, machines)
                val worked = purpose == OvermindPurpose.Review && OvermindSessions.same(card.assignee, ref)
                OvermindRecipient(session, ref, when { ref == null -> "its computer is not known yet"; worked -> "did the work"; else -> "" },
                    ref != null && !worked, purpose == OvermindPurpose.Pin && OvermindSessions.same(OvermindSessions.pinned(card), ref))
            }
}

object OvermindJournal {
    /** The question the task waits with: the latest `[needs-human]` entry, only when it belongs to the last transition. */
    fun question(view: OvermindTaskView): String {
        val entry = view.journal.lastOrNull { it.marker == "[needs-human]" } ?: return ""
        return if (view.head.isNotEmpty() && entry.event == view.head) entry.text else ""
    }
    fun latest(view: OvermindTaskView, marker: String) = view.journal.lastOrNull { it.marker == marker }?.text.orEmpty()
    fun author(entry: OvermindJournalEntry): String = when {
        entry.actorKind == "human" -> if (entry.actorDevice.isEmpty()) "a person" else "a person on ${entry.actorDevice}"
        entry.actorSession.isNotEmpty() -> entry.actorSession
        entry.actorKind == "autopilot" -> "autopilot"
        else -> entry.actorKind
    }
}

/** An agent chooses an image's name and type, so only bytes that are a known picture format are shown or opened. */
object OvermindImages {
    private fun starts(bytes: ByteArray, offset: Int, vararg values: Int) =
        bytes.size >= offset + values.size && values.indices.all { bytes[offset + it] == values[it].toByte() }
    fun format(bytes: ByteArray): String? = when {
        starts(bytes, 0, 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a) -> "png"
        starts(bytes, 0, 0xff, 0xd8, 0xff) -> "jpeg"
        starts(bytes, 0, 0x47, 0x49, 0x46, 0x38, 0x37, 0x61) || starts(bytes, 0, 0x47, 0x49, 0x46, 0x38, 0x39, 0x61) -> "gif"
        starts(bytes, 0, 0x52, 0x49, 0x46, 0x46) && starts(bytes, 8, 0x57, 0x45, 0x42, 0x50) -> "webp"
        starts(bytes, 0, 0x42, 0x4d) && bytes.size >= 14 -> "bmp"
        else -> null
    }
}
