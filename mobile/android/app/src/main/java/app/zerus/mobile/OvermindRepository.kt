package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID

/** Overmind is served by the workspace gateway's own connector: two operations and no session. */
object OvermindAccess {
    const val READ = "overmind_read"
    const val WRITE = "overmind_write"
    val operations = setOf(READ, WRITE)
    val readActions = setOf("projects", "board", "task", "decisions", "attachment")
    val writeActions = setOf("create", "edit", "move", "reorder", "pin", "unpin", "label", "link", "comment", "send", "review-send",
        "rework-send", "answer", "resolve-delivery", "done", "reopen", "delete")
    /** Writes whose delivery to a session can stay unknown; they are resolved on the task, never repeated. */
    val deliveringActions = setOf("send", "review-send", "rework-send", "answer", "resolve-delivery")
    fun supports(operations: Set<String>?) = operations != null && operations.containsAll(this.operations)
    fun snapshotOperations(computer: JSONObject): Set<String> = computer.optJSONObject("snapshot")?.optJSONObject("mobile_capabilities")
        ?.takeIf { it.optInt("protocol_version") == 1 }?.optJSONArray("operations")?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String }.toSet() }.orEmpty()
    /** Tasks are shown only while the relay and one connected gateway both list both operations; otherwise they are hidden, not failing. */
    fun gateway(connectionId: String, machines: List<Machine>, relay: Set<String>?, machine: (MachineKey) -> Set<String>?): Machine? {
        if (!supports(relay)) return null
        return machines.filter { it.connectionId == connectionId && !it.lastKnown && supports(machine(MachineKey(connectionId, it.id))) }
            .sortedWith(preference).firstOrNull()
    }
    /** Online first, then a direct route, then the machine id; decision cards use the same choice. */
    val preference: Comparator<Machine> = compareBy({ !it.online }, { it.route != MachineRoute.Direct }, { it.id })
}

/** hgs and the relay refuse controls and a leading '-' or '@'. The label only says which client acted, so it is cleaned, never refused. */
object OvermindDevice {
    private fun unsafe(point: Int) = Character.getType(point).let { it == Character.CONTROL.toInt() || it == Character.SURROGATE.toInt() }
    fun label(raw: String?): String {
        val kept = StringBuilder()
        raw.orEmpty().codePoints().forEach { if (!unsafe(it)) kept.appendCodePoint(it) }
        val start = kept.toString().trimStart { it.isWhitespace() || it == '-' || it == '@' }
        val cut = if (start.codePointCount(0, start.length) > 64) start.substring(0, start.offsetByCodePoints(0, 64)) else start
        return cut.trimEnd()
    }
    fun valid(label: String) = label.codePointCount(0, label.length) in 1..64 && label.codePoints().noneMatch { unsafe(it) } &&
        !label.startsWith("-") && !label.startsWith("@")
}

data class OvermindGateway(val connection: Connection, val computerId: String) { val key get() = MachineKey(connection.id, computerId) }
/** A task named by a decision card: its workspace, the gateway that announced it and the task UUID. */
data class OvermindTaskKey(val connectionId: String, val computerId: String, val task: String)
/** hgs refused the request; nothing was changed. */
class OvermindRefusal(val code: String, message: String, val field: String = "", val state: String = "") : Exception(message)

data class OvermindSession(val machine: String, val name: String)
data class OvermindStage(val name: String, val role: String)
/** A board summary or a write's compact card. `raw` keeps hgs fields this app does not read yet. */
data class OvermindCard(val uuid: String, val ref: String, val project: String, val title: String, val stage: String, val priority: Int,
    val rank: String, val labels: List<String>, val pinned: OvermindSession?, val assignee: OvermindSession?, val reviewer: OvermindSession?,
    val flags: List<String>, val markers: List<String>, val parent: String, val follows: List<String>, val createdAt: Long, val updatedAt: Long,
    val readOnly: Boolean, val deleted: Boolean, val journalCount: Int, val pinnedCurrent: String, val queueOf: OvermindSession?, val raw: String)
data class OvermindBoard(val project: String, val cursor: Long?, val stages: List<OvermindStage>, val cards: List<OvermindCard>, val workflow: String)
data class OvermindJournalEntry(val marker: String, val text: String, val at: Long, val event: String, val actorKind: String,
    val actorMachine: String, val actorSession: String, val actorDevice: String)
data class OvermindImageRef(val name: String, val mime: String, val bytes: Long, val sha256: String)
/** A full task with one window of its journal: `journal` starts at index `journalFrom` of `journalTotal` entries. */
data class OvermindTaskView(val card: OvermindCard, val prompt: String, val attachments: List<OvermindImageRef>, val journal: List<OvermindJournalEntry>,
    val journalTotal: Int, val journalFrom: Int, val head: String, val raw: String)
