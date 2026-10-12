package app.zerus.mobile

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

/** One immutable Overmind change, saved before the network and settled only from its receipt. The request_id is the
 * relay identity and hgs `request_id` (the task UUID for `create`), so an explicit retry can never apply it twice. */
data class OvermindWrite(val requestId: String, val connectionId: String, val computerId: String, val action: String, val request: String,
    val attachments: List<Attachment> = emptyList(), val device: String = "", val status: String = "sending", val error: String = "",
    val code: String = "", val field: String = "", val delivery: String = "", val task: String = "", val result: String = "", val createdAt: Long = 0) {
    val gateway get() = MachineKey(connectionId, computerId)
    val pending get() = status in listOf("sending", "uncertain")
    /** The session may have received it: the person checks and resolves the delivery on the task, as on the desktop. */
    val needsResolution get() = status == "uncertain" && (delivery == "uncertain" || code == "delivery_unknown")
    /** The relay finished without a result and will not run it again: the person checks the task (for `create`, [task] is its UUID). */
    val needsCheck get() = status == "uncertain" && code == OvermindWrites.CHECK
    /** Only an unconfirmed transport is retried, by hand, with the same request_id and payload. */
    val retryable get() = status == "uncertain" && !needsResolution && !needsCheck
    fun recover() = if (status == "sending") copy(status = "uncertain", error = OvermindWrites.UNCONFIRMED) else this
}

object OvermindWrites {
    const val UNCONFIRMED = "The change was not confirmed. Retry sends the same request, which is never applied twice."
    const val RESOLVE = "The session may have received this. Check it, then resolve the delivery on the task."
    /** A phone-side code, never an hgs refusal: the outcome is unknown and final. */
    const val CHECK = "check_task"
    const val CHECK_TASK = "The change may have been applied, and it will not be sent again. Check the task before changing it again."
    const val UNSAVED = "The change may have been applied, but its result could not be saved on this phone. Check the task before changing it again."
    private const val MAX_REQUEST = 192 * 1024
    private const val MAX_CARD = 64 * 1024
    private const val KEEP = 20
    private val rejected = setOf(400, 401, 403, 404, 409, 413, 429)
    private val unrecorded = setOf(404, 409)

    fun create(requestId: String, gateway: MachineKey, action: String, request: JSONObject, attachments: List<Attachment>, device: String, createdAt: Long): OvermindWrite {
        require(OvermindDigests.canonical(requestId)) { "Invalid request identity." }
        require(action in OvermindAccess.writeActions) { "This task change is not supported." }
        require(attachments.isEmpty() || action in setOf("create", "edit")) { "Only new and edited tasks carry images." }
        AttachmentPolicy.selection(attachments)
        // The connector adds the envelope request_id; a different one in the request would fail before hgs.
        val body = JSONObject(request.toString()).apply { remove("request_id") }.toString()
        require(body.toByteArray(Charsets.UTF_8).size <= MAX_REQUEST) { "This change is too large to send from the phone." }
        val task = if (action == "create") requestId else JSONObject(body).string("task")
        return OvermindWrite(requestId, gateway.connectionId, gateway.computerId, action, body, attachments.toList(), OvermindDevice.label(device), task = task, createdAt = createdAt)
    }

    fun settle(write: OvermindWrite, receipt: JSONObject): OvermindWrite {
        if (receipt.string("request_id") != write.requestId) return write.copy(status = "uncertain", error = UNCONFIRMED)
        val result = receipt.optJSONObject("result")
        val delivering = write.action in OvermindAccess.deliveringActions
        return when (receipt.string("state")) {
            "completed" -> when (result?.opt("ok")) {
                true -> {
                    val card = result.optJSONObject("task")?.takeIf { it.toString().length <= MAX_CARD }
                    // `sent` or `uncertain`: hgs never answers ok with `not_sent`; an undelivered send is refused
                    // `not_delivered` instead (src/overmind/deliver.rs).
                    val delivery = result.opt("delivery") as? String ?: ""
                    val uncertain = delivering && delivery == "uncertain"
                    write.copy(status = if (uncertain) "uncertain" else "completed", delivery = delivery, code = "", field = "",
                        error = if (uncertain) RESOLVE else "",
                        task = card?.string("uuid")?.ifBlank { null } ?: (result.opt("deleted") as? String)?.ifBlank { null } ?: write.task,
                        result = card?.toString().orEmpty(), request = if (uncertain) write.request else "", attachments = if (uncertain) write.attachments else emptyList())
                }
                false -> {
                    val refusal = result.optJSONObject("error") ?: JSONObject()
                    val code = refusal.string("code")
                    write.copy(status = if (delivering && code == "delivery_unknown") "uncertain" else "refused", code = code, field = refusal.string("field"),
                        error = refusal.string("message").ifBlank { "The task board refused this change. Nothing was changed." })
                }
                // No boolean `ok`, or a result the relay no longer keeps ("request history expired"): terminal and unknown.
                else -> unknown(write)
            }
            "failed" -> write.copy(status = "failed", error = receipt.string("error").ifBlank { "The gateway could not run this change. Nothing was changed." })
            // The connector may have run it; the relay answers the same request_id with this receipt forever.
            "uncertain" -> unknown(write)
            // Still queued or claimed: sending the same request again only returns its receipt.
            else -> write.copy(status = "uncertain", error = UNCONFIRMED)
        }
    }
    private fun unknown(write: OvermindWrite) = write.copy(status = "uncertain", code = CHECK, field = "", delivery = "", error = CHECK_TASK)

