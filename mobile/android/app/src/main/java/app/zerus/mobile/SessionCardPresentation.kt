package app.zerus.mobile

import org.json.JSONObject

/** One subagent row beneath an expanded session card. */
data class SessionCardChild(val id: String, val name: String, val status: String, val detail: String,
    val provider: String, val kind: DesktopBadgeKind, val group: Boolean) {
    val description: String get() = "Subagent $name, $status"
}

/** The card's subagent counter: compact label, spoken form, and whether it shows child rows. */
data class SessionChildCounter(val label: String, val description: String, val expandable: Boolean)

/**
 * Session card checkout and subagent semantics from the desktop: branchContext, projectContext,
 * childState, inactiveChild, childStatus and childCount in tray/src/SessionPresentation.h, and
 * childRoster, showChild and the child rows in tray/src/SessionsWindow.cpp.
 */
object SessionCardPresentation {
    // Qt reads these fields strictly: a value of another JSON type is absent.
    private fun text(raw: JSONObject, key: String): String = raw.opt(key) as? String ?: ""
    private fun flag(raw: JSONObject, key: String): Boolean = raw.opt(key) == true
    private fun integer(raw: JSONObject, key: String, fallback: Int): Int {
        val value = (raw.opt(key) as? Number)?.toDouble() ?: return fallback
        return if (value.isFinite() && value == Math.floor(value) && value >= Int.MIN_VALUE && value <= Int.MAX_VALUE) value.toInt() else fallback
    }
    private fun simplified(value: String): String = buildString {
        var space = false
        for (char in value.trim()) {
            if (char.isWhitespace()) space = true
            else { if (space) append(' '); space = false; append(char) }
        }
    }

    /** The card's checkout line: the branch, or detached HEAD. Ordinary folders have none. */
    fun branch(raw: JSONObject): String {
        if (text(raw, "git_metadata_state") == "not_repo") return ""
        return text(raw, "git_branch").ifEmpty { if (flag(raw, "git_detached")) "detached HEAD" else "" }
    }

    private fun worktree(raw: JSONObject): String =
        if (!flag(raw, "git_worktree") || text(raw, "git_metadata_state") == "not_repo") ""
        else text(raw, "git_worktree_name").let { if (it.isEmpty()) "worktree" else "worktree: $it" }

    /** projectContext without the project name, as the header and Details show it. */
    fun checkout(raw: JSONObject): String = listOf(branch(raw), worktree(raw)).filter { it.isNotEmpty() }.joinToString(" / ")

    /** Cards show only the branch; the spoken description keeps the linked worktree. */
    fun checkoutDescription(raw: JSONObject): String {
        val branch = branch(raw)
        if (branch.isEmpty()) return ""
        val name = text(raw, "git_worktree_name")
        val worktree = when {
            worktree(raw).isEmpty() -> ""
            name.isEmpty() -> ", linked worktree"
            else -> ", linked worktree $name"
        }
        return (if (branch == "detached HEAD") "Detached HEAD" else "Git branch $branch") + worktree
    }

    fun childState(child: JSONObject): String = text(child, "display_state").ifEmpty { text(child, "state") }

    fun inactiveChild(child: JSONObject, countsComplete: Boolean = false): Boolean {
        val state = childState(child)
        val inactive = state == "finished" || state == "idle" || state == "ready"
        // Explicit display evidence wins over an older raw turn state or count.
        if (state.isNotEmpty() && !inactive) return false
        if (flag(child, "group")) return countsComplete && (child.opt("active_count") as? Number)?.toDouble() == 0.0
        return inactive
    }

    fun childStatus(child: JSONObject): String = when (childState(child)) {
        "working" -> "Working"
        "finished", "ready", "idle" -> "Ready"
        "error" -> "Error"
        "paused" -> "Paused"
        "approval", "input", "attention" -> "Needs input"
        else -> "Unknown"
    }

    /** Only explicit requests for user intervention escalate; results and failures belong to the parent. */
    fun childAction(child: JSONObject): Boolean = childState(child) in setOf("approval", "input", "attention")

    fun childName(child: JSONObject, id: String): String =
        ((child.opt("label") as? String) ?: (child.opt("name") as? String) ?: "").ifEmpty { id }

