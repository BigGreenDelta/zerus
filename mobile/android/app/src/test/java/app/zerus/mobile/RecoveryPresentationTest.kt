package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class RecoveryPresentationTest {
    private val target = Target("computer","codex/example/mobile","run","conversation","workspace")
    private fun raw() = JSONObject().put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
        .put("recovery",JSONObject().put("name",target.session).put("id","job").put("identity",JSONArray(listOf(target.run,target.conversation)))
            .put("state","waiting").put("attempt",2).put("class_attempt",1).put("delays",JSONArray(listOf(15,30,-1))).put("due_at",120).put("not_before",115))
    @Test fun nativeCountdownAndRateLimitAreDistinct() {
        val view = RecoveryPresentation.from(raw(),target,true)!!
        assertEquals("Retry in 10 s",view.status(110.0))
        assertFalse(view.retryTimeAllowed(110.0))
        assertTrue(view.retryTimeAllowed(115.0))
        assertEquals("1/2 attempts",view.attempts)
        assertFalse(view.recorded)
    }
    @Test fun repeatingScheduleAndAcknowledgmentAreReportedWithoutInventingSuccess() {
        val raw = raw();val job = raw.getJSONObject("recovery")
        job.put("delays",JSONArray(listOf(15,0))).put("state","retrying").put("acknowledged",false)
        assertEquals("2 attempts (repeats until stopped)",RecoveryPresentation.from(raw,target,true)!!.attempts)
        assertEquals("Waiting for delivery confirmation…",RecoveryPresentation.from(raw,target,true)!!.status(0.0))
        job.put("acknowledged",true)
        assertEquals("Recovery in progress",RecoveryPresentation.from(raw,target,true)!!.status(0.0))
        assertFalse(RecoveryPresentation.from(raw,target,true)!!.resumed)
    }
    @Test fun resumedTurnIsOrdinaryWorkWhileTheEpisodeKeepsItsBudget() {
        val raw = raw();val job = raw.getJSONObject("recovery")
        job.put("state","retrying").put("acknowledged",true).put("resumed_at",125)
        val view = RecoveryPresentation.from(raw,target,true)!!
        assertTrue(view.resumed);assertEquals("Agent resumed after recovery",view.status(0.0))
        assertEquals("1/2 attempts",view.attempts)
        job.put("acknowledged",false)
        assertFalse(RecoveryPresentation.from(raw,target,true)!!.resumed)
        job.put("acknowledged",true).put("resumed_at","125")
        assertFalse("an invalid time is not a resumption",RecoveryPresentation.from(raw,target,true)!!.resumed)
        job.put("resumed_at",125).put("state","waiting")
        assertFalse("a rescheduled episode is no longer resumed",RecoveryPresentation.resumed(job))
    }
    @Test fun cachedOrReusedJobIsReadOnly() {
        assertTrue(RecoveryPresentation.from(raw(),target,false)!!.recorded)
        val reused = raw();reused.getJSONObject("recovery").put("identity",JSONArray(listOf("newrun",target.conversation)))
        assertTrue(RecoveryPresentation.from(reused,target,true)!!.recorded)
    }
    @Test fun unknownMetricsStayUnknownAndUnknownStateRemainsVisible() {
        val raw = raw();val job = raw.getJSONObject("recovery")
        job.remove("attempt");job.put("state","custom_state").put("due_at","120")
        val view = RecoveryPresentation.from(raw,target,true)!!
        assertEquals("Attempts not reported",view.attempts)
        assertNull(view.dueAt)
        assertEquals("Custom state",view.status(0.0))
    }
}
