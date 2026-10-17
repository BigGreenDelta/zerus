package app.zerus.mobile

import android.Manifest
import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import androidx.camera.core.CameraSelector
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.VideoCapture
import androidx.camera.view.PreviewView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.LifecycleRegistry
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Fresh owned emulator only. Uses real CameraX and both synthetic cameras, without transport. */
class CameraXRebindStopInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 30_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { label }
            SystemClock.sleep(25)
        }
    }
    private class CaptureOwner : LifecycleOwner {
        val registry = LifecycleRegistry(this)
        override val lifecycle: Lifecycle get() = registry
    }
    private fun field(recorder: RoundVideoRecorder, name: String): Any? = recorder.javaClass
        .getDeclaredField(name).apply { isAccessible = true }.get(recorder)

    override fun onStart() {
        val result = Bundle()
        val report = JSONObject().put("scope", "Owned fresh dual-camera emulator; no relay, phone, native agent or live send")
        try {
            check(android.os.Build.FINGERPRINT.contains("generic") || android.os.Build.MODEL.contains("sdk"))
            check(PrivateStore(targetContext).connections().isEmpty())
            check(PrivateStore(targetContext).voiceState() == VoiceState())
            uiAutomation.grantRuntimePermission(targetContext.packageName, Manifest.permission.CAMERA)
            uiAutomation.grantRuntimePermission(targetContext.packageName, Manifest.permission.RECORD_AUDIO)
            val activity = startActivitySync(Intent(targetContext, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val preview = main { PreviewView(activity).apply {
                implementationMode = PreviewView.ImplementationMode.COMPATIBLE
                activity.setContentView(this)
            } }
            val cameras = ProcessCameraProvider.getInstance(activity).get(30, TimeUnit.SECONDS)
            check(main { cameras.hasCamera(CameraSelector.DEFAULT_FRONT_CAMERA) && cameras.hasCamera(CameraSelector.DEFAULT_BACK_CAMERA) }) {
                "Both synthetic cameras are required; a no-op switch is not qualification"
            }
            val cases = JSONArray()
            try {
                for (mode in listOf("stop", "cancel", "lifecycle")) {
                    val owner = main { CaptureOwner().apply {
                        registry.handleLifecycleEvent(Lifecycle.Event.ON_CREATE)
                        registry.handleLifecycleEvent(Lifecycle.Event.ON_START)
                    } }
                    val recorder = main { RoundVideoRecorder(activity, owner).also { it.previewSurface(preview.surfaceProvider) } }
                    val started = AtomicInteger()
                    val elapsed = AtomicInteger()
                    val completed = AtomicInteger()
                    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
                    val capture = scope.async { recorder.capture({ started.incrementAndGet() }, { elapsed.set(it) }) }
                    capture.invokeOnCompletion { completed.incrementAndGet() }
                    try {
                        await("$mode initial real video frames") { started.get() == 1 && elapsed.get() >= 1_000 }
                        val switched = main {
                            val video = field(recorder, "video") as VideoCapture<*>
                            val before = checkNotNull(video.camera).cameraInfo.lensFacing
                            check(before == CameraSelector.LENS_FACING_FRONT)
                            check(recorder.switchCamera()) { "Camera switch must succeed" }
                            val after = checkNotNull(video.camera).cameraInfo.lensFacing
                            check(after == CameraSelector.LENS_FACING_BACK && before != after)
                            // Same main-loop action: no sleep, frame timer or encoder-readiness workaround.
                            when (mode) {
                                "cancel" -> recorder.cancel()
                                "lifecycle" -> {
                                    recorder.stop()
                                    owner.registry.handleLifecycleEvent(Lifecycle.Event.ON_STOP)
                                }
                                else -> recorder.stop()
                            }
                            check(!recorder.switchCamera()) { "Terminal capture must reject another switch" }
                            before to after
                        }
                        val media = runBlocking { withTimeout(30_000) { capture.await() } }
                        await("Single terminal capture completion") { completed.get() == 1 }
                        if (mode == "cancel") {
                            check(media.pcm.isEmpty() && media.video == null)
                        } else {
                            val video = checkNotNull(media.video)
                            check(video.bytes.copyOfRange(4, 8).contentEquals("ftyp".toByteArray()))
                            check(video.durationMs in 100..VideoProtocol.MAX_DURATION_MS)
                            check(media.pcm.size >= 3_200)
                            check(kotlin.math.abs(media.pcm.size / 32 - video.durationMs) <= 1_000)
                        }
                        main {
                            check(field(recorder, "recording") == null && field(recorder, "provider") == null)
                            check(field(recorder, "preview") == null && field(recorder, "video") == null)
                            recorder.stop()
                            check(!recorder.switchCamera())
                        }
                        check(File(targetContext.cacheDir, "round-video-capture").listFiles().orEmpty().isEmpty())
                        check(PrivateStore(targetContext).voiceState() == VoiceState())
                        cases.put(JSONObject().put("mode", mode).put("front", switched.first).put("back", switched.second)
                            .put("start_events", started.get()).put("completions", completed.get())
                            .put("pcm_bytes", media.pcm.size).put("video_bytes", media.video?.bytes?.size ?: 0))
                    } finally {
                        scope.cancel()
                        main {
                            recorder.cancel()
                            owner.registry.handleLifecycleEvent(Lifecycle.Event.ON_DESTROY)
                        }
                    }
                }
            } finally { main { activity.finish() } }
            report.put("cases", cases).put("passed", true)
            File(targetContext.getExternalFilesDir(null), "camerax-rebind-stop.json").writeText(report.toString(2))
            result.putString("stream", "CameraX rebind smoke passed: real front/back binding followed immediately by Stop, Cancel and lifecycle stop; finalized media, terminal identity and private cleanup verified.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "CameraX rebind smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
}
