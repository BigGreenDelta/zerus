package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.net.InetAddress
import java.nio.file.Files
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.KeyGenerator

/** A synthetic relay that answers every gateway request at once with a completed receipt. */
internal class FakeOvermindRelay(private val answer: (JSONObject) -> JSONObject?) : Dispatcher() {
    val envelopes = mutableListOf<JSONObject>()
    override fun dispatch(request: RecordedRequest): MockResponse {
        val envelope = JSONObject(request.body.readUtf8())
        synchronized(envelopes) { envelopes += envelope }
        val result = answer(envelope.getJSONObject("payload")) ?: return MockResponse().setBody(JSONObject().put("request_id", envelope.getString("request_id"))
            .put("state", "failed").put("result", JSONObject.NULL).put("error", "synthetic connector failure").toString())
        return MockResponse().setBody(JSONObject().put("request_id", envelope.getString("request_id")).put("state", "completed")
            .put("result", result).put("error", JSONObject.NULL).toString())
    }
}
/** One IPv4 route: after a dropped connection OkHttp would otherwise try `localhost`'s other address. */
internal fun overmindServer(relay: Dispatcher, body: (MockWebServer, OvermindGateway) -> Unit) = MockWebServer().use { server ->
    server.dispatcher = relay; server.start(InetAddress.getByName("127.0.0.1"), 0)
    body(server, OvermindGateway(Connection("workspace", "Example", "http://127.0.0.1:${server.port}", "synthetic-token"), GATEWAY))
}
internal const val GATEWAY = "11111111-1111-4111-8111-111111111111"
internal fun ok() = JSONObject().put("ok", true)
internal val workflow: JSONObject = JSONObject().put("stages", JSONArray(listOf("backlog" to "Todo", "active" to "Doing", "done" to "Done")
    .map { (role, name) -> JSONObject().put("name", name).put("role", role) }))
internal fun card(uuid: String, stage: String, rank: String, deleted: Boolean = false) = JSONObject().put("uuid", uuid).put("ref", "#" + uuid.take(2))
    .put("project", "example").put("title", "Task $uuid").put("stage", stage).put("priority", 2).put("rank", rank).put("labels", JSONArray())
    .put("pinned", JSONObject.NULL).put("assignee", JSONObject().put("machine", "m1").put("name", "codex/example/worker")).put("reviewer", JSONObject.NULL)
    .put("flags", JSONArray()).put("parent", "").put("follows", JSONArray()).put("created_at", 1).put("updated_at", 2)
    .put("read_only", false).put("deleted", deleted).put("journal_count", 0).put("markers", JSONArray(listOf("working")))

class OvermindRepositoryTest {
    private val a = "aaaaaaaa-0000-4000-8000-000000000001"
    private val b = "bbbbbbbb-0000-4000-8000-000000000002"
    private val c = "cccccccc-0000-4000-8000-000000000003"
    private val d = "dddddddd-0000-4000-8000-000000000004"
    private fun page(cursor: Long, tasks: List<JSONObject>, next: String?) = ok().put("project", "example").put("cursor", cursor)
        .put("workflow", workflow).put("tasks", JSONArray(tasks)).put("next", next ?: JSONObject.NULL)

    @Test fun gatewayReadsCarryNoSessionAndOnlyTheirEnvelopeFields() = runBlocking<Unit> {
        val relay = FakeOvermindRelay { ok().put("cursor", 7).put("projects", JSONArray().put(JSONObject().put("project", "example").put("total", 3)
            .put("roles", JSONObject().put("needs_human", 1).put("backlog", 2)))) }
        overmindServer(relay) { _, gateway -> runBlocking {
            val projects = OvermindRepository(RelayApi()).projects(gateway)
            assertEquals(7L, projects.cursor)
            assertEquals(OvermindProject("example", 3, mapOf("needs_human" to 1L, "backlog" to 2L)), projects.projects.single())
            val envelope = relay.envelopes.single()
            assertEquals(setOf("request_id", "computer_id", "operation", "session", "payload"), envelope.keys().asSequence().toSet())
            assertEquals("", envelope.getString("session"))
            assertEquals(GATEWAY, envelope.getString("computer_id"))
            assertEquals("overmind_read", envelope.getString("operation"))
            assertEquals(setOf("action", "request"), envelope.getJSONObject("payload").keys().asSequence().toSet())
            assertEquals("projects", envelope.getJSONObject("payload").getString("action"))
            assertEquals(0, envelope.getJSONObject("payload").getJSONObject("request").length())
        } }
        assertThrows(IllegalArgumentException::class.java) { runBlocking {
            RelayApi().submitGateway(Connection("w", "W", "https://relay.example", "t"), GATEWAY, "send", JSONObject(), "id") } }
    }

