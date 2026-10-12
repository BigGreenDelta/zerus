package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionFiltersTest {
    private val machines = listOf(Machine("a", "same", "Computer A", true), Machine("b", "same", "Computer B", false))
    private fun session(raw: String = "{}", connection: String = "a", pending: Int = 0, archive: String = "", name: String = "codex/project/task") =
        Session(Target("same", name, "run", "conversation", connection, archive), "Long title", "codex", "MISLEADING UI STATUS", "Project", "Preview", pending, JSONObject(raw))
    @Test fun filtersFollowSharedDesktopContract() {
        val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/session-filter-contract.json")).bufferedReader().use { it.readText() })
        assertEquals(contract.getJSONArray("filters").let { array -> (0 until array.length()).map(array::getString) }, SessionFilter.entries.map { it.id })
        contract.getJSONArray("cases").objects().forEach { example ->
            val item = session(example.getJSONObject("raw").toString())
            val matches = SessionFilter.entries.filter { SessionFilters.matches(item, it, example.getBoolean("online"), example.getBoolean("draft")) }.map { it.id }
            val expected = example.getJSONArray("matches").let { array -> (0 until array.length()).map(array::getString) }
            assertEquals(example.getString("name"), expected, matches)
        }
    }
    @Test fun localDraftsJoinActiveOnceWithinMachineProjectAndSearchScope() {
        val project = ProjectKey("a", "swarm", "project")
        val idle = session("""{"activity":"idle"}""", name="codex/project/draft").copy(projectKey=project)
        val working = session("""{"activity":"busy","phase":"input"}""", name="codex/project/overlap").copy(projectKey=project)
        val offline = idle.copy(target=idle.target.copy(connectionId="b"))
        val archive = idle.copy(target=idle.target.copy(archiveId="archive"))
        val items = listOf(idle, working, offline, archive)
        val local = listOf(Draft(idle.target, "unsent"), Draft(working.target, "unsent"),
            Draft(offline.target, "", attachments=listOf(Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "notes.txt", "text/plain", 4, "a".repeat(64)))),
            Draft(archive.target, "unsent"))
        val drafts = SessionFilters.draftedSessions(items, local)
        assertEquals(3, SessionFilters.counts(items, machines, drafts).getValue(SessionFilter.Active))
        assertEquals(1, SessionFilters.counts(items, machines, drafts).getValue(SessionFilter.Attention))
        assertTrue(SessionFilters.matches(offline, SessionFilter.Active, false, offline.target.key in drafts))
        assertFalse(SessionFilters.matches(archive, SessionFilter.Active, true, true))
        val scoped = SessionFilters.scoped(items, machines, setOf(MachineKey("a", "same")), "draft", project)
        assertEquals(listOf(idle, archive), scoped)
        assertEquals(1, SessionFilters.counts(scoped, machines, drafts).getValue(SessionFilter.Active))
        assertEquals(1, SessionFilters.counts(items, machines).getValue(SessionFilter.Active))
    }
    @Test fun draftPresenceExcludesBlankAndDispatchedPayloadsAndKeepsRecoverableSessionDrafts() {
        val item = session("""{"activity":"idle","subagents":{"child":{"state":"idle"}}}""")
        val editing = Draft(item.target, "unsent")
        listOf(editing.copy(text=" \n\t"), editing.copy(status="submitting"), editing.copy(status="submitted"),
            editing.copy(status="recorded"), editing.copy(detachedId="detached"), editing.copy(questionId="question"),
            editing.copy(target=item.target.copy(connectionId="b"))).forEach { excluded ->
            assertTrue(SessionFilters.draftedSessions(listOf(item), listOf(excluded)).isEmpty())
        }
        assertEquals(setOf(item.target.key), SessionFilters.draftedSessions(listOf(item), listOf(editing.copy(target=item.target.copy(run="old-run", conversation="old-conversation")))))
        val child = editing.copy(target=item.target.copy(conversation="child-conversation", agentId="child", parentConversation=item.target.conversation))
        assertEquals(setOf(item.target.key), SessionFilters.draftedSessions(listOf(item), listOf(child)))
        assertTrue(SessionFilters.draftedSessions(listOf(item), listOf(child.copy(target=child.target.copy(parentConversation="older-parent")))).isEmpty())
        val profile = item.copy(raw=JSONObject(item.raw.toString()).put("subagent_source", "hook_profiles"))
        assertTrue(SessionFilters.draftedSessions(listOf(profile), listOf(child)).isEmpty())
    }
    @Test fun identitySearchFindsSavedArchiveAndChildIdsWithinTheMachineScope() {
        val item = session("""{"state":"paused","subagents":{"agent-one":{"conversation_id":"child-conversation"}}}""", archive="archive-one")
        listOf("CONVERSATION", "versat", "run", "archive-one", "agent-one", "child-conversation").forEach { query ->
            assertEquals(listOf(item), SessionFilters.scoped(listOf(item), machines, emptySet(), query))
            assertTrue(SessionFilters.scoped(listOf(item), machines, setOf(MachineKey("b", "same")), query).isEmpty())
        }
        val profiles = item.copy(raw=JSONObject("""{"subagent_source":"hook_profiles","subagents":{"profile-only":{"conversation_id":"profile-conversation"}}}"""))
        assertTrue(SessionFilters.scoped(listOf(profiles), machines, emptySet(), "profile-only").isEmpty())
        val preview = item.copy(raw=JSONObject("""{"subagent_previews":[{"agent_id":"preview-child","conversation_id":"preview-conversation"}]}"""))
        assertEquals(listOf(preview), SessionFilters.scoped(listOf(preview), machines, emptySet(), "preview-child"))
        val mention = item.copy(preview="Working on task zerus-ab12")
        assertEquals(listOf(mention), SessionFilters.scoped(listOf(mention), machines, emptySet(), "zerus-ab12"))
    }
    @Test fun omittedLifecycleDefaultsRunningAsDesktopDoes() {
        val item = session("""{"activity":"busy","phase":"tool","process_state":"running"}""")
        assertEquals("running", SessionFilters.state(item))
        assertTrue(SessionFilters.matches(item, SessionFilter.Active, true))
        assertFalse(SessionFilters.matches(item, SessionFilter.Saved, true))
    }
    @Test fun activeWorkRequiresReachableCurrentProcessAndNeverDisplayString() {
        val item = session("""{"activity":"busy","phase":"working","state":"running"}""")
        assertTrue(SessionFilters.matches(item, SessionFilter.Active, true))
        assertFalse(SessionFilters.matches(item, SessionFilter.Active, false))
        listOf("""{"activity":"busy","process_state":"exited"}""", """{"activity":"busy","state":"paused"}""",
            """{"activity":"busy","conversation_state":"ended"}""", """{"activity":"unknown"}""", """{"activity":"idle"}""").forEach {
            assertFalse(SessionFilters.matches(session(it), SessionFilter.Active, true))
        }
    }
    @Test fun activeIncludesAttentionForInputApprovalAndNonRecoveringError() {
        listOf("input", "approval", "error").forEach { phase ->
            val item = session(JSONObject().put("activity", "busy").put("phase", phase).toString())
            assertTrue(SessionFilters.matches(item, SessionFilter.Attention, true))
            assertTrue(SessionFilters.matches(item, SessionFilter.Active, true))
            assertFalse(SessionFilters.matches(item, SessionFilter.Attention, false))
            assertFalse(SessionFilters.matches(item, SessionFilter.Active, false))
        }
    }
    @Test fun activeCountsUnionOnceAndPreservesExplicitAttentionOffline() {
        val working = session("""{"activity":"busy","phase":"tool"}""", name = "codex/project/working")
        val attention = session("""{"activity":"idle","phase":"input"}""", name = "codex/project/input")
        val overlap = session("""{"activity":"busy"}""", pending = 1, name = "codex/project/optional-question")
        val child = session("""{"activity":"idle","subagents":{"child":{"display_state":"input"}}}""", name = "codex/project/child")
        val reminder = session("""{"state":"paused","review_later":true}""", connection = "b", name = "codex/project/reminder")
        val archived = session("""{"activity":"busy","phase":"input","review_later":true}""", archive = "archive-one")
        val saved = session("""{"state":"paused","activity":"busy"}""")
        val items = listOf(working, attention, overlap, child, reminder, archived, saved)
        val counts = SessionFilters.counts(items, machines)
        assertEquals(5, counts.getValue(SessionFilter.Active))
        assertEquals(4, counts.getValue(SessionFilter.Attention))
        assertEquals(listOf(working, attention, overlap, child, reminder), items.filter {
            SessionFilters.matches(it, SessionFilter.Active, SessionFilters.online(it, machines))
        })
        assertTrue(SessionFilters.matches(reminder, SessionFilter.Active, false))
        assertFalse(SessionFilters.matches(attention, SessionFilter.Active, false))
        assertFalse(SessionFilters.matches(session("""{"phase":"approval","attention_acknowledged":true}"""), SessionFilter.Active, true))
    }
    @Test fun scheduledRecoveryErrorDoesNotRequestAttentionButBlockedRecoveryDoes() {
        listOf("waiting", "dispatching", "retrying").forEach { recovery ->
            assertFalse(SessionFilters.needsAction(session(JSONObject().put("phase", "error").put("recovery", JSONObject().put("state", recovery)).toString())))
        }
        listOf("uncertain", "blocked", "exhausted").forEach { recovery ->
            assertTrue(SessionFilters.needsAction(session(JSONObject().put("phase", "idle").put("recovery", JSONObject().put("state", recovery)).toString())))
        }
        assertFalse(SessionFilters.needsAction(session("""{"state":"paused","recovery":{"state":"blocked"}}""")))
    }
    @Test fun optionalQuestionsAndExplicitChildRequestsCountButRoutineFailuresDoNot() {
        assertTrue(SessionFilters.needsAttention(session("""{"activity":"busy"}""", pending = 1), true))
        assertFalse(SessionFilters.needsAttention(session("""{"activity":"busy"}""", pending = 1), false))
        listOf("input", "approval", "attention").forEach { child ->
            assertTrue(SessionFilters.needsAttention(session(JSONObject().put("activity", "busy").put("subagents", JSONObject().put("child", JSONObject().put("display_state", child))).toString()), true))
        }
        assertFalse(SessionFilters.needsAttention(session("""{"activity":"busy","subagents":{"child":{"state":"error"}}}"""), true))
        assertFalse(SessionFilters.needsAttention(session("""{"activity":"busy","subagent_source":"hook_profiles","subagents":{"child":{"display_state":"input"}}}"""), true))
        assertTrue(SessionFilters.needsAttention(session("""{"activity":"busy","subagent_source":"hooks","subagent_previews":[{"state":"input"}]}"""), true))
    }
    @Test fun exposedReviewFlagCanRemainOfflineButUnreadAndStaleApprovalCannot() {
        assertTrue(SessionFilters.needsAttention(session("""{"review_later":true}"""), false))
        assertFalse(SessionFilters.needsAttention(session("""{"unread_reply":true,"phase":"approval"}"""), false))
        assertFalse(SessionFilters.needsAttention(session("""{"phase":"approval","attention_acknowledged":true}"""), true))
    }
    @Test fun archivesAreExclusiveAndSavedMeansNonRunning() {
        val archived = session("""{"state":"archived","activity":"busy","phase":"input"}""", archive = "archive-one")
        SessionFilter.entries.filterNot { it == SessionFilter.Archive }.forEach { assertFalse(SessionFilters.matches(archived, it, true)) }
        assertTrue(SessionFilters.matches(archived, SessionFilter.Archive, false))
        listOf("paused", "stopped").forEach { state -> assertTrue(SessionFilters.matches(session(JSONObject().put("state", state).toString()), SessionFilter.Saved, true)) }
        assertFalse(SessionFilters.matches(session(), SessionFilter.Archive, true))
    }
    @Test fun machineScopeNeverConflatesSameIdAcrossWorkspaces() {
        val a = session("""{"activity":"busy"}""")
        val b = session("""{"activity":"busy"}""", connection = "b")
        val scoped = SessionFilters.scoped(listOf(a, b), machines, setOf(MachineKey("a", "same")), "")
        assertEquals(listOf(a), scoped)
        assertNotEquals(MachineKey("a", "same").key, MachineKey("b", "same").key)
        assertEquals(1, SessionFilters.counts(scoped, machines).getValue(SessionFilter.Active))
    }
    @Test fun searchAndProjectScopeApplyBeforeEveryCounterAndCards() {
        val key = ProjectKey("a", "swarm", "project")
        val a = session("""{"activity":"busy","git_branch":"feature/synthetic"}""").copy(projectKey = key)
        val b = session("""{"activity":"busy"}""").copy(projectKey = ProjectKey("a", "swarm", "other"))
        val scoped = SessionFilters.scoped(listOf(a, b), machines, emptySet(), "synthetic", key)
        assertEquals(listOf(a), scoped)
        assertEquals(1, SessionFilters.counts(scoped, machines).getValue(SessionFilter.All))
        assertEquals(1, SessionFilters.counts(scoped, machines).getValue(SessionFilter.Active))
        assertTrue(SessionFilters.scoped(listOf(a), machines, emptySet(), "computer a").isNotEmpty())
    }
    @Test fun groupOrderUsesCatalogAndNeverShowsEmptyGroupsOrMergesNames() {
        val a = ProjectKey("a", "swarm", "a"); val b = ProjectKey("b", "swarm", "b"); val empty = ProjectKey("a", "swarm", "empty")
        val projects = listOf(ProjectSummary(b, "Same", "", emptyList(), emptyMap()), ProjectSummary(empty, "Empty", "", emptyList(), emptyMap()), ProjectSummary(a, "Same", "", emptyList(), emptyMap()))
        val groups = SessionFilters.grouped(listOf(session().copy(projectKey = a), session(connection = "b").copy(projectKey = b)), projects)
        assertEquals(listOf(b, a), groups.map { it.key })
        assertEquals(2, groups.size)
    }
    @Test fun lifecycleStatusAndShortLabelMirrorDesktop() {
        assertEquals("task", SessionFilters.label(session()))
        assertEquals("nested/task", SessionFilters.label(session(name = "codex/project/nested/task")))
        assertEquals("custom", SessionFilters.label(session("""{"tag":"custom"}""")))
        assertEquals("Archived", SessionFilters.status(session("""{"activity":"idle"}""", archive = "archive-one"), true))
        assertEquals("Paused", SessionFilters.status(session("""{"state":"paused","activity":"idle"}"""), true))
        assertEquals("Stopped", SessionFilters.status(session("""{"state":"stopped","activity":"idle"}"""), true))
        assertEquals("Offline", SessionFilters.status(session("""{"activity":"busy"}"""), false))
    }
    @Test fun archiveDatesUseLocalTimeWithSecondsToDistinguishSameMinuteVersions() {
        val zone = java.util.TimeZone.getTimeZone("UTC")
        val a = session("{\"archived_at\":3}", archive = "first")
        val b = session("{\"archived_at\":4}", archive = "second")
        val first = SessionFilters.archiveDate(a, java.util.Locale.US, zone)
        assertTrue(first.contains("12:00:03"))
        assertNotEquals(first, SessionFilters.archiveDate(b, java.util.Locale.US, zone))
        assertNotEquals(first, SessionFilters.archiveDate(a, java.util.Locale.US, java.util.TimeZone.getTimeZone("GMT+03:00")))
    }
    @Test fun missingInvalidOrLiveArchiveDatesAreOmitted() {
        assertEquals("", SessionFilters.archiveDate(session("{\"archived_at\":3}")))
        listOf("{}", "{\"archived_at\":null}", "{\"archived_at\":\"invalid\"}", "{\"archived_at\":0}", "{\"archived_at\":-1}", "{\"archived_at\":1e100}").forEach {
            assertEquals("", SessionFilters.archiveDate(session(it, archive = "first")))
        }
    }

}
