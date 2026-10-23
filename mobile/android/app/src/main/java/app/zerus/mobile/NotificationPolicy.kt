package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

/** Optional: only pending optional questions, which the agent keeps working without. */
enum class SessionAlertKind { Input, Error, Finished, Optional }
data class NotificationPreferences(val input: Boolean = true, val errors: Boolean = true,
    val finished: Boolean = false, val generation: Long = 0, val master: Boolean = true,
    val inputEnabledAt: Long = 0, val errorsEnabledAt: Long = 0, val finishedEnabledAt: Long = 0,
    val optional: Boolean = false, val optionalEnabledAt: Long = 0) {
    fun enabledAt(kind: SessionAlertKind) = when (kind) {
        SessionAlertKind.Input -> inputEnabledAt
        SessionAlertKind.Error -> errorsEnabledAt
        SessionAlertKind.Finished -> finishedEnabledAt
        SessionAlertKind.Optional -> optionalEnabledAt
    }
    fun allows(kind: SessionAlertKind) = master && when (kind) {
        SessionAlertKind.Input -> input
        SessionAlertKind.Error -> errors
        SessionAlertKind.Finished -> finished
        SessionAlertKind.Optional -> optional
    }
}
data class NotificationSlot(val connection: String, val computer: String, val session: String) {
    val key get() = JSONArray(listOf(connection, computer, session)).toString()
    val tag get() = "zerus:session:v2:$key"
}
data class NotificationEvent(val id: Long, val computer: String, val session: String, val kind: String, val at: Double,
    val alertKind: String? = null, val nativeReplyId:String? = null) {
    /** The transition this row announces; relay alert rows carry it in alert_kind. */
    val signal get() = if (kind == "alert") alertKind.orEmpty() else kind
    val isAlert get() = kind == "alert" && alertKind != null
}
/** optional: sorted IDs of pending optional questions in the session's conversation. */
data class NotificationCurrent(val slot: NotificationSlot, val target: String, val title: String,
    val machine: String, val kind: SessionAlertKind?, val identity: String, val idle: Boolean,
    val online: Boolean = true, val live: Boolean = true, val conversation: String = "", val optional: List<String> = emptyList(), val suppressed: Boolean = false,
    val notificationPreference:SessionNotificationPreference?=null)
data class NotificationRecord(val slot: NotificationSlot, val target: String, val observedAt: Double,
    val observedFingerprint: String = "", val postedFingerprint: String = "",
    val postedKind: SessionAlertKind? = null, val postedTarget: String = "", val eventId: Long = 0, val eventAt: Double = 0.0,
    val summaryOnly: Boolean = false, val candidateFingerprint: String = "",
    val candidateTarget: String = "", val candidateEventId: Long = 0, val candidateEventAt: Double = 0.0, val restoredAt: Long = -1,
    val optionalSeen: List<String> = emptyList())
data class NotificationState(val cursor: Long = 0, val bootstrap: Boolean = true, val draining: Boolean = false,
    val batchActive: Boolean = false, val batchSoundUsed: Boolean = false, val lastSoundElapsed: Long = -1,
    val settingsGeneration: Long = -1, val pending: List<NotificationEvent> = emptyList(),
    val records: List<NotificationRecord> = emptyList(), val pendingPosts: Set<String> = emptySet(), val truncated: Boolean = false, val deliveryNeeded: Boolean = false, val overflowKinds: Set<SessionAlertKind> = emptySet(), val pendingRestoreKinds: Set<SessionAlertKind> = emptySet(), val presenceAlerts: Boolean = false)
data class SessionNotice(val current: NotificationCurrent, val kind: SessionAlertKind, val fingerprint: String,
    val fresh: Boolean, val eventId: Long = 0, val eventAt: Double = 0.0) {
    val body get() = "${when (kind) {
        SessionAlertKind.Input -> "Needs input"
        SessionAlertKind.Error -> "Error"
        SessionAlertKind.Finished -> "Turn finished"
        SessionAlertKind.Optional -> "Optional question"
    }} on ${current.machine}"
}
data class NotificationPlan(val state: NotificationState, val desired: List<SessionNotice>,
    val retained: Set<String>, val reconcile: Boolean, val overflow: Boolean = false,
    val overflowKinds: Set<SessionAlertKind> = emptySet(), val restoreKinds: Set<SessionAlertKind> = emptySet())
