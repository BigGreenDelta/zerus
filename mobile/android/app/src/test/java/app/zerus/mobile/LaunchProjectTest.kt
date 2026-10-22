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
    private fun inferenceCatalog()=catalog().also { it.getJSONArray("projects").put(JSONObject().put("id","ungrouped").put("name","Inbox").put("folders",JSONArray())) }
    private fun infer(raw:JSONObject, directory:String="/example/source", explicit:Boolean=false, worktrees:JSONObject?=null,
                      browser:JSONObject=JSONObject().put("path",directory)) =
        LaunchPresentation.inferredProject(raw,"ungrouped",explicit,directory,browser,worktrees)
    @Test fun untouchedInboxFollowsOnlyExactVerifiedFolderAndExplicitChoiceWins() {
        val raw=inferenceCatalog()
        assertEquals("project-one",infer(raw))
        assertEquals("ungrouped",infer(raw,explicit=true))
        assertEquals("project-one",LaunchPresentation.inferredProject(raw,"project-one",false,"/elsewhere",JSONObject().put("path","/elsewhere"),null))
        assertEquals("ungrouped",infer(raw,"/example/source/nested"))
        assertEquals("ungrouped",infer(raw,browser=JSONObject().put("path","/other")))
        assertEquals("project-one",infer(raw,"/example/link",browser=JSONObject().put("path","/example/source").put("requested_path","/example/link")))
    }
    @Test fun ambiguousOrOtherComputerProjectFoldersNeverChooseAutomatically() {
        val raw=inferenceCatalog()
        val other=JSONObject().put("id","other").put("name","Other").put("folders",JSONArray().put(JSONObject().put("id","other-folder").put("path","/example/source")))
        raw.getJSONArray("projects").put(other)
        assertEquals("ungrouped",infer(raw))
        other.getJSONArray("folders").getJSONObject(0).put("local",false)
        assertEquals("project-one",infer(raw))
        raw.getJSONArray("projects").getJSONObject(0).getJSONArray("folders").getJSONObject(0).put("local",false)
        assertEquals("ungrouped",infer(raw))
    }
    @Test fun relatedWorktreeInferenceRequiresCompleteExactAvailableCatalog() {
        val raw=inferenceCatalog()
        val trees=JSONObject().put("state","ok").put("stale",false).put("partial",false).put("path","/example/linked").put("common_dir","/example/source/.git")
            .put("worktrees",JSONArray().put(JSONObject().put("path","/example/source").put("kind","main").put("available",true))
                .put(JSONObject().put("path","/example/linked").put("kind","linked").put("available",true)))
        assertEquals("project-one",infer(raw,"/example/linked",worktrees=trees))
        for(field in listOf("partial","stale")) {
            assertEquals("ungrouped",infer(raw,"/example/linked",worktrees=JSONObject(trees.toString()).put(field,true)))
        }
        assertEquals("ungrouped",infer(raw,"/example/linked",worktrees=JSONObject(trees.toString()).put("path","/other")))
        trees.getJSONArray("worktrees").getJSONObject(0).put("kind","bare")
        assertEquals("ungrouped",infer(raw,"/example/linked",worktrees=trees))
    }
    @Test fun onlyUntouchedInboxOmitsAssignmentAndNeverAddsAProtocolField() {
        val raw=inferenceCatalog()
        fun args(project:String="ungrouped",implicitInbox:Boolean=false)=LaunchPresentation.arguments(raw,"claude","/example/linked","new-session","profile-two",project,null,implicitInbox=implicitInbox)
        val implicit=args(implicitInbox=true)
        assertEquals(setOf("agent","directory","tag","account_id"),implicit.keys().asSequence().toSet())
        assertFalse(implicit.has("implicitInbox"));assertFalse(implicit.has("implicit_inbox"))
        val explicit=args()
        assertEquals("ungrouped",explicit.getString("project_id"));assertEquals("swarm-exact",explicit.getString("swarm_id"));assertTrue(explicit.getBoolean("add_folder"))
        assertEquals(explicit.toString(),args(implicitInbox=false).toString())
        assertEquals(args("project-one").toString(),args("project-one",true).toString())
        assertEquals("project-one",args("project-one",true).getString("project_id"))
        val noInbox=catalog()
        assertThrows(IllegalStateException::class.java) { LaunchPresentation.arguments(noInbox,"claude","/example/linked","new-session","profile-two","ungrouped",null,implicitInbox=true) }
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
