package app.zerus.mobile

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.zIndex
import kotlinx.coroutines.launch
import kotlin.math.abs

internal object OvermindPalette {
    val Mint = Color(0xFF67E8CB)
    val Amber = Color(0xFFFFCB7D)
    val Background = Color(0xFF101517)
    val Surface = Color(0xFF1A2225)
    val Raised = Color(0xFF243136)
    val Muted = Color(0xFF9AACB2)
    val Danger = Color(0xFFFF9A8B)
}

@Composable internal fun OvermindChip(text: String, color: Color = OvermindPalette.Mint, modifier: Modifier = Modifier) {
    Surface(color = color.copy(alpha = .12f), shape = RoundedCornerShape(6.dp), modifier = modifier) {
        Text(text, Modifier.padding(horizontal = 7.dp, vertical = 3.dp), color = color, style = MaterialTheme.typography.labelSmall, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
    }
}

/** Queue and Your Call counts, hidden when both are zero. */
@Composable internal fun OvermindBadgeRow(badge: OvermindBadge?, compact: Boolean = false) {
    if (badge == null || badge.empty) return
    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        if (badge.queue > 0) OvermindChip(if (compact) "Queue ${badge.queue}" else "${badge.queue} in Queue", OvermindPalette.Muted)
        if (badge.yourCall > 0) OvermindChip("Your Call ${badge.yourCall}", OvermindPalette.Amber)
    }
}

/** A write's settlement and the next step it offers. Nothing is repeated without a tap here. */
@Composable internal fun OvermindOutcomeBar(outcome: OvermindOutcome?, onAnyway: (OvermindAnyway) -> Unit, onResolve: (String) -> Unit,
    onRetry: (OvermindWrite) -> Unit, onDismiss: () -> Unit, retrying: Boolean = false) {
    outcome ?: return
    val color = when (outcome.tone) { OvermindTone.Info -> OvermindPalette.Mint; OvermindTone.Warning -> OvermindPalette.Amber; OvermindTone.Error -> OvermindPalette.Danger }
    Surface(color = color.copy(alpha = .1f), shape = RoundedCornerShape(10.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(start = 12.dp, top = 8.dp, bottom = 4.dp, end = 4.dp)) {
            Row(verticalAlignment = Alignment.Top) {
                Text(outcome.text, Modifier.weight(1f).padding(top = 4.dp), color = color, style = MaterialTheme.typography.bodySmall)
                IconButton(onClick = onDismiss, modifier = Modifier.size(32.dp)) { Icon(Icons.Default.Close, "Dismiss message", Modifier.size(18.dp)) }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                outcome.anyway?.let { offer -> TextButton(onClick = { onAnyway(offer) }) { Text(offer.label) } }
                if (outcome.resolve.isNotEmpty()) TextButton(onClick = { onResolve(outcome.resolve) }) { Text("Resolve delivery") }
                if (outcome.check.isNotEmpty()) TextButton(onClick = { onResolve(outcome.check) }) { Text("Check the task") }
                outcome.retry?.let { write -> TextButton(onClick = { onRetry(write) }, enabled = !retrying) { Text("Retry") } }
            }
        }
    }
}

