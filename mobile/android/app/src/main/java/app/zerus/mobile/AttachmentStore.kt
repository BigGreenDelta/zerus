package app.zerus.mobile

import android.content.Context
import android.net.Uri
import android.os.CancellationSignal
import android.provider.OpenableColumns
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.system.Os
import android.system.OsConstants
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.channels.Channels
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.StandardCopyOption
import java.nio.file.StandardOpenOption
import java.nio.file.attribute.PosixFilePermission
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Encrypted durable copies of user-selected documents, separate from preferences.
 * Draft/outbox ownership belongs to PrivateStore. Removal requires its complete
 * protected-ID set; this class never automatically deletes published objects.
 */
class AttachmentStore(context: Context) {
    private val resolver = context.applicationContext.contentResolver
    private val root = File(context.applicationContext.noBackupFilesDir.canonicalFile, "attachments-v1")
    private val files = EncryptedAttachmentFiles(root, ::key) {
        val descriptor = Os.open(root.absolutePath, OsConstants.O_RDONLY, 0)
        try { Os.fsync(descriptor) } finally { Os.close(descriptor) }
    }

    private fun key(): SecretKey = synchronized(KEY_LOCK) {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(KEY_ALIAS, null) as? SecretKey) ?: KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true).build())
        }.generateKey()
    }

    suspend fun importDocument(uri: Uri, current: List<Attachment> = emptyList()): Attachment = coroutineScope {
        if (uri.scheme != "content") throw AttachmentException("Choose a document using the file picker.")
        val signal = CancellationSignal()
        val activeInput = AtomicReference<InputStream?>()
        var imported: Attachment? = null
        var successful = false
        // Cancellation can interrupt a provider query/open or a blocking read.
        val cancellation = launch(Dispatchers.IO, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally {
                runCatching { signal.cancel() }
                runCatching { activeInput.getAndSet(null)?.close() }
            }
        }
        try {
            withContext(Dispatchers.IO) {
                FILE_LOCK.withLock {
                    AttachmentPolicy.selection(current, adding = true)
                    ensureActive()
                    val displayName = resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null, signal)
                        ?.use { cursor ->
                            val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                            if (column >= 0 && cursor.moveToFirst()) cursor.getString(column) else null
                        }
                    val mime = resolver.getType(uri)
                    resolver.openAssetFileDescriptor(uri, "r", signal)?.use { descriptor ->
                        descriptor.createInputStream().use { input ->
                            activeInput.set(input)
                            // Provider size metadata is intentionally ignored. Count actual bytes.
                            files.copyFrom(input, displayName, mime, current) { ensureActive() }
                                .also { imported = it }
                        }
                    } ?: throw AttachmentException("The selected document could not be opened.")
                }
            }.also { successful = true }
        } catch (error: CancellationException) {
            throw error
        } catch (error: AttachmentException) {
            throw error
        } catch (_: Exception) {
            ensureActive()
            throw AttachmentException("The selected document could not be copied. Check access and available storage.")
        } finally {
            withContext(NonCancellable) {
                cancellation.cancelAndJoin()
            }
            // Prompt cancellation may discard withContext's result after it has
            // committed the file. Remove only this unpublished import's new ID.
            if (!successful || !coroutineContext.isActive) {
                withContext(NonCancellable + Dispatchers.IO) {
                    FILE_LOCK.withLock { imported?.let { files.remove(it.id, emptySet()) } }
                }
            }
        }
    }

    suspend fun readForWire(attachment: Attachment): JSONObject = withContext(Dispatchers.IO) {
        FILE_LOCK.withLock { files.wire(attachment) { ensureActive() } }
    }

    /** App-captured recordings use the same authenticated durable store as selected files. */
    internal suspend fun importRecording(wav: ByteArray): Attachment = withContext(Dispatchers.IO) {
        FILE_LOCK.withLock { wav.inputStream().use { files.copyFrom(it, "voice.wav", "audio/wav", emptyList()) { ensureActive() } } }
    }

    suspend fun payload(attachments: List<Attachment>): JSONArray = withContext(Dispatchers.IO) {
        FILE_LOCK.withLock {
            AttachmentPolicy.selection(attachments)
            JSONArray().also { array -> attachments.forEach { array.put(files.wire(it) { ensureActive() }) } }
        }
    }

    /** Authenticated plaintext of a saved draft/outbox file, for local viewing only. */
    suspend fun decrypt(attachment: Attachment, sink: (ByteArray) -> Unit) = withContext(Dispatchers.IO) {
        FILE_LOCK.withLock { files.decrypt(attachment, { ensureActive() }, sink) }
    }

    suspend fun remove(id: String, protectedIds: Set<String>): Boolean = withContext(Dispatchers.IO) {
        FILE_LOCK.withLock { files.remove(id, protectedIds) }
    }

    companion object {
        private const val KEY_ALIAS = "zerus_attachments_v1"
        private val KEY_LOCK = Any()
        private val FILE_LOCK = Mutex()
    }
}

