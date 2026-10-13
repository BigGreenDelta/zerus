package app.zerus.mobile

import android.Manifest
import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.ChevronLeft
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
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
    var manage by remember { mutableStateOf(false) }
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
        TextButton(onClick = { manage = true }, enabled = !model.demo && model.storageReady && !model.updateInstallPreparing) { Text("Manage speech providers") }
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
    if (manage) SpeechConfigurationScreen(model, choice) { manage = false }
}

@Composable internal fun SpeechComposerButton(model: ZerusViewModel, target: Target, saveEditor: () -> Unit) {
    val speech = model.speech
    val context = LocalContext.current
    val view = LocalView.current
    val navigation = model.speechNavigation
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val obscuration = LocalConversationObscuration.current
    val saveCurrentEditor by rememberUpdatedState(saveEditor)
    var settings by remember(target, navigation) { mutableStateOf(false) }
    var microphoneDenied by rememberSaveable { mutableStateOf(false) }
    var systemSettingsError by rememberSaveable { mutableStateOf(false) }
    var permissionTarget by remember { mutableStateOf<Pair<Target, Long>?>(null) }
    var ownedCapture by remember(target, navigation) { mutableStateOf<Long?>(null) }
    var dismissedFailure by remember(target, navigation) { mutableStateOf<Long?>(null) }
    var focused by remember(target, navigation) { mutableStateOf(false) }
    val activationKeys = remember(target, navigation) { VoiceActivationKeys() }
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        val captured = permissionTarget; permissionTarget = null
        if (!granted && captured?.first == model.selected?.target && captured?.second == model.speechNavigation) {
            speech.permissionDenied(); settings = false; systemSettingsError = false; microphoneDenied = true
        }
        // Permission UI ends the original press. Granting access never revives it;
        // the next explicit hold or accessible activation starts local capture.
    }
    fun start(gesture: Boolean): Long? {
        saveCurrentEditor()
        val current = model.draft(target)
        if (current.text.isNotEmpty() || current.attachments.isNotEmpty() || model.pickerDraft?.target == target) return null
        if (speech.startReason(target).isNotBlank()) { settings = true; return null }
        model.attachments.stopAudio()
        if (!SpeechMicrophoneAccess.granted(context)) {
            permissionTarget = target to model.speechNavigation
            permission.launch(Manifest.permission.RECORD_AUDIO)
            return null
        }
        val feedback: (VoiceCaptureFeedback.Event) -> Unit = {
            if (model.selected?.target == target && model.speechNavigation == navigation && obscuration?.obscured != true &&
                lifecycle.currentState == Lifecycle.State.RESUMED && view.isAttachedToWindow) VoiceHaptics.perform(view, it)
        }
        return (if (gesture) speech.startGesture(target, feedback) else speech.start(target, feedback)).also { ownedCapture = it }
    }
    fun stopOwned() { ownedCapture?.let { speech.stopCapture(it, userInitiated = false) } }
    DisposableEffect(target, navigation, lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) { permissionTarget = null; activationKeys.clear(); stopOwned() }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer); permissionTarget = null; activationKeys.clear(); stopOwned() }
    }
    LaunchedEffect(obscuration?.obscured) {
        if (obscuration?.obscured == true) { permissionTarget = null; stopOwned() }
    }
    val enabled = !model.demo && model.storageReady && !model.updateInstallPreparing &&
        target.agentId.isBlank() && target.archiveId.isBlank() && !speech.savingRecording
    val currentEnabled by rememberUpdatedState(enabled)
    val currentStart by rememberUpdatedState<(Boolean) -> Long?>(::start)
    fun activate(captureId: Long?): Boolean {
        if (!currentEnabled) return false
        if (captureId != null) speech.stopCapture(captureId) else currentStart(false)
        return true
    }
    val currentActivate by rememberUpdatedState(::activate)
    val cancelDistance = with(LocalDensity.current) { 80.dp.toPx() }
    val gesture = remember(target, navigation, cancelDistance) { VoiceGesture(cancelDistance) }
    val label = if (speech.recording) "Stop recording" else "Voice input"
    val actionCapture = speech.activeCaptureId
    val actionColor = if (speech.recording) MaterialTheme.colorScheme.errorContainer else if (enabled) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceVariant
    val actionContent = if (speech.recording) MaterialTheme.colorScheme.onErrorContainer else MaterialTheme.colorScheme.onPrimary
    Surface(shape = CircleShape,
        color = Color.Transparent, contentColor = actionContent,
        modifier = Modifier.size(48.dp).onFocusChanged {
            focused = it.isFocused
            if (!it.isFocused) activationKeys.clear()
        }.onKeyEvent { event ->
            val key = event.nativeKeyEvent
            when (val result = activationKeys.handle(key.keyCode, key.deviceId, key.action, key.repeatCount,
                key.isAltPressed || key.isCtrlPressed || key.isMetaPressed || key.isShiftPressed, key.isCanceled, speech.activeCaptureId)) {
                VoiceActivationKeys.Result.Pass -> false
                VoiceActivationKeys.Result.Consume -> true
                is VoiceActivationKeys.Result.Activate -> { currentActivate(result.captureId); true }
            }
        }.focusable(enabled).semantics {
            role = Role.Button; contentDescription = label
            stateDescription = if (speech.recording) "Recording. Slide left to cancel." else "Hold to record. Release to review."
            if (!enabled) disabled()
            onClick(label = if (speech.recording) "Stop recording" else "Start recording") {
                currentActivate(actionCapture)
            }
            if (actionCapture != null) customActions = listOf(CustomAccessibilityAction("Cancel recording") { speech.cancelCapture(actionCapture); true })
        }.pointerInput(target, navigation, gesture) {
            // These keys do not change when capture starts or the timer ticks.
            awaitEachGesture {
                val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                down.consume()
                if (!gesture.begin()) return@awaitEachGesture
                try {
                    if (currentEnabled) {
                        if (speech.recording) speech.stop() else gesture.captured(currentStart(true))
                    }
                    while (true) {
                        val event = awaitPointerEvent(PointerEventPass.Initial)
                        val pointer = event.changes.firstOrNull { it.id == down.id }
                        if (pointer == null || event.changes.any { it.id != down.id && it.pressed }) break
                        val decision = gesture.move(pointer.position.x - down.position.x)
                        ownedCapture?.let { speech.moveGesture(it, gesture.progress) }
                        decision?.let { speech.finishGesture(it.captureId, it.cancel) }
                        pointer.consume()
                        if (!pointer.pressed) {
                            gesture.release()?.let { speech.finishGesture(it.captureId, it.cancel) }
                            break
                        }
                    }
                } finally {
                    gesture.interrupt()?.let { speech.finishGesture(it.captureId, it.cancel, userInitiated = false) }
                }
            }
        }) {
        Box(contentAlignment = Alignment.Center) {
            ComposerActionFace(actionColor, actionContent, focused) {
                Icon(if (speech.recording) Icons.Default.Stop else Icons.Default.Mic, null,
                    Modifier.size(22.dp).graphicsLayer { translationX = -12.dp.toPx() * speech.gestureCancelProgress })
            }
        }
    }
    speech.captureFailure?.takeIf { it.first == ownedCapture && it.first != dismissedFailure }?.let { failed ->
        ObscureConversation()
        ZerusAlertDialog(onDismissRequest = { dismissedFailure = failed.first }, title = { Text("Recording not saved") },
            text = { Text(failed.second) },
            confirmButton = { TextButton(onClick = { dismissedFailure = failed.first }) { Text("Done") } })
    }
    if (microphoneDenied) {
        ObscureConversation()
        ZerusAlertDialog(onDismissRequest = { microphoneDenied = false }, title = { Text("Microphone access is off") },
            text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("In Android app settings, open Permissions, then Microphone, and allow access while using Zerus. Return here and hold Voice input to record, or activate it with accessibility controls.")
                if (systemSettingsError) Text("Android could not open this page. Open Android Settings, then Apps, Zerus, Permissions and Microphone.", color = MaterialTheme.colorScheme.error)
            } },
            confirmButton = { TextButton(onClick = {
                permissionTarget = null
                if (SpeechMicrophoneAccess.openSettings(context)) microphoneDenied = false else systemSettingsError = true
            }) { Text("Open app settings") } },
            dismissButton = { TextButton(onClick = { microphoneDenied = false }) { Text("Not now") } })
    }
    if (settings) {
        ObscureConversation()
        ZerusAlertDialog(onDismissRequest = { settings = false }, title = { Text("Voice input") },
            text = { Column(Modifier.verticalScroll(rememberScrollState())) { SpeechSettingsContent(model) } },
            confirmButton = { TextButton(onClick = { settings = false; start(false) }, enabled = speech.startReason(target).isBlank()) { Text("Start recording") } },
            dismissButton = { TextButton(onClick = { settings = false }) { Text("Close") } })
    }
}

