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
    /** Only an unconfirmed transport is retried, by hand, with the same request_id and payload. */
    val retryable get() = status == "uncertain" && !needsResolution
    fun recover() = if (status == "sending") copy(status = "uncertain", error = OvermindWrites.UNCONFIRMED) else this
}

object OvermindWrites {
    const val UNCONFIRMED = "The change was not confirmed. Retry sends the same request, which is never applied twice."
    const val RESOLVE = "The session may have received this. Check it, then resolve the delivery on the task."
    private const val MAX_REQUEST = 192 * 1024
    private const val MAX_CARD = 64 * 1024
    private const val KEEP = 20
    private val rejected = setOf(400, 401, 403, 404, 409, 413, 429)

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
                    val delivery = result.opt("delivery") as? String ?: ""
                    val reason = result.opt("reason") as? String ?: ""
                    val uncertain = delivering && delivery == "uncertain"
                    write.copy(status = if (uncertain) "uncertain" else "completed", delivery = delivery, code = "", field = "",
                        error = when { uncertain -> RESOLVE; delivery == "not_sent" -> reason.ifBlank { "Saved, but not sent to the session." }; else -> "" },
                        task = card?.string("uuid")?.ifBlank { null } ?: (result.opt("deleted") as? String)?.ifBlank { null } ?: write.task,
                        result = card?.toString().orEmpty(), request = if (uncertain) write.request else "", attachments = if (uncertain) write.attachments else emptyList())
                }
                false -> {
                    val refusal = result.optJSONObject("error") ?: JSONObject()
                    val code = refusal.string("code")
                    write.copy(status = if (delivering && code == "delivery_unknown") "uncertain" else "refused", code = code, field = refusal.string("field"),
                        error = refusal.string("message").ifBlank { "The task board refused this change. Nothing was changed." })
                }
                else -> write.copy(status = "uncertain", error = UNCONFIRMED)
            }
            "failed" -> write.copy(status = "failed", error = receipt.string("error").ifBlank { "The gateway could not run this change. Nothing was changed." })
            else -> write.copy(status = "uncertain", error = UNCONFIRMED)
        }
    }

    /** A transport failure after the request may have left the phone stays unconfirmed; a refusal at submission is final. */
    fun failure(write: OvermindWrite, error: Exception, attempted: Boolean): OvermindWrite = when {
        !attempted -> write.copy(status = "failed", error = error.message.orEmpty().ifBlank { "The change was not sent." })
        error is RelayException && error.status in rejected -> write.copy(status = "failed", error = error.message.orEmpty())
        else -> write.copy(status = "uncertain", error = UNCONFIRMED)
    }

    /** Settles only a write that is still pending, so a reviewed or settled record is never overwritten. */
    fun result(state: MessageState, requestId: String, update: (OvermindWrite) -> OvermindWrite): MessageState =
        state.overmindWrites.find { it.requestId == requestId && it.pending }?.let { state.overmind(update(it)) } ?: state
    fun review(state: MessageState, requestId: String): MessageState =
        state.overmindWrites.find { it.requestId == requestId && it.status != "reviewed" }?.let { state.overmind(it.copy(status = "reviewed")) } ?: state
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

/** Persists a write before the network and settles it from its receipt. It never retries on its own. */
class OvermindOutbox(private val repository: OvermindRepository, private val wire: suspend (List<Attachment>) -> JSONArray,
    private val durable: suspend ((MessageState) -> MessageState) -> MessageState) {
    suspend fun submit(gateway: OvermindGateway, write: OvermindWrite): OvermindWrite {
        require(write.status == "sending" && write.gateway == gateway.key) { "Review this change before sending it." }
        durable { state -> require(state.overmindWrites.none { it.requestId == write.requestId }) { "This request already exists." }; state.overmind(write) }
        return send(gateway, write)
    }
    /** Explicit only: the same request_id and payload, so the relay answers with the first receipt when it has one. */
    suspend fun retry(gateway: OvermindGateway, write: OvermindWrite): OvermindWrite {
        require(write.retryable && write.gateway == gateway.key) { "Only an unconfirmed change can be retried." }
        val sending = write.copy(status = "sending", error = "")
        durable { state -> require(state.overmindWrites.any { it.requestId == write.requestId && it.recover().retryable }) { "This change was already settled." }
            state.overmind(sending) }
        return send(gateway, sending)
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
    private suspend fun send(gateway: OvermindGateway, write: OvermindWrite): OvermindWrite {
        var attempted = false; var failure: Exception? = null
        val settled = try {
            val payload = payload(write)
            attempted = true
            OvermindWrites.settle(write, repository.await(gateway, repository.submitWrite(gateway, write.requestId, payload)))
        } catch (e: Exception) { failure = e; OvermindWrites.failure(write, e, attempted) }
        val saved = withContext(NonCancellable) { durable { state -> OvermindWrites.result(state, write.requestId) { settled } } }
        if (failure is CancellationException) throw failure
        return saved.overmindWrites.find { it.requestId == write.requestId } ?: settled
    }
}