    /** Profile groups have no individual identities, so they never become rows. */
    fun roster(raw: JSONObject): List<Pair<String, JSONObject>> {
        if (text(raw, "subagent_source") == "hook_profiles") return emptyList()
        val roster = raw.optJSONObject("subagents") ?: return emptyList()
        // Stable ID order avoids moving a row under the finger when a child finishes.
        return roster.keys().asSequence().toList().sorted().map { id -> id to (roster.optJSONObject(id) ?: JSONObject()) }
    }

    /** Children with an unsent draft on this phone stay visible after their turn finishes. */
    fun draftedChildren(parent: Target, drafts: List<Draft>): Set<String> = drafts.filter { draft ->
        val child = draft.target
        child.agentId.isNotBlank() && (draft.text.isNotBlank() || draft.attachments.isNotEmpty()) &&
            child.copy(conversation = child.parentConversation, agentId = "", parentConversation = "") == parent
    }.mapTo(mutableSetOf()) { it.target.agentId }

    /** The expanded rows. Finished, idle and ready turns are hidden unless completed children are included. */
    fun children(session: Session, online: Boolean, includeCompleted: Boolean = false,
        drafted: Set<String> = emptySet()): List<SessionCardChild> {
        val current = SessionFilters.currentActivity(session, online)
        val complete = flag(session.raw, "subagent_counts_complete")
        return roster(session.raw).filter { (id, child) ->
            includeCompleted || !flag(child, "group") && id in drafted || !inactiveChild(child, complete)
        }.map { (id, child) ->
            val status = if (current) childStatus(child) else "Recorded"
            val kind = when {
                current && childAction(child) -> DesktopBadgeKind.Attention
                current && childState(child) == "working" -> DesktopBadgeKind.Working
                status == "Paused" -> DesktopBadgeKind.Paused
                else -> DesktopBadgeKind.Neutral
            }
            val detail = (child.opt("detail") as? String) ?: text(child, "task")
            SessionCardChild(id, childName(child, id), status, simplified(detail),
                (child.opt("provider") as? String) ?: session.agent, kind, flag(child, "group"))
        }
    }

    /** Compact counter forms such as 3, 2+, 1/3, 2+/5, 3/? and ?; expanded forms are spoken. */
    fun childCount(session: Session, online: Boolean, expanded: Boolean = false, includeCompleted: Boolean = false): String {
        val raw = session.raw
        val source = text(raw, "subagent_source")
        if (!SessionFilters.currentActivity(session, online) || source == "unavailable") return ""
        val count = integer(raw, "subagent_count", 0)
        val active = maxOf(integer(raw, "subagent_active_count", count), count)
        val total = integer(raw, "subagent_total_count", -1)
        val complete = flag(raw, "subagent_counts_complete")
        if (source == "hook_profiles" && !complete) return if (expanded) "Observed runs: incomplete counts" else "?"
        if (!includeCompleted) {
            if (source == "hooks" && !complete && total > 0)
                return if (expanded) "$active confirmed active; some states unknown" else "$active+"
            if (active > 0) return if (expanded) "$active active" else "$active"
            return ""
        }
        if (source == "hooks" && !complete && total > 0)
            return if (expanded) "$active confirmed active / $total observed; some states unknown" else "$active+/$total"
        if (total > 0)
            return if (expanded) (if (source == "hook_profiles") "$active active / $total observed runs" else "$active active / $total total") else "$active/$total"
        if (total < 0 && source == "hook_profiles") return if (expanded) "Observed runs: count unavailable" else "?"
        if (total < 0 && active > 0) return if (expanded) "$active active / total unknown" else "$active/?"
        return ""
    }

    /** A counter without a count still opens visible rows, labeled Agents. */
    fun counter(session: Session, online: Boolean, children: List<SessionCardChild>,
        includeCompleted: Boolean = false): SessionChildCounter? {
        val count = childCount(session, online, false, includeCompleted)
        val expandable = children.isNotEmpty()
        val label = if (count.isEmpty() && expandable) "Agents" else count
        if (label.isEmpty()) return null
        val spoken = childCount(session, online, true, includeCompleted)
        return SessionChildCounter(label, if (spoken.isEmpty()) "Subagents" else "Subagents, $spoken", expandable)
    }

    /** Native child inspection accepts only these agent identities. */
    fun openableAgent(id: String): Boolean = id.isNotBlank() && id != "main" && id.length <= 256 &&
        id.none { it.isISOControl() || it == '/' || it == '\\' }
}
