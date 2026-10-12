package app.zerus.mobile

import android.Manifest
import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp

@Composable internal fun SpeechSettingsContent(model: ZerusViewModel, target: Target? = model.selected?.target,
    preview: VoiceDraft? = null, showPreview: Boolean = true) {
    val speech = model.speech
    var hostsOpen by remember { mutableStateOf(false) }
    var profilesOpen by remember { mutableStateOf(false) }
    val choicePreview = speech.draft?.takeIf { it.target == target && (preview == null || it.audio.id == preview.audio.id) }
    val currentPreview = preview == null || choicePreview != null
    val choice = choicePreview?.selection ?: speech.selection
    val selected = speech.computers.find { it.id == choice.host?.computerId }
    val profiles = selected?.profiles.orEmpty()
    val profile = profiles.find { it.id == choice.profileId }
    LaunchedEffect(speech.ready, target, currentPreview) { if (speech.ready && currentPreview) speech.refreshProfiles(target) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("Voice messages", style = MaterialTheme.typography.titleLarge)
        if (!currentPreview) {
            Text("This recording is no longer available.")
            return@Column
        }
        Text("Choose the computer and profile that transcribe recordings for this conversation. The original audio stays with the conversation.", style = MaterialTheme.typography.bodySmall)
        Box {
            OutlinedButton(onClick = { hostsOpen = true }, enabled = speech.ready && !speech.loading && speech.computers.isNotEmpty() && !speech.recording && !model.updateInstallPreparing,
                modifier = Modifier.fillMaxWidth()) {
                Text(selected?.name ?: if (choice.host == null) "Choose voice computer" else "Selected computer unavailable", maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            DropdownMenu(expanded = hostsOpen, onDismissRequest = { hostsOpen = false }) {
                speech.computers.forEach { machine ->
                    DropdownMenuItem(text = { Text(machine.name) }, onClick = {
                        hostsOpen = false
                        target?.let { speech.chooseHost(MachineKey(it.connectionId, machine.id), it, choicePreview?.audio?.id) }
                    })
                }
            }
        }
        Box {
            OutlinedButton(onClick = { profilesOpen = true }, enabled = !speech.loading && profiles.isNotEmpty() && !speech.recording && !model.updateInstallPreparing,
                modifier = Modifier.fillMaxWidth()) {
                Text(profile?.name ?: choice.profileId.ifBlank { "Choose voice profile" }, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            DropdownMenu(expanded = profilesOpen, onDismissRequest = { profilesOpen = false }) {
                profiles.forEach { profile -> DropdownMenuItem(text = { Column {
                    Text(profile.name)
                    if (profile.model.isNotBlank()) Text(profile.model, style = MaterialTheme.typography.bodySmall)
                    if (!profile.available) Text(profile.reason.ifBlank { "Unavailable" }, style = MaterialTheme.typography.bodySmall)
                } }, onClick = { profilesOpen = false; speech.chooseProfile(profile.id, target, choicePreview?.audio?.id) }) }
            }
        }
        Text("Tap Send after recording. Zerus keeps the audio in conversation history, transcribes it, then sends the recognized text to the agent. Your typed draft is kept separately. Your gateway can read the audio.", style = MaterialTheme.typography.bodySmall)
        if (speech.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (speech.profileNotice.isNotBlank()) Text(speech.profileNotice, style = MaterialTheme.typography.bodySmall)
        if (speech.notice.isNotBlank()) Text(speech.notice, style = MaterialTheme.typography.bodySmall)
        if (speech.statistics.isNotBlank()) Text(speech.statistics, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { speech.refreshProfiles(target) }, enabled = !speech.loading) { Text("Refresh voice profiles") }
        if (showPreview) SpeechComposerStatus(model)
        speech.legacyResult?.let { legacy ->
            Text("An earlier dictation result remains saved on this phone. It has not been sent.", style = MaterialTheme.typography.bodySmall)
            if (legacy.text.isNotBlank()) {
                val clipboard = LocalClipboardManager.current
                SelectionContainer { Text(legacy.text) }
                TextButton(onClick = { clipboard.setText(AnnotatedString(legacy.text)) }) { Text("Copy earlier transcription") }
            }
        }
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
        model.attachments.stopAudio()
        if (SpeechMicrophoneAccess.granted(context)) speech.start(target)
        else { permissionTarget = target to model.speechNavigation; permission.launch(Manifest.permission.RECORD_AUDIO) }
    }
    IconButton(onClick = { if (speech.recording) speech.stop() else start() }, enabled = !model.demo && model.storageReady && !model.updateInstallPreparing && target.agentId.isBlank() && target.archiveId.isBlank()) {
        Icon(if (speech.recording) Icons.Default.Stop else Icons.Default.Mic, if (speech.recording) "Stop recording" else "Voice input",
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
    val draft = speech.draft
    var confirmCold by remember { mutableStateOf<VoiceDraft?>(null) }
    var confirmDiscard by remember { mutableStateOf<VoiceDraft?>(null) }
    var providerSettings by remember { mutableStateOf<VoiceDraft?>(null) }
    if (!speech.recording && !speech.savingRecording && draft == null) return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            speech.recording -> {
                Text("Recording ${speech.seconds / 60}:${(speech.seconds % 60).toString().padStart(2, '0')} (up to 2 minutes)", color = MaterialTheme.colorScheme.error)
                val configuration = LocalConfiguration.current
                if (configuration.keyboard != Configuration.KEYBOARD_NOKEYS && configuration.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO)
                    Text("Space or Esc stops recording. Your typed draft is kept.", style = MaterialTheme.typography.bodySmall)
                Row {
                    TextButton(onClick = { speech.stop() }) { Text("Stop recording") }
                    TextButton(onClick = { speech.discardRecording() }) { Text("Cancel") }
                }
            }
            speech.savingRecording -> { Text("Saving recording…"); LinearProgressIndicator(Modifier.fillMaxWidth()) }
            draft != null -> {
                Text("Voice message for ${draft.target.session}", style = MaterialTheme.typography.labelMedium)
                val computer = speech.computers.find { it.id == draft.machineId }
                Text(if (draft.machineId.isBlank() || draft.profileId.isBlank()) "Choose a transcription provider before sending."
                    else "Transcription: ${computer?.name ?: draft.machineId} / ${computer?.profiles?.find { it.id == draft.profileId }?.name ?: draft.profileId}", style = MaterialTheme.typography.bodySmall)
                VoiceAudio(model, draft.target, draft.audio.displayed(), draft.durationMs)
                val reason = speech.sendReason()
                Row {
                    TextButton(onClick = {
                        model.attachments.stopAudio()
                        if (model.voiceColdCache()) confirmCold = draft else speech.submit()
                    }, enabled = reason.isBlank()) { Text("Send voice message") }
                    TextButton(onClick = { confirmDiscard = draft }) { Text("Discard") }
                    IconButton(onClick = { providerSettings = draft }, enabled = !speech.savingRecording) {
                        Icon(Icons.Default.Settings, "Transcription provider")
                    }
                }
                if (reason.isNotBlank()) Text(reason, style = MaterialTheme.typography.bodySmall)
                if (model.selected?.target != draft.target) model.sessions.find { it.target == draft.target }?.let { session ->
                    TextButton(onClick = { model.open(session) }) { Text("Open original conversation") }
                }
            }
        }
        if (speech.notice.isNotBlank()) Text(speech.notice, style = MaterialTheme.typography.bodySmall)
    }
    providerSettings?.let { captured ->
        ObscureConversation()
        AlertDialog(onDismissRequest = { providerSettings = null }, title = { Text("Transcription provider") },
            text = { Column(Modifier.verticalScroll(rememberScrollState())) {
                Text("Recording for ${captured.target.session}")
                SpeechSettingsContent(model, captured.target, captured, showPreview = false)
            } },
            confirmButton = { TextButton(onClick = { providerSettings = null }) { Text("Done") } })
    }
    confirmCold?.let { captured ->
        AlertDialog(onDismissRequest = { confirmCold = null }, title = { Text("Send with full context?") },
            text = { Text("The prompt cache for ${captured.target.session} has expired. Sending this voice message can process the full conversation again and use more tokens. Your typed draft will stay here.") },
            confirmButton = { TextButton(onClick = {
                confirmCold = null
                if (speech.draft == captured && speech.sendReason().isBlank()) speech.submit(allowColdCache = true)
            }) { Text("Send with full context") } },
            dismissButton = { TextButton(onClick = { confirmCold = null }) { Text("Cancel") } })
    }
    confirmDiscard?.let { captured ->
        AlertDialog(onDismissRequest = { confirmDiscard = null }, title = { Text("Discard recording?") },
            text = { Text("Discard the saved voice recording for ${captured.target.session}? This removes only this unsent recording. Your typed draft and sent messages are kept.") },
            confirmButton = { TextButton(onClick = {
                confirmDiscard = null
                if (speech.draft == captured && !speech.savingRecording) { model.attachments.stopAudio(); speech.discardRecording() }
            }) { Text("Discard recording") } },
            dismissButton = { TextButton(onClick = { confirmDiscard = null }) { Text("Keep recording") } })
    }
}

@Composable internal fun VoiceAudio(model: ZerusViewModel, target: Target?, audio: DisplayedAttachment, durationMs: Int,
    waveform: List<Int> = emptyList()) {
    val viewer = model.attachments
    val key = AttachmentIdentity.playbackKey(target, audio)
    val row = viewer.state(target, audio)
    val active = viewer.playingKey == key
    VoicePlayer(key, waveform, durationMs, if (active) viewer.audioPositionMs else 0,
        active && viewer.playing, row?.loading == true,
        onToggle = { viewer.toggleAudio(target, audio) },
        onSeek = { viewer.seekAudio(target, audio, it, durationMs) })
    row?.error?.takeIf { it.isNotBlank() }?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
}

@Composable internal fun VoiceMessageContent(model: ZerusViewModel, target: Target?, voice: VoiceMessage) {
    var expanded by rememberSaveable(voice.id) { mutableStateOf(false) }
    val local = model.speech.outgoing.find { it.id == voice.id && target?.let(it::belongsTo) == true }
    Text("Voice message", style = MaterialTheme.typography.labelMedium)
    VoiceAudio(model, target, voice.audio, voice.durationMs, voice.waveform)
    Text(VoiceProtocol.label(voice.state), style = MaterialTheme.typography.bodySmall)
    val error = local?.error?.ifBlank { voice.error } ?: voice.error
    if (error.isNotBlank()) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
    if (voice.transcript.isNotBlank()) {
        TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "Hide recognized text" else "Show recognized text") }
        if (expanded) SelectionContainer { Text(voice.transcript) }
    }
    if (local != null && voice.state !in setOf("submitted", "failed", "cancelled")) Row {
        TextButton(onClick = { model.speech.checkStatus(local) }, enabled = local.id !in model.speech.activeRequests) { Text("Check status") }
        TextButton(onClick = { model.speech.cancel(local) }, enabled = voice.state != "uploading") { Text("Cancel delivery") }
    }
    if (local?.canRestoreUnsent == true) TextButton(onClick = { model.speech.restoreUnsent(local) },
        enabled = model.speech.draft == null && !model.speech.savingRecording) { Text("Return to recording") }
}

@Composable internal fun VoiceSavedMessages(model: ZerusViewModel) {
    val speech = model.speech
    var remove by remember { mutableStateOf<VoiceOutgoing?>(null) }
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SpeechComposerStatus(model)
        speech.outgoing.sortedByDescending { it.createdAt }.forEach { entry ->
            Card { Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(entry.target.session, style = MaterialTheme.typography.titleSmall)
                VoiceMessageContent(model, entry.effectiveTarget, entry.displayed())
                Row {
                    model.sessions.find { entry.belongsTo(it.target) }?.let { session ->
                        TextButton(onClick = { model.open(session) }) { Text("Open conversation") }
                    }
                    if (entry.terminal) TextButton(onClick = { remove = entry }, enabled = entry.id !in speech.activeRequests) { Text("Remove local copy") }
                }
            } }
        }
    }
    remove?.let { entry -> AlertDialog(onDismissRequest = { remove = null }, title = { Text("Remove local voice copy?") },
        text = { Text("Remove this phone's saved audio and receipt for ${entry.target.session}? This does not cancel delivery or remove the message from the computer's conversation history.") },
        confirmButton = { TextButton(onClick = { model.attachments.stopAudio(); speech.dismiss(entry); remove = null }) { Text("Remove local copy") } },
        dismissButton = { TextButton(onClick = { remove = null }) { Text("Keep") } }) }
}
