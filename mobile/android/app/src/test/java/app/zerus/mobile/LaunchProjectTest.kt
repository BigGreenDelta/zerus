package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class LaunchProjectTest {
    private fun account(id:String,default:Boolean=false)=JSONObject().put("id",id).put("provider","claude").put("label","Same label")
        .put("native",false).put("installed",true).put("is_default",default).put("auth_status","signed_in")
    private fun catalog()=JSONObject().put("agents",JSONArray().put("claude")).put("accounts",JSONArray().put(account("profile-one")).put(account("profile-two",true)))
        .put("project_launch_supported",true).put("swarm_id","swarm-exact").put("default_project","project-one")
        .put("projects",JSONArray().put(JSONObject().put("id","project-one").put("name","Work").put("accessible",true)
            .put("folders",JSONArray().put(JSONObject().put("id","folder-one").put("name","Source").put("path","/example/source"))))
            .put(JSONObject().put("id","hidden").put("name","Unavailable").put("accessible",false)))

    @Test fun distinctSameLabelProfilesHaveConcreteDefaultAndExactIdentifiers() {
        val accounts=LaunchPresentation.accounts(catalog(),"claude")
        assertEquals(listOf("profile-one","profile-two"),accounts.map { it.id })
        assertEquals("profile-two",LaunchPresentation.defaultAccount(accounts))
        assertEquals("profile-one",LaunchPresentation.defaultAccount(accounts,"profile-one"))
        assertEquals("Same label",LaunchPresentation.accountTitle(accounts[0]))
        assertEquals("Same label",LaunchPresentation.accountTitle(accounts[1]))
    }
    @Test fun defaultAbsentOrGoneStillChoosesAnExplicitOfferedProfile() {
        val accounts=LaunchPresentation.accounts(catalog(),"claude").map { it.copy(isDefault=false) }
        assertEquals("profile-one",LaunchPresentation.defaultAccount(accounts,"removed"))
        assertEquals("",LaunchPresentation.defaultAccount(emptyList()))
        assertTrue(LaunchPresentation.accounts(catalog(),"codex").isEmpty())
    }
    @Test fun sanitizationPreservesOnlyBoundedLaunchMetadataAndCurrentLocalProjects() {
        val raw=catalog()
        raw.getJSONArray("accounts").getJSONObject(0).put("home","/private").put("identity",JSONObject().put("email","private@example.test")).put("credentials","private-token")
        val sanitized=LaunchPresentation.sanitizeCatalog(raw)
        assertFalse(sanitized.toString().contains("private"))
        assertEquals("profile-two",LaunchPresentation.defaultAccount(LaunchPresentation.accounts(sanitized,"claude")))
        assertEquals(listOf("project-one"),LaunchPresentation.projects(sanitized).map { it.id })
        assertEquals("/example/source",LaunchPresentation.projects(sanitized).single().folders.single().path)
        assertEquals("project-one",sanitized.getString("default_project"))
    }
    @Test fun projectFolderIsExactAndOutsideFolderAdditionIsExplicit() {
        val raw=catalog()
        val existing=LaunchPresentation.projectArguments(raw,"project-one","/example/source")
        assertEquals("swarm-exact",existing.getString("swarm_id"))
        assertEquals("folder-one",existing.getString("project_folder_id"))
        assertFalse(existing.getBoolean("add_folder"))
        val outside=LaunchPresentation.projectArguments(raw,"project-one","/example/outside")
        assertTrue(outside.getBoolean("add_folder"))
        assertFalse(outside.has("project_folder_id"))
        assertThrows(IllegalStateException::class.java) { LaunchPresentation.projectArguments(raw,"hidden","/example/source") }
        assertTrue(LaunchPresentation.projects(raw.put("project_launch_supported",false)).isEmpty())
    }
    @Test fun verifiedBrowserAcceptsSelectedSymlinkPathWithoutInventingAnotherPath() {
        val browser=JSONObject().put("path","/example/canonical").put("requested_path","/example/symlink")
        assertTrue(LaunchPresentation.selectedFolder(browser,"/example/symlink"))
        assertFalse(LaunchPresentation.selectedFolder(browser,"/example/other"))
    }
    @Test fun createdSessionWithMissingAssignmentRemainsCompletedAndDurablyReviewable() {
        val target=Target("computer","","","","workspace")
        val args=JSONObject().put("agent","claude").put("directory","/example/source").put("tag","new session")
            .put("account_id","profile-two").put("swarm_id","swarm-exact").put("project_id","project-one").put("project_folder_id","folder-one").put("add_folder",false)
        SessionActionPolicies.arguments("launch",args)
        val action=SessionAction("request",target,"launch",args.toString())
        val native=JSONObject().put("request_id","request").put("status","created").put("result_target",JSONObject()
            .put("name","claude/source/new session").put("run_id","new-run").put("conversation_id","new-conversation"))
        val receipt=JSONObject().put("request_id","request").put("state","completed").put("result",native)
        val completed=SessionActionPolicies.result(action,receipt)
        assertEquals("completed",completed.status)
        assertFalse(completed.blocksSending)
        assertTrue(completed.needsProjectReview)
        assertEquals("new-run",completed.resultTarget?.run)
        var state=MessageState().action(completed)
        repeat(150) { state=state.action(action.copy(requestId="other-$it",status="completed",arguments="{}")) }
        val restored=MessageCodec.state(MessageCodec.stateJson(state))
        assertTrue(restored.sessionActions.single { it.requestId=="request" }.needsProjectReview)
        val reviewed=state.action(completed.copy(status="reviewed"))
        assertFalse(reviewed.sessionActions.find { it.requestId=="request" }?.needsProjectReview==true)
        native.put("project_assignment",JSONObject().put("status","assigned").put("swarm_id","wrong").put("project_id","project-one"))
        assertTrue(SessionActionPolicies.result(action,receipt).needsProjectReview)
        native.getJSONObject("project_assignment").put("swarm_id","swarm-exact")
        assertFalse(SessionActionPolicies.result(action,receipt).needsProjectReview)
    }
}
