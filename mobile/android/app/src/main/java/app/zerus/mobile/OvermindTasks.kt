package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
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

/** Reads, writes and navigation of the Tasks screens. Reads refresh on demand or when the gateway digest moves past a board's
 * cursor; writes go through the outbox and are never repeated without a tap. Everything of a workspace is dropped on disconnect. */
class OvermindTasks(private val scope: CoroutineScope, private val repository: OvermindRepository,
    private val gateway: (String) -> OvermindGateway?,
    private val submit: (String, String, JSONObject, List<Attachment>, (OvermindWrite) -> Unit) -> String,
    private val retryWrite: (OvermindWrite, (OvermindWrite) -> Unit) -> Unit) {
    companion object {
        const val UNAVAILABLE = "Tasks are not available for this workspace."
        private const val IMAGE_BUDGET = 48L * 1024 * 1024
    }
    var boards by mutableStateOf<Map<OvermindBoardKey, OvermindBoardState>>(emptyMap()); private set
    var cards by mutableStateOf<Map<OvermindCardKey, OvermindCardState>>(emptyMap()); private set
    var images by mutableStateOf<Map<String, OvermindImageState>>(emptyMap()); private set
    var screen by mutableStateOf<OvermindScreen?>(null); private set
    /** The Tasks window's message bar. */
    var outcome by mutableStateOf<OvermindOutcome?>(null); private set
    /** Send a task here: the session chosen in its long-press sheet, and what the send said. */
    var sendTo by mutableStateOf<Session?>(null); private set
    var sendOutcome by mutableStateOf<OvermindOutcome?>(null); private set
    /** Add as task, per composer target key. */
    var notices by mutableStateOf<Map<String, OvermindOutcome>>(emptyMap()); private set
    private val reads = mutableMapOf<Any, Job>()
    private val again = mutableSetOf<OvermindCardKey>()

    private fun message(e: Exception) = e.message?.takeIf { it.isNotBlank() } ?: "The task board could not be read."
    private fun connected(connectionId: String) = gateway(connectionId) != null
    private fun updateBoard(key: OvermindBoardKey, change: (OvermindBoardState) -> OvermindBoardState) {
        if (connected(key.connectionId)) boards = boards + (key to change(boards[key] ?: OvermindBoardState()))
    }
    private fun updateCard(key: OvermindCardKey, change: (OvermindCardState) -> OvermindCardState) {
        if (connected(key.connectionId)) cards = cards + (key to change(cards[key] ?: OvermindCardState()))
    }

    fun board(connectionId: String, project: String) = boards[OvermindBoardKey(connectionId, project)]
    fun card(connectionId: String, task: String) = cards[OvermindCardKey(connectionId, task)]

    /** The cached board at once, then its changes from the gateway. Without [explicit], a fresh board is read again only once the
     * gateway digest ([rev]) moved past its cursor. One read per board at a time. */
    fun load(connectionId: String, project: String, rev: Long? = null, explicit: Boolean = false) {
        if (project.isEmpty()) return
        val key = OvermindBoardKey(connectionId, project)
        if (reads[key]?.isActive == true) return
        val current = boards[key]
        val board = current?.board
        if (!explicit && current?.fresh == true && board != null && current.error.isEmpty() && (rev == null || (board.cursor ?: -1) >= rev)) return
        val target = gateway(connectionId) ?: return
        updateBoard(key) { it.copy(loading = true) }
        reads[key] = scope.launch {
            try {
                val base = board ?: repository.cachedBoard(target, project)?.also { cached -> updateBoard(key) { it.copy(board = cached) } }
                val read = repository.board(target, project, base)
                updateBoard(key) { it.copy(board = read, loading = false, error = "", fresh = true) }
            } catch (e: CancellationException) { updateBoard(key) { it.copy(loading = false) }; throw e }
            catch (e: Exception) { updateBoard(key) { it.copy(loading = false, error = message(e)) } }
            finally { reads.remove(key) }
        }
    }

    /** Reads the full task; a request while one runs reads again after it, as the board entry may have changed meanwhile. */
    fun loadCard(connectionId: String, task: String) {
        if (task.isEmpty()) return
        val key = OvermindCardKey(connectionId, task)
        if (reads[key]?.isActive == true) { again += key; return }
        val target = gateway(connectionId) ?: return
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
        val target = gateway(connectionId) ?: return
        updateCard(key) { it.copy(older = true) }
        reads[key] = scope.launch {
            try { val longer = repository.olderJournal(target, view); updateCard(key) { it.copy(view = longer, older = false, error = "") } }
            catch (e: CancellationException) { updateCard(key) { it.copy(older = false) }; throw e }
            catch (e: Exception) { updateCard(key) { it.copy(older = false, error = message(e)) } }
            finally { reads.remove(key) }
        }
    }

    fun imageKey(connectionId: String, task: String, sha256: String) = "$connectionId\n$task\n$sha256"
    /** Reads an image in ranges and keeps it only when its bytes are a picture; an agent chose its name and type. Tapping a failed one reads it again. */
    fun image(connectionId: String, task: String, sha256: String, retry: Boolean = false) {
        val key = imageKey(connectionId, task, sha256)
        val current = images[key]
        if (current != null && !(retry && current.error.isNotEmpty())) return
        val target = gateway(connectionId) ?: return
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

    fun openBoard(connectionId: String, project: String) { screen = OvermindScreen(connectionId, project); outcome = null }
    fun openTask(connectionId: String, task: String, project: String = screen?.takeIf { it.connectionId == connectionId }?.project.orEmpty()) {
        screen = OvermindScreen(connectionId, project, task)
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
        return submit(connectionId, action, request, attachments) { settled -> settle(connectionId, settled, context, report, after) }
    }
    private fun settle(connectionId: String, settled: OvermindWrite, context: OvermindWriteContext, report: ((OvermindOutcome?) -> Unit)?, after: (OvermindWrite) -> Unit) {
        if (!connected(connectionId)) return
        val said = OvermindOutcomes.of(settled, context)
        // A delivery that may have arrived stays on show until another delivery, a problem or a dismissal.
        if (report != null) report(said)
        else if (said != null && (outcome?.resolve.isNullOrEmpty() || settled.action in OvermindAccess.deliveringActions || said.tone != OvermindTone.Info)) outcome = said
        after(settled)
        // The board and the card are read again: the compact card in the receipt lacks the board's markers and queues.
        val project = runCatching { JSONObject(settled.result).optString("project") }.getOrNull().orEmpty()
            .ifEmpty { screen?.takeIf { it.connectionId == connectionId }?.project.orEmpty() }
        if (project.isNotEmpty() && boards.containsKey(OvermindBoardKey(connectionId, project))) load(connectionId, project, explicit = true)
        if (settled.task.isNotEmpty() && cards.containsKey(OvermindCardKey(connectionId, settled.task)) && settled.action != "delete") loadCard(connectionId, settled.task)
    }
    /** "<action> anyway" is a new write: a new request_id with the refusal code confirmed. */
    fun anyway(connectionId: String, offer: OvermindAnyway, report: ((OvermindOutcome?) -> Unit)? = null) =
        write(connectionId, offer.action, offer.request, offer.context, report = report)
    /** Explicit only: the same request_id and payload, which the relay and hgs never apply twice. */
    fun retry(write: OvermindWrite, context: OvermindWriteContext, report: ((OvermindOutcome?) -> Unit)? = null, after: (OvermindWrite) -> Unit = {}) {
        retryWrite(write) { settled -> settle(write.connectionId, settled, context, report, after) }
    }

    /** Add as task: the workflow's first Ready stage comes from the board (cached or read now); the draft clears only once added. */
    fun addAsTask(noticeKey: String, connectionId: String, project: String, prompt: String, attachments: List<Attachment>,
        destination: OvermindDestination, session: OvermindSession?, onAdded: (OvermindWrite) -> Unit) {
        notices = notices + (noticeKey to OvermindOutcome("Adding the task…", OvermindTone.Info))
        scope.launch {
            try {
                val target = gateway(connectionId) ?: error(UNAVAILABLE)
                val stages = if (destination == OvermindDestination.Backlog) emptyList() else
                    (board(connectionId, project)?.board ?: repository.cachedBoard(target, project) ?: repository.board(target, project)).stages
                val request = OvermindRequests.addAsTask(project, prompt, destination, session, stages)
                val report: (OvermindOutcome?) -> Unit = { said -> notices = if (said == null) notices - noticeKey else notices + (noticeKey to said) }
                val id = submit(connectionId, "create", request, attachments) { settled ->
                    val card = runCatching { OvermindParser.card(JSONObject(settled.result)) }.getOrNull()
                    val ref = card?.ref?.ifBlank { null } ?: "the task"
                    val done = when (destination) {
                        OvermindDestination.Queue -> "Added $ref to this session's queue."
                        OvermindDestination.Pool -> "Added $ref to the project pool."
                        OvermindDestination.Backlog -> "Added $ref to ${card?.stage?.ifBlank { null } ?: "Backlog"}."
                    }
                    settle(connectionId, settled, OvermindWriteContext(ref, done = done), report) { if (it.status == "completed") onAdded(it) }
                }
                if (id.isEmpty()) notices = notices - noticeKey
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { notices = notices + (noticeKey to OvermindOutcome("The task was not added: ${message(e)}", OvermindTone.Error)) }
        }
    }

    /** A disconnected workspace keeps nothing here: boards, cards, images, messages and the open window. */
    fun purge(connectionId: String) {
        reads.entries.filter { (key, _) -> when (key) {
            is OvermindBoardKey -> key.connectionId == connectionId
            is OvermindCardKey -> key.connectionId == connectionId
            is String -> key.startsWith("$connectionId\n")
            else -> false
        } }.forEach { it.value.cancel() }
        boards = boards.filterKeys { it.connectionId != connectionId }
        cards = cards.filterKeys { it.connectionId != connectionId }
        images = images.filterKeys { !it.startsWith("$connectionId\n") }
        again.removeAll { it.connectionId == connectionId }
        if (screen?.connectionId == connectionId) { screen = null; outcome = null }
        if (sendTo?.target?.connectionId == connectionId) closeChooser()
        notices = notices.filterKeys { key -> runCatching { org.json.JSONArray(key).optString(0) }.getOrNull() != connectionId }
    }
}
