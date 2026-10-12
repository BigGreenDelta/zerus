package app.zerus.mobile

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

private val P = OvermindPalette
private const val MAX_PROMPT_BYTES = 64 * 1024
private const val AUTO_IMAGE_BYTES = 2L * 1024 * 1024

/** At most [maxSide] pixels on the longest side; null when the bytes do not decode as a picture. */
private fun decodeImage(bytes: ByteArray, maxSide: Int): Bitmap? {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
    var sample = 1
    while (maxOf(bounds.outWidth, bounds.outHeight) / sample > maxSide) sample *= 2
    return BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
}

@Composable private fun Heading(text: String, modifier: Modifier = Modifier) =
    Text(text, modifier, color = P.Muted, style = MaterialTheme.typography.labelMedium, fontWeight = FontWeight.SemiBold)

@Composable private fun Note(text: String, color: Color = P.Amber) = Text(text, color = color, style = MaterialTheme.typography.bodySmall)

/** The card of one task: what `show` says (prompt, images, details, journal) and every action of the desktop card panel.
 * Writes report in the window's message bar; destructive ones ask first. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun OvermindTaskCard(model: ZerusViewModel, screen: OvermindScreen) {
    val tasks = model.tasks
    val connectionId = screen.connectionId
    val uuid = screen.task
    val boardState = tasks.board(connectionId, screen.project)
    val board = boardState?.board
    val entry = board?.cards?.find { it.uuid == uuid }
    val state = tasks.card(connectionId, uuid)
    val view = state?.view
    // The task is read again whenever its board entry changes.
    LaunchedEffect(connectionId, uuid, entry?.raw) { tasks.loadCard(connectionId, uuid) }
    val card = if (entry != null && (view == null || entry.updatedAt > view.card.updatedAt)) entry else view?.card ?: entry
    if (card == null) {
        Column(Modifier.fillMaxSize().padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (state?.error?.isNotEmpty() == true) Note("Could not read this task: ${state.error}") else Text("Loading the task…", color = P.Muted)
            if (state?.loading == true) LinearProgressIndicator(Modifier.fillMaxWidth(), color = P.Mint)
        }
        return
    }
    val gone = boardState?.fresh == true && !boardState.loading && boardState.error.isEmpty() && entry == null
    val role = board?.let { OvermindWorkflow.role(it, card.stage) }.orEmpty()
    val editable = !card.readOnly && !gone && !model.demo
    val busy = model.overmindWrites.any { it.task == uuid && it.status == "sending" && it.action in OvermindAccess.deliveringActions }
    val unresolved = model.overmindWrites.filter { it.task == uuid && it.needsResolution }
    val epic = "epic" in card.labels
    val projectKey = model.projects.find { it.key.connectionId == connectionId && it.catalogued && it.key.id == card.project }?.key
    fun write(action: String, request: JSONObject, session: String = "", done: String = "", files: List<Attachment> = emptyList(), after: (OvermindWrite) -> Unit = {}) =
        tasks.write(connectionId, action, request.put("task", uuid), OvermindWriteContext(card.ref, session, done), files, after = after)
    var question by remember { mutableStateOf<Pair<OvermindQuestion, () -> Unit>?>(null) }
    fun ask(title: String, text: String, accept: String, run: () -> Unit) { question = OvermindQuestion(title, text, accept) to run }
    var recipients by remember { mutableStateOf<OvermindPurpose?>(null) }
    var preview by remember { mutableStateOf<Pair<String, ByteArray>?>(null) }
    var imageNotice by remember(uuid) { mutableStateOf("") }
    val keptImages = view?.attachments.orEmpty()
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.GetMultipleContents()) { uris ->
        model.importOvermindImages(uris) { files ->
            val total = keptImages.sumOf { it.bytes } + files.sumOf { it.bytes }
            imageNotice = ""
            if (keptImages.size + files.size > AttachmentPolicy.MAX_FILES || total > AttachmentPolicy.MAX_TOTAL_BYTES) {
                model.releaseOvermindFiles(files)
                imageNotice = "The images were not added: a task holds at most ${AttachmentPolicy.MAX_FILES} images, 20 MiB in total."
            } else write("edit", JSONObject().put("attachments", JSONArray(keptImages.map { JSONObject().put("sha256", it.sha256) })),
                done = "Added ${files.size} image(s) to ${card.ref}.", files = files) { if (!it.pending) model.releaseOvermindFiles(files) }
        }
    }

    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        item(key = "header") { Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                SelectionContainer { Text(card.ref, color = P.Muted, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.labelLarge) }
                if (card.stage.isNotEmpty()) OvermindChip(card.stage)
                if (state?.loading == true) CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp, color = P.Mint)
            }
            TitleEditor(card, editable) { title -> write("edit", JSONObject().put("title", title)) }
            val meta = listOfNotNull(card.assignee?.let { "Assigned to ${it.name}" }, card.reviewer?.let { "Review by ${it.name}" },
                (entry?.queueOf)?.let { "In the queue of ${it.name}" })
            if (meta.isNotEmpty()) Text(meta.joinToString(" · "), color = P.Muted, style = MaterialTheme.typography.bodySmall)
            if (gone) Note("${card.ref} is no longer on this board: it was deleted, or its project changed.", P.Danger)
            if (state?.error?.isNotEmpty() == true) Note("Could not read ${card.ref}: ${state.error}", P.Danger)
            if (card.readOnly) Note("A newer version of hgs wrote this task. Update hgs on the gateway to change it.")
            if ("orphaned" in card.flags) Note("The session that held this task has ended. Move the task back to the queue to send it again.")
            if ("conflict" in card.flags) Note("Conflicting changes were recorded for this task; the journal says which.")
            if (epic) Note("This is an epic: it holds other tasks and is never sent.", P.Muted)
        } }
        if (!gone && ("delivery_uncertain" in card.flags || unresolved.isNotEmpty())) item(key = "delivery") {
            Surface(color = P.Amber.copy(alpha = .1f), shape = RoundedCornerShape(10.dp)) { Column(Modifier.fillMaxWidth().padding(12.dp)) {
                val reason = view?.let { OvermindJournal.latest(it, "[sent?]") }.orEmpty()
                Text(if (reason.isEmpty()) "The last delivery was not confirmed. Did it arrive?" else "The last delivery was not confirmed: $reason. Did it arrive?",
                    color = P.Amber, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.bodySmall)
                fun resolve(delivered: Boolean) = write("resolve-delivery", JSONObject().put("delivered", delivered), done =
                    if (delivered) "Recorded that ${card.ref} was delivered." else "Recorded that ${card.ref} was not delivered.") { settled ->
                    if (settled.status == "completed") unresolved.forEach(model::reviewOvermindWrite)
                }
                Row {
                    TextButton(onClick = { resolve(true) }, enabled = editable && !busy) { Text("It was delivered") }
                    TextButton(onClick = { ask("Not delivered", "Record that ${card.ref} did not arrive? A send goes back to the queue and a review request is withdrawn; " +
                        "an answer or rework stays recorded.", "Not delivered") { resolve(false) } }, enabled = editable && !busy) { Text("Not delivered") }
                }
            } }
        }
        if (role == OvermindRoles.YOUR_CALL && !gone) item(key = "answer") { AnswerBox(view, card, editable && !busy) { answer, done ->
            write("answer", JSONObject().put("answer", answer), card.assignee?.name.orEmpty(),
                "Sent your answer on ${card.ref} to ${card.assignee?.name ?: "its session"}.") { if (it.status == "completed") done() }
        } }
        if (!gone && board != null) item(key = "actions") { FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            if (OvermindWorkflow.claimable(board, card.stage)) OutlinedButton(onClick = { recipients = OvermindPurpose.Send }, enabled = editable && !epic && !busy) { Text("Send to session…") }
            if (role == OvermindRoles.REVIEW) OutlinedButton(onClick = { recipients = OvermindPurpose.Review }, enabled = editable && !epic && !busy) { Text("Send for review…") }
            val assignee = card.assignee
            if ("review-failed" in card.labels && assignee != null) OutlinedButton(onClick = {
                write("rework-send", JSONObject(), assignee.name, "Sent the rework of ${card.ref} to ${assignee.name}.")
            }, enabled = editable && !busy) { Text("Send rework") }
            val targets = OvermindWorkflow.moveTargets(board, card)
            val done = OvermindWorkflow.first(board.stages, OvermindRoles.DONE)
            if (done != null && role != OvermindRoles.DONE && targets.any { it.name == done.name })
                OutlinedButton(onClick = { write("done", JSONObject(), done = "${card.ref} is done.") }, enabled = editable) { Text("Done") }
            Box {
                var menu by remember { mutableStateOf(false) }
                OutlinedButton(onClick = { menu = true }, enabled = editable && targets.isNotEmpty()) { Text("Move to…") }
                DropdownMenu(menu, { menu = false }) {
                    targets.forEach { stage -> DropdownMenuItem(text = { Text(stage.name) }, onClick = {
                        menu = false
                        val run = { write("move", JSONObject().put("to", stage.name)); Unit }
                        OvermindWorkflow.moveQuestion(board, card, stage.name)?.let { question = it to run } ?: run()
                    }) }
                }
            }
        } }
        item(key = "prompt") { PromptEditor(view, editable, card.ref, ::ask) { prompt, saved -> write("edit", JSONObject().put("prompt", prompt)) { if (it.status == "completed") saved() } } }
        item(key = "images") { Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Heading("Images", Modifier.weight(1f))
                if (editable && view != null) TextButton(onClick = { picker.launch("image/*") }, enabled = keptImages.size < AttachmentPolicy.MAX_FILES) { Text("Add…") }
            }
            if (keptImages.isEmpty()) Text(if (view == null) "Loading…" else "No images", color = P.Muted, style = MaterialTheme.typography.bodySmall)
            if (imageNotice.isNotEmpty()) Note(imageNotice)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                keptImages.forEach { image -> ImageTile(model, connectionId, uuid, image, editable, onOpen = { bytes -> preview = image.name to bytes }) {
                    ask("Remove the image", "Remove ${image.name} from ${card.ref}?", "Remove") {
                        write("edit", JSONObject().put("attachments", JSONArray(keptImages.filter { it.sha256 != image.sha256 }.map { JSONObject().put("sha256", it.sha256) })),
                            done = "Removed ${image.name} from ${card.ref}.")
                    }
                } }
            }
        } }
        item(key = "details") { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Heading("Priority", Modifier.width(72.dp))
                var menu by remember { mutableStateOf(false) }
                Box {
                    TextButton(onClick = { menu = true }, enabled = editable) { Text(priorityName(card.priority)) }
                    DropdownMenu(menu, { menu = false }) { (0..4).forEach { value -> DropdownMenuItem(text = { Text(priorityName(value)) }, onClick = {
                        menu = false; if (value != card.priority) write("edit", JSONObject().put("priority", value))
                    }) } }
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Heading("Pin", Modifier.width(72.dp))
                val pinned = OvermindSessions.pinned(card)
                Text(pinned?.let { "Pinned to ${it.name}" } ?: "Not pinned", Modifier.weight(1f), style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                TextButton(onClick = { recipients = OvermindPurpose.Pin }, enabled = editable) { Text("Pin to…") }
                if (pinned != null) TextButton(onClick = { write("unpin", JSONObject()) }, enabled = editable) { Text("Unpin") }
            }
        } }
        item(key = "labels") { LabelEditor(card, editable, remove = { label -> write("label", JSONObject().put("label", label).put("present", false)) }) { label, done ->
            write("label", JSONObject().put("label", label).put("present", true)) { if (it.status == "completed") done() }
        } }
        item(key = "relations") { RelationEditor(view, card, board, editable, open = { tasks.openTask(connectionId, it) },
            remove = { relation, target -> write("link", JSONObject().put("relation", relation).put("target", target).put("present", false)) }) { relation, target, done ->
            write("link", JSONObject().put("relation", relation).put("target", target).put("present", true)) { if (it.status == "completed") done() }
        } }
        item(key = "comment") { CommentBox(editable && view != null) { text, done -> write("comment", JSONObject().put("text", text), done = "Commented on ${card.ref}.") {
            if (it.status == "completed") done() } } }
        item(key = "journal") { Row(verticalAlignment = Alignment.CenterVertically) {
            Heading(if (view == null) "Journal" else "Journal (${view.journalTotal})", Modifier.weight(1f))
            if (view != null && view.journalFrom > 0) TextButton(onClick = { tasks.older(connectionId, uuid) }, enabled = state?.older != true && state?.loading != true) {
                Text(if (state?.older == true) "Loading…" else "Load older entries")
            }
        } }
        if (view != null && view.journal.isEmpty()) item(key = "journal:empty") { Text("Nothing recorded yet.", color = P.Muted, style = MaterialTheme.typography.bodySmall) }
        items(view?.journal.orEmpty().withIndex().toList(), key = { "journal:${(view?.journalFrom ?: 0) + it.index}" }) { (_, entry) -> JournalEntry(entry) }
        if (!gone) item(key = "delete") {
            TextButton(onClick = { ask("Delete ${card.ref}", "Delete ${card.ref} “${card.title}”? It disappears from the board on every machine.", "Delete") {
                write("delete", JSONObject(), done = "Deleted ${card.ref}.") { if (it.status == "completed" && tasks.screen?.task == uuid) tasks.closeTask() }
            } }, enabled = editable) { Icon(Icons.Default.DeleteOutline, null, tint = P.Danger); Spacer(Modifier.width(6.dp)); Text("Delete task…", color = P.Danger) }
        }
    }
    question?.let { (ask, run) -> OvermindConfirm(ask, run) { question = null } }
    recipients?.let { purpose -> RecipientDialog(model, projectKey, card, purpose, onDismiss = { recipients = null }) { recipient ->
        recipients = null
        val target = recipient.ref ?: return@RecipientDialog
        when (purpose) {
            OvermindPurpose.Pin -> write("pin", JSONObject().put("session", OvermindRequests.session(target)))
            OvermindPurpose.Send -> write("send", JSONObject().put("session", OvermindRequests.session(target)), target.name, "Sent ${card.ref} to ${target.name}.")
            OvermindPurpose.Review -> write("review-send", JSONObject().put("session", OvermindRequests.session(target)), target.name, "Sent ${card.ref} to ${target.name} for review.")
        }
    } }
    preview?.let { (name, bytes) -> OvermindImagePreview(name, bytes) { preview = null } }
}

private fun priorityName(value: Int) = when (value) { 0 -> "P0 · highest"; 2 -> "P2 · default"; 4 -> "P4 · lowest"; else -> "P$value" }

@Composable private fun TitleEditor(card: OvermindCard, editable: Boolean, save: (String) -> Unit) {
    var editing by rememberSaveable(card.uuid) { mutableStateOf<String?>(null) }
    val draft = editing
    if (draft == null) Row(verticalAlignment = Alignment.Top) {
        SelectionContainer(Modifier.weight(1f)) { Text(card.title, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold) }
        if (editable) IconButton(onClick = { editing = card.title }) { Icon(Icons.Default.Edit, "Edit the title", tint = P.Muted) }
    } else Column {
        OutlinedTextField(draft, { editing = it.replace('\n', ' ').take(200) }, Modifier.fillMaxWidth(), label = { Text("Title") }, singleLine = true)
        Row {
            TextButton(onClick = { save(draft.trim()); editing = null }, enabled = draft.isNotBlank() && draft.trim() != card.title) { Text("Save") }
            TextButton(onClick = { editing = null }) { Text("Cancel") }
        }
    }
}

@Composable private fun AnswerBox(view: OvermindTaskView?, card: OvermindCard, enabled: Boolean, send: (String, () -> Unit) -> Unit) {
    var answer by rememberSaveable(card.uuid) { mutableStateOf("") }
    Surface(color = P.Amber.copy(alpha = .1f), shape = RoundedCornerShape(10.dp)) { Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Your Call: the agent asks", color = P.Amber, fontWeight = FontWeight.SemiBold, style = MaterialTheme.typography.bodySmall)
        val asked = view?.let(OvermindJournal::question).orEmpty()
        MarkdownText(asked.ifEmpty { if (view == null) "Loading the question…" else "The question is in the journal below." })
        OutlinedTextField(answer, { answer = it }, Modifier.fillMaxWidth().heightIn(min = 88.dp), placeholder = { Text("Your answer") }, enabled = enabled)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            Button(onClick = { val sent = answer; send(sent) { if (answer == sent) answer = "" } }, enabled = enabled && answer.isNotBlank()) { Text("Answer") }
        }
        Text("Records the answer and types it into ${card.assignee?.name ?: "the session that asked"}.", color = P.Muted, style = MaterialTheme.typography.labelSmall)
    } }
}

@Composable private fun PromptEditor(view: OvermindTaskView?, editable: Boolean, ref: String, ask: (String, String, String, () -> Unit) -> Unit,
    save: (String, () -> Unit) -> Unit) {
    val uuid = view?.card?.uuid.orEmpty()
    var draft by rememberSaveable(uuid) { mutableStateOf<String?>(null) }
    var base by rememberSaveable(uuid) { mutableStateOf("") }
    var showCurrent by rememberSaveable(uuid) { mutableStateOf(false) }
    val saved = view?.prompt.orEmpty()
    val text = draft
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Heading("Prompt", Modifier.weight(1f))
            if (text == null && editable && view != null) TextButton(onClick = { draft = saved; base = saved }) { Text("Edit") }
            if (text != null) {
                TextButton(onClick = { if (text == base) draft = null else ask("Discard changes", "Discard your changes to the prompt of $ref?", "Discard") { draft = null } }) { Text("Discard") }
                val bytes = text.toByteArray(Charsets.UTF_8).size
                TextButton(onClick = {
                    val run = { save(text) { if (draft == text) draft = null }; Unit }
                    if (base != saved) ask("Replace the prompt", "The prompt of $ref was changed elsewhere since you started editing. Save your version in its place?", "Replace", run)
                    else run()
                }, enabled = editable && text.isNotBlank() && bytes <= MAX_PROMPT_BYTES && text != saved) { Text("Save") }
            }
        }
        if (text == null) {
            if (view == null) Text("Loading the prompt…", color = P.Muted, style = MaterialTheme.typography.bodySmall)
            else SelectionContainer { Text(saved, style = MaterialTheme.typography.bodyMedium) }
        } else {
            OutlinedTextField(text, { draft = it }, Modifier.fillMaxWidth().heightIn(min = 120.dp, max = 360.dp), enabled = editable)
            val bytes = text.toByteArray(Charsets.UTF_8).size
            when {
                bytes > MAX_PROMPT_BYTES -> Note("The prompt is longer than 64 KiB (${(bytes + 1023) / 1024} KiB): shorten it to save it.")
                text.isBlank() -> Note("Write the task's prompt to save it.", P.Muted)
                base != saved -> {
                    Note("The prompt was changed elsewhere since you started editing; saving replaces that version.")
                    TextButton(onClick = { showCurrent = !showCurrent }) { Text(if (showCurrent) "Hide the current version" else "Show the current version") }
                    if (showCurrent) SelectionContainer { Text(saved, Modifier.fillMaxWidth().background(P.Surface, RoundedCornerShape(8.dp)).padding(8.dp),
                        color = P.Muted, style = MaterialTheme.typography.bodySmall) }
                }
            }
        }
    }
}

@Composable private fun ImageTile(model: ZerusViewModel, connectionId: String, task: String, image: OvermindImageRef, editable: Boolean,
    onOpen: (ByteArray) -> Unit, onRemove: () -> Unit) {
    val tasks = model.tasks
    // Small images load with the card; a large one costs many relay ranges and loads on a tap.
    LaunchedEffect(connectionId, task, image.sha256) { if (image.bytes in 1..AUTO_IMAGE_BYTES) tasks.image(connectionId, task, image.sha256) }
    val state = tasks.images[tasks.imageKey(connectionId, task, image.sha256)]
    // (decoded, bitmap): bytes that passed the format check may still fail to decode on this phone.
    val decoded by produceState<Pair<Boolean, Bitmap?>>(false to null, state?.bytes) {
        val bytes = state?.bytes
        value = if (bytes == null) false to null else true to withContext(Dispatchers.Default) { runCatching { decodeImage(bytes, 256) }.getOrNull() }
    }
    val thumbnail = decoded.second
    val failure = state?.error?.ifEmpty { null } ?: if (decoded.first && thumbnail == null) "This picture cannot be shown on the phone." else null
    Box(Modifier.size(width = 96.dp, height = 72.dp)) {
        Surface(onClick = {
            val bytes = state?.bytes
            if (bytes != null && thumbnail != null) onOpen(bytes) else if (state == null || state.error.isNotEmpty()) tasks.image(connectionId, task, image.sha256, retry = true)
        }, color = P.Surface, shape = RoundedCornerShape(8.dp), modifier = Modifier.fillMaxSize().semantics {
            contentDescription = when { failure != null -> "${image.name}: $failure"; thumbnail != null -> "Open ${image.name}"; state == null -> "Load ${image.name}"
                else -> "${image.name}, loading" }
        }) {
            Box(contentAlignment = Alignment.Center) {
                val bitmap = thumbnail
                when {
                    bitmap != null -> Image(remember(bitmap) { bitmap.asImageBitmap() }, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
                    failure != null -> Icon(Icons.Default.BrokenImage, null, tint = P.Amber)
                    state == null -> Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Icon(Icons.Default.Download, null, Modifier.size(18.dp), tint = P.Muted)
                        Text("%.1f MiB".format(java.util.Locale.ROOT, image.bytes / 1_048_576.0), color = P.Muted, style = MaterialTheme.typography.labelSmall)
                    }
                    else -> CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp, color = P.Mint)
                }
            }
        }
        if (editable) IconButton(onClick = onRemove, modifier = Modifier.align(Alignment.TopEnd).size(28.dp)) {
            Icon(Icons.Default.Close, "Remove ${image.name}", Modifier.background(P.Background.copy(alpha = .7f), RoundedCornerShape(10.dp)).padding(2.dp).size(14.dp))
        }
    }
}

/** In-app only: a task image is never handed to another app under the name an agent chose. */
@Composable private fun OvermindImagePreview(name: String, bytes: ByteArray, onDismiss: () -> Unit) {
    val image by produceState<Bitmap?>(null, bytes) { value = withContext(Dispatchers.Default) { decodeImage(bytes, 2048) } }
    var scale by remember { mutableFloatStateOf(1f) }
    var offset by remember { mutableStateOf(Offset.Zero) }
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            image?.let { bitmap ->
                Image(remember(bitmap) { bitmap.asImageBitmap() }, name, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize()
                    .pointerInput(bytes) { detectTransformGestures { _, pan, zoom, _ -> scale = (scale * zoom).coerceIn(1f, 8f); offset = if (scale == 1f) Offset.Zero else offset + pan } }
                    .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y))
            } ?: CircularProgressIndicator(Modifier.align(Alignment.Center), color = P.Mint)
            Row(Modifier.fillMaxWidth().background(Color.Black.copy(alpha = .55f)).windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
                .padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onDismiss) { Icon(Icons.Default.Close, "Close", tint = Color.White) }
                Text(name, Modifier.weight(1f), color = Color.White, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleSmall)
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable private fun LabelEditor(card: OvermindCard, editable: Boolean, remove: (String) -> Unit, add: (String, () -> Unit) -> Unit) {
    var label by rememberSaveable(card.uuid) { mutableStateOf("") }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Heading("Labels")
        if (card.labels.isNotEmpty()) FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            card.labels.forEach { name ->
                Surface(color = P.Raised, shape = RoundedCornerShape(6.dp)) { Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(name, Modifier.padding(start = 8.dp, end = if (editable && name !in OvermindWorkflow.reserved) 0.dp else 8.dp, top = 4.dp, bottom = 4.dp),
                        color = if (name == "bug") P.Danger else if (name == "blocked") P.Amber else MaterialTheme.colorScheme.onSurface, style = MaterialTheme.typography.labelMedium)
                    if (editable && name !in OvermindWorkflow.reserved) IconButton(onClick = { remove(name) }, modifier = Modifier.size(28.dp)) {
                        Icon(Icons.Default.Close, "Remove the label $name", Modifier.size(14.dp), tint = P.Muted)
                    }
                } }
            }
        }
        if (editable) Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(label, { label = it.lowercase().filterNot(Char::isWhitespace).take(64) }, Modifier.weight(1f), singleLine = true,
                placeholder = { Text("Add a label") })
            TextButton(onClick = { val added = label.trim(); add(added) { if (label == added) label = "" } }, enabled = label.isNotBlank()) { Text("Add") }
        }
    }
}

