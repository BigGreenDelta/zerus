package app.zerus.mobile

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp

/** Literal logical project names, with one explicit Move. Opening or selecting never mutates. */
@Composable internal fun SessionProjectDialog(controller:SessionProjectController,target:Target,onDismiss:()->Unit) {
    LaunchedEffect(controller,target) { controller.open(target) }
    DisposableEffect(controller,target) { onDispose { controller.dismiss() } }
    val catalog=controller.catalog
    var chosen by remember(target,catalog?.epoch,catalog?.membershipToken) { mutableStateOf<SessionProjectChoice?>(null) }
    ZerusAlertDialog(onDismissRequest=onDismiss,title={ Text("Move session to project") },text={
        Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
            Text("Change this session’s project grouping. Its working folder and running agent stay unchanged.")
            if(controller.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
            controller.reason(target).takeIf { it.isNotBlank() }?.let { Text(it) }
            if(controller.notice.isNotBlank()) Text(controller.notice)
            controller.pending.forEach { action ->
                Text(SessionProjectProtocol.uncertain,style=MaterialTheme.typography.bodySmall)
                TextButton(onClick={ controller.check(action) },enabled=action.requestId !in controller.active) { Text("Check original request") }
            }
            if(catalog!=null) {
                if(catalog.projects.isEmpty()) Text("No live logical projects are available in this swarm.")
                LazyColumn(Modifier.fillMaxWidth().heightIn(max=320.dp)) {
                    items(catalog.projects,key={ it.id }) { project ->
                        Row(Modifier.fillMaxWidth().clickable(role=Role.RadioButton,enabled=controller.canAssign(project),onClick={ chosen=project }).padding(vertical=4.dp),
                            verticalAlignment=Alignment.CenterVertically) {
                            RadioButton(selected=chosen==project,onClick=null,enabled=controller.canAssign(project))
                            Column(Modifier.weight(1f).padding(start=8.dp)) {
                                Text(project.name)
                                if(project.id==catalog.currentProjectId) Text("Current project",style=MaterialTheme.typography.bodySmall)
                            }
                        }
                    }
                }
            }
            TextButton(onClick={ chosen=null;controller.refresh() },enabled=!controller.loading && controller.active.isEmpty()) { Text("Refresh projects") }
        }
    },confirmButton={ TextButton(onClick={ chosen?.let(controller::assign);chosen=null },enabled=chosen?.let(controller::canAssign)==true) { Text("Move") } },
        dismissButton={ TextButton(onClick=onDismiss) { Text("Close") } })
}
