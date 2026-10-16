package app.zerus.mobile

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

class AndroidUpdateViewModel(application: Application): AndroidViewModel(application) {
    val updates=AndroidUpdates.get(application)
    val state=updates.state
    var error by mutableStateOf(""); private set
    var busy by mutableStateOf(false);private set
    var preparingInstall by mutableStateOf(false);private set
    /** The session committed by the latest Install tap while the update screen stayed open. */
    var requestedConfirmation by mutableIntStateOf(-1);private set
    private var downloadJob:Job?=null
    init {
        UpdateDiscoveryWorker.schedule(application)
        viewModelScope.launch { try { updates.recover();updates.check() } catch(_:Exception) { error="Updates are currently unavailable. You can check again." } }
    }
    fun recover() = viewModelScope.launch { try { updates.recover() } catch(_:Exception) { error="Could not read the saved installer state. No installation was repeated." } }
    fun check() = viewModelScope.launch { if(busy) return@launch;busy=true;error=""
        try { updates.check(manual=true) } catch(_:Exception) { error="Could not check for updates." } finally { busy=false } }
    fun setChannel(channel: String) = viewModelScope.launch {
        if(busy) return@launch;busy=true;error=""
        try { updates.setChannel(channel) } catch(_:Exception) { error="Could not change the update channel." } finally { busy=false }
    }
    fun download(update:AndroidUpdate) {
        if(busy) return
        busy=true;error=""
        downloadJob=viewModelScope.launch { try { updates.download(update) } catch(_:Exception) { } finally { busy=false;downloadJob=null } }
    }
    fun cancelDownload() { downloadJob?.cancel() }
    fun installPermission(onOpen:()->Unit) { try { onOpen() } catch(_:Exception) { error="Android installation settings could not open. Allow app updates for Zerus in Android settings, then try again." } }
    fun install(model:ZerusViewModel,update:AndroidUpdate) = viewModelScope.launch {
        if(busy || !UpdateInstallGuard.canStart(state.value.sessionId,state.value.downloaded,update)) return@launch
        busy=true;preparingInstall=true;error="";requestedConfirmation=-1
        try { model.prepareUpdateInstall();requestedConfirmation=updates.install(update) }
        catch(_:Exception) { error=model.updateInstallReason().ifBlank { "Could not prepare this update. Check Android installation permission and the saved installer state." } }
        finally { model.finishUpdateInstallPreparation();preparingInstall=false;busy=false }
    }
    fun confirm(model:ZerusViewModel,onConfirm:(Intent)->Unit) = viewModelScope.launch {
        if(busy) return@launch
        val id=state.value.sessionId
        if(!UpdateInstallGuard.canOpenConfirmation(id,id,state.value.status,updates.confirmation != null)) return@launch
        busy=true;error=""
        try {
            model.prepareUpdateInstall()
            updates.openConfirmation(id,onConfirm)
        } catch(_:Exception) { error=model.updateInstallReason().ifBlank { "Android confirmation could not open. The existing session remains saved." } }
        finally { model.finishUpdateInstallPreparation();busy=false }
    }
    /** Install already accepted Android's prompt, so open it once instead of asking for a second tap. */
    fun openRequestedConfirmation(model:ZerusViewModel,onConfirm:(Intent)->Unit) {
        if(busy || !UpdateInstallGuard.opensConfirmation(requestedConfirmation,state.value.sessionId,state.value.status,updates.confirmation != null)) return
        requestedConfirmation=-1
        confirm(model,onConfirm)
    }
    fun forgetRequestedConfirmation() { requestedConfirmation=-1 }
    fun cancelInstall() = viewModelScope.launch { if(busy) return@launch;busy=true;error=""
        try { updates.cancelInstall() } catch(_:Exception) { error="Could not cancel the existing installer session. No second installation started." } finally { busy=false } }
}

