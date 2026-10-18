package app.zerus.mobile

import org.json.JSONObject
import java.util.UUID

/** Current-process evidence only. A scheduled receipt never grants another automatic attempt by itself. */
internal class ScheduledSettingsRetry {
    sealed interface Proof {
        val original: SessionAction
        val child: SessionAction
        val model: String
        val effort: String
        val target get() = child.target
        val pendingId get() = child.pendingId
        val originalArguments get() = original.arguments
        val childArguments get() = child.arguments
    }
    sealed interface Grant : Proof
    sealed interface ConsumedGrant : Proof {
        val grant: Grant
    }

    private class ReadyGrant(
        override val original: SessionAction,
        override val child: SessionAction,
        override val model: String,
        override val effort: String,
    ) : Grant
    private class UsedGrant(override val grant: Grant) : ConsumedGrant {
        override val original = grant.original
        override val child = grant.child
        override val model = grant.model
        override val effort = grant.effort
    }
    private class Record(
        val original: SessionAction,
        val child: SessionAction,
        val model: String,
        val effort: String,
        val ackNanos: Long,
    ) {
        var lastObservationNanos = ackNanos
        var leftReady = false
        var issued = false
        var consumed = false
        var grant: ReadyGrant? = null
    }
    private val records = linkedMapOf<String, Record>()

    /** Called only after this process's submitted child and its scheduled receipt are durable. No restore path. */
    fun register(original: SessionAction, scheduledChild: SessionAction, ackNanos: Long, scheduledResult: JSONObject): Boolean {
        val child = scheduledChild
        val target = child.target
        if (ackNanos <= 0 || original.operation != "settings" || child.operation != "settings" ||
            original.status != "scheduled" || child.status != "scheduled" ||
            original.requestId.isBlank() || child.requestId.isBlank() || original.requestId == child.requestId ||
            original.applyAttemptId != child.requestId || child.applyAttemptId.isNotBlank() ||
            original.target != target || !liveTarget(target) ||
            original.resultTarget?.let { it != target } == true || child.resultTarget?.let { it != target } == true ||
            !uuid(original.pendingId) || child.pendingId != original.pendingId) return false
        val requested = arguments(original.arguments, false) ?: return false
        val attempted = arguments(child.arguments, true) ?: return false
        if (attempted.text("expected_pending_id") != original.pendingId) return false
        if (scheduledResult.text("request_id") != child.requestId || scheduledResult.text("status") != "scheduled" ||
            scheduledResult.text("name") != target.session || scheduledResult.text("run_id") != target.run ||
            scheduledResult.text("conversation_id") != target.conversation ||
            !emptyOptional(scheduledResult, "archive_id") || !emptyOptional(scheduledResult, "agent_id") ||
            !emptyOptional(scheduledResult, "parent_conversation_id") || scheduledResult.text("apply_when") != "ready" ||
            scheduledResult.text("pending_settings_id") != original.pendingId) return false
        val model = scheduledResult.text("model")?.takeIf { it.isNotBlank() && it.length <= 120 && it.none(Char::isISOControl) } ?: return false
        val effort = scheduledResult.text("effort")?.takeIf { it.length <= 128 && it.none(Char::isISOControl) } ?: return false
        for ((field, value) in listOf("model" to model, "effort" to effort)) {
            if (requested.has(field) && (!attempted.has(field) || requested.text(field) != value)) return false
            if (attempted.has(field) && attempted.text(field) != value) return false
        }
        val previous = records[original.requestId]
        if (previous != null && (!previous.consumed || previous.child.requestId == child.requestId ||
                ackNanos <= previous.lastObservationNanos || previous.original.target != original.target ||
                previous.original.arguments != original.arguments || previous.original.pendingId != original.pendingId ||
                previous.model != model || previous.effort != effort)) return false
        if (records.values.any { it.child.requestId == child.requestId }) return false
        records.remove(original.requestId)
        records[original.requestId] = Record(original, child, model, effort, ackNanos)
        while (records.size > 64) records.remove(records.keys.first())
        return true
    }

