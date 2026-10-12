package app.zerus.mobile

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp

/** Palette and automatic reset edit only this phone's exact machine profile. */
@Composable internal fun MachineColorDialog(name: String, key: MachineKey, selected: String?, saving: Boolean,
    onSave: (String?) -> Unit, onDismiss: () -> Unit) {
    var choice by remember(key, selected) { mutableStateOf(selected) }
    val preview = choice ?: MachineColors.automatic(key)
    ZerusAlertDialog(onDismissRequest = { if (!saving) onDismiss() }, title = { Text("Machine label color") }, text = {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Saved only on this phone for this machine in this workspace. You can change it while offline.")
            MachineLabel(name, colorHex = preview)
            Text("Selected: ${if (choice == null) "Automatic" else MachineColors.names.getOrNull(MachineColors.palette.indexOf(choice)) ?: "Custom"} ($preview)",
                style = MaterialTheme.typography.bodySmall)
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                MachineColors.palette.chunked(4).forEach { row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        row.forEach { hex ->
                            val label = MachineColors.names[MachineColors.palette.indexOf(hex)]
                            Box(Modifier.size(48.dp).selectable(selected = choice == hex, enabled = !saving,
                                role = Role.RadioButton, onClick = { choice = hex })
                                .semantics { contentDescription = "$label, $hex" }, contentAlignment = Alignment.Center) {
                                Box(Modifier.size(32.dp).background(Color(hex.drop(1).toLong(16) or 0xff000000), CircleShape),
                                    contentAlignment = Alignment.Center) {
                                    if (choice == hex) Icon(Icons.Default.Check, null, Modifier.size(20.dp), tint = Color.Black)
                                }
                            }
                        }
                    }
                }
            }
            Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).selectable(selected = choice == null, enabled = !saving,
                role = Role.RadioButton, onClick = { choice = null }), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                RadioButton(selected = choice == null, onClick = null, enabled = !saving)
                Text("Automatic (reset)")
            }
        }
    }, confirmButton = { TextButton(onClick = { onSave(choice) }, enabled = !saving) { Text(if (saving) "Saving…" else "Save") } },
        dismissButton = { TextButton(onClick = onDismiss, enabled = !saving) { Text("Cancel") } })
}
