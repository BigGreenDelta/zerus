package app.zerus.mobile

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import app.zerus.mobile.replica.SessionRef
import app.zerus.mobile.replica.TaskActor
import app.zerus.mobile.replica.TaskInfo
import app.zerus.mobile.replica.TaskLogEntry
import app.zerus.mobile.replica.TaskSettings
import app.zerus.mobile.replica.TaskStage
import app.zerus.mobile.replica.TaskTarget
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** The app's own popup surfaces (spec "Android interface", Colours), not the Material 3 baseline. */
internal object TasksColors {
    val Sheet = Color(0xFF1A2225)
    val Dialog = Color(0xFF1F292C)
    val Snackbar = Color(0xFF2A3538)
    val Handle = Color(0xFF354449)
    val Card = Color(0xFF1A2225)
    val Accent = Color(0xFF67E8CB)
    val Attention = Color(0xFFFFCB7D)
    val Muted = Color(0xFF9AACB2)
    val Danger = Color(0xFFE06C75)
}

private fun stageIcon(tab: TaskTab): ImageVector = when (tab) {
    TaskTab.REVIEW -> Icons.Default.Inbox
    TaskTab.ACTIVE -> Icons.Default.PlayArrow
    TaskTab.TODO -> Icons.AutoMirrored.Filled.PlaylistAdd
    TaskTab.BACKLOG -> Icons.Default.Schedule
    TaskTab.DONE -> Icons.Default.Check
}

private fun actionIcon(action: TaskAction): ImageVector = when (action) {
    TaskAction.EDIT -> Icons.Default.Edit
    TaskAction.TO_BACKLOG -> Icons.Default.Schedule
    TaskAction.TO_TOP -> Icons.Default.KeyboardDoubleArrowUp
    TaskAction.SEND -> Icons.Default.PlayArrow
    TaskAction.TO_TODO, TaskAction.BACK_TO_TODO -> Icons.AutoMirrored.Filled.PlaylistAdd
    TaskAction.CANCEL -> Icons.Default.Close
    TaskAction.ACCEPT -> Icons.Default.Check
}

private fun stageLabel(stage: String) = when (stage) {
    "backlog" -> "Backlog"; "todo" -> "To do"; "active" -> "In progress"; "review" -> "In review"; "done" -> "Done"
    else -> stage
}

private fun time(ms: Long) = SimpleDateFormat("d MMM HH:mm", Locale.getDefault()).format(Date(ms))