    /** A persisted scheduled action without a registration from this process is not a retry candidate. */
    fun tracks(original: SessionAction): Boolean = records[original.requestId]?.let {
        !it.consumed && it.original == original && original.applyAttemptId == it.child.requestId
    } == true

    /** Only fresh, sequential network inspections may observe leaving Ready and returning to it. */
    fun inspect(
        original: SessionAction,
        scheduledChild: SessionAction,
        target: Target,
        raw: JSONObject,
        requestStartNanos: Long,
        nowNanos: Long,
        verified: Boolean,
        online: Boolean,
    ): Grant? {
        val record = records[original.requestId] ?: return null
        if (record.original != original || record.child != scheduledChild || original.applyAttemptId != scheduledChild.requestId) {
            records.remove(original.requestId)
            return null
        }
        if (record.consumed || record.issued || target != record.child.target || !verified || !online ||
            requestStartNanos <= record.ackNanos || requestStartNanos <= record.lastObservationNanos ||
            nowNanos < requestStartNanos || nowNanos - requestStartNanos > 10_000_000_000L) return null
        if (raw.text("name") != target.session || raw.text("run_id") != target.run ||
            raw.text("conversation_id") != target.conversation || !emptyOptional(raw, "archive_id") ||
            !emptyOptional(raw, "agent_id") || !emptyOptional(raw, "parent_conversation_id") ||
            raw.text("pending_settings_id") != record.child.pendingId ||
            raw.text("pending_model") != record.model || raw.text("pending_effort") != record.effort ||
            raw.opt("settings_change_supported") != true) {
            records.remove(original.requestId)
            return null
        }
        record.lastObservationNanos = nowNanos
        when (raw.text("settings_apply_when")) {
            // A terminal chooser can make this ready even while phase/activity still both say idle.
            "ready" -> record.leftReady = true
            "now" -> if (raw.text("phase") == "idle" && raw.text("activity") == "idle" && record.leftReady) {
                record.issued = true
                return ReadyGrant(record.original, record.child, record.model, record.effort).also { record.grant = it }
            }
            else -> {
                records.remove(original.requestId)
                return null
            }
        }
        return null
    }

    /** A blocked preflight loses this Ready grant; another distinct NotReady/Ready is required. */
    fun discard(grant: Grant): Boolean {
        val record = records[grant.original.requestId] ?: return false
        if (record.consumed || record.grant !== grant) return false
        record.grant = null
        record.issued = false
        record.leftReady = false
        return true
    }

    /** Consume immediately after the caller's fresh preflight and before its durable original/child CAS. */
    fun consume(grant: Grant): ConsumedGrant? {
        val record = records[grant.original.requestId] ?: return null
        if (record.consumed || record.grant !== grant) return null
        record.grant = null
        record.consumed = true
        return UsedGrant(grant)
    }

    private fun JSONObject.text(field: String): String? = opt(field) as? String
    private fun emptyOptional(raw: JSONObject, field: String) = !raw.has(field) || raw.text(field) == ""
    private fun uuid(value: String) = runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)
    private fun liveTarget(target: Target) = listOf(target.connectionId, target.computerId, target.session, target.run, target.conversation).all(String::isNotBlank) &&
        target.agentId.isBlank() && target.archiveId.isBlank() && target.parentConversation.isBlank()
    private fun arguments(encoded: String, child: Boolean): JSONObject? = runCatching {
        JSONObject(encoded).takeIf { args ->
            val allowed = if (child) setOf("model", "effort", "expected_pending_id") else setOf("model", "effort")
            args.keys().asSequence().all { it in allowed } && listOf("model", "effort").any(args::has) &&
                listOf("model", "effort").all { !args.has(it) || args.opt(it) is String } &&
                (!child || args.opt("expected_pending_id") is String)
        }
    }.getOrNull()
}