@Composable private fun RelationEditor(view: OvermindTaskView?, card: OvermindCard, board: OvermindBoard?, editable: Boolean, open: (String) -> Unit,
    remove: (String, String) -> Unit, add: (String, String, () -> Unit) -> Unit) {
    val related = view?.raw?.let { raw -> runCatching { JSONObject(raw).optJSONArray("related") }.getOrNull() }
        ?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String } }.orEmpty()
    val groups = listOfNotNull(card.parent.takeIf { it.isNotEmpty() }?.let { "parent" to listOf(it) },
        card.follows.takeIf { it.isNotEmpty() }?.let { "follows" to it }, related.takeIf { it.isNotEmpty() }?.let { "related" to it })
    val names = mapOf("parent" to "Parent", "follows" to "Follows", "related" to "Related")
    var kind by rememberSaveable(card.uuid) { mutableStateOf("follows") }
    var target by rememberSaveable(card.uuid) { mutableStateOf("") }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Heading("Relations")
        if (groups.isEmpty()) Text("No relations", color = P.Muted, style = MaterialTheme.typography.bodySmall)
        groups.forEach { (relation, targets) -> targets.forEach { uuid ->
            val other = board?.cards?.find { it.uuid == uuid }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(names.getValue(relation), Modifier.width(72.dp), color = P.Muted, style = MaterialTheme.typography.labelMedium)
                TextButton(onClick = { open(uuid) }, enabled = other != null) { Text(other?.let { "${it.ref}  ${it.title}" } ?: uuid.take(8), maxLines = 1, overflow = TextOverflow.Ellipsis) }
                Spacer(Modifier.weight(1f))
                if (editable) IconButton(onClick = { remove(relation, uuid) }, modifier = Modifier.size(32.dp)) {
                    Icon(Icons.Default.Close, "Remove the relation to ${other?.ref ?: uuid.take(8)}", Modifier.size(16.dp), tint = P.Muted)
                }
            }
        } }
        if (editable) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            var menu by remember { mutableStateOf(false) }
            Box {
                TextButton(onClick = { menu = true }) { Text(names.getValue(kind)) }
                DropdownMenu(menu, { menu = false }) { names.forEach { (id, name) -> DropdownMenuItem(text = { Text(name) }, onClick = { kind = id; menu = false }) } }
            }
            OutlinedTextField(target, { target = it.trim().take(64) }, Modifier.weight(1f), singleLine = true, placeholder = { Text("Task ref, e.g. om-k3f9x") })
            TextButton(onClick = { val linked = target; add(kind, linked) { if (target == linked) target = "" } }, enabled = target.isNotBlank()) { Text("Link") }
        }
    }
}

