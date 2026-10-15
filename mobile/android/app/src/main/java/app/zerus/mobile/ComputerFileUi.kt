package app.zerus.mobile

import android.app.Activity
import android.content.Intent
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp

/** File actions are tied to the link the person opened, including across navigation. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun ComputerFileHost(files: ComputerFiles) {
    if (files.selection != null || files.export != null) ObscureConversation()
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    files.external?.let { opened ->
        val selectionId = files.selection?.id.orEmpty()
        LaunchedEffect(opened) {
            files.consumeExternal()
            AttachmentViewing.view(context, opened)?.let { files.failed(selectionId, it) }
        }
    }
    files.notice.takeIf { it.isNotBlank() }?.let { notice ->
        LaunchedEffect(notice) { files.consumeNotice(); Toast.makeText(context, notice, Toast.LENGTH_LONG).show() }
    }
    files.export?.let { pending -> key(pending.id) { ComputerFileSaveHost(files, pending) } }
    files.selection?.let { chosen ->
        AlertDialog(onDismissRequest = files::close,
            title = { Row {
                Text(chosen.reference.name, Modifier.weight(1f), maxLines = 2, overflow = TextOverflow.Ellipsis)
                IconButton(onClick = { clipboard.setText(AnnotatedString(chosen.reference.path)) }) {
                    Icon(Icons.Default.ContentCopy, "Copy file path")
                }
            } },
            text = { Column(Modifier.heightIn(max = 280.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text("On ${chosen.computer}", style = MaterialTheme.typography.labelMedium)
                SelectionContainer { Text(chosen.reference.path + chosen.reference.location, style = MaterialTheme.typography.bodySmall) }
                if (chosen.busy) {
                    Text("Downloading…", style = MaterialTheme.typography.bodySmall)
                    val progress = chosen.progress
                    if (progress == null) LinearProgressIndicator(Modifier.fillMaxWidth())
                    else LinearProgressIndicator(progress = { progress }, modifier = Modifier.fillMaxWidth())
                }
                if (chosen.error.isNotBlank()) Text(chosen.error, color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodySmall)
            } },
            confirmButton = { FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)) {
                Button(onClick = { files.open(save = false) }, enabled = !chosen.busy && files.export == null && !files.clearing) { Text("Open") }
                OutlinedButton(onClick = { files.open(save = true) }, enabled = !chosen.busy && files.export == null && !files.clearing) { Text("Save…") }
                TextButton(onClick = files::close) { Text(if (chosen.busy) "Cancel" else "Close") }
            } })
    }
}

/** A separate registry key and captured ID prevent a late result saving another file. */
@Composable private fun ComputerFileSaveHost(files: ComputerFiles, pending: ComputerFileExport) {
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        files.finishExport(pending.id, result.data?.data?.takeIf { result.resultCode == Activity.RESULT_OK })
    }
    LaunchedEffect(pending.id) {
        if (files.markExportLaunched(pending.id)) {
            try {
                launcher.launch(Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType(pending.opened.mime).putExtra(Intent.EXTRA_TITLE, pending.opened.name))
            } catch (_: Exception) {
                files.finishExport(pending.id, null)
                files.failed(pending.selectionId, "Android could not open the save dialog. Try again.")
            }
        }
    }
}
