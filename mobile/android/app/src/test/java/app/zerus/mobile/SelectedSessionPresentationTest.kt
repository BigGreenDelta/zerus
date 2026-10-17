package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SelectedSessionPresentationTest {
    @Test fun olderInspectionCannotResurrectSettledWaitOrDropNewerWait() {
        val wait=JSONObject("""{"schema":1,"status":"ok","revision":3,"run_id":"run","conversation_id":"conversation","count":0}""")
        val current=selected.copy(raw=raw().put("coordination_wait",wait))
        val stale=raw().put("coordination_wait",JSONObject(wait.toString()).put("revision",2).put("count",1))
        assertEquals(0,SelectedSessionPresentation.session(current,stale).raw.getJSONObject("coordination_wait").getInt("count"))
        val awaiting=current.copy(raw=raw().put("coordination_wait",JSONObject(wait.toString()).put("count",1)))
        assertEquals(1,SelectedSessionPresentation.session(awaiting,raw()).raw.getJSONObject("coordination_wait").getInt("count"))
        assertEquals(0,SelectedSessionPresentation.session(awaiting,raw().put("coordination_wait",JSONObject(wait.toString()).put("revision",4))).raw.getJSONObject("coordination_wait").getInt("count"))
    }
    private val target = Target("computer","codex/example/mobile","run","conversation","workspace")
    private val selected = Session(target,"Mobile","codex","","example","",0,JSONObject().put("name",target.session))
    private fun raw() = JSONObject().put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
        .put("activity","idle").put("tracked",true).put("state","running")
    @Test fun exactCachedInspectionPreservesReadyInsteadOfInventingUntracked() {
        assertEquals("Ready",SelectedSessionPresentation.status(selected,raw()))
        assertEquals(DesktopBadgeKind.Neutral,DesktopSessionPalette.badge(SelectedSessionPresentation.session(selected,raw()),true).kind)
    }
    @Test fun anotherRunOrConversationCannotSupplyStatus() {
        assertEquals("Status not reported",SelectedSessionPresentation.status(selected,raw().put("run_id","other")))
        assertEquals("Status not reported",SelectedSessionPresentation.status(selected,raw().put("conversation_id","other")))
    }
    @Test fun knownUntrackedAndWorkingStillUseNativeSemantics() {
        assertEquals("Not tracked",SelectedSessionPresentation.status(selected,raw().put("tracked",false).put("activity","")))
        assertEquals("Working",SelectedSessionPresentation.status(selected,raw().put("activity","busy")))
    }
    @Test fun archivedStatusNeverComesFromLiveInspection() {
        val archive = selected.copy(target = target.copy(archiveId="archive"))
        assertSame(archive,SelectedSessionPresentation.session(archive,raw()))
    }
}