/** Occupies the existing editor area; the microphone keeps its original pointer node. */
@Composable internal fun SpeechRecordingRow(speech: SpeechController) {
    val progress = speech.gestureCancelProgress
    val style = MaterialTheme.typography.bodyMedium
    val elapsed = voiceRecordingTime(speech.elapsedMs)
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(Modifier.size(8.dp).background(MaterialTheme.colorScheme.error, CircleShape))
        Text(elapsed, modifier = Modifier.semantics { contentDescription = "Recording $elapsed" }, maxLines = 1,
            style = style.copy(fontFeatureSettings = "tnum"), color = MaterialTheme.colorScheme.onSurface)
        Box(Modifier.weight(1f).clipToBounds(), contentAlignment = Alignment.Center) {
            if (speech.touchRecording) Row(
                Modifier.graphicsLayer { translationX = -32.dp.toPx() * progress; alpha = 1f - progress },
                verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.ChevronLeft, null, Modifier.size(16.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Text("Slide to cancel", maxLines = 1, overflow = TextOverflow.Ellipsis,
                    style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else Text("Recording", maxLines = 1, overflow = TextOverflow.Ellipsis,
                style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable internal fun SpeechComposerStatus(model: ZerusViewModel) {
    val speech = model.speech
    val draft = speech.draft
    var confirmCold by remember { mutableStateOf<VoiceDraft?>(null) }
    var confirmDiscard by remember { mutableStateOf<VoiceDraft?>(null) }
    var providerSettings by remember { mutableStateOf<VoiceDraft?>(null) }
    if (!speech.recording && !speech.savingRecording && draft == null) return
    if (speech.touchRecording) return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            speech.recording -> {
                val recordingId = speech.activeCaptureId
                val configuration = LocalConfiguration.current
                if (configuration.keyboard != Configuration.KEYBOARD_NOKEYS && configuration.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO)
                    Text("Space or Esc stops recording. Your typed draft is kept.", style = MaterialTheme.typography.bodySmall)
                Row {
                    TextButton(onClick = { recordingId?.let(speech::stopCapture) }) { Text("Stop recording") }
                    TextButton(onClick = { recordingId?.let(speech::cancelCapture) }) { Text("Cancel") }
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
        ZerusAlertDialog(onDismissRequest = { providerSettings = null }, title = { Text("Transcription provider") },
            text = { Column(Modifier.verticalScroll(rememberScrollState())) {
                Text("Recording for ${captured.target.session}")
                SpeechSettingsContent(model, captured.target, captured, showPreview = false)
            } },
            confirmButton = { TextButton(onClick = { providerSettings = null }) { Text("Done") } })
    }
    confirmCold?.let { captured ->
        ZerusAlertDialog(onDismissRequest = { confirmCold = null }, title = { Text("Send with full context?") },
            text = { Text("The prompt cache for ${captured.target.session} has expired. Sending this voice message can process the full conversation again and use more tokens. Your typed draft will stay here.") },
            confirmButton = { TextButton(onClick = {
                confirmCold = null
                if (speech.draft == captured && speech.sendReason().isBlank()) speech.submit(allowColdCache = true)
            }) { Text("Send with full context") } },
            dismissButton = { TextButton(onClick = { confirmCold = null }) { Text("Cancel") } })
    }
    confirmDiscard?.let { captured ->
        ZerusAlertDialog(onDismissRequest = { confirmDiscard = null }, title = { Text("Discard recording?") },
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
    remove?.let { entry -> ZerusAlertDialog(onDismissRequest = { remove = null }, title = { Text("Remove local voice copy?") },
        text = { Text("Remove this phone's saved audio and receipt for ${entry.target.session}? This does not cancel delivery or remove the message from the computer's conversation history.") },
        confirmButton = { TextButton(onClick = { model.attachments.stopAudio(); speech.dismiss(entry); remove = null }) { Text("Remove local copy") } },
        dismissButton = { TextButton(onClick = { remove = null }) { Text("Keep") } }) }
}
