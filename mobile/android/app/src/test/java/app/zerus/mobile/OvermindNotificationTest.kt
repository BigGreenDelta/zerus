package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class OvermindNotificationTest {
    private val task = "aaaaaaaa-0000-4000-8000-000000000001"
    private val slot = "overmind:$task"
    private val connection = Connection("workspace", "Example workspace", "https://relay.example", "synthetic-token")
    private val now = 1_000.0
    private fun gateway(ops: Collection<String> = OvermindAccess.operations + "send", signature: String = "sig-1", digest: Boolean = true, online: Boolean = true) =
        JSONObject().put("id", GATEWAY).put("name", "Gateway machine").put("online", online).put("via", JSONObject.NULL).put("snapshot", JSONObject()
            .put("sessions", JSONArray()).put("mobile_capabilities", JSONObject().put("protocol_version", 1).put("operations", JSONArray(ops.toList())))
            .also { if (digest) it.put("mobile_overmind", JSONObject().put("schema", 1).put("rev", 3).put("projects", JSONObject())
                .put("decisions", JSONArray().put(JSONObject().put("task", task).put("project", "example").put("title", "Pick a release name").put("signature", signature)))) })
    private fun catalog(record: JSONObject = gateway(), overmind: Boolean = true) =
        NotificationCatalogParser.parse(connection, listOf(record), listOf(MachineAlias(MachineKey(connection.id, GATEWAY), "Home desktop")), overmind)
    private fun alert(id: Long = 2, kind: String = "decision", session: String = slot) = NotificationEvent(id, GATEWAY, session, "alert", now, kind)
    private fun plan(state: NotificationState, current: List<NotificationCurrent>, events: List<NotificationEvent>, alertsOnly: Boolean = true,
        preferences: NotificationPreferences = NotificationPreferences(), at: Double = now): NotificationPlan {
        val parsed = NotificationCatalog(current, mapOf(GATEWAY to GATEWAY))
        return NotificationPolicy.plan(state, current, events.mapNotNull { parsed.resolve(connection.id, it) }, preferences, at, alertsOnly = alertsOnly)
    }
    private val ready = NotificationState(bootstrap = false, settingsGeneration = 0)

    @Test fun decisionCardsComeFromTheGatewayDigestOnlyWhenTasksAreServed() {
        val current = catalog().sessions.single()
        assertEquals(NotificationSlot(connection.id, GATEWAY, slot), current.slot)
        assertEquals(SessionAlertKind.Decision, current.kind)
        assertEquals("Pick a release name", current.title)
        assertEquals("Home desktop", current.machine)
        assertEquals("Needs your decision on Home desktop", SessionNotice(current, SessionAlertKind.Decision, "f", false).body)
        assertNotEquals(current.identity, catalog(gateway(signature = "sig-2")).sessions.single().identity)
        assertEquals(current.identity, catalog(gateway()).sessions.single().identity)
        assertTrue(catalog(overmind = false).sessions.isEmpty())
        assertTrue(catalog(gateway(ops = listOf("overmind_read", "send"))).sessions.isEmpty())
        assertTrue(catalog(gateway(digest = false)).sessions.isEmpty())
        assertFalse(catalog(gateway(online = false)).sessions.single().online)
        val events = NotificationCatalogParser.events(JSONObject().put("events", JSONArray().put(JSONObject().put("id", 1).put("computer_id", GATEWAY)
            .put("session", slot).put("kind", "alert").put("alert_kind", "decision").put("created_at", now))
            .put(JSONObject().put("id", 2).put("computer_id", GATEWAY).put("session", slot).put("kind", "decision").put("created_at", now))))
        assertEquals(listOf("decision", null), events.map { it.alertKind })
        assertEquals(current.slot, catalog().resolve(connection.id, events.first())?.first)
    }

    @Test fun aDecisionAnnouncesOnlyWithItsAlertAndFollowsThePreference() {
        val current = catalog().sessions
        assertTrue(plan(ready, current, emptyList()).desired.isEmpty())
        // Another kind's alert on the slot does not announce a decision.
        assertTrue(plan(ready, current, listOf(alert(kind = "attention"))).desired.isEmpty())
        val announced = plan(ready, current, listOf(alert()))
        val notice = announced.desired.single()
        assertEquals(SessionAlertKind.Decision to true, notice.kind to notice.fresh)
        assertTrue(plan(ready, current, listOf(alert()), preferences = NotificationPreferences(decisions = false)).desired.isEmpty())
        assertTrue(plan(ready, current, listOf(alert()), preferences = NotificationPreferences(master = false)).desired.isEmpty())
        // Without presence alerts an ordinary decision event announces, as attention does.
        assertTrue(plan(ready, current, listOf(NotificationEvent(3, GATEWAY, slot, "decision", now)), alertsOnly = false).desired.single().fresh)
        // A posted card stays while the decision waits and leaves with it.
        val posted = NotificationPolicy.reserve(announced.state, notice, true, 10_000)
        val later = plan(posted, current, emptyList(), at = now + 30)
        assertEquals(setOf(current.single().slot.key), later.retained)
        assertFalse(later.desired.single().fresh)
        assertTrue(plan(posted, emptyList(), emptyList(), at = now + 60).retained.isEmpty())
    }

    @Test fun aStaleDecisionAlertOnlyRefreshes() {
        val gone = plan(ready, emptyList(), listOf(alert()))
        assertTrue(gone.desired.isEmpty())
        assertTrue(gone.retained.isEmpty())
        val hidden = plan(ready, catalog(overmind = false).sessions, listOf(alert()))
        assertTrue(hidden.desired.isEmpty())
        assertNull(OvermindDigests.task("overmind:not-a-task"))
    }

    @Test fun preferencesAndStateCodecCarryDecisions() {
        val defaults = NotificationPreferences()
        assertTrue(defaults.allows(SessionAlertKind.Decision))
        assertFalse(defaults.copy(decisions = false).allows(SessionAlertKind.Decision))
        assertFalse(defaults.copy(master = false).allows(SessionAlertKind.Decision))
        assertEquals(7L, defaults.copy(decisionsEnabledAt = 7).enabledAt(SessionAlertKind.Decision))
        val record = NotificationRecord(NotificationSlot(connection.id, GATEWAY, slot), "target", now, postedFingerprint = "f", postedKind = SessionAlertKind.Decision)
        val state = NotificationState(records = listOf(record), overmind = true, overflowKinds = setOf(SessionAlertKind.Decision))
        val restored = NotificationStateCodec.decode(JSONObject(NotificationStateCodec.encode(state).toString()))
        assertEquals(state, restored)
        val future = NotificationStateCodec.encode(state).also { it.getJSONArray("records").getJSONObject(0).put("posted_kind", "FutureKind") }
        assertNull(NotificationStateCodec.decode(future).records.single().postedKind)
        assertFalse(NotificationStateCodec.decode(NotificationStateCodec.encode(NotificationState()).apply { remove("overmind") }).overmind)
    }

    @Test fun decisionRecordsStayReadableByOlderBuilds() {
        val decision = NotificationRecord(NotificationSlot(connection.id, GATEWAY, slot), "target", now, postedFingerprint = "f", postedKind = SessionAlertKind.Decision)
        val input = decision.copy(slot = NotificationSlot(connection.id, GATEWAY, "session"), postedKind = SessionAlertKind.Input)
        val encoded = JSONObject(NotificationStateCodec.encode(NotificationState(records = listOf(decision, input))).toString())
        // Builds before Decision decode `kind` with an unguarded valueOf over the three names they know.
        val legacy = encoded.getJSONArray("records").objects().map { row -> row.string("kind").takeIf { it.isNotEmpty() }?.let { name ->
            require(name in setOf("Input", "Error", "Finished")) { "an older build would fail on $name" }; SessionAlertKind.valueOf(name) } }
        assertEquals(listOf(null, SessionAlertKind.Input), legacy)
        assertEquals(listOf(SessionAlertKind.Decision, SessionAlertKind.Input), NotificationStateCodec.decode(encoded).records.map { it.postedKind })
        // A state written by an older build still decodes.
        encoded.getJSONArray("records").objects().forEach { it.remove("posted_kind") }
        assertEquals(listOf(null, SessionAlertKind.Input), NotificationStateCodec.decode(encoded).records.map { it.postedKind })
    }

    @Test fun aMissingDigestKeepsPostedDecisionCards() {
        val withDigest = catalog()
        val key = withDigest.sessions.single().slot.key
        val announced = plan(ready, withDigest.sessions, listOf(alert()))
        val posted = NotificationPolicy.reserve(announced.state, announced.desired.single(), true, 10_000).copy(pendingPosts = emptySet())
        val gap = catalog(gateway(digest = false))
        assertTrue(gap.sessions.isEmpty())
        val held = plan(posted, gap.withHeld(posted.records), emptyList(), at = now + 30)
        assertEquals(setOf(key), held.retained)
        assertTrue(held.desired.isEmpty())
        val back = plan(held.state, catalog().withHeld(held.state.records), emptyList(), at = now + 60)
        assertEquals(setOf(key), back.retained)
        assertFalse(back.desired.single().fresh)
        // A gateway that stops serving Tasks, or a relay without the operations, holds nothing.
        assertTrue(catalog(gateway(ops = listOf("send"), digest = false)).withHeld(posted.records).isEmpty())
        assertTrue(catalog(gateway(digest = false), overmind = false).withHeld(posted.records).isEmpty())
        assertTrue(plan(posted, catalog(gateway(ops = listOf("send"))).withHeld(posted.records), emptyList(), at = now + 30).retained.isEmpty())
    }

    @Test fun aDecisionWaitingOnTwoGatewaysShowsOneCard() {
        val second = "22222222-2222-4222-8222-222222222222"
        val laptop = gateway().put("id", second).put("name", "Laptop")
        fun computers(rows: List<JSONObject>) = NotificationCatalogParser.parse(connection, rows, emptyList(), true).sessions.map { it.slot.computer }
        assertEquals(listOf(GATEWAY), computers(listOf(laptop, gateway())))
        assertEquals(listOf(second), computers(listOf(gateway(online = false), laptop)))
        assertEquals(listOf(second), computers(listOf(gateway().put("via", JSONObject().put("gateway_id", second)), laptop)))
    }
}
