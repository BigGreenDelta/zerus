package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID
import javax.crypto.KeyGenerator

/** Synthetic identities, ranges and a temporary cache directory; no device or network. */
class AttachmentViewTest {
    private lateinit var temporary: File
    private var now = 1_000_000_000L
    private val message = "00000000-0000-4000-8000-000000000001"
    private val target = Target("computer", "codex/example/tag", "run", "conversation", "connection")
    private val file = DisplayedAttachment("Screenshot.png", "image/png", 1_200_000, "[Image #1]", message, 0)

    @Before fun setup() { temporary = Files.createTempDirectory("zerus-attachment-view-").toFile() }
    @After fun cleanup() { temporary.deleteRecursively() }

    private fun cache(maxBytes: Long = 100L * 1024 * 1024) = AttachmentViewCache(File(temporary, "attachment-view"), { now }, maxBytes)
    private fun sha256(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun stored(cache: AttachmentViewCache, key: String, data: ByteArray, name: String = "file.bin") =
        cache.begin(key, name, "application/octet-stream").run { write(data); commit(sha256(data)) }

    private fun result(id: String, offset: Long, data: ByteArray, digest: String = "a".repeat(64), change: JSONObject.() -> Unit = {}) =
        JSONObject().put("request_id", id).put("name", target.session).put("message_id", message).put("index", 0)
            .put("bytes", file.bytes).put("sha256", digest).put("offset", offset).put("length", data.size)
            .put("file_name", file.name).put("mime", file.mime).put("data_base64", Base64.getEncoder().encodeToString(data)).apply(change)

    @Test fun receiptPositionsAreTheOnlyRemoteIdentity() {
        assertTrue(AttachmentIdentity.remote(file))
        for (invalid in listOf(file.copy(messageId = ""), file.copy(messageId = message.uppercase().replace('0', 'A')),
            file.copy(messageId = "../../private/file.png"), file.copy(index = -1), file.copy(index = 8),
            file.copy(bytes = 0), file.copy(bytes = AttachmentPolicy.MAX_FILE_BYTES + 1)))
            assertFalse(invalid.toString(), AttachmentIdentity.remote(invalid))
        val payload = AttachmentIdentity.payload(target, file, "request", 524_288, 524_288)
        assertEquals(setOf("request_id", "expected_run_id", "expected_conversation_id", "message_id", "index", "offset", "length"), payload.keys().asSequence().toSet())
        assertEquals(listOf("run", "conversation", message, 0, 524_288L, 524_288), listOf(payload["expected_run_id"],
            payload["expected_conversation_id"], payload["message_id"], payload["index"], (payload["offset"] as Number).toLong(), payload["length"]))
        val child = target.copy(agentId = "worker-1", parentConversation = "parent", archiveId = UUID.randomUUID().toString())
        val childPayload = AttachmentIdentity.payload(child, file, "request", 0, 10)
        assertEquals(listOf("parent", "worker-1", child.archiveId), listOf(childPayload["expected_conversation_id"], childPayload["agent_id"], childPayload["archive_id"]))
        for ((offset, length) in listOf(-1L to 1, 0L to 0, 0L to 524_289, file.bytes to 1, file.bytes - 1 to 2))
            assertThrows(IllegalArgumentException::class.java) { AttachmentIdentity.payload(target, file, "request", offset, length) }
        assertThrows(IllegalArgumentException::class.java) { AttachmentIdentity.payload(target.copy(conversation = ""), file, "request", 0, 1) }
        assertNotEquals(AttachmentIdentity.remoteKey(target, file), AttachmentIdentity.remoteKey(target.copy(run = "other"), file))
        assertNotEquals(AttachmentIdentity.remoteKey(target, file), AttachmentIdentity.remoteKey(target, file.copy(index = 1)))
    }

    @Test fun capabilityGatingExplainsUpdatesAndConnectivity() {
        val both = setOf("history", "attachment_read")
        assertEquals(AttachmentAccess.Supported, AttachmentIdentity.access(both, both, true))
        assertEquals(AttachmentAccess.Unsupported, AttachmentIdentity.access(setOf("history"), both, true))
        assertEquals(AttachmentAccess.Unsupported, AttachmentIdentity.access(both, setOf("history"), true))
        assertEquals(AttachmentAccess.Offline, AttachmentIdentity.access(both, both, false))
        assertEquals(AttachmentAccess.Unknown, AttachmentIdentity.access(null, both, true))
        assertEquals("Update Zerus on this computer to open attachments.",
            AttachmentIdentity.unavailableReason(target, file, AttachmentAccess.Unsupported))
        assertNull(AttachmentIdentity.unavailableReason(target, file, AttachmentAccess.Supported))
        assertNotNull(AttachmentIdentity.unavailableReason(target, file.copy(messageId = ""), AttachmentAccess.Supported))
        assertNotNull(AttachmentIdentity.unavailableReason(target.copy(connectionId = "demo"), file, AttachmentAccess.Supported))
        assertEquals(AttachmentIdentity.UPDATE_REQUIRED, AttachmentIdentity.relayFailure(409))
        assertEquals(AttachmentIdentity.UPDATE_REQUIRED, AttachmentIdentity.failure(JSONObject().put("state", "failed").put("error", "native attachment read ABI is unavailable")))
        assertTrue(AttachmentIdentity.failure(JSONObject().put("state", "queued")).contains("did not answer"))
    }

    @Test fun rangesMustAnswerTheExactRequestFileAndPosition() {
        val data = ByteArray(524_288) { it.toByte() }
        val first = AttachmentIdentity.chunk(result("one", 0, data), "one", target, file, 0, data.size, null)
        assertArrayEquals(data, first.data)
        val tail = ByteArray(524_288) { 7 }
        AttachmentIdentity.chunk(result("two", data.size.toLong(), tail), "two", target, file, data.size.toLong(), 524_288, first)
        val changes = listOf<JSONObject.() -> Unit>({ put("request_id", "other") }, { put("name", "codex/other") },
            { put("message_id", UUID.randomUUID().toString()) }, { put("index", 1) }, { put("bytes", file.bytes + 1) },
            { put("sha256", "b".repeat(64)) }, { put("sha256", "not-a-digest") }, { put("offset", 1) }, { put("length", 10) },
            { put("data_base64", "not base64!") }, { put("archive_id", UUID.randomUUID().toString()) }, { put("agent_id", "child") })
        for (change in changes) assertThrows(AttachmentOpenException::class.java) {
            AttachmentIdentity.chunk(result("two", data.size.toLong(), tail, change = change), "two", target, file, data.size.toLong(), 524_288, first)
        }
        assertThrows(AttachmentOpenException::class.java) {
            AttachmentIdentity.chunk(result("one", 0, data.copyOf(100)), "one", target, file, 0, data.size, null)
        }
    }

    @Test fun ownDeliveredFilesKeepTheirPrivateCopyByPosition() {
        val local = listOf(Attachment(UUID.randomUUID().toString(), "Screenshot.png", "image/png", 1_200_000, "c".repeat(64)),
            Attachment(UUID.randomUUID().toString(), "notes.txt", "text/plain", 5, "d".repeat(64)))
        val native = listOf(file, file.copy(name = "renamed.txt", mime = "text/plain", bytes = 5, index = 1))
        val merged = AttachmentIdentity.withLocal(native, local)
        assertEquals(local[0].id, merged[0].localId)
        assertEquals(message, merged[0].messageId)
        assertEquals("", merged[1].localId)
        assertEquals(local[0], AttachmentIdentity.local(merged[0]))
        assertNull(AttachmentIdentity.local(merged[0].copy(sha256 = "bad")))
        assertNotEquals(AttachmentIdentity.rowKey(target, merged[0]), AttachmentIdentity.rowKey(target, file))
        val displayed = local[1].displayed()
        assertEquals(listOf(local[1].id, local[1].sha256, ""), listOf(displayed.localId, displayed.sha256, displayed.messageId))
    }

    @Test fun nativeAndPersistedHistoryKeepReceiptPositions() {
        val receipt = JSONObject().put("type", "UserPromptSubmit").put("source", "hgs_delivery").put("message_id", message)
            .put("agent_id", "").put("at", 3.0).put("detail", "Look").put("submitted_text", "Look")
            .put("attachments", JSONArray().put(JSONObject().put("request_id", message).put("index", 0).put("name", "Screenshot.png")
                .put("mime", "image/png").put("bytes", 1_200_000).put("reference", "[Image #1]")))
        val event = NativeParser.events(JSONObject().put("attachment_messages", JSONArray().put(receipt))).single()
        assertEquals(file, event.attachments.single())
        val restored = EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(event))).single()
        assertEquals(file, restored.attachments.single())
        val legacy = EventHistoryCodec.encode(listOf(event.copy(attachments = listOf(file.copy(messageId = "", index = -1)))))
        assertFalse(legacy.toString().contains("message_id"))
    }

    @Test fun cacheVerifiesDigestIdentityAndNeverExposesPartialFiles() {
        val cache = cache()
        val data = "synthetic attachment".toByteArray()
        val pending = cache.begin("key", "../../evil.png", "image/png")
        pending.write(data)
        assertNull(cache.lookup("key"))
        val entry = pending.commit(sha256(data))
        assertEquals("evil.png", entry.file.name)
        assertTrue(entry.file.canonicalPath.startsWith(File(temporary, "attachment-view").canonicalPath))
        assertArrayEquals(data, cache.lookup("key")!!.file.readBytes())
        assertNull(cache.lookup("other"))
        val wrong = cache.begin("bad", "bad.bin", "application/octet-stream")
        wrong.write(data)
        assertThrows(AttachmentOpenException::class.java) { wrong.commit("0".repeat(64)) }
        assertNull(cache.lookup("bad"))
        val oversize = cache.begin("large", "large.bin", "application/octet-stream")
        assertThrows(AttachmentOpenException::class.java) { oversize.write(ByteArray((AttachmentPolicy.MAX_FILE_BYTES + 1).toInt())) }
        oversize.abort()
        entry.file.writeBytes("tampered!!!!!!!!!!!!".toByteArray())
        assertNull(cache.lookup("key"))
        assertFalse(entry.file.exists())
        assertEquals(listOf<String>(), File(temporary, "attachment-view").list()!!.toList())
    }

    @Test fun cacheExpiresAfterADayAndTrimsLeastRecentlyOpenedWithinBudget() {
        val cache = cache(maxBytes = 25)
        stored(cache, "a", ByteArray(10) { 1 }); now += 1000
        stored(cache, "b", ByteArray(10) { 2 }); now += 1000
        assertNotNull(cache.lookup("a")); now += 1000
        stored(cache, "c", ByteArray(10) { 3 })
        assertNull(cache.lookup("b"))
        assertNotNull(cache.lookup("a"))
        assertNotNull(cache.lookup("c"))
        now += 24L * 60 * 60 * 1000
        assertNull(cache.lookup("a"))
        stored(cache, "d", ByteArray(5) { 4 })
        cache.trim()
        assertEquals(1, File(temporary, "attachment-view").listFiles()!!.count { it.isDirectory })
        val active = cache.begin("e", "e.bin", "application/octet-stream")
        File(File(temporary, "attachment-view"), ".part-stale").mkdirs()
        cache.trim()
        assertTrue(File(temporary, "attachment-view").list()!!.any { it.startsWith(".part-") })
        assertFalse(File(File(temporary, "attachment-view"), ".part-stale").exists())
        active.abort()
        cache.clear()
        assertEquals(0, File(temporary, "attachment-view").list()!!.size)
    }

    @Test fun savedDraftFilesDecryptToTheirExactAuthenticatedBytes() {
        val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        val files = EncryptedAttachmentFiles(File(temporary, "attachments"), { key }) {}
        val data = ByteArray(200_000) { (it % 251).toByte() }
        val saved = files.copyFrom(ByteArrayInputStream(data), "photo.jpg", "image/jpeg", emptyList())
        val output = ByteArrayOutputStream()
        files.decrypt(saved) { output.write(it) }
        assertArrayEquals(data, output.toByteArray())
        assertThrows(AttachmentException::class.java) { files.decrypt(saved.copy(sha256 = "e".repeat(64))) {} }
        assertEquals(Base64.getEncoder().encodeToString(data), files.wire(saved).getString("data_base64"))
    }
}
