package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

@Composable internal fun ConversationSearchDialog(model: ZerusViewModel,session: Session,
    onJump: (ConversationMessageJump) -> Unit,onDismiss: () -> Unit) {
    ObscureConversation()
    val target = session.target
    val current = model.selected?.target == target
    val events = if(current) model.conversationEvents else emptyList()
    var query by remember(target) { mutableStateOf("") }
    val prepared by produceState<Pair<List<Event>,List<SearchableMessage>>?>(null,events) {
        value = events to withContext(Dispatchers.Default) { ConversationSearch.prepare(events) }
    }
    val messages = prepared?.takeIf { it.first == events }?.second
    val searched by produceState<Pair<String,MessageSearchResults>?>(null,query,messages) {
        if(messages == null) return@produceState
        delay(150)
        value = query to withContext(Dispatchers.Default) { ConversationSearch.find(messages,query) }
    }
    val results = searched?.takeIf { it.first == query }?.second
    ZerusAlertDialog(onDismissRequest = onDismiss,title = { Text("Search loaded messages") },text = {
        Column(Modifier.heightIn(max = 520.dp),verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text("${events.size} messages currently loaded. Earlier or unloaded pages are outside this search.",
                style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
            OutlinedTextField(query,{ query = it },Modifier.fillMaxWidth(),label = { Text("Find literal text") },singleLine = true)
            if(!current) Text("This session changed. Close search before continuing.")
            else if(query.length > 2048) Text("Search text exceeds 2048 characters.",style = MaterialTheme.typography.bodySmall)
            else if(query.isNotBlank()) {
                Text(if(results == null) "Searching…" else "${results.matches} matches in loaded messages",style = MaterialTheme.typography.labelMedium)
                Column(Modifier.weight(1f,false).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    results?.hits.orEmpty().forEach { hit -> Surface(onClick = {
                        onDismiss()
                        onJump(ConversationMessageJump(target,hit.eventId))
                    },modifier = Modifier.fillMaxWidth(),color = MaterialTheme.colorScheme.surfaceContainerHigh,shape = MaterialTheme.shapes.small) {
                        Column(Modifier.padding(12.dp),verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text(when(hit.role) {
                                "AgentMessage", "Stop" -> DesktopIcons.providerName(session.agent).takeUnless { it == "Terminal" }.orEmpty().ifBlank { "Assistant" }
                                "AgentThinking" -> "Agent activity"
                                "Tool", "ToolUse", "ToolResult" -> "Tool"
                                else -> hit.role
                            },fontWeight = FontWeight.SemiBold,style = MaterialTheme.typography.labelMedium)
                            Text(hit.snippet,style = MaterialTheme.typography.bodySmall)
                        }
                    } }
                    if(results != null && results!!.matches > results!!.hits.size) Text("Showing the first ${results!!.hits.size} matches. Narrow your search for another result.",style = MaterialTheme.typography.bodySmall)
                }
            }
        }
    },confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}