/** A destructive or surprising change asks first; Cancel is the safe answer. */
@Composable internal fun OvermindConfirm(question: OvermindQuestion, onConfirm: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(onDismissRequest = onDismiss, title = { Text(question.title) }, text = { Text(question.text) },
        confirmButton = { TextButton(onClick = { onDismiss(); onConfirm() }) { Text(question.accept, color = OvermindPalette.Danger) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

/** Decision taps, the Tasks window and Send a task here; always composed so a tap is handled on any tab. */
@Composable internal fun OvermindTasksHost(model: ZerusViewModel) {
    val tasks = model.tasks
    val request = model.overmindOpenRequest
    LaunchedEffect(request, model.catalogBusy, model.storageReady, model.connections) {
        val key = request ?: return@LaunchedEffect
        val waiting = !model.storageReady || model.catalogBusy
        when (OvermindRouting.step(key, model.tasksAvailable(key.connectionId), waiting,
            !model.storageReady || model.connections.any { it.id == key.connectionId })) {
            OvermindOpenStep.Open -> { model.consumeOvermindOpen(); tasks.openTask(key.connectionId, key.task, project = "") }
            OvermindOpenStep.Drop -> model.consumeOvermindOpen()
            else -> Unit
        }
    }
    // A failed refresh keeps the window and its drafts; a disconnect or a gateway that stopped serving Tasks closes it.
    tasks.screen?.let { screen ->
        if (!model.tasksWithdrawn(screen.connectionId)) OvermindTasksWindow(model, screen)
        else LaunchedEffect(screen) { tasks.close() }
    }
    tasks.sendTo?.let { OvermindSendTaskDialog(model, it) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun OvermindTasksWindow(model: ZerusViewModel, screen: OvermindScreen) {
    ObscureConversation()
    val tasks = model.tasks
    fun back() { if (screen.task.isNotEmpty()) tasks.closeTask() else tasks.close() }
    val rev = model.overmindDigest(screen.connectionId)?.rev
    LaunchedEffect(screen.connectionId, screen.project, rev) { tasks.load(screen.connectionId, screen.project, rev) }
    Dialog(onDismissRequest = ::back, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(), color = OvermindPalette.Background) {
            Scaffold(containerColor = OvermindPalette.Background, topBar = {
                TopAppBar(title = { Column {
                    Text(model.overmindProjectName(screen.connectionId, screen.project), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(if (screen.task.isEmpty()) "Tasks" else "Task", style = MaterialTheme.typography.labelSmall, color = OvermindPalette.Muted)
                } }, navigationIcon = { IconButton(onClick = ::back) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") } }, actions = {
                    IconButton(onClick = {
                        tasks.load(screen.connectionId, screen.project, explicit = true)
                        if (screen.task.isNotEmpty()) tasks.loadCard(screen.connectionId, screen.task)
                    }) { Icon(Icons.Default.Refresh, "Refresh tasks") }
                }, colors = TopAppBarDefaults.topAppBarColors(containerColor = OvermindPalette.Background))
            }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().imePadding()) {
                    OvermindErrorStrip(model)
                    // Its next steps act on the workspace the message belongs to, which is the one shown.
                    val shown = tasks.outcome?.takeIf { it.connectionId == screen.connectionId }
                    Box(Modifier.fillMaxWidth().padding(horizontal = 12.dp)) {
                        OvermindOutcomeBar(shown, onAnyway = { offer -> shown?.let { tasks.anyway(it.connectionId, offer) } },
                            onResolve = { task -> shown?.let { tasks.dismissOutcome(); tasks.openTask(it.connectionId, task) } },
                            onRetry = { write -> tasks.retry(write, context(model, screen, write.task, OvermindOutcomes.recipient(write))) }, onDismiss = tasks::dismissOutcome,
                            retrying = model.overmindWrites.any { it.status == "sending" && it.requestId == shown?.retry?.requestId })
                    }
                    if (screen.task.isNotEmpty()) OvermindTaskCard(model, screen) else OvermindBoardContent(model, screen)
                }
            }
        }
    }
}

/** The app's error line, shown inside the full-window Tasks screens too: a change that was not saved says so here. */
@Composable internal fun OvermindErrorStrip(model: ZerusViewModel) {
    if (model.error.isBlank()) return
    Surface(color = Color(0xFF3A2821), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(start = 16.dp, top = 4.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(model.error, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall)
            IconButton(onClick = { model.clearError() }) { Icon(Icons.Default.Close, "Dismiss error") }
        }
    }
}

/** Names for a write's message: the task's ref and the session a delivery went to. */
internal fun context(model: ZerusViewModel, screen: OvermindScreen, task: String, session: String = "", done: String = ""): OvermindWriteContext {
    val card = model.tasks.board(screen.connectionId, screen.project)?.board?.cards?.find { it.uuid == task } ?: model.tasks.card(screen.connectionId, task)?.view?.card
    return OvermindWriteContext(card?.ref?.ifBlank { null } ?: task.take(8), session.ifEmpty { card?.assignee?.name.orEmpty() }, done)
}

@Composable private fun OvermindBoardContent(model: ZerusViewModel, screen: OvermindScreen) {
    val tasks = model.tasks
    val state = tasks.board(screen.connectionId, screen.project)
    val board = state?.board
    var question by remember { mutableStateOf<Pair<OvermindQuestion, () -> Unit>?>(null) }
    fun move(card: OvermindCard, to: String) {
        val board = tasks.board(screen.connectionId, screen.project)?.board ?: return
        val run = { tasks.write(screen.connectionId, "move", org.json.JSONObject().put("task", card.uuid).put("to", to), context(model, screen, card.uuid)) }
        val ask = OvermindWorkflow.moveQuestion(board, card, to)
        if (ask == null) run() else question = ask to { run() }
    }
    fun reorder(column: OvermindColumn, card: OvermindCard, index: Int) {
        OvermindWorkflow.reorder(column.cards.map { it.uuid }, card.uuid, index)?.let { tasks.write(screen.connectionId, "reorder", it, context(model, screen, card.uuid)) }
    }
    Column(Modifier.fillMaxSize()) {
        val status = when {
            board == null && state?.error?.isNotEmpty() == true -> "Tasks are unavailable: ${state.error}"
            board == null -> "Loading tasks…"
            state.error.isNotEmpty() -> "Could not read the latest tasks: ${state.error}"
            board.cards.isEmpty() -> "No tasks in this project yet."
            else -> ""
        }
        Box(Modifier.fillMaxWidth().height(3.dp)) { if (state?.loading == true) LinearProgressIndicator(Modifier.fillMaxWidth(), color = OvermindPalette.Mint) }
        if (status.isNotEmpty()) Text(status, Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            color = if (state?.error?.isNotEmpty() == true) OvermindPalette.Amber else OvermindPalette.Muted, style = MaterialTheme.typography.bodySmall)
        board ?: return@Column
        val view = remember(board) { OvermindWorkflow.view(board) }
        if (view.yourCall.isNotEmpty()) YourCallStrip(view.yourCall) { tasks.openTask(screen.connectionId, it.uuid) }
        if (view.columns.isEmpty()) return@Column
        val pager = rememberPagerState(pageCount = { view.columns.size })
        val scope = rememberCoroutineScope()
        ScrollableTabRow(selectedTabIndex = pager.currentPage.coerceIn(0, view.columns.lastIndex), edgePadding = 8.dp, containerColor = OvermindPalette.Background,
            contentColor = OvermindPalette.Mint) {
            view.columns.forEachIndexed { index, column ->
                Tab(selected = pager.currentPage == index, onClick = { scope.launch { pager.animateScrollToPage(index) } },
                    text = { Text("${column.stage.name} ${column.cards.size}", maxLines = 1) })
            }
        }
        HorizontalPager(pager, Modifier.weight(1f).fillMaxWidth(), key = { view.columns.getOrNull(it)?.stage?.name ?: it }) { page ->
            val column = view.columns[page]
            ColumnPage(board, column, onOpen = { tasks.openTask(screen.connectionId, it.uuid) }, onMove = ::move, onReorder = { card, index -> reorder(column, card, index) })
        }
    }
    question?.let { (ask, run) -> OvermindConfirm(ask, run) { question = null } }
}

@Composable private fun YourCallStrip(cards: List<OvermindCard>, onOpen: (OvermindCard) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp).background(OvermindPalette.Amber.copy(alpha = .08f), RoundedCornerShape(10.dp))
        .padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text("Your Call (${cards.size})", color = OvermindPalette.Amber, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelLarge)
        Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            cards.forEach { card ->
                Surface(onClick = { onOpen(card) }, color = OvermindPalette.Surface, shape = RoundedCornerShape(8.dp),
                    modifier = Modifier.widthIn(max = 260.dp).semantics { contentDescription = "Open ${card.ref}: ${card.title}, waits for your answer" }) {
                    Text("${card.ref}  ${card.title}", Modifier.padding(horizontal = 10.dp, vertical = 8.dp), maxLines = 1, overflow = TextOverflow.Ellipsis,
                        style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    }
}

/** Where a dragged card would land: the visible card whose middle is nearest to the dragged card's middle. */
private fun dropIndex(list: LazyListState, uuid: String, offset: Float): Int? {
    val items = list.layoutInfo.visibleItemsInfo
    val dragged = items.find { it.key == uuid } ?: return null
    val middle = dragged.offset + offset + dragged.size / 2f
    return items.minByOrNull { abs(it.offset + it.size / 2f - middle) }?.index
}

@Composable private fun ColumnPage(board: OvermindBoard, column: OvermindColumn, onOpen: (OvermindCard) -> Unit,
    onMove: (OvermindCard, String) -> Unit, onReorder: (OvermindCard, Int) -> Unit) {
    val list = rememberLazyListState()
    var dragging by remember { mutableStateOf<String?>(null) }
    var offset by remember { mutableFloatStateOf(0f) }
    val haptics = LocalHapticFeedback.current
    if (column.cards.isEmpty()) {
        Text("No tasks in ${column.stage.name}.", Modifier.fillMaxWidth().padding(24.dp), color = OvermindPalette.Muted, style = MaterialTheme.typography.bodyMedium)
        return
    }
    LazyColumn(Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        items(column.cards, key = { it.uuid }) { card ->
            val moving = dragging == card.uuid
            val gestures = if (card.readOnly) Modifier else Modifier.pointerInput(card.uuid, column.cards) {
                detectDragGesturesAfterLongPress(onDragStart = { dragging = card.uuid; offset = 0f; haptics.performHapticFeedback(HapticFeedbackType.LongPress) },
                    onDrag = { change, amount -> change.consume(); offset += amount.y },
                    onDragEnd = { val index = dropIndex(list, card.uuid, offset); dragging = null; offset = 0f; index?.let { onReorder(card, it) } },
                    onDragCancel = { dragging = null; offset = 0f })
            }
            val position = column.cards.indexOf(card)
            TaskRow(board, card, Modifier.zIndex(if (moving) 1f else 0f).graphicsLayer { translationY = if (moving) offset else 0f; shadowElevation = if (moving) 12f else 0f }
                .clickable(onClickLabel = "Open task") { onOpen(card) }.then(gestures),
                moveUp = if (position > 0 && !card.readOnly) ({ onReorder(card, position - 1) }) else null,
                moveDown = if (position < column.cards.lastIndex && !card.readOnly) ({ onReorder(card, position + 1) }) else null,
                onMove = { onMove(card, it) })
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable private fun TaskRow(board: OvermindBoard, card: OvermindCard, modifier: Modifier, moveUp: (() -> Unit)?, moveDown: (() -> Unit)?, onMove: (String) -> Unit) {
    var menu by remember { mutableStateOf(false) }
    val markers = OvermindWorkflow.markers(card.markers)
    val labels = OvermindWorkflow.labels(card)
    val session = OvermindSessions.tag(card)
    Surface(color = OvermindPalette.Surface, shape = RoundedCornerShape(12.dp), modifier = modifier.fillMaxWidth().semantics {
        contentDescription = listOfNotNull(card.ref, card.title, markers.joinToString(", ").ifEmpty { null },
            labels.takeIf { it.isNotEmpty() }?.let { "labels " + it.joinToString(", ") }, session.ifEmpty { null }?.let { "session $it" }).joinToString(", ")
    }) {
        Row(Modifier.padding(start = 12.dp, top = 10.dp, bottom = 10.dp), verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(card.ref, color = OvermindPalette.Muted, style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.SemiBold)
                    if (card.priority != 2) Text("P${card.priority}", color = if (card.priority < 2) OvermindPalette.Amber else OvermindPalette.Muted, style = MaterialTheme.typography.labelSmall)
                    if (card.readOnly) Icon(Icons.Default.Lock, "Read-only", Modifier.size(12.dp), tint = OvermindPalette.Muted)
                }
                Text(card.title, maxLines = 3, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
                if (markers.isNotEmpty() || labels.isNotEmpty() || session.isNotEmpty()) FlowRow(horizontalArrangement = Arrangement.spacedBy(5.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    card.markers.zip(markers).forEach { (marker, text) ->
                        OvermindChip(text, if (marker in setOf("conflict", "uncertain", "orphaned", "needs_work")) OvermindPalette.Amber else OvermindPalette.Mint)
                    }
                    labels.forEach { OvermindChip(it, if (it == "bug") OvermindPalette.Danger else if (it == "blocked") OvermindPalette.Amber else OvermindPalette.Muted) }
                    if (session.isNotEmpty()) OvermindChip(session, OvermindPalette.Muted, Modifier.widthIn(max = 220.dp))
                }
            }
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, "Task actions for ${card.ref}", tint = OvermindPalette.Muted) }
                DropdownMenu(menu, { menu = false }) {
                    moveUp?.let { up -> DropdownMenuItem(text = { Text("Move up") }, onClick = { menu = false; up() }, leadingIcon = { Icon(Icons.Default.ArrowUpward, null) }) }
                    moveDown?.let { down -> DropdownMenuItem(text = { Text("Move down") }, onClick = { menu = false; down() }, leadingIcon = { Icon(Icons.Default.ArrowDownward, null) }) }
                    val targets = OvermindWorkflow.moveTargets(board, card)
                    if (targets.isEmpty()) DropdownMenuItem(text = { Text("No stage this task can move to") }, onClick = {}, enabled = false)
                    targets.forEach { stage -> DropdownMenuItem(text = { Text("Move to ${stage.name}") }, onClick = { menu = false; onMove(stage.name) }) }
                }
            }
        }
    }
}

/** Send a task here: a Queue task of the session's project, chosen and then sent with an explicit tap. */
@Composable private fun OvermindSendTaskDialog(model: ZerusViewModel, captured: Session) {
    val tasks = model.tasks
    val session = model.sessions.find { it.target == captured.target } ?: captured
    val project = model.overmindProject(session)
    val ref = model.overmindSession(session)
    val connectionId = session.target.connectionId
    val rev = model.overmindDigest(connectionId)?.rev
    LaunchedEffect(connectionId, project, rev) { project?.let { tasks.load(connectionId, it, rev) } }
    val state = project?.let { tasks.board(connectionId, it) }
    val choices = remember(state?.board, ref) { if (state?.board != null && ref != null) OvermindSendChoices.of(state.board, ref) else emptyList() }
    var chosen by remember { mutableStateOf("") }
    var sent by remember { mutableStateOf("") }
    val sending = sent.isNotEmpty() && model.overmindWrites.any { it.requestId == sent && it.status == "sending" }
    val outcome = tasks.sendOutcome
    val report: (OvermindOutcome?) -> Unit = tasks::reportSend
    AlertDialog(onDismissRequest = { if (!sending) tasks.closeChooser() }, title = { Text("Send a task here") }, text = {
        Column(Modifier.heightIn(max = 460.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Claim a Queue task for ${session.target.session} and type it there.", color = OvermindPalette.Muted, style = MaterialTheme.typography.bodySmall)
            when {
                project == null -> Text(OvermindTasks.UNAVAILABLE, color = OvermindPalette.Amber)
                ref == null -> Text(OvermindRequests.UNKNOWN_MACHINE, color = OvermindPalette.Amber)
                state?.board == null -> Text(if (state?.error?.isNotEmpty() == true) "Tasks are unavailable: ${state.error}" else "Loading tasks…", color = OvermindPalette.Muted)
                choices.isEmpty() -> Text("No task waits in this project's Queue.", color = OvermindPalette.Muted)
            }
            choices.forEach { choice ->
                Surface(onClick = { chosen = choice.card.uuid }, enabled = !sending, color = if (chosen == choice.card.uuid) OvermindPalette.Raised else Color.Transparent,
                    shape = RoundedCornerShape(10.dp)) {
                    Row(Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(chosen == choice.card.uuid, null)
                        Column(Modifier.weight(1f)) {
                            Text("${choice.card.ref}  ${choice.card.title}", maxLines = 2, overflow = TextOverflow.Ellipsis)
                            Text(choice.note, color = OvermindPalette.Muted, style = MaterialTheme.typography.labelSmall)
                        }
                    }
                }
            }
            if (model.error.isNotBlank()) Text(model.error, color = OvermindPalette.Amber, style = MaterialTheme.typography.bodySmall)
            OvermindOutcomeBar(outcome, onAnyway = { offer -> sent = tasks.anyway(connectionId, offer, report) }, onResolve = { task -> tasks.closeChooser(); project?.let { tasks.openBoard(connectionId, it) }; tasks.openTask(connectionId, task) },
                onRetry = { write -> tasks.retry(write, OvermindWriteContext(choices.find { it.card.uuid == write.task }?.card?.ref.orEmpty(), session.target.session,
                    "Sent to ${session.target.session}."), report) },
                onDismiss = { tasks.reportSend(null) })
        }
    }, confirmButton = {
        val choice = choices.find { it.card.uuid == chosen }
        TextButton(onClick = {
            val target = ref ?: return@TextButton
            val card = choice?.card ?: return@TextButton
            tasks.reportSend(null)
            sent = tasks.write(connectionId, "send", OvermindRequests.send(card.uuid, target),
                OvermindWriteContext(card.ref, target.name, "Sent ${card.ref} to ${target.name}."), report = report)
        }, enabled = choice != null && ref != null && !sending && !model.demo) { Text(if (sending) "Sending…" else "Send") }
    }, dismissButton = { TextButton(onClick = tasks::closeChooser, enabled = !sending) { Text("Close") } })
}
