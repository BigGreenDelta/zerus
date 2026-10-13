package app.zerus.mobile

import org.junit.Assert.assertEquals
import org.junit.Test

class VoiceCaptureFeedbackTest {
    @Test fun acceptedCaptureHasOneStartAndOneExplicitFinish() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.started(); feedback.started()
        feedback.finish(userInitiated = true)
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start), events)
        feedback.finish(userInitiated = true, cancelled = true)
        feedback.stopped(); feedback.stopped()
        feedback.started()
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start, VoiceCaptureFeedback.Event.Finish), events)
    }

    @Test fun cancelRemainsTerminalAfterFingerReturnsOrReleases() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.started()
        feedback.finish(userInitiated = true, cancelled = true)
        feedback.finish(userInitiated = true)
        feedback.stopped()
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start, VoiceCaptureFeedback.Event.Cancel), events)
    }

    @Test fun releaseBeforeMicrophoneReadySuppressesLateStartAndFinish() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.finish(userInitiated = true)
        feedback.stopped()
        feedback.started()
        feedback.finish(userInitiated = true)
        assertEquals(emptyList<VoiceCaptureFeedback.Event>(), events)
    }

    @Test fun backgroundFailureAndNaturalCompletionDoNotPulseOrPermitLateActions() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.started()
        feedback.finish(userInitiated = false)
        feedback.finish(userInitiated = true, cancelled = true)
        feedback.stopped()
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start), events)
    }

    @Test fun elapsedFormattingUsesCapturedTenthsAcrossMinuteBoundary() {
        assertEquals("0:00.0", voiceRecordingTime(-1))
        assertEquals("0:00.0", voiceRecordingTime(99))
        assertEquals("0:00.1", voiceRecordingTime(100))
        assertEquals("0:59.9", voiceRecordingTime(59_999))
        assertEquals("1:00.0", voiceRecordingTime(60_000))
        assertEquals("2:00.0", voiceRecordingTime(120_000))
    }

    @Test fun unsupportedFeedbackCannotAbortCaptureOrRepeatAnEvent() {
        var calls = 0
        val feedback = VoiceCaptureFeedback { calls++; error("Fixture haptic dispatch unavailable") }
        feedback.started(); feedback.started()
        feedback.finish(userInitiated = true); feedback.finish(userInitiated = true)
        feedback.stopped()
        assertEquals(2, calls)
    }

    @Test fun lifecycleExitSuppressesUserPulseStillWaitingForHardwareRelease() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.started(); feedback.finish(userInitiated = true)
        feedback.finish(userInitiated = false); feedback.stopped()
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start), events)
    }

    @Test fun naturalHardwareCompletionRetiresLateReleaseWithoutTerminalPulse() {
        val events = mutableListOf<VoiceCaptureFeedback.Event>()
        val feedback = VoiceCaptureFeedback(events::add)
        feedback.started(); feedback.stopped()
        feedback.finish(userInitiated = true); feedback.stopped()
        assertEquals(listOf(VoiceCaptureFeedback.Event.Start), events)
    }
}