/** One history line, as the desktop writes it: who, when and what happened. */
private fun historyLine(entry: TaskLogEntry): Pair<String, String> {
    val who = when (val actor = entry.actor) { is TaskActor.Agent -> actor.session.session; TaskActor.Person -> "You" }
    val what = when (entry.kind) {
        "created" -> "created the task"
        "edited" -> "edited the prompt"
        "stage" -> "moved it to ${stageLabel(entry.text)}"
        "target" -> "changed the session: ${entry.text}"
        "sent" -> "sent it to ${entry.session?.session.orEmpty()}"
        "report" -> "reported: ${entry.text}"
        "cancelled" -> "cancelled it"
        "comment" -> "commented: ${entry.text}"
        else -> entry.text.ifBlank { entry.kind }
    }
    return "$who  ${time(entry.at)}" to what
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun SheetHandle() = BottomSheetDefaults.DragHandle(color = TasksColors.Handle)

@Composable private fun StatusDot(dot: ChipDot) {
    val color = when (dot) { ChipDot.WORKING -> TasksColors.Accent; ChipDot.ATTENTION -> TasksColors.Attention; ChipDot.READY -> TasksColors.Muted }
    Box(Modifier.size(8.dp).background(color, CircleShape))
}

/** The project's tasks full-screen, as Terminal opens; Back returns to the conversation. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun TasksScreen(model: ZerusViewModel) {
    val key = model.tasksKey ?: return
    val chosen = model.tasksChosen
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    LaunchedEffect(Unit) {
        snapshotFlow { model.taskNotice }.collect { message ->
            if (message != null) { model.taskNotice = null; scope.launch { snackbar.showSnackbar(message) } }
        }
    }
    var openTask by remember(key) { mutableStateOf<String?>(null) }
    var cancelling by remember(key) { mutableStateOf<TaskInfo?>(null) }
    var projectSettings by remember(key) { mutableStateOf(false) }
    var allSessions by remember { mutableStateOf(false) }
    val tasks = model.taskListing
    val counts = TasksPresentation.counts(tasks, chosen, model.tasksOnlyThisSession)
    val chips = model.taskChips(key)
    fun act(task: TaskInfo, state: ActionState) {
        when {
            state.action == TaskAction.EDIT -> openTask = task.uuid
            !state.enabled -> model.taskNotice = state.reason
            state.action == TaskAction.CANCEL -> cancelling = task
            else -> model.taskAction(task, state.action)
        }
    }
    Dialog(onDismissRequest = { model.closeTasks() },
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            Scaffold(containerColor = MaterialTheme.colorScheme.background, topBar = {
                TopAppBar(title = {
                    Column {
                        Text("Tasks", maxLines = 1)
                        Text(model.taskProjectName(key), maxLines = 1, overflow = TextOverflow.Ellipsis,
                            style = MaterialTheme.typography.labelMedium, color = TasksColors.Muted)
                    }
                }, navigationIcon = { IconButton(onClick = { model.closeTasks() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } },
                    actions = {
                        val only = model.tasksOnlyThisSession
                        IconButton(onClick = { model.setOnlyThisSession(!only) }, modifier = Modifier.semantics {
                            contentDescription = "Only this session"; stateDescription = if (only) "On" else "Off"
                        }) { Icon(Icons.Default.FilterList, null, tint = if (only) TasksColors.Accent else LocalContentColor.current) }
                        IconButton(onClick = { projectSettings = true }) { Icon(Icons.Default.Settings, "Project task settings") }
                    }, colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background))
            }, snackbarHost = {
                SnackbarHost(snackbar) { Snackbar(it, containerColor = TasksColors.Snackbar, contentColor = MaterialTheme.colorScheme.onSurface) }
            }, bottomBar = {
                TaskInput(model, Modifier.navigationBarsPadding().imePadding().padding(horizontal = 12.dp, vertical = 8.dp))
            }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize()) {
                    Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 12.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        chips.forEach { chip ->
                            FilterChip(selected = chip.session == chosen, onClick = { model.chooseTaskSession(key, chip.session) },
                                label = { Text(chip.session.name, maxLines = 1) }, leadingIcon = { StatusDot(chip.dot) })
                        }
                        AssistChip(onClick = { allSessions = true }, label = { Icon(Icons.Default.MoreHoriz, "Sessions of every project", Modifier.size(18.dp)) })
                    }
                    val selected = TaskTab.entries.indexOf(model.taskTab)
                    PrimaryScrollableTabRow(selectedTabIndex = selected, edgePadding = 0.dp, containerColor = MaterialTheme.colorScheme.background) {
                        TaskTab.entries.forEach { tab ->
                            val count = if (tab == TaskTab.DONE) model.doneRecent.takeIf { model.doneLoaded } else counts[tab]
                            Tab(model.taskTab == tab, { model.selectTaskTab(tab) },
                                text = { Text(if (count != null && count > 0) "${tab.label} $count" else tab.label, maxLines = 1, softWrap = false) })
                        }
                    }
                    if (model.taskBusy) LinearProgressIndicator(Modifier.fillMaxWidth())
                    if (model.taskTab == TaskTab.DONE) DoneList(model, Modifier.weight(1f), onOpen = { openTask = it.uuid })
                    else {
                        val cards = TasksPresentation.cards(tasks, model.taskTab, chosen, model.tasksOnlyThisSession, model::sendRoute, model::cancelRoute)
                        LazyColumn(Modifier.weight(1f), contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                            if (cards.isEmpty()) item { Text("Nothing ${model.taskTab.label.replaceFirstChar { it.lowercase() }}.",
                                Modifier.padding(8.dp), color = TasksColors.Muted) }
                            items(cards, key = { it.uuid }) { card ->
                                val task = tasks.first { it.uuid == card.uuid }
                                TaskCardView(card, onOpen = { openTask = card.uuid }, onAction = { act(task, it) },
                                    badge = if (card.tab == TaskTab.ACTIVE) ({ ActiveSessionBadge(model, key, task.session) }) else null)
                            }
                        }
                    }
                }
            }
            openTask?.let { uuid -> tasks.firstOrNull { it.uuid == uuid }?.let { task ->
                TaskSettingsSheet(model, key, task, onDismiss = { openTask = null }, onCancel = { openTask = null; cancelling = task })
            } }
            cancelling?.let { task ->
                val direct = ((model.cancelRoute(task) as? RouteState.Ready)?.route?.direct) ?: true
                CancelTaskDialog(task, direct, onDismiss = { cancelling = null }) { reason -> cancelling = null; model.cancelTask(task, reason) }
            }
            if (projectSettings) ProjectTaskSettingsSheet(model, onDismiss = { projectSettings = false })
            if (allSessions) AllSessionsSheet(model, onDismiss = { allSessions = false })
        }
    }
}

@Composable private fun ActiveSessionBadge(model: ZerusViewModel, key: ProjectKey, ref: SessionRef?) {
    val session = ref?.let { r -> model.projectSessions(key).firstOrNull { s ->
        s.target.session == r.session && model.machines.any { it.id == s.target.computerId && it.machineId == r.machine }
    } } ?: return
    SessionBadge(session, SessionFilters.online(session, model.machines), Modifier.padding(bottom = 4.dp))
}

/** The session card style; the header keeps its height, so its icons never move the text. */
@Composable private fun TaskCardView(card: TaskCard, onOpen: () -> Unit, onAction: (ActionState) -> Unit, badge: (@Composable () -> Unit)? = null) {
    Card(onClick = onOpen, shape = RoundedCornerShape(14.dp), colors = CardDefaults.cardColors(containerColor = TasksColors.Card),
        border = if (card.highlighted) BorderStroke(1.5.dp, TasksColors.Accent) else null) {
        Column(Modifier.fillMaxWidth().padding(start = 14.dp, end = 4.dp, bottom = 12.dp)) {
            Row(Modifier.fillMaxWidth().height(44.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Icon(stageIcon(card.tab), null, Modifier.size(16.dp), tint = TasksColors.Muted)
                Text(card.id, style = MaterialTheme.typography.labelMedium, color = TasksColors.Muted, maxLines = 1)
                Text(card.sessionLabel, Modifier.weight(1f), style = MaterialTheme.typography.labelMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    color = if (card.highlighted) TasksColors.Accent else TasksColors.Muted)
                card.actions.forEach { state ->
                    // A disabled action still answers a tap with its reason.
                    IconButton(onClick = { onAction(state) }, modifier = Modifier.size(40.dp).semantics {
                        if (!state.enabled) stateDescription = "Unavailable: ${state.reason}"
                    }) {
                        Icon(actionIcon(state.action), state.action.label, Modifier.size(20.dp),
                            tint = if (state.enabled) MaterialTheme.colorScheme.onSurface else TasksColors.Muted.copy(alpha = .45f))
                    }
                }
            }
            badge?.invoke()
            Text(card.text, Modifier.padding(end = 10.dp), maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
            if (card.note.isNotBlank()) Text(card.note, Modifier.padding(top = 6.dp, end = 10.dp), maxLines = 3, overflow = TextOverflow.Ellipsis,
                style = MaterialTheme.typography.bodySmall, color = TasksColors.Muted)
        }
    }
}

/** Done from a computer: the last 7 days, then "Show older (N)". A tap fetches the task, then opens it. */
@Composable private fun DoneList(model: ZerusViewModel, modifier: Modifier, onOpen: (TaskInfo) -> Unit) {
    val rows = if (model.doneExpanded) model.doneRows else model.doneRows.take(model.doneRecent)
    val older = TasksPresentation.olderLabel(model.doneTotal, model.doneRecent, rows.size, model.doneExpanded, model.doneNext != null)
    LazyColumn(modifier, contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (model.doneError.isNotBlank()) item {
            Column(Modifier.padding(8.dp)) {
                Text(model.doneError, color = TasksColors.Muted)
                TextButton(onClick = { model.loadDone(more = false) }, enabled = !model.doneLoading) { Text("Try again") }
            }
        }
        if (rows.isEmpty() && model.doneLoaded && model.doneError.isBlank()) item {
            Text(if (model.doneTotal > 0) "Nothing done in the last 7 days." else "Nothing done.", Modifier.padding(8.dp), color = TasksColors.Muted)
        }
        items(rows, key = { it.uuid }) { row ->
            val card = TaskCard(row.uuid, row.id, row.text, TaskTab.DONE, row.session?.session ?: "any session", false, "",
                listOf(ActionState(TaskAction.BACK_TO_TODO, !model.taskBusy, "Another task action is running")))
            TaskCardView(card, onOpen = { model.openDoneTask(row, onOpen) }, onAction = { state ->
                if (!state.enabled) model.taskNotice = state.reason
                else model.openDoneTask(row) { task -> model.taskAction(task, TaskAction.BACK_TO_TODO) }
            })
        }
        if (model.doneLoading) item { LinearProgressIndicator(Modifier.fillMaxWidth().padding(8.dp)) }
        else if (older != null) item { TextButton(onClick = { model.loadDone(more = true) }) { Text(older) } }
    }
}

/** The app's message input without the attach button; nothing is sent to the agent. */
@Composable private fun TaskInput(model: ZerusViewModel, modifier: Modifier) {
    val editor = rememberTextFieldState()
    val focus = remember { FocusRequester() }
    val stage = if (model.taskTab == TaskTab.BACKLOG) "Backlog" else "To do"
    val text = editor.text.toString()
    val reason = TasksPresentation.addReason(model.tasksChosen)
    val canAdd = text.isNotBlank() && reason.isEmpty()
    CompactMessageInput(state = editor, placeholder = "Add to $stage", readOnly = false, inputTransformation = InputTransformation { },
        focusRequester = focus, onFocusChanged = { }, modifier = modifier, leading = { Spacer(Modifier.width(8.dp)) }, action = {
            // A session that cannot take tasks answers the tap with why.
            ComposerActionButton(onClick = { if (canAdd) { model.addTask(text.trim()); editor.clearText() } else model.taskNotice = reason },
                enabled = text.isNotBlank(),
                containerColor = if (canAdd) TasksColors.Accent else Color(0xFF2B3B3B),
                contentColor = if (canAdd) MaterialTheme.colorScheme.background else TasksColors.Muted,
                disabledContainerColor = Color(0xFF2B3B3B), disabledContentColor = TasksColors.Muted) {
                if (text.isNotBlank() && !canAdd) Icon(Icons.Default.Info, "Why the task cannot be added", Modifier.size(22.dp))
                else Icon(Icons.AutoMirrored.Filled.PlaylistAdd, "Add to $stage", Modifier.size(22.dp))
            }
        })
}

/** The task settings: the prompt, the session, the stage's actions, the history and a comment. */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable private fun TaskSettingsSheet(model: ZerusViewModel, key: ProjectKey, task: TaskInfo, onDismiss: () -> Unit, onCancel: () -> Unit) {
    val tab = TasksPresentation.tab(task.stage)
    val editable = task.writable && task.stage in listOf(TaskStage.TODO, TaskStage.BACKLOG)
    var text by remember(task.uuid, task.text) { mutableStateOf(task.text) }
    var target by remember(task.uuid, task.target) { mutableStateOf(task.target) }
    var comment by remember(task.uuid) { mutableStateOf("") }
    var choosing by remember { mutableStateOf(false) }
    val textChanged = text != task.text
    val targetChanged = target != task.target
    // Send now's reason follows the session chosen here, before it is saved.
    val draft = task.copy(target = target)
    val actions = TasksPresentation.cards(listOf(draft), tab, model.tasksChosen, false, model::sendRoute, model::cancelRoute)
        .single().actions.filter { it.action != TaskAction.EDIT }
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = TasksColors.Sheet, dragHandle = { SheetHandle() }) {
        Column(Modifier.fillMaxWidth().fillMaxHeight(.9f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp).padding(bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Icon(stageIcon(tab), null, Modifier.size(18.dp), tint = TasksColors.Muted)
                Text(task.id, Modifier.weight(1f), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                Text(tab.label, style = MaterialTheme.typography.labelMedium, color = TasksColors.Muted)
            }
            if (editable) OutlinedTextField(text, { text = it }, Modifier.fillMaxWidth(), label = { Text("Prompt") }, minLines = 3, maxLines = 12)
            else SelectionContainer { Text(task.text, style = MaterialTheme.typography.bodyMedium) }
            val sessionLabel = (target as? TaskTarget.Session)?.session?.session ?: "Any session"
            if (editable) OutlinedButton(onClick = { choosing = true }, Modifier.fillMaxWidth()) {
                Text("Session: $sessionLabel", Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                Icon(Icons.Default.ArrowDropDown, null)
            } else Text("Session: ${TasksPresentation.sessionLabel(task)}", style = MaterialTheme.typography.bodyMedium, color = TasksColors.Muted)
            if (TasksPresentation.note(task).isNotBlank()) Text(TasksPresentation.note(task), style = MaterialTheme.typography.bodySmall, color = TasksColors.Muted)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                actions.forEach { state ->
                    OutlinedButton(onClick = {
                        if (state.action == TaskAction.CANCEL) onCancel()
                        else { model.saveThen(task, text.takeIf { textChanged }, target.takeIf { targetChanged }, state.action); onDismiss() }
                    }, enabled = state.enabled) {
                        Icon(actionIcon(state.action), null, Modifier.size(18.dp)); Spacer(Modifier.width(6.dp)); Text(state.action.label)
                    }
                }
            }
            actions.filter { !it.enabled && it.reason.isNotBlank() }.forEach {
                Text("${it.action.label}: ${it.reason}", style = MaterialTheme.typography.bodySmall, color = TasksColors.Muted)
            }
            if (textChanged || targetChanged) Button(onClick = {
                model.saveTask(task.uuid, text.takeIf { textChanged }, target.takeIf { targetChanged })
            }, enabled = text.isNotBlank()) { Text("Save") }
            HorizontalDivider(color = TasksColors.Handle)
            Text("History", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
            task.log.forEach { entry ->
                val (who, what) = historyLine(entry)
                Column {
                    Text(who, style = MaterialTheme.typography.labelMedium, color = TasksColors.Muted)
                    SelectionContainer { Text(what, style = MaterialTheme.typography.bodySmall) }
                }
            }
            if (task.writable) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(comment, { comment = it }, Modifier.weight(1f), label = { Text("Comment") }, maxLines = 4)
                TextButton(onClick = { model.commentTask(task.uuid, comment.trim()); comment = "" }, enabled = comment.isNotBlank()) { Text("Add") }
            }
        }
    }
    if (choosing) TaskSessionSheet(model, key, onDismiss = { choosing = false }) { picked -> target = picked; choosing = false }
}

/** Any session or one of the project's sessions, for a task in To do or Backlog. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun TaskSessionSheet(model: ZerusViewModel, key: ProjectKey, onDismiss: () -> Unit, onPick: (TaskTarget) -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss, containerColor = TasksColors.Sheet, dragHandle = { SheetHandle() }) {
        Column(Modifier.fillMaxWidth().padding(bottom = 16.dp)) {
            ListItem(headlineContent = { Text("Any session") }, modifier = Modifier.clickable { onPick(TaskTarget.AnySession) },
                colors = ListItemDefaults.colors(containerColor = TasksColors.Sheet))
            model.taskChips(key).filter { it.session.machineId.isNotBlank() }.forEach { chip ->
                ListItem(headlineContent = { Text(chip.session.name) }, leadingContent = { StatusDot(chip.dot) },
                    modifier = Modifier.clickable { onPick(TaskTarget.Session(SessionRef(chip.session.machineId, chip.session.name))) },
                    colors = ListItemDefaults.colors(containerColor = TasksColors.Sheet))
            }
        }
    }
}

/** The ⋯ chip: the sessions of every project; one of another project shows that project's tasks. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun AllSessionsSheet(model: ZerusViewModel, onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss, containerColor = TasksColors.Sheet, dragHandle = { SheetHandle() }) {
        LazyColumn(Modifier.fillMaxWidth(), contentPadding = PaddingValues(bottom = 16.dp)) {
            model.projects.filter { it.catalogued }.forEach { project ->
                val chips = model.taskChips(project.key)
                if (chips.isNotEmpty()) {
                    item(key = "project:${project.key}") {
                        Text(project.name, Modifier.padding(horizontal = 20.dp, vertical = 8.dp), style = MaterialTheme.typography.titleSmall,
                            fontWeight = FontWeight.SemiBold, color = TasksColors.Muted)
                    }
                    items(chips, key = { "session:${project.key}:${it.session.computerId}:${it.session.name}" }) { chip ->
                        ListItem(headlineContent = { Text(chip.session.name) }, leadingContent = { StatusDot(chip.dot) },
                            modifier = Modifier.clickable { model.chooseTaskSession(project.key, chip.session); onDismiss() },
                            colors = ListItemDefaults.colors(containerColor = TasksColors.Sheet))
                    }
                }
            }
        }
    }
}

/** The desktop's cancel dialog: the text, what will happen, the optional reason. When the session's computer is
 * out of reach, another computer cancels it there without the cancel message. */
@Composable private fun CancelTaskDialog(task: TaskInfo, direct: Boolean, onDismiss: () -> Unit, onConfirm: (String) -> Unit) {
    var reason by remember(task.uuid) { mutableStateOf("") }
    val session = task.session?.session ?: "the session"
    AlertDialog(onDismissRequest = onDismiss, containerColor = TasksColors.Dialog, title = { Text("Cancel ${task.id}?") }, text = {
        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(task.text, maxLines = 4, overflow = TextOverflow.Ellipsis)
            Text(if (direct) "${task.id} moves to In review, and $session gets the cancel message."
                else "${task.id} moves to In review. The computer of $session is out of reach, so $session gets no cancel message.",
                style = MaterialTheme.typography.bodySmall,
                color = TasksColors.Muted)
            OutlinedTextField(reason, { reason = it }, Modifier.fillMaxWidth(), label = { Text("Reason (optional)") },
                supportingText = { Text("Saved as a comment on the task, not sent to the agent") }, maxLines = 4)
        }
    }, confirmButton = { TextButton(onClick = { onConfirm(reason.trim()) }) { Text("Cancel task", color = TasksColors.Danger) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Keep running") } })
}

/** The project's task settings: the templates, agents acting on other sessions and their hourly limit. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun ProjectTaskSettingsSheet(model: ZerusViewModel, onDismiss: () -> Unit) {
    var loaded by remember { mutableStateOf<TaskSettings?>(null) }
    LaunchedEffect(model.tasksKey) { loaded = model.taskSettings() }
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        containerColor = TasksColors.Sheet, dragHandle = { SheetHandle() }) {
        val original = loaded
        if (original == null) { Text("Reading the project's task settings…", Modifier.padding(20.dp)); return@ModalBottomSheet }
        var task by remember(original) { mutableStateOf(original.taskTemplate) }
        var cancel by remember(original) { mutableStateOf(original.cancelTemplate) }
        var agentsOther by remember(original) { mutableStateOf(original.agentsOther) }
        var limit by remember(original) { mutableStateOf(original.agentLimit.toString()) }
        val parsedLimit = limit.trim().ifEmpty { "0" }.toLongOrNull()?.takeIf { it in 0..1000 }
        val edited = TaskSettings(task, cancel, agentsOther, parsedLimit ?: original.agentLimit)
        Column(Modifier.fillMaxWidth().fillMaxHeight(.9f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp).padding(bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Project tasks · ${model.tasksKey?.let(model::taskProjectName).orEmpty()}", style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold)
            OutlinedTextField(task, { task = it }, Modifier.fillMaxWidth(), label = { Text("Task prompt (Send now)") }, minLines = 4, maxLines = 12)
            OutlinedTextField(cancel, { cancel = it }, Modifier.fillMaxWidth(), label = { Text("Cancel message") }, minLines = 3, maxLines = 10)
            Text("Placeholders: {ref}, {author}, {prompt}, {tools}, {history}", style = MaterialTheme.typography.bodySmall, color = TasksColors.Muted)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Agents may start and cancel tasks in other sessions", Modifier.weight(1f))
                Switch(agentsOther, { agentsOther = it })
            }
            OutlinedTextField(limit, { limit = it.filter(Char::isDigit).take(4) }, Modifier.fillMaxWidth(),
                label = { Text("Limit tasks started by agents, per hour") }, singleLine = true, isError = parsedLimit == null,
                supportingText = { Text(if (parsedLimit == null) "From 0 to 1000" else "0 is no limit") })
            if (edited != original) Button(onClick = { model.saveTaskSettings(original, edited); onDismiss() }, enabled = parsedLimit != null) { Text("Save") }
        }
    }
}