data class OvermindProject(val project: String, val total: Long, val roles: Map<String, Long>)
data class OvermindProjects(val cursor: Long?, val projects: List<OvermindProject>)
data class OvermindDecision(val task: String, val ref: String, val project: String, val title: String, val stage: String,
    val question: String, val assignee: OvermindSession?, val signature: String)
class OvermindImage(val name: String, val mime: String, val bytes: ByteArray)

object OvermindParser {
    private fun text(raw: JSONObject, key: String) = raw.opt(key) as? String ?: ""
    private fun strings(raw: JSONObject, key: String) = (raw.opt(key) as? JSONArray)?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String } }.orEmpty()
    fun count(value: Any?): Long? = (value as? Number)?.toDouble()?.takeIf { it.isFinite() && it >= 0 && it % 1.0 == 0.0 && it <= 9_007_199_254_740_991.0 }?.toLong()
    private fun session(value: Any?) = (value as? JSONObject)?.let { raw -> text(raw, "name").takeIf { it.isNotEmpty() }?.let { OvermindSession(text(raw, "machine"), it) } }
    fun card(raw: JSONObject): OvermindCard? {
        val uuid = text(raw, "uuid").takeIf { it.isNotBlank() && it.length <= 64 } ?: return null
        return OvermindCard(uuid, text(raw, "ref"), text(raw, "project"), text(raw, "title"), text(raw, "stage"), count(raw.opt("priority"))?.toInt() ?: 0,
            text(raw, "rank"), strings(raw, "labels"), session(raw.opt("pinned")), session(raw.opt("assignee")), session(raw.opt("reviewer")),
            strings(raw, "flags"), strings(raw, "markers"), text(raw, "parent"), strings(raw, "follows"), count(raw.opt("created_at")) ?: 0,
            count(raw.opt("updated_at")) ?: 0, raw.opt("read_only") == true, raw.opt("deleted") == true, count(raw.opt("journal_count"))?.toInt() ?: 0,
            text(raw, "pinned_current"), session(raw.opt("queue_of")), raw.toString())
    }
    fun stages(workflow: JSONObject?): List<OvermindStage> = workflow?.optJSONArray("stages")?.objects().orEmpty()
        .mapNotNull { stage -> text(stage, "name").takeIf { it.isNotEmpty() }?.let { OvermindStage(it, text(stage, "role")) } }
    /** Board order: stage position, then rank, then UUID, as hgs pages it. */
    fun board(project: String, cursor: Long?, workflow: String, cards: Collection<OvermindCard>): OvermindBoard {
        val stages = stages(runCatching { JSONObject(workflow) }.getOrNull())
        val position = stages.withIndex().associate { it.value.name to it.index }
        return OvermindBoard(project, cursor, stages, cards.filterNot { it.deleted }
            .sortedWith(compareBy<OvermindCard>({ position[it.stage] ?: Int.MAX_VALUE }, { it.rank }, { it.uuid })), workflow)
    }
    fun boardJson(board: OvermindBoard): JSONObject = JSONObject().put("project", board.project).put("cursor", board.cursor ?: JSONObject.NULL)
        .put("workflow", JSONObject(board.workflow)).put("tasks", JSONArray(board.cards.map { JSONObject(it.raw) }))
    fun board(raw: JSONObject): OvermindBoard? = runCatching {
        board(raw.getString("project"), count(raw.opt("cursor")), raw.getJSONObject("workflow").toString(), raw.getJSONArray("tasks").objects().mapNotNull(::card))
    }.getOrNull()
    fun task(page: JSONObject): OvermindTaskView {
        val raw = page.getJSONObject("task")
        val card = checkNotNull(card(raw)) { "The task board returned a task without its identity." }
        val journal = raw.optJSONArray("journal")?.objects().orEmpty().map { entry ->
            val actor = entry.optJSONObject("actor") ?: JSONObject()
            OvermindJournalEntry(text(entry, "marker"), text(entry, "text"), count(entry.opt("at")) ?: 0, text(entry, "event"),
                text(actor, "kind"), text(actor, "machine"), text(actor, "session"), text(actor, "device"))
        }
        val total = count(raw.opt("journal_total"))?.toInt() ?: journal.size
        val from = count(raw.opt("journal_from"))?.toInt() ?: (total - journal.size).coerceAtLeast(0)
        return OvermindTaskView(card, text(raw, "prompt"), raw.optJSONArray("attachments")?.objects().orEmpty().map {
            OvermindImageRef(text(it, "name"), text(it, "mime"), count(it.opt("bytes")) ?: 0, text(it, "sha256"))
        }, journal, total, from, text(raw, "head"), raw.toString())
    }
    fun projects(page: JSONObject) = OvermindProjects(count(page.opt("cursor")), page.optJSONArray("projects")?.objects().orEmpty().mapNotNull { raw ->
        val project = raw.opt("project") as? String ?: return@mapNotNull null
        val roles = raw.optJSONObject("roles")
        OvermindProject(project, count(raw.opt("total")) ?: 0, roles?.keys()?.asSequence()?.mapNotNull { role -> count(roles.opt(role))?.let { role to it } }?.toMap().orEmpty())
    })
    fun decisions(page: JSONObject): List<OvermindDecision> = page.optJSONArray("decisions")?.objects().orEmpty().mapNotNull { raw ->
        val task = text(raw, "task").takeIf { it.isNotBlank() } ?: return@mapNotNull null
        OvermindDecision(task, text(raw, "ref"), text(raw, "project"), text(raw, "title"), text(raw, "stage"), text(raw, "question"),
            session(raw.opt("assignee")), text(raw, "signature"))
    }
}

