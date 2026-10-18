package app.zerus.mobile

import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ScheduledSettingsRearmTest {
    private val target=Target("computer","codex/fixture/settings","run","conversation","workspace")
    private val pending="00000000-0000-4000-8000-000000000071"
    private val original=SessionAction("original",target,"settings",JSONObject().put("model","synthetic").toString(),status="scheduled",pendingId=pending,applyAttemptId="child")
    private val child=SessionAction("child",target,"settings",JSONObject().put("model","synthetic").put("expected_pending_id",pending).toString(),status="scheduled",pendingId=pending)
    private val next=SessionAction("next",target,"settings",JSONObject(child.arguments).put("effort","high").toString())
    private fun consumed():ScheduledSettingsRetry.ConsumedGrant {
        val tracker=ScheduledSettingsRetry()
        val result=JSONObject().put("request_id",child.requestId).put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("status","scheduled").put("apply_when","ready").put("pending_settings_id",pending).put("model","synthetic").put("effort","high")
        assertTrue(tracker.register(original,child,1,result))
        val raw=JSONObject().put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("pending_settings_id",pending).put("pending_model","synthetic").put("pending_effort","high")
            .put("phase","idle").put("activity","idle").put("settings_change_supported",true).put("settings_apply_when","ready")
        assertNull(tracker.inspect(original,child,target,raw,2,3,true,true))
        val grant=tracker.inspect(original,child,target,raw.put("settings_apply_when","now"),4,5,true,true)!!
        return tracker.consume(grant)!!
    }
    @Test fun consumedProofAtomicallyReplacesLatestChildAndNeverReusesOriginalAttempt() {
        val proof=consumed();val state=MessageState(sessionActions=listOf(original,child))
        val applied=ScheduledSettingsAttempt.begin(state,original,next,proof)
        assertEquals("next",applied.sessionActions.first { it.requestId==original.requestId }.applyAttemptId)
        assertEquals(next,applied.sessionActions.first { it.requestId==next.requestId })
        assertEquals(original.arguments,applied.sessionActions.first { it.requestId==original.requestId }.arguments)
        assertThrows(IllegalStateException::class.java) { ScheduledSettingsAttempt.begin(applied,original,next.copy(requestId="duplicate"),proof) }
    }
    @Test fun staleDurableRelationChangedNativePinsOrTargetCannotUseConsumedGrant() {
        val proof=consumed();val state=MessageState(sessionActions=listOf(original,child))
        for(changed in listOf(original.copy(status="applied"),original.copy(applyAttemptId="other"),original.copy(arguments="{\"model\":\"changed\"}"))) {
            assertThrows(IllegalStateException::class.java) { ScheduledSettingsAttempt.begin(state.copy(sessionActions=listOf(changed,child)),original,next,proof) }
        }
        assertThrows(IllegalStateException::class.java) { ScheduledSettingsAttempt.begin(state.copy(sessionActions=listOf(original,child.copy(status="uncertain"))),original,next,proof) }
        for(changed in listOf(next.copy(target=target.copy(run="new")),next.copy(arguments=JSONObject(next.arguments).put("effort","low").toString()),next.copy(arguments=JSONObject(next.arguments).put("expected_pending_id","changed").toString()),next.copy(arguments=JSONObject(child.arguments).toString()))) {
            assertThrows(IllegalArgumentException::class.java) { ScheduledSettingsAttempt.begin(state,original,changed,proof) }
        }
    }
    @Test fun suspendedRearmPreservesNewerComposerAndLatestDurableReference()=runBlocking {
        val proof=consumed();val draft=Draft(target,"Original composition",attachments=emptyList())
        val entered=CompletableDeferred<Unit>();val release=CompletableDeferred<Unit>()
        val scope=CoroutineScope(SupervisorJob()+Dispatchers.Default)
        val writer=OrderedStatePersistence(MessageState(drafts=listOf(draft),sessionActions=listOf(original,child)),scope,
            { entered.complete(Unit);release.await() },uiDispatcher=Dispatchers.Unconfined)
        val writing=async { writer.durable({ ScheduledSettingsAttempt.begin(it,original,next,proof) }) }
        entered.await();writer.edit { MessageChanges.text(it,draft.copy(text="Newest unsent text",revision="new")) }
        release.complete(Unit);writing.await();writer.flush()
        assertEquals("Newest unsent text",writer.state.value.drafts.single().text)
        assertEquals("next",writer.state.value.sessionActions.first { it.requestId==original.requestId }.applyAttemptId)
        assertEquals(next,writer.state.value.sessionActions.first { it.requestId==next.requestId })
        assertTrue(writer.state.value.outgoing.isEmpty());scope.cancel()
    }
}
