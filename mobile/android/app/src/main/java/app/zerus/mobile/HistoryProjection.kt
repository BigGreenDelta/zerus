package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

/** Same-epoch corrections to derived display rows. Never a read mark or a delivery receipt. */
object HistoryProjection {
    const val maxBytes=880*1024
    const val maxPages=4
    data class Entry(val generation:Long,val id:String,val change:String,val successor:Event?,val resolved:Boolean)
    data class Page(val entries:List<Entry>,val next:Long,val complete:Boolean) {
        val prefix:List<Entry> get()=entries.takeWhile { it.resolved }
        fun applied(after:Long):Long=if(prefix.size==entries.size && complete) next else prefix.lastOrNull()?.generation ?: after
    }
    data class Run(val start:Long,val until:Long,val after:Long=start,val pages:Int=0,val stopped:Boolean=false) {
        init { require(start>=0 && until>=start) }
        val canRead:Boolean get()=pages<maxPages && after<until && !stopped
        fun accepted(page:Page):Run { require(canRead);return copy(after=page.applied(after),pages=pages+1,stopped=page.complete || page.prefix.size!=page.entries.size) }
    }
    fun supported(relay:Set<String>?,native:Set<String>?)=relay?.contains("history_projection")==true && native?.contains("history_projection")==true
    fun number(raw:JSONObject,key:String,missing:Long?=null):Long {
        if(!raw.has(key) && missing!=null) return missing
        val n=raw.opt(key);require(n is Int || n is Long)
        return (n as Number).toLong().also { require(it>=0) }
    }
    fun generation(raw:JSONObject):Long {
        val upgrade=raw.optJSONObject("source_status")?.optJSONObject("projection_upgrade") ?: return 0
        return number(upgrade,"generation",0)
    }
    private fun text(value:Any?,limit:Int):String {
        require(value is String && value.isNotEmpty() && value.toByteArray(Charsets.UTF_8).size<=limit && value.none { it.isISOControl() })
        return value
    }
    private fun id(value:Any?):String { require(value is String && value.length==64 && value.all { it in '0'..'9' || it in 'a'..'f' });return value }
    /** Count compact ASCII JSON, including surrogate pairs, as the computer's envelope budget does. */
    private fun asciiBytes(raw:JSONObject):Long {
        val s=raw.toString();var count=0L;var i=0
        while(i<s.length) {
            val c=s[i]
            if(c.isHighSurrogate()) { require(i+1<s.length && s[i+1].isLowSurrogate());count+=12;i+=2 }
            else { require(!c.isLowSurrogate());count+=if(c.code<128) 1 else 6;i++ }
        }
        return count
    }
    fun payload(target:Target,request:String,epoch:String,after:Long,until:Long):JSONObject {
        require(after>=0 && after<=until);text(epoch,128)
        return HistoryRequests.payload(target,request).apply { remove("limit");put("history_epoch",epoch);put("projection_after",after);put("projection_until",until) }
    }
    fun parse(target:Target,request:String,epoch:String,after:Long,until:Long,raw:JSONObject):Page {
        require(after>=0 && until>=after && asciiBytes(raw)<=maxBytes)
        require(raw.opt("request_id")==request && raw.opt("name")==target.session && raw.opt("run_id")==target.run && raw.opt("conversation_id")==target.conversation)
        fun echo(field:String,expected:String)=if(raw.has(field)) raw.opt(field)==expected else expected.isEmpty()
        require(echo("archive_id",target.archiveId) && echo("agent_id",target.agentId) && echo("parent_conversation_id",if(target.agentId.isBlank()) "" else target.parentConversation))
        require(raw.opt("history_epoch")==epoch && number(raw,"projection_generation")>=until)
        val complete=raw.opt("complete");require(complete is Boolean)
        val next=number(raw,"next_projection_after");val values=raw.getJSONArray("projection_changes");require(values.length()<=256)
        var previous=after
        val entries=(0 until values.length()).map { index ->
            val row=values.getJSONObject(index)
            require(row.keys().asSequence().toSet().let { it.containsAll(setOf("generation","history_id","change","resolution","successor")) && (it-setOf("generation","history_id","change","resolution","successor","reason")).isEmpty() })
            val generation=number(row,"generation");require(generation>previous && generation<=until);previous=generation
            val old=id(row.opt("history_id"));val change=row.opt("change");require(change=="retired" || change=="refreshed")
            val resolution=row.opt("resolution");val successor=row.opt("successor")
            val event=when(resolution) {
                "unresolved" -> { require(successor===JSONObject.NULL && (!row.has("reason") || row.opt("reason") in setOf("no_successor","unexpected_successor","oversize")));null }
                "resolved" -> {
                    require(!row.has("reason"))
                    if(change=="retired") { require(successor===JSONObject.NULL);null }
                    else {
                        require(successor is JSONObject);id(successor.opt("history_id"));text(successor.opt("history_cursor"),8192)
                        require(successor.opt("history_stream") in setOf("journal","provider","attachment","voice") && !successor.has("native_match_hash"))
                        require(successor.opt("type") in setOf("UserPromptSubmit","UserPromptQueued","UserMessage","TurnStarted","QuestionAnswered","AgentMessage","Stop"))
                        val aliases=successor.getJSONArray("original_ids");require(aliases.length()<=8)
                        repeat(aliases.length()) { text(aliases.get(it),8192) }
                        if(successor.has("incoming_seq") && !successor.isNull("incoming_seq")) number(successor,"incoming_seq")
                        if(successor.has("agent_id") && !successor.isNull("agent_id")) require(successor.opt("agent_id") in setOf("",target.agentId.ifBlank { "main" }))
                        val parsed=NativeParser.events(JSONObject().put("history_epoch",epoch).put("agent_id",target.agentId).put("events",JSONArray().put(successor)))
                        require(parsed.size==1);parsed.single().also { require(it.at.isFinite() && it.historyEpoch==epoch && it.id=="history:${it.historyId}") }
                    }
                }
                else -> error("Unknown history projection resolution")
            }
            Entry(generation,old,change as String,event,resolution=="resolved")
        }
        require(if(complete) next==until else entries.isNotEmpty() && next==previous && next<until)
        return Page(entries,next,complete)
    }
    /** Applies only the resolved prefix to the latest window, not the run's captured rows. */
    fun apply(window:HistoryWindow,target:Target,epoch:String,start:Long,entries:List<Entry>,applied:Long):HistoryWindow? {
        if(window.target!=target || window.epoch!=epoch) return null
        var rows=window.events;var dropped=false
        for(entry in entries) {
            require(entry.resolved)
            val old=rows.firstOrNull { it.id=="history:${entry.id}" && it.historyId==entry.id && it.historyEpoch==epoch }
            if(entry.change=="retired") {
                if(old!=null && old.role=="You" && old.requestId.isBlank() && old.outgoingId.isBlank() && old.voice==null && old.source !in setOf("hgs_delivery","mobile_outbox","hgs_voice")) rows=rows.filterNot { it.id==old.id }
            } else {
                val successor=entry.successor ?: error("Missing history successor")
                val known=rows.any { it.id==successor.id && it.historyEpoch==epoch }
                if(old==null && !known) continue // Never import an unseen history range.
                val previous=if(old!=null && old.id!=successor.id) rows.mapNotNull { if(it.id==old.id) { if(known) null else successor } else it } else rows
                val merged=RollingConversationHistory.merge(target,target,previous,listOf(successor))
                rows=merged.events;dropped=dropped || merged.truncated
            }
        }
        val marker=if(window.projectionApplied>=start) maxOf(window.projectionApplied,applied) else window.projectionApplied
        return window.copy(events=rows,projectionApplied=marker,hasBefore=window.hasBefore || dropped,
            before=if(dropped) rows.firstOrNull { it.historyCursor.isNotBlank() }?.historyCursor else window.before)
    }
}