@Composable private fun CommentBox(enabled: Boolean, send: (String, () -> Unit) -> Unit) {
    var text by rememberSaveable { mutableStateOf("") }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Heading("Comment")
        OutlinedTextField(text, { text = it }, Modifier.fillMaxWidth(), placeholder = { Text("Add a note to the journal") }, enabled = enabled, maxLines = 6)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
            TextButton(onClick = { val sent = text; send(sent) { if (text == sent) text = "" } }, enabled = enabled && text.isNotBlank()) { Text("Comment") }
        }
    }
}

@Composable private fun JournalEntry(entry: OvermindJournalEntry) {
    Column(Modifier.fillMaxWidth().background(P.Surface, RoundedCornerShape(10.dp)).padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        val time = if (entry.at > 0) java.text.SimpleDateFormat("d MMM HH:mm", java.util.Locale.getDefault()).format(java.util.Date(entry.at * 1000)) else ""
        Text(listOf(entry.marker, OvermindJournal.author(entry), time).filter { it.isNotBlank() }.joinToString(" · "), color = P.Muted, style = MaterialTheme.typography.labelSmall)
        if (entry.text.isNotBlank()) ProvideTextStyle(MaterialTheme.typography.bodyMedium) { MarkdownText(entry.text) }
    }
}

