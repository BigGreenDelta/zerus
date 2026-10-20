package app.zerus.mobile

import org.json.JSONObject

data class HistoryPage(val target:Target,val epoch:String,val events:List<Event>,val before:String?,val after:String?,val hasBefore:Boolean,val hasAfter:Boolean,
    val latestIncoming:Long?,val totalIncoming:Long?,val complete:Boolean,val indexing:Boolean,val truncated:Boolean,val raw:JSONObject,val projectionApplied:Long=0) {
    companion object {
        fun cached(target:Target,raw:JSONObject):HistoryPage {
            if(raw.optInt("zerus_page_window") != 1) return parse(target,raw.getString("request_id"),raw)
            require(MessageCodec.target(raw.getJSONObject("target"))==target)
            val epoch=raw.getString("epoch");require(epoch.isNotBlank() && epoch.length<=512)
            val events=EventHistoryCodec.decode(raw.getJSONArray("rows"));require(events.size<=100 && events.all { it.historyEpoch==epoch && it.historyId.isNotBlank() && it.historyCursor.isNotBlank() })
            fun cursor(name:String)=(raw.opt(name) as? String)?.also { require(it.isNotBlank() && it.length<=8192) }
            return HistoryPage(target,epoch,events,cursor("before"),cursor("after"),raw.getBoolean("has_before"),raw.getBoolean("has_after"),null,null,false,false,true,raw,HistoryProjection.number(raw,"projection_applied",0))
        }
        private fun slice(window:HistoryWindow,start:Int,end:Int):JSONObject? {
            val rows=window.events.subList(start,end)
            if(rows.isEmpty()) return null
            val raw=JSONObject().put("zerus_page_window",1).put("target",MessageCodec.targetJson(window.target)).put("epoch",window.epoch)
                .put("projection_applied",window.projectionApplied).put("rows",EventHistoryCodec.encode(rows)).put("before",rows.first().historyCursor).put("after",rows.last().historyCursor)
                .put("has_before",window.hasBefore || start>0).put("has_after",window.hasAfter || end<window.events.size)
            return raw.takeIf { it.toString().toByteArray(Charsets.UTF_8).size<900*1024 }
        }
        private fun canonicalRetention(window:HistoryWindow):HistoryWindow {
            // Inspection may append provisional journal rows. They remain visible in memory,
            // but have no authority to supply a canonical page cursor or read sequence.
            val rows=window.events.filter { it.historyEpoch==window.epoch && it.historyId.isNotBlank() && it.historyCursor.isNotBlank() }
            val omitted=rows.size!=window.events.size
            return window.copy(events=rows,hasAfter=window.hasAfter || omitted,atHead=window.atHead && !omitted)
        }
        fun retained(window:HistoryWindow,anchor:String):JSONObject? {
            val canonical=canonicalRetention(window)
            val index=canonical.events.indexOfFirst { it.historyCursor==anchor };if(index<0) return null
            val start=(index-49).coerceAtLeast(0)
            return slice(canonical,start,(start+100).coerceAtMost(canonical.events.size))
        }
        fun neighbors(window:HistoryWindow,anchor:String):List<Triple<String,String,JSONObject>> {
            val canonical=canonicalRetention(window)
            val index=canonical.events.indexOfFirst { it.historyCursor==anchor };if(index<0) return emptyList()
            val start=(index-49).coerceAtLeast(0);val end=(start+100).coerceAtMost(canonical.events.size)
            return buildList {
                if(start>0) slice(canonical,(start-100).coerceAtLeast(0),start)?.let { add(Triple("before",canonical.events[start].historyCursor,it)) }
                if(end<canonical.events.size) slice(canonical,end,(end+100).coerceAtMost(canonical.events.size))?.let { add(Triple("after",canonical.events[end-1].historyCursor,it)) }
            }
        }
        fun parse(target:Target,id:String,raw:JSONObject):HistoryPage {
            require(raw.string("request_id") == id && raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation)
            require(raw.string("archive_id") == target.archiveId && raw.string("agent_id") == target.agentId && (target.agentId.isBlank() || raw.string("parent_conversation_id") == target.parentConversation))
            val epoch=raw.getString("history_epoch");require(epoch.isNotBlank() && epoch.length <= 512)
            fun cursor(name:String):String?=(raw.opt(name) as? String)?.also { require(it.isNotBlank() && it.length<=8192) }
            val head=raw.getJSONObject("head")
            fun number(name:String):Long?=(head.opt(name) as? Number)?.let { number -> val value=number.toLong();require(value>=0 && number.toDouble()==value.toDouble());value }
            val complete=head.optBoolean("complete");val latest=number("incoming_seq");val total=number("total_incoming")
            require(!complete || latest != null && total != null)
            val nativeRows=raw.getJSONArray("events");require(nativeRows.length()<=100)
            nativeRows.objects().forEach { row ->
                require(row.string("history_id").isNotBlank() && row.string("history_id").length<=2048 && row.string("history_cursor").isNotBlank() && row.string("history_cursor").length<=8192)
                val sequence=row.opt("incoming_seq")
                require(sequence==null || sequence===JSONObject.NULL || sequence is Number && sequence.toLong()>=0 && sequence.toDouble()==sequence.toLong().toDouble())
            }
            val events=NativeParser.events(raw);require(events.size<=100 && events.all { it.historyId.isNotBlank() && it.historyCursor.isNotBlank() && it.historyEpoch==epoch })
            val before=cursor("next_before");val after=cursor("next_after")
            val hasBefore=raw.getBoolean("has_more_before");val hasAfter=raw.getBoolean("has_more_after")
            require(!hasBefore || before != null);require(!hasAfter || after != null)
            return HistoryPage(target,epoch,events,before,after,hasBefore,hasAfter,latest,total,complete,raw.optBoolean("indexing"),raw.optBoolean("truncated"),raw,if(complete) HistoryProjection.generation(raw) else 0)
        }
    }
}
/**
 * [events] are durable canonical rows, the only ones paged, cached or used as cursors. [inbox] holds the
 * latest inspection's undelivered inbox rows (hgs keeps them out of history); at the head they follow
 * the durable tail in [displayed] until a durable row represents them or the inspection drops them.
 */