    @Test fun boardPagesUntilNextIsNullThenReconcilesSinceTheFirstCursor() = runBlocking<Unit> {
        val relay = FakeOvermindRelay { payload ->
            val request = payload.getJSONObject("request")
            val since = if (request.has("since")) request.getLong("since") else null
            when (since to request.optString("after")) {
                null to "" -> page(10, listOf(card(a, "Todo", "a"), card(b, "Todo", "b")), "page-2")
                // The global cursor moved between pages: the walk continues instead of restarting.
                null to "page-2" -> page(11, listOf(card(c, "Doing", "a")), null)
                10L to "" -> page(12, listOf(card(b, "Done", "a")), "since-2")
                10L to "since-2" -> page(13, listOf(card(a, "Todo", "a", deleted = true)), null)
                12L to "" -> page(14, listOf(card(d, "Todo", "z")), null)
                else -> error("unexpected board request $request")
            }
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            val repository = OvermindRepository(RelayApi())
            val board = repository.board(gateway, "example")
            assertEquals(listOf(c, b), board.cards.map { it.uuid })
            assertEquals(listOf("Doing", "Done"), board.cards.map { it.stage })
            assertEquals(12L, board.cursor)
            assertEquals(listOf("Todo", "Doing", "Done"), board.stages.map { it.name })
            val requests = relay.envelopes.map { it.getJSONObject("payload").getJSONObject("request") }
            assertEquals(listOf("", "page-2", "", "since-2"), requests.map { it.optString("after") })
            assertEquals(listOf(null, null, 10L, 10L), requests.map { if (it.has("since")) it.getLong("since") else null })
            assertTrue(requests.all { it.getString("project") == "example" && !it.has("stage") })
            val updated = repository.board(gateway, "example", board)
            assertEquals(listOf(d, c, b), updated.cards.map { it.uuid })
            assertEquals(14L, updated.cursor)
            assertEquals(5, relay.envelopes.size)
        } }
    }

    @Test fun aBoardCursorThatGoesBackReloadsTheWholeBoard() = runBlocking<Unit> {
        var reset = false
        val relay = FakeOvermindRelay { payload ->
            val request = payload.getJSONObject("request")
            if (request.has("since") && request.getLong("since") == 50L) { reset = true; page(3, emptyList(), null) }
            else if (request.has("since")) page(4, emptyList(), null) else page(3, listOf(card(d, "Todo", "a")), null)
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            val stale = OvermindBoard("example", 50, emptyList(), listOf(OvermindParser.card(card(a, "Todo", "a"))!!), workflow.toString())
            val board = OvermindRepository(RelayApi()).board(gateway, "example", stale)
            assertTrue(reset)
            assertEquals(listOf(d), board.cards.map { it.uuid })
        } }
    }

    @Test fun taskJournalWalksOlderPagesAndRestartsWhenTheTotalChanges() = runBlocking<Unit> {
        var total = 5
        fun entry(index: Int) = JSONObject().put("marker", "[comment]").put("text", "entry $index").put("at", index)
            .put("event", "event-$index").put("actor", JSONObject().put("kind", "human").put("machine", "m1"))
        val relay = FakeOvermindRelay { payload ->
            val request = payload.getJSONObject("request")
            val before = if (request.has("journal_before")) request.getInt("journal_before") else total
            val from = maxOf(0, before - 2)
            if (before == 1) total = 6
            ok().put("task", card(a, "Todo", "a").put("prompt", "Synthetic prompt").put("attachments", JSONArray())
                .put("journal", JSONArray((from until before).map(::entry))).put("journal_total", total).put("journal_from", from).put("head", "event-4"))
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            val repository = OvermindRepository(RelayApi())
            val newest = repository.task(gateway, a)
            assertEquals(listOf("entry 3", "entry 4"), newest.journal.map { it.text })
            assertEquals(5 to 3, newest.journalTotal to newest.journalFrom)
            assertEquals("Synthetic prompt", newest.prompt)
            val older = repository.olderJournal(gateway, newest)
            assertEquals(listOf("entry 1", "entry 2", "entry 3", "entry 4"), older.journal.map { it.text })
            assertEquals(1, older.journalFrom)
            // A late-synced event changed the total: the walk restarts from the newest page.
            val restarted = repository.olderJournal(gateway, older)
            assertEquals(6 to 4, restarted.journalTotal to restarted.journalFrom)
            assertEquals(listOf("entry 4", "entry 5"), restarted.journal.map { it.text })
            val requests = relay.envelopes.map { it.getJSONObject("payload").getJSONObject("request") }
            assertEquals(listOf(null, 3, 1, null), requests.map { if (it.has("journal_before")) it.getInt("journal_before") else null })
            assertTrue(requests.all { it.getString("task") == a })
        } }
    }

    @Test fun refusalsAndFailuresSurfaceWithoutAnotherAttempt() = runBlocking<Unit> {
        val refusal = FakeOvermindRelay { JSONObject().put("ok", false).put("error", JSONObject().put("code", "too_large")
            .put("message", "the answer does not fit").put("field", "limit").put("state", "invalid")) }
        overmindServer(refusal) { _, gateway -> runBlocking {
            val error = runCatching { OvermindRepository(RelayApi()).decisions(gateway) }.exceptionOrNull() as OvermindRefusal
            assertEquals("too_large" to "limit", error.code to error.field)
            assertEquals(1, refusal.envelopes.size)
        } }
        val failure = FakeOvermindRelay { null }
        overmindServer(failure) { _, gateway -> runBlocking {
            val error = runCatching { OvermindRepository(RelayApi()).projects(gateway) }.exceptionOrNull()
            assertEquals("synthetic connector failure", error?.message)
            assertEquals(1, failure.envelopes.size)
        } }
    }

    @Test fun decisionsParseTheirQuestionAndSignature() = runBlocking<Unit> {
        val relay = FakeOvermindRelay { ok().put("cursor", 9).put("decisions", JSONArray().put(JSONObject().put("task", a).put("ref", "#4")
            .put("project", "example").put("title", "Pick a name").put("stage", "Your Call").put("question", "Which one?")
            .put("assignee", JSONObject.NULL).put("signature", "sig-1"))) }
        overmindServer(relay) { _, gateway -> runBlocking {
            val decision = OvermindRepository(RelayApi()).decisions(gateway).single()
            assertEquals(listOf(a, "Pick a name", "Which one?", "sig-1"), listOf(decision.task, decision.title, decision.question, decision.signature))
            assertNull(decision.assignee)
        } }
    }

    @Test fun attachmentRangesAreBoundedAndTheContentAddressIsVerified() = runBlocking<Unit> {
        val content = ByteArray(700_000) { (it * 31).toByte() }
        val sha = MessageDigest.getInstance("SHA-256").digest(content).joinToString("") { "%02x".format(it) }
        var tamper = false
        val relay = FakeOvermindRelay { payload ->
            val request = payload.getJSONObject("request")
            val offset = request.getInt("offset"); val end = minOf(content.size, offset + request.getInt("length"))
            val data = content.copyOfRange(offset, end).also { if (tamper) it[0] = (it[0] + 1).toByte() }
            ok().put("task", request.getString("task")).put("sha256", request.getString("sha256")).put("name", "screen.png").put("mime", "image/png")
                .put("size", content.size).put("offset", offset).put("length", data.size).put("data_base64", Base64.getEncoder().encodeToString(data))
        }
        overmindServer(relay) { _, gateway -> runBlocking {
            val repository = OvermindRepository(RelayApi())
            val image = repository.attachment(gateway, a, sha)
            assertArrayEquals(content, image.bytes)
            assertEquals("screen.png" to "image/png", image.name to image.mime)
            val ranges = relay.envelopes.map { it.getJSONObject("payload").getJSONObject("request") }
            assertEquals(listOf(0 to 524_288, 524_288 to 175_712), ranges.map { it.getInt("offset") to it.getInt("length") })
            assertTrue(ranges.all { it.getString("sha256") == sha && it.getString("task") == a })
            tamper = true
            assertTrue(runCatching { repository.attachment(gateway, a, sha) }.isFailure)
            assertTrue(runCatching { repository.attachment(gateway, a, "not-a-hash") }.isFailure)
        } }
    }

    @Test fun tasksHiddenWithoutCapabilities() {
        val both = OvermindAccess.operations + "send"
        fun machine(id: String, online: Boolean = true, route: MachineRoute = MachineRoute.Direct) = Machine("workspace", id, "Machine $id", online, route = route)
        val gateway = machine(GATEWAY); val peer = machine("peer", route = MachineRoute.Via(GATEWAY))
        fun pick(relay: Set<String>?, ops: Map<String, Set<String>>, machines: List<Machine> = listOf(peer, gateway)) =
            OvermindAccess.gateway("workspace", machines, relay) { key -> ops[key.computerId] }
        assertEquals(gateway, pick(both, mapOf(GATEWAY to both, "peer" to setOf("send"))))
        assertNull(pick(setOf("send", "overmind_read"), mapOf(GATEWAY to both)))
        assertNull(pick(null, mapOf(GATEWAY to both)))
        assertNull(pick(both, mapOf(GATEWAY to setOf("overmind_read", "send"))))
        assertNull(pick(both, emptyMap()))
        assertNull(pick(both, mapOf(GATEWAY to both), listOf(gateway.copy(online = false, lastKnown = true))))
        assertNull(OvermindAccess.gateway("other", listOf(gateway), both) { both })
        val record = JSONObject().put("id", GATEWAY).put("snapshot", JSONObject().put("mobile_capabilities",
            JSONObject().put("protocol_version", 1).put("operations", JSONArray(both.toList()))))
        assertTrue(OvermindAccess.supports(OvermindAccess.snapshotOperations(record)))
        record.getJSONObject("snapshot").getJSONObject("mobile_capabilities").put("protocol_version", 2)
        assertFalse(OvermindAccess.supports(OvermindAccess.snapshotOperations(record)))
    }

    @Test fun deviceLabelsAreCleanedInsteadOfRefused() {
        assertEquals("Pixel 9", OvermindDevice.label("  Pixel 9  "))
        assertEquals("phone", OvermindDevice.label("-@ -phone"))
        assertEquals("phonelabel", OvermindDevice.label("phone\u0000\u007f\u0085label"))
        assertEquals("Pixel 9 @home - work", OvermindDevice.label("Pixel 9 @home - work"))
        assertEquals("Ü".repeat(64), OvermindDevice.label("Ü".repeat(70)))
        assertEquals("😀".repeat(64), OvermindDevice.label("😀".repeat(65)))
        assertEquals("x", OvermindDevice.label("x\uD800"))
        listOf("", "  ", "--", "@", "\n\t", null).forEach { assertEquals("", OvermindDevice.label(it)) }
        listOf("Pixel 9", "phone", "Ü".repeat(64), "😀".repeat(64), "x").forEach { assertTrue(it, OvermindDevice.valid(it)) }
        listOf("", "-phone", "@peer", "phone\u0080", "a".repeat(65)).forEach { assertFalse(it, OvermindDevice.valid(it)) }
    }

    @Test fun encryptedCacheKeepsBoardsPerWorkspaceAndDisconnectPurgesThem() = runBlocking<Unit> {
        val root = Files.createTempDirectory("zerus-overmind-cache").toFile()
        try {
            val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
            fun cache() = OvermindCache(EncryptedConversationFiles(root, { key }, maxEntries = 128, maxBytes = 64L * 1024 * 1024))
            val board = OvermindBoard("example", 12, OvermindParser.stages(workflow), listOf(OvermindParser.card(card(a, "Todo", "a").put("title", "Private board title"))!!), workflow.toString())
            val here = MachineKey("workspace", GATEWAY); val other = MachineKey("other", GATEWAY)
            cache().put(here, "board", "example", OvermindParser.boardJson(board)); cache().put(other, "board", "example", OvermindParser.boardJson(board))
            cache().put(here, "task", a, ok().put("task", card(a, "Todo", "a").put("prompt", "Private prompt")))
            root.listFiles()!!.forEach { assertFalse(String(it.readBytes(), Charsets.ISO_8859_1).contains("Private")) }
            val restored = OvermindParser.board(cache().get(here, "board", "example")!!)!!
            assertEquals(listOf(a), restored.cards.map { it.uuid })
            assertEquals("Private board title", restored.cards.single().title)
            assertEquals(12L, restored.cursor)
            assertNull(cache().get(here, "board", "another"))
            assertNull(cache().get(here, "task", "example"))
            val purged = cache()
            purged.purgeWorkspace("workspace")
            assertNull(purged.get(here, "board", "example"))
            assertNull(cache().get(here, "task", a))
            assertEquals(listOf(a), OvermindParser.board(cache().get(other, "board", "example")!!)!!.cards.map { it.uuid })
        } finally { root.deleteRecursively() }
    }

    @Test fun anInFlightReadCannotRefillAPurgedWorkspace() = runBlocking<Unit> {
        val root = Files.createTempDirectory("zerus-overmind-purge-race").toFile()
        try {
            val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
            val cache = OvermindCache(EncryptedConversationFiles(root, { key }, maxEntries = 128, maxBytes = 64L * 1024 * 1024))
            // The workspace is disconnected while its board read is on the wire.
            val relay = FakeOvermindRelay { runBlocking { cache.purgeWorkspace("workspace") }; page(5, listOf(card(a, "Todo", "a")), null) }
            overmindServer(relay) { _, gateway -> runBlocking {
                val repository = OvermindRepository(RelayApi(), cache)
                assertEquals(listOf(a), repository.board(gateway, "example").cards.map { it.uuid })
                assertNull(repository.cachedBoard(gateway, "example"))
                cache.put(gateway.key, "board", "example", ok())
                assertNull(cache.get(gateway.key, "board", "example"))
            } }
            assertTrue(root.listFiles().orEmpty().none { it.name.endsWith(".bin") })
        } finally { root.deleteRecursively() }
    }

    @Test fun readsFillTheCacheForTheNextOpen() = runBlocking<Unit> {
        val root = Files.createTempDirectory("zerus-overmind-read-cache").toFile()
        try {
            val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
            val cache = OvermindCache(EncryptedConversationFiles(root, { key }, maxEntries = 128, maxBytes = 64L * 1024 * 1024))
            val relay = FakeOvermindRelay { payload -> when (payload.getString("action")) {
                "board" -> page(5, listOf(card(a, "Todo", "a")), null)
                "projects" -> ok().put("cursor", 5).put("projects", JSONArray())
                else -> ok().put("cursor", 5).put("decisions", JSONArray())
            } }
            overmindServer(relay) { _, gateway -> runBlocking {
                val repository = OvermindRepository(RelayApi(), cache)
                assertNull(repository.cachedBoard(gateway, "example"))
                repository.board(gateway, "example"); repository.projects(gateway); repository.decisions(gateway)
                assertEquals(listOf(a), repository.cachedBoard(gateway, "example")!!.cards.map { it.uuid })
                assertEquals(5L, repository.cachedProjects(gateway)!!.cursor)
                assertEquals(emptyList<OvermindDecision>(), repository.cachedDecisions(gateway))
                repository.purgeWorkspace("workspace")
                assertNull(repository.cachedBoard(gateway, "example"))
            } }
        } finally { root.deleteRecursively() }
    }
}
