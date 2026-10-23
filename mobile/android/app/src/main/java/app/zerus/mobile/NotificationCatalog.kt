package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class NotificationCatalog(val sessions: List<NotificationCurrent>, val identities: Map<String, String?>) {
    fun resolve(connection: String, event: NotificationEvent): Pair<NotificationSlot, NotificationEvent>? =
        identities[event.computer]?.let { NotificationSlot(connection, it, event.session) to event }
}

object NotificationCatalogParser {
    fun parse(connection: Connection, records: List<JSONObject>, aliases: List<MachineAlias>, actions: List<SessionAction> = emptyList()): NotificationCatalog {
        val identities = mutableMapOf<String, String?>()
        val current = mutableListOf<NotificationCurrent>()
        val known=SessionNotificationPreferences.known(actions=actions)
        val unique = records.groupBy { it.getString("id") }.filterValues { it.size == 1 }.values.map { it.single() }
        for (raw in unique) {
            val machine = MachineNames.apply(MachineCatalog.parse(connection.id, raw), aliases)
            for (id in machine.serverAliases + machine.id) {
                if (!identities.containsKey(id)) identities[id] = machine.id
                else if (identities[id] != machine.id) identities[id] = null
            }
            for (session in NativeParser.sessions(connection, raw)) {
                if (session.target.session.isBlank() || session.target.session.length > 1024 || SessionFilters.archived(session)) continue
                val value = SessionNotificationPreferences.merge(session.raw,session.target,known)
                val phase = value.string("phase")
                val recovery = value.optJSONObject("recovery")?.string("state").orEmpty()
                val (optionalItems, blocking) = value.optJSONArray("mobile_attention")?.objects().orEmpty()
                    .partition(QuestionPolicies::optionalAttention)
                // Optional questions never change the identity of a request that needs action.
                val pending = blocking.map {
                    JSONArray(listOf(it.string("question_id"), it.string("question_hash"))).toString()
                }.sorted()
                val optional = optionalItems.map { it.string("question_id") }.filter { it.isNotBlank() }.distinct().sorted()
                val nativeKind = when {
                    phase == "error" && recovery !in setOf("waiting", "dispatching", "retrying") ||
                        recovery in setOf("uncertain", "blocked", "exhausted") -> SessionAlertKind.Error
                    phase in setOf("input", "approval") || pending.isNotEmpty() || value.optInt("pending_question_count") > 0 || SessionNotificationPreferences.hasChildAttention(value) -> SessionAlertKind.Input
                    optional.isNotEmpty() -> SessionAlertKind.Optional
                    else -> null
                }
                val kind=SessionNotificationPreferences.alertKind(value,session.target,nativeKind,optional)
                val identity = NotificationPolicy.fingerprint(JSONArray(listOf(phase, value.string("attention_id"),
                    value.opt("question_request"), value.opt("question_requests"), pending, recovery,
                    value.optJSONObject("provider_error")?.let { listOf(it.string("source"), it.string("message_id"), it.string("code", "type"), it.string("message")) },
                    value.optJSONObject("recovery")?.let { listOf(it.string("request_id"), it.string("error", "reason")) },
                    SessionNotificationPreferences.attentionIdentity(value,session.target),value.optInt("pending_question_count"))).toString())
                val target = if (session.target.run.isNotBlank() && session.target.conversation.isNotBlank())
                    JSONArray(listOf(session.target.run, session.target.conversation)).toString() else ""
                current += NotificationCurrent(NotificationSlot(connection.id, machine.id, session.target.session), target,
                    clean(SessionFilters.label(session).ifBlank { session.title }, "Session"), clean(machine.name, "Computer"), kind,
                    identity, value.string("activity") == "idle", machine.online,
                    SessionFilters.state(session) == "running" && value.string("process_state") != "exited",
                    session.target.conversation, optional,SessionNotificationPreferences.suppressed(value,session.target,kind,optional),SessionNotificationPreferences.parse(value,session.target))
            }
        }
        return NotificationCatalog(current, identities)
    }

    private fun clean(value: String, fallback: String) = value.filterNot {
        Character.isISOControl(it) || Character.getType(it) == Character.FORMAT.toInt()
    }.take(256).ifBlank { fallback }

    private val alertKinds = setOf("attention", "error", "completed")
    /** Relay capability that makes alert rows the only source of new cards. */
    fun presenceAlerts(capabilities: JSONObject) = capabilities.optJSONArray("features")?.let { features ->
        (0 until features.length()).any { features.optString(it) == "presence_alerts" } } == true

    fun events(raw: JSONObject): List<NotificationEvent> {
        val values = raw.getJSONArray("events")
        require(values.length() <= 100)
        return values.objects().map { row ->
            val id = row.getLong("id")
            val computer = row.getString("computer_id")
            val session = row.getString("session")
            val at = row.getDouble("created_at")
            require(id > 0 && computer.length in 1..512 && session.length in 1..1024 &&
                session.none { Character.isISOControl(it) } && at.isFinite() && at > 0)
            val kind = row.getString("kind")
            // Unknown alert kinds stay plain refresh rows instead of failing the page.
            NotificationEvent(id, computer, session, kind, at, row.string("alert_kind").takeIf { kind == "alert" && it in alertKinds },
                row.string("native_reply_id").takeIf { it.toByteArray(Charsets.UTF_8).size in 1..256 && it.none(Char::isISOControl) })
        }.also { events -> require(events.zipWithNext().all { (a, b) -> a.id < b.id }) }
    }
}
