package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.async
import kotlinx.coroutines.CoroutineStart
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer

class SessionNotificationPreferencesTest {
    private val target = Target("computer","codex/project/session","run","conversation","workspace")
    private val signature = "a".repeat(64)
    private fun baseline() = JSONObject().put("reply_id","reply-old").put("attention_signature",signature)
        .put("optional_question_ids",JSONArray()).put("main_attention",JSONObject.NULL).put("child_attention",JSONObject())
    private fun preference(muted: Boolean = false, revision: Long = 2, suppressed: JSONObject = baseline()) = JSONObject()
        .put("conversation_id",target.conversation).put("muted",muted).put("revision",revision).put("suppressed",suppressed)
    private fun raw(muted: Boolean = false, suppressed: JSONObject = baseline()) = JSONObject().put("name",target.session)
        .put("run_id",target.run).put("conversation_id",target.conversation).put("state","running").put("tracked",true)
        .put("activity","idle").put("phase","idle").put("reply_id","reply-old").put("attention_signature",signature)
        .put("notification_preferences",preference(muted,suppressed=suppressed))
    private fun attention(id: String = "request-old", phase: String = "input", started: Any = 100) = JSONObject()
        .put("phase",phase).put("request_id",id).put("started_at",started)
    private fun session(raw: JSONObject) = Session(target,"Session","codex","idle","Project","",2,raw)
    private fun current(raw: JSONObject, kind: SessionAlertKind?, optional: List<String> = emptyList(), online: Boolean = true) =
        NotificationCurrent(NotificationSlot(target.connectionId,target.computerId,target.session),"run/conversation","Session","Computer",kind,"identity",true,
            online=online,conversation=target.conversation,optional=optional,suppressed=SessionNotificationPreferences.suppressed(raw,target,kind,optional))
    private fun plan(raw: JSONObject, kind: SessionAlertKind?, optional: List<String> = emptyList(), previous: NotificationState = NotificationState(bootstrap=false,settingsGeneration=0),
        eventKind: String = "attention", online: Boolean = true): NotificationPlan {
        val now = current(raw,kind,optional,online)
        return NotificationPolicy.plan(previous,listOf(now),listOf(now.slot to NotificationEvent(10,target.computerId,target.session,eventKind,1000.0)),
            NotificationPreferences(finished=true,optional=true),1000.0,active=setOf(now.slot.key))
    }

