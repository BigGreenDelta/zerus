package app.zerus.mobile

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import androidx.core.content.ContextCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicBoolean

/** Bounded foreground recording. Neither PCM conversion nor AudioRecord.read runs on the UI thread. */
internal class SpeechRecorder(private val context: Context) {
    private val stopped = AtomicBoolean(false)
    fun stop() { stopped.set(true) }

    suspend fun capture(maxDurationMs: Int, started: suspend () -> Unit = {}, progress: suspend (Int) -> Unit): ByteArray = withContext(Dispatchers.IO) {
        currentCoroutineContext().ensureActive()
        if (stopped.get()) return@withContext byteArrayOf()
        check(ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            "Allow microphone access to record your voice."
        }
        val candidates = listOf(16_000, 48_000)
        var recorder: AudioRecord? = null
        var sampleRate = 16_000
        for (rate in candidates) {
            currentCoroutineContext().ensureActive()
            if (stopped.get()) return@withContext byteArrayOf()
            val minimum = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (minimum <= 0) continue
            val candidate = try { AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, rate,
                AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(minimum, rate / 5 * 2)) }
            catch (_: IllegalArgumentException) { continue }
            if (candidate.state == AudioRecord.STATE_INITIALIZED) { recorder = candidate; sampleRate = rate; break }
            candidate.release()
        }
        val active = checkNotNull(recorder) { "This device could not open a microphone." }
        try {
            currentCoroutineContext().ensureActive()
            if (stopped.get()) return@withContext byteArrayOf()
            active.startRecording()
            check(active.recordingState == AudioRecord.RECORDSTATE_RECORDING) { "The microphone is unavailable. Check other recording apps." }
            withContext(Dispatchers.Main) { started() }
            val pcm = Pcm16Converter(sampleRate)
            val maxBytes = minOf(SpeechProtocol.maxBytes, maxDurationMs.coerceIn(1_000, 120_000) * 32)
            val samples = ShortArray(sampleRate / 20)
            var lastTenth = -1
            while (!stopped.get() && pcm.size < maxBytes) {
                currentCoroutineContext().ensureActive()
                val count = active.read(samples, 0, samples.size, AudioRecord.READ_BLOCKING)
                check(count > 0) { "The microphone stopped responding." }
                pcm.add(samples, count, maxBytes)
                val elapsedMs = pcm.size / 32
                val tenth = elapsedMs / 100
                if (tenth != lastTenth) { lastTenth = tenth; withContext(Dispatchers.Main) { progress(elapsedMs) } }
            }
            pcm.bytes()
        } finally {
            runCatching { active.stop() }
            active.release()
        }
    }
}

/** The 48 kHz fallback uses a three-sample low-pass average, retaining partial groups between reads. */
internal class Pcm16Converter(sampleRate: Int) {
    private val ratio = when (sampleRate) { 16_000 -> 1; 48_000 -> 3; else -> error("Unsupported microphone sample rate") }
    private val output = ByteArrayOutputStream()
    private var sum = 0
    private var count = 0
    val size get() = output.size()
    fun add(samples: ShortArray, length: Int, limit: Int = SpeechProtocol.maxBytes) {
        require(length in 0..samples.size)
        for (index in 0 until length) {
            if (output.size() + 2 > limit) break
            sum += samples[index].toInt(); count++
            if (count == ratio) {
                val sample = sum / ratio
                output.write(sample and 255); output.write((sample shr 8) and 255)
                count = 0; sum = 0
            }
        }
    }
    fun bytes(): ByteArray = output.toByteArray()
}
