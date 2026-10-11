package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionCardPresentationTest {
    private val parent = Target("mac", "claude/zerus/mobile", "run", "conversation", "workspace")
    private fun session(raw: String, archive: String = "") =
        Session(parent.copy(archiveId = archive), "Mobile", "claude", "", "zerus", "", 0, JSONObject(raw))
    private fun live(fields: String) = session("""{"activity":"busy",$fields}""")
    private fun count(fields: String, includeCompleted: Boolean, online: Boolean = true) = live(fields).let {
        SessionCardPresentation.childCount(it, online, false, includeCompleted) to SessionCardPresentation.childCount(it, online, true, includeCompleted)
    }

    @Test fun checkoutLineShowsOnlyTheBranchOrDetachedHead() {
        val raw = JSONObject("""{"git_branch":"work-76f5c0ef","git_worktree":true,"git_worktree_name":"zerus-work-76f5c0ef","git_metadata_state":"ok"}""")
        assertEquals("work-76f5c0ef", SessionCardPresentation.branch(raw))
        assertEquals("work-76f5c0ef / worktree: zerus-work-76f5c0ef", SessionCardPresentation.checkout(raw))
        assertEquals("Git branch work-76f5c0ef, linked worktree zerus-work-76f5c0ef", SessionCardPresentation.checkoutDescription(raw))
        val detached = JSONObject("""{"git_branch":"","git_detached":true,"git_worktree":true,"git_metadata_state":"ok"}""")
        assertEquals("detached HEAD", SessionCardPresentation.branch(detached))
        assertEquals("detached HEAD / worktree", SessionCardPresentation.checkout(detached))
        assertEquals("Detached HEAD, linked worktree", SessionCardPresentation.checkoutDescription(detached))
        // A reported branch wins over the detached flag, as in branchContext.
        assertEquals("main", SessionCardPresentation.branch(JSONObject("""{"git_branch":"main","git_detached":true}""")))
        assertEquals("Git branch main", SessionCardPresentation.checkoutDescription(JSONObject("""{"git_branch":"main"}""")))
    }

    @Test fun ordinaryFoldersAndMissingMetadataHaveNoCheckoutLine() {
        listOf("""{"git_branch":"","git_metadata_state":"not_repo"}""", """{"git_branch":"main","git_detached":true,"git_worktree":true,"git_metadata_state":"not_repo"}""",
            """{}""", """{"git_branch":null,"git_detached":"true"}""", """{"git_branch":7}""").forEach {
            val raw = JSONObject(it)
            assertEquals(it, "", SessionCardPresentation.branch(raw))
            assertEquals(it, "", SessionCardPresentation.checkout(raw))
            assertEquals(it, "", SessionCardPresentation.checkoutDescription(raw))
        }
    }

    @Test fun archivedCardsKeepTheirRecordedBranchLikeTheDesktop() {
        val archived = session("""{"state":"archived","git_branch":"relay","git_worktree":true,"git_worktree_name":"zerus-relay"}""", archive = "archive")
        assertEquals("relay", SessionCardPresentation.branch(archived.raw))
    }

    @Test fun childStatusUsesDisplayEvidenceAndTheDesktopCaptions() {
        mapOf("working" to "Working", "finished" to "Ready", "ready" to "Ready", "idle" to "Ready", "error" to "Error", "paused" to "Paused",
            "approval" to "Needs input", "input" to "Needs input", "attention" to "Needs input", "unknown" to "Unknown", "" to "Unknown",
            "failed" to "Unknown").forEach { (state, caption) ->
            assertEquals(state, caption, SessionCardPresentation.childStatus(JSONObject().put("state", state)))
        }
        assertEquals("Needs input", SessionCardPresentation.childStatus(JSONObject("""{"state":"finished","display_state":"input"}""")))
        assertEquals("Working", SessionCardPresentation.childStatus(JSONObject("""{"state":"working","display_state":null}""")))
        assertTrue(SessionCardPresentation.childAction(JSONObject("""{"state":"working","display_state":"approval"}""")))
        assertFalse(SessionCardPresentation.childAction(JSONObject("""{"state":"error"}""")))
    }

    @Test fun inactiveChildrenFollowExplicitEvidenceAndCompleteGroupCounts() {
        listOf("finished", "idle", "ready").forEach { assertTrue(SessionCardPresentation.inactiveChild(JSONObject().put("state", it))) }
        listOf("working", "error", "paused", "input", "unknown", "").forEach { assertFalse(it, SessionCardPresentation.inactiveChild(JSONObject().put("state", it))) }
        assertFalse(SessionCardPresentation.inactiveChild(JSONObject("""{"state":"finished","display_state":"unknown"}""")))
        val group = JSONObject("""{"group":true,"active_count":0}""")
        assertTrue(SessionCardPresentation.inactiveChild(group, countsComplete = true))
        assertFalse(SessionCardPresentation.inactiveChild(group, countsComplete = false))
        assertFalse(SessionCardPresentation.inactiveChild(JSONObject("""{"group":true,"active_count":2}"""), countsComplete = true))
        assertFalse(SessionCardPresentation.inactiveChild(JSONObject("""{"group":true,"state":"working","active_count":0}"""), countsComplete = true))
    }

    @Test fun rowsHideCompletedTurnsKeepDraftsAndUseStableIdOrder() {
        val item = live(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":5,
            "subagents":{"c-ready":{"name":"Ready child","state":"finished","detail":"Done"},
            "a-input":{"name":"Reviewer","state":"working","display_state":"input","detail":"  Approve\n the   plan  ","provider":"codex"},
            "b-working":{"label":"Explorer","name":"ignored","state":"working","task":"Search the code"},
            "d-paused":{"state":"paused","detail":null,"task":"Resume later"},
            "e-error":{"name":"Builder","state":"error"}}""")
        val rows = SessionCardPresentation.children(item, true)
        assertEquals(listOf("a-input", "b-working", "d-paused", "e-error"), rows.map { it.id })
        assertEquals(SessionCardChild("a-input", "Reviewer", "Needs input", "Approve the plan", "codex", DesktopBadgeKind.Attention, false), rows[0])
        assertEquals(SessionCardChild("b-working", "Explorer", "Working", "Search the code", "claude", DesktopBadgeKind.Working, false), rows[1])
        assertEquals(SessionCardChild("d-paused", "d-paused", "Paused", "Resume later", "claude", DesktopBadgeKind.Paused, false), rows[2])
        assertEquals(DesktopBadgeKind.Neutral, rows[3].kind); assertEquals("Error", rows[3].status)
        assertEquals("Subagent Reviewer, Needs input", rows[0].description)
        assertEquals(listOf("a-input", "b-working", "c-ready", "d-paused", "e-error"), SessionCardPresentation.children(item, true, includeCompleted = true).map { it.id })
        assertEquals(listOf("a-input", "b-working", "c-ready", "d-paused", "e-error"), SessionCardPresentation.children(item, true, drafted = setOf("c-ready")).map { it.id })
    }

    @Test fun rowsOfAnUnavailableOrArchivedParentAreRecorded() {
        val fields = """"subagents":{"one":{"name":"One","state":"working","display_state":"input"},"two":{"state":"bogus"}}"""
        val offline = SessionCardPresentation.children(live(fields), false)
        assertEquals(listOf("Recorded", "Recorded"), offline.map { it.status })
        assertEquals(listOf(DesktopBadgeKind.Neutral, DesktopBadgeKind.Neutral), offline.map { it.kind })
        val archived = session("""{"state":"archived","activity":"idle",$fields}""", archive = "archive")
        assertEquals(listOf("Recorded", "Recorded"), SessionCardPresentation.children(archived, true).map { it.status })
        assertEquals("Unknown", SessionCardPresentation.children(live(fields), true)[1].status)
    }

    @Test fun profileGroupsNeverBecomeRowsAndMalformedChildrenStayUnknown() {
        val profiles = live(""""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":8,
            "subagents":{"collapsed":{"label":"Do not invent a child","state":"working"}},
            "subagent_previews":[{"id":"profile:code","label":"code","state":"working","group":true,"active_count":2}]""")
        assertTrue(SessionCardPresentation.roster(profiles.raw).isEmpty())
        assertTrue(SessionCardPresentation.children(profiles, true, includeCompleted = true).isEmpty())
        val counter = SessionCardPresentation.counter(profiles, true, emptyList(), includeCompleted = true)!!
        assertEquals(SessionChildCounter("2/8", "Subagents, 2 active / 8 observed runs", false), counter)
        val malformed = SessionCardPresentation.children(live(""""subagents":{"odd":"text"}"""), true)
        assertEquals(listOf("odd" to "Unknown"), malformed.map { it.name to it.status })
    }

    @Test fun childCountMatchesEveryDesktopBranch() {
        // Session and Overview cards show only active counts unless completed subagents are included.
        assertEquals("3" to "3 active", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3,"subagent_total_count":10""", false))
        assertEquals("" to "", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0,"subagent_total_count":175""", false))
        assertEquals("0+" to "0 confirmed active; some states unknown", count(""""subagent_source":"hooks","subagent_counts_complete":false,"subagent_active_count":0,"subagent_total_count":175""", false))
        assertEquals("?" to "Observed runs: incomplete counts", count(""""subagent_source":"hook_profiles","subagent_counts_complete":false,"subagent_active_count":3,"subagent_total_count":3""", false))
        assertEquals("?" to "Observed runs: incomplete counts", count(""""subagent_source":"hook_profiles","subagent_counts_complete":false,"subagent_active_count":3,"subagent_total_count":3""", true))
        // Active/total forms.
        assertEquals("1/3" to "1 active / 3 total", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":1,"subagent_total_count":3""", true))
        assertEquals("1/175" to "1 active / 175 total", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":1,"subagent_total_count":175""", true))
        assertEquals("2+/5" to "2 confirmed active / 5 observed; some states unknown", count(""""subagent_source":"hooks","subagent_counts_complete":false,"subagent_active_count":2,"subagent_total_count":5""", true))
        assertEquals("2/8" to "2 active / 8 observed runs", count(""""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":8""", true))
        assertEquals("?" to "Observed runs: count unavailable", count(""""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":null""", true))
        assertEquals("3/?" to "3 active / total unknown", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3""", true))
        assertEquals("" to "", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0""", true))
        assertEquals("" to "", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0,"subagent_total_count":0""", true))
        // Only the hooks source reports partial evidence with a plus.
        assertEquals("2/6" to "2 active / 6 total", count(""""subagent_source":"mixed_hooks","subagent_counts_complete":false,"subagent_active_count":2,"subagent_total_count":6""", true))
        assertEquals("2" to "2 active", count(""""subagent_source":"mixed_hooks","subagent_counts_complete":false,"subagent_active_count":2,"subagent_total_count":6""", false))
    }

    @Test fun childCountUsesTheLargerActiveFieldAndNeedsCurrentActivity() {
        assertEquals("4/9" to "4 active / 9 total", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_count":4,"subagent_active_count":2,"subagent_total_count":9""", true))
        assertEquals("5" to "5 active", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_count":5""", false))
        assertEquals("5" to "5 active", count(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_count":5,"subagent_active_count":null""", false))
        val fields = """"subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3,"subagent_total_count":10"""
        assertEquals("" to "", count(fields, true, online = false))
        assertEquals("" to "", count(""""subagent_source":"unavailable","subagent_active_count":3,"subagent_total_count":10""", true))
        listOf("""{"state":"paused","activity":"busy",$fields}""", """{"activity":"unknown",$fields}""", """{"activity":"busy","process_state":"exited",$fields}""",
            """{"activity":"busy","conversation_state":"ended",$fields}""").forEach {
            assertEquals(it, "", SessionCardPresentation.childCount(session(it), true, false, true))
        }
        assertEquals("", SessionCardPresentation.childCount(session("""{"state":"archived","activity":"busy",$fields}""", archive = "archive"), true, false, true))
        assertEquals("3", SessionCardPresentation.childCount(session("""{"activity":"idle",$fields}"""), true))
    }

    @Test fun counterOpensVisibleRowsAndNamesUncountedRowsAgents() {
        val paused = session("""{"state":"paused","activity":"idle","subagent_source":"hooks","subagent_active_count":1,"subagent_total_count":2,
            "subagents":{"one":{"state":"working"}}}""")
        val rows = SessionCardPresentation.children(paused, true)
        assertEquals(SessionChildCounter("Agents", "Subagents", true), SessionCardPresentation.counter(paused, true, rows))
        val working = live(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":1,"subagent_total_count":4,
            "subagents":{"one":{"state":"working"},"two":{"state":"finished"}}""")
        assertEquals(SessionChildCounter("1", "Subagents, 1 active", true),
            SessionCardPresentation.counter(working, true, SessionCardPresentation.children(working, true)))
        val finished = live(""""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0,"subagent_total_count":1,
            "subagents":{"two":{"state":"finished"}}""")
        assertNull(SessionCardPresentation.counter(finished, true, SessionCardPresentation.children(finished, true)))
        assertEquals(SessionChildCounter("0/1", "Subagents, 0 active / 1 total", true), SessionCardPresentation.counter(finished, true,
            SessionCardPresentation.children(finished, true, includeCompleted = true), includeCompleted = true))
    }

    @Test fun draftsKeepOnlyTheirExactParentsChildrenVisible() {
        fun child(agent: String, conversation: String = parent.conversation, text: String = "Continue") =
            Draft(parent.copy(agentId = agent, parentConversation = conversation, conversation = "$conversation/$agent"), text)
        val drafts = listOf(child("kept"), child("blank", text = " "), child("other", conversation = "earlier"), Draft(parent, "Parent draft"),
            Draft(parent.copy(computerId = "arch", agentId = "elsewhere", parentConversation = parent.conversation, conversation = parent.conversation + "/elsewhere"), "x"),
            Draft(parent.copy(agentId = "attached", parentConversation = parent.conversation, conversation = parent.conversation + "/attached"), "",
                attachments = listOf(Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "notes.txt", "text/plain", 4, "a".repeat(64)))))
        assertEquals(setOf("kept", "attached"), SessionCardPresentation.draftedChildren(parent, drafts))
    }

    @Test fun onlyNativeChildIdentitiesOpen() {
        assertTrue(SessionCardPresentation.openableAgent("a62c99618f031a11b"))
        listOf("", " ", "main", "a/b", "a\\b", "a\u0000b", "x".repeat(257)).forEach { assertFalse(it, SessionCardPresentation.openableAgent(it)) }
        assertTrue(SessionCardPresentation.openableAgent("x".repeat(256)))
    }
}
