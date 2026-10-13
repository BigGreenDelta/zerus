package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionDetailsPresentationTest {
    private val target = Target("computer","provider/project/tag","run","conversation","workspace")
    @Test fun detailsExposeExactIdentityIncludingArchives() {
        val view = SessionDetailsPresentation.parse(null, target.copy(archiveId="archive-one"), false)
        assertEquals("conversation", view.fields.single { it.label == "Session ID" }.value)
        assertEquals("archive-one", view.fields.single { it.label == "Archive ID" }.value)
        assertFalse(view.fields.any { it.label == "Run ID" })
    }
    private fun task(run: String) = JSONObject().put("run_id",run).put("source","plan").put("items",JSONArray().put(JSONObject()
        .put("id","task").put("title","Native task").put("status","in_progress")))
    private fun model() = JSONObject().put("model","native-model").put("effort","high").put("settings_change_supported",true)
        .put("settings_apply_when","now").put("model_options",JSONArray().put(JSONObject().put("id","native-model")
            .put("label","Native model").put("effort_options",JSONArray(listOf("low","high")))))
    @Test fun taskOwnersUseNativeRosterAndWrongRunIsLastRecorded() {
        val raw = JSONObject().put("task_lists",JSONObject().put("main",task("old")).put("agent:child",task(target.run)))
            .put("subagents",JSONObject().put("child",JSONObject().put("label","Named child")))
        val view = SessionDetailsPresentation.parse(raw,target,true)
        assertEquals("main",view.tasks.first().owner)
        assertTrue(view.tasks.first().recorded)
        assertEquals("Named child",view.tasks[1].label)
        assertFalse(view.tasks[1].recorded)
        assertEquals("In progress",SessionDetailsPresentation.taskStatus(view.tasks[1].tasks.single().status))
    }
    @Test fun cachedAndArchivedTasksCannotAppearCurrent() {
        val raw = JSONObject().put("task_lists",JSONObject().put("main",task(target.run)))
        assertTrue(SessionDetailsPresentation.parse(raw,target,false).tasks.single().recorded)
        assertTrue(SessionDetailsPresentation.parse(raw,target.copy(archiveId="archive"),true).tasks.single().recorded)
    }
    @Test fun nativeReadyDoesNotCompleteActiveGoal() {
        val raw = JSONObject().put("activity","idle").put("phase","idle").put("goal",JSONObject()
            .put("objective","Finish native objective").put("status","active").put("tokens_used",0).put("token_budget",1000).put("time_used_seconds",61))
        val view = SessionDetailsPresentation.parse(raw,target,true)
        assertEquals("Pursuing goal",view.goal.first { it.label == "Status" }.value)
        assertEquals("0 / 1000",view.goal.first { it.label == "Tokens" }.value)
        assertEquals("1m",view.goal.first { it.label == "Recorded active time" }.value)
        assertTrue(view.goalNotice.contains("does not complete"))
    }
    @Test fun malformedGoalMetricsNeverBecomeNumbers() {
        val raw = JSONObject().put("goal",JSONObject().put("objective","Goal").put("status","mystery")
            .put("tokens_used","12").put("time_used_seconds",-1).put("token_budget",999))
        val view = SessionDetailsPresentation.parse(raw,target,true)
        assertEquals("Goal status unknown (mystery)",view.goal.first { it.label == "Status" }.value)
        assertFalse(view.goal.any { it.label in listOf("Tokens","Recorded active time") })
    }
    @Test fun hookProfilesRemainGroupsNotIndividualAgents() {
        val raw = JSONObject().put("subagent_source","hook_profiles")
            .put("subagent_groups",JSONObject().put("reviewer",JSONObject().put("label","Reviewer profile").put("active_count",2).put("total_count",4)))
            .put("subagents",JSONObject().put("fake-individual",JSONObject().put("state","input")))
        val view = SessionDetailsPresentation.parse(raw,target,true)
        assertEquals("reviewer",view.agents.single().id)
        assertTrue(view.agents.single().group)
        assertEquals(2L,view.agents.single().active)
        assertTrue(view.agentNotice.contains("Individual agent identities are not reported"))
    }
    @Test fun knownChildIdentityAndStateRemainNative() {
        val raw = JSONObject().put("subagents",JSONObject().put("exact-child",JSONObject().put("name","Worker").put("state","finished").put("run_id",target.run)))
        val child = SessionDetailsPresentation.parse(raw,target,true).agents.single()
        assertEquals("exact-child",child.id)
        assertEquals("Ready",SessionDetailsPresentation.agentStatus(child.state))
        assertFalse(child.group); assertFalse(child.recorded)
    }
    @Test fun processIdentityAndCapabilitiesDoNotInferStopFromRunning() {
        val p = JSONObject().put("id","exact").put("run_id",target.run).put("conversation_id","old").put("status","running")
            .put("capabilities",JSONObject().put("output",true).put("stop",false))
        val view = SessionDetailsPresentation.parse(JSONObject().put("generation","opaque-generation")
            .put("processes",JSONObject().put("items",JSONArray().put(p)).put("complete",false)),target,true)
        assertTrue(view.processes.single().stale)
        assertTrue(view.processes.single().output)
        assertFalse(view.processes.single().stop)
        assertEquals("opaque-generation",view.generation)
        assertFalse(view.processComplete)
    }
    @Test fun settingsSnapshotIgnoresHistoryAndTimestampPolls() {
        val original = model()
        val changed = JSONObject(original.toString()).put("last_event_at",123).put("journal",JSONArray().put(JSONObject().put("detail","next event")))
        assertEquals(SessionDetailsPresentation.settings(original).signature,SessionDetailsPresentation.settings(changed).signature)
    }
    @Test fun settingsNoticeIsReadAndInvalidatesSnapshot() {
        val raw = model()
        assertEquals("",SessionDetailsPresentation.settings(raw).notice)
        val noticed = JSONObject(raw.toString()).put("settings_change_notice","Claude re-reads the full history on the next message.")
        assertEquals("Claude re-reads the full history on the next message.",SessionDetailsPresentation.settings(noticed).notice)
        assertNotEquals(SessionDetailsPresentation.settings(raw).signature,SessionDetailsPresentation.settings(noticed).signature)
    }
    @Test fun settingsCatalogAndPendingIdentityChangesInvalidateSnapshot() {
        val raw = model()
        val before = SessionDetailsPresentation.settings(raw).signature
        val pending = JSONObject(raw.toString()).put("pending_settings_id","new-intent").put("pending_model","native-model").put("pending_effort","low")
        assertNotEquals(before,SessionDetailsPresentation.settings(pending).signature)
        raw.getJSONArray("model_options").getJSONObject(0).put("effort_options",JSONArray(listOf("low")))
        assertNotEquals(before,SessionDetailsPresentation.settings(raw).signature)
    }
    @Test fun unreportedFieldsAreHonestAndNoModelWindowIsInvented() {
        val view = SessionDetailsPresentation.parse(JSONObject(),target,false)
        assertTrue(view.recorded)
        assertEquals("Not reported",view.fields.first { it.label == "Process" }.value)
        assertTrue(view.goal.isEmpty())
        assertTrue(view.processes.isEmpty())
        assertEquals("No goal reported.",view.goalNotice)
    }
    @Test fun nestedGenerationAndUnfamiliarStatesKeepNativeEvidence() {
        val view = SessionDetailsPresentation.parse(JSONObject().put("generation","parent").put("processes",JSONObject().put("generation","process-generation")),target,true)
        assertEquals("process-generation",view.generation)
        assertEquals("Finished",SessionDetailsPresentation.agentStatus("completed"))
        assertEquals("native-custom-state",SessionDetailsPresentation.agentStatus("native-custom-state"))
        assertEquals("blocked",SessionDetailsPresentation.taskStatus("blocked"))
    }
}
