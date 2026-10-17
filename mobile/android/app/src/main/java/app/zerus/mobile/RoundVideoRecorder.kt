package app.zerus.mobile

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadataRetriever
import androidx.camera.core.CameraSelector
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.FallbackStrategy
import androidx.camera.video.FileOutputOptions
import androidx.camera.video.Quality
import androidx.camera.video.QualitySelector
import androidx.camera.video.Recorder
import androidx.camera.video.Recording
import androidx.camera.video.VideoCapture
import androidx.camera.video.VideoRecordEvent
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.Base64
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

internal data class CapturedVideo(val bytes: ByteArray, val durationMs: Int, val width: Int, val height: Int, val posterBase64: String)
internal data class CapturedMedia(val pcm: ByteArray, val video: CapturedVideo? = null)
internal interface CaptureDevice {
    suspend fun capture(started: () -> Unit, progress: (Int) -> Unit): CapturedMedia
    fun stop()
    fun cancel() = stop()
}
internal class AudioCaptureDevice(context: Context) : CaptureDevice {
    private val recorder = SpeechRecorder(context)
    override suspend fun capture(started: () -> Unit, progress: (Int) -> Unit) = CapturedMedia(recorder.capture(120_000, started, progress))
    override fun stop() = recorder.stop()
}

/** A single owned CameraX recording. Temp MP4s never enter shared Android storage. */
internal class RoundVideoRecorder(private val context: Context, private val owner: LifecycleOwner) : CaptureDevice {
    private val stopped = AtomicBoolean()
    private val discarded = AtomicBoolean()
    private var provider: ProcessCameraProvider? = null
    private var preview: Preview? = null
    private var video: VideoCapture<Recorder>? = null
    private var recording: Recording? = null
    private var surface: Preview.SurfaceProvider? = null
    private var front = true
    fun previewSurface(value: Preview.SurfaceProvider?) {
        surface = value
        preview?.setSurfaceProvider(value)
    }
    fun switchCamera(): Boolean {
        val cameras = provider ?: return false
        val capture = video ?: return false
        val livePreview = preview ?: return false
        val next = if (front) CameraSelector.DEFAULT_BACK_CAMERA else CameraSelector.DEFAULT_FRONT_CAMERA
        if (stopped.get()) return false
        return runCatching {
            if (!cameras.hasCamera(next)) return@runCatching false
            cameras.unbind(livePreview, capture)
            cameras.bindToLifecycle(owner, next, livePreview, capture)
            front = !front
            true
        }.getOrElse { stop(); false }
    }
    override fun stop() {
        stopped.set(true)
        ContextCompat.getMainExecutor(context).execute { recording?.stop() }
    }
    override fun cancel() { discarded.set(true); stop() }
    @androidx.annotation.OptIn(markerClass = [androidx.camera.video.ExperimentalPersistentRecording::class])
    @SuppressLint("MissingPermission") // The initiating gesture checks both permissions; revocation is a capture failure.
    override suspend fun capture(started: () -> Unit, progress: (Int) -> Unit): CapturedMedia {
        val output = withContext(Dispatchers.IO) {
            val directory = File(context.cacheDir, "round-video-capture").apply { check(isDirectory || mkdirs()) }
            File.createTempFile("capture-", ".mp4", directory)
        }
        try {
            withContext(Dispatchers.Main) {
                check(!stopped.get() && owner.lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) { "Recording stopped before the camera opened." }
                val future = ProcessCameraProvider.getInstance(context)
                val cameras = suspendCancellableCoroutine { continuation ->
                    future.addListener({
                        if (continuation.isActive) runCatching { future.get() }
                            .onSuccess { continuation.resume(it) }.onFailure { continuation.resumeWithException(it) }
                    }, ContextCompat.getMainExecutor(context))
                }
                check(!stopped.get()) { "Recording stopped before the camera opened." }
                provider = cameras
                val selector = if (cameras.hasCamera(CameraSelector.DEFAULT_FRONT_CAMERA)) CameraSelector.DEFAULT_FRONT_CAMERA
                    else CameraSelector.DEFAULT_BACK_CAMERA.also { front = false }
                check(cameras.hasCamera(selector)) { "No camera is available." }
                val recorder = Recorder.Builder().setQualitySelector(QualitySelector.from(Quality.SD,
                    FallbackStrategy.lowerQualityOrHigherThan(Quality.SD))).build()
                val capture = VideoCapture.withOutput(recorder)
                val livePreview = Preview.Builder().build().also { it.setSurfaceProvider(surface) }
                video = capture; preview = livePreview
                cameras.bindToLifecycle(owner, selector, livePreview, capture)
                val options = FileOutputOptions.Builder(output).setFileSizeLimit(VideoProtocol.MAX_BYTES.toLong() - 131_072)
                    .setDurationLimitMillis(59_000).build()
                suspendCancellableCoroutine<Unit> { continuation ->
                    continuation.invokeOnCancellation { stop() }
                    recording = recorder.prepareRecording(context, options).withAudioEnabled().asPersistentRecording()
                        .start(ContextCompat.getMainExecutor(context)) { event ->
                            when (event) {
                                is VideoRecordEvent.Start -> if (stopped.get()) recording?.stop() else started()
                                is VideoRecordEvent.Status -> progress((event.recordingStats.recordedDurationNanos / 1_000_000).toInt())
                                is VideoRecordEvent.Finalize -> if (continuation.isActive) {
                                    if (!event.hasError() || event.error in setOf(VideoRecordEvent.Finalize.ERROR_DURATION_LIMIT_REACHED,
                                            VideoRecordEvent.Finalize.ERROR_FILE_SIZE_LIMIT_REACHED)) continuation.resume(Unit)
                                    else continuation.resumeWithException(IllegalStateException("The camera could not save this recording (${event.error})."))
                                }
                            }
                        }
                    if (stopped.get()) recording?.stop()
                }
            }
            if (discarded.get()) return CapturedMedia(ByteArray(0))
            return withContext(Dispatchers.Default) {
                ensureActive()
                check(output.length() in 1..VideoProtocol.MAX_BYTES.toLong()) { "The recording exceeds 10 MiB." }
                val retriever = MediaMetadataRetriever()
                try {
                    retriever.setDataSource(output.absolutePath)
                    val duration = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toIntOrNull() ?: 0
                    val encodedWidth = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
                    val encodedHeight = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
                    val rotation = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
                    val width = if (rotation % 180 != 0) encodedHeight else encodedWidth
                    val height = if (rotation % 180 != 0) encodedWidth else encodedHeight
                    check(duration in 100..VideoProtocol.MAX_DURATION_MS && width in 16..1920 && height in 16..1920) { "The video recording is too short or unsupported." }
                    val poster = (if (android.os.Build.VERSION.SDK_INT >= 27) retriever.getScaledFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 160, 160) else null)?.let { frame ->
                        try { poster(frame) } finally { frame.recycle() }
                    }.orEmpty()
                    CapturedMedia(VideoAudioDecode.read(output), CapturedVideo(output.readBytes(), duration, width, height, poster))
                } finally { retriever.release() }
            }
        } finally {
            withContext(NonCancellable + Dispatchers.Main) {
                recording?.close(); recording = null
                preview?.let { p -> video?.let { v -> provider?.unbind(p, v) } }
                preview = null; video = null; provider = null
            }
            withContext(NonCancellable + Dispatchers.IO) { output.delete() }
        }
    }
    private fun poster(bitmap: Bitmap): String {
        for (quality in listOf(70, 50, 30)) {
            val bytes = ByteArrayOutputStream().also { bitmap.compress(Bitmap.CompressFormat.JPEG, quality, it) }.toByteArray()
            if (bytes.size <= 8192) return Base64.getEncoder().encodeToString(bytes)
        }
        return ""
    }
}
