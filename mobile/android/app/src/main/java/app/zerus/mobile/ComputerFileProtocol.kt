package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.util.Base64
import java.util.UUID

/** Bounded file reads pinned to the computer and conversation of the tapped link. */
internal object ComputerFileProtocol {
    const val OPERATION = "file_read"
    const val CHUNK_BYTES = 512 * 1024
    const val UPDATE_REQUIRED = "Update this gateway and Zerus on the computer to open file links."
    private val digest = Regex("^[a-f0-9]{64}$")
    data class Chunk(val data: ByteArray, val bytes: Long, val sha256: String, val path: String, val name: String, val mime: String)

    fun access(relay: Set<String>?, machine: Set<String>?, online: Boolean?): AttachmentAccess = when {
        relay == null || machine == null -> AttachmentAccess.Unknown
        OPERATION !in relay || OPERATION !in machine -> AttachmentAccess.Unsupported
        online == false -> AttachmentAccess.Offline
        else -> AttachmentAccess.Supported
    }
    fun payload(target: Target, reference: ComputerFileReference, requestId: String, offset: Long, length: Int, first: Chunk?): JSONObject {
        require(UUID.fromString(requestId).toString() == requestId && target.run.isNotBlank() && AttachmentIdentity.conversation(target).isNotBlank())
        require(offset >= 0 && length in 1..CHUNK_BYTES && offset + length <= AttachmentPolicy.MAX_FILE_BYTES)
        require(first != null || offset == 0L)
        return JSONObject().put("request_id", requestId).put("expected_run_id", target.run)
            .put("expected_conversation_id", AttachmentIdentity.conversation(target)).put("path", reference.path)
            .put("offset", offset).put("length", length).also {
                if (first != null) it.put("sha256", first.sha256)
                if (target.archiveId.isNotBlank()) it.put("archive_id", target.archiveId)
                if (target.agentId.isNotBlank()) it.put("agent_id", target.agentId)
            }
    }
    fun chunk(result: JSONObject, requestId: String, target: Target, reference: ComputerFileReference,
        offset: Long, length: Int, first: Chunk?): Chunk {
        fun number(key: String) = (result.opt(key) as? Number)?.toString()?.toLongOrNull()
        if (result.string("request_id") != requestId || result.string("name") != target.session ||
            result.string("run_id") != target.run || result.string("conversation_id") != AttachmentIdentity.conversation(target) ||
            result.string("archive_id") != target.archiveId || result.string("agent_id") != target.agentId ||
            result.string("requested_path") != reference.path || number("offset") != offset)
            throw AttachmentOpenException("The computer returned a different file. Open the link again.")
        val bytes = number("bytes")?.takeIf { it in 1..AttachmentPolicy.MAX_FILE_BYTES }
            ?: throw AttachmentOpenException("Only nonempty files up to 10 MiB can be opened.")
        val sha = result.string("sha256")
        val path = result.string("path")
        val name = result.string("file_name")
        val mime = result.string("mime")
        if (!sha.matches(digest) || !path.startsWith('/') || path.startsWith("//") || path.any { it.isISOControl() } ||
            path.toByteArray(Charsets.UTF_8).size > 8192 || name.isBlank() || name != path.substringAfterLast('/') ||
            name.toByteArray(Charsets.UTF_8).size > 255 || name.any { it.isISOControl() || it == '\\' } ||
            mime.isBlank() || mime != AttachmentPolicy.mime(mime))
            throw AttachmentOpenException("The computer returned invalid file details.")
        if (first != null && (sha != first.sha256 || bytes != first.bytes || path != first.path || name != first.name || mime != first.mime))
            throw AttachmentOpenException("The file changed during download. Open it again.")
        val encoded = result.string("data_base64")
        val data = try { Base64.getDecoder().decode(encoded) }
            catch (_: IllegalArgumentException) { throw AttachmentOpenException("The computer returned invalid file data.") }
        val expected = minOf(length.toLong(), bytes - offset)
        if (offset >= bytes || data.size.toLong() != expected || number("length") != expected ||
            Base64.getEncoder().encodeToString(data) != encoded)
            throw AttachmentOpenException("The computer returned an incomplete file. Open it again.")
        return Chunk(data, bytes, sha, path, name, mime)
    }
    fun cacheKey(target: Target, file: Chunk) = JSONArray(listOf("computer-file", target.key, file.path, file.sha256)).toString()
    fun failure(receipt: JSONObject): String = when (receipt.string("state")) {
        "queued", "claimed" -> "The computer did not answer in time. Check its connection and try again."
        else -> receipt.string("error").let { if (it.isBlank()) "The computer could not read this file."
            else if ("unsupported" in it || "unavailable" in it) UPDATE_REQUIRED else it.take(300) }
    }
}