data class HistoryWindow(val target:Target,val epoch:String,val events:List<Event>,val before:String?,val after:String?,val hasBefore:Boolean,val hasAfter:Boolean,val atHead:Boolean,
    val inbox:List<Event> = emptyList(),val projectionApplied:Long=0) {
    val displayed:List<Event> get() {
        if(!atHead || inbox.isEmpty()) return events
        val known=events.flatMap(HistoryInspection::identities).toSet()
        return events+inbox.filter { row -> HistoryInspection.identities(row).none { it in known } }
    }
    fun merge(page:HistoryPage,direction:String,maxEvents:Int=500,maxBytes:Int=2*1024*1024):HistoryWindow {
        require(target==page.target && epoch==page.epoch)
        require(direction in setOf("before","after"))
        val aliases=RollingConversationHistory.merge(target,target,events,page.events,Int.MAX_VALUE,Int.MAX_VALUE)
        val values=aliases.events.associateBy { it.id }
        // Canonical server order is authoritative; equal timestamps must not reorder pages.
        val incomingCanonical=page.events.filter { it.historyId.isNotBlank() }.map { it.id }.toSet()
        val oldOrder=events.filterNot { it.historyId.isBlank() && aliases.supersededIds[it.id] in incomingCanonical }
        val order=if(direction=="before") page.events+oldOrder else oldOrder+page.events
        val rows=order.mapNotNull { row -> values[aliases.supersededIds[row.id] ?: row.id] }.distinctBy { it.id }.toMutableList()
        var dropped=false
        while(rows.size>maxEvents || EventHistoryCodec.encode(rows).toString().toByteArray(Charsets.UTF_8).size>maxBytes) {
            if(rows.isEmpty()) break
            rows.removeAt(if(direction=="before") rows.lastIndex else 0);dropped=true
        }
        val nextHasAfter=if(direction=="after") page.hasAfter else hasAfter || dropped
        return copy(events=rows,before=if(direction=="before") page.before else if(dropped) rows.firstOrNull()?.historyCursor else before,
            after=if(direction=="after") page.after else if(dropped) rows.lastOrNull()?.historyCursor else after,
            hasBefore=if(direction=="before") page.hasBefore else hasBefore || dropped,
            hasAfter=nextHasAfter,
            atHead=!nextHasAfter,projectionApplied=minOf(projectionApplied,page.projectionApplied))
    }
    fun inspection(incoming:List<Event>,previousInspection:List<Event> = emptyList()):HistoryWindow {
        if(!atHead) return this
        val known=events.flatMap { HistoryInspection.identities(it) }.toSet()
        val updated=events.map { canonical ->
            val exact=incoming.filter { row -> row.id in HistoryInspection.identities(canonical) && row.source==canonical.source && row.nativeType==canonical.nativeType && row.role==canonical.role }.singleOrNull()
            val refreshed=if(exact!=null && (!exact.detailTruncated || canonical.detailTruncated)) {
                val voice = exact.voice?.let { VoiceProtocol.newer(canonical.voice, it) } ?: canonical.voice
                canonical.copy(text=voice?.transcript ?: exact.text,attachments=if (voice == null) exact.attachments else listOf(voice.audio),detailTruncated=exact.detailTruncated,voice=voice)
            } else canonical
            // An OpenCode prompt indexed before hgs could name its sending request keeps none in
            // history; this conversation's inspection row of the same message may name it now.
            if(exact!=null && exact.source=="opencode_native" && canonical.requestId.isBlank() && exact.requestId.isNotBlank())
                refreshed.copy(requestId=exact.requestId) else refreshed
        }
        val watermark=HistoryInspection.journalWatermark(events+previousInspection)
        val newer=incoming.filter { row -> row.id !in known && watermark!=null && HistoryInspection.journalSequence(row)?.let { it>watermark }==true }
        val inbox=HistoryInspection.inbox(incoming)
        if(newer.isEmpty()) return copy(events=updated,inbox=inbox)
        val cursor=events.lastOrNull { it.historyCursor.isNotBlank() }?.historyCursor ?: after
        val page=HistoryPage(target,epoch,newer,before,cursor,hasBefore,false,null,null,false,false,false,JSONObject(),projectionApplied)
        return copy(events=updated,inbox=inbox).merge(page,"after")
    }
    companion object { fun from(page:HistoryPage)=HistoryWindow(page.target,page.epoch,page.events,page.before,page.after,page.hasBefore,page.hasAfter,!page.hasAfter,projectionApplied=page.projectionApplied) }
}