@Composable internal fun UpdatesContent(model: ZerusViewModel, updater: AndroidUpdateViewModel, interactionBlocked: () -> Boolean) {
    val state by updater.state.collectAsState()
    val context = LocalContext.current
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val lifecycleState by lifecycle.currentStateAsState()
    LaunchedEffect(state.status, state.sessionId, updater.requestedConfirmation, updater.busy, lifecycleState) {
        if(lifecycleState.isAtLeast(Lifecycle.State.RESUMED) && !interactionBlocked()) updater.openRequestedConfirmation(model) { intent ->
            check(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED));context.startActivity(intent)
        }
    }
    // Leaving the update screen withdraws the automatic prompt; the Continue button remains.
    DisposableEffect(updater) { onDispose { updater.forgetRequestedConfirmation() } }
    LaunchedEffect(state.sessionId,lifecycleState) {
        if(state.sessionId < 0 || !lifecycleState.isAtLeast(Lifecycle.State.RESUMED)) return@LaunchedEffect
        while(updater.state.value.sessionId == state.sessionId) {
            updater.recover().join()
            delay(1_000)
        }
    }
    val blocked=interactionBlocked()
    val busy=blocked || updater.busy || state.status in setOf("loading","checking","downloading")
    val selectionId = model.pickerId
    val canCancelSelection = model.pickerDraft != null && !model.attachmentPreparing && !model.attachmentImporting
    UpdateProgressPanel(state,busy,updater.preparingInstall,updater.updates.confirmation != null,
        model.updateInstallReason().takeUnless { updater.preparingInstall }.orEmpty(),updater.error,
        onChannel={ if(!blocked) updater.setChannel(it) },onAction={ action ->
            if(!blocked) when(action) {
                UpdateAction.Check -> updater.check()
                UpdateAction.Download -> state.available?.let(updater::download)
                UpdateAction.CancelDownload -> updater.cancelDownload()
                UpdateAction.Install -> state.downloaded?.let { update ->
                    if(!context.packageManager.canRequestPackageInstalls()) updater.installPermission {
                        context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:${context.packageName}")))
                    } else updater.install(model,update)
                }
                UpdateAction.Confirm -> updater.confirm(model) { intent ->
                    check(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED));context.startActivity(intent)
                }
                UpdateAction.Recover -> updater.recover()
                UpdateAction.CancelInstall -> updater.cancelInstall()
            }
        },cancelEnabled=!blocked && (!updater.busy || state.status=="downloading"),
        onCancelSelection=if(canCancelSelection) ({ if(!blocked && !updater.busy) model.cancelAttachmentPick(selectionId, pendingOnly=true) }) else null)
}

