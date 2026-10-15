package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.nio.file.Files
import java.security.MessageDigest
import java.util.Base64

class ComputerFileDownloadsTest {
    private val target = Target("computer-a", "codex/example/session", "run", "conversation", "gateway")
    private val reference = ComputerFileReference("/workspace/example/Сводная итог.xlsx")
    private fun hash(data: ByteArray) = MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun result(data: ByteArray, id: String, offset: Long, length: Int) = JSONObject()
        .put("request_id", id).put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
        .put("requested_path", reference.path).put("path", reference.path).put("file_name", reference.name)
        .put("mime", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet").put("bytes", data.size)
        .put("sha256", hash(data)).put("offset", offset).put("length", minOf(length, data.size-offset.toInt()))
        .put("data_base64", Base64.getEncoder().encodeToString(data.copyOfRange(offset.toInt(), minOf(offset.toInt()+length, data.size))))

    @Test fun binaryChunksKeepOriginalTargetAndFinishWithVerifiedPrivateBytes() = runBlocking {
        val root = Files.createTempDirectory("computer-file-test").toFile()
        try {
            val data = ByteArray(1_100_123) { (it % 256).toByte() }
            val cache = AttachmentViewCache(root)
            var calls = 0
            val downloads = ComputerFileDownloads(cache) { actualTarget, actualReference, id, offset, length, first ->
                assertEquals(target, actualTarget); assertEquals(reference, actualReference); calls++
                if (offset > 0) assertEquals(hash(data), first?.sha256)
                result(data, id, offset, length)
            }
            val progress = mutableListOf<Float>()
            val (_, file) = downloads.load(target, reference, progress::add)
            assertEquals(3, calls); assertEquals(1f, progress.last())
            assertArrayEquals(data, file.file.readBytes())
            assertEquals(hash(data), file.sha256)
            assertEquals(reference.name, file.name)
        } finally { root.deleteRecursively() }
    }

    @Test fun changedSecondChunkNeverPublishesAPartialOrMixedFile() = runBlocking {
        val root = Files.createTempDirectory("computer-file-test").toFile()
        try {
            val data = ByteArray(700_000) { (it % 256).toByte() }
            val cache = AttachmentViewCache(root)
            val downloads = ComputerFileDownloads(cache) { _, _, id, offset, length, _ ->
                result(data, id, offset, length).also { if (offset > 0) it.put("sha256", "b".repeat(64)) }
            }
            var rejected = false
            try { downloads.load(target, reference) {} } catch (_: AttachmentOpenException) { rejected = true }
            assertTrue(rejected)
            assertTrue(root.listFiles().orEmpty().isEmpty())
        } finally { root.deleteRecursively() }
    }
}
