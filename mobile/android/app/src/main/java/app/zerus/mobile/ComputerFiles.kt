package app.zerus.mobile

import android.app.Application
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.UUID

/** Downloads are explicit, immutable snapshots. Every chunk checks the original target and digest. */
internal class ComputerFileDownloads(private val cache: AttachmentViewCache,
    private val read: suspend (Target, ComputerFileReference, String, Long, Int, ComputerFileProtocol.Chunk?) -> JSONObject) {
    suspend fun load(target: Target, reference: ComputerFileReference, progress: (Float) -> Unit): Pair<String, AttachmentViewCache.Entry> {
        suspend fun range(offset: Long, length: Int, first: ComputerFileProtocol.Chunk?): ComputerFileProtocol.Chunk {
            val id = UUID.randomUUID().toString()
            return withContext(Dispatchers.Default) {
                ComputerFileProtocol.chunk(read(target, reference, id, offset, length, first), id, target, reference, offset, length, first)
            }
        }
        val first = range(0, ComputerFileProtocol.CHUNK_BYTES, null)
        val key = ComputerFileProtocol.cacheKey(target, first)
        withContext(Dispatchers.IO) { cache.lookup(key) }?.let { return key to it }
        val pending = withContext(Dispatchers.IO) { cache.begin(key, first.name, first.mime) }
        try {
            withContext(Dispatchers.IO) { pending.write(first.data) }
            var offset = first.data.size.toLong()
            progress(offset.toFloat() / first.bytes)
            while (offset < first.bytes) {
                val length = minOf(ComputerFileProtocol.CHUNK_BYTES.toLong(), first.bytes - offset).toInt()
                val chunk = range(offset, length, first)
                withContext(Dispatchers.IO) { pending.write(chunk.data) }
                offset += chunk.data.size
                progress(offset.toFloat() / first.bytes)
            }
            return key to withContext(Dispatchers.IO) { pending.commit(first.sha256) }
        } finally { withContext(NonCancellable + Dispatchers.IO) { pending.abort() } }
    }
}

internal data class ComputerFileSelection(val id: String, val target: Target, val reference: ComputerFileReference,
    val computer: String, val busy: Boolean = false, val progress: Float? = null, val error: String = "")
internal data class ComputerFileExport(val id: String, val selectionId: String, val opened: OpenedAttachment, val launched: Boolean = false, val writing: Boolean = false)