/** Stable slots keep status changes from moving the actions and the following About section. */
@Composable internal fun UpdateProgressPanel(state: UpdateState,busy: Boolean,preparing: Boolean,
    confirmationAvailable: Boolean,installReason: String,error: String,onChannel:(String)->Unit,
    onAction:(UpdateAction)->Unit,cancelEnabled:Boolean=true,onCancelSelection:(()->Unit)?=null) {
    val presentation=UpdatePresentation.content(state,preparing,confirmationAvailable)
    val pending=state.sessionId>=0
    Column(verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Text("Release channel",style=MaterialTheme.typography.labelLarge)
        Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            for(channel in listOf("stable","dev")) FilterChip(selected=state.channel==channel,
                onClick={ onChannel(channel) },enabled=!busy && !pending,
                label={ Text(if(channel=="dev") "Dev" else "Stable") })
        }
        Text(if(state.channel=="dev") "Latest verified main builds." else "Published stable releases.",style=MaterialTheme.typography.bodySmall)
        Text(presentation.version,style=MaterialTheme.typography.titleMedium)
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val density=LocalDensity.current
            val measurer=rememberTextMeasurer()
            val style=MaterialTheme.typography.bodyLarge
            val width=with(density) { maxWidth.roundToPx().coerceAtLeast(1) }
            val height=remember(measurer,width,style,density.density,density.fontScale) {
                (UpdatePresentation.installationStatuses+"One\nTwo").maxOf {
                    measurer.measure(it,style=style,constraints=Constraints(maxWidth=width)).size.height
                }
            }
            Box(Modifier.heightIn(min=with(density) { height.toDp() })) { Text(presentation.status) }
        }
        Box(Modifier.fillMaxWidth().height(4.dp)) {
            if(presentation.progress) {
                val progress=if(state.status=="downloading") state.available?.size?.takeIf { it>0 }?.let { (state.bytes.toFloat()/it).coerceIn(0f,1f) }
                    else state.installProgress?.takeIf { state.status=="awaiting_result" && it>0 }
                if(progress==null) LinearProgressIndicator(Modifier.fillMaxWidth())
                else LinearProgressIndicator(progress={ progress },modifier=Modifier.fillMaxWidth())
            }
        }
        val issue=state.error.ifBlank { error }.ifBlank { if(pending) "" else installReason }
        if(issue.isNotBlank()) Text(issue,color=MaterialTheme.colorScheme.error,style=MaterialTheme.typography.bodySmall)
        if(onCancelSelection != null) TextButton(onClick=onCancelSelection,enabled=!busy,
            modifier=Modifier.align(Alignment.End).heightIn(min=48.dp)) { Text("Cancel selection") }
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val density=LocalDensity.current
            val measurer=rememberTextMeasurer()
            val textWidth=with(density) { (maxWidth-48.dp).roundToPx().coerceAtLeast(1) }
            val style=MaterialTheme.typography.labelLarge
            val labelHeight=remember(measurer,textWidth,style,density.density,density.fontScale) {
                UpdatePresentation.primaryLabels.maxOf { measurer.measure(it,style=style,constraints=Constraints(maxWidth=textWidth)).size.height }
            }
            val buttonHeight=with(density) { labelHeight.toDp()+16.dp }.coerceAtLeast(48.dp)
            Button(onClick={ presentation.primary?.let(onAction) },
                enabled=!busy && presentation.primary != null && (presentation.primary !in setOf(UpdateAction.Install,UpdateAction.Confirm) || installReason.isBlank()),
                modifier=Modifier.fillMaxWidth().heightIn(min=buttonHeight)) { Text(presentation.primaryLabel) }
        }
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val density=LocalDensity.current
            val width=with(density) { ((maxWidth-8.dp)/2-24.dp).roundToPx().coerceAtLeast(1) }
            val measurer=rememberTextMeasurer()
            val style=MaterialTheme.typography.labelLarge
            val labelHeight=remember(measurer,width,style,density.density,density.fontScale) {
                listOf("Check for updates","Check status","Cancel").maxOf {
                    measurer.measure(it,style=style,constraints=Constraints(maxWidth=width)).size.height
                }
            }
            val rowHeight=with(density) { labelHeight.toDp()+16.dp }.coerceAtLeast(48.dp)
            Row(Modifier.fillMaxWidth().heightIn(min=rowHeight),horizontalArrangement=Arrangement.spacedBy(8.dp),verticalAlignment=Alignment.CenterVertically) {
                Box(Modifier.weight(1f)) { presentation.secondary?.let { action ->
                    TextButton(onClick={ onAction(action) },enabled=!busy,modifier=Modifier.fillMaxWidth()) {
                        Text(if(action==UpdateAction.Recover) "Check status" else "Check for updates")
                    }
                } }
                Box(Modifier.weight(1f)) { presentation.cancel?.let { action ->
                    TextButton(onClick={ onAction(action) },enabled=cancelEnabled,modifier=Modifier.fillMaxWidth()) { Text("Cancel") }
                } }
            }
        }
        Text("Android asks you to confirm installation. Your drafts, paired workspaces and app data stay saved.",style=MaterialTheme.typography.bodySmall)
        Text("Both channels update the same app. Switching channels never installs an older Android version.",style=MaterialTheme.typography.bodySmall)
    }
}
