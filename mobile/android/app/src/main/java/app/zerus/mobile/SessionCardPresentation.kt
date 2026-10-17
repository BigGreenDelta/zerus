package app.zerus.mobile

import org.json.JSONObject

/** One subagent row beneath an expanded session card. */
data class SessionCardChild(val id: String, val name: String, val status: String, val detail: String,
    val provider: String, val kind: DesktopBadgeKind, val group: Boolean) {
    val description: String get() = "Subagent $name, $status"
}

/** The card's subagent counter: compact label, spoken form, and whether it shows child rows. */
data class SessionChildCounter(val label: String, val description: String, val expandable: Boolean)

/** The card's optional-question chip: compact count and its spoken form. Information only. */
data class OptionalQuestionChip(val count: Int, val label: String, val description: String)

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

    /** The branch, or detached HEAD. Ordinary folders have none. */
    fun branch(raw: JSONObject): String {
        if (text(raw, "git_metadata_state") == "not_repo") return ""
        return text(raw, "git_branch").ifEmpty { if (flag(raw, "git_detached")) "detached HEAD" else "" }
    }

    private fun linked(raw: JSONObject): Boolean = flag(raw, "git_worktree") && text(raw, "git_metadata_state") != "not_repo"

    // Path components of a local or remote path, whichever separator its machine uses.
    private fun parts(path: String): List<String> = path.split('/', '\\').filter { it.isNotEmpty() }

    /** checkoutFolder: a linked worktree names its main checkout (`zerus`, not `zerus-work-1a2b`). */
    fun folder(raw: JSONObject): String {
        val common = if (linked(raw)) parts(text(raw, "git_common_dir")) else emptyList()
        val repository = (if (common.size > 1 && common.last().startsWith('.')) common.dropLast(1) else common).lastOrNull().orEmpty()
            .let { if (it.length > 4 && it.endsWith(".git")) it.dropLast(4) else it }
        if (repository.isNotEmpty()) return repository
        val root = if (text(raw, "git_metadata_state") == "not_repo") "" else text(raw, "git_root")
        return parts(root.ifEmpty { text(raw, "cwd").ifEmpty { text(raw, "cwd_canonical") } }).lastOrNull().orEmpty()
    }

    /** The card's checkout line: folder / branch (or detached HEAD); an ordinary folder shows its name. */
    fun line(raw: JSONObject): String = listOf(folder(raw), branch(raw)).filter { it.isNotEmpty() }.joinToString(" / ")

    /** projectContext: the card line plus a linked-worktree mark, as the desktop header and Details show it. */
    fun checkout(raw: JSONObject): String = line(raw).let { if (linked(raw) && it.isNotEmpty()) "$it · worktree" else it }

    /** The spoken checkout line keeps the linked worktree's own folder. */
    fun checkoutDescription(raw: JSONObject): String {
        val folder = folder(raw)
        val branch = branch(raw)
        val name = text(raw, "git_worktree_name")
        val worktree = when {
            branch.isEmpty() || !linked(raw) -> ""
            name.isEmpty() -> ", linked worktree"
            else -> ", linked worktree $name"
        }
        return listOf(if (folder.isEmpty()) "" else "Folder $folder", when (branch) {
            "" -> ""
            "detached HEAD" -> "Detached HEAD"
            else -> "Git branch $branch"
        }).filter { it.isNotEmpty() }.joinToString(", ") + worktree
    }

    fun childState(child: JSONObject): String = text(child, "display_state").ifEmpty { text(child, "state") }

    fun inactiveChild(child: JSONObject, countsComplete: Boolean = false): Boolean {
        val state = childState(child)
        // A failed turn has ended too; its error belongs to the parent's result.
        val inactive = state == "finished" || state == "idle" || state == "ready" || state == "error"
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

    /** The expanded rows. Finished, idle, ready and failed turns are hidden unless completed children are included. */
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
            if (source != "hook_profiles" && !complete && total > 0)
                return if (expanded) "$active confirmed active; some states unknown" else "$active+"
            if (active > 0) return if (expanded) "$active active" else "$active"
            return ""
        }
        if (source != "hook_profiles" && !complete && total > 0)
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

    /**
     * Pending optional Codex questions in the session's current conversation: the chip count shared
     * with the desktop through optional-question-contract.json. The listing's strict
     * `optional_questions.count` wins. Without it (an older host, or null while hgs could not read
     * its question index), the optional `mobile_attention` items published by the connector are
     * counted once per question ID. A saved, archived, exited or offline session shows none.
     * Optional questions are not attention: this count never changes status or filters.
     */
    fun optionalQuestions(session: Session, online: Boolean): Int {
        val raw = session.raw
        if (!online || SessionFilters.archived(session) || SessionFilters.state(session) != "running" ||
            text(raw, "process_state") == "exited") return 0
        (raw.opt("optional_questions") as? JSONObject)?.let { summary ->
            integer(summary, "count", -1).let { if (it >= 0) return it }
        }
        return raw.optJSONArray("mobile_attention")?.objects().orEmpty().filter(QuestionPolicies::optionalAttention)
            .mapNotNull { it.opt("question_id") as? String }.filter { it.isNotBlank() }.distinct().size
    }

    /** The compact chip label: the count up to 99, then 99+, as the desktop chip. */
    fun optionalQuestionLabel(count: Int): String = if (count > 99) "99+" else count.toString()

    fun optionalQuestionDescription(count: Int): String =
        if (count == 1) "1 optional question waiting" else "$count optional questions waiting"

    /** The chip for this card, or null when no optional question is pending. */
    fun optionalQuestionChip(session: Session, online: Boolean): OptionalQuestionChip? =
        optionalQuestions(session, online).takeIf { it > 0 }?.let {
            OptionalQuestionChip(it, optionalQuestionLabel(it), optionalQuestionDescription(it))
        }

    /** Native child inspection accepts only these agent identities. */
    fun openableAgent(id: String): Boolean = id.isNotBlank() && id != "main" && id.length <= 256 &&
        id.none { it.isISOControl() || it == '/' || it == '\\' }
}
