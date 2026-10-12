package app.zerus.mobile

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

@Composable internal fun SpeechSettingsContent(model: ZerusViewModel) {
    val speech = model.speech
    var hostsOpen by remember { mutableStateOf(false) }
    var profilesOpen by remember { mutableStateOf(false) }
    val selected = model.machines.find { MachineKey(it.connectionId, it.id) == speech.selection.host }
    LaunchedEffect(speech.ready, speech.selection.host) { if (speech.ready) speech.refreshProfiles() }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Voice input", style = MaterialTheme.typography.titleLarge)
        Text("Choose the computer and profile that process your recordings. This can be different from the computer running your conversation.", style = MaterialTheme.typography.bodySmall)
        Box {
            OutlinedButton(onClick = { hostsOpen = true }, enabled = speech.ready && !speech.busy && !model.updateInstallPreparing,
                modifier = Modifier.fillMaxWidth()) {
                Text(selected?.name ?: if (speech.selection.host == null) "Choose voice computer" else "Selected computer unavailable", maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            DropdownMenu(expanded = hostsOpen, onDismissRequest = { hostsOpen = false }) {
                model.machines.forEach { machine ->
                    val host = MachineKey(machine.connectionId, machine.id)
                    val reason = model.speechHostReason(host)
                    DropdownMenuItem(text = { Column {
                        Text(machine.name)
                        if (model.connections.size > 1) Text(model.connections.find { it.id == machine.connectionId }?.displayName.orEmpty(), style = MaterialTheme.typography.bodySmall)
                        if (reason.isNotBlank()) Text(reason, style = MaterialTheme.typography.bodySmall)
                    } }, onClick = { hostsOpen = false; speech.chooseHost(host) })
                }
                if (model.machines.isEmpty()) DropdownMenuItem(text = { Text("Connect a computer first") }, onClick = {}, enabled = false)
            }
        }
        Box {
            OutlinedButton(onClick = { profilesOpen = true }, enabled = speech.profiles.isNotEmpty() && !speech.busy && !model.updateInstallPreparing,
                modifier = Modifier.fillMaxWidth()) {
                Text(speech.profile?.name ?: speech.selection.profileId.ifBlank { "Choose voice profile" }, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            DropdownMenu(expanded = profilesOpen, onDismissRequest = { profilesOpen = false }) {
                speech.profiles.forEach { profile -> DropdownMenuItem(text = { Column {
                    Text(profile.name)
                    if (profile.model.isNotBlank()) Text(profile.model, style = MaterialTheme.typography.bodySmall)
                    if (!profile.available) Text(profile.reason.ifBlank { "Unavailable" }, style = MaterialTheme.typography.bodySmall)
                } }, onClick = { profilesOpen = false; speech.chooseProfile(profile.id) }) }
            }
        }
        Text("Recordings are sent through your gateway to the selected computer after you stop. Your gateway can read the audio. The result is an editable draft; sending remains a separate action.", style = MaterialTheme.typography.bodySmall)
        if (speech.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (speech.notice.isNotBlank()) Text(speech.notice, style = MaterialTheme.typography.bodySmall)
        if (speech.statistics.isNotBlank()) Text(speech.statistics, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { speech.refreshProfiles() }, enabled = speech.selection.host != null && !speech.loading && !speech.busy) { Text("Refresh voice profiles and statistics") }
        if (speech.busy || speech.attempt != null) SpeechComposerStatus(model)
    }
}

@Composable internal fun SpeechComposerButton(model: ZerusViewModel, target: Target, saveEditor: () -> Unit) {
    val speech = model.speech
    val context = LocalContext.current
    var settings by remember { mutableStateOf(false) }
    var microphoneDenied by rememberSaveable { mutableStateOf(false) }
    var systemSettingsError by rememberSaveable { mutableStateOf(false) }
    var permissionTarget by remember { mutableStateOf<Pair<Target, Long>?>(null) }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val captured = permissionTarget; permissionTarget = null
        if (!granted) {
            speech.permissionDenied(); settings = false; systemSettingsError = false; microphoneDenied = true
        }
        else if (captured?.first == model.selected?.target && captured?.second == model.speechNavigation) speech.start(captured.first)
    }
    fun start() {
        saveEditor()
        if (speech.startReason(target).isNotBlank()) { settings = true; return }
        if (SpeechMicrophoneAccess.granted(context)) speech.start(target)
        else { permissionTarget = target to model.speechNavigation; permission.launch(Manifest.permission.RECORD_AUDIO) }
    }
    IconButton(onClick = { if (speech.recording) speech.stop() else start() }, enabled = !model.demo && model.storageReady && !model.updateInstallPreparing) {
        Icon(if (speech.recording) Icons.Default.Stop else Icons.Default.Mic, if (speech.recording) "Stop recording and transcribe" else "Voice input",
            tint = if (speech.recording) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant)
    }
    if (microphoneDenied) {
        ObscureConversation()
        AlertDialog(onDismissRequest = { microphoneDenied = false }, title = { Text("Microphone access is off") },
            text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("In Android app settings, open Permissions, then Microphone, and allow access while using Zerus. Return here and tap Voice input to start recording.")
                if (systemSettingsError) Text("Android could not open this page. Open Android Settings, then Apps, Zerus, Permissions and Microphone.", color = MaterialTheme.colorScheme.error)
            } },
            confirmButton = { TextButton(onClick = {
                // Opening settings and returning never retains an intent to start recording.
                permissionTarget = null
                if (SpeechMicrophoneAccess.openSettings(context)) microphoneDenied = false else systemSettingsError = true
            }) { Text("Open app settings") } },
            dismissButton = { TextButton(onClick = { microphoneDenied = false }) { Text("Not now") } })
    }
    if (settings) {
        ObscureConversation()
        AlertDialog(onDismissRequest = { settings = false }, title = { Text("Voice input") },
            text = { Column(Modifier.verticalScroll(rememberScrollState())) { SpeechSettingsContent(model) } },
            confirmButton = { TextButton(onClick = { settings = false; start() }, enabled = speech.startReason(target).isBlank()) { Text("Start recording") } },
            dismissButton = { TextButton(onClick = { settings = false }) { Text("Close") } })
    }
}

@Composable internal fun SpeechComposerStatus(model: ZerusViewModel) {
    val speech = model.speech
    val attempt = speech.attempt
    val clipboard = LocalClipboardManager.current
    var discard by remember { mutableStateOf(false) }
    if (!speech.busy && attempt == null) return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        if (speech.recording) {
            Text("Recording ${speech.seconds / 60}:${(speech.seconds % 60).toString().padStart(2, '0')} (up to 2 minutes)", color = MaterialTheme.colorScheme.error)
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = { speech.stop() }) { Text("Stop and transcribe") }
                TextButton(onClick = { speech.discardRecording() }) { Text("Cancel") }
            }
        } else if (speech.recorded) {
            Text("Recording ready (${speech.seconds}s)", style = MaterialTheme.typography.bodySmall)
            Row {
                TextButton(onClick = { speech.submit() }, enabled = !speech.working) { Text("Transcribe") }
                TextButton(onClick = { speech.discardRecording() }, enabled = !speech.working) { Text("Discard") }
            }
        } else if (attempt != null) {
            Text("Voice for ${attempt.origin.target.session}", style = MaterialTheme.typography.labelSmall)
            Text(speech.notice.ifBlank { "Voice recording for ${attempt.origin.target.session}" }, style = MaterialTheme.typography.bodySmall)
            if (attempt.state == "completed" && attempt.text.isNotBlank()) {
                SelectionContainer { Text(attempt.text, maxLines = 6, overflow = TextOverflow.Ellipsis) }
                FlowRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    TextButton(onClick = { speech.insertReviewed() }, enabled = !speech.working && model.selected?.target == attempt.origin.target) { Text("Insert into draft") }
                    if (model.selected?.target != attempt.origin.target) model.sessions.find { it.target == attempt.origin.target }?.let { original ->
                        TextButton(onClick = { model.open(original) }) { Text("Open conversation") }
                    }
                    TextButton(onClick = { clipboard.setText(AnnotatedString(attempt.text)) }) { Text("Copy") }
                    TextButton(onClick = { discard = true }, enabled = !speech.working) { Text("Discard") }
                }
            } else Row {
                if (attempt.state !in setOf("completed", "cancelled", "failed")) {
                    TextButton(onClick = { speech.checkStatus() }, enabled = !speech.working) { Text("Check status") }
                    TextButton(onClick = { speech.cancel() }) { Text("Cancel") }
                }
                if (!speech.working && attempt.state in setOf("completed", "failed", "cancelled", "uncertain")) TextButton(onClick = { discard = true }) { Text("Dismiss") }
            }
        }
    }
    if (discard) AlertDialog(onDismissRequest = { discard = false }, title = { Text("Discard voice result?") },
        text = { Text("This removes the saved voice request and transcription from this phone. Your draft is preserved.") },
        confirmButton = { TextButton(onClick = { speech.dismiss(); discard = false }) { Text("Discard") } },
        dismissButton = { TextButton(onClick = { discard = false }) { Text("Keep") } })
}