/** The project's live sessions with their state; a session whose computer is unknown is shown but cannot be chosen. A tap acts. */
@Composable private fun RecipientDialog(model: ZerusViewModel, project: ProjectKey?, card: OvermindCard, purpose: OvermindPurpose, onDismiss: () -> Unit,
    onChoose: (OvermindRecipient) -> Unit) {
    val recipients = project?.let { OvermindRecipients.of(model.sessions, model.machines, it, card, purpose) }.orEmpty()
    AlertDialog(onDismissRequest = onDismiss, title = { Text(when (purpose) {
        OvermindPurpose.Send -> "Send ${card.ref} to a session"; OvermindPurpose.Review -> "Send ${card.ref} for review"; OvermindPurpose.Pin -> "Pin ${card.ref} to a session"
    }) }, text = {
        Column(Modifier.heightIn(max = 440.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(when (purpose) {
                OvermindPurpose.Send -> "Claims the task for the session and types it there."
                OvermindPurpose.Review -> "Asks the session to review the work."
                OvermindPurpose.Pin -> "Reserves the task for the session: it waits in that session's queue."
            }, color = P.Muted, style = MaterialTheme.typography.bodySmall)
            if (recipients.isEmpty()) Text("No sessions in this project.", color = P.Muted)
            recipients.forEach { recipient ->
                Surface(onClick = { onChoose(recipient) }, enabled = recipient.enabled, color = Color.Transparent, shape = RoundedCornerShape(8.dp)) {
                    Row(Modifier.fillMaxWidth().padding(vertical = 8.dp, horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        if (purpose == OvermindPurpose.Pin) Icon(if (recipient.checked) Icons.Default.CheckCircle else Icons.Default.RadioButtonUnchecked, null,
                            Modifier.size(18.dp), tint = if (recipient.checked) P.Mint else P.Muted)
                        Column(Modifier.weight(1f).padding(start = 8.dp)) {
                            Text(SessionFilters.label(recipient.session), maxLines = 1, overflow = TextOverflow.Ellipsis,
                                color = if (recipient.enabled) MaterialTheme.colorScheme.onSurface else P.Muted)
                            Text(listOf(recipient.session.target.session, recipient.note.ifEmpty { recipient.session.status }).filter { it.isNotBlank() }.joinToString(" · "),
                                color = P.Muted, style = MaterialTheme.typography.labelSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)
                        }
                    }
                }
            }
        }
    }, confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}
