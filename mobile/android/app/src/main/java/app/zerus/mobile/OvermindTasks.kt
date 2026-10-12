package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

data class OvermindBoardKey(val connectionId: String, val project: String)
/** [fresh]: read from the gateway in this run, not only from the encrypted cache. */
data class OvermindBoardState(val board: OvermindBoard? = null, val loading: Boolean = false, val error: String = "", val fresh: Boolean = false)
data class OvermindCardKey(val connectionId: String, val task: String)
data class OvermindCardState(val view: OvermindTaskView? = null, val loading: Boolean = false, val error: String = "", val older: Boolean = false)
/** An image's verified bytes and their real format, or why it is not shown. */
class OvermindImageState(val bytes: ByteArray? = null, val format: String? = null, val loading: Boolean = false, val error: String = "")
/** The Tasks window: a project's board, and the card open on it ("" for none). A decision tap may open a card before its project is known. */
data class OvermindScreen(val connectionId: String, val project: String, val task: String = "")
/** Unsaved edits of one card. Like the desktop's, they live while the app runs: Back, another card or a decision tap keeps them;
 * a save or a discard ends them, and a disconnect drops them. [prompt] is null while the prompt is not being edited. */
data class OvermindCardDrafts(val title: String? = null, val prompt: String? = null, val promptBase: String = "", val answer: String = "",
    val comment: String = "", val label: String = "", val link: String = "", val linkKind: String = "follows")

/** Reads, writes and navigation of the Tasks screens. Reads refresh on demand or when the gateway digest moves past a board's
 * cursor; writes go through the outbox and are never repeated without a tap. State is kept while the workspace is connected,
 * also through a failed refresh ([gateway] null), and everything of a workspace is dropped on disconnect. */
