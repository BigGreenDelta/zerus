package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class HistoryProjectionTest {
    private val target=Target("computer","claude/project","run","conversation","workspace")
    private val a="a".repeat(64)
    private val b="b".repeat(64)
    private val c="c".repeat(64)
    private fun row(id:String=a,text:String="old",at:Double=1.0)=JSONObject().put("history_id",id).put("history_cursor","cursor:$id")
        .put("history_stream","provider").put("type","UserMessage").put("source","claude_native").put("detail",text).put("at",at).put("original_ids",JSONArray())
    private fun event(id:String=a,text:String="old",at:Double=1.0)=Event("history:$id","You",text,"",at,source="claude_native",nativeType="UserMessage",historyId=id,historyCursor="cursor:$id",historyEpoch="epoch")
    private fun entry(generation:Long,id:String=a,change:String="retired",successor:JSONObject?=null,resolved:Boolean=true)=JSONObject()
        .put("generation",generation).put("history_id",id).put("change",change).put("resolution",if(resolved) "resolved" else "unresolved")
        .put("successor",successor ?: JSONObject.NULL)
    private fun response(entries:List<JSONObject>,until:Long=entries.lastOrNull()?.getLong("generation") ?: 0,complete:Boolean=true,next:Long=until)=JSONObject()
        .put("request_id","request").put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
        .put("history_epoch","epoch").put("projection_generation",until).put("projection_changes",JSONArray(entries)).put("complete",complete).put("next_projection_after",next)
    private fun parse(raw:JSONObject,after:Long=0,until:Long=raw.getLong("projection_generation"))=HistoryProjection.parse(target,"request","epoch",after,until,raw)
    private fun window(vararg rows:Event,marker:Long=0)=HistoryWindow(target,"epoch",rows.toList(),rows.firstOrNull()?.historyCursor,rows.lastOrNull()?.historyCursor,false,false,true,projectionApplied=marker)
    private fun apply(window:HistoryWindow,page:HistoryProjection.Page,start:Long=window.projectionApplied)=HistoryProjection.apply(window,target,"epoch",start,page.prefix,page.applied(start))!!

    @Test fun thirtyThreeRefreshesCommitOnePrefixAndKeepTheLatestWindow() = runBlocking {
        val ids=(1..33).map { it.toString(16).padStart(64,'0') }
        val initial=window(*ids.map { event(it) }.toTypedArray())
        val page=parse(response(ids.mapIndexed { i,id -> entry(i+1L,id,"refreshed",row(id,"fixed $i")) }))
        val latest=initial.copy(events=initial.events+event(c,"arrived",100.0))
        val rebased=HistoryInspection.rebase(initial,apply(initial,page),{latest}) { apply(it!!,page) }
        assertSame(latest,rebased.base)
        assertEquals(33L,rebased.value.projectionApplied)
        assertEquals((0..32).map { "fixed $it" }+"arrived",rebased.value.events.map { it.text })
    }
    @Test fun unresolvedRetirementOrRefreshStopsOnlyItsPrefix() {
        for(change in listOf("retired","refreshed")) {
            val page=parse(response(listOf(entry(1),entry(2,b,change,resolved=false),entry(3,c))))
            val fixed=apply(window(event(a),event(b),event(c)),page)
            assertEquals(listOf(b,c),fixed.events.map { it.historyId });assertEquals(1L,fixed.projectionApplied)
            assertEquals(1,page.prefix.size)
        }
    }
    @Test fun olderAnswersNeverReplaceCorrectionsCoveredByANewerPage() {
        for(change in listOf("retired","refreshed")) {
            val stale=entry(1,a,change,if(change=="refreshed") row(a,"stale") else null)
            val page=parse(response(listOf(stale,entry(3,b,"refreshed",row(b,"new b")))))
            val fixed=apply(window(event(a,"newer a"),event(b),marker=2),page,start=0)
            assertEquals(listOf("newer a","new b"),fixed.events.map { it.text })
            assertEquals(3L,fixed.projectionApplied)
        }
    }
    @Test fun malformedShapeAnywhereRejectsTheWholeResponse() {
        val bad=listOf(entry(2,b,"refreshed"),entry(2,b,"retired",row(b)),entry(2,b,"refreshed",row(b),false),
            entry(2,b).put("reason","oversize"),entry(2,b).put("generation",1),entry(2,b).put("generation",2.0),
            entry(2,"not-a-hash"),entry(2,b).put("extra",true),entry(2,b,"refreshed",row(b).put("history_cursor","")))
        for(value in bad) assertThrows(IllegalArgumentException::class.java) { parse(response(listOf(entry(1),value),until=3)) }
    }
    @Test fun identityEpochBoundsCountAndIncompleteProgressAreStrict() {
        val original=response(listOf(entry(1)),until=2,complete=false,next=1)
        val invalid=listOf(original.toString().let(::JSONObject).put("name","other"),JSONObject(original.toString()).put("run_id","other"),
            JSONObject(original.toString()).put("conversation_id","other"),JSONObject(original.toString()).put("history_epoch","other"),
            JSONObject(original.toString()).put("archive_id",JSONObject.NULL),JSONObject(original.toString()).put("complete","false"),
            JSONObject(original.toString()).put("next_projection_after",0),JSONObject(original.toString()).put("projection_generation",1),
            response(emptyList(),2,false,0),response(listOf(entry(2)),2,false,2),response((1..257).map { entry(it.toLong()) }))
        for(value in invalid) assertThrows(IllegalArgumentException::class.java) { HistoryProjection.parse(target,"request","epoch",0,2,value) }
        assertEquals(1L,parse(original).next)
        val tail=parse(response(emptyList(),5),5,5);assertTrue(tail.complete);assertEquals(5L,tail.applied(5))
    }
    @Test fun asciiEnvelopeBudgetIncludesUnicodeAndIdentityOverhead() {
        val fixture=javaClass.classLoader!!.getResourceAsStream("history-projection-wire.json")!!.bufferedReader().use { JSONObject(it.readText()) }
        fixture.getJSONArray("cases").objects().forEach { case -> assertEquals(case.getLong("bytes"),HistoryProjection.asciiBytes(case.get("value"))) }
        val value=response(listOf(entry(1,a,"refreshed",row(a,"я".repeat(151_000)))))
        assertTrue(value.toString().toByteArray().size<HistoryProjection.maxBytes)
        assertThrows(IllegalArgumentException::class.java) { parse(value) }
        val unresolved=parse(response(listOf(entry(1,a,"refreshed",resolved=false).put("reason","oversize"))))
        assertEquals(0L,unresolved.applied(0))
        val boundary=response(listOf(entry(1,a,"refreshed",row(a,"x".repeat(HistoryProjection.maxBytes)))))
        assertThrows(IllegalArgumentException::class.java) { parse(boundary) }
        val exact=response(listOf(entry(1))).put("padding","")
        val base=HistoryProjection.asciiBytes(exact)
        exact.put("padding","x".repeat((HistoryProjection.maxBytes-base).toInt()))
        assertEquals(HistoryProjection.maxBytes.toLong(),HistoryProjection.asciiBytes(exact))
        assertTrue(parse(exact).complete)
        exact.put("padding",exact.getString("padding")+"x")
        assertThrows(IllegalArgumentException::class.java) { parse(exact) }
    }
    @Test fun aliasesReplaceInPlaceAndAnUnseenRangeIsNeverImported() {
        val receipt=event(c,"receipt body").copy(requestId="receipt",source="hgs_delivery",submittedText="send",attachments=listOf(DisplayedAttachment("photo","image/png",7)))
        val successor=row(b,"receipt body",2.0).put("source","hgs_delivery").put("message_id","receipt").put("submitted_text","send")
            .put("attachments",JSONArray().put(JSONObject().put("name","photo").put("mime","image/png").put("bytes",7)))
            .put("original_ids",JSONArray(listOf("history:$a","history:$c")))
        val page=parse(response(listOf(entry(1,a,"refreshed",successor))))
        val fixed=apply(window(event(a),event(b,"older successor"),receipt),page)
        assertEquals(listOf(b),fixed.events.map { it.historyId });assertEquals("receipt",fixed.events.single().requestId)
        assertEquals("photo",fixed.events.single().attachments.single().name)
        assertEquals(c,apply(window(event(c)),parse(response(listOf(entry(1,a,"refreshed",row(b)))))).events.single().historyId)
    }
    @Test fun retirementNeverDeletesProtectedOrForeignRows() {
        val page=parse(response(listOf(entry(1))))
        for(protected in listOf(event().copy(requestId="receipt"),event().copy(outgoingId="outbox"),event().copy(source="hgs_voice"),event().copy(role="AgentMessage"),event().copy(historyEpoch="other"))) {
            assertEquals(listOf(protected),apply(window(protected),page).events)
        }
        assertNull(HistoryProjection.apply(window(event()),target.copy(run="other"),"epoch",0,page.prefix,1))
        assertNull(HistoryProjection.apply(window(event()),target,"other",0,page.prefix,1))
    }
    @Test fun anOlderSliceLowersCoverageAndIsCorrectedAgainWithoutChangingReadState() {
        val fixed=apply(window(event()),parse(response(listOf(entry(1)))))
        val old=HistoryPage(target,"epoch",listOf(event()),"old","old",false,false,null,null,false,false,true,JSONObject())
        val merged=fixed.merge(old,"before")
        assertEquals(0L,merged.projectionApplied)
        assertTrue(apply(merged,parse(response(listOf(entry(1))))).events.isEmpty())
        val oldDuringRun=window(event(),marker=0)
        assertEquals(0L,HistoryProjection.apply(oldDuringRun,target,"epoch",3,listOf(HistoryProjection.Entry(4,a,"retired",null,true)),4)!!.projectionApplied)
    }
    @Test fun sliceCoverageIsAtomicLegacyCompatibleAndNetworkNeedsCompleteHead() {
        val retained=HistoryPage.retained(window(event(),marker=8),"cursor:$a")!!
        assertEquals(8L,HistoryPage.cached(target,retained).projectionApplied)
        val legacy=JSONObject(retained.toString()).apply { remove("projection_applied") };assertEquals(0L,HistoryPage.cached(target,legacy).projectionApplied)
        assertThrows(IllegalArgumentException::class.java) { HistoryPage.cached(target,JSONObject(retained.toString()).put("projection_applied",8.0)) }
        val raw=JSONObject().put("request_id","request").put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation).put("history_epoch","epoch")
            .put("events",JSONArray().put(row())).put("has_more_before",false).put("has_more_after",false)
            .put("head",JSONObject().put("complete",true).put("incoming_seq",0).put("total_incoming",0))
            .put("source_status",JSONObject().put("projection_upgrade",JSONObject().put("generation",9)))
        assertEquals(9L,HistoryPage.parse(target,"request",raw).projectionApplied)
        raw.getJSONObject("head").put("complete",false);assertEquals(0L,HistoryPage.parse(target,"request",raw).projectionApplied)
    }
    @Test fun fourPageBudgetSnapshotsUntilAndResumesWithoutRepeatingThePrefix() {
        var run=HistoryProjection.Run(0,6);val requested=mutableListOf<Long>()
        while(run.canRead) {
            requested+=run.after
            run=run.accepted(parse(response(listOf(entry(run.after+1)),6,false,run.after+1),run.after))
        }
        assertEquals(listOf(0L,1L,2L,3L),requested);assertEquals(4L,run.after);assertEquals(6L,run.until)
        val tail=response(listOf(entry(5),entry(6,b)),6).put("projection_generation",8)
        val resumed=HistoryProjection.Run(run.after,6).accepted(parse(tail,4,6))
        assertFalse(resumed.canRead);assertEquals(6L,resumed.after)
        assertFalse(HistoryProjection.supported(setOf("history"),setOf("history_projection")))
        assertFalse(HistoryProjection.supported(setOf("history_projection"),null))
        assertTrue(HistoryProjection.supported(setOf("history_projection"),setOf("history_projection")))
    }


    @Test fun relayRequestPinsArchiveAndChildAndSubmitsOnlyOnce() = runBlocking {
        okhttp3.mockwebserver.MockWebServer().use { server ->
            val child=target.copy(agentId="worker",parentConversation="conversation",conversation="conversation/worker",archiveId="10000000-0000-4000-8000-000000000001")
            server.dispatcher=object:okhttp3.mockwebserver.Dispatcher() {
                override fun dispatch(request:okhttp3.mockwebserver.RecordedRequest):okhttp3.mockwebserver.MockResponse {
                    val body=JSONObject(request.body.clone().readUtf8());val id=body.getString("request_id")
                    val raw=response(listOf(entry(1))).put("request_id",id).put("conversation_id",child.conversation)
                        .put("agent_id",child.agentId).put("parent_conversation_id",child.parentConversation).put("archive_id",child.archiveId)
                    return okhttp3.mockwebserver.MockResponse().setBody(JSONObject().put("request_id",id).put("state","completed").put("result",raw).toString())
                }
            }
            server.start()
            val connection=Connection(target.connectionId,"Test",server.url("/").toString().trimEnd('/'),"synthetic-token")
            val page=RelayApi().historyProjection(connection,child,"epoch",0,1)
            assertEquals(1L,page.applied(0));assertEquals(1,server.requestCount)
            val sent=JSONObject(server.takeRequest().body.readUtf8());val payload=sent.getJSONObject("payload")
            assertEquals("history_projection",sent.getString("operation"));assertEquals(target.computerId,sent.getString("computer_id"))
            assertEquals("conversation",payload.getString("expected_conversation_id"));assertEquals("worker",payload.getString("agent_id"))
            assertEquals(child.archiveId,payload.getString("archive_id"));assertFalse(payload.has("limit"))
        }
    }
    @Test fun refreshGrowthKeepsTheWindowBoundAndItsOlderNavigation() {
        val rows=(1..20).map { event(it.toString(16).padStart(64,'0'),"x".repeat(100_000),it.toDouble()) }
        val first=rows.first().historyId
        val page=parse(response(listOf(entry(1,first,"refreshed",row(first,"x".repeat(500_000),1.0)))))
        val fixed=apply(window(*rows.toTypedArray()),page)
        assertTrue(fixed.hasBefore);assertEquals(fixed.events.first().historyCursor,fixed.before)
        assertTrue(EventHistoryCodec.encode(fixed.events).toString().toByteArray().size<=2*1024*1024)
    }
}
