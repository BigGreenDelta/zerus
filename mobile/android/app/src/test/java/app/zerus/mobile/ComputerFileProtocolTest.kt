package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.Base64
import java.util.UUID

class ComputerFileProtocolTest {
    private val target = Target("computer-a", "codex/project/session", "run", "conversation", "gateway")
    private val reference = ComputerFileReference("/work/example/Сводная.xlsx")
    private val id = UUID.randomUUID().toString()
    private fun result() = JSONObject().put("request_id", id).put("name", target.session).put("run_id", target.run)
        .put("conversation_id", target.conversation).put("requested_path", reference.path).put("path", reference.path)
        .put("file_name", reference.name).put("mime", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
        .put("bytes", 3).put("sha256", "a".repeat(64)).put("offset", 0).put("length", 3)
        .put("data_base64", Base64.getEncoder().encodeToString(byteArrayOf(0, -1, 80)))
    @Test fun exactComputerConversationAndUnicodePathArePinned() {
        val payload = ComputerFileProtocol.payload(target, reference, id, 0, ComputerFileProtocol.CHUNK_BYTES, null)
        assertEquals(reference.path, payload.getString("path"))
        val file = ComputerFileProtocol.chunk(result(), id, target, reference, 0, ComputerFileProtocol.CHUNK_BYTES, null)
        assertArrayEquals(byteArrayOf(0, -1, 80), file.data)
        assertNotEquals(ComputerFileProtocol.cacheKey(target, file), ComputerFileProtocol.cacheKey(target.copy(computerId = "other"), file))
    }
    @Test fun mismatchedIdentitiesVersionsAndMalformedRangesAreRejected() {
        val first = ComputerFileProtocol.chunk(result(), id, target, reference, 0, ComputerFileProtocol.CHUNK_BYTES, null)
        for ((field, value) in listOf("request_id" to "other", "name" to "other", "run_id" to "other", "conversation_id" to "other",
            "requested_path" to "/work/other.xlsx", "path" to "relative.xlsx", "file_name" to "other.xlsx", "mime" to "bad type",
            "bytes" to 0, "bytes" to 3.5, "offset" to 1, "length" to 2, "data_base64" to "not base64")) {
            val changed = result().put(field, value)
            assertThrows(field, AttachmentOpenException::class.java) {
                ComputerFileProtocol.chunk(changed, id, target, reference, 0, ComputerFileProtocol.CHUNK_BYTES, null)
            }
        }
        assertThrows(AttachmentOpenException::class.java) {
            ComputerFileProtocol.chunk(result().put("sha256", "b".repeat(64)), id, target, reference, 0, ComputerFileProtocol.CHUNK_BYTES, first)
        }
    }
}
