package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ConversationPagingTest {
    private val target=Target("computer","codex/project/chat","run","conversation","workspace")
    private fun event(i:Int)=Event("history:$i","AgentMessage","reply $i","",i.toDouble(),nativeType="AgentMessage",historyId="$i",historyCursor="opaque:$i",historyEpoch="epoch",incomingSeq=i.toLong())
    private fun page(start:Int,end:Int,before:Boolean=true,after:Boolean=false)=HistoryPage(target,"epoch",(start..end).map(::event),"opaque:$start","opaque:$end",before,after,1000,1000,true,false,false,JSONObject())
    @Test fun emptyCanonicalHeadStillFetchesFirstPendingVoiceWithoutMovingAnOlderWindow() {
        val empty=HistoryWindow.from(page(1,0,before=false).copy(before=null,after=null))
        val pending=event(1).copy(id="voice:fixture",role="You",nativeType="UserMessage",source="hgs_voice",
            historyId="",historyCursor="",historyEpoch="",incomingSeq=null)
        // Inspection cannot invent canonical order for a voice row before native submission.
        val inspected=empty.inspection(listOf(pending))
        assertTrue(inspected.events.isEmpty())
        assertEquals("" to "",HistoryRequests.refreshAtHead(inspected))
        val canonical=HistoryWindow.from(page(1,1,before=false).copy(events=listOf(pending.copy(historyId="1",historyCursor="opaque:1",historyEpoch="epoch"))))
        assertEquals("after" to "opaque:1",HistoryRequests.refreshAtHead(canonical))
        assertEquals("after" to "opaque:1",HistoryRequests.refreshAtHead(canonical.copy(events=canonical.events+pending.copy(id="voice:next"))))
        assertNull(HistoryRequests.refreshAtHead(canonical.copy(atHead=false,hasAfter=true)))
    }
    @Test fun prependKeepsOlderPageAndAnchorInsteadOfEvictingIt() {
        val current=HistoryWindow.from(page(501,1000))
        val older=current.merge(page(401,500,after=true),"before")
        assertEquals(500,older.events.size)
        assertEquals("history:401",older.events.first().id)
        assertTrue(older.events.any { it.id=="history:550" })
        assertTrue(older.hasAfter);assertFalse(older.atHead)
        val newer=older.merge(page(901,1000),"after")
        assertEquals("history:1000",newer.events.last().id);assertTrue(newer.atHead)
    }
    @Test fun equalTimestampPagesKeepCanonicalOrderAcrossOwnAndIncomingRows() {
        fun row(i:Int)=event(i).copy(at=1.0,role=if(i%2==0) "You" else "AgentMessage")
        val old=HistoryWindow.from(page(3,4).copy(events=listOf(row(3),row(4))))
        val before=old.merge(page(1,2,after=true).copy(events=listOf(row(1),row(2))),"before")
        assertEquals(listOf("history:1","history:2","history:3","history:4"),before.events.map { it.id })
        val after=before.merge(page(5,6).copy(events=listOf(row(5),row(6))),"after")
        assertEquals((1..6).map { "history:$it" },after.events.map { it.id })
    }
    @Test fun smallPrependRetainsHeadCoverageAndNewIncomingInspectionStillAdvances() {
        val current=HistoryWindow.from(page(101,200))
        val prepended=current.merge(page(51,100,after=true),"before")
        assertFalse(prepended.hasAfter);assertTrue(prepended.atHead)
        val advanced=prepended.inspection(listOf(event(201).copy(id="journal:journal:AgentMessage:native:201",historyId="",historyCursor="",historyEpoch="",incomingSeq=null)),listOf(event(200).copy(id="journal:journal:AgentMessage:native:200")))
        assertEquals("journal:journal:AgentMessage:native:201",advanced.events.last().id);assertTrue(advanced.atHead)
        assertTrue(advanced.events.any { it.id=="history:51" })
    }
    @Test fun foreignTargetOrEpochCannotMixHistory() {
        val window=HistoryWindow.from(page(1,4))
        assertThrows(IllegalArgumentException::class.java) { window.merge(page(5,8).copy(target=target.copy(conversation="other")),"after") }
        assertThrows(IllegalArgumentException::class.java) { window.merge(page(5,8).copy(epoch="other"),"after") }
    }
    @Test fun actualVisibleCanonicalRowMarksEarlierIncomingReadAndOlderBackfillDoesNotAddUnread() {
        var state=ConversationReadState().head(target,"epoch",1000,1000,true)
        assertTrue(ConversationReadPolicies.unread(state.record(target),event(500)))
        state=state.visible(target,listOf(event(700)))
        assertEquals(700,state.record(target)!!.readThrough)
        assertFalse(ConversationReadPolicies.unread(state.record(target),event(500)))
        assertTrue(ConversationReadPolicies.unread(state.record(target),event(701)))
        state=state.visible(target,listOf(event(50)))
        assertEquals(700,state.record(target)!!.readThrough)
        assertEquals(300,state.record(target)!!.headIncoming!!-state.record(target)!!.readThrough)
    }
    @Test fun headOnlyProbeAfterMarkReadDoesNotAdvanceWatermarkForUnseenReply() {
        val initial=ConversationReadState().head(target,"epoch",600,600,true)
        val marked=ReadAttentionPolicies.mark(initial,ReadAttentionEvidence(target,"evidence","epoch",600,true,emptySet()))
        val latest=event(602).copy(incomingSeq=602,historyEpoch="epoch")
        val observed=marked.head(target,"epoch",602,602,true)
        val after=observed.visible(target,listOf(latest).filter { it.id in observed.record(target)!!.seen })
        val restarted=ConversationReadPolicies.decode(ConversationReadPolicies.encode(after)).record(target)!!
        assertEquals(600L,restarted.readThrough)
        assertEquals(2L,restarted.headIncoming!!-restarted.readThrough)
        assertTrue(ConversationReadPolicies.unread(restarted,latest))
    }
    @Test fun staleHeadRetainsKnownUnreadLowerBoundWithoutDoubleCountingLoadedRows() {
        val record=ConversationReadRecord(target,epoch="epoch",readThrough=600,headIncoming=602,headTotal=602,headComplete=true)
        assertEquals(2,ConversationReadPolicies.lowerBound(record,listOf(event(599))))
        assertEquals(2,ConversationReadPolicies.lowerBound(record,listOf(event(601),event(602))))
        assertEquals(3,ConversationReadPolicies.lowerBound(record,listOf(event(601),event(602),event(603))))
        assertEquals(0,ConversationReadPolicies.lowerBound(record.copy(headIncoming=null),listOf(event(599))))
    }
    @Test fun cachedCanonicalRowsRetainReadWatermarkWithoutClaimingCurrentCompleteTotal() {
        val read=ConversationReadState().head(target,"epoch",600,600,true).visible(target,listOf(event(600)))
        val offline=read.head(target,"epoch",null,null,false)
        val record=offline.record(target)!!
        assertFalse(record.headComplete);assertEquals(600L,record.headIncoming)
        assertFalse(ConversationReadPolicies.unread(record,event(599)))
        assertFalse(ConversationReadPolicies.unread(record,event(600)))
        assertTrue(ConversationReadPolicies.unread(record,event(601)))
        val viewed=offline.visible(target,listOf(event(601))).record(target)!!
        assertEquals(601L,viewed.readThrough)
        assertFalse(ConversationReadPolicies.unread(viewed,event(601)))
        assertFalse(viewed.headComplete)
        assertEquals(0,ConversationReadPolicies.lowerBound(viewed,null))
        val changed=offline.head(target,"other",null,null,false).record(target)!!
        assertEquals(0L,changed.readThrough);assertNull(changed.headIncoming)
    }
    @Test fun openingAndEncodingDoesNotAcknowledgeAndCursorSurvivesRestart() {
        val state=ConversationReadState().head(target,"epoch",1000,1000,true).viewport(target,"history:500",23,"opaque:500","epoch")
        val restored=ConversationReadPolicies.decode(ConversationReadPolicies.encode(state))
        assertEquals(0,restored.record(target)!!.readThrough)
        assertEquals(SavedConversationViewport("history:500",23,"opaque:500","epoch"),restored.record(target)!!.viewport)
    }
    @Test fun newEpochWithoutCanonicalSequenceDoesNotReuseOldWatermark() {
        val state=ConversationReadState().head(target,"epoch",1000,1000,true).visible(target,listOf(event(700)))
            .head(target,"new",null,null,false).visible(target,listOf(event(800).copy(historyEpoch="new",incomingSeq=null)))
        assertEquals(0,state.record(target)!!.readThrough);assertFalse(state.record(target)!!.headComplete)
    }
    @Test fun confirmedRenameCopiesReadStateOnlyWhenDestinationAbsent() {
        val destination=target.copy(session="codex/project/renamed")
        val action=SessionAction("id",target,"rename","{}",status="completed",resultTarget=destination)
        val state=ConversationReadState().head(target,"epoch",10,10,true).visible(target,listOf(event(7))).viewport(target,"history:7",2,"opaque:7","epoch")
        val moved=state.promote(action)
        assertEquals(7,moved.record(destination)!!.readThrough)
        assertEquals(state.record(target)!!.viewport,moved.record(destination)!!.viewport)
        assertEquals(state,state.promote(action.copy(operation="fork")))
        val occupied=state.head(destination,"epoch",10,10,true)
        assertEquals(occupied,occupied.promote(action))
    }
    @Test fun allProvisionalOrMissingCanonicalAnchorCannotCreateRetainedPages() {
        val provisional = event(2).copy(id="journal:fixture:AgentMessage:native:2",historyId="",historyCursor="",historyEpoch="",incomingSeq=null)
        val window = HistoryWindow.from(page(1,1).copy(events=listOf(provisional)))
        assertNull(HistoryPage.retained(window,"opaque:1"))
        assertTrue(HistoryPage.neighbors(window,"opaque:1").isEmpty())
        val canonical = HistoryWindow.from(page(1,10))
        assertNull(HistoryPage.retained(canonical,"foreign cursor"))
        assertTrue(HistoryPage.neighbors(canonical,"foreign cursor").isEmpty())
    }
    @Test fun inspectionTailCannotPoisonRetainedAnchorOrNeighborPages() {
        val inspection = event(901).copy(id="journal:fixture:AgentMessage:native:901",historyId="",historyCursor="",historyEpoch="",incomingSeq=null)
        val live = HistoryWindow.from(page(401,900)).inspection(listOf(inspection),
            listOf(event(900).copy(id="journal:fixture:AgentMessage:native:900")))
        assertEquals(inspection.id,live.events.last().id)
        val retained = HistoryPage.retained(live,"opaque:880")!!
        val restored = HistoryPage.cached(target,retained)
        assertTrue(restored.events.any { it.historyCursor=="opaque:880" })
        assertFalse(restored.events.any { it.id==inspection.id })
        assertTrue(restored.hasAfter)
        assertFalse(restored.complete)
        assertNull(restored.latestIncoming)
        HistoryPage.neighbors(live,"opaque:700").forEach { (_,_,raw) ->
            assertTrue(HistoryPage.cached(target,raw).events.all { it.historyEpoch=="epoch" && it.historyId.isNotBlank() && it.historyCursor.isNotBlank() })
        }
        assertNull(HistoryPage.retained(live,inspection.historyCursor))
    }
    @Test fun retainedWindowIsDisplayOnlyAndRestoresExactAnchor() {
        val window=HistoryWindow.from(page(401,500,after=true))
        val raw=HistoryPage.retained(window,"opaque:450")!!
        val restored=HistoryPage.cached(target,raw)
        assertFalse(restored.complete);assertNull(restored.latestIncoming)
        assertTrue(restored.events.any { it.id=="history:450" })
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.cached(target.copy(conversation="other"),raw) }
    }
    @Test fun offlineSavedNeighborPageWorksWithoutLiveCapabilitiesAndNeverClaimsFreshHead()=kotlinx.coroutines.runBlocking {
        val window=HistoryWindow.from(page(401,900,after=true))
        val saved=HistoryPage.retained(window,"opaque:567")!!
        val around=HistoryPage.cached(target,saved)
        val neighbors=HistoryPage.neighbors(window,"opaque:567")
        val before=neighbors.single { it.first=="before" && it.second==around.before }.third
        var liveAttempts=0
        val loaded=HistoryReadPaths.read(target,network={ liveAttempts++;throw java.io.IOException("No verified capability connection") },cached={before})
        assertEquals(1,liveAttempts);assertFalse(loaded.verified);assertFalse(loaded.page.complete)
        assertTrue(loaded.page.events.last().incomingSeq!!<around.events.first().incomingSeq!!)
        assertNull(loaded.page.latestIncoming)
    }
    @Test fun nativeCanonicalReplacementRetainsSameFixedUnreadDividerAndReadAnchor() {
        val legacy="provider:native:AgentMessage:legacy"
        val canonical=event(9).copy(originalIds=listOf(legacy))
        val aliases=HistoryAliases.exact(listOf(canonical),setOf(legacy))
        var state=ConversationReadState().viewed(target,setOf(legacy)).viewport(target,legacy,12)
        state=state.supersede(target,aliases)
        assertEquals(canonical.id,HistoryAliases.boundary(legacy,aliases))
        assertEquals(canonical.id,state.record(target)!!.viewport!!.eventId)
        assertTrue(canonical.id in state.record(target)!!.seen)
        val ambiguous=HistoryAliases.exact(listOf(canonical,event(10).copy(originalIds=listOf(legacy))),setOf(legacy))
        assertEquals(legacy,HistoryAliases.boundary(legacy,ambiguous))
    }
    @Test fun freshHeadObservationAloneDoesNotCreateAnEstablishedReadBaseline() {
        val first=ConversationReadState().head(target,"epoch",1000,1000,true)
        assertFalse(first.record(target)!!.established);assertEquals(0,first.record(target)!!.readThrough)
        assertTrue(first.visible(target,listOf(event(500))).record(target)!!.established)
        assertTrue(first.viewport(target,"history:500",0,"opaque:500","epoch").record(target)!!.established)
    }
    @Test fun readOnlyArchiveAndChildPayloadsKeepExactScopeWithoutMutationBuilder() {
        val archive=target.copy(archiveId="55555555-5555-4555-8555-555555555555")
        val payload=HistoryRequests.payload(archive,"request","before","opaque")
        assertEquals(archive.archiveId,payload.getString("archive_id"));assertEquals(archive.conversation,payload.getString("expected_conversation_id"))
        val child=target.copy(agentId="child",parentConversation="conversation",conversation="conversation/child")
        val scoped=HistoryRequests.payload(child,"request")
        assertEquals("conversation",scoped.getString("expected_conversation_id"));assertEquals("child",scoped.getString("agent_id"))
        assertFalse(scoped.has("archive_id"))
    }
    @Test fun nativePageRequiresExactScopeCanonicalRowsAndCompleteCounters() {
        val raw=JSONObject().put("request_id","request").put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("history_epoch","epoch").put("events",JSONArray().put(JSONObject().put("type","AgentMessage").put("seq",1).put("detail","reply").put("at",1)
                .put("history_id","h1").put("history_cursor","cursor1").put("incoming_seq",1)))
            .put("head",JSONObject().put("complete",true).put("incoming_seq",1).put("total_incoming",1))
            .put("has_more_before",false).put("has_more_after",false).put("next_before",JSONObject.NULL).put("next_after",JSONObject.NULL)
        assertEquals("history:h1",HistoryPage.parse(target,"request",raw).events.single().id)
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.parse(target,"wrong",raw) }
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.parse(target.copy(archiveId="archive"),"request",raw) }
        raw.getJSONObject("head").remove("incoming_seq")
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.parse(target,"request",raw) }
    }
}
