package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AddTask
import androidx.compose.material.icons.filled.Checklist
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import org.json.JSONObject

private val E = OvermindPalette

/** Projects → project → Tasks, with the gateway digest's Queue and Your Call counts. Hidden without Tasks. */
@Composable internal fun OvermindProjectTasksButton(model: ZerusViewModel, project: ProjectSummary) {
    val id = model.overmindProject(project) ?: return
    val connectionId = project.key.connectionId
    OutlinedButton(onClick = { model.tasks.openBoard(connectionId, id) }, modifier = Modifier.fillMaxWidth()) {
        Icon(Icons.Default.Checklist, null); Spacer(Modifier.width(8.dp)); Text("Tasks")
        Spacer(Modifier.weight(1f))
        OvermindBadgeRow(OvermindBadges.project(model.overmindDigest(connectionId), id), compact = true)
    }
}

@Composable internal fun OvermindProjectBadges(model: ZerusViewModel, project: ProjectSummary) {
    val id = model.overmindProject(project) ?: return
    OvermindBadgeRow(OvermindBadges.project(model.overmindDigest(project.key.connectionId), id), compact = true)
}

/** A session's own queue and Your Call, from its project's board when the phone has read it. */
@Composable internal fun OvermindSessionBadges(model: ZerusViewModel, session: Session) {
    val project = model.overmindProject(session) ?: return
    OvermindBadgeRow(OvermindBadges.session(model.tasks.board(session.target.connectionId, project)?.board, model.overmindSession(session)), compact = true)
}

/** Boards behind the session badges: only projects whose digest shows queued or waiting tasks, a few at a time, read again as the digest moves. */
@Composable internal fun OvermindWatchBoards(model: ZerusViewModel, sessions: List<Session>) {
    val watched = sessions.mapNotNull { session -> model.overmindProject(session)?.let { session.target.connectionId to it } }.distinct()
        .filter { (connection, project) -> OvermindBadges.project(model.overmindDigest(connection), project)?.empty == false }.take(8)
    val revs = watched.map { model.overmindDigest(it.first)?.rev }
    LaunchedEffect(watched, revs) { watched.forEachIndexed { index, (connection, project) -> model.tasks.load(connection, project, revs[index]) } }
}

/** Add as task: this session's queue (pinned to it), the project pool or Backlog. Nothing goes to an agent. */
@Composable internal fun OvermindAddAsTask(model: ZerusViewModel, session: Session, beforeAdd: () -> Unit) {
    val target = session.target
    if (target.agentId.isNotBlank() || model.overmindProject(session) == null) return
    var menu by remember { mutableStateOf(false) }
    val draft = model.draft(target)
    // An add still running, or a create of this prompt that may have been recorded, blocks another one (see addTaskReason).
    val reason = when {
        model.demo -> "Pair to add tasks."
        !model.storageReady || model.updateInstallPreparing -> "Private storage is not ready."
        draft.status != "editing" -> "Review the saved draft first."
        model.pickerDraft?.target == target -> "Wait for file selection to finish."
        else -> model.addTaskReason(target)
    }
    val known = model.overmindSession(session) != null
    Box {
        IconButton(onClick = { beforeAdd(); menu = true }, enabled = !model.demo) { Icon(Icons.Default.AddTask, "Add as task", tint = E.Muted) }
        DropdownMenu(menu, { menu = false }) {
            Text("Add as task", Modifier.padding(horizontal = 16.dp, vertical = 6.dp), fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelLarge)
            if (reason.isNotBlank()) Text(reason, Modifier.widthIn(max = 280.dp).padding(horizontal = 16.dp, vertical = 4.dp), color = E.Amber,
                style = MaterialTheme.typography.bodySmall)
            OvermindDestination.entries.forEach { destination ->
                val unknown = destination == OvermindDestination.Queue && !known
                DropdownMenuItem(text = { Column {
                    Text(destination.label)
                    if (unknown) Text(OvermindRequests.UNKNOWN_MACHINE, color = E.Muted, style = MaterialTheme.typography.labelSmall)
                } }, enabled = reason.isBlank() && !unknown, onClick = { menu = false; beforeAdd(); model.addAsTask(target, destination) })
            }
        }
    }
}

