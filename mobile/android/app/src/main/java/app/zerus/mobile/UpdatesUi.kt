package app.zerus.mobile

import android.app.Application
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.currentStateAsState
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

class AndroidUpdateViewModel(application: Application): AndroidViewModel(application) {
    val updates=AndroidUpdates.get(application)
    val state=updates.state
    var error by mutableStateOf(""); private set
    var busy by mutableStateOf(false);private set
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
        if(busy) return@launch;busy=true;error="";requestedConfirmation=-1
        try { model.prepareUpdateInstall();requestedConfirmation=updates.install(update) }
        catch(_:Exception) { error=model.updateInstallReason().ifBlank { "Could not prepare this update. Check Android installation permission and the saved installer state." } }
        finally { model.finishUpdateInstallPreparation();busy=false }
    }
    fun confirm(model:ZerusViewModel,onConfirm:(Intent)->Unit) = viewModelScope.launch {
        if(busy) return@launch
        val id=state.value.sessionId;val intent=updates.confirmation ?: return@launch
        busy=true;error=""
        try {
            model.prepareUpdateInstall()
            check(state.value.sessionId==id && state.value.status=="confirmation")
            onConfirm(intent)
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
    val pending = state.sessionId >= 0
    val lifecycleState by lifecycle.currentStateAsState()
    LaunchedEffect(state.status, state.sessionId, updater.requestedConfirmation, updater.busy, lifecycleState) {
        if(lifecycleState.isAtLeast(Lifecycle.State.RESUMED) && !interactionBlocked()) updater.openRequestedConfirmation(model) { intent ->
            check(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED));context.startActivity(intent)
        }
    }
    // Leaving the update screen withdraws the automatic prompt; the Continue button remains.
    DisposableEffect(updater) { onDispose { updater.forgetRequestedConfirmation() } }
    val busy = interactionBlocked() || updater.busy || state.status in setOf("loading", "checking", "downloading")
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Release channel",style=MaterialTheme.typography.labelLarge)
        Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
            for(channel in listOf("stable", "dev")) FilterChip(
                selected=state.channel == channel,
                onClick={ if(!interactionBlocked()) updater.setChannel(channel) },
                enabled=!busy && !pending,
                label={ Text(if(channel == "dev") "Dev" else "Stable") })
        }
        Text(if(state.channel == "dev") "Latest verified main builds." else "Published stable releases.",style=MaterialTheme.typography.bodySmall)
        Text(when(state.status) {
            "loading" -> "Reading saved update status…"
            "checking" -> "Checking the ${state.channel} channel…"
            "available" -> "Update ${state.available?.versionName.orEmpty()} is available."
            "downloading" -> "Downloading update…"
            "downloaded" -> "Update ${state.downloaded?.versionName.orEmpty()} is downloaded and verified."
            "installing" -> "Installation submitted to Android. Waiting for its result."
            "confirmation" -> "Android is waiting for your installation confirmation."
            "installed" -> "The updated app version is installed."
            "current" -> "This app is up to date on the ${state.channel} channel."
            "ahead" -> "The installed version is newer than this channel. Android cannot downgrade; wait for a newer compatible build."
            else -> "Check for a newer ${state.channel} build."
        })
        if(state.status=="downloading") LinearProgressIndicator(progress={ state.available?.size?.let { state.bytes.toFloat()/it } ?: 0f },modifier=Modifier.fillMaxWidth())
        if(state.error.isNotBlank()) Text(state.error,color=MaterialTheme.colorScheme.error)
        if(updater.error.isNotBlank()) Text(updater.error,color=MaterialTheme.colorScheme.error)
        Text("Checks are anonymous and independent of paired workspaces. Downloads and installation require your choice. Android will ask you to confirm installation.",style=MaterialTheme.typography.bodySmall)
        Text("Both channels update the same app and preserve its data. Switching channels never installs an older Android version.",style=MaterialTheme.typography.bodySmall)
        TextButton(onClick={ if (!interactionBlocked()) updater.check() },enabled=!busy && !pending) { Text("Check for updates") }
        state.available?.takeIf { it != state.downloaded }?.let { update ->
            Button(onClick={ if (!interactionBlocked()) updater.download(update) },enabled=!busy && !pending) { Text("Download ${update.versionName}") }
        }
        if(state.status=="downloading") TextButton(onClick={ if (!interactionBlocked()) updater.cancelDownload() },enabled=!interactionBlocked()) { Text("Cancel download") }
        state.downloaded?.takeIf { !pending }?.let { update ->
            val reason=model.updateInstallReason()
            if(reason.isNotBlank()) Text(reason,style=MaterialTheme.typography.bodySmall)
            Button(onClick={
                if (interactionBlocked()) return@Button
                if(!context.packageManager.canRequestPackageInstalls()) updater.installPermission { context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:${context.packageName}"))) }
                else updater.install(model,update)
            },enabled=!busy && reason.isBlank()) { Text("Install downloaded update") }
            Text("Your drafts will be saved first. Paired workspaces and app data stay in place.",style=MaterialTheme.typography.bodySmall)
        }
        if(state.status=="confirmation") {
            if(updater.updates.confirmation != null) Button(onClick={ if (!interactionBlocked()) updater.confirm(model) { intent ->
                check(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED));context.startActivity(intent)
            } },enabled=!busy && model.updateInstallReason().isBlank()) { Text("Continue Android confirmation") }
            else Text("The confirmation screen was interrupted. Cancel the saved installation, then choose Install again. It has not been repeated automatically.",style=MaterialTheme.typography.bodySmall)
        }
        if(pending) {
            TextButton(onClick={ if (!interactionBlocked()) updater.recover() },enabled=!busy) { Text("Check installation status") }
            TextButton(onClick={ if (!interactionBlocked()) updater.cancelInstall() },enabled=!busy) { Text("Cancel pending installation") }
        }
    }
}