    /** Whether the relay's answer to the submission itself proves it never recorded the write. A first submission refused with
     * 400/401/403/404/409/413/429 was not recorded. A retry's original may already be recorded. `Store::submit`
     * (services/mobile/relay/src/store/requests.rs) looks a known request_id up first and answers with its first receipt, or 409
     * for other content; only 404 (unknown computer) and 409 (identity or route conflict) come solely after that lookup, so only
     * they prove a retry was never recorded. Everything else on a retry stays unconfirmed: 400/408/413 while the relay reads the
     * body, 401/403 from authentication, 429 from per-client rate limiting or capacity (both before the lookup), and any 5xx.
     * Only the submission is classified: once the relay accepted it, an error while polling its receipt proves nothing. */
    fun refused(status: Int, retry: Boolean) = status in (if (retry) unrecorded else rejected)
    /** A failure after the request may have left the phone stays unconfirmed. Only a first submission becomes `failed` without
     * a receipt: before the network, or [refused] by the relay. A retry also fails when the relay [refused] it. */
    fun failure(write: OvermindWrite, error: Exception, attempted: Boolean, retry: Boolean = false, refused: Boolean = false): OvermindWrite = when {
        !attempted && !retry -> write.copy(status = "failed", error = error.message.orEmpty().ifBlank { "The change was not sent." })
        !attempted -> write.copy(status = "uncertain", error = listOf(error.message.orEmpty(), UNCONFIRMED).filter(String::isNotBlank).joinToString(" "))
        refused -> write.copy(status = "failed", error = error.message.orEmpty())
        else -> write.copy(status = "uncertain", error = UNCONFIRMED)
    }

    /** Settles only a write that is still pending, so a reviewed or settled record is never overwritten. */
    fun result(state: MessageState, requestId: String, update: (OvermindWrite) -> OvermindWrite): MessageState =
        state.overmindWrites.find { it.requestId == requestId && it.pending }?.let { state.overmind(update(it)) } ?: state
    /** A reviewed write is final and names no images any more, so their saved copies can be released. */
    fun review(state: MessageState, requestId: String): MessageState =
        state.overmindWrites.find { it.requestId == requestId && it.status != "reviewed" }?.let { state.overmind(it.copy(status = "reviewed", attachments = emptyList())) } ?: state
    /** A disconnected workspace keeps no writes. Returns the state without them and the saved images that only they named,
     * which can be released; a copy another workspace's draft or write still names is kept. */
    fun disconnect(state: MessageState, connectionId: String): Pair<MessageState, List<Attachment>> {
        val (dropped, kept) = state.overmindWrites.partition { it.connectionId == connectionId }
        val next = state.copy(overmindWrites = kept)
        val named = next.attachmentIds()
        return next to dropped.flatMap { it.attachments }.filter { it.id !in named }.distinctBy { it.id }
    }
    /** Pending writes stay; only the most recent settled ones are kept. */
    fun retain(writes: List<OvermindWrite>) = writes.filter { it.pending } + writes.filterNot { it.pending }.takeLast(KEEP)

