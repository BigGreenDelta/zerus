package app.zerus.mobile

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.flow.first

/** One staged form: model list, pinned effort entry, and a separate effort page. */
@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun NativeSettingsSheetContent(
    settings: NativeSettings,scope: String,selectedModel: String,selectedEffort: String,reason: String,
    onModelChange: (String) -> Unit,onEffortChange: (String) -> Unit,
    onApply: () -> Unit,onDismiss: () -> Unit,modifier: Modifier = Modifier,nativeEfforts: Boolean = false,
) {
    var effortPage by remember { mutableStateOf(false) }
    val modelLabel = settings.models.find { it.id == selectedModel }?.label ?: selectedModel.ifBlank { "Not reported" }
    val efforts = SessionDetailsPresentation.effortChoices(settings,selectedModel,nativeEfforts)
    fun effortLabel(effort: String) = SessionDetailsPresentation.effortLabel(settings,selectedModel,effort)
    val modelsScroll = rememberLazyListState()
    val effortsScroll = rememberLazyListState()
    val currentScroll = if(effortPage) effortsScroll else modelsScroll
    val groupedModels = settings.models.any { it.provider.isNotBlank() }
    val selectedIndex = if(effortPage) efforts.indexOf(selectedEffort) else settings.models.indexOfFirst { it.id == selectedModel }
    LaunchedEffect(effortPage,selectedIndex,groupedModels) {
        if(!effortPage && groupedModels) return@LaunchedEffect
        if(selectedIndex >= 0) {
            snapshotFlow { currentScroll.layoutInfo.totalItemsCount }.first { it > selectedIndex }
            val layout = currentScroll.layoutInfo
            if(layout.visibleItemsInfo.none { it.index == selectedIndex && it.offset >= layout.viewportStartOffset &&
                    it.offset + it.size <= layout.viewportEndOffset }) currentScroll.scrollToItem(selectedIndex)
        }
    }
    BackHandler(enabled = effortPage) { effortPage = false }
    Column(modifier.fillMaxWidth().heightIn(max = LocalConfiguration.current.screenHeightDp.dp * .9f)
        .padding(start = 20.dp,end = 20.dp,bottom = 16.dp),verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(Modifier.fillMaxWidth(),verticalAlignment = Alignment.CenterVertically) {
            if(effortPage) IconButton(onClick = { effortPage = false }) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack,"Back to models")
            } else Spacer(Modifier.size(48.dp))
            Text(if(effortPage) "Effort" else "Select model",Modifier.weight(1f),
                style = MaterialTheme.typography.titleLarge,fontWeight = FontWeight.SemiBold,textAlign = TextAlign.Center)
            Spacer(Modifier.size(48.dp))
        }
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Settings apply to $scope.",style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,maxLines = 2,overflow = TextOverflow.Ellipsis)
            if(settings.pendingId.isNotBlank()) {
                val pendingLabel = settings.models.find { it.id == settings.pendingModel }?.label ?: settings.pendingModel
                Text("Pending: $pendingLabel / ${SessionDetailsPresentation.effortLabel(settings,settings.pendingModel,settings.pendingEffort)}",style = MaterialTheme.typography.bodySmall,
                    maxLines = 2,overflow = TextOverflow.Ellipsis)
            }
            if(effortPage) Text(modelLabel,style = MaterialTheme.typography.labelLarge)
        }
        if(!effortPage && groupedModels) {
            val models=settings.models.map { option -> OpenCodeModel(option.id,option.label,option.provider,option.providerLabel,
                option.free,option.favorite,option.isDefault,option.efforts.map { NativeEffort(it,option.effortLabels[it]) }) }
            OpenCodeModelList(models,selectedModel,onModelChange,Modifier.fillMaxWidth().weight(1f,fill=false))
        } else LazyColumn(Modifier.fillMaxWidth().weight(1f,fill = false).clip(RoundedCornerShape(16.dp)).selectableGroup(),
            state = currentScroll,verticalArrangement = Arrangement.spacedBy(2.dp)) {
            if(effortPage) itemsIndexed(efforts) { _,effort ->
                NativeSettingsRow(effortLabel(effort),"Effort, ${effortLabel(effort)}",selectedEffort == effort) { onEffortChange(effort) }
            } else itemsIndexed(settings.models) { _,option ->
                NativeSettingsRow(option.label,"Model, ${option.label}",selectedModel == option.id) { onModelChange(option.id) }
            }
        }
        if(!effortPage) OpenCodeCatalog.truncationMessage(settings.modelsTruncated,false).takeIf(String::isNotBlank)?.let {
            Text(it,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if(!effortPage) Surface(onClick = { effortPage = true },enabled = efforts.size > 1 || selectedEffort.isNotBlank(),
            shape = RoundedCornerShape(16.dp),color = MaterialTheme.colorScheme.surfaceContainer,
            modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).semantics { contentDescription = "Choose effort, ${effortLabel(selectedEffort)}" }) {
            Row(Modifier.padding(16.dp),verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FlowRow(Modifier.weight(1f),horizontalArrangement = Arrangement.SpaceBetween,
                    verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text("Effort")
                    Text(effortLabel(selectedEffort),color = MaterialTheme.colorScheme.primary)
                }
                Icon(Icons.Default.ChevronRight,null)
            }
        }
        Column(Modifier.fillMaxWidth().heightIn(max = 128.dp).verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(when(settings.applyWhen) {
                "ready" -> "Apply when the agent is ready."; "resume" -> "Save for the next resume."
                "now" -> "Apply now."; else -> "The machine determines when these settings apply."
            },style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
            if(settings.supported && settings.notice.isNotBlank()) Text(settings.notice,style = MaterialTheme.typography.bodySmall)
            if(reason.isNotBlank()) Text(reason,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.error)
        }
        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
            FlowRow(Modifier.fillMaxWidth(),horizontalArrangement = Arrangement.spacedBy(8.dp,Alignment.End),
                verticalArrangement = Arrangement.spacedBy(8.dp)) {
                TextButton(onClick = onApply,enabled = reason.isBlank()) {
                    val applyingPending = settings.pendingId.isNotBlank() && selectedModel == settings.pendingModel && selectedEffort == settings.pendingEffort
                    Text(when(settings.applyWhen) {
                        "ready" -> "Apply when ready"; "resume" -> "Save for resume"; else -> if(applyingPending) "Apply pending" else "Apply"
                    })
                }
                val cancelFocus = remember { FocusRequester() }
                val window = LocalWindowInfo.current
                LaunchedEffect(Unit) {
                    snapshotFlow { window.isWindowFocused }.first { it }
                    cancelFocus.requestFocus()
                }
                TextButton(onClick = onDismiss,modifier = Modifier.focusRequester(cancelFocus)) { Text("Cancel") }
            }
        }
    }
}

@Composable private fun NativeSettingsRow(label: String,description: String,selected: Boolean,onClick: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).selectable(selected,role = Role.RadioButton,onClick = onClick)
            .semantics { contentDescription = description }) {
        Row(Modifier.padding(horizontal = 16.dp,vertical = 12.dp),verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(label,Modifier.weight(1f),color = if(selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface)
            if(selected) Icon(Icons.Default.Check,null,Modifier.size(24.dp),tint = MaterialTheme.colorScheme.primary)
            else Spacer(Modifier.size(24.dp))
        }
    }
}