/** Reads go to the gateway with a fresh request each time. The cache only presents; it never verifies a write. */
class OvermindRepository(private val api: RelayApi, private val cache: OvermindCache? = null,
    private val polls: Int = 35, private val pollDelayMs: Long = 800) {
    companion object {
        const val RANGE = 524_288
        const val MAX_IMAGE = 10_485_760L
        private const val MAX_BOARD_PAGES = 100
    }
    suspend fun read(gateway: OvermindGateway, action: String, request: JSONObject): JSONObject {
        require(action in OvermindAccess.readActions) { "Unsupported task read." }
        val id = UUID.randomUUID().toString()
        val receipt = await(gateway, api.submitGateway(gateway.connection, gateway.computerId, OvermindAccess.READ,
            JSONObject().put("action", action).put("request", request), id))
        return when (receipt.string("state")) {
            "completed" -> {
                val result = receipt.optJSONObject("result") ?: error("The task board returned no answer.")
                when (result.opt("ok")) {
                    true -> result
                    false -> { val refusal = result.optJSONObject("error") ?: JSONObject()
                        throw OvermindRefusal(refusal.string("code"), refusal.string("message").ifBlank { "The task board refused this request." }, refusal.string("field"), refusal.string("state")) }
                    else -> error("The task board returned an invalid answer.")
                }
            }
            "failed" -> error(receipt.string("error").ifBlank { "The task board could not answer." })
            else -> error("The task board has not answered yet. Try again.")
        }
    }
    suspend fun submitWrite(gateway: OvermindGateway, requestId: String, payload: JSONObject) =
        api.submitGateway(gateway.connection, gateway.computerId, OvermindAccess.WRITE, payload, requestId)
    suspend fun await(gateway: OvermindGateway, receipt: JSONObject) = api.await(gateway.connection, receipt, polls, pollDelayMs)

    private suspend fun remember(gateway: OvermindGateway, kind: String, id: String, value: JSONObject) {
        try { cache?.put(gateway.key, kind, id, value) } catch (e: kotlinx.coroutines.CancellationException) { throw e } catch (_: Exception) {}
    }
    private suspend fun cached(gateway: OvermindGateway, kind: String, id: String): JSONObject? =
        try { cache?.get(gateway.key, kind, id) } catch (e: kotlinx.coroutines.CancellationException) { throw e } catch (_: Exception) { null }
    suspend fun cachedProjects(gateway: OvermindGateway) = cached(gateway, "projects", "")?.let { runCatching { OvermindParser.projects(it) }.getOrNull() }
    suspend fun cachedBoard(gateway: OvermindGateway, project: String) = cached(gateway, "board", project)?.let { OvermindParser.board(it) }?.takeIf { it.project == project }
    suspend fun cachedTask(gateway: OvermindGateway, task: String) = cached(gateway, "task", task)?.let { runCatching { OvermindParser.task(it) }.getOrNull() }
    suspend fun cachedDecisions(gateway: OvermindGateway) = cached(gateway, "decisions", "")?.let { runCatching { OvermindParser.decisions(it) }.getOrNull() }
    suspend fun purgeWorkspace(connectionId: String) { cache?.purgeWorkspace(connectionId) }

    suspend fun projects(gateway: OvermindGateway): OvermindProjects =
        read(gateway, "projects", JSONObject()).let { page -> OvermindParser.projects(page).also { remember(gateway, "projects", "", page) } }
    suspend fun decisions(gateway: OvermindGateway): List<OvermindDecision> =
        read(gateway, "decisions", JSONObject()).let { page -> OvermindParser.decisions(page).also { remember(gateway, "decisions", "", page) } }

    private class Pages(val cursor: Long?, val workflow: String?, val cards: List<OvermindCard>)
    /** One walk with `after` until `next` is null. The global cursor moves constantly, so a change between pages is not a restart. */
    private suspend fun pages(gateway: OvermindGateway, project: String, since: Long?): Pages {
        var after: String? = null; var cursor: Long? = null; var workflow: String? = null
        val cards = mutableListOf<OvermindCard>()
        repeat(MAX_BOARD_PAGES) { index ->
            val page = read(gateway, "board", JSONObject().put("project", project).also { request ->
                since?.let { request.put("since", it) }; after?.let { request.put("after", it) } })
            check(page.opt("project") == project) { "The task board answered for another project." }
            if (index == 0) cursor = OvermindParser.count(page.opt("cursor"))
            page.optJSONObject("workflow")?.let { workflow = it.toString() }
            page.optJSONArray("tasks")?.objects().orEmpty().mapNotNullTo(cards, OvermindParser::card)
            val next = page.opt("next") as? String
            if (next.isNullOrEmpty()) return Pages(cursor, workflow, cards)
            check(next != after) { "The task board repeated a page." }
            after = next
        }
        error("This board has too many tasks to show on the phone.")
    }
    private fun merge(base: OvermindBoard, changes: Pages): OvermindBoard {
        val cards = LinkedHashMap<String, OvermindCard>()
        base.cards.forEach { cards[it.uuid] = it }
        changes.cards.forEach { if (it.deleted) cards.remove(it.uuid) else cards[it.uuid] = it }
        return OvermindParser.board(base.project, changes.cursor ?: base.cursor, changes.workflow ?: base.workflow, cards.values)
    }
    /** A full walk is reconciled with the changes since its first page; a cached board only asks for those changes. */
    suspend fun board(gateway: OvermindGateway, project: String, previous: OvermindBoard? = null): OvermindBoard {
        val known = previous?.takeIf { it.project == project && it.cursor != null }
        var board = if (known != null) {
            val changes = pages(gateway, project, known.cursor)
            if (changes.cursor != null && changes.cursor < known.cursor!!) null else merge(known, changes)
        } else null
        if (board == null) {
            val full = pages(gateway, project, null)
            val base = merge(OvermindParser.board(project, full.cursor, full.workflow ?: "{}", emptyList()), full)
            board = if (full.cursor == null) base else merge(base, pages(gateway, project, full.cursor))
        }
        remember(gateway, "board", project, OvermindParser.boardJson(board))
        return board
    }

    suspend fun task(gateway: OvermindGateway, task: String): OvermindTaskView {
        require(task.isNotBlank() && task.length <= 256) { "Choose a task." }
        val page = read(gateway, "task", JSONObject().put("task", task))
        return OvermindParser.task(page).also { remember(gateway, "task", it.card.uuid, page) }
    }
    /** The next older journal window. A different total means late-synced events shifted the indices: restart from the newest page. */
    suspend fun olderJournal(gateway: OvermindGateway, view: OvermindTaskView): OvermindTaskView {
        if (view.journalFrom <= 0) return view
        val older = OvermindParser.task(read(gateway, "task", JSONObject().put("task", view.card.uuid).put("journal_before", view.journalFrom)))
        check(older.card.uuid == view.card.uuid) { "The task board returned another task." }
        if (older.journalTotal != view.journalTotal) return task(gateway, view.card.uuid)
        return older.copy(journal = older.journal + view.journal)
    }

    /** Reads an image in ranges of at most 512 KiB and checks it against its content address. */
    suspend fun attachment(gateway: OvermindGateway, task: String, sha256: String): OvermindImage {
        require(task.isNotBlank() && sha256.matches(Regex("[0-9a-f]{64}"))) { "This image reference is invalid." }
        val bytes = ByteArrayOutputStream()
        var size: Long? = null; var name = ""; var mime = ""
        while (size == null || bytes.size() < size) {
            val offset = bytes.size().toLong()
            val length = minOf(RANGE.toLong(), (size ?: MAX_IMAGE) - offset, MAX_IMAGE - offset)
            check(length > 0) { "This image is larger than the phone can show." }
            val range = read(gateway, "attachment", JSONObject().put("task", task).put("sha256", sha256).put("offset", offset).put("length", length))
            val reported = OvermindParser.count(range.opt("size"))
            check(range.opt("sha256") == sha256 && OvermindParser.count(range.opt("offset")) == offset && reported != null && reported in 1..MAX_IMAGE &&
                (size == null || size == reported)) { "The image changed while it was loading." }
            val data = Base64.getDecoder().decode(range.string("data_base64"))
            check(data.isNotEmpty() && data.size.toLong() <= length && OvermindParser.count(range.opt("length")) == data.size.toLong()) { "The image changed while it was loading." }
            size = reported; name = range.string("name"); mime = range.string("mime")
            bytes.write(data)
        }
        val content = bytes.toByteArray()
        val digest = MessageDigest.getInstance("SHA-256").digest(content).joinToString("") { "%02x".format(it) }
        check(content.size.toLong() == size && digest == sha256) { "The image changed while it was loading." }
        return OvermindImage(name, mime, content)
    }
}
