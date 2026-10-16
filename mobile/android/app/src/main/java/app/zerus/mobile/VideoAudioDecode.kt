package app.zerus.mobile

import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteOrder

/** Decode only the MP4's audio track. Called on the media preparation worker. */
internal object VideoAudioDecode {
    suspend fun read(file: File): ByteArray {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            extractor.setDataSource(file.absolutePath)
            val track = (0 until extractor.trackCount).firstOrNull {
                extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: error("The recording has no audio track.")
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            format.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            val decoder = MediaCodec.createDecoderByType(checkNotNull(format.getString(MediaFormat.KEY_MIME)))
            codec = decoder; decoder.configure(format, null, null, 0); decoder.start()
            var conversion = MediaPcm16Converter(format.getInteger(MediaFormat.KEY_SAMPLE_RATE), format.getInteger(MediaFormat.KEY_CHANNEL_COUNT))
            var encoding = AudioFormat.ENCODING_PCM_16BIT
            var inputDone = false; var outputDone = false
            val information = MediaCodec.BufferInfo()
            val deadline = System.nanoTime() + 30_000_000_000L
            val context = currentCoroutineContext()
            while (!outputDone && !conversion.full) {
                context.ensureActive()
                check(System.nanoTime() < deadline) { "Preparing the recording's audio timed out." }
                if (!inputDone) {
                    val index = decoder.dequeueInputBuffer(10_000)
                    if (index >= 0) {
                        val input = checkNotNull(decoder.getInputBuffer(index))
                        val size = extractor.readSampleData(input, 0)
                        if (size < 0) {
                            decoder.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inputDone = true
                        } else {
                            decoder.queueInputBuffer(index, 0, size, extractor.sampleTime.coerceAtLeast(0), 0); extractor.advance()
                        }
                    }
                }
                when (val index = decoder.dequeueOutputBuffer(information, 10_000)) {
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val output = decoder.outputFormat
                        conversion = MediaPcm16Converter(output.getInteger(MediaFormat.KEY_SAMPLE_RATE), output.getInteger(MediaFormat.KEY_CHANNEL_COUNT))
                        encoding = if (output.containsKey(MediaFormat.KEY_PCM_ENCODING)) output.getInteger(MediaFormat.KEY_PCM_ENCODING) else AudioFormat.ENCODING_PCM_16BIT
                        check(encoding in setOf(AudioFormat.ENCODING_PCM_16BIT, AudioFormat.ENCODING_PCM_FLOAT)) { "Unsupported recording audio format." }
                    }
                    else -> if (index >= 0) {
                        val output = checkNotNull(decoder.getOutputBuffer(index)).order(ByteOrder.LITTLE_ENDIAN)
                        output.position(information.offset); output.limit(information.offset + information.size)
                        while (output.remaining() >= if (encoding == AudioFormat.ENCODING_PCM_FLOAT) 4 else 2) {
                            val sample = if (encoding == AudioFormat.ENCODING_PCM_FLOAT) (output.float.coerceIn(-1f, 1f) * 32767).toInt() else output.short.toInt()
                            conversion.sample(sample)
                        }
                        decoder.releaseOutputBuffer(index, false)
                        outputDone = information.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    }
                }
            }
            return conversion.bytes().also { check(it.size >= 3200) { "No speech audio was recorded." } }
        } finally {
            codec?.let { runCatching { it.stop() }; it.release() }; extractor.release()
        }
    }
}

/** Streaming mono conversion with a bounded box filter, including 44.1 kHz AAC. */
internal class MediaPcm16Converter(private val rate: Int, private val channels: Int) {
    init { require(rate in 8_000..192_000 && channels in 1..8) }
    private val output = ByteArrayOutputStream()
    private var channel = 0; private var channelSum = 0L
    private var phase = 0; private var sum = 0L; private var frames = 0
    val full get() = output.size() >= 1_920_000
    fun sample(value: Int) {
        if (full) return
        channelSum += value.coerceIn(-32768, 32767); channel++
        if (channel != channels) return
        sum += channelSum / channels; frames++; phase += 16_000
        channel = 0; channelSum = 0
        while (phase >= rate && !full) {
            val sample = (sum / frames).toInt()
            output.write(sample and 255); output.write(sample shr 8 and 255); phase -= rate
        }
        if (phase < 16_000) { sum = 0; frames = 0 }
    }
    fun bytes(): ByteArray = output.toByteArray()
}
