package app.zerus.mobile

import android.Manifest
import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.DeleteOutline
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.HelpOutline
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Subject
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.CloudUpload
import androidx.compose.material.icons.automirrored.filled.Send
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
import androidx.compose.ui.semantics.clearAndSetSemantics
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
import androidx.compose.ui.text.rememberTextMeasurer
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
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Preview before sending")
                Text("Review audio and video recordings, then tap Send. Applies on this device.", style = MaterialTheme.typography.bodySmall)
            }
            Switch(checked = speech.previewBeforeSend, onCheckedChange = speech::setPreviewBeforeSend,
                enabled = speech.ready && !speech.recording && !speech.savingRecording && !model.updateInstallPreparing,
                modifier = Modifier.semantics { contentDescription = "Preview before sending" })
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
        Text((if (speech.previewBeforeSend) "Hold to record and release to preview; tap Send when ready. " else "Hold to record and release to send. ") +
            "Slide up to lock, or left to cancel. Stop always keeps a preview. Zerus keeps the media in conversation history, transcribes it, then sends the recognized text to the agent. Your typed draft is kept separately. Your gateway can read the recordings.", style = MaterialTheme.typography.bodySmall)
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
    val lifecycleOwner = LocalLifecycleOwner.current
    val lifecycle = lifecycleOwner.lifecycle
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
    val permission = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants ->
        val granted = grants.values.all { it }
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
        val required = if (speech.videoMode) arrayOf(Manifest.permission.RECORD_AUDIO, Manifest.permission.CAMERA) else arrayOf(Manifest.permission.RECORD_AUDIO)
        if (required.any { androidx.core.content.ContextCompat.checkSelfPermission(context, it) != android.content.pm.PackageManager.PERMISSION_GRANTED }) {
            permissionTarget = target to model.speechNavigation
            permission.launch(required)
            return null
        }
        val feedback: (VoiceCaptureFeedback.Event) -> Unit = {
            if (model.selected?.target == target && model.speechNavigation == navigation && obscuration?.obscured != true &&
                lifecycle.currentState == Lifecycle.State.RESUMED && view.isAttachedToWindow) VoiceHaptics.perform(view, it)
        }
        return (if (speech.videoMode) speech.startVideo(target, gesture, RoundVideoRecorder(context, lifecycleOwner), feedback)
            else if (gesture) speech.startGesture(target, feedback) else speech.start(target, feedback)).also { ownedCapture = it }
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
        if (captureId != null) speech.sendCapture(captureId) else currentStart(false)
        return true
    }
    val currentActivate by rememberUpdatedState(::activate)
    val cancelDistance = with(LocalDensity.current) { 80.dp.toPx() }
    val lockDistance = with(LocalDensity.current) { 57.dp.toPx() }
    val gesture = remember(target, navigation, cancelDistance, lockDistance) { VoiceGesture(cancelDistance, lockDistance) }
    val finishLabel = if (speech.captureNeedsPreview) "Stop and preview" else "Send recording"
    val label = if (speech.recordingLocked) finishLabel else if (speech.recording) "Recording" else if (speech.videoMode) "Video input" else "Voice input"
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
            stateDescription = if (speech.recordingLocked) "Recording locked. $finishLabel." else if (speech.recording) "Slide left to cancel, up to lock."
                else if (speech.previewBeforeSend) "Tap to switch microphone and camera. Hold to record, release to preview."
                else "Tap to switch microphone and camera. Hold to record, release to send."
            if (!enabled) disabled()
            onClick(label = if (speech.recording) finishLabel else "Start locked recording") {
                currentActivate(actionCapture)
            }
            customActions = if (actionCapture != null) listOf(
                CustomAccessibilityAction("Stop and preview") { speech.stopCapture(actionCapture); true },
                CustomAccessibilityAction("Cancel recording") { speech.cancelCapture(actionCapture); true })
            else listOf(CustomAccessibilityAction("Switch microphone and camera") { speech.toggleMode(target); true })
        }.pointerInput(target, navigation, gesture) {
            fun apply(action: VoiceGesture.Action?, interrupted: Boolean = false) {
                when (action) {
                    VoiceGesture.Action.Start -> gesture.captured(currentStart(true))
                    VoiceGesture.Action.SwitchMode -> speech.toggleMode(target)
                    is VoiceGesture.Action.Lock -> speech.lockCapture(action.captureId)
                    is VoiceGesture.Action.Finish -> when (action.end) {
                        VoiceGesture.End.Send -> speech.sendCapture(action.captureId)
                        VoiceGesture.End.Cancel -> speech.finishGesture(action.captureId, true)
                        VoiceGesture.End.Preview -> speech.finishGesture(action.captureId, false, userInitiated = !interrupted)
                    }
                    null -> Unit
                }
            }
            // Keep this pointer owner mounted when capture starts, locks or the timer ticks.
            awaitEachGesture {
                val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                down.consume()
                if (!currentEnabled) return@awaitEachGesture
                val existing = speech.activeCaptureId
                if (existing != null) {
                    // Locked recording sends on an intentional click; losing the pointer never sends.
                    while (true) {
                        val event = awaitPointerEvent(PointerEventPass.Initial)
                        val pointer = event.changes.firstOrNull { it.id == down.id } ?: break
                        if (event.changes.any { it.id != down.id && it.pressed }) break
                        // Compose cancellation synthesizes an already-consumed up; it cannot Send.
                        if (pointer.isConsumed) break
                        pointer.consume()
                        if (!pointer.pressed) {
                            if (speech.recordingLocked && pointer.position.x in 0f..size.width.toFloat() &&
                                pointer.position.y in 0f..size.height.toFloat()) speech.sendCapture(existing)
                            break
                        }
                    }
                    return@awaitEachGesture
                }
                if (!gesture.begin()) return@awaitEachGesture
                var waiting = true
                val deadline = android.os.SystemClock.uptimeMillis() + 150L
                try {
                    while (true) {
                        val event = if (waiting) withTimeoutOrNull((deadline - android.os.SystemClock.uptimeMillis()).coerceAtLeast(1)) {
                            awaitPointerEvent(PointerEventPass.Initial)
                        } else awaitPointerEvent(PointerEventPass.Initial)
                        if (event == null) { waiting = false; apply(gesture.hold()); continue }
                        val pointer = event.changes.firstOrNull { it.id == down.id }
                        if (pointer == null || event.changes.any { it.id != down.id && it.pressed }) break
                        // Test incoming consumption before our own consume/move/release handling.
                        if (pointer.isConsumed) break
                        apply(gesture.move(pointer.position.x - down.position.x, pointer.position.y - down.position.y))
                        ownedCapture?.let { speech.moveGesture(it, gesture.progress, gesture.lockProgress) }
                        pointer.consume()
                        if (!pointer.pressed) { apply(gesture.release()); break }
                    }
                } finally { apply(gesture.interrupt(), interrupted = true) }
            }
        }) {
        Box(contentAlignment = Alignment.Center) {
            ComposerActionFace(actionColor, actionContent, focused) {
                Icon(if (speech.recordingLocked) (if (speech.captureNeedsPreview) Icons.Default.Stop else Icons.AutoMirrored.Filled.Send) else if (speech.videoMode) Icons.Default.Videocam else Icons.Default.Mic, null,
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
        ZerusAlertDialog(onDismissRequest = { microphoneDenied = false }, title = { Text("Recording access is off") },
            text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("In Android app settings, open Permissions and allow Microphone and Camera access while using Zerus. Return here and hold Voice input to record, or activate it with accessibility controls.")
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
    val textMeasurer = rememberTextMeasurer()
    val fullCueWidth = textMeasurer.measure("Slide to cancel", style, softWrap = false, maxLines = 1).size.width +
        with(LocalDensity.current) { 16.dp.roundToPx() }
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(Modifier.size(8.dp).background(MaterialTheme.colorScheme.error, CircleShape))
        Text(elapsed, modifier = Modifier.semantics { contentDescription = "Recording $elapsed" }, maxLines = 1,
            style = style.copy(fontFeatureSettings = "tnum"), color = MaterialTheme.colorScheme.onSurface)
        if (speech.touchRecording) Icon(Icons.Default.Lock, "Slide up to lock", Modifier.size(18.dp).graphicsLayer {
            translationY = -12.dp.toPx() * speech.gestureLockProgress
        }, tint = MaterialTheme.colorScheme.onSurfaceVariant)
        BoxWithConstraints(Modifier.weight(1f).clipToBounds(), contentAlignment = Alignment.Center) {
            val cueText = if (constraints.maxWidth >= fullCueWidth) "Slide to cancel" else "Cancel"
            if (speech.touchRecording) Row(
                Modifier.graphicsLayer { translationX = -32.dp.toPx() * progress; alpha = 1f - progress },
                verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Default.ChevronLeft, null, Modifier.size(16.dp), tint = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(cueText,
                    modifier = Modifier.clearAndSetSemantics { contentDescription = "Slide to cancel" },
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                    style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else Text("Recording", maxLines = 1, overflow = TextOverflow.Ellipsis,
                style = style, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun SpeechComposerStatus(model: ZerusViewModel) {
    val speech = model.speech
    val draft = speech.draft
    val sendReason = if (draft != null) speech.sendReason() else ""
    var confirmCold by remember { mutableStateOf<VoiceDraft?>(null) }
    var confirmDiscard by remember { mutableStateOf<VoiceDraft?>(null) }
    var providerSettings by remember { mutableStateOf<VoiceDraft?>(null) }
    if (!speech.recording && !speech.savingRecording && draft == null) return
    if (speech.touchRecording && speech.videoRecorder == null) return
    // The live camera has a dedicated layer above history, beside the unchanged pointer owner.
    if (speech.recording && speech.videoRecorder != null) return
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        when {
            speech.recording -> {
                val recordingId = speech.activeCaptureId
                val configuration = LocalConfiguration.current
                if (configuration.keyboard != Configuration.KEYBOARD_NOKEYS && configuration.hardKeyboardHidden == Configuration.HARDKEYBOARDHIDDEN_NO)
                    Text("Space or Esc stops recording. Your typed draft is kept.", style = MaterialTheme.typography.bodySmall)
                if (speech.touchRecording) Text("Slide up to lock, left to cancel", style = MaterialTheme.typography.bodySmall)
                else FlowRow(Modifier.fillMaxWidth()) {
                    TextButton(onClick = { recordingId?.let(speech::stopCapture) }) { Text("Stop recording") }
                    TextButton(onClick = { recordingId?.let(speech::cancelCapture) }) { Text("Cancel") }
                    if (!speech.captureNeedsPreview) TextButton(onClick = { recordingId?.let(speech::sendCapture) }) { Text("Send") }
                }
            }
            speech.savingRecording -> { Text("Saving recording…"); LinearProgressIndicator(Modifier.fillMaxWidth()) }
            draft != null -> {
                val providerReason = speech.providerReason(draft)
                Surface(Modifier.fillMaxWidth(), shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceContainer,
                    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline)) {
                    Column(Modifier.padding(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        if (model.selected?.target != draft.target) Text("For ${draft.target.session}", style = MaterialTheme.typography.labelMedium)
                        val actions: @Composable () -> Unit = {
                            Row {
                                ComposerActionButton(onClick = { confirmDiscard = draft }, enabled = true,
                                    containerColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .12f), contentColor = MaterialTheme.colorScheme.onSurface,
                                    disabledContainerColor = MaterialTheme.colorScheme.surface, disabledContentColor = MaterialTheme.colorScheme.onSurfaceVariant) {
                                    Icon(Icons.Default.DeleteOutline, "Discard", Modifier.size(22.dp))
                                }
                                ComposerActionButton(onClick = {
                                    if (speech.draft == draft && speech.sendReason().isBlank()) {
                                        model.attachments.stopAudio()
                                        if (model.voiceColdCache()) confirmCold = draft else speech.submit()
                                    }
                                }, enabled = sendReason.isBlank(),
                                    containerColor = MaterialTheme.colorScheme.primary, contentColor = MaterialTheme.colorScheme.onPrimary,
                                    disabledContainerColor = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .12f), disabledContentColor = MaterialTheme.colorScheme.onSurfaceVariant) {
                                    Icon(Icons.AutoMirrored.Filled.Send, if (draft.video == null) "Send voice message" else "Send video message", Modifier.size(22.dp))
                                }
                            }
                        }
                        if (draft.video != null) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            VideoCircle(model, draft.target, draft.video.displayed(draft.audio.id), diameter = 160.dp)
                            Column { Text("Pending", style = MaterialTheme.typography.labelSmall); actions() }
                        } else VoiceAudio(model, draft.target, draft.audio.displayed(), draft.durationMs, status = "Pending", trailing = actions)
                        if (speech.loading) Text("Checking transcription provider…", style = MaterialTheme.typography.bodySmall)
                        else if (providerReason.isNotBlank()) {
                            Text(providerReason, style = MaterialTheme.typography.bodySmall)
                            TextButton(onClick = { providerSettings = draft },
                                modifier = Modifier.semantics { contentDescription = "Transcription provider" }) {
                                Text(if (draft.machineId.isBlank() || draft.profileId.isBlank()) "Choose provider" else "Review provider")
                            }
                        }
                        if (sendReason.isNotBlank() && sendReason != providerReason) Text(sendReason, style = MaterialTheme.typography.bodySmall)
                        if (model.selected?.target != draft.target) model.sessions.find { it.target == draft.target }?.let { session ->
                            TextButton(onClick = { model.open(session) }) { Text("Open original conversation") }
                        }
                    }
                }
            }
        }
        if (speech.notice.isNotBlank() && speech.notice != sendReason) Text(speech.notice, style = MaterialTheme.typography.bodySmall)
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
    waveform: List<Int> = emptyList(), status: String? = null, trailing: (@Composable () -> Unit)? = null,
    metadata: (@Composable () -> Unit)? = null, leading: (@Composable () -> Unit)? = null,
    endControl: (@Composable () -> Unit)? = null) {
    val viewer = model.attachments
    val key = AttachmentIdentity.playbackKey(target, audio)
    val row = viewer.state(target, audio)
    val active = viewer.playingKey == key
    VoicePlayer(key, waveform, durationMs, if (active) viewer.audioPositionMs else 0,
        active && viewer.playing, row?.loading == true,
        onToggle = { viewer.toggleAudio(target, audio) },
        onSeek = { viewer.seekAudio(target, audio, it, durationMs) }, status = status, trailing = trailing,
        metadata = metadata, leading = leading, endControl = endControl)
    row?.error?.takeIf { it.isNotBlank() }?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun VoiceMessageContent(model: ZerusViewModel, target: Target?, voice: VoiceMessage,
    at: Double = voice.createdAt, videoBubbleColor: Color? = null) {
    var expanded by rememberSaveable(voice.id) { mutableStateOf(false) }
    var actionsOpen by remember(voice.id) { mutableStateOf(false) }
    val local = model.speech.outgoing.find { it.id == voice.id && target?.let(it::belongsTo) == true }
    val delivery = RecordingDelivery.of(voice.state)
    val pending = local != null && voice.state !in setOf("submitted", "failed", "cancelled")
    val restorable = local?.canRestoreUnsent == true
    val transcriptControl: @Composable () -> Unit = {
        IconToggleButton(checked = expanded, onCheckedChange = {
            // A video changes between its circular player and its derived-audio player.
            // Retire only this recording's decoder; another message keeps its playback.
            if (voice.video != null && model.attachments.playingKey in listOf(
                    AttachmentIdentity.playbackKey(target, voice.audio),
                    AttachmentIdentity.playbackKey(target, voice.video.file))) model.attachments.stopAudio()
            expanded = it
        }) {
            Box(Modifier.size(36.dp).background(
                if (voice.video != null && !expanded) MaterialTheme.colorScheme.surface else Color.Transparent,
                CircleShape), contentAlignment = Alignment.Center) {
                Icon(if (expanded) Icons.Default.KeyboardArrowUp else Icons.Default.Subject,
                    if (expanded) "Hide recognized text" else "Show recognized text", Modifier.size(20.dp),
                    tint = MaterialTheme.colorScheme.primary)
            }
        }
    }
    val metadata: @Composable () -> Unit = {
        Row(Modifier.background(if (voice.video != null && !expanded) MaterialTheme.colorScheme.surface.copy(alpha = .92f)
            else Color.Transparent, RoundedCornerShape(12.dp)), verticalAlignment = Alignment.CenterVertically) {
            if (voice.video == null && voice.transcript.isNotBlank()) transcriptControl()
            if (pending || restorable) Box {
                IconButton(onClick = { actionsOpen = true }) { Icon(Icons.Default.MoreVert, "Recording actions", Modifier.size(20.dp)) }
                DropdownMenu(expanded = actionsOpen, onDismissRequest = { actionsOpen = false }) {
                    if (pending && local != null) {
                        DropdownMenuItem(text = { Text("Check status") }, enabled = local.id !in model.speech.activeRequests,
                            onClick = { actionsOpen = false; model.speech.checkStatus(local) })
                        DropdownMenuItem(text = { Text("Cancel delivery") }, enabled = voice.state != "uploading",
                            onClick = { actionsOpen = false; model.speech.cancel(local) })
                    }
                    if (restorable && local != null) DropdownMenuItem(text = { Text("Return to recording") },
                        enabled = model.speech.draft == null && !model.speech.savingRecording,
                        onClick = { actionsOpen = false; model.speech.restoreUnsent(local) })
                }
            }
            if (at > 0) Text(remember(at) { java.text.SimpleDateFormat("HH:mm", java.util.Locale.getDefault())
                .format(java.util.Date((at * 1000).toLong())) }, style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            TooltipBox(positionProvider = TooltipDefaults.rememberPlainTooltipPositionProvider(),
                tooltip = { PlainTooltip { Text(VoiceProtocol.label(voice.state)) } }, state = rememberTooltipState()) {
                val color = if (delivery in setOf(RecordingDelivery.Failed, RecordingDelivery.Uncertain)) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant
                Box(Modifier.heightIn(min = 24.dp).padding(horizontal = 6.dp), contentAlignment = Alignment.Center) {
                    if (delivery == RecordingDelivery.Processing) CircularProgressIndicator(Modifier.size(16.dp)
                        .semantics { contentDescription = VoiceProtocol.label(voice.state) }, color = color, strokeWidth = 1.5.dp)
                    else Icon(when (delivery) {
                        RecordingDelivery.Sent -> Icons.Default.Check
                        RecordingDelivery.Failed -> Icons.Default.ErrorOutline
                        RecordingDelivery.Cancelled -> Icons.Default.Close
                        RecordingDelivery.Uploading -> Icons.Default.CloudUpload
                        else -> Icons.Default.HelpOutline
                    }, delivery.description, Modifier.size(18.dp), tint = color)
                }
            }
        }
    }
    val error = local?.error?.ifBlank { voice.error } ?: voice.error
    val video = voice.video
    if (video == null) {
        VoiceAudio(model, target, voice.audio, voice.durationMs, voice.waveform, metadata = metadata)
        if (error.isNotBlank()) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        if (expanded && voice.transcript.isNotBlank()) SelectionContainer { Text(voice.transcript) }
    } else if (expanded && voice.transcript.isNotBlank()) {
        Surface(color = videoBubbleColor ?: MaterialTheme.colorScheme.surfaceVariant, shape = RoundedCornerShape(17.dp)) {
            Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                VoiceAudio(model, target, voice.audio, voice.durationMs, voice.waveform,
                    leading = { VideoAudioThumbnail(model, target, voice) }, endControl = transcriptControl, metadata = {})
                SelectionContainer { Text(voice.transcript) }
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) { metadata() }
                if (error.isNotBlank()) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
        }
    } else Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.CenterEnd) {
        Column(Modifier.widthIn(max = 288.dp).fillMaxWidth()) {
            BoxWithConstraints(Modifier.fillMaxWidth()) {
                val diameter = (maxWidth - 48.dp).coerceIn(48.dp, 240.dp)
                Box(Modifier.fillMaxWidth().height(diameter + 8.dp)) {
                    Box(Modifier.align(Alignment.TopEnd)) { VideoCircle(model, target, video, diameter, showFooter = false) }
                    Row(Modifier.align(Alignment.BottomCenter).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            if (voice.transcript.isNotBlank()) transcriptControl()
                            val viewer = model.attachments
                            val active = viewer.playingKey == AttachmentIdentity.playbackKey(target, video.file)
                            Text(VoiceWaveform.time(if (active) viewer.audioPositionMs else video.durationMs),
                                Modifier.background(MaterialTheme.colorScheme.surface.copy(alpha = .9f), RoundedCornerShape(12.dp))
                                    .padding(horizontal = 6.dp, vertical = 2.dp), style = MaterialTheme.typography.labelSmall)
                        }
                        metadata()
                    }
                }
            }
            model.attachments.state(target, video.file)?.error?.takeIf(String::isNotBlank)?.let {
                Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
            if (error.isNotBlank()) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
        }
    }
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
