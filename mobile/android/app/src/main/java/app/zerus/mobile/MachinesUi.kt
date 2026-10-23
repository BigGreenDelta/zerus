package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp

private val Mint = Color(0xFF67E8CB)
private val Surface = Color(0xFF1A2225)
private val Muted = Color(0xFF9AACB2)

@Composable internal fun MachinesScreen(model: ZerusViewModel, live: Boolean, onPair: () -> Unit, onDisconnect: (Connection) -> Unit,
    onNotifications: (Boolean) -> Unit, onLive: (Boolean) -> Unit) {
    var naming by remember { mutableStateOf<Machine?>(null) }
    var coloring by remember { mutableStateOf<Machine?>(null) }
    val trees = remember(model.machines) { MachineCatalog.groupsByConnection(model.machines) }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Text("Machines", style = MaterialTheme.typography.headlineMedium, fontWeight = FontWeight.SemiBold)
        if (model.demo) {
            MachineTree(model, trees["demo"].orEmpty(), onRename = { naming = it }, onColor = { coloring = it })
            OutlinedButton(onClick = { model.stopPreview() }) { Text("Exit preview") }
        }
        model.connections.forEach { connection -> Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(connection.displayName, fontWeight = FontWeight.SemiBold)
            Text(connection.endpoint, color = Muted, style = MaterialTheme.typography.bodySmall)
            Text(if(connection.protocolVersion==2) "Encrypted workspace. Refresh to check enrollment and connected computers." else "Legacy connection. This relay can read relayed content.",color=Muted,style=MaterialTheme.typography.bodySmall)
            if(connection.id in model.pendingEncryptedPairings) TextButton(onClick={model.resumeEncryptedPair(connection)},enabled=!model.busy) { Text("Resume pairing") }
            if (!model.demo) MachineTree(model, trees[connection.id].orEmpty(),
                onRename = { naming = it }, onColor = { coloring = it })
            Text(if (!BuildConfig.FIREBASE_ENABLED) "Push is unavailable in this build. Use the live connection."
                else model.pushStatuses[connection.id].orEmpty().ifBlank { "Setting up Firebase push…" }, color = Muted, style = MaterialTheme.typography.bodySmall)
            TextButton(onClick = { onDisconnect(connection) }) { Text(if(connection.protocolVersion==2) "Forget encrypted connection" else "Disconnect workspace") }
        } }
        OutlinedButton(onClick = onPair, modifier = Modifier.fillMaxWidth()) { Icon(Icons.Default.Add, null); Spacer(Modifier.width(8.dp)); Text("Pair another workspace") }
        HorizontalDivider()
        Text("Notifications", style = MaterialTheme.typography.titleLarge)
        Row(verticalAlignment = Alignment.CenterVertically) { Column(Modifier.weight(1f)) { Text("Session alerts"); Text("Choose which session updates can alert you.", style = MaterialTheme.typography.bodySmall, color = Muted) }; Switch(model.notifications, onNotifications) }
        NotificationTypeSettings(model.notifications)
        MobileDeliverySettings(model)
        Row(verticalAlignment = Alignment.CenterVertically) { Column(Modifier.weight(1f)) { Text("Keep a live connection"); Text("For phones without Firebase push. Uses an ongoing notification; Android may delay alerts during battery saving.", style = MaterialTheme.typography.bodySmall, color = Muted) }; Switch(live, onLive, enabled = model.connections.isNotEmpty()) }
    }
    coloring?.let { machine ->
        val key = MachineKey(machine.connectionId,machine.id)
        ObscureConversation()
        MachineColorDialog(model.machineName(key.connectionId,key.computerId,machine.name),key,
            model.machineColorOverride(key),key in model.machineColorSaving,
            onSave = { color -> model.setMachineColor(key,color) { coloring=null } },onDismiss = { coloring=null })
    }
    naming?.let { machine ->
        val key=MachineKey(machine.connectionId,machine.id)
        var name by remember(key) { mutableStateOf(machine.name) }
        val saving=key in model.machineNameSaving
        val valid=runCatching { MachineNames.checked(name) }.isSuccess
        fun saveName() { if(valid&&!saving) model.renameMachine(key,name) { naming=null } }
        ZerusAlertDialog(onDismissRequest={ if(!saving) naming=null },title={ Text("Name on this phone") },text={
            Column(verticalArrangement=Arrangement.spacedBy(12.dp)) {
                Text("This private label applies only to this machine in this workspace. Its hostname, SSH settings and session identity stay unchanged.")
                Text(model.connections.find { it.id==machine.connectionId }?.displayName.orEmpty(),style=MaterialTheme.typography.labelMedium)
                OutlinedTextField(name,{name=it},label={Text("Machine name")},singleLine=true,readOnly=saving,isError=!valid,keyboardOptions=KeyboardOptions(imeAction=ImeAction.Done),keyboardActions=KeyboardActions(onDone={saveName()}))
                Text("Reported name: ${machine.nativeName}",style=MaterialTheme.typography.bodySmall)
                if(model.machineAlias(key).isNotBlank()) TextButton(onClick={model.renameMachine(key,null) { naming=null }},enabled=!saving) { Text("Reset to reported name") }
            }
        },confirmButton={TextButton(onClick=::saveName,enabled=valid&&!saving) { Text(if(saving) "Saving…" else "Save") }},
            dismissButton={TextButton(onClick={naming=null},enabled=!saving) { Text("Cancel") }})
    }

}