data class NotificationPage(val state: NotificationState, val full: Boolean, val empty: Boolean)

/** Only needs that cannot be posted individually join the "More sessions need attention" summary. */
val SessionAlertKind.overflows get() = this == SessionAlertKind.Input || this == SessionAlertKind.Error

/** Events wake current-slot presentation; they cannot identify a historical run. */
object NotificationPolicy {
    const val maxPending = 128
    const val maxRecords = 256
    const val cooldownMillis = 30_000L
    /** Optional question IDs remembered per slot, so each one announces at most once. */
    const val maxOptionalSeen = 64

    fun fingerprint(value: String) = MessageDigest.getInstance("SHA-256").digest(value.toByteArray(Charsets.UTF_8))
        .joinToString("") { "%02x".format(it) }

    // Each alert kind survives both other alerts and ordinary refresh rows.
    private fun pendingKey(event: NotificationEvent) = JSONArray(if (event.kind == "alert")
        listOf(event.computer, event.session, "alert", event.alertKind)
        else listOf(event.computer, event.session)).toString()

    fun ingest(state: NotificationState, events: List<NotificationEvent>): NotificationPage {
        if (events.isEmpty()) return NotificationPage(state.copy(draining = false), false, true)
        val pending = state.pending.associateByTo(linkedMapOf()) { pendingKey(it) }
        var cursor = state.cursor
        for (event in events) {
            if (event.id <= cursor) continue
            val key = pendingKey(event)
            if (key !in pending && pending.size >= maxPending) return NotificationPage(
                state.copy(cursor = cursor, pending = pending.values.toList(), draining = true,
                    batchActive = true, batchSoundUsed = if (state.batchActive) state.batchSoundUsed else false), true, false)
            pending[key] = event
            cursor = event.id
        }
        require(cursor > state.cursor) { "Notification page did not advance." }
        return NotificationPage(state.copy(cursor = cursor, pending = pending.values.toList(), draining = true,
            batchActive = true, batchSoundUsed = if (state.batchActive) state.batchSoundUsed else false), false, false)
    }

