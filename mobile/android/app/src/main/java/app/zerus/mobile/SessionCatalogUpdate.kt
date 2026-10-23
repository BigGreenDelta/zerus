package app.zerus.mobile

/** One background publication: never expose raw rows before local read decoration.
 * Unchanged cards keep their objects, even when a poll decoded fresh JSON objects. */
object SessionCatalogUpdate {
    fun prepare(incoming:List<Session>,previous:List<Session>,actions:List<SessionAction>,
        reads:ConversationReadState,ready:Boolean,heads:Set<String>,loaded:Map<Target,List<Event>>):List<Session> {
        val merged=SessionNotificationPreferences.mergeRows(incoming,previous,actions)
        val decorated=SessionReadAttention.prepare(merged,reads,ready,heads,loaded)
        val old=previous.associateBy { it.target }
        val rows=decorated.map { row ->
            old[row.target]?.takeIf { retained ->
                row.copy(raw=retained.raw)==retained && row.raw.toString()==retained.raw.toString()
            } ?: row
        }
        return if(rows.size==previous.size && rows.indices.all { rows[it]===previous[it] }) previous else rows
    }
}
