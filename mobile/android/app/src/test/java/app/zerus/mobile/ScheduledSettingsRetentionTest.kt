package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ScheduledSettingsRetentionTest {
    private val target=Target("computer","session","run","conversation","workspace")
    private val pending="00000000-0000-4000-8000-000000000071"
    private val original=SessionAction("original",target,"settings",JSONObject().put("model","synthetic").toString(),status="scheduled",pendingId=pending,applyAttemptId="latest")
    private fun child(id:String)=SessionAction(id,target,"settings",JSONObject().put("model","synthetic").put("expected_pending_id",pending).toString(),status="scheduled",createdAt=1,pendingId=pending)
    @Test fun oldConfirmedScheduledChildrenAreBoundedButLatestProofRemains() {
        var state=MessageState(sessionActions=listOf(original,child("latest")))
        repeat(150) { state=state.action(child("old-$it")) }
        assertEquals(102,state.sessionActions.size)
        assertEquals(original,state.sessionActions.first { it.requestId==original.requestId })
        assertEquals(child("latest"),state.sessionActions.first { it.requestId=="latest" })
        assertEquals((50 until 150).map { "old-$it" },state.sessionActions.filter { it.requestId.startsWith("old-") }.map { it.requestId })
    }
    @Test fun olderChildRejoinsTerminalPoolWhenOriginalReferencesNewChild() {
        var state=MessageState(sessionActions=listOf(original,child("latest")))
        state=state.action(original.copy(applyAttemptId="new")).action(child("new"))
        repeat(150) { state=state.action(child("old-$it")) }
        assertFalse(state.sessionActions.any { it.requestId=="latest" })
        assertTrue(state.sessionActions.any { it.requestId=="new" })
        assertEquals(102,state.sessionActions.size)
    }
    @Test fun initialConsentUncertainSendingOtherAndMalformedLegacyKeepOldSemantics() {
        val values=listOf(original.copy(applyAttemptId=""),child("sending").copy(status="sending"),child("uncertain").copy(status="uncertain"),
            child("other").copy(operation="recovery_action"),child("missing").copy(arguments="{}"),child("empty").copy(arguments="{\"expected_pending_id\":\"\"}"),
            child("bool").copy(arguments="{\"expected_pending_id\":true}"),child("malformed").copy(arguments="{"),
            child("invalid-id").copy(arguments=JSONObject().put("model","synthetic").put("expected_pending_id","invalid").toString()),
            child("missing-choice").copy(arguments=JSONObject().put("expected_pending_id",pending).toString()),
            child("extra").copy(arguments=JSONObject(child("extra").arguments).put("unknown",true).toString()),
            child("invalid-choice").copy(arguments=JSONObject(child("invalid-choice").arguments).put("model",true).toString()))
        var state=MessageState(sessionActions=values)
        repeat(150) { state=state.action(child("old-$it")) }
        assertTrue(state.sessionActions.containsAll(values))
        assertEquals(values.size+100,state.sessionActions.size)
    }
    @Test fun originalApplyProofSurvivesPrunedOldChildAndCannotRestartWithoutGrant() {
        var state=MessageState(sessionActions=listOf(original,child("latest")))
        repeat(150) { state=state.action(child("old-$it").copy(status="completed")) }
        assertEquals("latest",state.sessionActions.first { it.requestId==original.requestId }.applyAttemptId)
        assertThrows(IllegalStateException::class.java) { ScheduledSettingsAttempt.begin(state,original,child("new").copy(status="sending")) }
    }
}
