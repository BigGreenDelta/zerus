/*
 * Copyright 2026 Zerus contributors
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

package androidx.camera.video.internal.audio

import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/** 1.6.2 package-local constructor/queue bridge, only in the unit-test classpath. */
object SyntheticAudioFixture {
    fun create(settings: AudioSettings, executor: Executor): AudioSource = AudioSource(
        settings, executor, null,
        AudioStreamFactory { _, _ ->
            FakeAudioStream({ index -> FakeAudioStream.AudioData(ByteBuffer.allocate(1024), index * 10_000_000L) }, readDelayMs = 1)
        }, 1000L)
    fun drain(source: AudioSource) {
        val latch = CountDownLatch(1)
        source.mExecutor.execute { latch.countDown() }
        check(latch.await(1, TimeUnit.SECONDS))
    }
}
