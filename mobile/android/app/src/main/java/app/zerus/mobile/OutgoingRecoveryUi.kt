package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

@Composable internal fun OutgoingRecovery(message: OutgoingMessage, checking: Boolean, onCheck: () -> Unit, onReview: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, shape = MaterialTheme.shapes.medium) {
        Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Previous message not confirmed", color = MaterialTheme.colorScheme.tertiary,
                    maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.labelMedium)
                Text(message.text.ifBlank { message.attachments.joinToString { it.name } }, maxLines = 1,
                    overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodySmall)
            }
            IconButton(onClick = onCheck, enabled = !checking, modifier = Modifier.size(48.dp)) {
                if (checking) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                else Icon(Icons.Default.Refresh, "Check previous message delivery")
            }
            IconButton(onClick = onReview, enabled = !checking, modifier = Modifier.size(48.dp)) {
                Icon(Icons.Default.MoreVert, "Review previous message")
            }
        }
    }
}
