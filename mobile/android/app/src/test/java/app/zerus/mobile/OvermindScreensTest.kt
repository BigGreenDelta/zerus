package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class OvermindScreensTest {
    private val machine = "22222222-2222-4222-8222-222222222222"
    private val other = "33333333-3333-4333-8333-333333333333"
    private val worker = OvermindSession(machine, "codex/example/worker")
    private val reviewer = OvermindSession(machine, "claude/example/review")
    private fun stages(vararg pairs: Pair<String, String>) = JSONArray(pairs.map { (name, role) -> JSONObject().put("name", name).put("role", role) })
    private val defaultStages = stages("Backlog" to "backlog", "Queue" to "ready", "Design" to "active", "Review" to "review",
        "Your Call" to "needs_human", "Done" to "done", "Icebox" to "parked")
    private fun rule(action: String, from: List<String>, to: String, actors: List<String> = listOf("human"), requires: List<String> = emptyList()) =
        JSONObject().put("action", action).put("from", JSONArray(from)).put("to", to).put("actors", JSONArray(actors)).put("requires", JSONArray(requires))
    private fun workflow(rules: List<JSONObject> = emptyList()) = JSONObject().put("stages", defaultStages).put("rules", JSONArray(rules)).toString()
    private fun session(value: OvermindSession?) = value?.let { JSONObject().put("machine", it.machine).put("name", it.name) } ?: JSONObject.NULL
    private fun task(uuid: String, stage: String, rank: String = "m", labels: List<String> = emptyList(), assignee: OvermindSession? = null,
        queueOf: OvermindSession? = null, pinned: OvermindSession? = null, readOnly: Boolean = false, pinnedCurrent: String = "") = checkNotNull(OvermindParser.card(
        JSONObject().put("uuid", uuid).put("ref", "om-" + uuid.take(4)).put("project", "example").put("title", "Task $uuid").put("stage", stage)
            .put("priority", 2).put("rank", rank).put("labels", JSONArray(labels)).put("pinned", session(pinned)).put("assignee", session(assignee))
            .put("reviewer", JSONObject.NULL).put("flags", JSONArray()).put("markers", JSONArray()).put("queue_of", session(queueOf))
            .put("read_only", readOnly).put("updated_at", 5).also { if (pinnedCurrent.isNotEmpty()) it.put("pinned_current", pinnedCurrent) }))
    private fun board(cards: List<OvermindCard>, rules: List<JSONObject> = emptyList()) = OvermindParser.board("example", 40, workflow(rules), cards)

    @Test fun columnsFollowTheWorkflowWithYourCallApart() {
        val asked = task("a1", "Your Call", assignee = worker)
        val view = OvermindWorkflow.view(board(listOf(task("q2", "Queue", "n"), task("q1", "Queue", "a"), asked, task("x1", "Retired"), task("d1", "Done"))))
        assertEquals(listOf("Backlog", "Queue", "Design", "Review", "Done", "Icebox", "Retired"), view.columns.map { it.stage.name })
        assertEquals(listOf("q1", "q2"), view.columns[1].cards.map { it.uuid })
        assertEquals(listOf(asked), view.yourCall)
        assertTrue(view.columns.none { column -> column.cards.any { it.uuid == "a1" } })
        // A stage the workflow no longer names stays visible, last, without a role.
        assertEquals("", view.columns.last().stage.role)
        assertEquals(listOf("needs work", "unconfirmed delivery"), OvermindWorkflow.markers(listOf("needs_work", "uncertain")))
        assertEquals(listOf("bug"), OvermindWorkflow.labels(task("l1", "Queue", labels = listOf("reviewed", "bug", "review-failed"))))
        assertEquals("renamed", OvermindSessions.tag(task("p1", "Queue", pinned = worker, pinnedCurrent = "renamed")))
        assertEquals(worker.name, OvermindSessions.tag(task("p2", "Design", assignee = worker, pinned = reviewer)))
    }

    @Test fun humanMovesFollowRulesWithoutRequires() {
        val card = task("t1", "Backlog")
        // Without rules hgs decides, but a person never starts a task by moving it, and never moves to Your Call.
        assertEquals(listOf("Queue", "Review", "Done", "Icebox"), OvermindWorkflow.moveTargets(board(listOf(card)), card).map { it.name })
        val ruled = board(listOf(card), listOf(rule("move", listOf("Backlog"), "Queue"), rule("move", listOf("Backlog"), "Icebox", listOf("agent")),
            rule("move", listOf("Backlog"), "Review", requires = listOf("spec")), rule("move", listOf("Queue"), "Backlog")))
        assertEquals(listOf("Queue"), OvermindWorkflow.moveTargets(ruled, card).map { it.name })
        val working = task("t2", "Design", assignee = worker)
        val unclaim = board(listOf(working), listOf(rule("unclaim", listOf("Design"), "Queue"), rule("move", listOf("Design"), "Queue")))
        assertEquals(listOf("Queue"), OvermindWorkflow.moveTargets(unclaim, working).map { it.name })
        assertTrue(OvermindWorkflow.moveTargets(board(listOf(working), listOf(rule("move", listOf("Design"), "Queue"))), working).isEmpty())
        val waiting = task("t3", "Your Call", assignee = worker)
        assertEquals(listOf("Icebox"), OvermindWorkflow.moveTargets(board(listOf(waiting)), waiting).map { it.name })
        assertTrue(OvermindWorkflow.moveTargets(board(listOf(task("t4", "Queue", readOnly = true))), task("t4", "Queue", readOnly = true)).isEmpty())
        assertTrue(OvermindWorkflow.claimable(board(emptyList()), "Queue"))
        assertFalse(OvermindWorkflow.claimable(board(emptyList()), "Backlog"))
        assertTrue(OvermindWorkflow.claimable(board(emptyList(), listOf(rule("claim", listOf("Backlog"), "Design"))), "Backlog"))
        assertFalse(OvermindWorkflow.claimable(board(emptyList(), listOf(rule("claim", listOf("Backlog"), "Design"))), "Queue"))
    }

    @Test fun movesThatTakeBackOrReopenAskFirst() {
        val working = task("t1", "Design", assignee = worker)
        val all = board(listOf(working, task("t2", "Done"), task("t3", "Your Call", assignee = worker), task("t4", "Backlog")))
        val back = checkNotNull(OvermindWorkflow.moveQuestion(all, working, "Queue"))
        assertEquals("Take back", back.accept)
        assertTrue(back.text, back.text.contains("Take om-t1 back from codex/example/worker"))
        val reopen = checkNotNull(OvermindWorkflow.moveQuestion(all, all.cards.first { it.uuid == "t2" }, "Queue"))
        assertEquals("Reopen", reopen.accept)
        assertTrue(OvermindWorkflow.moveQuestion(all, all.cards.first { it.uuid == "t3" }, "Icebox")!!.text.contains("without an answer"))
        assertNull(OvermindWorkflow.moveQuestion(all, all.cards.first { it.uuid == "t4" }, "Queue"))
    }

    @Test fun reorderNamesTheCardBelowAndSkipsDropsInPlace() {
        val column = listOf("a", "b", "c", "d")
        assertEquals(JSONObject().put("task", "d").put("before", "b").toString(), OvermindWorkflow.reorder(column, "d", 1).toString())
        assertEquals(JSONObject().put("task", "a").toString(), OvermindWorkflow.reorder(column, "a", 3).toString())
        assertEquals(JSONObject().put("task", "a").put("before", "c").toString(), OvermindWorkflow.reorder(column, "a", 1).toString())
        assertNull(OvermindWorkflow.reorder(column, "b", 1))
        assertNull(OvermindWorkflow.reorder(column, "missing", 0))
        assertEquals(JSONObject().put("task", "c").put("before", "a").toString(), OvermindWorkflow.reorder(column, "c", -4).toString())
        assertEquals(JSONObject().put("task", "b").toString(), OvermindWorkflow.reorder(column, "b", 9).toString())
    }

    @Test fun badgesCountQueuesAndYourCall() {
        val digest = OvermindDigest(7, mapOf("example" to mapOf("ready" to 3L, "needs_human" to 1L, "active" to 2L)), emptyList())
        assertEquals(OvermindBadge(3, 1), OvermindBadges.project(digest, "example"))
        assertEquals(OvermindBadge(0, 0), OvermindBadges.project(digest, "absent"))
        assertNull(OvermindBadges.project(null, "example"))
        val elsewhere = OvermindSession(other, worker.name)
        val cards = board(listOf(task("q1", "Queue", queueOf = worker), task("q2", "Queue", queueOf = worker), task("q3", "Queue", queueOf = elsewhere),
            task("q4", "Queue"), task("y1", "Your Call", assignee = worker), task("y2", "Your Call", assignee = reviewer), task("w1", "Design", assignee = worker)))
        assertEquals(OvermindBadge(2, 1), OvermindBadges.session(cards, worker))
        assertEquals(OvermindBadge(0, 1), OvermindBadges.session(cards, reviewer))
        assertNull(OvermindBadges.session(cards, null))
        assertNull(OvermindBadges.session(null, worker))
        assertTrue(OvermindBadge(0, 0).empty)
    }

    @Test fun addAsTaskPayloadPinsTheQueueOrUsesThePoolOrBacklog() {
        val workflowStages = OvermindParser.stages(JSONObject(workflow()))
        val queue = OvermindRequests.addAsTask("example", "Fix the login flow", OvermindDestination.Queue, worker, workflowStages)
        assertEquals(JSONObject().put("project", "example").put("prompt", "Fix the login flow").put("stage", "Queue")
            .put("pinned", JSONObject().put("machine", machine).put("name", worker.name)).toString(), queue.toString())
        val pool = OvermindRequests.addAsTask("example", "Fix it", OvermindDestination.Pool, worker, workflowStages)
        assertEquals("Queue", pool.getString("stage")); assertFalse(pool.has("pinned"))
        val backlog = OvermindRequests.addAsTask("example", "Fix it", OvermindDestination.Backlog, null, emptyList())
        assertEquals(setOf("project", "prompt"), backlog.keys().asSequence().toSet())
        assertFalse(queue.has("request_id"))
        assertEquals("Overmind does not know this session's computer yet.",
            runCatching { OvermindRequests.addAsTask("example", "Fix it", OvermindDestination.Queue, null, workflowStages) }.exceptionOrNull()?.message)
        assertEquals("The project's workflow has no Ready stage.",
            runCatching { OvermindRequests.addAsTask("example", "Fix it", OvermindDestination.Pool, worker, emptyList()) }.exceptionOrNull()?.message)
        assertEquals("Write the task's prompt first.",
            runCatching { OvermindRequests.addAsTask("example", "  ", OvermindDestination.Backlog, worker, workflowStages) }.exceptionOrNull()?.message)
        val image = Attachment("44444444-4444-4444-8444-444444444444", "screen.png", "image/png", 10, "a".repeat(64))
        val notes = image.copy(id = "55555555-5555-4555-8555-555555555555", name = "notes.txt", mime = "text/plain")
        assertEquals("", OvermindRequests.addReason("Fix it", listOf(image)))
        assertEquals("Only images can be added to a task. Remove notes.txt first.", OvermindRequests.addReason("Fix it", listOf(image, notes)))
        assertEquals("Write the task's prompt first.", OvermindRequests.addReason("", listOf(image)))
        // An add still running, or a create of this prompt that may have been recorded, blocks a second one.
        assertEquals(OvermindRequests.ADDING, OvermindRequests.addReason("Fix it", listOf(image), adding = true))
        assertEquals(OvermindRequests.MAYBE_ADDED, OvermindRequests.addReason("Fix it", listOf(image), pending = true))
        assertEquals(JSONObject().put("task", "t1").put("session", JSONObject().put("machine", machine).put("name", worker.name)).toString(),
            OvermindRequests.send("t1", worker).toString())
    }

    private fun refused(action: String, request: JSONObject, code: String, field: String = "confirm") =
        OvermindWrites.create("66666666-6666-4666-8666-666666666666", MachineKey("workspace", GATEWAY), action, request, emptyList(), "Pixel", 1)
            .copy(status = "refused", code = code, field = field, error = "busy")

    @Test fun confirmableRefusalsOfferAnywayAsANewRequestWithTheCodeConfirmed() {
        val context = OvermindWriteContext("om-t1", worker.name)
        listOf("agent_busy", "wip_exceeded", "predecessors_open", "pinned_elsewhere").forEach { code ->
            val write = refused("send", OvermindRequests.send("t1", worker), code)
            val outcome = checkNotNull(OvermindOutcomes.of(write, context))
            assertEquals(OvermindTone.Error, outcome.tone)
            val anyway = checkNotNull(outcome.anyway)
            assertEquals("Send om-t1 to codex/example/worker anyway", anyway.label)
            assertEquals("send", anyway.action)
            assertEquals(JSONArray(listOf(code)).toString(), anyway.request.getJSONArray("confirm").toString())
            assertFalse(anyway.request.has("request_id"))
            // The relay refuses another body under the same id: "anyway" is a new write with a new request_id.
            val again = OvermindWrites.create(java.util.UUID.randomUUID().toString(), write.gateway, anyway.action, anyway.request, emptyList(), "Pixel", 2)
            assertNotEquals(write.requestId, again.requestId)
            assertEquals("t1", again.task)
        }
        // A second refusal keeps the codes already confirmed.
        val second = refused("review-send", OvermindRequests.send("t1", reviewer).put("confirm", JSONArray(listOf("agent_busy"))), "wip_exceeded")
        val review = checkNotNull(OvermindOutcomes.of(second, OvermindWriteContext("om-t1", reviewer.name))?.anyway)
        assertEquals(JSONArray(listOf("agent_busy", "wip_exceeded")).toString(), review.request.getJSONArray("confirm").toString())
        assertEquals("Send om-t1 to claude/example/review for review anyway", review.label)
        assertEquals("Send the rework of om-t1 anyway", OvermindOutcomes.of(refused("rework-send", JSONObject().put("task", "t1"), "agent_busy"), context)!!.anyway!!.label)
        assertEquals("Send the answer anyway", OvermindOutcomes.of(refused("answer", JSONObject().put("task", "t1").put("answer", "Yes"), "agent_busy"), context)!!.anyway!!.label)
        // Only a delivery's confirm field is confirmable; other refusals are text.
        assertNull(OvermindOutcomes.of(refused("send", OvermindRequests.send("t1", worker), "epic", "") , context)!!.anyway)
        assertNull(OvermindOutcomes.of(refused("move", JSONObject().put("task", "t1").put("to", "Done"), "agent_busy"), context)!!.anyway)
    }

    @Test fun uncertainDeliveriesOfferResolveAndNeverRetry() {
        val base = OvermindWrites.create("77777777-7777-4777-8777-777777777777", MachineKey("workspace", GATEWAY), "send", OvermindRequests.send("t1", worker), emptyList(), "Pixel", 1)
        val context = OvermindWriteContext("om-t1", worker.name, "Sent om-t1 to ${worker.name}.")
        val maybe = OvermindOutcomes.of(base.copy(status = "uncertain", delivery = "uncertain", error = OvermindWrites.RESOLVE), context)!!
        assertEquals("t1", maybe.resolve); assertNull(maybe.retry); assertNull(maybe.anyway)
        assertTrue(maybe.text, maybe.text.contains("may have been sent"))
        assertEquals(OvermindTone.Warning, maybe.tone)
        assertEquals("t1", OvermindOutcomes.of(base.copy(status = "uncertain", code = "delivery_unknown"), context)!!.resolve)
        val lost = base.copy(status = "uncertain", error = OvermindWrites.UNCONFIRMED)
        assertEquals(lost, OvermindOutcomes.of(lost, context)!!.retry)
        val check = OvermindOutcomes.of(base.copy(status = "uncertain", code = OvermindWrites.CHECK, error = OvermindWrites.CHECK_TASK), context)!!
        assertEquals("t1", check.check); assertNull(check.retry)
        assertEquals(OvermindOutcome("Sent om-t1 to ${worker.name}.", OvermindTone.Info), OvermindOutcomes.of(base.copy(status = "completed"), context))
        assertNull(OvermindOutcomes.of(base.copy(status = "completed"), context.copy(done = "")))
        assertNull(OvermindOutcomes.of(base, context))
        assertEquals(OvermindTone.Error, OvermindOutcomes.of(base.copy(status = "failed", error = "Nothing was changed."), context)!!.tone)
    }

    @Test fun anUndeliveredAnswerIsSentAgainByANewRequestThatRepeatsIt() {
        val context = OvermindWriteContext("om-t1", worker.name)
        val first = refused("answer", JSONObject().put("task", "t1").put("answer", "Yes").put("confirm", JSONArray(listOf("agent_busy"))), "not_delivered", "")
        val outcome = checkNotNull(OvermindOutcomes.of(first, context))
        assertTrue(outcome.text, outcome.text.contains("recorded"))
        val again = checkNotNull(outcome.anyway)
        assertEquals("Send the answer again" to "answer", again.label to again.action)
        assertEquals(first.requestId, again.request.getString("repeat"))
        assertEquals("Yes", again.request.getString("answer"))
        assertEquals(JSONArray(listOf("agent_busy")).toString(), again.request.getJSONArray("confirm").toString())
        assertFalse(again.request.has("request_id"))
        assertEquals("Sent your answer on om-t1 to ${worker.name}.", again.context.done)
        // Sent as a new write; when that one is not typed either, the next one still repeats the request that recorded the answer.
        val second = OvermindWrites.create("99999999-9999-4999-8999-999999999999", first.gateway, again.action, again.request, emptyList(), "Pixel", 2)
            .copy(status = "refused", code = "not_delivered", error = "not delivered")
        assertNotEquals(first.requestId, second.requestId)
        assertEquals(first.requestId, OvermindOutcomes.of(second, context)?.anyway?.request?.getString("repeat"))
    }

    @Test fun decisionTapsOpenTheTaskOnlyWhereTasksAreShown() {
        val key = OvermindTaskKey("workspace", GATEWAY, "88888888-8888-4888-8888-888888888888")
        assertNull(OvermindRouting.step(null, available = true, refreshing = false, connected = true))
        assertEquals(OvermindOpenStep.Open, OvermindRouting.step(key, available = true, refreshing = true, connected = true))
        assertEquals(OvermindOpenStep.Wait, OvermindRouting.step(key, available = false, refreshing = true, connected = true))
        assertEquals(OvermindOpenStep.Drop, OvermindRouting.step(key, available = false, refreshing = false, connected = true))
        assertEquals(OvermindOpenStep.Drop, OvermindRouting.step(key, available = true, refreshing = false, connected = false))
    }

    private val project = ProjectSummary(ProjectKey("workspace", "swarm-1", "example"), "Example", "", emptyList(), emptyMap())
    private val live = Session(Target(GATEWAY, worker.name, "run", "conversation", "workspace"), "Worker", "Codex", "running", "Example", "", 0,
        JSONObject().put("state", "running"), project.key)

    @Test fun tasksHiddenWithoutCapabilities() {
        // No gateway serving both operations: no Tasks entry, no sheet item, no composer action.
        assertNull(OvermindEntry.project(false, project))
        assertEquals("example", OvermindEntry.project(true, project))
        // Folder groups and native aliases have no Overmind project.
        assertNull(OvermindEntry.project(true, project.copy(key = ProjectKey("workspace", "folder:$GATEWAY", "/work"), catalogued = false)))
        val machines = listOf(Machine("workspace", GATEWAY, "Gateway", true, nodeId = machine))
        assertNull(OvermindEntry.sendTaskReason(false, live, listOf(project), machines))
        assertEquals("", OvermindEntry.sendTaskReason(true, live, listOf(project), machines))
        assertEquals("Overmind does not know this session's computer yet.",
            OvermindEntry.sendTaskReason(true, live, listOf(project), listOf(machines.single().copy(nodeId = ""))))
        assertNull(OvermindEntry.sendTaskReason(true, live.copy(target = live.target.copy(archiveId = "archived")), listOf(project), machines))
        val target = live.target
        val facts = SessionMenuFacts(target, live.raw, "running", false, true, emptySet())
        assertTrue(SessionActionsMenu.items(facts).flatten().none { it.id == "send_task" })
        val offered = SessionActionsMenu.items(facts.copy(tasks = "")).flatten().single { it.id == "send_task" }
        assertEquals("Send a task here…", offered.label); assertTrue(offered.enabled)
        assertFalse(SessionActionsMenu.items(facts.copy(tasks = "Overmind does not know this session's computer yet.")).flatten().single { it.id == "send_task" }.enabled)
    }

    @Test fun anOpenWindowOutlivesAFailedRefresh() {
        val both = OvermindAccess.operations
        // A failed refresh leaves the gateway only last known: the window, its drafts and its messages stay.
        assertFalse(OvermindEntry.withdrawn(true, both, freshMachines = false, gateway = false))
        assertFalse(OvermindEntry.withdrawn(true, null, freshMachines = false, gateway = false))
        assertFalse(OvermindEntry.withdrawn(true, both, freshMachines = true, gateway = true))
        assertTrue(OvermindEntry.withdrawn(false, both, freshMachines = true, gateway = true))
        assertTrue(OvermindEntry.withdrawn(true, setOf("send"), freshMachines = false, gateway = false))
        assertTrue(OvermindEntry.withdrawn(true, both, freshMachines = true, gateway = false))
    }

    @Test fun sessionRefsUseTheOvermindMachineOfTheComputer() {
        val record = JSONObject().put("id", GATEWAY).put("name", "Gateway").put("online", true).put("machine_id", machine)
        assertEquals(machine, MachineCatalog.parse("workspace", record).nodeId)
        val fromCatalog = JSONObject().put("id", GATEWAY).put("name", "Gateway").put("snapshot", JSONObject().put("mobile_projects", JSONObject().put("node_id", other)))
        assertEquals(other, MachineCatalog.parse("workspace", fromCatalog).nodeId)
        assertEquals("", MachineCatalog.parse("workspace", JSONObject().put("id", GATEWAY).put("machine_id", "not-a-uuid")).nodeId)
        val machines = listOf(MachineCatalog.parse("workspace", record))
        assertEquals(worker, OvermindSessions.ref(live, machines))
        assertNull(OvermindSessions.ref(live.copy(target = live.target.copy(agentId = "child")), machines))
        assertNull(OvermindSessions.ref(live, emptyList()))
    }

    @Test fun sendChoicesOfferThisSessionsQueueThenThePool() {
        val mine = task("q2", "Queue", "b", queueOf = worker, pinned = worker)
        val pool = task("q1", "Queue", "a")
        val reserved = task("q3", "Queue", "c", queueOf = reviewer, pinned = reviewer)
        val choices = OvermindSendChoices.of(board(listOf(pool, mine, reserved, task("e1", "Queue", "d", labels = listOf("epic")),
            task("r1", "Queue", "e", readOnly = true), task("b1", "Backlog"))), worker)
        assertEquals(listOf("q2", "q1", "q3"), choices.map { it.card.uuid })
        assertEquals(listOf("In this session's queue", "Project pool", "Reserved for claude/example/review"), choices.map { it.note })
    }

    @Test fun recipientsAreTheProjectsLiveSessionsWithTheirComputers() {
        val machines = listOf(Machine("workspace", GATEWAY, "Gateway", true, nodeId = machine), Machine("workspace", "unknown", "Laptop", true))
        val second = live.copy(target = live.target.copy(session = reviewer.name), title = "Reviewer")
        val stranger = live.copy(target = live.target.copy(computerId = "unknown", session = "codex/example/other"))
        val archived = live.copy(target = live.target.copy(session = "old", archiveId = "a1"))
        val elsewhere = live.copy(target = live.target.copy(session = "codex/other/x"), projectKey = ProjectKey("workspace", "swarm-1", "other"))
        val card = task("t1", "Review", assignee = worker, pinned = reviewer)
        val review = OvermindRecipients.of(listOf(live, second, stranger, archived, elsewhere), machines, project.key, card, OvermindPurpose.Review)
        assertEquals(listOf(worker.name, reviewer.name, "codex/example/other"), review.map { it.session.target.session })
        assertEquals(listOf(false, true, false), review.map { it.enabled })
        assertEquals(listOf("did the work", "", "its computer is not known yet"), review.map { it.note })
        val pin = OvermindRecipients.of(listOf(live, second), machines, project.key, card, OvermindPurpose.Pin)
        assertEquals(listOf(false, true), pin.map { it.checked })
    }

    @Test fun journalShowsTheCurrentQuestionAndWhoWrote() {
        val raw = JSONObject().put("ok", true).put("task", JSONObject().put("uuid", "t1").put("ref", "om-t1").put("project", "example").put("title", "T")
            .put("stage", "Your Call").put("head", "e2").put("prompt", "Do it").put("journal", JSONArray(listOf(
                JSONObject().put("marker", "[needs-human]").put("text", "Old question").put("event", "e0").put("actor", JSONObject().put("kind", "agent").put("session", "codex/x")),
                JSONObject().put("marker", "[needs-human]").put("text", "Which API?").put("event", "e2").put("actor", JSONObject().put("kind", "agent").put("session", "codex/x")),
                JSONObject().put("marker", "[comment]").put("text", "Note").put("event", "e3").put("actor", JSONObject().put("kind", "human").put("device", "Pixel 9"))))))
        val view = OvermindParser.task(raw)
        assertEquals("Which API?", OvermindJournal.question(view))
        assertEquals("", OvermindJournal.question(view.copy(head = "e9")))
        assertEquals(listOf("codex/x", "codex/x", "a person on Pixel 9"), view.journal.map(OvermindJournal::author))
        assertEquals("a person", OvermindJournal.author(view.journal.last().copy(actorDevice = "")))
    }

    @Test fun onlyBytesThatDecodeAsAnImageAreShown() {
        fun bytes(vararg values: Int) = values.map { it.toByte() }.toByteArray() + ByteArray(16)
        assertEquals("png", OvermindImages.format(bytes(0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)))
        assertEquals("jpeg", OvermindImages.format(bytes(0xff, 0xd8, 0xff, 0xe0)))
        assertEquals("gif", OvermindImages.format("GIF89a".toByteArray() + ByteArray(8)))
        assertEquals("webp", OvermindImages.format("RIFF".toByteArray() + ByteArray(4) + "WEBP".toByteArray() + ByteArray(4)))
        assertEquals("bmp", OvermindImages.format("BM".toByteArray() + ByteArray(20)))
        // An agent names an image: a script called screen.png is still not a picture.
        assertNull(OvermindImages.format("#!/bin/sh\necho".toByteArray()))
        assertNull(OvermindImages.format(ByteArray(0)))
    }
}