/** The selected link and any Android document result retain their own original computer and file. */
internal class ComputerFiles(private val application: Application, private val api: RelayApi, private val scope: CoroutineScope,
    private val connection: (String) -> Connection?, private val access: (Target) -> AttachmentAccess) {
    private val cache = AttachmentViewCache(File(application.cacheDir, "computer-file-view"))
    private val downloads = ComputerFileDownloads(cache, ::read)
    var selection by mutableStateOf<ComputerFileSelection?>(null); private set
    var external by mutableStateOf<OpenedAttachment?>(null); private set
    var export by mutableStateOf<ComputerFileExport?>(null); private set
    var notice by mutableStateOf(""); private set
    private var job: Job? = null
    private val reads = mutableSetOf<Job>()
    private var exportJob: Job? = null
    var clearing by mutableStateOf(false); private set

    fun show(target: Target, reference: ComputerFileReference, computer: String) {
        job?.cancel(); external = null
        selection = ComputerFileSelection(UUID.randomUUID().toString(), target, reference, computer)
    }
    fun close() { job?.cancel(); selection = null; external = null }
    /** Forgetting a gateway removes private copies after owned I/O has drained. */
    fun clear() {
        if (clearing) return
        clearing = true
        val reading = reads.toList(); reading.forEach { it.cancel() }
        val writing = exportJob
        selection = null; external = null; export = null
        scope.launch(NonCancellable + Dispatchers.IO) {
            reading.forEach { it.join() }; writing?.join()
            val error = runCatching { cache.clear() }.exceptionOrNull()
            withContext(Dispatchers.Main) {
                clearing = false
                if (error != null) notice = "Temporary downloaded files could not be removed."
            }
        }
    }
    fun consumeExternal() { external = null }
    fun consumeNotice() { notice = "" }
    fun open(save: Boolean) {
        val chosen = selection ?: return
        if (chosen.busy || export != null || clearing) return
        selection = chosen.copy(busy = true, error = "")
        val reading = scope.launch(start = CoroutineStart.LAZY) {
            try {
                if (chosen.target.connectionId == "demo") throw AttachmentOpenException("Computer files are not available in the demo.")
                if (chosen.target.run.isBlank() || AttachmentIdentity.conversation(chosen.target).isBlank())
                    throw AttachmentOpenException("Refresh this conversation, then open the file again.")
                when (access(chosen.target)) {
                    AttachmentAccess.Unsupported -> throw AttachmentOpenException(ComputerFileProtocol.UPDATE_REQUIRED)
                    AttachmentAccess.Offline -> throw AttachmentOpenException("This computer is offline. Connect it and try again.")
                    AttachmentAccess.Unknown -> throw AttachmentOpenException("Refresh this computer's connection, then try again.")
                    AttachmentAccess.Supported -> Unit
                }
                val (key, entry) = downloads.load(chosen.target, chosen.reference) { progress ->
                    if (selection?.id == chosen.id) selection = selection?.copy(progress = progress)
                }
                if (selection?.id != chosen.id) return@launch
                selection = selection?.copy(busy = false, progress = null)
                val opened = OpenedAttachment(key, entry.file, entry.name, entry.mime)
                if (save) export = ComputerFileExport(UUID.randomUUID().toString(), chosen.id, opened)
                else external = opened
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { failed(chosen.id, error.message?.take(300).orEmpty().ifBlank { "The file could not be downloaded. Try again." }) }
            finally { reads.remove(coroutineContext[Job]) }
        }
        job = reading; reads += reading; reading.start()
    }
    fun failed(id: String, error: String) {
        if (selection?.id == id) selection = selection?.copy(busy = false, progress = null, error = error)
        else notice = error
    }
    private suspend fun read(target: Target, reference: ComputerFileReference, id: String, offset: Long, length: Int,
        first: ComputerFileProtocol.Chunk?): JSONObject {
        val gateway = connection(target.connectionId) ?: throw AttachmentOpenException("Reconnect to the gateway, then try again.")
        val receipt = try {
            api.await(gateway, api.submit(gateway, target, ComputerFileProtocol.OPERATION,
                ComputerFileProtocol.payload(target, reference, id, offset, length, first), id), maxPolls = 150, pollDelayMs = 300)
        } catch (error: RelayException) {
            throw AttachmentOpenException(when (error.status) {
                400, 409 -> ComputerFileProtocol.UPDATE_REQUIRED
                401, 403 -> "Reconnect to this gateway, then try again."
                429 -> "The gateway is busy. Try again in a moment."
                else -> "The gateway could not deliver this file. Try again."
            })
        }
        if (receipt.string("state") != "completed" || receipt.optJSONObject("result") == null)
            throw AttachmentOpenException(ComputerFileProtocol.failure(receipt))
        return receipt.getJSONObject("result")
    }
    /** Consume before the platform picker so activity recreation cannot launch it twice. */
    fun markExportLaunched(id: String): Boolean {
        val pending = export?.takeIf { it.id == id && !it.launched } ?: return false
        export = pending.copy(launched = true)
        return true
    }
    fun finishExport(id: String, uri: Uri?) {
        val pending = export?.takeIf { it.id == id && it.launched && !it.writing } ?: return
        // Keep the original export locked until its write finishes, even if the
        // user navigates or taps a file belonging to another computer.
        if (uri == null) { export = null; return }
        export = pending.copy(writing = true)
        exportJob = scope.launch {
            try {
                require(uri.scheme == "content") { "Android returned an invalid save destination." }
                withContext(Dispatchers.IO) {
                    val entry = cache.lookup(pending.opened.key) ?: throw AttachmentOpenException("This saved copy expired. Choose Save again.")
                    application.contentResolver.openOutputStream(uri, "w")?.use { output ->
                        entry.file.inputStream().use { input -> input.copyTo(output) }
                    } ?: throw AttachmentOpenException("Android could not open the save destination.")
                }
                notice = "Saved ${pending.opened.name}"
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (error: Exception) { failed(pending.selectionId, error.message.orEmpty().ifBlank { "The file could not be saved." }) }
            finally { if (export?.id == pending.id) export = null }
        }
    }
}