    fun plan(state: NotificationState, current: List<NotificationCurrent>, events: List<Pair<NotificationSlot, NotificationEvent>>,
        preferences: NotificationPreferences, now: Double, active: Set<String> = emptySet(), alertsOnly: Boolean = false): NotificationPlan {
        val old = state.records.associateBy { it.slot.key }
        // With presence alerts only relay alert rows announce; other rows refresh state.
        val announcing = events.filter { (_, event) -> event.kind != "notification_preferences" && (if (alertsOnly) event.isAlert else event.kind != "alert") }
        val candidates = announcing.groupBy { it.first.key }
        // Relay and phone order input and error differently, so either need alert announces either card.
        val needs = setOf("attention", "error")
        val alerted = events.filter { (_, event) -> event.isAlert && event.signal in needs }.map { it.first.key }.toSet()
        val transitions = events.filter { (_, event) -> event.kind in needs }.map { it.first.key }.toSet()
        val reconcile = preferences.generation != state.settingsGeneration
        val restoreKinds = (state.pendingRestoreKinds + SessionAlertKind.entries.filter {
            preferences.enabledAt(it) > state.settingsGeneration }).filter(preferences::allows).toSet()
        val desired = mutableListOf<SessionNotice>()
        val records = mutableListOf<NotificationRecord>()
        val retained = mutableSetOf<String>()
        val recoverable = mutableSetOf<String>()
        for (session in current.distinctBy { it.slot.key }) {
            val previous = old[session.slot.key]
            if(session.suppressed) {
                records += NotificationRecord(session.slot,session.target,now,
                    observedFingerprint=session.kind?.let { fingerprint(JSONArray(listOf(session.target,it.name,session.identity)).toString()) }.orEmpty(),
                    optionalSeen=(previous?.optionalSeen.orEmpty()+session.optional).distinct().takeLast(maxOptionalSeen))
                continue
            }
            if (!session.live) {
                // A stopped session keeps only the optional questions it announced; resuming
                // the same conversation does not announce them again.
                if (previous != null && previous.optionalSeen.isNotEmpty()) records += NotificationRecord(session.slot,
                    previous.target, previous.observedAt, summaryOnly = state.truncated, optionalSeen = previous.optionalSeen)
                continue
            }
            if (!session.online) {
                if (previous?.postedKind?.let(preferences::allows) == true) retained += session.slot.key
                if (previous != null) records += if (session.slot.key in state.pendingPosts && session.slot.key !in active)
                    previous.copy(postedFingerprint = "", postedKind = null) else previous
                continue
            }
            val event = candidates[session.slot.key]?.filter { (_, event) -> !alertsOnly ||
                if (session.kind != null && session.kind != SessionAlertKind.Finished)
                    event.signal in needs else event.signal == "completed"
            }?.maxByOrNull { it.second.id }?.second
            val recent = event != null && event.at <= now && event.at >= now - 120
            var kind = session.kind
            var freshCompletion = false
            var identity = kind?.let { fingerprint(JSONArray(listOf(session.target, it.name, session.identity)).toString()) }.orEmpty()
            // Optional questions announce once per question ID, across resumes and removals.
            val seenOptional = previous?.optionalSeen.orEmpty()
            val newOptional = session.optional.filter { it !in seenOptional }
            if (kind == SessionAlertKind.Optional) {
                // Fewer questions keep the posted card, and a dismissed card stays dismissed.
                identity = previous?.takeIf { newOptional.isEmpty() && it.postedKind == SessionAlertKind.Optional &&
                    it.postedFingerprint.isNotBlank() }?.postedFingerprint
                    ?: fingerprint(JSONArray(listOf(session.conversation, SessionAlertKind.Optional.name, JSONArray(session.optional))).toString())
                // When disabled, a completion can still apply.
                if (!preferences.allows(kind)) { kind = null; identity = "" }
            }
            if (kind == null && session.idle && !state.bootstrap) {
                if (preferences.finished && event?.signal == "completed" && recent && previous != null &&
                    session.target.isNotBlank() && previous.target == session.target && event.at > previous.observedAt) {
                    kind = SessionAlertKind.Finished
                    freshCompletion = true
                    identity = fingerprint(JSONArray(listOf(session.target, "Finished", event.nativeReplyId ?: event.id)).toString())
                } else if (previous != null && previous.candidateFingerprint.isNotBlank() &&
                    previous.candidateTarget == session.target && previous.candidateEventAt in (now - 120)..now) {
                    kind = SessionAlertKind.Finished
                    identity = previous.candidateFingerprint
                } else if (previous?.postedKind == SessionAlertKind.Finished &&
                    previous.postedTarget == session.target && previous.eventAt in (now - 120)..now) {
                    kind = SessionAlertKind.Finished
                    identity = previous.postedFingerprint
                }
            }
            var detailOnly = false
            var held = false
            if (alertsOnly && kind != null && kind != SessionAlertKind.Finished &&
                previous?.postedFingerprint != identity && session.slot.key !in alerted) {
                // A posted input or error card follows detail changes silently, as without
                // presence alerts. A need the relay announced without an alert, or one never
                // posted, was observed while the owner was at a computer: no card until it alerts.
                detailOnly = previous != null && previous.postedFingerprint.isNotBlank() &&
                    previous.postedKind != null && previous.postedKind != SessionAlertKind.Finished &&
                    previous.postedTarget == session.target && session.slot.key !in transitions
                if (!detailOnly) { held = kind == SessionAlertKind.Optional; kind = null; identity = "" }
            }
            val freshTransition = recent && !detailOnly && if (kind == SessionAlertKind.Optional) newOptional.isNotEmpty()
                else identity != previous?.observedFingerprint
            // Questions held for a presence alert stay new; any other outcome has shown or
            // superseded them.
            val optionalSeen = if (held) seenOptional
                else (seenOptional.filter { it !in session.optional } + session.optional).takeLast(maxOptionalSeen)
            val summaryOnly = previous?.summaryOnly?.let { it && !freshTransition } ?: state.truncated
            val observed = previous ?: NotificationRecord(session.slot, session.target, now)
            val recoveringCompletion = kind == SessionAlertKind.Finished && !freshCompletion
            val finishedEventId = if (recoveringCompletion) previous?.candidateEventId?.takeIf { it > 0 } ?: previous?.eventId ?: 0
                else event?.id ?: previous?.eventId ?: 0
            val finishedEventAt = if (recoveringCompletion) previous?.candidateEventAt?.takeIf { it > 0 } ?: previous?.eventAt ?: 0.0
                else event?.at ?: previous?.eventAt ?: 0.0
            records += observed.copy(target = session.target, observedAt = now,
                observedFingerprint = if (detailOnly) observed.observedFingerprint else identity,
                candidateFingerprint = if (kind == SessionAlertKind.Finished) identity else "",
                candidateTarget = if (kind == SessionAlertKind.Finished) session.target else "",
                candidateEventId = if (kind == SessionAlertKind.Finished) finishedEventId else 0,
                candidateEventAt = if (kind == SessionAlertKind.Finished) finishedEventAt else 0.0,
                postedFingerprint = if (kind == null) "" else observed.postedFingerprint,
                postedKind = if (kind == null) null else observed.postedKind, summaryOnly = summaryOnly, optionalSeen = optionalSeen)
            if (kind != null && preferences.allows(kind)) {
                retained += session.slot.key
                recoverable += session.slot.key
                val fresh = !state.bootstrap && !reconcile && freshTransition && !summaryOnly
                desired += SessionNotice(session, kind, identity, fresh,
                    finishedEventId, finishedEventAt)
            }
        }
        // Prefer retained delivery evidence before observation-only entries.
        val bounded = records.sortedWith(compareByDescending<NotificationRecord> { it.slot.key in active || it.slot.key in state.pendingPosts }
            .thenByDescending { old[it.slot.key]?.observedAt ?: now }.thenBy { it.slot.key })
            .take(maxRecords)
        val eligible = bounded.filterNot { it.summaryOnly }.map { it.slot.key }.toSet()
        val individual = desired.filter { it.current.slot.key in eligible }
        val overflowKinds = desired.filter { it.current.slot.key !in eligible && it.kind.overflows }.map { it.kind }.toSet()
        return NotificationPlan(state.copy(pending = emptyList(), records = bounded,
            bootstrap = state.bootstrap && state.draining, settingsGeneration = preferences.generation,
            batchActive = false, deliveryNeeded = true, pendingRestoreKinds = restoreKinds, pendingPosts = state.pendingPosts.intersect(recoverable),
            truncated = state.truncated || records.size > maxRecords), individual, retained, reconcile, overflowKinds.isNotEmpty(), overflowKinds,
            restoreKinds)
    }

