package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

/** The timer changes this small card only, never the message parser or editor state. */
@Composable internal fun NativeRecoveryCard(model: ZerusViewModel,target: Target) {
    val raw = model.activity
    val job = remember(raw,target,model.activityVerified) { RecoveryPresentation.from(raw,target,model.activityVerified) } ?: return
    if(job.state == "succeeded" || job.resumed) return
    var now by remember(job.id) { mutableDoubleStateOf(System.currentTimeMillis()/1000.0) }
    LaunchedEffect(job.id,job.state) { while(job.state == "waiting") { now=System.currentTimeMillis()/1000.0;delay(1000) } }
    var history by remember(target,job.id) { mutableStateOf(false) }
    val nowReason = model.recoveryActionReason(target,job.id,"now")
    val cancelReason = model.recoveryActionReason(target,job.id,"cancel")
    Card(Modifier.fillMaxWidth().padding(horizontal = 12.dp,vertical = 4.dp),colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        Column(Modifier.padding(horizontal = 12.dp,vertical = 8.dp),verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(job.status(now) + if(job.recorded) " / Last known" else "",fontWeight = FontWeight.SemiBold,style = MaterialTheme.typography.labelLarge)
            Text(job.attempts,style = MaterialTheme.typography.bodySmall)
            if(job.reason.isNotBlank()) Text(job.reason,style = MaterialTheme.typography.bodySmall,maxLines = 2,overflow = TextOverflow.Ellipsis)
            else if(job.action == "retry_request") Text("Retry the failed provider request",style = MaterialTheme.typography.bodySmall)
            else if(job.action == "continue_message") Text("Send a continuation message in this conversation",style = MaterialTheme.typography.bodySmall)
            Row {
                if(job.state == "waiting") {
                    TextButton(onClick = { model.recoveryAction(target,job.id,"now") },enabled = !job.recorded && nowReason.isBlank() && job.retryTimeAllowed(now)) { Text("Retry now") }
                    TextButton(onClick = { model.recoveryAction(target,job.id,"cancel") },enabled = !job.recorded && cancelReason.isBlank()) { Text("Cancel retry") }
                }
                TextButton(onClick = { history = true }) { Text("Details") }
            }
        }
    }
    if(history) {
        ObscureConversation()
        ZerusAlertDialog(onDismissRequest = { history = false },title = { Text("Native recovery") },text = {
            SelectionContainer { Column(Modifier.heightIn(max = 440.dp).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(job.status(now));Text(job.attempts)
                if(job.reason.isNotBlank()) Text(job.reason)
                if(job.recorded) Text("Last known job. Actions require a fresh matching session and recovery job.")
                if(job.state == "waiting" && !job.retryTimeAllowed(now)) Text("Retry now respects the provider's reported minimum retry time.")
                listOf(nowReason,cancelReason).filter { it.isNotBlank() }.distinct().forEach { Text(it,style = MaterialTheme.typography.bodySmall) }
                Text("Recorded attempts",fontWeight = FontWeight.SemiBold)
                if(job.history.isEmpty()) Text("No attempt history reported.")
                job.history.forEach { attempt ->
                    Text(listOfNotNull(attempt.at?.let { java.text.SimpleDateFormat("d MMM HH:mm:ss",java.util.Locale.getDefault()).format(java.util.Date((it*1000).toLong())) },
                        attempt.status,attempt.attempt?.let { "$it attempts" }).joinToString(" / "),style = MaterialTheme.typography.labelMedium)
                    if(attempt.reason.isNotBlank()) Text(attempt.reason)
                }
            } }
        },confirmButton = { TextButton(onClick = { history = false }) { Text("Close") } })
    }
}
