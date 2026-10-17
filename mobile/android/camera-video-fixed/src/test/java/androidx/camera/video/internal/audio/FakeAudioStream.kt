/*
 * Copyright 2023 The Android Open Source Project
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

// Modified by Zerus for CameraX 1.6.2 fixture APIs; see test-provenance.json.
package androidx.camera.video.internal.audio

import android.annotation.SuppressLint
import androidx.camera.video.internal.audio.AudioStream.PacketInfo
import androidx.core.util.Preconditions.checkArgument
import java.nio.ByteBuffer
import java.util.concurrent.Executor
import kotlin.math.min

class FakeAudioStream(
    private val audioDataProvider: (index: Int) -> AudioData,
    isSilenced: Boolean = false,
    private val exceptionOnStart: AudioStream.AudioStreamException? = null,
    private val exceptionOnStartMaxTimes: Int = Int.MAX_VALUE,
    private val readDelayMs: Long = 0,
) : AudioStream {
    var isSilenced: Boolean = isSilenced
        set(value) {
            if (field != value) {
                field = value
                notifySilence()
            }
        }

    private val _audioDataList = mutableListOf<AudioData>()
    private var bufferIndex = 0
    private var isReleased = false
    private var isStarted = false
    private var audioStreamCallback: AudioStream.AudioStreamCallback? = null
    private var callbackExecutor: Executor? = null
    private var exceptionOnStartTimes = 0

    override fun start() {
        if (isReleased) {
            throw IllegalStateException()
        }
        if (exceptionOnStart != null && exceptionOnStartTimes++ < exceptionOnStartMaxTimes) {
            throw exceptionOnStart
        }
        if (isStarted) {
            return
        }
        isStarted = true
        notifySilence()
    }

    override fun stop() {
        if (isReleased) {
            throw IllegalStateException()
        }
        if (!isStarted) {
            return
        }
        isStarted = false
        exceptionOnStartTimes = 0
    }

    override fun release() {
        if (isReleased) {
            return
        }
        isReleased = true
    }

    @SuppressLint("BanThreadSleep")
    override fun read(byteBuffer: ByteBuffer): PacketInfo {
        if (isReleased || !isStarted) {
            throw IllegalStateException()
        }
        if (readDelayMs > 0) {
            Thread.sleep(readDelayMs)
        }
        val audioData = audioDataProvider.invoke(bufferIndex++)
        _audioDataList.add(audioData)
        val readSizeInByte = min(audioData.byteBuffer.remaining(), byteBuffer.remaining())
        val packet = PacketInfo.of(readSizeInByte, audioData.timestampNs)
        if (packet.sizeInBytes > 0) {
            // Duplicate and limit source size to prevent BufferOverflowException.
            val sourceByteBuffer = audioData.byteBuffer.duplicate()
            sourceByteBuffer.limit(readSizeInByte)

            val originalPosition = byteBuffer.position()
            byteBuffer.put(sourceByteBuffer)
            byteBuffer.limit(byteBuffer.position())
            byteBuffer.position(originalPosition)
        }
        return packet
    }

    override fun setCallback(callback: AudioStream.AudioStreamCallback?, executor: Executor?) {
        checkArgument(
            callback == null || executor != null,
            "executor can't be null with non-null callback.",
        )
        audioStreamCallback = callback
        callbackExecutor = executor
        notifySilence()
    }

    fun getAudioDataList(): List<AudioData> {
        return _audioDataList
    }

    private fun notifySilence() {
        if (!isStarted || isReleased) {
            return
        }
        callbackExecutor?.execute { audioStreamCallback?.onSilenceStateChanged(isSilenced) }
    }

    data class AudioData(val byteBuffer: ByteBuffer, val timestampNs: Long)
}