object HistoryRequests {
    /** An empty canonical head has no cursor, but can still gain its first voice message. */
    fun refreshAtHead(window:HistoryWindow):Pair<String,String>? {
        if(!window.atHead) return null
        val cursor=window.events.lastOrNull { it.historyCursor.isNotBlank() }?.historyCursor
        return if(cursor==null) "" to "" else "after" to cursor
    }
    fun payload(target:Target,id:String,direction:String="",cursor:String="",limit:Int=100):JSONObject {
        require(target.run.isNotBlank() && target.conversation.isNotBlank() && limit in 1..100)
        require(direction in setOf("","before","after","around","unread") && (direction.isEmpty()==cursor.isEmpty()) && cursor.length<=8192)
        val raw=JSONObject().put("request_id",id).put("expected_run_id",target.run)
            .put("expected_conversation_id",if(target.agentId.isBlank()) target.conversation else target.parentConversation).put("limit",limit)
        if(target.archiveId.isNotBlank()) raw.put("archive_id",target.archiveId)
        if(target.agentId.isNotBlank()) { require(target.parentConversation.isNotBlank());raw.put("agent_id",target.agentId) }
        if(direction=="unread") {
            val marker=org.json.JSONArray(cursor);require(marker.length()==2)
            val epoch=marker.getString(0);val sequence=marker.getLong(1);require(epoch.isNotBlank() && epoch.length<=512 && sequence>=0)
            raw.put("history_epoch",epoch).put("around_incoming_seq",sequence)
        } else if(direction.isNotBlank()) raw.put(direction,cursor)
        return raw
    }
}

/** Journal sequence proves arrival within that journal; provider IDs and clocks do not. */
object HistoryInspection {
    data class Rebased<T>(val base:HistoryWindow?,val value:T)
    suspend fun <T> rebase(initial:HistoryWindow?,value:T,current:()->HistoryWindow?,merge:suspend (HistoryWindow?)->T):Rebased<T> {
        var expected=initial;var result=value
        while(current() !== expected) { expected=current();result=merge(expected) }
        return Rebased(expected,result)
    }
    fun identities(row:Event)=(row.originalIds+listOf(row.id,row.originalId)).filter(String::isNotBlank)
    private fun sequence(id:String):Long? {
        if(!id.startsWith("journal:")) return null
        val token=id.substringAfterLast(":native:","")
        return token.takeIf { it.isNotEmpty() && it.all { char -> char in '0'..'9' } }?.toLongOrNull()
    }
    fun journalSequence(row:Event)=identities(row).mapNotNull(::sequence).maxOrNull()
    fun journalWatermark(rows:List<Event>)=rows.mapNotNull(::journalSequence).maxOrNull()
    /** Inspection-only inbox rows (queued or not delivered); never canonical history. */
    fun inbox(incoming:List<Event>)=incoming.filter { it.role=="You" && it.delivery in MessageDeliveryPresentation.inboxStates }.take(64)
    /** New canonical evidence only: inbox rows never invalidate the history head. */
    fun changed(window:HistoryWindow,incoming:List<Event>,previous:List<Event>):Boolean {
        val known=(window.events+previous).flatMap(::identities).toSet()
        return incoming.any { row -> row.delivery !in MessageDeliveryPresentation.inboxStates && identities(row).none { it in known } }
    }
    fun retain(current:HistoryWindow,candidate:HistoryWindow?,incoming:List<Event>,previous:List<Event>):HistoryWindow =
        if(current==candidate) current else if(current.atHead) current.inspection(incoming,previous) else current
}
