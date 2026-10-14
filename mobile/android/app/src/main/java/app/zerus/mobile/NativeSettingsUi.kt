package app.zerus.mobile

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** Keep both settings discoverable even when the native model catalog is long. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun NativeSettingsChoices(
    models: List<NativeModelChoice>,selectedModel: String,efforts: List<String>,selectedEffort: String,
    onModelChange: (String) -> Unit,onEffortChange: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    var modelsOpen by remember { mutableStateOf(false) }
    val modelLabel = models.find { it.id == selectedModel }?.label ?: selectedModel.ifBlank { "Not reported" }
    Column(modifier.fillMaxWidth(),verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Model",fontWeight = FontWeight.SemiBold)
            Box(Modifier.fillMaxWidth()) {
                Surface(onClick = { modelsOpen = true },enabled = models.isNotEmpty(),
                    shape = RoundedCornerShape(12.dp),color = MaterialTheme.colorScheme.surfaceContainer,
                    border = BorderStroke(1.dp,MaterialTheme.colorScheme.outline),
                    modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).semantics {
                        contentDescription = "Model, $modelLabel"
                        stateDescription = if(modelsOpen) "Expanded" else "Collapsed"
                    }) {
                    Row(Modifier.padding(12.dp),verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(modelLabel,Modifier.weight(1f),style = MaterialTheme.typography.bodyLarge,
                            maxLines = 2,overflow = TextOverflow.Ellipsis)
                        Icon(Icons.Default.ExpandMore,null)
                    }
                }
                DropdownMenu(modelsOpen,onDismissRequest = { modelsOpen = false },modifier = Modifier.heightIn(max = 320.dp)) {
                    models.forEach { option ->
                        DropdownMenuItem(text = { Text(option.label) },
                            modifier = Modifier.semantics { selected = option.id == selectedModel },
                            trailingIcon = if(option.id == selectedModel) {
                                { Icon(Icons.Default.Check,"Selected",tint = MaterialTheme.colorScheme.primary) }
                            } else null,
                            onClick = { modelsOpen = false;onModelChange(option.id) })
                    }
                }
            }
        }
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Effort: ${effortLabel(selectedEffort)}",fontWeight = FontWeight.SemiBold)
            Text("Reasoning level for this model.",style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            FlowRow(Modifier.fillMaxWidth().selectableGroup(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),verticalArrangement = Arrangement.spacedBy(4.dp)) {
                (listOf("") + efforts).distinct().forEach { effort ->
                    val label = effortLabel(effort)
                    val chosen = selectedEffort == effort
                    Surface(shape = RoundedCornerShape(8.dp),
                        color = if(chosen) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainer,
                        contentColor = if(chosen) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurface,
                        border = BorderStroke(1.dp,if(chosen) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outline),
                        modifier = Modifier.heightIn(min = 48.dp).selectable(chosen,role = Role.RadioButton,
                            onClick = { onEffortChange(effort) }).semantics { contentDescription = "Effort, $label" }) {
                        Box(Modifier.padding(horizontal = 12.dp,vertical = 8.dp),contentAlignment = Alignment.Center) {
                            Text(label,style = MaterialTheme.typography.labelLarge)
                        }
                    }
                }
            }
        }
    }
}

private fun effortLabel(effort: String) = when(effort) {
    "" -> "Default"
    "xhigh" -> "Extra high"
    else -> effort.replaceFirstChar { it.uppercase() }
}
