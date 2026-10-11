package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID

fun Attachment.displayed() = DisplayedAttachment(name, mime, bytes, localId = id, sha256 = sha256)

enum class AttachmentAccess { Supported, Unsupported, Offline, Unknown }

class AttachmentOpenException(message: String) : Exception(message)

/** Exact identities and the additive `attachment_read` contract; pure JVM logic. */
object AttachmentIdentity {
    const val OPERATION = "attachment_read"
    /** Decoded bytes per relay result; base64 and metadata stay below the 1 MiB result limit. */
    const val CHUNK_BYTES = 512 * 1024
    const val UPDATE_REQUIRED = "Update Zerus on this computer to open attachments."
    private val digest = Regex("^[a-f0-9]{64}$")

    fun validMessageId(value: String) = value.length == 36 &&
        runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)

    /** A native receipt position the computer can resolve; the phone never names a path. */
    fun remote(file: DisplayedAttachment) = validMessageId(file.messageId) && file.index in 0..7 &&
        file.bytes in 1..AttachmentPolicy.MAX_FILE_BYTES

    /** This phone's private encrypted copy of a draft/outbox file, when still present. */
    fun local(file: DisplayedAttachment): Attachment? = if (file.localId.isBlank()) null else runCatching {
        Attachment(file.localId, file.name, file.mime, file.bytes, file.sha256).also(AttachmentPolicy::validate)
    }.getOrNull()

    fun conversation(target: Target) = if (target.agentId.isBlank()) target.conversation else target.parentConversation

    fun remoteKey(target: Target, file: DisplayedAttachment): String = JSONArray(listOf("remote", target.connectionId,
        target.computerId, target.session, target.run, target.conversation, target.archiveId, target.agentId,
        target.parentConversation, file.messageId, file.index)).toString()

    fun localKey(attachment: Attachment): String = JSONArray(listOf("local", attachment.id, attachment.sha256)).toString()

    /** Row identity for progress and errors: the local copy when present, else the receipt position. */
    fun rowKey(target: Target?, file: DisplayedAttachment): String = local(file)?.let(::localKey)
        ?: if (target != null && remote(file)) remoteKey(target, file)
        else JSONArray(listOf("none", target?.key.orEmpty(), file.name, file.mime, file.bytes, file.reference)).toString()

    /** Native rows of this phone's own message keep its private copies, by position. */
    fun withLocal(native: List<DisplayedAttachment>, local: List<Attachment>) = native.mapIndexed { index, file ->
        local.getOrNull(file.index.takeIf { it >= 0 } ?: index)?.takeIf { it.name == file.name && it.bytes == file.bytes }
            ?.let { file.copy(localId = it.id, sha256 = it.sha256) } ?: file
    }

    fun unavailableReason(target: Target?, file: DisplayedAttachment, access: AttachmentAccess): String? = when {
        target == null || !remote(file) -> "This attachment cannot be opened from the phone."
        target.connectionId == "demo" -> "Attachments are not available in the demo."
        target.run.isBlank() || conversation(target).isBlank() -> "Refresh the conversation, then try again."
        access == AttachmentAccess.Unsupported -> UPDATE_REQUIRED
        access == AttachmentAccess.Offline -> "This computer is offline. Try again when it is connected."
        access == AttachmentAccess.Unknown -> "Connect to this computer, then try again."
        else -> null
    }

    fun access(relay: Set<String>?, machine: Set<String>?, online: Boolean?): AttachmentAccess = when {
        relay == null || machine == null -> AttachmentAccess.Unknown
        OPERATION !in relay || OPERATION !in machine -> AttachmentAccess.Unsupported
        online == false -> AttachmentAccess.Offline
        else -> AttachmentAccess.Supported
    }

    fun payload(target: Target, file: DisplayedAttachment, requestId: String, offset: Long, length: Int): JSONObject {
        require(remote(file) && target.run.isNotBlank() && conversation(target).isNotBlank())
        require(offset in 0 until file.bytes && length in 1..CHUNK_BYTES && offset + length <= file.bytes)
        return JSONObject().put("request_id", requestId).put("expected_run_id", target.run)
            .put("expected_conversation_id", conversation(target)).put("message_id", file.messageId)
            .put("index", file.index).put("offset", offset).put("length", length).also {
                if (target.archiveId.isNotBlank()) it.put("archive_id", target.archiveId)
                if (target.agentId.isNotBlank()) it.put("agent_id", target.agentId)
            }
    }

    class Chunk(val data: ByteArray, val total: Long, val sha256: String)

    /** Verifies one range answers this exact request, file and position. */
    fun chunk(result: JSONObject, requestId: String, target: Target, file: DisplayedAttachment, offset: Long, length: Int,
              first: Chunk?): Chunk {
        fun number(key: String) = (result.opt(key) as? Number)?.toLong()
        if (result.string("request_id") != requestId || result.string("name") != target.session ||
            result.string("message_id") != file.messageId || number("index") != file.index.toLong() ||
            result.string("archive_id") != target.archiveId || result.string("agent_id") != target.agentId)
            throw AttachmentOpenException("The computer returned a different attachment. Refresh and try again.")
        val total = number("bytes")
        if (total != file.bytes) throw AttachmentOpenException("The attachment changed on the computer. Refresh and try again.")
        val sha256 = result.string("sha256")
        if (!sha256.matches(digest) || first != null && first.sha256 != sha256 || number("offset") != offset)
            throw AttachmentOpenException("The attachment changed while downloading. Try again.")
        val data = try { Base64.getDecoder().decode(result.string("data_base64")) }
            catch (_: IllegalArgumentException) { throw AttachmentOpenException("The computer returned invalid attachment data.") }
        val expected = minOf(length.toLong(), total - offset)
        if (data.size.toLong() != expected || number("length") != expected)
            throw AttachmentOpenException("The computer returned an incomplete attachment range. Try again.")
        return Chunk(data, total, sha256)
    }

    /** A clear message for a failed or unconfirmed read receipt. */
    fun failure(receipt: JSONObject): String = when (receipt.string("state")) {
        "queued", "claimed" -> "The computer did not answer in time. Check that it is online, then try again."
        else -> receipt.string("error").let { error ->
            when {
                "unavailable" in error || "unsupported operation" in error -> UPDATE_REQUIRED
                error.isBlank() -> "The computer could not read this attachment."
                else -> "The computer could not read this attachment: ${error.take(200)}"
            }
        }
    }

    fun relayFailure(status: Int): String = when (status) {
        400, 409 -> UPDATE_REQUIRED
        401, 403 -> "Reconnect to this gateway, then try again."
        429 -> "The gateway is busy. Try again in a moment."
        else -> "The gateway could not deliver this request ($status). Try again."
    }
}

