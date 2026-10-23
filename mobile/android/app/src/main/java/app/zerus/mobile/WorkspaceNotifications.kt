package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

/** Relay hints may wake a refresh; only current authenticated native facts grant an alert. */
internal object WorkspaceNotifications {
    private data class Scope(val session:String,val kind:String?,val completedAt:Double?,val replyId:String)

    fun events(connection:Connection,records:List<JSONObject>,page:JSONObject,after:Long,now:Double):JSONObject {
        val current=NotificationCatalogParser.parse(connection,records,emptyList()).sessions.groupBy { it.slot.key }
        val scopes=mutableMapOf<Pair<String,String>,Scope?>()
        for(computer in records) for(session in NativeParser.sessions(connection,computer)) {
            val raw=session.raw;val scope=raw.string("relay_scope_id")
            if(scope.isBlank() || session.target.session.isBlank()) continue
            val key=computer.string("id") to scope
            if(scopes.containsKey(key)) { scopes[key]=null;continue }
            val notice=current[NotificationSlot(connection.id,computer.string("id"),session.target.session).key]?.singleOrNull()
            val active=notice!=null && notice.live && notice.online && !notice.suppressed && notice.target.isNotBlank() &&
                SessionFilters.currentActivity(session,true) && raw.string("run_id")==session.target.run && raw.string("conversation_id")==session.target.conversation
            val kind=if(active && !SharedReadPolicies.attentionAcknowledged(session)) when(notice?.kind) {
                SessionAlertKind.Input,SessionAlertKind.Optional -> "attention"
                SessionAlertKind.Error -> "error"
                else -> null
            } else null
            val replyAt=raw.optDouble("reply_at",0.0);val replyId=raw.string("reply_id")
            val completion=active && notice?.kind==null && notice?.idle==true && replyId.toByteArray(Charsets.UTF_8).size in 1..256 && replyId.none(Char::isISOControl) &&
                !SharedReadPolicies.replyRead(session) && !SessionFilters.replyInProgress(session,true) &&
                replyAt.isFinite() && replyAt>0 && replyAt in (now-120)..now
            scopes[key]=Scope(session.target.session,kind,if(completion) replyAt else null,replyId)
        }
        val raw=page.getJSONArray("events");require(raw.length()<=100)
        val events=JSONArray()
        for(index in 0 until raw.length()) {
            val row=raw.getJSONObject(index);val computer=WorkspaceProtocol.string(row,"computer_id")
            val scope=scopes[computer to row.string("scope_id")]
            val kind=if(row.string("alert_kind")=="input") "attention" else row.string("alert_kind")
            val mapped=row.string("kind")=="alert" && scope!=null && (kind==scope.kind || kind=="completed" && scope.completedAt!=null)
            events.put(JSONObject().put("id",row.getLong("id")).put("computer_id",computer)
                .put("session",scope?.session ?: "encrypted-workspace-refresh")
                .put("created_at",if(mapped && kind=="completed") scope?.completedAt else row.getDouble("created_at"))
                .put("kind",if(mapped) "alert" else "refresh").put("alert_kind",if(mapped) kind else JSONObject.NULL)
                .put("native_reply_id",if(mapped && kind=="completed") scope?.replyId else JSONObject.NULL))
        }
        return JSONObject().put("events",events).put("cursor",page.opt("cursor") ?: page.opt("next_after") ?: after)
    }
}
