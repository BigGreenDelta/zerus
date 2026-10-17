/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Modified by Zerus: focused upstream orderings and 1.6.2 fixture adapters; see test-provenance.json.
package androidx.camera.video

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import android.util.Range
import android.util.Size
import androidx.camera.core.SurfaceRequest
import androidx.camera.core.impl.CameraInternal
import androidx.camera.core.impl.CameraInfoInternal
import androidx.camera.core.impl.utils.executor.CameraXExecutors
import androidx.camera.core.impl.utils.executor.CameraXExecutors.directExecutor
import androidx.camera.core.impl.utils.executor.CameraXExecutors.mainThreadExecutor
import androidx.camera.testing.impl.fakes.*
import androidx.camera.video.Recorder.sRetrySetupVideoDelayMs
import androidx.camera.video.Recorder.sRetrySetupVideoMaxCount
import androidx.camera.video.VideoOutput.SourceState.*
import androidx.camera.video.VideoRecordEvent.Finalize
import androidx.camera.video.VideoRecordEvent.Finalize.ERROR_NONE
import androidx.camera.video.internal.BufferProvider
import androidx.camera.video.internal.OutputStorage
import androidx.camera.video.internal.audio.AudioSettings
import androidx.camera.video.internal.audio.SyntheticAudioFixture
import androidx.camera.video.internal.audio.FakeAudioStream
import androidx.camera.video.internal.encoder.*
import androidx.camera.video.internal.muxer.MuxerFactory
import androidx.test.core.app.ApplicationProvider
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.Executor
import java.util.concurrent.atomic.AtomicReference
import java.lang.reflect.Proxy
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.Assert.*
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.mockito.Mockito.*
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config