/**
 * Bounded private cache of opened attachments under the app cache directory.
 * Each entry is `<sha256(identity)>/f/<sanitized name>` plus `entry.json` with
 * the exact identity, size and digest; it is verified on every reuse. Entries
 * expire 24 h after creation; the least recently opened go first beyond the
 * total budget. Files leave the app only through FileProvider read grants.
 */
class AttachmentViewCache(private val root: File, private val clock: () -> Long = System::currentTimeMillis,
                          val maxBytes: Long = 100L * 1024 * 1024, val maxAgeMs: Long = 24L * 60 * 60 * 1000) {
    class Entry(val file: File, val name: String, val mime: String, val sha256: String, val bytes: Long)

    private val active = mutableSetOf<String>()

    private fun prepare() {
        if ((!root.isDirectory && !root.mkdirs()) || Files.isSymbolicLink(root.toPath()))
            throw AttachmentOpenException("Attachment storage on this phone is unavailable.")
        root.setReadable(false, false); root.setReadable(true, true)
        root.setWritable(false, false); root.setWritable(true, true)
        root.setExecutable(false, false); root.setExecutable(true, true)
    }

    private fun hex(value: ByteArray) = value.joinToString("") { "%02x".format(it.toInt() and 255) }
    private fun directory(key: String) = File(root, hex(MessageDigest.getInstance("SHA-256").digest(key.toByteArray(Charsets.UTF_8))))
    private fun regular(file: File) = Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)
    private fun digestOf(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) { val count = input.read(buffer); if (count < 0) break; digest.update(buffer, 0, count) }
        }
        return hex(digest.digest())
    }

    @Synchronized fun lookup(key: String): Entry? {
        prepare()
        val directory = directory(key)
        val metadata = File(directory, "entry.json")
        if (!regular(metadata)) { directory.deleteRecursively(); return null }
        val entry = try {
            val value = JSONObject(metadata.readText())
            val created = value.getLong("created")
            val now = clock()
            val file = File(File(directory, "f"), value.getString("file"))
            if (value.getString("key") != key || now - created >= maxAgeMs || created > now + 60_000 ||
                value.getString("file") != AttachmentPolicy.filename(value.getString("file")) || !regular(file) ||
                file.length() != value.getLong("bytes") || digestOf(file) != value.getString("sha256")) null
            else Entry(file, value.getString("name"), value.getString("mime"), value.getString("sha256"), file.length())
        } catch (_: Exception) { null }
        if (entry == null) { directory.deleteRecursively(); return null }
        directory.setLastModified(clock())
        return entry
    }

    /** One download or decryption in progress; nothing is visible until [commit]. */
    inner class Pending internal constructor(private val key: String, private val name: String, private val mime: String,
                                             private val part: File) {
        private val file = File(File(part, "f"), AttachmentPolicy.filename(name))
        private val output = FileOutputStream(file)
        private val digest = MessageDigest.getInstance("SHA-256")
        private var written = 0L
        private var finished = false

        fun write(bytes: ByteArray) {
            check(!finished)
            written += bytes.size
            if (written > AttachmentPolicy.MAX_FILE_BYTES) throw AttachmentOpenException("Attachments larger than 10 MiB cannot be opened.")
            digest.update(bytes)
            output.write(bytes)
        }

        fun commit(sha256: String): Entry {
            check(!finished)
            output.fd.sync(); output.close()
            val actual = hex(digest.digest())
            if (written == 0L || actual != sha256) { abort(); throw AttachmentOpenException("The downloaded attachment did not match its digest. Try again.") }
            File(part, "entry.json").writeText(JSONObject().put("key", key).put("name", name).put("mime", mime)
                .put("file", file.name).put("bytes", written).put("sha256", actual).put("created", clock()).toString())
            synchronized(this@AttachmentViewCache) {
                val destination = directory(key)
                destination.deleteRecursively()
                Files.move(part.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE)
                active -= part.name
                finished = true
                destination.setLastModified(clock())
                trimLocked()
                return Entry(File(File(destination, "f"), file.name), name, mime, actual, written)
            }
        }

        fun abort() {
            if (finished) return
            finished = true
            runCatching { output.close() }
            synchronized(this@AttachmentViewCache) { part.deleteRecursively(); active -= part.name }
        }
    }

    @Synchronized fun begin(key: String, name: String, mime: String): Pending {
        prepare()
        val part = File(root, ".part-" + UUID.randomUUID())
        if (!File(part, "f").mkdirs()) throw AttachmentOpenException("Attachment storage on this phone is unavailable.")
        active += part.name
        return try { Pending(key, name, mime, part) } catch (error: Exception) {
            part.deleteRecursively(); active -= part.name; throw error
        }
    }

    @Synchronized fun trim() { prepare(); trimLocked() }

    private fun trimLocked() {
        val now = clock()
        root.listFiles().orEmpty().filter { it.name !in active && (it.name.startsWith(".part-") || !it.isDirectory ||
            Files.isSymbolicLink(it.toPath())) }.forEach { it.deleteRecursively() }
        val entries = root.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".part-") }.mapNotNull { directory ->
            val created = runCatching { JSONObject(File(directory, "entry.json").readText()).getLong("created") }.getOrNull()
            if (created == null || now - created >= maxAgeMs || created > now + 60_000) { directory.deleteRecursively(); null }
            else Triple(directory, directory.lastModified(), File(directory, "f").listFiles().orEmpty().sumOf { it.length() })
        }.sortedBy { it.second }.toMutableList()
        var total = entries.sumOf { it.third }
        while (total > maxBytes && entries.isNotEmpty()) {
            val (directory, _, size) = entries.removeAt(0)
            directory.deleteRecursively(); total -= size
        }
    }

    @Synchronized fun clear() {
        root.listFiles().orEmpty().filter { it.name !in active }.forEach { it.deleteRecursively() }
    }
}