class OvermindTasks(private val scope: CoroutineScope, private val repository: OvermindRepository,
    private val gateway: (String) -> OvermindGateway?,
    private val connected: (String) -> Boolean,
    /** (connection, action, request, images, settled, unsettled): the request_id, or "" when nothing was saved. `unsettled` runs
     * when the write ended without a settlement to report (not saved, or its result could not be saved). */
    private val submit: (String, String, JSONObject, List<Attachment>, (OvermindWrite) -> Unit, (String) -> Unit) -> String,
    private val retryWrite: (OvermindWrite, (OvermindWrite) -> Unit) -> Unit,
    /** Gives up saved image copies that a change never saved; it keeps any that a draft or a write still names. */
    private val release: (List<Attachment>) -> Unit = {}) {
    companion object {
        const val UNAVAILABLE = "Tasks are not available for this workspace."
        const val UNREACHABLE = "The gateway that serves Tasks was not confirmed by the last refresh. Refresh, then try again."
        private const val IMAGE_BUDGET = 48L * 1024 * 1024
    }
    var boards by mutableStateOf<Map<OvermindBoardKey, OvermindBoardState>>(emptyMap()); private set
    var cards by mutableStateOf<Map<OvermindCardKey, OvermindCardState>>(emptyMap()); private set
    var images by mutableStateOf<Map<String, OvermindImageState>>(emptyMap()); private set
    var drafts by mutableStateOf<Map<OvermindCardKey, OvermindCardDrafts>>(emptyMap()); private set
    var screen by mutableStateOf<OvermindScreen?>(null); private set
    /** The Tasks window's message bar; its next steps act on its own [OvermindOutcome.connectionId]. */
    var outcome by mutableStateOf<OvermindOutcome?>(null); private set
    /** Send a task here: the session chosen in its long-press sheet, and what the send said. */
    var sendTo by mutableStateOf<Session?>(null); private set
    var sendOutcome by mutableStateOf<OvermindOutcome?>(null); private set
    /** Add as task, per composer target key: its message, and the adds still reading the board or awaiting their settlement. */
    var notices by mutableStateOf<Map<String, OvermindOutcome>>(emptyMap()); private set
    var adding by mutableStateOf<Set<String>>(emptySet()); private set
    private val reads = mutableMapOf<Any, Job>()
    private val again = mutableSetOf<Any>()

    private fun message(e: Exception) = e.message?.takeIf { it.isNotBlank() } ?: "The task board could not be read."
    private fun updateBoard(key: OvermindBoardKey, change: (OvermindBoardState) -> OvermindBoardState) {
        if (connected(key.connectionId)) boards = boards + (key to change(boards[key] ?: OvermindBoardState()))
    }
    private fun updateCard(key: OvermindCardKey, change: (OvermindCardState) -> OvermindCardState) {
        if (connected(key.connectionId)) cards = cards + (key to change(cards[key] ?: OvermindCardState()))
    }

    fun board(connectionId: String, project: String) = boards[OvermindBoardKey(connectionId, project)]
    fun card(connectionId: String, task: String) = cards[OvermindCardKey(connectionId, task)]
    fun drafts(connectionId: String, task: String) = drafts[OvermindCardKey(connectionId, task)] ?: OvermindCardDrafts()
    fun editDrafts(connectionId: String, task: String, change: (OvermindCardDrafts) -> OvermindCardDrafts) {
        if (!connected(connectionId) || task.isEmpty()) return
        val key = OvermindCardKey(connectionId, task)
        val next = change(drafts[key] ?: OvermindCardDrafts())
        drafts = if (next == OvermindCardDrafts()) drafts - key else drafts + (key to next)
    }

    /** The cached board at once, then its changes from the gateway. Without [explicit], a fresh board is read again only once the
     * gateway digest ([rev]) moved past its cursor. One read per board at a time; an explicit request meanwhile reads again after it. */
    fun load(connectionId: String, project: String, rev: Long? = null, explicit: Boolean = false) {
        if (project.isEmpty()) return
        val key = OvermindBoardKey(connectionId, project)
        if (reads[key]?.isActive == true) { if (explicit) again += key; return }
        val current = boards[key]
        val board = current?.board
        if (!explicit && current?.fresh == true && board != null && current.error.isEmpty() && (rev == null || (board.cursor ?: -1) >= rev)) return
        val target = gateway(connectionId) ?: run { updateBoard(key) { it.copy(loading = false, error = UNREACHABLE) }; return }
        updateBoard(key) { it.copy(loading = true) }
        reads[key] = scope.launch {
            try {
                val base = board ?: repository.cachedBoard(target, project)?.also { cached -> updateBoard(key) { it.copy(board = cached) } }
                val read = repository.board(target, project, base)
                updateBoard(key) { it.copy(board = read, loading = false, error = "", fresh = true) }
            } catch (e: CancellationException) { updateBoard(key) { it.copy(loading = false) }; throw e }
            catch (e: Exception) { updateBoard(key) { it.copy(loading = false, error = message(e)) } }
            finally { reads.remove(key); if (again.remove(key)) load(connectionId, project, explicit = true) }
        }
    }

    /** Reads the full task; a request while one runs reads again after it, as the board entry may have changed meanwhile. */
    fun loadCard(connectionId: String, task: String) {
        if (task.isEmpty()) return
        val key = OvermindCardKey(connectionId, task)
        if (reads[key]?.isActive == true) { again += key; return }
        val target = gateway(connectionId) ?: run { updateCard(key) { it.copy(loading = false, error = UNREACHABLE) }; return }
        updateCard(key) { it.copy(loading = true) }
        reads[key] = scope.launch {
            try {
                if (cards[key]?.view == null) repository.cachedTask(target, task)?.let { cached -> updateCard(key) { it.copy(view = cached) } }
                val view = repository.task(target, task)
                updateCard(key) { it.copy(view = view, loading = false, error = "") }
                // A decision tap names only the task: its project comes with it.
                screen?.let { shown -> if (shown.connectionId == connectionId && shown.task == task && shown.project.isEmpty()) screen = shown.copy(project = view.card.project) }
            } catch (e: CancellationException) { updateCard(key) { it.copy(loading = false) }; throw e }
            catch (e: Exception) { updateCard(key) { it.copy(loading = false, error = message(e)) } }
            finally { reads.remove(key); if (again.remove(key)) loadCard(connectionId, task) }
        }
    }

    fun older(connectionId: String, task: String) {
        val key = OvermindCardKey(connectionId, task)
        val view = cards[key]?.view ?: return
        if (reads[key]?.isActive == true || view.journalFrom <= 0) return
        val target = gateway(connectionId) ?: run { updateCard(key) { it.copy(error = UNREACHABLE) }; return }
        updateCard(key) { it.copy(older = true) }
        reads[key] = scope.launch {
            try { val longer = repository.olderJournal(target, view); updateCard(key) { it.copy(view = longer, older = false, error = "") } }
            catch (e: CancellationException) { updateCard(key) { it.copy(older = false) }; throw e }
            catch (e: Exception) { updateCard(key) { it.copy(older = false, error = message(e)) } }
            finally { reads.remove(key); if (again.remove(key)) loadCard(connectionId, task) }
        }
    }

    fun imageKey(connectionId: String, task: String, sha256: String) = "$connectionId\n$task\n$sha256"
    /** Reads an image in ranges and keeps it only when its bytes are a picture; an agent chose its name and type. Tapping a failed
     * image, or one whose read is no longer running, reads it again. */
    fun image(connectionId: String, task: String, sha256: String, retry: Boolean = false) {
        val key = imageKey(connectionId, task, sha256)
        val current = images[key]
        if (reads[key]?.isActive == true) return
        if (current != null && !(retry && (current.error.isNotEmpty() || current.loading))) return
        if (!connected(connectionId)) return
        val target = gateway(connectionId) ?: run { images = images + (key to OvermindImageState(error = UNREACHABLE)); return }
        images = images + (key to OvermindImageState(loading = true))
        reads[key] = scope.launch {
            val state = try {
                val image = repository.attachment(target, task, sha256)
                OvermindImages.format(image.bytes)?.let { OvermindImageState(image.bytes, it) }
                    ?: OvermindImageState(error = "This file is not a picture the phone can show, so it is not opened.")
            } catch (e: CancellationException) { images = images - key; throw e }
            catch (e: Exception) { OvermindImageState(error = e.message?.ifBlank { null } ?: "The image could not be read.") }
            finally { reads.remove(key) }
            if (connected(connectionId)) images = trim(images + (key to state))
        }
    }
    private fun trim(all: Map<String, OvermindImageState>): Map<String, OvermindImageState> {
        var total = all.values.sumOf { it.bytes?.size?.toLong() ?: 0 }
        val kept = LinkedHashMap(all)
        for (key in all.keys) { if (total <= IMAGE_BUDGET) break; kept.remove(key)?.bytes?.let { total -= it.size } }
        return kept
    }

    /** Another workspace's message never stays on the window. */
    private fun show(next: OvermindScreen) {
        if (outcome?.connectionId != next.connectionId) outcome = null
        screen = next
    }
    fun openBoard(connectionId: String, project: String) { show(OvermindScreen(connectionId, project)); outcome = null }
    fun openTask(connectionId: String, task: String, project: String = screen?.takeIf { it.connectionId == connectionId }?.project.orEmpty()) {
        show(OvermindScreen(connectionId, project, task))
    }
    fun closeTask() { screen = screen?.copy(task = "") }
    fun close() { screen = null; outcome = null }
    fun chooseTaskFor(session: Session) { sendTo = session; sendOutcome = null }
    fun closeChooser() { sendTo = null; sendOutcome = null }
    fun dismissOutcome() { outcome = null }
    fun reportSend(said: OvermindOutcome?) { sendOutcome = said }
    fun dismissNotice(key: String) { notices = notices - key }
    fun notify(key: String, said: OvermindOutcome?) { notices = if (said == null) notices - key else notices + (key to said) }

    /** Saves and sends one change; its settlement becomes the message ([report], the window's bar by default). A delivery that
     * may have arrived stays on show until another delivery or a dismissal. Returns the request_id, or "" when nothing was saved. */
    fun write(connectionId: String, action: String, request: JSONObject, context: OvermindWriteContext, attachments: List<Attachment> = emptyList(),
        report: ((OvermindOutcome?) -> Unit)? = null, after: (OvermindWrite) -> Unit = {}): String {
        if (report == null && (outcome?.resolve.isNullOrEmpty() || action in OvermindAccess.deliveringActions)) outcome = null
        val answer = if (action == "answer") request.optString("answer") else null
        val id = submit(connectionId, action, request, attachments, { settled -> recorded(connectionId, settled, answer); settle(connectionId, settled, context, report, after) }) {}
        // A change that was never saved (no gateway, storage not ready, refused by the outbox) leaves its imported images to nobody.
        if (id.isEmpty() && attachments.isNotEmpty()) release(attachments)
        return id
    }
    /** An answer that the journal holds (or may hold) ends its draft, so it never pre-fills the box for a later question.
     * A text typed since stays. */
    private fun recorded(connectionId: String, settled: OvermindWrite, answer: String?) {
        if (answer != null && OvermindOutcomes.recordsAnswer(settled))
            editDrafts(connectionId, settled.task) { if (it.answer == answer) it.copy(answer = "") else it }
    }
    private fun settle(connectionId: String, settled: OvermindWrite, context: OvermindWriteContext, report: ((OvermindOutcome?) -> Unit)?, after: (OvermindWrite) -> Unit) {
        if (!connected(connectionId)) return
        val said = OvermindOutcomes.of(settled, context)?.copy(connectionId = connectionId)
        // A delivery that may have arrived stays on show until another delivery, a problem or a dismissal; the window shows
        // only messages of the workspace it shows.
        if (report != null) report(said)
        else if (said != null && screen?.connectionId == connectionId &&
            (outcome?.resolve.isNullOrEmpty() || settled.action in OvermindAccess.deliveringActions || said.tone != OvermindTone.Info)) outcome = said
        after(settled)
        // The board and the card are read again: the compact card in the receipt lacks the board's markers and queues.
        val project = runCatching { JSONObject(settled.result).optString("project") }.getOrNull().orEmpty()
            .ifEmpty { screen?.takeIf { it.connectionId == connectionId }?.project.orEmpty() }
        if (project.isNotEmpty() && boards.containsKey(OvermindBoardKey(connectionId, project))) load(connectionId, project, explicit = true)
        if (settled.task.isNotEmpty() && cards.containsKey(OvermindCardKey(connectionId, settled.task)) && settled.action != "delete") loadCard(connectionId, settled.task)
    }
    /** A follow-up ("<action> anyway", "Send the answer again") is a new write with a new request_id. */
    fun anyway(connectionId: String, offer: OvermindAnyway, report: ((OvermindOutcome?) -> Unit)? = null) =
        write(connectionId, offer.action, offer.request, offer.context, report = report)
    /** Explicit only: the same request_id and payload, which the relay and hgs never apply twice. */
    fun retry(write: OvermindWrite, context: OvermindWriteContext, report: ((OvermindOutcome?) -> Unit)? = null, after: (OvermindWrite) -> Unit = {}) {
        val answer = if (write.action == "answer") runCatching { JSONObject(write.request).optString("answer") }.getOrNull() else null
        retryWrite(write) { settled -> recorded(write.connectionId, settled, answer); settle(write.connectionId, settled, context, report, after) }
    }

    /** Add as task: the workflow's first Ready stage comes from the board (cached or read now); the draft clears only once added.
     * One add per composer at a time, from the tap until its settlement. */
    fun addAsTask(noticeKey: String, connectionId: String, project: String, prompt: String, attachments: List<Attachment>,
        destination: OvermindDestination, session: OvermindSession?, onAdded: (OvermindWrite) -> Unit) {
        if (noticeKey in adding || !connected(connectionId)) return
        adding = adding + noticeKey
        notices = notices + (noticeKey to OvermindOutcome("Adding the task…", OvermindTone.Info, connectionId = connectionId))
        fun done() { adding = adding - noticeKey }
        scope.launch {
            try {
                val target = gateway(connectionId) ?: error(UNREACHABLE)
                val stages = if (destination == OvermindDestination.Backlog) emptyList() else
                    (board(connectionId, project)?.board ?: repository.cachedBoard(target, project) ?: repository.board(target, project)).stages
                val request = OvermindRequests.addAsTask(project, prompt, destination, session, stages)
                // Disconnected while the board was read: nothing is sent.
                if (!connected(connectionId)) { done(); return@launch }
                val report: (OvermindOutcome?) -> Unit = { said -> notify(noticeKey, said) }
                val id = submit(connectionId, "create", request, attachments, { settled ->
                    done()
                    val card = runCatching { OvermindParser.card(JSONObject(settled.result)) }.getOrNull()
                    val ref = card?.ref?.ifBlank { null } ?: "the task"
                    val added = when (destination) {
                        OvermindDestination.Queue -> "Added $ref to this session's queue."
                        OvermindDestination.Pool -> "Added $ref to the project pool."
                        OvermindDestination.Backlog -> "Added $ref to ${card?.stage?.ifBlank { null } ?: "Backlog"}."
                    }
                    settle(connectionId, settled, OvermindWriteContext(ref, done = added), report) { if (it.status == "completed") onAdded(it) }
                }) { _ -> done(); dismissNotice(noticeKey) }
                if (id.isEmpty()) { done(); dismissNotice(noticeKey) }
            } catch (e: CancellationException) { done(); throw e }
            catch (e: Exception) { done(); notify(noticeKey, OvermindOutcome("The task was not added: ${message(e)}", OvermindTone.Error, connectionId = connectionId)) }
        }
    }

    /** A disconnected workspace keeps nothing here: boards, cards, images, drafts, messages and the open window. */
    fun purge(connectionId: String) {
        fun mine(key: Any) = when (key) {
            is OvermindBoardKey -> key.connectionId == connectionId
            is OvermindCardKey -> key.connectionId == connectionId
            is String -> key.startsWith("$connectionId\n")
            else -> false
        }
        reads.entries.filter { mine(it.key) }.forEach { it.value.cancel() }
        boards = boards.filterKeys { it.connectionId != connectionId }
        cards = cards.filterKeys { it.connectionId != connectionId }
        images = images.filterKeys { !it.startsWith("$connectionId\n") }
        drafts = drafts.filterKeys { it.connectionId != connectionId }
        again.removeAll(::mine)
        if (screen?.connectionId == connectionId) { screen = null; outcome = null }
        if (outcome?.connectionId == connectionId) outcome = null
        if (sendTo?.target?.connectionId == connectionId) closeChooser()
        val workspace = { key: String -> runCatching { JSONArray(key).optString(0) }.getOrNull() == connectionId }
        notices = notices.filterKeys { !workspace(it) }
        adding = adding.filterNot(workspace).toSet()
    }
}