    @Test fun unknownMalformedAndWrongConversationNeverInventASharedPreference() {
        assertNull(SessionNotificationPreferences.parse(JSONObject(),target))
        assertNotNull(SessionNotificationPreferences.parse(raw(),target.copy(run="resumed")))
        assertNull(SessionNotificationPreferences.parse(raw(),target.copy(conversation="fork")))
        for(value in listOf<Any>("false",0,JSONObject.NULL)) {
            val valueRaw=raw().put("notification_preferences",preference().put("muted",value))
            assertNull(SessionNotificationPreferences.parse(valueRaw,target))
        }
        for(value in listOf<Any>(-1,2.0,"2",true)) assertNull(SessionNotificationPreferences.parse(raw().put("notification_preferences",preference().put("revision",value)),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("attention_signature","not-a-hash")),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("reply_id","reply\nprivate")),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("child_attention",JSONObject().put("child\u0001",attention()))),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("optional_question_ids",JSONArray(listOf("b","a")))),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("optional_question_ids",JSONArray(List(65) { "id-$it" }.sorted()))),target))
        val children=JSONObject();repeat(257) { children.put("child-$it",attention()) }
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("child_attention",children)),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("main_attention",attention(started="100").put("run_id",target.run))),target))
    }

    @Test fun transportPreservesTheDurableOuterIDWithoutAddingNativeControlFields() = runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("""{"request_id":"request","state":"queued"}"""));server.start()
            val connection=Connection(target.connectionId,"Fixture",server.url("/").toString().trimEnd('/'),"synthetic")
            val args=JSONObject().put("muted",true).put("expected_revision",2)
            RelayApi().submit(connection,target,SessionNotificationPreferences.operation,SessionNotificationPreferences.payload(target,args),"request")
            val request=server.takeRequest();val body=JSONObject(request.body.readUtf8())
            assertEquals("POST",request.method);assertEquals("/v1/requests",request.path)
            assertEquals("request",body.getString("request_id"));assertEquals(SessionNotificationPreferences.operation,body.getString("operation"))
            assertEquals(setOf("run_id","conversation_id","muted","expected_revision"),body.getJSONObject("payload").keys().asSequence().toSet())
            assertEquals(1,server.requestCount)
        }
    }

    @Test fun exactFourFieldPayloadAndStrictCASArgumentsUseOuterRequestIdentityOnly() {
        val args=SessionActionPolicies.arguments(SessionNotificationPreferences.operation,JSONObject().put("muted",true).put("expected_revision",2))
        val payload=SessionNotificationPreferences.payload(target,args)
        assertEquals(setOf("run_id","conversation_id","muted","expected_revision"),payload.keys().asSequence().toSet())
        assertEquals(target.run,payload.getString("run_id"));assertEquals(target.conversation,payload.getString("conversation_id"))
        for(value in listOf<Any>("true",1,JSONObject.NULL)) assertThrows(IllegalArgumentException::class.java) {
            SessionActionPolicies.arguments(SessionNotificationPreferences.operation,JSONObject().put("muted",value).put("expected_revision",2))
        }
        assertThrows(IllegalArgumentException::class.java) { SessionNotificationPreferences.arguments(JSONObject(args.toString()).put("request_id","extra")) }
        assertThrows(IllegalArgumentException::class.java) { SessionNotificationPreferences.arguments(JSONObject(args.toString()).put("expected_revision",2.0)) }
    }

    @Test fun receiptsConfirmOnlyExactCASOutcomeAndNeverOptimisticallyChangeTheCatalog() {
        val row=raw();val original=row.toString()
        val action=SessionAction("request",target,SessionNotificationPreferences.operation,JSONObject().put("muted",true).put("expected_revision",2).toString())
        fun receipt(applied: Boolean = true, revision: Long = 3) = JSONObject().put("request_id",action.requestId).put("state","completed").put("result",
            JSONObject().put("ok",true).put("applied",applied).put("run_id",target.run).put("conversation_id",target.conversation).put("preferences",preference(true,revision)))
        assertEquals("completed",SessionActionPolicies.result(action,receipt()).status)
        assertEquals("completed",SessionActionPolicies.result(action,receipt(false,2)).status)
        for(wrong in listOf(receipt().put("request_id","other"),receipt(false,3),receipt(true,2),
            receipt().apply { getJSONObject("result").put("run_id","restarted") },receipt().apply { getJSONObject("result").put("ok",false) },
            receipt().apply { getJSONObject("result").getJSONObject("preferences").put("muted",false) }))
            assertEquals("uncertain",SessionActionPolicies.result(action,wrong).status)
        assertEquals("failed",SessionActionPolicies.result(action,JSONObject().put("request_id","request").put("state","failed").put("error","conflict")).status)
        for(error in listOf("conflict","stale_identity","baseline_unavailable","not_found","capacity","revision_exhausted")) {
            val refused=JSONObject().put("request_id","request").put("state","completed").put("result",JSONObject().put("ok",false).put("error",error))
            val result=SessionActionPolicies.result(action,refused)
            assertEquals("failed",result.status);assertFalse(result.blocksSending);assertTrue(result.error.isNotBlank())
            assertEquals("uncertain",SessionActionPolicies.result(action,JSONObject(refused.toString()).put("request_id","other")).status)
        }
        assertEquals("uncertain",SessionActionPolicies.result(action,JSONObject().put("request_id","request").put("state","completed").put("result",JSONObject().put("ok",false).put("error","unknown"))).status)
        assertEquals("uncertain",action.recover().status)
        assertEquals(original,row.toString())
        assertEquals("",SessionActionPolicies.inspectReason(target,action.operation,JSONObject(action.arguments),row))
        row.getJSONObject("notification_preferences").put("revision",3)
        assertTrue(SessionActionPolicies.inspectReason(target,action.operation,JSONObject(action.arguments),row).contains("changed"))
    }

    @Test fun controlsRequireKnownABIAndLiveParentButKeepNativeFiltersUnreadAndPending() {
        val row=raw(true).put("phase","input").put("mobile_unread_reply",true).put("pending_question_count",2)
        val s=session(row);val original=row.toString()
        val facts=SessionMenuFacts(target,row,"running",false,true,setOf(SessionNotificationPreferences.operation))
        val item=SessionActionsMenu.items(facts).flatten().single { it.id==SessionNotificationPreferences.operation }
        assertTrue(item.enabled);assertEquals("Unmute notifications",item.label)
        assertEquals(SessionNotificationPreferences.unavailable,SessionActionsMenu.notificationReason(facts.copy(supported=emptySet())))
        assertTrue(SessionActionsMenu.notificationReason(facts.copy(target=target.copy(archiveId="archive"))).isNotEmpty())
        assertTrue(SessionActionsMenu.notificationReason(facts.copy(target=target.copy(agentId="child"))).isNotEmpty())
        assertTrue(SessionFilters.needsAttention(s,true));assertTrue(SessionFilters.active(s,true))
        assertEquals(DesktopBadgeKind.Neutral,DesktopSessionPalette.badge(s,true).kind)
        assertTrue(SessionFilters.visibleUnreadReply(s,true));assertEquals(2,s.pendingCount)
        assertEquals(DesktopBadgeKind.Working,DesktopSessionPalette.badge(session(raw(true).put("activity","busy")),true).kind)
        assertEquals(DesktopBadgeKind.Error,DesktopSessionPalette.badge(session(raw(true).put("phase","error")),true).kind)
        assertEquals(original,row.toString())
    }

    @Test fun mutedAndOfflineThroughUnmuteBaselineWithdrawCardsWithoutAnyReplayOrOverflow() {
        val muted=raw(true).put("phase","input").put("attention_id","request-old")
        val slot=current(muted,SessionAlertKind.Input).slot
        val old=NotificationState(bootstrap=false,settingsGeneration=0,records=listOf(NotificationRecord(slot,"run/conversation",900.0,postedFingerprint="old",postedKind=SessionAlertKind.Input)))
        val quiet=plan(muted,SessionAlertKind.Input,previous=old,online=false)
        assertTrue(quiet.desired.isEmpty());assertTrue(quiet.retained.isEmpty());assertFalse(quiet.overflow)
        val baseline=baseline().put("main_attention",attention().put("run_id",target.run))
        val resumed=raw(false,baseline).put("phase","input").put("attention_id","request-old").put("attention_started",9000)
        // No device-local muted state was observed: the host's persisted baseline is sufficient.
        assertTrue(plan(resumed,SessionAlertKind.Input).desired.isEmpty())
        assertTrue(plan(raw(false,baseline),null,eventKind="completed").desired.isEmpty())
        val newReply=raw(false,baseline).put("reply_id","reply-new")
        assertFalse(SessionNotificationPreferences.suppressed(newReply,target,null,emptyList()))
        val newRequest=JSONObject(resumed.toString()).put("attention_id","request-new")
        assertTrue(plan(newRequest,SessionAlertKind.Input).desired.single().fresh)
    }

    @Test fun independentMainSiblingOptionalAndTimestampBaselinesNeverHideNewIdentities() {
        val baseline=baseline().put("main_attention",attention().put("run_id",target.run))
            .put("optional_question_ids",JSONArray(listOf("optional-old"))).put("child_attention",JSONObject().put("child-a",attention("child-request","approval")))
        val row=raw(false,baseline).put("phase","input").put("attention_id","request-old")
            .put("subagents",JSONObject().put("child-a",JSONObject().put("state","approval").put("attention_id","child-request").put("attention_started",99999)))
        assertTrue(plan(row,SessionAlertKind.Input).desired.isEmpty())
        val sibling=JSONObject(row.toString()).apply { getJSONObject("subagents").put("child-b",JSONObject().put("state","input").put("question_id","new-sibling")) }
        assertTrue(plan(sibling,SessionAlertKind.Input).desired.single().fresh)
        assertTrue(plan(raw(false,baseline),SessionAlertKind.Optional,listOf("optional-old")).desired.isEmpty())
        assertTrue(plan(raw(false,baseline),SessionAlertKind.Optional,listOf("optional-old","optional-new")).desired.single().fresh)
        val noId=baseline().put("main_attention",attention(id="",started=100).put("run_id",target.run))
        val fallback=raw(false,noId).put("phase","input").put("attention_started",100)
        assertTrue(plan(fallback,SessionAlertKind.Input).desired.isEmpty())
        fallback.put("attention_started",101)
        assertTrue(plan(fallback,SessionAlertKind.Input).desired.single().fresh)
        val unknown=raw(false,baseline().put("main_attention",attention(id="",started=JSONObject.NULL).put("run_id",target.run))).put("phase","input")
        assertNull(SessionNotificationPreferences.parse(unknown,target))
        assertFalse(SessionNotificationPreferences.suppressed(unknown,target,SessionAlertKind.Input,emptyList()))
        val childClock=baseline().put("child_attention",JSONObject().put("child-a",attention(id="",phase="approval",started=100)))
        val child=raw(false,childClock).put("subagents",JSONObject().put("child-a",JSONObject().put("state","approval").put("last_event_at",100)))
        assertTrue(plan(child,SessionAlertKind.Input).desired.isEmpty())
        child.getJSONObject("subagents").getJSONObject("child-a").put("last_event_at",101)
        assertTrue(plan(child,SessionAlertKind.Input).desired.single().fresh)
        val decoyRead=raw(false,baseline()).put("read",JSONObject().put("reply_id","reply-new"))
        assertTrue(SessionNotificationPreferences.suppressed(decoyRead,target,null,emptyList()))
    }

    @Test fun preferenceEventsOnlyRefreshAndCannotBecomeAnAudibleTransition() {
        val row=raw().put("phase","input").put("attention_id","new-request")
        assertFalse(plan(row,SessionAlertKind.Input,eventKind="notification_preferences").desired.single().fresh)
    }

    private fun confirmed(muted:Boolean,revision:Long,bound:Target=target,suppressed:JSONObject=baseline()):SessionAction {
        val action=SessionAction("receipt-$revision-${bound.computerId}-${bound.conversation}",bound,SessionNotificationPreferences.operation,
            JSONObject().put("muted",muted).put("expected_revision",revision-1).toString())
        return SessionActionPolicies.result(action,JSONObject().put("request_id",action.requestId).put("state","completed").put("result",
            JSONObject().put("ok",true).put("applied",true).put("run_id",bound.run).put("conversation_id",bound.conversation)
                .put("preferences",preference(muted,revision,suppressed).put("conversation_id",bound.conversation))))
    }

    @Test fun heldOldCatalogAndDetailCannotUndoAConfirmedMuteButHigherHostRevisionCan() = runBlocking {
        val bound=target.copy(computerId="22222222-2222-4222-8222-222222222222")
        val connection=Connection(target.connectionId,"Fixture","https://relay.example.test","synthetic")
        val old=raw().put("phase","input").put("attention_id","old-input")
        val machine=JSONObject().put("id",bound.computerId).put("name","Computer").put("online",true)
            .put("snapshot",JSONObject().put("sessions",JSONArray(listOf(old))))
        val held=CompletableDeferred<List<JSONObject>>()
        var actions=emptyList<SessionAction>()
        val late=async(start=CoroutineStart.UNDISPATCHED) { val records=held.await();SessionCatalogParser.parse(connection,records,actions=actions).projects.sessions.single() }
        val oldNotice=NotificationCatalogParser.parse(connection,listOf(machine),emptyList()).sessions.single()
        actions=listOf(confirmed(true,3,bound))
        held.complete(listOf(machine))
        val row=late.await()
        assertTrue(SessionNotificationPreferences.muted(row))
        assertEquals(3L,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(old,bound,listOf(row.raw),actions),bound)!!.revision)
        assertFalse(SessionNotificationPreferences.deliveryAllowed(oldNotice,actions))
        val renamed=bound.copy(session="codex/project/renamed",run="resumed")
        val conflicting=raw().put("notification_preferences",preference(false,3))
        assertTrue(SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(conflicting,renamed,listOf(row.raw),actions),renamed)!!.muted)
        val higher=raw().put("notification_preferences",preference(false,4))
        assertFalse(SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(higher,renamed,listOf(row.raw),actions),renamed)!!.muted)
        assertNull(SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(JSONObject(),bound.copy(conversation="fork"),emptyList(),actions),"fork"))
        val freshNotice=oldNotice.copy(notificationPreference=SessionNotificationPreferences.parse(higher,bound))
        assertTrue(SessionNotificationPreferences.deliveryAllowed(freshNotice,actions))
        val updated=NotificationCatalogParser.parse(connection,listOf(machine),emptyList(),actions).sessions.single()
        assertTrue(updated.suppressed)
    }

    @Test fun confirmedUnmuteBaselineBlocksOfflineReplayAndQueuedOlderNotices() {
        val old=raw().put("phase","input").put("attention_id","request-old")
        val muted=confirmed(true,3)
        val suppressed=baseline().put("main_attention",attention().put("run_id",target.run))
        val unmuted=confirmed(false,4,suppressed=suppressed)
        val actions=listOf(muted,unmuted)
        val merged=SessionNotificationPreferences.mergeDetail(old,target,emptyList(),actions)
        assertFalse(SessionNotificationPreferences.parse(merged,target)!!.muted)
        assertTrue(plan(merged,SessionAlertKind.Input).desired.isEmpty())
        assertTrue(plan(merged,null,eventKind="completed").desired.isEmpty())
        val queued=current(old,SessionAlertKind.Input).copy(notificationPreference=SessionNotificationPreferences.parse(old,target))
        assertFalse(SessionNotificationPreferences.deliveryAllowed(queued,actions))
        merged.put("attention_id","new-input")
        assertTrue(plan(merged,SessionAlertKind.Input).desired.single().fresh)
    }

    @Test fun proofHistoryKeepsHighestRevisionAndOneMiBWithoutLosingReceipts() {
        val high=confirmed(true,5);val older=confirmed(false,3)
        var state=MessageState().action(high).action(older)
        assertTrue(state.sessionActions.first().confirmedNotificationPreference.isNotEmpty())
        assertTrue(state.sessionActions.last().confirmedNotificationPreference.isEmpty())
        val invalid=confirmed(false,99).copy(requestId="failed-proof",status="failed")
        val malformed=high.copy(requestId="malformed-proof",confirmedNotificationPreference="{broken")
        val filtered=state.action(invalid).action(malformed)
        assertTrue(filtered.sessionActions.takeLast(2).all { it.confirmedNotificationPreference.isEmpty() })
        assertEquals(5L,SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(raw(),target,emptyList(),filtered.sessionActions),target)!!.revision)
        val large=baseline().put("main_attention",attention("x".repeat(60000)).put("run_id",target.run))
            .put("child_attention",JSONObject().put("one",attention("x".repeat(60000))).put("two",attention("x".repeat(60000))).put("three",attention("x".repeat(60000))))
        repeat(8) { index -> state=state.action(confirmed(true,6,target.copy(conversation="conversation-$index"),large)) }
        assertEquals(10,state.sessionActions.size)
        val proofs=state.sessionActions.map { it.confirmedNotificationPreference }.filter(String::isNotEmpty)
        assertTrue(proofs.size in 1..5)
        assertTrue(proofs.sumOf { JSONObject.quote(it).toByteArray(Charsets.UTF_8).size } <= 1024*1024)
        assertTrue(state.sessionActions.all { it.status=="completed" && !it.blocksSending })
        val recovered=MessageCodec.state(MessageCodec.stateJson(state))
        assertEquals(state.sessionActions.map { it.requestId },recovered.sessionActions.map { it.requestId })
        assertEquals(proofs,recovered.sessionActions.map { it.confirmedNotificationPreference }.filter(String::isNotEmpty))
    }

    @Test fun baselineBoundsUseUTF8AndAllowOnlyLFInDeepSeekCompositeRequestIDs() {
        val composite="question-one\nquestion-two"
        val suppressed=baseline().put("main_attention",attention(composite).put("run_id",target.run))
        val row=raw(false,suppressed).put("phase","input").put("attention_id",composite)
        assertNotNull(SessionNotificationPreferences.parse(row,target))
        assertTrue(SessionNotificationPreferences.suppressed(row,target,SessionAlertKind.Input,emptyList()))
        row.put("attention_id","question-two\nquestion-one")
        assertFalse(SessionNotificationPreferences.suppressed(row,target,SessionAlertKind.Input,emptyList()))
        for(id in listOf("x\rnext","x\tnext","x\u007fnext","é".repeat(32769)))
            assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("main_attention",attention(id).put("run_id",target.run))),target))
        assertNotNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("main_attention",attention("é".repeat(32768)).put("run_id",target.run))),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("reply_id","é".repeat(129))),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("main_attention",attention().put("phase","é".repeat(129)).put("run_id",target.run))),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("child_attention",JSONObject().put("child",attention("",started=JSONObject.NULL)))),target))
    }

    @Test fun equalOpaqueConversationIDsFromDifferentProvidersNeverShareProofOrMute() {
        val proof=confirmed(true,3)
        val foreign=target.copy(session="claude/project/session")
        assertNotEquals(SessionNotificationPreferences.key(target),SessionNotificationPreferences.key(foreign))
        assertFalse(SessionNotificationPreferences.sameConversation(target,foreign))
        val original=session(raw(true).put("notification_preferences",preference(true,3)))
        val other=session(raw()).copy(target=foreign)
        val merged=SessionNotificationPreferences.mergeRows(listOf(other),listOf(original),listOf(proof)).single()
        assertFalse(SessionNotificationPreferences.muted(merged))
        assertFalse(SessionNotificationPreferences.parse(SessionNotificationPreferences.mergeDetail(other.raw,foreign,emptyList(),listOf(proof)),foreign)!!.muted)
        val notice=current(raw(),SessionAlertKind.Input).copy(slot=NotificationSlot(foreign.connectionId,foreign.computerId,foreign.session))
        assertTrue(SessionNotificationPreferences.deliveryAllowed(notice,listOf(proof)))
        assertTrue(SessionNotificationPreferences.sameConversation(target,target.copy(run="resumed",session="codex/other/renamed")))
    }

    @Test fun completePreferenceRejectsOversizeASCIIEscapingAndUnknownShapes() {
        val request="é".repeat(32768)
        val one=baseline().put("main_attention",attention(request).put("run_id",target.run))
        assertNotNull(SessionNotificationPreferences.parse(raw(suppressed=one),target))
        val oversized=JSONObject(one.toString()).put("child_attention",JSONObject().put("child",attention(request)))
        val value=raw(suppressed=oversized)
        assertTrue(value.getJSONObject("notification_preferences").toString().toByteArray(Charsets.UTF_8).size<384*1024)
        assertNull(SessionNotificationPreferences.parse(value,target))
        assertNull(SessionNotificationPreferences.parse(raw().put("notification_preferences",preference().put("future",true)),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("future",true)),target))
        assertNull(SessionNotificationPreferences.parse(raw(suppressed=baseline().put("main_attention",attention().put("run_id",target.run).put("future",true))),target))
        val action=confirmed(true,3).copy(status="sending",confirmedNotificationPreference="")
        val receipt=JSONObject().put("request_id",action.requestId).put("state","completed").put("result",JSONObject()
            .put("ok",true).put("applied",true).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("preferences",preference(true,3,oversized)))
        assertEquals("uncertain",SessionActionPolicies.result(action,receipt).status)
        assertTrue(SessionActionPolicies.result(action,receipt).confirmedNotificationPreference.isEmpty())
    }

    private fun parsedCurrent(value:JSONObject):NotificationCurrent {
        val connection=Connection(target.connectionId,"Fixture","https://relay.example.test","synthetic")
        val machine=JSONObject().put("id","22222222-2222-4222-8222-222222222222").put("name","Computer").put("online",true)
            .put("snapshot",JSONObject().put("sessions",JSONArray(listOf(value))))
        return NotificationCatalogParser.parse(connection,listOf(machine),emptyList()).sessions.single()
    }
    private fun catalogPlan(raw:JSONObject,previous:NotificationState=NotificationState(bootstrap=false,settingsGeneration=0),at:Double=1000.0):NotificationPlan {
        val current=parsedCurrent(raw)
        return NotificationPolicy.plan(previous,listOf(current),listOf(current.slot to NotificationEvent(at.toLong(),current.slot.computer,current.slot.session,"attention",at)),
            NotificationPreferences(finished=true,optional=true),at)
    }

    @Test fun realCatalogFingerprintAnnouncesEachNewFallbackClockButIgnoresClockForAnOwnedID() {
        val baseline=baseline().put("main_attention",attention("",started=100).put("run_id",target.run))
        val old=raw(suppressed=baseline).put("phase","input").put("attention_started",100)
        val first=catalogPlan(old);assertTrue(first.desired.isEmpty())
        val next=JSONObject(old.toString()).put("attention_started",101)
        val second=catalogPlan(next,first.state,1001.0);assertTrue(second.desired.single().fresh)
        val third=catalogPlan(JSONObject(next.toString()).put("attention_started",102),second.state,1002.0)
        assertTrue(third.desired.single().fresh)
        val identified=JSONObject(next.toString()).put("attention_id","new-owned-id")
        val owned=catalogPlan(identified,third.state,1003.0);assertTrue(owned.desired.single().fresh)
        val clockOnly=catalogPlan(JSONObject(identified.toString()).put("attention_started",999),owned.state,1004.0)
        assertFalse(clockOnly.desired.single().fresh)
    }

    @Test fun realCatalogKeepsCoveredSiblingQuietAfterRemovalsAndAnnouncesNewChildOrOptional() {
        fun child(id:String)=JSONObject().put("state","approval").put("attention_id",id).put("attention_started",100)
        fun optional(id:String)=JSONObject().put("source","codex_async").put("optional",true).put("question_id",id).put("question_hash","hash-$id")
        val suppressed=baseline().put("child_attention",JSONObject().put("a",attention("a",phase="approval")).put("b",attention("b",phase="approval")))
            .put("optional_question_ids",JSONArray(listOf("optional-old")))
        val original=raw(suppressed=suppressed).put("subagents",JSONObject().put("a",child("a")).put("b",child("b")))
            .put("mobile_attention",JSONArray(listOf(optional("optional-old"))))
        val first=catalogPlan(original);assertTrue(first.desired.isEmpty())
        val remaining=JSONObject(original.toString()).put("subagents",JSONObject().put("b",child("b"))).put("attention_signature","b".repeat(64))
        val second=catalogPlan(remaining,first.state,1001.0);assertTrue(second.desired.isEmpty())
        val removedOptional=JSONObject(remaining.toString()).put("mobile_attention",JSONArray()).put("attention_signature","c".repeat(64))
        val third=catalogPlan(removedOptional,second.state,1002.0);assertTrue(third.desired.isEmpty())
        val newChild=JSONObject(removedOptional.toString()).put("subagents",JSONObject().put("b",child("b")).put("new",child("new")))
        val fourth=catalogPlan(newChild,third.state,1003.0);assertTrue(fourth.desired.single().fresh)
        val changedChild=JSONObject(newChild.toString());changedChild.getJSONObject("subagents").getJSONObject("new").put("attention_id","new-again")
        val fifth=catalogPlan(changedChild,fourth.state,1004.0);assertTrue(fifth.desired.single().fresh)
        val newOptional=JSONObject(removedOptional.toString()).put("mobile_attention",JSONArray(listOf(optional("optional-new"))))
        val sixth=catalogPlan(newOptional,third.state,1005.0)
        assertEquals(SessionAlertKind.Optional,sixth.desired.single().kind);assertTrue(sixth.desired.single().fresh)
        val unknownMain=JSONObject(removedOptional.toString()).put("pending_question_count",1)
        assertFalse(parsedCurrent(unknownMain).suppressed)
        assertTrue(catalogPlan(unknownMain,third.state,1006.0).desired.single().fresh)
    }

    @Test fun pendingOrUncertainMuteBlocksQueuedDeliveryAndKnownNoWriteFailureReleasesIt() {
        val now=parsedCurrent(raw())
        val bound=target.copy(computerId=now.slot.computer)
        val pending=SessionAction("pending",bound,SessionNotificationPreferences.operation,JSONObject().put("muted",true).put("expected_revision",2).toString())
        assertFalse(SessionNotificationPreferences.deliveryAllowed(now,listOf(pending)))
        assertFalse(SessionNotificationPreferences.deliveryAllowed(now,listOf(pending.recover())))
        val refused=SessionActionPolicies.result(pending,JSONObject().put("request_id","pending").put("state","completed").put("result",JSONObject().put("ok",false).put("error","conflict")))
        assertEquals("failed",refused.status);assertFalse(refused.blocksSending)
        assertTrue(SessionNotificationPreferences.deliveryAllowed(now,listOf(refused)))
        assertTrue(SessionNotificationPreferences.known(actions=listOf(pending)).isEmpty())
    }

    @Test fun coveredMainCannotHideNewRecoveryOrUnaccountedBlockingQuestions() {
        val suppressed=baseline().put("main_attention",attention("main-a").put("run_id",target.run))
        val original=raw(suppressed=suppressed).put("phase","input").put("attention_id","main-a").put("attention_started",100)
        val first=catalogPlan(original);assertTrue(first.desired.isEmpty())
        for(state in listOf("exhausted","blocked","uncertain")) {
            val recovery=JSONObject(original.toString()).put("recovery",JSONObject().put("state",state).put("request_id","recovery-b").put("error","new failure"))
            assertFalse(parsedCurrent(recovery).suppressed)
            val next=catalogPlan(recovery,first.state,1001.0)
            assertEquals(SessionAlertKind.Error,next.desired.single().kind);assertTrue(next.desired.single().fresh)
        }
        val unknown=JSONObject(original.toString()).put("pending_question_count",1).put("attention_signature","b".repeat(64))
        assertFalse(parsedCurrent(unknown).suppressed)
        assertTrue(catalogPlan(unknown,first.state,1002.0).desired.single().fresh)
        val question=JSONObject().put("question_id","new-blocking").put("question_hash","new-hash").put("optional",false)
        val blocking=JSONObject(original.toString()).put("mobile_attention",JSONArray(listOf(question))).put("attention_signature","c".repeat(64))
        assertFalse(parsedCurrent(blocking).suppressed)
        assertTrue(catalogPlan(blocking,first.state,1003.0).desired.single().fresh)
    }
}