@Composable private fun MachineTree(model: ZerusViewModel, groups: List<MachineTreeGroup>,
    onRename: (Machine) -> Unit, onColor: (Machine) -> Unit) {
    groups.forEach { group ->
        val root = group.root
        key(MachineKey(root.machine.connectionId, root.machine.id)) {
            Card(colors = CardDefaults.cardColors(containerColor = Surface)) {
                Column {
                    MachineTreeRowContent(model, root, onRename, onColor, hasChildren = group.children.isNotEmpty())
                    group.children.forEachIndexed { index, child ->
                        key(MachineKey(child.machine.connectionId, child.machine.id)) {
                            MachineTreeRowContent(model, child, onRename, onColor, lastChild = index == group.children.lastIndex)
                        }
                    }
                }
            }
        }
    }
}

@Composable private fun MachineTreeRowContent(model: ZerusViewModel, row: MachineTreeRow,
    onRename: (Machine) -> Unit, onColor: (Machine) -> Unit,
    hasChildren: Boolean = false, lastChild: Boolean = false) {
    val machine = row.machine
    val machineKey = MachineKey(machine.connectionId, machine.id)
    var menu by remember(machineKey) { mutableStateOf(false) }
    var badgeHeight by remember(machineKey) { mutableIntStateOf(0) }
    val child = row.parent != null
    val caption = when (machine.route) {
        MachineRoute.Direct -> "Gateway"
        MachineRoute.Unknown -> ""
        is MachineRoute.Via -> row.gatewayName.takeIf { it.isNotBlank() }
            ?.let { "Through $it" } ?: "Through another computer"
    }
    Box(Modifier.fillMaxWidth().drawBehind {
        if (badgeHeight > 0 && (child || hasChildren)) {
            val trunk = 30.dp.toPx()
            val badgeTop = 10.dp.toPx()
            val badgeCenter = badgeTop + badgeHeight / 2f
            val stroke = 1.dp.toPx()
            val color = Muted.copy(alpha = .5f)
            if (child) {
                drawLine(color, Offset(trunk, 0f), Offset(trunk, if (lastChild) badgeCenter else size.height), stroke)
                drawLine(color, Offset(trunk, badgeCenter), Offset(40.dp.toPx(), badgeCenter), stroke)
            } else {
                drawLine(color, Offset(trunk, badgeTop + badgeHeight), Offset(trunk, size.height), stroke)
            }
        }
    }) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp), verticalAlignment = Alignment.Top,
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (child) Spacer(Modifier.width(20.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                MachineLabel(machine.name, modifier = Modifier.onSizeChanged { badgeHeight = it.height },
                    colorHex = model.machineColor(machineKey))
                Column(Modifier.padding(start = if (hasChildren) 28.dp else 0.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    if (caption.isNotEmpty()) Text(caption, color = Muted, style = MaterialTheme.typography.bodySmall)
                    Text(MachineCatalog.status(machine),
                        color = if (machine.online && !machine.lastKnown) Mint else Muted, style = MaterialTheme.typography.bodySmall)
                }
            }
            if (!model.demo) {
                Box {
                    IconButton(onClick = { menu = true }, enabled = model.storageReady, modifier = Modifier.size(48.dp)) {
                        Icon(Icons.Default.MoreVert, "Actions for ${machine.name}")
                    }
                    DropdownMenu(expanded = menu && model.storageReady, onDismissRequest = { menu = false }) {
                        DropdownMenuItem(text = { Text("Rename machine") }, leadingIcon = { Icon(Icons.Default.Edit, null) },
                            onClick = { menu = false; onRename(machine) })
                        DropdownMenuItem(text = { Text("Label color") }, leadingIcon = { Icon(Icons.Default.Palette, null) },
                            onClick = { menu = false; onColor(machine) })
                    }
                }
            }
        }
    }
}
