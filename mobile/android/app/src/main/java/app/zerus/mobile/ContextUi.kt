package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

/** Its clock belongs to this small footer, never the editor or history viewport. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun ContextFooter(model: ZerusViewModel, session: Session, captureDraft: () -> Draft = { model.draft(session.target) }) {
    val target = session.target
    val raw = model.activity
    val recorded = !model.activityVerified || target.archiveId.isNotBlank() || model.demo
    var nowMillis by remember { mutableLongStateOf(System.currentTimeMillis()) }
    val clockMinute = ContextPresentation.countdownMinute(raw, nowMillis / 1000.0)
    // Only the cheap countdown key changes each second. Full native detail formatting is remembered.
    val view = remember(raw, recorded, clockMinute) { ContextPresentation.from(raw, recorded, nowMillis / 1000.0) }
    LaunchedEffect(raw) {
        if (ContextPresentation.countdownMinute(raw, System.currentTimeMillis() / 1000.0) != null)
            while (true) { delay(1000); nowMillis = System.currentTimeMillis() }
    }
    var details by remember(target.key) { mutableStateOf(false) }
    var clear by remember(target.key) { mutableStateOf(false) }
    var review by remember(target.key) { mutableStateOf<ContextOperation?>(null) }
    var restore by remember(target.key) { mutableStateOf<Draft?>(null) }
    if(details || clear || review != null || restore != null) ObscureConversation()
    val dark = MaterialTheme.colorScheme.background.luminance() < .5f
    val muted = Color(ContextPresentation.color(ContextTone.Muted, dark))
    val operation = model.contextOperation(target)
    val status = operation?.let { contextOperationLabel(it.operation, it.status) }.orEmpty()
    Surface(onClick = { details = true }, color = MaterialTheme.colorScheme.background,
        modifier = Modifier.fillMaxWidth().height(48.dp)) {
        Column(Modifier.padding(horizontal = 8.dp, vertical = 4.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(view.summary, Modifier.weight(1f), color = Color(ContextPresentation.color(view.tone, dark)),
                    style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(view.cacheSummary, Modifier.widthIn(max = 140.dp), color = Color(ContextPresentation.cacheColor(view.cacheTone, dark)),
                    style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Icon(Icons.Default.Info, "Context and cache details", Modifier.size(16.dp), tint = muted)
            }
            Text(listOf(if (view.recorded) "Last known" else "", status.ifBlank { view.compactionSummary })
                .filter { it.isNotBlank() }.joinToString(" / ").ifBlank { "Context and cache details" },
                color = muted, style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
    if (details) ModalBottomSheet(onDismissRequest = { details = false }, containerColor = MaterialTheme.colorScheme.background) {
        Column(Modifier.fillMaxWidth().heightIn(max = 620.dp).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Context and cache", style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
            Text(SessionFilters.label(session), color = muted, style = MaterialTheme.typography.bodySmall)
            if (view.recorded) Text("Last known values / Read-only", color = muted, style = MaterialTheme.typography.labelMedium)
            SelectionContainer {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    view.details.forEach { item -> Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        Text(item.label, Modifier.weight(1f), color = muted, style = MaterialTheme.typography.bodySmall)
                        Text(item.value, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall)
                    } }
                    view.notes.forEach { note -> Text(note, color = muted, style = MaterialTheme.typography.bodySmall) }
                }
            }
            operation?.let { current ->
                HorizontalDivider()
                Text(contextOperationLabel(current.operation, current.status), style = MaterialTheme.typography.labelLarge)
                if (current.error.isNotBlank()) Text(current.error, color = Color(ContextPresentation.color(ContextTone.Warning, dark)),
                    style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (current.status in listOf("submitted", "uncertain")) TextButton(onClick = { model.checkContextOperation(current) },
                        enabled = model.activityVerified && !model.demo && target.archiveId.isBlank() && "context:${current.requestId}" !in model.receiptFlights) { Text("Check result") }
                    if (current.status in listOf("failed", "uncertain")) TextButton(onClick = { review = current },
                        enabled = model.activityVerified && !model.demo && target.archiveId.isBlank() && "context:${current.requestId}" !in model.receiptFlights) { Text("Review") }
                }
            }
            if (model.contextNotice.isNotBlank()) Text(model.contextNotice, color = muted, style = MaterialTheme.typography.bodySmall)
            val saved = model.contextDrafts(target)
            if (saved.isNotEmpty()) {
                HorizontalDivider()
                Text("Drafts from earlier context", style = MaterialTheme.typography.titleSmall)
                saved.forEach { previous ->
                    Text(previous.text.ifBlank { "Saved files" }, maxLines = 3, overflow = TextOverflow.Ellipsis,
                        style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = { restore = previous }, enabled = !model.demo && model.canRestoreContextDraft(previous, target)) { Text("Review and restore") }
                }
            }
            if (model.compactContinuationActive) {
                Text("The draft stays saved while compaction is pending. Changes cancel the pending send.",
                    color = muted, style = MaterialTheme.typography.bodySmall)
                TextButton(onClick = { model.cancelCompactContinuation() }) { Text("Cancel pending send") }
            }
            HorizontalDivider()
            val available = !model.demo && model.storageReady && model.activityVerified && !model.contextBlocked(target) && !model.sendingBlocked(target)
            val compactReason = model.contextActionReason(target, "compact_context").ifBlank { view.compactReason }
            val clearReason = model.contextActionReason(target, "clear_context").ifBlank { view.clearReason }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { captureDraft(); model.compactContext(target) }, enabled = available && compactReason.isBlank()) { Text("Compact") }
                TextButton(onClick = { captureDraft(); clear = true }, enabled = available && clearReason.isBlank()) { Text("Clear context") }
            }
            Button(onClick = { val captured = captureDraft(); model.compactContext(target, continueDraft = true, expectedDraft = captured) },
                enabled = available && compactReason.isBlank() &&
                    (model.draft(target).text.isNotBlank() || model.draft(target).attachments.isNotEmpty()) && !model.sendingBlocked(target)) {
                Text("Compact and continue")
            }
            if (compactReason.isNotBlank()) Text("Compact: $compactReason", color = muted, style = MaterialTheme.typography.bodySmall)
            if (clearReason.isNotBlank()) Text("Clear: $clearReason", color = muted, style = MaterialTheme.typography.bodySmall)
            if (model.contextBlocked(target) || model.sendingBlocked(target)) Text("Resolve the pending message or context operation before another context change.",
                color = muted, style = MaterialTheme.typography.bodySmall)
            if (available && compactReason.isBlank()) Text("Compaction uses tokens and may omit details. A submitted command is not yet a completed compaction.",
                color = muted, style = MaterialTheme.typography.bodySmall)
            Spacer(Modifier.height(20.dp))
        }
    }
    if (clear) ClearContextConfirmation(model, target, onDismiss = { clear = false }, beforeClear = { captureDraft() })
    review?.let { operation -> ContextOperationReview(model, operation, onDismiss = { review = null }) }
    restore?.let { previous -> ContextDraftRestore(model, previous, target, beforeRestore = { captureDraft() }, onDismiss = { restore = null }) }
}

@Composable internal fun ClearContextConfirmation(model: ZerusViewModel, target: Target, onDismiss: () -> Unit, beforeClear: () -> Unit = {}) {
    val computer = remember(target.key) { model.machines.find { it.connectionId == target.connectionId && it.id == target.computerId }?.name ?: target.computerId }
    val raw = model.activity
    val recorded = !model.activityVerified || target.archiveId.isNotBlank()
    val view = remember(raw, recorded) { ContextPresentation.from(raw, recorded) }
    ZerusAlertDialog(onDismissRequest = onDismiss, title = { Text("Clear context?") },
        text = { Text("Clear the conversation context in ${target.session} on $computer?\n\nThis runs the agent’s native clear command. Future messages start without the current conversation context. Your unsent draft is kept.") },
        confirmButton = { TextButton(onClick = { beforeClear(); model.clearContext(target); onDismiss() }, enabled = !model.demo && model.storageReady &&
            model.selected?.target == target && model.activityVerified && !model.contextBlocked(target) && !model.sendingBlocked(target) &&
            model.contextActionReason(target, "clear_context").isBlank() && view.clearReason.isBlank()) { Text("Clear context") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@Composable internal fun ContextOperationReview(model: ZerusViewModel, operation: ContextOperation, onDismiss: () -> Unit) {
    val current = model.contextOperations.find { it.requestId == operation.requestId }
    ZerusAlertDialog(onDismissRequest = onDismiss, title = { Text("Review context command") },
        text = { Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(contextOperationLabel(operation.operation, operation.status))
            Text("The command may already have changed ${operation.target.session}. Check its original receipt and the native conversation before allowing another context command. Reviewing does not repeat the command or send a message.")
            if (operation.error.isNotBlank()) Text(operation.error, style = MaterialTheme.typography.bodySmall)
        } }, confirmButton = { TextButton(onClick = { model.reviewContextOperation(operation); onDismiss() },
            enabled = !model.demo && model.storageReady && current?.status in listOf("failed", "uncertain") &&
                "context:${operation.requestId}" !in model.receiptFlights) { Text("Mark reviewed") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

@Composable internal fun ContextDraftRestore(model: ZerusViewModel, draft: Draft, target: Target,
    beforeRestore: () -> Unit = {}, onDismiss: () -> Unit) {
    ZerusAlertDialog(onDismissRequest = onDismiss, title = { Text("Restore earlier draft?") },
        text = { Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Restore this saved draft into the current conversation in ${target.session}? Your current composition is kept in Saved drafts. This prepares the editor without sending.")
            SelectionContainer { Text(draft.text.ifBlank { "Saved files" }, maxLines = 8, overflow = TextOverflow.Ellipsis) }
            draft.attachments.forEach { Text(it.name, style = MaterialTheme.typography.bodySmall) }
        } }, confirmButton = { TextButton(onClick = { beforeRestore(); model.restoreContextDraft(draft, target); onDismiss() },
            enabled = !model.demo && model.storageReady && model.canRestoreContextDraft(draft, target)) { Text("Restore draft") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } })
}

internal fun contextOperationLabel(operation: String, status: String): String {
    val name = if (operation == "clear_context") "Context clear" else "Compaction"
    return name + " / " + when (status) {
        "sending" -> "Sending command…"
        "submitted" -> "Command accepted / Waiting for native result"
        "completed", "confirmed" -> "Completed"
        "uncertain" -> "Result unknown"
        "failed" -> "Not completed"
        "unchanged" -> "No history compacted"
        "cancelled" -> "Cancelled"
        else -> status
    }
}
