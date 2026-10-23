package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class WorkspaceNotificationsTest {
    private val computer="22222222-2222-4222-8222-222222222222"
    private val connection=Connection("protected","Fixture","https://relay.example","synthetic",2)
    private fun session()=JSONObject().put("name","provider/fixture/session").put("run_id","run").put("conversation_id","conversation")
        .put("state","running").put("process_state","running").put("activity","idle").put("phase","input")
        .put("relay_scope_id","opaque-scope").put("attention_signature","a".repeat(64))
    private fun preference()=JSONObject().put("conversation_id","conversation").put("muted",true).put("revision",1)
        .put("suppressed",JSONObject().put("reply_id",JSONObject.NULL).put("attention_signature",JSONObject.NULL)
            .put("optional_question_ids",JSONArray()).put("main_attention",JSONObject.NULL).put("child_attention",JSONObject()))
    private fun mapped(row:JSONObject,kind:String="input",scope:String="opaque-scope",online:Boolean=true,extra:JSONObject?=null):JSONObject {
        val records=listOf(JSONObject().put("id",computer).put("name","Fixture").put("online",online).put("via",JSONObject.NULL)
            .put("snapshot",JSONObject().put("sessions",JSONArray(listOfNotNull(row,extra)))))
        val page=JSONObject().put("events",JSONArray().put(JSONObject().put("id",1).put("computer_id",computer).put("scope_id",scope)
            .put("kind","alert").put("alert_kind",kind).put("created_at",1000.0))).put("cursor",1)
        return WorkspaceNotifications.events(connection,records,page,0,1000.0).getJSONArray("events").getJSONObject(0)
    }
    @Test fun hintsNeedCurrentNativeIdentityNeedAndUnreadUnmutedState() {
        assertEquals("attention",mapped(session()).string("alert_kind"))
        assertEquals("error",mapped(session().put("phase","error"),"error").string("alert_kind"))
        for(row in listOf(session().put("phase","idle"),session().put("run_id",""),session().put("conversation_id",""),
            session().put("state","paused"),session().put("state","archived"),session().put("process_state","exited"),
            session().put("notification_preferences",preference()),session().put("read",JSONObject().put("conversation_id","conversation").put("attention_signature","a".repeat(64))))) {
            assertEquals("refresh",mapped(row).string("kind"))
        }
        assertEquals("refresh",mapped(session(),"error").string("kind"))
        assertEquals("refresh",mapped(session(),scope="unknown").string("kind"))
        assertEquals("refresh",mapped(session(),online=false).string("kind"))
        assertEquals("refresh",mapped(session(),extra=session().put("name","provider/fixture/other")).string("kind"))
    }
    @Test fun completedHintNeedsRecentSignedUnreadReplyAndNoNativeWork() {
        fun reply()=session().put("phase","idle").put("reply_id","reply-current").put("reply_at",990.0)
        val valid=mapped(reply(),"completed")
        assertEquals("completed",valid.string("alert_kind"));assertEquals(990.0,valid.getDouble("created_at"),0.0)
        assertEquals("reply-current",valid.string("native_reply_id"))
        for(row in listOf(reply().put("reply_at",100.0),reply().put("reply_id",""),reply().put("activity","busy"),
            reply().put("process_work",JSONObject().put("count",1)),reply().put("notification_preferences",preference()),
            reply().put("read",JSONObject().put("conversation_id","conversation").put("reply_id","reply-current")))) {
            assertEquals("refresh",mapped(row,"completed").string("kind"))
        }
        assertEquals("refresh",mapped(reply().put("reply_at",1001.0),"completed").string("kind"))
    }
    @Test fun changedServiceEventIdentityCannotReannounceSameNativeReply() {
        val slot=NotificationSlot(connection.id,computer,"provider/fixture/session")
        val current=NotificationCurrent(slot,"[run,conversation]","Fixture","Computer",null,"",true,conversation="conversation")
        val preferences=NotificationPreferences(finished=true)
        val before=NotificationState(bootstrap=false,settingsGeneration=0,records=listOf(NotificationRecord(slot,current.target,980.0)))
        val event=NotificationEvent(1,computer,slot.session,"alert",990.0,"completed","reply-current")
        val first=NotificationPolicy.plan(before,listOf(current),listOf(slot to event),preferences,1000.0,alertsOnly=true)
        assertTrue(first.desired.single().fresh)
        val posted=NotificationPolicy.reserve(first.state,first.desired.single(),false,1000)
        val replay=NotificationPolicy.plan(posted,listOf(current),listOf(slot to event.copy(id=2,at=1001.0)),preferences,1002.0,alertsOnly=true)
        assertEquals(first.desired.single().fingerprint,replay.desired.single().fingerprint)
        assertFalse(replay.desired.single().fresh)
        val saved=NotificationStateCodec.decode(NotificationStateCodec.encode(before.copy(pending=listOf(event))))
        assertEquals("reply-current",saved.pending.single().nativeReplyId)
    }
}