    fun encode(write: OvermindWrite): JSONObject = JSONObject().put("request_id", write.requestId).put("connection", write.connectionId)
        .put("computer", write.computerId).put("action", write.action).put("request", write.request).put("attachments", JSONArray(write.attachments.map(Attachment::toJson)))
        .put("device", write.device).put("status", write.status).put("error", write.error).put("code", write.code).put("field", write.field)
        .put("delivery", write.delivery).put("task", write.task).put("result", write.result).put("created", write.createdAt)
    fun decode(raw: JSONObject) = OvermindWrite(raw.getString("request_id"), raw.getString("connection"), raw.getString("computer"), raw.getString("action"),
        raw.optString("request"), raw.optJSONArray("attachments")?.objects().orEmpty().map(Attachment::fromJson), raw.optString("device"),
        raw.getString("status"), raw.optString("error"), raw.optString("code"), raw.optString("field"), raw.optString("delivery"),
        raw.optString("task"), raw.optString("result"), raw.optLong("created")).recover()
}

/** The request may have reached the gateway, but its settled result could not be saved; the write stays unconfirmed. */
class OvermindUnsavedResult(cause: Throwable) : Exception(OvermindWrites.UNSAVED, cause)

/** Persists a write before the network and settles it from its receipt. It never retries on its own.
 * [connected] guards every reduction, so a write of a workspace disconnected meanwhile is dropped, never saved again. */
class OvermindOutbox(private val repository: OvermindRepository, private val wire: suspend (List<Attachment>) -> JSONArray,
    private val connected: (String) -> Boolean, private val durable: suspend ((MessageState) -> MessageState) -> MessageState) {
    private fun connectedOnly(state: MessageState, write: OvermindWrite) =
        if (connected(write.connectionId)) state else state.copy(overmindWrites = state.overmindWrites.filterNot { it.connectionId == write.connectionId })
    private fun requireConnected(write: OvermindWrite) = require(connected(write.connectionId)) { "This workspace was disconnected. The change was not sent." }
    suspend fun submit(gateway: OvermindGateway, write: OvermindWrite): OvermindWrite {
        require(write.status == "sending" && write.gateway == gateway.key) { "Review this change before sending it." }
        durable { state -> requireConnected(write); require(state.overmindWrites.none { it.requestId == write.requestId }) { "This request already exists." }
            state.overmind(write) }
        return send(gateway, write, retry = false)
    }
    /** Explicit only: the same request_id and payload, so the relay answers with the first receipt when it has one. */
    suspend fun retry(gateway: OvermindGateway, write: OvermindWrite): OvermindWrite {
        require(write.retryable && write.gateway == gateway.key) { "Only an unconfirmed change can be retried." }
        val sending = write.copy(status = "sending", error = "")
        durable { state -> requireConnected(write); require(state.overmindWrites.any { it.requestId == write.requestId && it.recover().retryable }) { "This change was already settled." }
            state.overmind(sending) }
        return send(gateway, sending, retry = true)
    }
    private suspend fun payload(write: OvermindWrite): JSONObject {
        val request = JSONObject(write.request)
        if (write.attachments.isNotEmpty()) {
            val files = request.optJSONArray("attachments") ?: JSONArray()
            wire(write.attachments).objects().forEach { files.put(it) }
            request.put("attachments", files)
        }
        return JSONObject().put("request_id", write.requestId).put("action", write.action).put("request", request)
            .also { if (write.device.isNotEmpty()) it.put("device", write.device) }
    }
    private suspend fun send(gateway: OvermindGateway, write: OvermindWrite, retry: Boolean): OvermindWrite {
        var attempted = false; var refused = false; var failure: Exception? = null
        val settled = try {
            val payload = payload(write)
            attempted = true
            val receipt = try { repository.submitWrite(gateway, write.requestId, payload) }
                catch (e: RelayException) { refused = OvermindWrites.refused(e.status, retry); throw e }
            // Accepted: the relay holds the write under its request_id, so a failed receipt poll leaves it unconfirmed.
            OvermindWrites.settle(write, repository.await(gateway, receipt))
        } catch (e: Exception) { failure = e; OvermindWrites.failure(write, e, attempted, retry, refused) }
        val saved = try {
            withContext(NonCancellable) { durable { state -> connectedOnly(OvermindWrites.result(state, write.requestId) { settled }, write) } }
        } catch (e: Exception) { if (attempted || retry) throw OvermindUnsavedResult(e) else throw e }
        if (failure is CancellationException) throw failure
        return saved.overmindWrites.find { it.requestId == write.requestId } ?: settled
    }
}