/** What Add as task did, under the composer; a confirmation fades, a problem stays until dismissed. */
@Composable internal fun OvermindComposerNotice(model: ZerusViewModel, session: Session) {
    val key = session.target.key
    val notice = model.tasks.notices[key] ?: return
    LaunchedEffect(notice) { if (notice.tone == OvermindTone.Info && notice.text != "Adding the task…") { delay(6_000); if (model.tasks.notices[key] == notice) model.tasks.dismissNotice(key) } }
    OvermindOutcomeBar(notice, onAnyway = {}, onResolve = { task ->
        model.tasks.dismissNotice(key)
        model.overmindProject(session)?.let { model.tasks.openBoard(session.target.connectionId, it) }
        model.tasks.openTask(session.target.connectionId, task)
    }, onRetry = { write -> model.retryAddAsTask(session.target, write) }, onDismiss = { model.tasks.dismissNotice(key) },
        retrying = model.overmindWrites.any { it.status == "sending" && it.requestId == notice.retry?.requestId })
}

private fun actionName(action: String) = when (action) {
    "create" -> "New task"; "edit" -> "Edit"; "move" -> "Move"; "reorder" -> "Reorder"; "pin" -> "Pin"; "unpin" -> "Unpin"; "label" -> "Label"
    "link" -> "Link"; "comment" -> "Comment"; "send" -> "Send to a session"; "review-send" -> "Send for review"; "rework-send" -> "Send rework"
    "answer" -> "Answer"; "resolve-delivery" -> "Resolve delivery"; "done" -> "Done"; "reopen" -> "Reopen"; "delete" -> "Delete"; else -> action
}

/** Saved drafts: task changes that are unconfirmed or failed. Retry repeats the same request; nothing repeats on its own. */
@Composable internal fun OvermindTaskChanges(model: ZerusViewModel) {
    val writes = model.overmindWrites.filter { it.pending || it.status == "failed" }.sortedByDescending { it.createdAt }
    if (writes.isEmpty()) return
    var dismiss by remember { mutableStateOf<OvermindWrite?>(null) }
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Task changes", style = MaterialTheme.typography.titleMedium)
        writes.forEach { write ->
            Card(colors = CardDefaults.cardColors(containerColor = E.Surface)) { Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                val ref = runCatching { OvermindParser.card(JSONObject(write.result))?.ref }.getOrNull()?.ifBlank { null } ?: write.task.take(8)
                Text(listOf(actionName(write.action), ref).filter { it.isNotBlank() }.joinToString(" · "), fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                OvermindChip(when { write.status == "sending" -> "Sending…"; write.needsResolution -> "May have been sent"; write.needsCheck -> "Check the task"
                    write.pending -> "Unconfirmed"; else -> "Not made" }, if (write.status == "sending") E.Mint else E.Amber)
                if (write.error.isNotBlank()) Text(write.error, color = E.Amber, style = MaterialTheme.typography.bodySmall)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (write.retryable) TextButton(onClick = { model.retryOvermindWrite(write) }) { Text("Retry") }
                    // A create that surely failed made no task to open.
                    if (write.task.isNotBlank() && model.tasksAvailable(write.connectionId) && write.action != "delete" && !(write.action == "create" && !write.pending))
                        TextButton(onClick = { model.tasks.openTask(write.connectionId, write.task, project = "") }) {
                            Text(if (write.needsResolution) "Resolve delivery" else "Open task")
                        }
                    if (write.status != "sending") TextButton(onClick = { if (write.pending) dismiss = write else model.reviewOvermindWrite(write) }) { Text("Dismiss") }
                }
            } }
        }
    }
    dismiss?.let { write -> OvermindConfirm(OvermindQuestion("Dismiss this change?",
        "It may already have been applied. Dismissing it removes it from this phone; it is not retried.", "Dismiss"), { model.reviewOvermindWrite(write) }) { dismiss = null } }
}
