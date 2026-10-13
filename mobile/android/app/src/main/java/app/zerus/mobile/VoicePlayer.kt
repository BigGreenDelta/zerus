package app.zerus.mobile

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.dp
import org.json.JSONArray
import kotlin.math.roundToInt

internal object VoiceWaveform {
    const val BINS = 64
    fun valid(bins: List<Int>) = bins.size == BINS && bins.all { it in 0..255 }
    /** Optional display metadata must never prevent the original recording from opening. */
    fun decode(raw: Any?): List<Int> {
        val array = raw as? JSONArray ?: return emptyList()
        if (array.length() != BINS) return emptyList()
        return (0 until BINS).map { index ->
            val value = array.opt(index)
            if (value !is Int && value !is Long) return emptyList()
            val number = (value as Number).toLong()
            if (number !in 0..255) return emptyList()
            number.toInt()
        }
    }
    fun seekPosition(fraction: Float, durationMs: Int): Int? =
        if (!fraction.isFinite() || durationMs !in 1..120_000) null
        else (fraction.coerceIn(0f, 1f) * durationMs).roundToInt()
    fun progress(positionMs: Int, durationMs: Int): Float =
        if (durationMs <= 0) 0f else (positionMs.toFloat() / durationMs).coerceIn(0f, 1f)
    fun time(milliseconds: Int): String = (milliseconds.coerceAtLeast(0) / 1000).let {
        "${it / 60}:${(it % 60).toString().padStart(2, '0')}"
    }
}

/** The slider supplies native keyboard/TalkBack seeking; its track draws only measured bins. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun VoicePlayer(identity: String, waveform: List<Int>, durationMs: Int,
    positionMs: Int, playing: Boolean, loading: Boolean, onToggle: () -> Unit, onSeek: (Float) -> Unit) {
    var seeking by remember(identity, durationMs) { mutableStateOf<Float?>(null) }
    val fraction = seeking ?: VoiceWaveform.progress(positionMs, durationMs)
    val displayedPosition = seeking?.let { VoiceWaveform.seekPosition(it, durationMs) } ?: positionMs
    val bins = remember(waveform) { waveform.takeIf(VoiceWaveform::valid).orEmpty() }
    val played = MaterialTheme.colorScheme.primary
    val unplayed = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = .65f)
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        FilledIconButton(onClick = onToggle, enabled = !loading, modifier = Modifier.size(48.dp)) {
            if (loading) CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            else Icon(if (playing) Icons.Default.Pause else Icons.Default.PlayArrow,
                if (playing) "Pause" else "Play", Modifier.size(28.dp))
        }
        Column(Modifier.weight(1f)) {
            Slider(value = fraction, onValueChange = { seeking = it },
                onValueChangeFinished = { seeking?.let(onSeek); seeking = null },
                enabled = durationMs > 0,
                modifier = Modifier.fillMaxWidth().height(48.dp).semantics {
                    contentDescription = "Recording playback position"
                    stateDescription = "${VoiceWaveform.time(displayedPosition)} of ${VoiceWaveform.time(durationMs)}"
                }, thumb = {}, track = { state ->
                    Canvas(Modifier.fillMaxWidth().height(30.dp)) {
                        fun drawTrack(color: androidx.compose.ui.graphics.Color) {
                            if (bins.isEmpty()) {
                                drawLine(color, Offset(0f, size.height / 2), Offset(size.width, size.height / 2),
                                    strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round)
                            } else {
                                val step = size.width / bins.size
                                val stroke = (step * .55f).coerceAtMost(3.dp.toPx())
                                val bottom = size.height - stroke / 2
                                bins.forEachIndexed { index, bin ->
                                    val height = (bin / 255f * (size.height - stroke)).coerceAtLeast(stroke)
                                    val x = (index + .5f) * step
                                    drawLine(color, Offset(x, bottom), Offset(x, bottom - height), stroke, StrokeCap.Round)
                                }
                            }
                        }
                        drawTrack(unplayed)
                        clipRect(right = size.width * state.value) { drawTrack(played) }
                    }
                })
            Text("${VoiceWaveform.time(displayedPosition)} / ${VoiceWaveform.time(durationMs)}",
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
