package app.zerus.mobile

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Send
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp

/** Matches RoundVideoRecorder's actual FileOutputOptions limit, rather than the wire's 60s maximum. */
internal object VideoRecordingClock {
    const val LIMIT_MS = 59_000
    fun remainingSeconds(elapsedMs: Int) = (LIMIT_MS - elapsedMs.coerceIn(0, LIMIT_MS) + 999) / 1000
    fun progress(elapsedMs: Int) = elapsedMs.coerceIn(0, LIMIT_MS).toFloat() / LIMIT_MS
}

private data class VideoCaptureView(val id: Long, val recorder: RoundVideoRecorder,
    val held: Boolean, val locked: Boolean, val previewBeforeSend: Boolean)

/** Same-window layer: never replaces the composer or steals the held gesture's pointer/focus. */
@Composable internal fun RoundVideoRecordingOverlay(speech: SpeechController, modifier: Modifier = Modifier,
    onVisibilityChange: (Boolean) -> Unit = {}) {
    val current = speech.videoRecorder?.let { recorder -> speech.activeCaptureId?.let {
        VideoCaptureView(it, recorder, speech.touchRecording, speech.recordingLocked, speech.captureNeedsPreview)
    } }
    var retained by remember { mutableStateOf<VideoCaptureView?>(null) }
    var closingId by remember { mutableStateOf<Long?>(null) }
    if (current != null) SideEffect { retained = current }
    val shown = current ?: retained
    val visible = current != null && closingId != current.id &&
        (current.locked || speech.gestureCaptureId == current.id)
    val transition = remember { MutableTransitionState(false) }
    transition.targetState = visible
    // Exposure outlives recording=false until every exit animation has finished.
    val exposure = transition.currentState || transition.targetState || !transition.isIdle
    SideEffect { onVisibilityChange(exposure) }
    val visibilityChanged by rememberUpdatedState(onVisibilityChange)
    DisposableEffect(Unit) { onDispose { visibilityChanged(false) } }
    if (current == null && transition.isIdle && !transition.currentState) SideEffect { retained = null }
    if (shown == null) return
    AnimatedVisibility(visibleState = transition, modifier = modifier,
        enter = fadeIn(tween(180)), exit = fadeOut(tween(180))) {
        val id = shown.id
        val active = current?.id == id && visible
        // Consume new touches above the chat; the existing held pointer keeps its original path.
        BoxWithConstraints(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.surface.copy(alpha = .97f))
            .pointerInput(id) { awaitPointerEventScope {
                while (true) awaitPointerEvent().changes.forEach { it.consume() }
            } }.semantics { paneTitle = "Video recording" }, contentAlignment = Alignment.Center) {
            val density = LocalDensity.current
            val measurer = rememberTextMeasurer()
            val hint = if (shown.previewBeforeSend) "Release to preview" else "Release to send"
            val textWidth = with(density) { (maxWidth - 32.dp).roundToPx().coerceAtLeast(1) }
            val timerStyle = MaterialTheme.typography.titleMedium.copy(fontFeatureSettings = "tnum")
            val timerHeight = with(density) { measurer.measure("0:59 left", timerStyle).size.height.toDp() }
            val bottomHeight = if (shown.held) with(density) {
                measurer.measure(hint, MaterialTheme.typography.bodyMedium,
                    constraints = Constraints(maxWidth = textWidth)).size.height.toDp()
            } else 48.dp
            val diameter = minOf(320.dp, maxWidth - 48.dp,
                maxHeight - 80.dp - timerHeight - bottomHeight).coerceAtLeast(72.dp)
            Column(Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp),
                horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
                val remaining = VideoRecordingClock.remainingSeconds(speech.elapsedMs)
                val timerColor = if (remaining <= 10) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (shown.locked) Icon(Icons.Default.Lock, "Recording locked", Modifier.size(18.dp))
                    Text("${remaining / 60}:${(remaining % 60).toString().padStart(2, '0')} left",
                        Modifier.semantics { contentDescription = "$remaining seconds remaining" },
                        style = timerStyle, color = timerColor, maxLines = 1)
                }
                key(id) {
                    // Exit animation retains only this preview; Finalize/cleanup never waits for it.
                    Box(Modifier.size(diameter + 16.dp).animateEnterExit(
                        enter = scaleIn(tween(180), initialScale = .75f) + slideInVertically(tween(180)) { it / 4 },
                        exit = scaleOut(tween(180), targetScale = .75f) + slideOutVertically(tween(180)) { it / 4 })
                        .semantics { contentDescription = "Live video preview" }, contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(progress = { VideoRecordingClock.progress(speech.elapsedMs) },
                            modifier = Modifier.fillMaxSize(), color = timerColor, trackColor = timerColor.copy(alpha = .16f),
                            strokeWidth = 3.dp)
                        LiveVideoCircle(shown.recorder, diameter, enabled = active)
                    }
                }
                if (shown.held) Text(hint, style = MaterialTheme.typography.bodyMedium, textAlign = TextAlign.Center)
                else Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    FilledTonalIconButton(onClick = {
                        if (speech.activeCaptureId == id) { closingId = id; speech.cancelCapture(id) }
                    }, enabled = active, modifier = Modifier.size(48.dp)) { Icon(Icons.Default.Close, "Cancel") }
                    FilledIconButton(onClick = {
                        if (speech.activeCaptureId == id) { closingId = id; speech.stopCapture(id) }
                    }, enabled = active, modifier = Modifier.size(48.dp)) { Icon(Icons.Default.Stop, "Stop recording") }
                    if (!shown.previewBeforeSend) FilledIconButton(onClick = {
                        if (speech.activeCaptureId == id) { closingId = id; speech.sendCapture(id) }
                    }, enabled = active, modifier = Modifier.size(48.dp)) { Icon(Icons.AutoMirrored.Filled.Send, "Send") }
                }
            }
        }
    }
}