    fun reserve(state: NotificationState, notice: SessionNotice, audible: Boolean, elapsed: Long, enabledAt: Long = 0): NotificationState {
        val key = notice.current.slot.key
        val existing = state.records.find { it.slot.key == key } ?: NotificationRecord(notice.current.slot, notice.current.target, notice.eventAt)
        val record = existing.copy(postedFingerprint = notice.fingerprint, postedKind = notice.kind,
            postedTarget = notice.current.target, eventId = notice.eventId, eventAt = notice.eventAt, restoredAt = maxOf(existing.restoredAt, enabledAt))
        return state.copy(records = (listOf(record) + state.records.filterNot { it.slot.key == key }).take(maxRecords),
            pendingPosts = (listOf(key) + state.pendingPosts.filterNot { it == key }).take(24).toSet(), batchSoundUsed = state.batchSoundUsed || audible,
            lastSoundElapsed = if (audible) elapsed else state.lastSoundElapsed)
    }

    fun canSound(state: NotificationState, elapsed: Long): Boolean = !state.batchSoundUsed &&
        (state.lastSoundElapsed < 0 || elapsed >= state.lastSoundElapsed && elapsed - state.lastSoundElapsed >= cooldownMillis)

    fun clockGuard(state: NotificationState, elapsed: Long) = if (state.lastSoundElapsed > elapsed)
        state.copy(lastSoundElapsed = elapsed) else state
}