/** Platform-neutral encrypted file format also exercised by synthetic JVM tests. */
internal class EncryptedAttachmentFiles(
    private val root: File,
    private val key: () -> SecretKey,
    private val syncDirectory: () -> Unit,
) {
    private fun directory() {
        if ((!root.isDirectory && !root.mkdirs()) || root.canonicalFile != root.absoluteFile
            || Files.isSymbolicLink(root.toPath())) throw AttachmentException("Private attachment storage is unavailable.")
        Files.setPosixFilePermissions(root.toPath(), setOf(PosixFilePermission.OWNER_READ,
            PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE))
    }

    private fun file(id: String): File {
        if (!AttachmentPolicy.validId(id)) throw AttachmentException("Invalid attachment reference.")
        return File(root, "$id.bin")
    }

    private fun aad(id: String, name: String, mime: String): ByteArray = ByteArrayOutputStream().also { output ->
        DataOutputStream(output).use { data -> data.writeUTF("zerus-attachment-v1"); data.writeUTF(id); data.writeUTF(name); data.writeUTF(mime) }
    }.toByteArray()

    fun copyFrom(input: InputStream, rawName: String?, rawMime: String?, current: List<Attachment>,
                 checkCancellation: () -> Unit = {}): Attachment {
        val existing = AttachmentPolicy.selection(current, adding = true)
        directory()
        val id = UUID.randomUUID().toString()
        val name = AttachmentPolicy.filename(rawName)
        val mime = AttachmentPolicy.mime(rawMime)
        val part = File(root, "$id.part")
        val destination = file(id)
        var complete = false
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply {
                init(Cipher.ENCRYPT_MODE, key()); updateAAD(aad(id, name, mime))
            }
            check(cipher.iv.size == 12)
            var bytes = 0L
            FileChannel.open(part.toPath(), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE).use { channel ->
                Files.setPosixFilePermissions(part.toPath(), setOf(PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE))
                val output = DataOutputStream(Channels.newOutputStream(channel))
                output.writeInt(MAGIC)
                output.write(cipher.iv)
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    checkCancellation()
                    val count = input.read(buffer)
                    if (count < 0) break
                    if (count == 0) continue
                    bytes += count
                    if (bytes > AttachmentPolicy.MAX_FILE_BYTES) throw AttachmentException("Each attachment must be at most 10 MiB.")
                    if (existing + bytes > AttachmentPolicy.MAX_TOTAL_BYTES) throw AttachmentException("Attachments exceed 20 MiB in total.")
                    digest.update(buffer, 0, count)
                    cipher.update(buffer, 0, count)?.let(output::write)
                }
                if (bytes == 0L) throw AttachmentException("Empty files cannot be attached.")
                output.write(cipher.doFinal())
                output.flush()
                channel.force(true)
            }
            checkCancellation()
            if (destination.exists()) throw AttachmentException("Could not reserve an attachment reference.")
            Files.move(part.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE)
            syncDirectory()
            checkCancellation()
            val result = Attachment(id, name, mime, bytes, digest.digest().hex())
            complete = true
            return result
        } finally {
            if (!complete) {
                Files.deleteIfExists(part.toPath())
                Files.deleteIfExists(destination.toPath())
            }
        }
    }

    fun wire(attachment: Attachment, checkCancellation: () -> Unit = {}): JSONObject {
        val result = ByteArrayOutputStream()
        decrypt(attachment, checkCancellation, result::write)
        return JSONObject().put("name", attachment.name).put("mime", attachment.mime)
            .put("data_base64", Base64.getEncoder().encodeToString(result.toByteArray()))
    }

    /** Streams plaintext to [sink]. It is authenticated only when this returns,
     * so a caller must discard everything it received if this throws. */
    fun decrypt(attachment: Attachment, checkCancellation: () -> Unit = {}, sink: (ByteArray) -> Unit) {
        AttachmentPolicy.validate(attachment)
        directory()
        val source = file(attachment.id)
        if (!Files.isRegularFile(source.toPath(), LinkOption.NOFOLLOW_LINKS)
            || source.length() !in 33..(AttachmentPolicy.MAX_FILE_BYTES + 32))
            throw AttachmentException("Saved attachment is missing or changed. Select the file again.")
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            var count = 0L
            fun plaintext(value: ByteArray?) {
                if (value == null) return
                count += value.size
                if (count > attachment.bytes || count > AttachmentPolicy.MAX_FILE_BYTES)
                    throw AttachmentException("Saved attachment size changed. Select the file again.")
                digest.update(value)
                sink(value)
            }
            DataInputStream(source.inputStream().buffered()).use { input ->
                if (input.readInt() != MAGIC) throw AttachmentException("Saved attachment is invalid.")
                val iv = ByteArray(12).also(input::readFully)
                val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply {
                    init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
                    updateAAD(aad(attachment.id, attachment.name, attachment.mime))
                }
                val buffer = ByteArray(64 * 1024)
                var encryptedBytes = 16L
                while (true) {
                    checkCancellation()
                    val size = input.read(buffer)
                    if (size < 0) break
                    encryptedBytes += size
                    if (encryptedBytes > AttachmentPolicy.MAX_FILE_BYTES + 32)
                        throw AttachmentException("Saved attachment size changed. Select the file again.")
                    plaintext(cipher.update(buffer, 0, size))
                }
                plaintext(cipher.doFinal()) // Authentication succeeds before any wire bytes are returned.
            }
            checkCancellation()
            if (count != attachment.bytes || digest.digest().hex() != attachment.sha256)
                throw AttachmentException("Saved attachment content changed. Select the file again.")
        } catch (error: AttachmentException) { throw error
        } catch (_: IOException) {
            throw AttachmentException("Saved attachment could not be read. Select the file again.")
        } catch (_: GeneralSecurityException) {
            throw AttachmentException("Saved attachment could not be verified. Select the file again.")
        }
    }

    fun remove(id: String, protectedIds: Set<String>): Boolean {
        if (!AttachmentPolicy.validId(id)) throw AttachmentException("Invalid attachment reference.")
        if (id in protectedIds) return false
        directory()
        val removed = Files.deleteIfExists(file(id).toPath())
        if (removed) syncDirectory()
        return removed
    }

    private fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 255) }
    companion object { private const val MAGIC = 0x5a415431 } // ZAT1 + 12-byte IV + authenticated ciphertext.
}
