package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class LaunchFormTest {
    private val projects=listOf(
        LaunchProject("p1","Zerus","#67e8cb",emptyList()),
        LaunchProject("p2","Infra","#ffcb7d",listOf(LaunchProjectFolder("f1","/srv/deploy/terraform","Terraform"))),
        LaunchProject("p3","Website","",listOf(LaunchProjectFolder("f2","/example/site","Site"),LaunchProjectFolder("f3","/example/site-docs","Docs"))))
    private fun entry(path:String,kind:String="linked",available:Boolean=true)=JSONObject().put("path",path).put("kind",kind).put("available",available)
    private fun catalog(requested:String="/repo",path:String=requested)=JSONObject().put("state","ok").put("stale",false).put("partial",false)
        .put("path",path).put("requested_path",requested).put("selected_root","/repo").put("common_dir","/repo/.git")
        .put("worktrees",JSONArray().put(entry("/repo","main")).put(entry("/repo-feature")))

    @Test fun projectSearchKeepsCatalogOrderAndMatchesNamesFoldersAndPaths() {
        assertEquals(listOf("p1","p2","p3"),LaunchPresentation.filterProjects(projects,"  ").map { it.id })
        assertEquals(listOf("p1"),LaunchPresentation.filterProjects(projects,"zer").map { it.id })
        assertEquals(listOf("p2"),LaunchPresentation.filterProjects(projects,"TERRAFORM").map { it.id })
        assertEquals(listOf("p2"),LaunchPresentation.filterProjects(projects,"/srv/deploy").map { it.id })
        assertEquals(listOf("p3"),LaunchPresentation.filterProjects(projects," docs ").map { it.id })
        assertEquals(listOf("p1","p2"),LaunchPresentation.filterProjects(projects,"r").map { it.id })
        assertTrue(LaunchPresentation.filterProjects(projects,"absent").isEmpty())
    }
    @Test fun projectSubtitleNamesThisComputersFolders() {
        assertEquals("No folder on this computer",LaunchPresentation.folderSummary(projects[0]))
        assertEquals("/srv/deploy/terraform",LaunchPresentation.folderSummary(projects[1]))
        assertEquals("2 folders",LaunchPresentation.folderSummary(projects[2]))
    }
    @Test fun automaticWorktreeNeedsAFreshCompleteCatalogOfTheSelectedCheckout() {
        assertTrue(WorktreePresentation.automaticAvailable(catalog(),"/repo"))
        // A folder inside a checkout qualifies, as on desktop; the catalog must belong to this selection.
        assertTrue(WorktreePresentation.automaticAvailable(catalog("/repo/app"),"/repo/app"))
        assertTrue(WorktreePresentation.automaticAvailable(catalog("~/repo","/repo"),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog(),"/other"))
        assertFalse(WorktreePresentation.automaticAvailable(null,"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("state","not_repo"),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("stale",true),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("partial",true),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("common_dir",""),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("selected_root","/"),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("selected_root","relative"),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("worktrees",JSONArray().put(entry("/repo","bare"))),"/repo"))
        assertFalse(WorktreePresentation.automaticAvailable(catalog().put("worktrees",JSONArray().put(entry("/repo","main",false))),"/repo"))
        // A sibling with a shared prefix is not inside the checkout.
        assertFalse(WorktreePresentation.automaticAvailable(catalog("/repository").put("worktrees",JSONArray().put(entry("/repo","main"))),"/repository"))
    }
    @Test fun automaticPathIsTheGeneratedSiblingOfTheSelectedRoot() {
        assertEquals("/repo-work-1a2b3c4d",WorktreePresentation.automaticPath(catalog(),"work-1a2b3c4d"))
        assertEquals("/srv/app-work-1a2b3c4d",WorktreePresentation.automaticPath(catalog().put("selected_root","/srv/app/"),"work-1a2b3c4d"))
        assertEquals("",WorktreePresentation.automaticPath(catalog().put("selected_root",""),"work-1a2b3c4d"))
        assertEquals("",WorktreePresentation.automaticPath(catalog(),""))
        assertTrue(LaunchPresentation.generatedName().matches(Regex("work-[0-9a-f]{8}")))
    }
    @Test fun automaticNotesExplainTheDirectFolderFallback() {
        assertEquals("",WorktreePresentation.automaticNote(catalog(),"/repo",true,false))
        assertEquals("",WorktreePresentation.automaticNote(null,"",true,false))
        assertEquals("Checking repository…",WorktreePresentation.automaticNote(null,"/repo",true,true))
        assertTrue(WorktreePresentation.automaticNote(catalog(),"/repo",false,false).startsWith("Update Zerus"))
        assertTrue(WorktreePresentation.automaticNote(catalog().put("state","not_repo"),"/repo",true,false).startsWith("Not a Git repository"))
        assertTrue(WorktreePresentation.automaticNote(catalog().put("stale",true),"/repo",true,false).startsWith("Repository details are unavailable"))
    }
    @Test fun createdCheckoutIsRelatedThroughRetainedEvidenceOnlyWhenFreshEvidenceCannotProveIt() {
        val project=LaunchProject("p","Repo","",listOf(LaunchProjectFolder("main","/repo","Repo")))
        val source=catalog()
        val created=WorktreePresentation.withCreated(source,"/repo-work-1a2b3c4d")
        assertEquals(3,created.getJSONArray("worktrees").length())
        assertEquals(3,WorktreePresentation.withCreated(created,"/repo-work-1a2b3c4d").getJSONArray("worktrees").length())
        assertEquals(2,source.getJSONArray("worktrees").length())
        assertEquals("main",WorktreePresentation.anchor(created,project,"/repo-work-1a2b3c4d")?.id)
        assertNull(WorktreePresentation.anchor(catalog(),project,"/repo-work-1a2b3c4d"))
        assertSame(created,WorktreePresentation.placement(null,created,project,"/repo-work-1a2b3c4d"))
        val fresh=catalog("/repo-work-1a2b3c4d").put("worktrees",JSONArray().put(entry("/repo","main")).put(entry("/repo-work-1a2b3c4d")))
        assertSame(fresh,WorktreePresentation.placement(fresh,created,project,"/repo-work-1a2b3c4d"))
        val unrelated=LaunchProject("u","Other","",listOf(LaunchProjectFolder("x","/elsewhere","Else")))
        assertSame(fresh,WorktreePresentation.placement(fresh,created,unrelated,"/repo-work-1a2b3c4d"))
        // Outside folders keep explicit add-folder semantics.
        val raw=JSONObject().put("project_launch_supported",true).put("worktree_supported",true).put("swarm_id","swarm")
            .put("projects",JSONArray().put(JSONObject().put("id","p").put("name","Repo").put("folders",JSONArray().put(JSONObject().put("id","main").put("path","/repo")))))
        val related=LaunchPresentation.projectArguments(raw,"p","/repo-work-1a2b3c4d",created)
        assertFalse(related.getBoolean("add_folder"))
        assertEquals("main",related.getString("worktree_folder_id"))
        assertEquals("/repo/.git",related.getString("worktree_common_dir"))
        assertTrue(LaunchPresentation.projectArguments(raw,"p","/repo-work-1a2b3c4d",null).getBoolean("add_folder"))
    }
    @Test fun disabledCreateExplainsTheFirstProblem() {
        assertEquals("",LaunchPresentation.disabledReason(LaunchReadiness()))
        assertEquals("This session was already created.",LaunchPresentation.disabledReason(LaunchReadiness(confirmed=true,catalogLoading=true)))
        assertEquals("Refresh the computer list.",LaunchPresentation.disabledReason(LaunchReadiness(machineReason="Refresh the computer list.",folderChosen=false)))
        assertEquals("Loading agents and projects…",LaunchPresentation.disabledReason(LaunchReadiness(catalogLoading=true,catalogAvailable=false)))
        assertEquals("Choose a project.",LaunchPresentation.disabledReason(LaunchReadiness(projectRequired=true,projectChosen=false,folderChosen=false)))
        assertEquals("Browse to choose a folder.",LaunchPresentation.disabledReason(LaunchReadiness(projectHasFolder=false,folderChosen=false)))
        assertEquals("Choose a folder.",LaunchPresentation.disabledReason(LaunchReadiness(folderChosen=false)))
        assertEquals("Checking folder…",LaunchPresentation.disabledReason(LaunchReadiness(folderLoading=true,folderVerified=false)))
        assertEquals("Check this folder again or browse to it.",LaunchPresentation.disabledReason(LaunchReadiness(folderVerified=false)))
        assertEquals("Checking repository…",LaunchPresentation.disabledReason(LaunchReadiness(repositoryLoading=true)))
        assertEquals("No agents are installed on this computer.",LaunchPresentation.disabledReason(LaunchReadiness(providers=0,providerChosen=false)))
        assertEquals("Choose an account.",LaunchPresentation.disabledReason(LaunchReadiness(accountChosen=false)))
        assertEquals("Enter a session name.",LaunchPresentation.disabledReason(LaunchReadiness(nameError=LaunchPresentation.nameError(""))))
        // Worktree problems only block Auto; None remains available.
        assertEquals("",LaunchPresentation.disabledReason(LaunchReadiness(worktreeReason="Check the previous worktree creation receipt.",automaticFailed=true)))
        assertEquals("Check the previous worktree creation receipt.",LaunchPresentation.disabledReason(LaunchReadiness(automatic=true,worktreeReason="Check the previous worktree creation receipt.")))
        assertTrue(LaunchPresentation.disabledReason(LaunchReadiness(automatic=true,automaticFailed=true)).startsWith("Automatic worktree creation failed"))
        assertTrue(LaunchPresentation.disabledReason(LaunchReadiness(automatic=true,automaticUsed=true)).startsWith("This form already created its worktree"))
        assertEquals("",LaunchPresentation.disabledReason(LaunchReadiness(automaticUsed=true)))
    }
}
