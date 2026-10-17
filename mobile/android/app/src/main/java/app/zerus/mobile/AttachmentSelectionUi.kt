package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/** The pending selection is global, so its recovery must remain reachable from another conversation. */
@Composable internal fun PendingAttachmentSelection(model: ZerusViewModel, target: Target? = null) {
    val origin = model.pickerDraft ?: return
    val selectionId = model.pickerId
    Column(Modifier.fillMaxWidth()) {
        val destination = if (target == origin.target) "" else " for ${origin.target.session}"
        Text(if (model.attachmentImporting) "Saving selected files$destination…" else "File selection is pending$destination.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        TextButton(onClick = { model.cancelAttachmentPick(selectionId) },
            enabled = !model.attachmentPreparing && !model.updateInstallPreparing,
            modifier = Modifier.align(Alignment.End).heightIn(min = 48.dp)) {
            Text(if (model.attachmentImporting) "Cancel import" else "Cancel selection")
        }
    }
}
