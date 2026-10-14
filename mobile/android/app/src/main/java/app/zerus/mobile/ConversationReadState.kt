package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class SavedConversationViewport(val eventId:String,val offset:Int,val cursor:String="",val epoch:String="")
data class ConversationReadRecord(val target:Target,val seen:List<String> = emptyList(),val viewport:SavedConversationViewport? = null,val incomplete:Boolean = false,
    val epoch:String="",val readThrough:Long=0,val headIncoming:Long?=null,val headTotal:Long?=null,val headComplete:Boolean=false,val reviewLater:Boolean=false,val established:Boolean=false)
data class ConversationReadState(val records:List<ConversationReadRecord> = emptyList()) {
    fun record(target:Target)=records.find { ConversationReadPolicies.key(it.target) == ConversationReadPolicies.key(target) }
    private fun put(next:ConversationReadRecord)=if(records.size >= 500 && record(next.target) == null) this else
        copy(records=records.filterNot { ConversationReadPolicies.key(it.target) == ConversationReadPolicies.key(next.target) }+next)
    fun viewed(target:Target,ids:Set<String>):ConversationReadState {
        val previous=record(target) ?: ConversationReadRecord(target)
        val additions=ids.filter { it !in previous.seen }.distinct()
        val available=(50_000 - records.sumOf { it.seen.size }).coerceAtLeast(0).coerceAtMost((5000-previous.seen.size).coerceAtLeast(0))
        // Legacy exact IDs are never silently evicted. Native canonical epochs use a watermark.
        return put(previous.copy(seen=previous.seen+additions.take(available),incomplete=previous.incomplete || additions.size > available,established=previous.established || ids.isNotEmpty()))
    }
    fun visible(target:Target,events:List<Event>):ConversationReadState {
        val incoming=events.filter(ConversationReadPolicies::incoming)
        val previous=record(target)
        val canonical=incoming.filter { previous?.epoch?.isNotBlank()==true && it.historyEpoch == previous.epoch && it.incomingSeq != null }
        val sequence=canonical.maxOfOrNull { it.incomingSeq!! }
        val next=if(sequence != null) put(previous!!.copy(readThrough=maxOf(previous.readThrough,sequence),established=true)) else this
        return next.viewed(target,incoming.filter { it.incomingSeq == null || it.historyEpoch != previous?.epoch }.map { it.id }.toSet())
    }
    fun head(target:Target,epoch:String,latest:Long?,total:Long?,complete:Boolean):ConversationReadState {
        if(epoch.isBlank() || epoch.length > 512 || latest != null && latest < 0 || total != null && total < 0) return this
        val old=record(target) ?: ConversationReadRecord(target)
        val same=old.epoch == epoch
        return put(old.copy(epoch=epoch,readThrough=if(same) old.readThrough else 0,
            headIncoming=if(same) latest?.let { maxOf(old.headIncoming ?: 0,it) } ?: old.headIncoming else latest,
            headTotal=if(same) total?.let { maxOf(old.headTotal ?: 0,it) } ?: old.headTotal else total,
            headComplete=complete && latest != null && total != null))
    }
    fun review(target:Target,value:Boolean)=put((record(target) ?: ConversationReadRecord(target)).copy(reviewLater=value))
    fun supersede(target:Target,aliases:Map<String,String>):ConversationReadState {
        val old=record(target) ?: return this
        var next=viewed(target,aliases.filterKeys { it in old.seen }.values.toSet())
        old.viewport?.let { anchor -> aliases[anchor.eventId]?.let { next=next.viewport(target,it,anchor.offset,anchor.cursor,anchor.epoch) } }
        return next
    }
    fun viewport(target:Target,eventId:String,offset:Int,cursor:String="",epoch:String=""):ConversationReadState {
        if(eventId.isBlank() || eventId.length > 2048 || offset < 0 || cursor.length > 8192 || epoch.length > 512) return this
        val previous=record(target) ?: ConversationReadRecord(target)
        return put(previous.copy(viewport=SavedConversationViewport(eventId,offset,cursor,epoch),established=true))
    }
    fun promote(action:SessionAction):ConversationReadState {
        val destination=action.resultTarget ?: return this
        if(!ActionDraftPolicies.proof(action,action.target,destination) || record(destination) != null) return this
        val previous=record(action.target) ?: return this
        return put(previous.copy(target=destination))
    }
}
object ConversationReadPolicies {
    fun key(target:Target)=JSONArray(listOf(target.connectionId,target.computerId,target.session,target.conversation,target.archiveId,target.agentId,target.parentConversation)).toString()
    fun incoming(event:Event)=event.nativeType in setOf("AgentMessage","Stop") && event.role != "You" && event.role != "You (answer)" && event.outgoingId.isBlank()
    /** Canonical order proves the incoming prefix before a visible message position, including an own row. */
    fun incomingThroughPosition(epoch:String,ordered:List<Event>,visible:List<Event>):List<Event> {
        val incomingVisible=visible.filter(::incoming)
        if(epoch.isBlank()) return incomingVisible
        fun canonical(row:Event)=row.historyEpoch==epoch && row.historyId.isNotBlank() && row.historyCursor.isNotBlank()
        fun position(row:Event)=incoming(row) || row.role in setOf("You","You (answer)") &&
            row.nativeType in setOf("UserPromptSubmit","UserPromptQueued","UserMessage","TurnStarted","QuestionAnswered")
        val through=visible.filter { canonical(it) && position(it) }.mapNotNull { shown ->
            ordered.indices.filter { index ->
                val row=ordered[index]
                canonical(row) && row.historyId==shown.historyId && row.historyCursor==shown.historyCursor &&
                    row.role==shown.role && row.nativeType==shown.nativeType && row.source==shown.source
            }.singleOrNull()
        }.maxOrNull() ?: return incomingVisible
        val prefix=ordered.take(through+1).filter { canonical(it) && incoming(it) && it.incomingSeq!=null }
        return (prefix+incomingVisible).distinctBy { it.id }
    }
    /** Resample visible content when canonical read evidence changes, independently of its text or geometry. */
    fun visibilityEvidence(rows:List<Event>)=rows.filter(::incoming).map { Triple(it.id,it.historyEpoch,it.incomingSeq) }
    fun unread(record:ConversationReadRecord?,event:Event)=incoming(event) && if(record?.epoch?.isNotBlank()==true && record.epoch == event.historyEpoch && event.incomingSeq != null)
        event.incomingSeq > record.readThrough else event.id !in record?.seen.orEmpty()
    fun lowerBound(record:ConversationReadRecord?,rows:List<Event>?):Int? {
        val known=if(record?.epoch?.isNotBlank()==true) record.headIncoming?.let { (it-record.readThrough).coerceIn(0,Int.MAX_VALUE.toLong()).toInt() } else null
        if(rows==null && known==null) return null
        val loaded=rows?.count { unread(record,it) } ?: 0
        return maxOf(loaded,known ?: 0)
    }
    fun encode(state:ConversationReadState)=JSONArray(state.records.map { row -> MessageCodec.targetJson(row.target).put("seen",JSONArray(row.seen))
        .put("incomplete",row.incomplete).put("epoch",row.epoch).put("read_through",row.readThrough).put("head_incoming",row.headIncoming ?: JSONObject.NULL)
        .put("head_total",row.headTotal ?: JSONObject.NULL).put("head_complete",row.headComplete).put("review_later",row.reviewLater).put("established",row.established)
        .put("viewport",row.viewport?.let { JSONObject().put("event_id",it.eventId).put("offset",it.offset).put("cursor",it.cursor).put("epoch",it.epoch) } ?: JSONObject.NULL) })
    fun decode(raw:JSONArray):ConversationReadState {
        require(raw.length() <= 500)
        var total=0
        val rows=raw.objects().map { row ->
            val seen=row.getJSONArray("seen");require(seen.length() <= 5000);total+=seen.length();require(total<=50_000)
            val ids=(0 until seen.length()).map(seen::getString);require(ids.all { it.length <= 2048 })
            val viewport=row.optJSONObject("viewport")?.let { SavedConversationViewport(it.getString("event_id"),it.getInt("offset"),it.optString("cursor"),it.optString("epoch")) }
            require(viewport == null || viewport.eventId.length<=2048 && viewport.offset>=0 && viewport.cursor.length<=8192 && viewport.epoch.length<=512)
            val epoch=row.optString("epoch");val watermark=row.optLong("read_through");require(epoch.length<=512 && watermark>=0)
            val latest=(row.opt("head_incoming") as? Number)?.toLong();val headTotal=(row.opt("head_total") as? Number)?.toLong()
            require(latest == null || latest>=0);require(headTotal == null || headTotal>=0)
            ConversationReadRecord(MessageCodec.target(row),ids,viewport,row.optBoolean("incomplete"),epoch,watermark,latest,headTotal,row.optBoolean("head_complete"),row.optBoolean("review_later"),row.optBoolean("established",ids.isNotEmpty() || viewport!=null || watermark>0))
        }
        return ConversationReadState(rows)
    }
}
