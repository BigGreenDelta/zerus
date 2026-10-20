package app.zerus.mobile

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

/** Read-only catalog marks; selecting a row only changes the caller's staged model. */
@OptIn(ExperimentalFoundationApi::class)
@Composable internal fun OpenCodeModelList(
    models: List<OpenCodeModel>, selected: String, onSelect: (String) -> Unit,
    modifier: Modifier = Modifier, nativeDefault: String? = null,
) {
    var query by remember(models) { mutableStateOf("") }
    val visible = remember(models, query) { OpenCodeCatalog.filter(models, query) }
    val groups = remember(visible) { OpenCodeCatalog.group(visible) }
    val scroll = rememberLazyListState()
    LaunchedEffect(models, selected) {
        var index = if (nativeDefault != null) 1 else 0
        val selectedIndex = if (selected.isEmpty() && nativeDefault != null) 0 else groups.firstNotNullOfOrNull { group ->
            val found = group.models.indexOfFirst { it.value == selected }
            val row = if (found >= 0) index + 1 + found else null
            index += 1 + group.models.size
            row
        }
        if (selectedIndex != null) scroll.scrollToItem(selectedIndex)
    }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (models.size > 8) OutlinedTextField(query, { query = it }, Modifier.fillMaxWidth(),
            label = { Text("Search models") }, singleLine = true)
        LazyColumn(Modifier.fillMaxWidth().weight(1f, fill = false).selectableGroup(), state = scroll,
            verticalArrangement = Arrangement.spacedBy(2.dp)) {
            if (nativeDefault != null) item(key = "native-default") {
                OpenCodeModelRow(nativeDefault, "", emptyList(), selected.isBlank()) { onSelect("") }
            }
            groups.forEach { group ->
                stickyHeader(key = "provider:${group.provider}") {
                    Surface(color = MaterialTheme.colorScheme.surface) {
                        Text(group.label, Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                            style = MaterialTheme.typography.labelLarge)
                    }
                }
                items(group.models, key = { "model:${it.value}" }) { model ->
                    OpenCodeModelRow(model.label, model.value, OpenCodeCatalog.marks(model), selected == model.value) { onSelect(model.value) }
                }
            }
            if (visible.isEmpty()) item { Text(if (query.isBlank()) "No models reported." else "No models match this search.",
                Modifier.padding(12.dp), style = MaterialTheme.typography.bodySmall) }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable private fun OpenCodeModelRow(label: String, value: String, marks: List<String>, selected: Boolean, onSelect: () -> Unit) {
    val description = (listOf("Model", label) + marks).joinToString(", ")
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp)
            .selectable(selected, role = Role.RadioButton, onClick = onSelect).semantics { contentDescription = description }) {
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(label, color = if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface)
                if (value.isNotBlank() && value != label) Text(value, style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    marks.forEach { mark ->
                        Surface(color = MaterialTheme.colorScheme.surfaceContainerHigh, shape = RoundedCornerShape(6.dp)) {
                            Row(Modifier.padding(horizontal = 6.dp, vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
                                if (mark == "Favorite") Icon(Icons.Default.Star, null, Modifier.size(14.dp))
                                Text(mark, style = MaterialTheme.typography.labelSmall)
                            }
                        }
                    }
                }
            }
            if (selected) Icon(Icons.Default.Check, null, Modifier.size(24.dp), tint = MaterialTheme.colorScheme.primary)
            else Spacer(Modifier.size(24.dp))
        }
    }
}
