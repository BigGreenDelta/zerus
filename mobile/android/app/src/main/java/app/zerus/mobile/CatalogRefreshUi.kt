package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** Remains readable after a fast request; an account refresh continues independently
 * on its computer and is described on that account, not as phone network activity. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable fun CatalogRefreshStatus(model:ZerusViewModel) {
    if(model.demo || !model.storageReady || model.connections.isEmpty()) return
    val state=model.catalogRefresh
    if(state.workspaces.isEmpty()) return
    var details by remember { mutableStateOf(false) }
    val received=state.workspaces.mapNotNull { it.lastReceivedAt }.maxOrNull()?.let {
        java.text.DateFormat.getTimeInstance(java.text.DateFormat.SHORT).format(java.util.Date((it*1000).toLong()))
    }
    val summary=when {
        state.running -> "Updating…"
        state.failures>0 -> "Update failed · Saved data kept"
        else -> received?.let { "Updated $it" } ?: "Connected"
    }
    val tone=if(state.failures>0 && !state.running) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
    Surface(color=MaterialTheme.colorScheme.surfaceContainerLow) {
        Row(Modifier.fillMaxWidth().heightIn(min=40.dp).padding(start=16.dp,end=4.dp),verticalAlignment=Alignment.CenterVertically) {
            Box(Modifier.size(14.dp),contentAlignment=Alignment.Center) {
                if(state.running) CircularProgressIndicator(Modifier.size(14.dp),strokeWidth=2.dp)
            }
            Text(summary,Modifier.weight(1f).padding(horizontal=8.dp,vertical=8.dp),
                style=MaterialTheme.typography.labelSmall,color=tone,maxLines=1,overflow=TextOverflow.Ellipsis)
            IconButton(onClick={details=true},modifier=Modifier.size(40.dp)) { Icon(Icons.Default.Info,"Refresh details",Modifier.size(18.dp),tint=tone) }
        }
    }
    if(details) ModalBottomSheet(onDismissRequest={details=false}) {
        LazyColumn(Modifier.fillMaxWidth(),contentPadding=PaddingValues(start=16.dp,end=16.dp,bottom=32.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
            item { Text("Refresh details",style=MaterialTheme.typography.titleLarge) }
            item { Text("Phone sync reads the latest machine reports. Usage providers update separately on each computer; syncing does not force a provider refresh.",style=MaterialTheme.typography.bodySmall) }
            items(state.workspaces,key={it.id}) { workspace ->
                Column(verticalArrangement=Arrangement.spacedBy(4.dp)) {
                    Text(workspace.name,style=MaterialTheme.typography.titleSmall)
                    Text(workspace.stage.label,style=MaterialTheme.typography.bodySmall)
                    if(workspace.stage in setOf(CatalogRefreshStage.Failed,CatalogRefreshStage.Interrupted))
                        Text("Saved sessions, accounts and counters remain visible.",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.error)
                    Text(workspace.lastReceivedAt?.let { "Last received ${AccountPresentation.date(it) ?: "at an unknown time"}" }
                        ?: "No reports received in this app session.",style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            item { Text("Accounts shows each machine's provider refresh state and the time its usage report was checked. Those values can arrive after phone sync finishes.",style=MaterialTheme.typography.bodySmall) }
        }
    }
}