/** Upstream968a36ce four ordering tests plus both prerequisites' recording/finalization assertions.
 * Only the fixture adapters target1.6.2; ordering bodies and their assertions are retained.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class RecorderRebindStopTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val context: Context = ApplicationProvider.getApplicationContext()
    private val fakeCamera = mock(CameraInternal::class.java, RETURNS_DEEP_STUBS)
    private val surfaceRequestsToClose = mutableListOf<SurfaceRequest>()
    private val recordersToRelease = mutableListOf<Recorder>()
    private val recordings = mutableListOf<Recording>()
    private var currentTimestampUs = 100_000L
    private var latestVideoEncoder: FakeEncoder? = null
    private var latestAudioEncoder: FakeEncoder? = null
    private var latestSurfaceRequest: SurfaceRequest? = null

    @Before fun setUp() {
        org.robolectric.shadows.ShadowLog.stream = System.out
        System.getProperty("zerus.camerax.expectedRecorderSha256")?.let { expected ->
            val bytes = checkNotNull(Recorder::class.java.getResourceAsStream("Recorder.class")).use { it.readBytes() }
            val actual = java.security.MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
            assertEquals("Negative baseline must load the exact original Recorder, without ABI mismatch", expected, actual)
        }
        shadowOf(context as Application).grantPermissions(Manifest.permission.RECORD_AUDIO)
        val info = fakeCamera.cameraInfoInternal
        `when`(info.cameraId).thenReturn("0")
        `when`(fakeCamera.cameraInfo).thenReturn(info)
        `when`(info.encoderProfilesProvider).thenReturn(androidx.camera.core.impl.EncoderProfilesProvider.EMPTY)
        // Use the Recorder fallback profile; codec hardware discovery belongs to the native suite.
        `when`(info.supportedDynamicRanges).thenReturn(emptySet())
        `when`(info.getSupportedResolutions(anyInt())).thenReturn(listOf(Size(640, 480)))
    }
    @After fun tearDown() {
        recordings.forEach { it.close() }
        idleMainLooper()
        recordersToRelease.forEach { it.onSourceStateChanged(INACTIVE) }
        surfaceRequestsToClose.forEach { it.deferrableSurface.close() }
        idleMainLooper()
        val latch = CountDownLatch(2)
        val audioExecutor = Recorder::class.java.getDeclaredField("AUDIO_EXECUTOR").apply { isAccessible = true }.get(null) as Executor
        audioExecutor.execute { latch.countDown() }
        CameraXExecutors.audioExecutor().execute { latch.countDown() }
        assertTrue(latch.await(1, TimeUnit.SECONDS))
    }
    private fun idleMainLooper() = shadowOf(Looper.getMainLooper()).idle()
    @Test
    fun persistentRecording_afterRebind_finalizeWaitsForAudioEncoderToStop() {
        // Arrange: a persistent recording with audio whose audio encoder stop can be held back.
        var audioEncoder: StopDeferringEncoder? = null
        val recorder =
            createRecorder(
                audioEncoderFactory = { _, _, _ ->
                    StopDeferringEncoder(createFakeAudioEncoder()).also { audioEncoder = it }
                }
            )
        val recording =
            createRecording(recorder, withAudio = true, asPersistentRecording = true)
                .startAndVerify()
                .sendFrames()

        // Unbind and rebind, so the video encoder is replaced while the audio encoder is kept.
        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()
        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()
        recording.sendFrames(3)

        // Act: stop the recording while the audio encoder has not stopped yet.
        val deferringAudioEncoder = checkNotNull(audioEncoder)
        deferringAudioEncoder.deferStop = true
        recording.stop()

        // Assert: the recording is not finalized until the audio encoder stops.
        recording.verifyNoFinalize()
        deferringAudioEncoder.completeDeferredStop()
        idle(recorder)
        recording.verifyFinalize()
    }

    @Test
    fun persistentRecording_stopAfterSourceInactive_finalizes() {
        val recorder = createRecorder()
        val recording =
            createRecording(recorder, asPersistentRecording = true, withAudio = true)
                .startAndVerify()
                .sendFrames()

        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()

        recording.stopAndVerify()
    }

    @Test
    fun persistentRecording_withoutAudio_stopAfterSourceInactive_finalizes() {
        val recorder = createRecorder()
        val recording =
            createRecording(recorder, asPersistentRecording = true, withAudio = false)
                .startAndVerify()
                .sendFrames()

        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()

        recording.stopAndVerify()
    }

    @Test
    fun persistentRecording_stopThenRebind_newVideoEncoderConfiguredWhileStopping_doesNotCrash() {
        var audioEncoder: StopDeferringEncoder? = null
        val recorder =
            createRecorder(
                audioEncoderFactory = { _, _, _ ->
                    StopDeferringEncoder(createFakeAudioEncoder()).also { audioEncoder = it }
                }
            )
        val recording =
            createRecording(recorder, asPersistentRecording = true, withAudio = true)
                .startAndVerify()
                .sendFrames()

        // Unbind, so the video encoder is released while the recording is retained.
        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()

        // Stop the recording, which stays in STOPPING until the audio encoder stops.
        val deferringAudioEncoder = checkNotNull(audioEncoder)
        deferringAudioEncoder.deferStop = true
        recording.stop()
        idleMainLooper()

        // Rebind, so a new video encoder is configured while the Recorder is STOPPING.
        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()
        recording.verifyNoFinalize()

        deferringAudioEncoder.completeDeferredStop()
        idle(recorder)
        recording.verifyFinalize(error = ERROR_NONE)

        // The new video encoder can be used for the next recording.
        createRecording(recorder = recorder).startAndVerify().sendFrames(3).stopAndVerify()
    }

    @Test
    fun persistentRecording_stopThenRebind_newVideoEncoderConfiguredAfterFinalized_doesNotCrash() {
        val recorder = createRecorder()
        val recording =
            createRecording(recorder, asPersistentRecording = true, withAudio = false)
                .startAndVerify()
                .sendFrames()

        // Unbind and stop the recording, so it is finalized while there is no video encoder.
        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()
        recording.stopAndVerify()

        // Rebind, so a new video encoder is configured after the recording is finalized.
        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()

        createRecording(recorder = recorder).startAndVerify().sendFrames(3).stopAndVerify()
    }

    @Test
    fun persistentRecording_rebindThenStop_newVideoEncoderConfiguredWhileStopping_doesNotCrash() {
        var failVideoEncoderCreation = false
        var audioEncoder: StopDeferringEncoder? = null
        val originalRetrySetupVideoMaxCount = sRetrySetupVideoMaxCount
        val originalRetrySetupVideoDelayMs = sRetrySetupVideoDelayMs
        try {
            val recorder =
                createRecorder(
                    videoEncoderFactory =
                        createVideoEncoderFactory(
                            shouldFail = {
                                failVideoEncoderCreation.also { failVideoEncoderCreation = false }
                            }
                        ),
                    audioEncoderFactory = { _, _, _ ->
                        StopDeferringEncoder(createFakeAudioEncoder()).also { audioEncoder = it }
                    },
                    retrySetupVideoMaxCount = 1,
                    retrySetupVideoDelayMs = RETRY_SETUP_VIDEO_DELAY_MS,
                )
            val recording =
                createRecording(recorder, asPersistentRecording = true, withAudio = true)
                    .startAndVerify()
                    .sendFrames()

            // Unbind, so the video encoder is released while the recording is retained.
            recorder.onSourceStateChanged(INACTIVE)
            latestSurfaceRequest?.deferrableSurface?.close()
            idleMainLooper()

            // Rebind. The first attempt to create the new video encoder fails, so the new video
            // encoder isn't configured until the setup is retried.
            failVideoEncoderCreation = true
            recorder.sendSurfaceRequest()
            recorder.onSourceStateChanged(ACTIVE_STREAMING)
            idleMainLooper()

            // Stop the recording, which stays in STOPPING until the audio encoder stops.
            val deferringAudioEncoder = checkNotNull(audioEncoder)
            deferringAudioEncoder.deferStop = true
            recording.stop()
            recording.verifyNoFinalize()

            // The new video encoder is configured while the Recorder is STOPPING.
            shadowOf(Looper.getMainLooper())
                .idleFor(RETRY_SETUP_VIDEO_DELAY_MS, TimeUnit.MILLISECONDS)
            recording.verifyNoFinalize()

            deferringAudioEncoder.completeDeferredStop()
            idle(recorder)
            recording.verifyFinalize(error = ERROR_NONE)

            // The new video encoder can be used for the next recording.
            createRecording(recorder = recorder).startAndVerify().sendFrames(3).stopAndVerify()
        } finally {
            sRetrySetupVideoMaxCount = originalRetrySetupVideoMaxCount
            sRetrySetupVideoDelayMs = originalRetrySetupVideoDelayMs
        }
    }

    @Test
    fun persistentRecording_rebindThenStop_newVideoEncoderConfiguredAfterFinalized_doesNotCrash() {
        var failVideoEncoderCreation = false
        val originalRetrySetupVideoMaxCount = sRetrySetupVideoMaxCount
        val originalRetrySetupVideoDelayMs = sRetrySetupVideoDelayMs
        try {
            val recorder =
                createRecorder(
                    videoEncoderFactory =
                        createVideoEncoderFactory(
                            shouldFail = {
                                failVideoEncoderCreation.also { failVideoEncoderCreation = false }
                            }
                        ),
                    retrySetupVideoMaxCount = 1,
                    retrySetupVideoDelayMs = RETRY_SETUP_VIDEO_DELAY_MS,
                )
            val recording =
                createRecording(recorder, asPersistentRecording = true, withAudio = false)
                    .startAndVerify()
                    .sendFrames()

            // Unbind, so the video encoder is released while the recording is retained.
            recorder.onSourceStateChanged(INACTIVE)
            latestSurfaceRequest?.deferrableSurface?.close()
            idleMainLooper()

            // Rebind. The first attempt to create the new video encoder fails, so the new video
            // encoder isn't configured until the setup is retried.
            failVideoEncoderCreation = true
            recorder.sendSurfaceRequest()
            recorder.onSourceStateChanged(ACTIVE_STREAMING)
            idleMainLooper()

            // Stop the recording. It is finalized right away since it has no video encoder.
            recording.stopAndVerify()

            // The new video encoder is configured after the recording is finalized.
            shadowOf(Looper.getMainLooper())
                .idleFor(RETRY_SETUP_VIDEO_DELAY_MS, TimeUnit.MILLISECONDS)

            createRecording(recorder = recorder).startAndVerify().sendFrames(3).stopAndVerify()
        } finally {
            sRetrySetupVideoMaxCount = originalRetrySetupVideoMaxCount
            sRetrySetupVideoDelayMs = originalRetrySetupVideoDelayMs
        }
    }

    @Test
    fun persistentRecording_canContinueRecordingAfterRebind() {
        val recorder = createRecorder()
        val recording =
            createRecording(recorder, asPersistentRecording = true, withAudio = true)
                .startAndVerify()
                .sendFrames()

        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()

        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()

        recording.clearEvents()
        recording.sendFrames(3)
        recording.verifyStatus(statusCount = 3)

        recording.stopAndVerify()
    }

    @Test
    fun persistentRecording_canContinueRecordingPausedAfterRebind() {
        val recorder = createRecorder()
        val recording =
            createRecording(recorder, asPersistentRecording = true)
                .startAndVerify()
                .sendFrames()
                .pauseAndVerify()

        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()

        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()

        recording.resumeAndVerify().sendFrames(3).stopAndVerify()
    }

    @Test
    fun persistentRecording_videoEncoderErrorWhileStopping_finalizes() {
        val recorder =
            createRecorder(
                videoEncoderFactory =
                    EncoderFactory { _, _, _ ->
                        val fakeEncoder = FakeEncoder().also { latestVideoEncoder = it }
                        object : Encoder by fakeEncoder {
                            override fun stop(expectedStopTimeUs: Long) {
                                fakeEncoder.triggerEncodeError(
                                    EncodeException.ERROR_UNKNOWN,
                                    "Test error",
                                )
                            }
                        }
                    }
            )
        val recording =
            createRecording(recorder, asPersistentRecording = true).startAndVerify().sendFrames()

        recording.stop()
        idleMainLooper()

        recording.verifyFinalize(error = Finalize.ERROR_ENCODING_FAILED)
    }

    @Test
    fun persistentRecording_staleReleasedEncoderStop_doesNotLoseCurrentFinalize() {
        val recorder = createRecorder()
        val recording = createRecording(recorder, asPersistentRecording = true).startAndVerify().sendFrames()
        val released = checkNotNull(latestVideoEncoder)
        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()
        recorder.sendSurfaceRequest()
        recorder.onSourceStateChanged(ACTIVE_STREAMING)
        idleMainLooper()
        assertNotSame(released, latestVideoEncoder)
        recording.sendFrames(3)
        // A released encoder can deliver an old stop callback after the new one is attached.
        released.stop()
        idleMainLooper()
        recording.verifyNoFinalize()
        // Losing the new completer to that stale callback would leave this unbound Stop hanging.
        recorder.onSourceStateChanged(INACTIVE)
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()
        recording.stopAndVerify()
    }

    private fun Recorder.sendSurfaceRequest(): SurfaceRequest {
        latestSurfaceRequest?.deferrableSurface?.close()
        idleMainLooper()
        val request = SurfaceRequest(Size(640, 480), fakeCamera) {}
        latestSurfaceRequest = request; surfaceRequestsToClose.add(request)
        onSurfaceRequested(request); idleMainLooper()
        return request
    }
    private fun createRecorder(
        videoEncoderFactory: EncoderFactory? = null,
        audioEncoderFactory: EncoderFactory? = null,
        retrySetupVideoMaxCount: Int? = null,
        retrySetupVideoDelayMs: Long? = null,
    ): Recorder {
        val recorder = Recorder.Builder().setExecutor(mainThreadExecutor())
            .setVideoEncoderFactory(videoEncoderFactory ?: createVideoEncoderFactory { false })
            .setAudioEncoderFactory(audioEncoderFactory ?: EncoderFactory { _, _, _ -> createFakeAudioEncoder() })
            .setMuxerFactory(MuxerFactory { NoOpMuxer() })
            .setOutputStorageFactory(object : OutputStorage.Factory {
                override fun create(options: OutputOptions): OutputStorage = object : OutputStorage {
                    override fun getOutputOptions() = options
                    override fun getAvailableBytes() = Long.MAX_VALUE
                }
            })
            .build().apply {
                retrySetupVideoMaxCount?.let { sRetrySetupVideoMaxCount = it }
                retrySetupVideoDelayMs?.let { sRetrySetupVideoDelayMs = it }
            }
        recordersToRelease += recorder
        recorder.sendSurfaceRequest();recorder.onSourceStateChanged(ACTIVE_STREAMING);idleMainLooper()
        return recorder
    }
    private fun createVideoEncoderFactory(shouldFail: () -> Boolean): EncoderFactory = EncoderFactory { _, config, _ ->
        if (shouldFail()) throw InvalidConfigException("Create video encoder fail on purpose.")
        FakeEncoder(encoderInfo = FakeVideoEncoderInfo(supportedBitrateRange = Range(1, 100_000_000)), encoderConfig = config)
            .also { latestVideoEncoder = it }
    }
    private fun createFakeAudioEncoder(): FakeEncoder {
        val input = FakeByteBufferInput()
        return FakeEncoder(encoderInput = input, onStateChanged = { active ->
            input.setState(if(active) BufferProvider.State.ACTIVE else BufferProvider.State.INACTIVE)
        }).also { latestAudioEncoder = it }
    }
    private fun idle(recorder: Recorder) {
        idleMainLooper()
        recorder.mAudioSource?.let { audio -> repeat(2) {
            SyntheticAudioFixture.drain(audio);idleMainLooper()
        } }
    }
    private fun createRecording(recorder: Recorder = createRecorder(), withAudio: Boolean = false,
        asPersistentRecording: Boolean = false): TestRecording = TestRecording(recorder, withAudio, asPersistentRecording)

    private inner class TestRecording(val recorder: Recorder, val withAudio: Boolean, val persistent: Boolean) {
        private val events = mutableListOf<VideoRecordEvent>()
        private lateinit var handle: Recording
        fun startAndVerify(): TestRecording {
            var pending = recorder.prepareRecording(context, FileOutputOptions.Builder(temporaryFolder.newFile()).build())
            if(withAudio) pending = pending.withAudioEnabled()
            if(persistent) pending = pending.asPersistentRecording()
            handle = pending.start(directExecutor()) { events += it };recordings += handle
            if (withAudio) {
                // 1.6.2 has no Recorder.Builder AudioStreamFactory seam. Substitute only the
                // pending record's one-time audio-source supplier before the start queue runs.
                val record = checkNotNull(recorder.mPendingRecordingRecord)
                val field = Recorder.RecordingRecord::class.java.getDeclaredField("mAudioSourceSupplier").apply { isAccessible = true }
                val supplier = Class.forName("androidx.camera.video.Recorder\$RecordingRecord\$AudioSourceSupplier")
                @Suppress("UNCHECKED_CAST") val reference = field.get(record) as AtomicReference<Any>
                reference.set(Proxy.newProxyInstance(supplier.classLoader, arrayOf(supplier)) { _, method, args ->
                    check(method.name == "get")
                    SyntheticAudioFixture.create(args!![0] as AudioSettings, args[1] as Executor)
                })
            }
            idle(recorder)
            assertEquals("Start events: $events", 1, events.filterIsInstance<VideoRecordEvent.Start>().size)
            return this
        }
        fun stop() { handle.stop();idle(recorder) }
        fun stopAndVerify(): TestRecording { stop();verifyFinalize();return this }
        fun verifyNoFinalize() { assertTrue("Unexpected Finalize: $events", events.none { it is Finalize }) }
        fun verifyFinalize(error: Int = ERROR_NONE) {
            val final = events.filterIsInstance<Finalize>()
            assertEquals("Exactly one Finalize: $events", 1, final.size)
            assertEquals("Finalize error: ${final.single().cause}", error, final.single().error)
        }
        fun clearEvents() { events.clear() }
        fun verifyStatus(statusCount: Int) { assertEquals(statusCount, events.filterIsInstance<VideoRecordEvent.Status>().size) }
        fun pauseAndVerify(): TestRecording { handle.pause();idle(recorder);assertTrue(events.any { it is VideoRecordEvent.Pause });return this }
        fun resumeAndVerify(): TestRecording { handle.resume();idle(recorder);assertTrue(events.any { it is VideoRecordEvent.Resume });return this }
        fun sendFrames(count: Int = 1): TestRecording {
            repeat(count) {
                assertTrue("Video encoder must be started", latestVideoEncoder?.isStarted == true)
                currentTimestampUs += 100_000L
                latestVideoEncoder?.sendEncodedData(presentationTimeUs = currentTimestampUs)
                if(withAudio && latestAudioEncoder?.isStarted == true)
                    latestAudioEncoder?.sendEncodedData(size = 512, presentationTimeUs = currentTimestampUs, isKeyFrame = false)
                idle(recorder)
            }
            return this
        }
    }
    /**
     * An [Encoder] that can hold back [stop] so a test controls when the encoder stops.
     *
     * Like `EncoderImpl`, [release] while the stop is held back is deferred until the encoder
     * stops.
     */
    private class StopDeferringEncoder(private val delegate: FakeEncoder) : Encoder by delegate {
        var deferStop = false
        private var deferredStopTimeUs: Long? = null
        private var isReleaseDeferred = false

        override fun stop(expectedStopTimeUs: Long) {
            if (deferStop) {
                deferredStopTimeUs = expectedStopTimeUs
            } else {
                delegate.stop(expectedStopTimeUs)
            }
        }

        override fun release() {
            if (deferredStopTimeUs != null) {
                isReleaseDeferred = true
            } else {
                delegate.release()
            }
        }

        fun completeDeferredStop() {
            val stopTimeUs = checkNotNull(deferredStopTimeUs) { "stop() was not called." }
            deferredStopTimeUs = null
            delegate.stop(stopTimeUs)
            if (isReleaseDeferred) {
                isReleaseDeferred = false
                delegate.release()
            }
        }
    }

    private companion object { private const val RETRY_SETUP_VIDEO_DELAY_MS = 10L }
}
