package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class NotificationCatalogTest {
    private val computer = "22222222-2222-4222-8222-222222222222"
    private val otherComputer = "33333333-3333-4333-8333-333333333333"
    private val alias = "44444444-4444-4444-8444-444444444444"
    private val connection = Connection("workspace", "Example workspace", "https://relay.example", "synthetic-token")
    private fun session(name: String = "raw/session/opaque") = JSONObject().put("name", name).put("tag", "Review requested")
        .put("title", "Provider title").put("run_id", "run").put("conversation_id", "conversation")
        .put("phase", "input").put("state", "running").put("process_state", "running").put("activity", "idle")
    private fun machine(sessions: List<JSONObject> = listOf(session()), id: String = computer, online: Boolean = true) =
        JSONObject().put("id", id).put("name", "Native machine").put("online", online).put("via", JSONObject.NULL)
            .put("snapshot", JSONObject().put("sessions", JSONArray(sessions)))
    private fun parse(rows: List<JSONObject> = listOf(machine()), workspace: Connection = connection) =
        NotificationCatalogParser.parse(workspace, rows, listOf(MachineAlias(MachineKey(connection.id, computer), "QA laptop")))
    private fun event(id: Long = 1, computer: String = this.computer) = NotificationEvent(id, computer, "raw/session/opaque", "attention", 1_000.0)

    @Test fun rawTagAndPrivateMachineNameAppearWithoutOpaqueSlotIds() {
        val current = parse().sessions.single()
        assertEquals("Review requested", current.title)
        assertEquals("QA laptop", current.machine)
        assertEquals("raw/session/opaque", current.slot.session)
        assertEquals(SessionAlertKind.Input, current.kind)
        assertEquals("Needs input on QA laptop", SessionNotice(current, SessionAlertKind.Input, "fingerprint", false).body)
    }

    @Test fun privateLabelDoesNotCrossWorkspaceBoundary() {
        val foreign = parse(workspace = connection.copy(id = "other"))
        assertEquals("Native machine", foreign.sessions.single().machine)
        assertNotEquals(parse().sessions.single().slot.tag, foreign.sessions.single().slot.tag)
    }

    @Test fun aliasesResolveOnlyOneCanonicalMachineAndAmbiguityFailsClosed() {
        val first = machine().put("aliases", JSONArray(listOf(alias)))
        assertEquals(computer, parse(listOf(first)).resolve(connection.id, event(computer = alias))?.first?.computer)
        val second = machine(id = otherComputer).put("aliases", JSONArray(listOf(alias)))
        assertNull(parse(listOf(first, second)).resolve(connection.id, event(computer = alias)))
        assertNull(parse().resolve(connection.id, event(computer = "missing")))
        assertTrue(parse(listOf(first, first)).sessions.isEmpty())
    }

    @Test fun archivePausedExitedAndOfflineDoNotCreateFreshLiveEligibility() {
        val archive = session().put("state", "archived").put("archive_id", "55555555-5555-4555-8555-555555555555")
        assertTrue(parse(listOf(machine(listOf(archive)))).sessions.isEmpty())
        val paused = session().put("state", "paused")
        assertFalse(parse(listOf(machine(listOf(paused)))).sessions.single().live)
        val exited = session().put("process_state", "exited")
        assertFalse(parse(listOf(machine(listOf(exited)))).sessions.single().live)
        assertFalse(parse(listOf(machine(online = false))).sessions.single().online)
        assertTrue(parse(listOf(machine(emptyList()))).sessions.isEmpty())
    }

    @Test fun sameQuestionFactsKeepIdentityAndChangedQuestionChangesIt() {
        val first = session().put("mobile_attention", JSONArray(listOf(JSONObject().put("question_id", "q1").put("question_hash", "h1"))))
        val original = parse(listOf(machine(listOf(first)))).sessions.single()
        assertEquals(original.identity, parse(listOf(machine(listOf(JSONObject(first.toString()))))).sessions.single().identity)
        val changed = JSONObject(first.toString()).put("mobile_attention", JSONArray(listOf(JSONObject().put("question_id", "q2").put("question_hash", "h2"))))
        assertNotEquals(original.identity, parse(listOf(machine(listOf(changed)))).sessions.single().identity)
    }

    @Test fun providerErrorIdentityChangesForNewFailureButRecoveryWaitingIsNotError() {
        val row = session().put("phase", "error").put("provider_error", JSONObject().put("source", "example")
            .put("message_id", "error-1").put("code", "unavailable").put("message", "Synthetic failure"))
        val first = parse(listOf(machine(listOf(row)))).sessions.single()
        assertEquals(SessionAlertKind.Error, first.kind)
        assertEquals(first.identity, parse(listOf(machine(listOf(JSONObject(row.toString()))))).sessions.single().identity)
        row.getJSONObject("provider_error").put("message_id", "error-2")
        assertNotEquals(first.identity, parse(listOf(machine(listOf(row)))).sessions.single().identity)
        row.put("recovery", JSONObject().put("state", "waiting"))
        assertNull(parse(listOf(machine(listOf(row)))).sessions.single().kind)
        row.put("recovery", JSONObject().put("state", "exhausted").put("request_id", "retry-1"))
        assertEquals(SessionAlertKind.Error, parse(listOf(machine(listOf(row)))).sessions.single().kind)
    }

    @Test fun presenceAlertsKeepPostedErrorCardAcrossFailedRecoveryStates() {
        // The relay emits no event when a failed recovery request only changes its failed state.
        val row = session().put("phase", "working").put("recovery", JSONObject().put("state", "uncertain").put("request_id", "retry-1"))
        val uncertain = parse(listOf(machine(listOf(row)))).sessions.single()
        row.getJSONObject("recovery").put("state", "exhausted")
        val exhausted = parse(listOf(machine(listOf(row)))).sessions.single()
        assertEquals(listOf(SessionAlertKind.Error, SessionAlertKind.Error), listOf(uncertain.kind, exhausted.kind))
        assertNotEquals(uncertain.identity, exhausted.identity)
        fun plan(state: NotificationState, current: NotificationCurrent, event: NotificationEvent?, at: Double) =
            NotificationPolicy.plan(state, listOf(current), listOfNotNull(event).map { current.slot to it },
                NotificationPreferences(), at, alertsOnly = true)
        val alerted = plan(NotificationState(bootstrap = false, settingsGeneration = 0), uncertain,
            NotificationEvent(2, computer, "raw/session/opaque", "alert", 1_000.0, "error"), 1_000.0)
        val posted = NotificationPolicy.reserve(alerted.state, alerted.desired.single(), true, 10_000)
        val later = plan(posted, exhausted, null, 1_030.0)
        assertEquals(setOf(exhausted.slot.key), later.retained)
        assertEquals(SessionAlertKind.Error, later.desired.single().kind)
        assertFalse(later.desired.single().fresh)
    }

    @Test fun optionalOnlyQuestionsAreTheirOwnKindAndNeverChangeARequest() {
        fun item(id: String, optional: Boolean? = true) = JSONObject().put("question_id", id).put("question_hash", "h-$id")
            .apply { optional?.let { put("optional", it) } }
        fun parsed(phase: String, vararg items: JSONObject, extra: (JSONObject) -> Unit = {}) =
            parse(listOf(machine(listOf(session().put("phase", phase).put("mobile_attention", JSONArray(items.toList())).also(extra))))).sessions.single()
        val optional = parsed("tool", item("codex-async:b:0"), item("codex-async:a:0"), item("codex-async:a:0"))
        assertEquals(SessionAlertKind.Optional, optional.kind)
        assertEquals(listOf("codex-async:a:0", "codex-async:b:0"), optional.optional)
        assertEquals("conversation", optional.conversation)
        assertEquals("Optional question on QA laptop", SessionNotice(optional, SessionAlertKind.Optional, "fingerprint", false).body)
        // Connectors without the marker: only hgs's async IDs are optional.
        assertEquals(SessionAlertKind.Optional, parsed("tool", item("codex-async:a:0", null)).kind)
        assertEquals(SessionAlertKind.Input, parsed("tool", item("tool-1", null)).kind)
        assertEquals(SessionAlertKind.Input, parsed("tool", item("codex-async:a:0", false)).kind)
        assertEquals(SessionAlertKind.Input, parsed("tool", item("codex-async:a:0"), item("tool-1", null)).kind)
        assertEquals(SessionAlertKind.Input, parsed("input", item("codex-async:a:0")).kind)
        assertEquals(SessionAlertKind.Input, parsed("tool", item("codex-async:a:0")) { it.put("pending_question_count", 1) }.kind)
        assertEquals(SessionAlertKind.Error, parsed("error", item("codex-async:a:0")).kind)
        assertNull(parsed("tool").kind)
        // A new optional question does not make a pending request look new.
        val request = parsed("input", item("tool-1", null))
        val withOptional = parsed("input", item("tool-1", null), item("codex-async:a:0"))
        assertEquals(request.identity, withOptional.identity)
        assertEquals(listOf("codex-async:a:0"), withOptional.optional)
    }

    @Test fun displayTextIsBoundedAndControlsCannotSpoofNotificationLines() {
        val row = session().put("tag", "Review\n\u202erequested" + "x".repeat(300))
        val current = parse(listOf(machine(listOf(row)))).sessions.single()
        assertTrue(current.title.length <= 256)
        assertFalse(current.title.contains('\n'))
        assertFalse(current.title.contains('\u202e'))
    }

    @Test fun pagesRequireOrderedIdsAndBoundedValidEventMetadata() {
        fun raw(ids: List<Int>) = JSONObject().put("events", JSONArray(ids.map { id -> JSONObject().put("id", id)
            .put("computer_id", computer).put("session", "review").put("kind", "attention").put("created_at", 1_000.0) }))
        assertEquals(100, NotificationCatalogParser.events(raw((1..100).toList())).size)
        assertThrows(IllegalArgumentException::class.java) { NotificationCatalogParser.events(raw((1..101).toList())) }
        assertThrows(IllegalArgumentException::class.java) { NotificationCatalogParser.events(raw(listOf(2, 1))) }
        assertThrows(IllegalArgumentException::class.java) { NotificationCatalogParser.events(raw(listOf(1, 1))) }
        val invalid = raw(listOf(1))
        invalid.getJSONArray("events").getJSONObject(0).put("session", "review\nspoof")
        assertThrows(IllegalArgumentException::class.java) { NotificationCatalogParser.events(invalid) }
    }

    @Test fun alertRowsCarryOnlyKnownKindsAndCapabilitiesEnableAlertMode() {
        fun row(id: Int, kind: String, alertKind: String? = null) = JSONObject().put("id", id).put("computer_id", computer)
            .put("session", "review").put("kind", kind).put("created_at", 1_000.0).apply { alertKind?.let { put("alert_kind", it) } }
        val events = NotificationCatalogParser.events(JSONObject().put("events", JSONArray(listOf(
            row(1, "attention"), row(2, "alert", "completed"), row(3, "alert", "future"), row(4, "attention", "error")))))
        assertEquals(listOf(null, "completed", null, null), events.map { it.alertKind })
        assertTrue(NotificationCatalogParser.presenceAlerts(JSONObject().put("features", JSONArray(listOf("inspect_after", "presence_alerts")))))
        assertFalse(NotificationCatalogParser.presenceAlerts(JSONObject().put("features", JSONArray(listOf("inspect_after")))))
        assertFalse(NotificationCatalogParser.presenceAlerts(JSONObject()))
    }
}