object NotificationStateCodec {
    fun encode(state: NotificationState): JSONObject = JSONObject().put("schema", 1).put("cursor", state.cursor)
        .put("bootstrap", state.bootstrap).put("draining", state.draining).put("batch", state.batchActive)
        .put("batch_sound", state.batchSoundUsed).put("sound_elapsed", state.lastSoundElapsed).put("settings", state.settingsGeneration)
        .put("pending", JSONArray(state.pending.map { JSONObject().put("id", it.id).put("computer", it.computer)
            .put("session", it.session).put("kind", it.kind).put("at", it.at).put("alert_kind", it.alertKind).put("native_reply_id",it.nativeReplyId) }))
        .put("records", JSONArray(state.records.map { JSONObject().put("slot", JSONArray(listOf(it.slot.connection, it.slot.computer, it.slot.session)))
            .put("target", it.target).put("observed", it.observedAt).put("fingerprint", it.observedFingerprint)
            .put("posted", it.postedFingerprint).put("kind", it.postedKind?.name).put("posted_target", it.postedTarget)
            .put("event", it.eventId).put("at", it.eventAt).put("summary_only", it.summaryOnly).put("candidate", it.candidateFingerprint)
            .put("candidate_target", it.candidateTarget).put("candidate_event", it.candidateEventId).put("candidate_at", it.candidateEventAt).put("restored", it.restoredAt)
            .put("optional_seen", JSONArray(it.optionalSeen)) }))
        .put("posting", JSONArray(state.pendingPosts.toList())).put("truncated", state.truncated).put("delivery_needed", state.deliveryNeeded).put("overflow_kinds", JSONArray(state.overflowKinds.map { it.name })).put("restore_kinds", JSONArray(state.pendingRestoreKinds.map { it.name })).put("presence_alerts", state.presenceAlerts)

    fun decode(raw: JSONObject): NotificationState {
        require(raw.optInt("schema") == 1)
        val pending = raw.optJSONArray("pending")?.objects().orEmpty()
        val records = raw.optJSONArray("records")?.objects().orEmpty()
        val posting = raw.optJSONArray("posting") ?: JSONArray()
        require(pending.size <= NotificationPolicy.maxPending && records.size <= NotificationPolicy.maxRecords && posting.length() <= 24)
        return NotificationState(raw.optLong("cursor").coerceAtLeast(0), raw.optBoolean("bootstrap", true), raw.optBoolean("draining"),
            raw.optBoolean("batch"), raw.optBoolean("batch_sound"), raw.optLong("sound_elapsed", -1), raw.optLong("settings", -1),
            pending.map { NotificationEvent(it.getLong("id"), it.getString("computer"), it.getString("session"), it.getString("kind"), it.getDouble("at"), it.string("alert_kind").ifEmpty { null },it.string("native_reply_id").ifEmpty { null }) },
            records.map { row -> val slot = row.getJSONArray("slot")
                NotificationRecord(NotificationSlot(slot.getString(0), slot.getString(1), slot.getString(2)), row.getString("target"),
                    row.getDouble("observed"), row.string("fingerprint"), row.string("posted"),
                    row.string("kind").takeIf { it.isNotEmpty() }?.let { runCatching { SessionAlertKind.valueOf(it) }.getOrNull() }, row.string("posted_target"), row.optLong("event"), row.optDouble("at"), row.optBoolean("summary_only"),
                    row.string("candidate"), row.string("candidate_target"), row.optLong("candidate_event"), row.optDouble("candidate_at"), row.optLong("restored", -1),
                    row.optJSONArray("optional_seen")?.let { ids -> (0 until ids.length()).map(ids::getString) }.orEmpty().takeLast(NotificationPolicy.maxOptionalSeen)) },
            (0 until posting.length()).map { posting.getString(it) }.toSet(), raw.optBoolean("truncated"), raw.optBoolean("delivery_needed"), raw.optJSONArray("overflow_kinds")?.let { kinds ->
                (0 until kinds.length()).mapNotNull { runCatching { SessionAlertKind.valueOf(kinds.getString(it)) }.getOrNull() }.toSet() }.orEmpty(),
            raw.optJSONArray("restore_kinds")?.let { kinds -> (0 until kinds.length()).mapNotNull {
                runCatching { SessionAlertKind.valueOf(kinds.getString(it)) }.getOrNull() }.toSet() }.orEmpty(), raw.optBoolean("presence_alerts"))
    }
}
