package app.zerus.mobile

import org.json.JSONArray
import java.text.DateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** Machine and project scopes include the workspace, even when native names coincide. */
data class MachineKey(val connectionId: String, val computerId: String) {
    val key: String get() = JSONArray(listOf(connectionId, computerId)).toString()
}
enum class SessionFilter(val id: String, val caption: String) {
    All("all", "All sessions"), Attention("attention", "Needs attention"), Active("active", "Active"),
    Saved("paused", "Saved sessions"), Archive("archived", "Archive")
}
data class SessionGroup(val key: ProjectKey, val name: String, val color: String, val sessions: List<Session>)

object SessionFilters {
    fun machineKey(target: Target) = MachineKey(target.connectionId, target.computerId)
    fun online(session: Session, machines: List<Machine>) = machines.any {
        it.connectionId == session.target.connectionId && it.id == session.target.computerId && it.online
    }
    fun archived(session: Session) = session.target.archiveId.isNotBlank() || session.raw.string("state") == "archived"
    fun state(session: Session) = session.raw.string("state").ifBlank { if (archived(session)) "archived" else "running" }
    fun currentActivity(session: Session, online: Boolean) = online && state(session) == "running" &&
        session.raw.string("process_state") != "exited" && session.raw.string("activity") != "unknown" &&
        !(session.raw.string("conversation_state") == "ended" && session.raw.string("activity") != "idle")
    fun needsAction(session: Session): Boolean {
        val recovery = session.raw.optJSONObject("recovery")?.string("state").orEmpty()
        val providerError = session.raw.optJSONObject("provider_error")?.string("error_kind").orEmpty()
        val phase = session.raw.string("phase")
        return !archived(session) && state(session) == "running" && session.raw.string("process_state") != "exited" &&
            (phase in listOf("approval", "input") || phase == "error" &&
                (recovery !in listOf("waiting", "dispatching", "retrying") || providerError in listOf("quota", "provider_policy")) ||
                recovery in listOf("uncertain", "blocked", "exhausted"))
    }
    fun working(session: Session, online: Boolean) = !archived(session) && currentActivity(session, online) &&
        session.raw.string("activity") == "busy" && !needsAction(session)
    private fun positiveCount(value: Any?) = (value as? Number)?.toDouble()?.let { it.isFinite() && it > 0 && it % 1.0 == 0.0 } == true
    /** Work is an unfinished turn or a working subagent. Commands the agent left running
     *  (stands, dev servers) never are: a finished turn is finished. */
    fun replyInProgress(session: Session, online: Boolean) = currentActivity(session, online) &&
        (session.raw.string("activity") == "busy" ||
            positiveCount(session.raw.opt("subagent_active_count") as? Number ?: session.raw.opt("subagent_count")))
    /** Keep the read identity while postponing reply badges and reply-only attention. */
    fun visibleUnreadReply(session: Session, online: Boolean) = !archived(session) && !replyInProgress(session, online) &&
        (session.raw.optBoolean("unread_reply") || session.raw.optBoolean("mobile_unread_reply"))
    /** Jobs the agent scheduled for itself start its next turns: neither Ready nor Working (SessionPresentation::scheduledIdle). */
    fun scheduledIdle(session: Session): Boolean {
        val jobs = session.raw.optJSONObject("schedules")?.opt("count") as? Number
        return session.raw.string("activity") == "idle" && session.raw.string("phase") == "idle" && (jobs?.toInt() ?: 0) > 0
    }
    fun scheduled(session: Session, online: Boolean) = !archived(session) && currentActivity(session, online) &&
        !needsAction(session) && scheduledIdle(session)
    /** The next run as a time of day, "due" once it has passed, or empty if unknown. */
    fun nextScheduledRun(session: Session, now: Long = System.currentTimeMillis(), locale: Locale = Locale.getDefault(),
        zone: TimeZone = TimeZone.getDefault()): String {
        val seconds = (session.raw.optJSONObject("schedules")?.opt("next_at") as? Number)?.toDouble() ?: return ""
        if (!seconds.isFinite() || seconds <= 0) return ""
        val next = (seconds * 1000).toLong()
        if (next <= now) return "due"
        val day = java.text.SimpleDateFormat("yyyyMMdd", locale).apply { timeZone = zone }
        val sameDay = day.format(Date(next)) == day.format(Date(now))
        return java.text.SimpleDateFormat(if (sameDay) "HH:mm" else "d MMM HH:mm", locale).apply { timeZone = zone }.format(Date(next))
    }
    fun needsAttention(session: Session, online: Boolean): Boolean {
        if (archived(session)) return false
        if (session.raw.optBoolean("review_later") || session.raw.optBoolean("mobile_review_later")) return true
        if (!online) return false
        val acknowledged = session.raw.optBoolean("attention_acknowledged")
        val children = session.raw.optJSONObject("subagents")?.let { roster -> roster.keys().asSequence().mapNotNull { roster.optJSONObject(it) }.toList() }.orEmpty() +
            session.raw.optJSONArray("subagent_previews")?.objects().orEmpty()
        val childAttention = currentActivity(session, online) && session.raw.string("subagent_source") != "hook_profiles" && children.any { it.string("display_state", "state") in listOf("approval", "input", "attention") }
        return !acknowledged && (needsAction(session) || childAttention) || visibleUnreadReply(session, online) ||
            currentActivity(session, online) && session.pendingCount > 0
    }
    /** Composer drafts remain local and count even when their machine is offline. */
    fun draftedSessions(sessions: List<Session>, drafts: List<Draft>): Set<String> {
        val unsent = drafts.filter { it.questionId.isBlank() && it.detachedId.isBlank() && it.target.archiveId.isBlank() &&
            it.status !in listOf("submitting", "submitted", "recorded") && (it.text.isNotBlank() || it.attachments.isNotEmpty()) }
        return sessions.filterNot(::archived).filter { session -> unsent.any { draft ->
            if (draft.target.agentId.isBlank()) ContextPolicies.sameSession(draft.target, session.target)
            else session.raw.string("subagent_source") != "hook_profiles" &&
                draft.target.copy(conversation = draft.target.parentConversation, agentId = "", parentConversation = "") == session.target &&
                session.raw.optJSONObject("subagents")?.optJSONObject(draft.target.agentId)?.optBoolean("group") == false
        } }.mapTo(mutableSetOf()) { it.target.key }
    }
    fun active(session: Session, online: Boolean, hasDraft: Boolean = false) =
        !archived(session) && (hasDraft || needsAttention(session, online) || working(session, online) || scheduled(session, online) ||
            replyInProgress(session, online) && session.raw.string("activity") == "idle" && session.raw.string("phase") == "idle")
    fun matches(session: Session, filter: SessionFilter, online: Boolean, hasDraft: Boolean = false): Boolean = when (filter) {
        SessionFilter.All -> !archived(session)
        SessionFilter.Attention -> needsAttention(session, online)
        SessionFilter.Active -> active(session, online, hasDraft)
        SessionFilter.Saved -> !archived(session) && state(session) != "running"
        SessionFilter.Archive -> archived(session)
    }
    fun label(session: Session): String = session.raw.string("tag").ifBlank {
        session.target.session.split('/').drop(2).joinToString("/").ifBlank {
            session.raw.string("project").ifBlank { session.target.session }
        }
    }
    fun archiveDate(session: Session, locale: Locale = Locale.getDefault(), zone: TimeZone = TimeZone.getDefault()): String {
        if (!archived(session)) return ""
        val seconds = session.raw.optDouble("archived_at", Double.NaN)
        if (!seconds.isFinite() || seconds <= 0 || seconds > 253_402_300_799.0) return ""
        return runCatching {
            DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.MEDIUM, locale).apply { timeZone = zone }
                .format(Date((seconds * 1000).toLong()))
        }.getOrDefault("")
    }
    fun status(session: Session, online: Boolean): String {
        val raw = session.raw
        return when {
            !online -> "Offline"
            archived(session) -> "Archived"
            state(session) == "paused" -> "Paused"
            state(session) == "stopped" -> "Stopped"
            raw.string("process_state") == "exited" -> "Agent exited"
            !raw.optBoolean("tracked") && raw.string("activity").isBlank() -> "Not tracked"
            raw.string("phase") == "approval" -> "Needs approval"
            raw.string("phase") == "input" -> "Needs input"
            raw.optJSONObject("recovery")?.string("state") == "waiting" -> "Waiting to retry"
            raw.optJSONObject("recovery")?.string("state") in listOf("dispatching", "retrying") -> "Retrying"
            raw.string("phase") == "error" -> "Error"
            raw.string("phase") == "interrupted" -> "Interrupted"
            raw.string("phase") == "compacting" -> "Compacting"
            raw.string("phase") == "starting" -> "Starting"
            raw.string("phase") == "tool" -> "Using a tool"
            raw.string("activity") == "busy" -> "Working"
            scheduledIdle(session) -> nextScheduledRun(session).let { if (it.isEmpty()) "Scheduled" else "Scheduled · $it" }
            raw.string("activity") == "idle" -> "Ready"
            raw.string("activity") in listOf("ended", "unknown") -> "Status unknown"
            else -> "Connecting"
        }
    }
    fun scoped(sessions: List<Session>, machines: List<Machine>, selectedMachines: Set<MachineKey>, query: String,
        project: ProjectKey? = null): List<Session> = sessions.filter { session ->
        (project == null || session.projectKey == project) && (selectedMachines.isEmpty() || machineKey(session.target) in selectedMachines) &&
            (query.isBlank() || (listOf(session.target.session, session.target.conversation, session.target.run, session.target.archiveId,
                session.target.agentId, label(session), session.title, session.preview, session.agent, session.project) + childIdentifiers(session) +
                listOf("prompt", "current_tool", "git_branch", "git_worktree_name", "activity_summary", "activity_detail", "cwd").map { SessionPreview.render(session.raw.string(it)) } +
                machines.filter { machineKey(session.target) == MachineKey(it.connectionId, it.id) }.map { it.name }).joinToString(" ").contains(query.trim(), true))
    }
    private fun childIdentifiers(session: Session): List<String> {
        if (session.raw.string("subagent_source") == "hook_profiles") return emptyList()
        val roster = session.raw.optJSONObject("subagents")
        val previews = session.raw.optJSONArray("subagent_previews")?.objects().orEmpty()
        return roster?.keys()?.asSequence()?.flatMap { id -> sequenceOf(id, roster.optJSONObject(id)?.string("conversation_id").orEmpty()) }?.toList().orEmpty() +
            previews.flatMap { listOf(it.string("id", "agent_id"), it.string("conversation_id")) }
    }
    fun counts(sessions: List<Session>, machines: List<Machine>, drafts: Set<String> = emptySet()): Map<SessionFilter, Int> = SessionFilter.entries.associateWith { filter ->
        sessions.count { matches(it, filter, online(it, machines), it.target.key in drafts) }
    }
    fun grouped(sessions: List<Session>, projects: List<ProjectSummary>): List<SessionGroup> {
        val grouped = sessions.groupBy { it.projectKey ?: ProjectKey(it.target.connectionId, "unassigned:${it.target.computerId}", "unassigned") }
        val order = projects.map { it.key } + grouped.keys.filter { key -> projects.none { it.key == key } }
        return order.mapNotNull { key -> grouped[key]?.takeIf { it.isNotEmpty() }?.let { members ->
            val project = projects.find { it.key == key }
            SessionGroup(key, project?.name ?: members.first().project.ifBlank { "Unassigned sessions" }, project?.color.orEmpty(),
                if (members.all(::archived)) members.sortedByDescending { it.raw.optDouble("archived_at", 0.0) } else members)
        } }
    }
}
