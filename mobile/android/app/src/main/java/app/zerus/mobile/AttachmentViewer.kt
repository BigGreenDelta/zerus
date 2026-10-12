package app.zerus.mobile

import android.app.Application
import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.InsertDriveFile
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.core.content.FileProvider
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID

data class AttachmentRowState(val loading: Boolean = false, val progress: Float? = null, val error: String = "")
data class OpenedAttachment(val key: String, val file: File, val name: String, val mime: String)

/**
 * Opens message attachments: a draft/outbox file from this phone's encrypted
 * store, else the exact bytes from the owning computer over `attachment_read`.
 * Reads are never retried automatically; tapping a failed row tries again.
 */
class AttachmentViewer(application: Application, private val store: AttachmentStore, private val api: RelayApi,
                       private val scope: CoroutineScope, private val connection: (String) -> Connection?,
                       private val access: (Target) -> AttachmentAccess) {
    private val cache = AttachmentViewCache(File(application.cacheDir, "attachment-view"))
    var rows by mutableStateOf<Map<String, AttachmentRowState>>(emptyMap()); private set
    var preview by mutableStateOf<OpenedAttachment?>(null); private set
    var external by mutableStateOf<OpenedAttachment?>(null); private set
    private val jobs = mutableMapOf<String, Job>()
    private val audioManager = application.getSystemService(Context.AUDIO_SERVICE) as android.media.AudioManager
    private var player: android.media.MediaPlayer? = null
    private var audioJob: Job? = null
    private var focus: android.media.AudioFocusRequest? = null
    private var audioPrepared = false
    private var playRequested = false
    private var seekInFlight = false
    private var audioRowKey = ""
    var playingKey by mutableStateOf(""); private set
    var playing by mutableStateOf(false); private set
    var audioPositionMs by mutableStateOf(0); private set

    /** Playback only begins after a user's Play tap and authenticated byte verification. */
    fun toggleAudio(target: Target?, file: DisplayedAttachment) {
        if (file.mime != "audio/wav") return
        val key = AttachmentIdentity.playbackKey(target, file)
        val current = player
        if (playingKey == key && current != null) {
            if (!audioPrepared) return
            runCatching {
                if (playing) { current.pause(); playing = false; playRequested = false }
                else {
                    playRequested = true
                    if (audioPositionMs >= current.duration) seekPrepared(current, 0)
                    else if (!seekInFlight) startAudio(current, key)
                }
            }
                .onFailure { audioFailed(key, "Could not play this recording. Try again.") }
            return
        }
        if (audioJob?.isActive == true && playingKey == key) return
        prepareAudio(target, file, key, true, 0)
    }
    /** Seeking is a read of this exact recording; a stopped recording remains paused. */
    fun seekAudio(target: Target?, file: DisplayedAttachment, fraction: Float, durationMs: Int) {
        if (file.mime != "audio/wav") return
        val position = VoiceWaveform.seekPosition(fraction, durationMs) ?: return
        val key = AttachmentIdentity.playbackKey(target, file)
        if (playingKey != key) { prepareAudio(target, file, key, false, position); return }
        audioPositionMs = position
        val current = player
        if (current != null && audioPrepared) runCatching { seekPrepared(current, position) }
            .onFailure { audioFailed(key, "Could not seek this recording. Try again.") }
    }
    private fun seekPrepared(current: android.media.MediaPlayer, position: Int) {
        audioPositionMs = position.coerceIn(0, current.duration.coerceAtLeast(0))
        seekInFlight = true
        current.seekTo(audioPositionMs.toLong(), android.media.MediaPlayer.SEEK_CLOSEST)
    }
    private fun startAudio(current: android.media.MediaPlayer, key: String) {
        if (player !== current || playingKey != key || !playRequested) return
        if (focus != null) { current.start(); playing = true; return }
        val request = android.media.AudioFocusRequest.Builder(android.media.AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
            .setAudioAttributes(android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_MEDIA)
                .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setOnAudioFocusChangeListener { change -> if (change < 0 && player === current) stopAudio() }.build()
        focus = request
        if (audioManager.requestAudioFocus(request) != android.media.AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            audioFailed(key, "Audio is in use. Tap Play again when it is available.")
        } else { current.start(); playing = true }
    }
    private fun prepareAudio(target: Target?, file: DisplayedAttachment, key: String, startPlaying: Boolean, positionMs: Int) {
        stopAudio(); playingKey = key
        val rowKey = AttachmentIdentity.rowKey(target, file); audioRowKey = rowKey
        playRequested = startPlaying; audioPositionMs = positionMs
        rows = rows + (rowKey to AttachmentRowState(loading = true))
        audioJob = scope.launch {
            try {
                val entry = load(rowKey, target, file)
                if (playingKey != key) return@launch
                val next = android.media.MediaPlayer()
                player = next
                next.setAudioAttributes(android.media.AudioAttributes.Builder()
                    .setUsage(android.media.AudioAttributes.USAGE_MEDIA).setContentType(android.media.AudioAttributes.CONTENT_TYPE_SPEECH).build())
                withContext(Dispatchers.IO) { next.setDataSource(entry.file.absolutePath) }
                next.setOnPreparedListener {
                    if (player !== next || playingKey != key) return@setOnPreparedListener
                    audioPrepared = true; rows = rows - rowKey
                    if (audioPositionMs > 0) seekPrepared(next, audioPositionMs)
                    else if (playRequested) startAudio(next, key)
                }
                next.setOnSeekCompleteListener {
                    if (player === next && playingKey == key) {
                        seekInFlight = false
                        audioPositionMs = next.currentPosition
                        if (playRequested && !playing) startAudio(next, key)
                    }
                }
                next.setOnCompletionListener {
                    if (player === next && playingKey == key) {
                        if (playRequested) stopAudio() else audioPositionMs = next.duration
                    }
                }
                next.setOnErrorListener { _, _, _ ->
                    if (player === next && playingKey == key) audioFailed(key, "Could not play this recording. Try again.")
                    true
                }
                next.prepareAsync()
                while (playingKey == key) {
                    kotlinx.coroutines.delay(200)
                    if (playing && !seekInFlight) audioPositionMs = runCatching { next.currentPosition }.getOrDefault(0)
                }
            } catch (cancelled: CancellationException) { throw cancelled }
            catch (_: Exception) { audioFailed(key, "The recording could not be loaded. Reconnect and try again.") }
        }
    }
    private fun audioFailed(key: String, message: String) {
        if (playingKey != key) return
        val rowKey = audioRowKey
        stopAudio(); failed(rowKey, message)
    }
    fun stopAudio() {
        audioJob?.cancel(); audioJob = null
        rows = rows - audioRowKey; audioRowKey = ""
        playingKey = ""; playing = false; audioPositionMs = 0
        audioPrepared = false; playRequested = false; seekInFlight = false
        player?.let { runCatching { it.release() } }; player = null
        focus?.let { audioManager.abandonAudioFocusRequest(it) }; focus = null
    }

    init { scope.launch(Dispatchers.IO) { runCatching { cache.trim() } } }

    fun state(target: Target?, file: DisplayedAttachment) = rows[AttachmentIdentity.rowKey(target, file)]

    fun open(target: Target?, file: DisplayedAttachment) {
        val key = AttachmentIdentity.rowKey(target, file)
        if (jobs[key]?.isActive == true) return
        rows = rows + (key to AttachmentRowState(loading = true))
        jobs[key] = scope.launch {
            try {
                val entry = load(key, target, file)
                rows = rows - key
                val opened = OpenedAttachment(key, entry.file, file.name, file.mime.ifBlank { entry.mime })
                if (AttachmentViewing.inApp(opened.mime)) preview = opened else external = opened
            } catch (error: CancellationException) {
                rows = rows - key; throw error
            } catch (error: AttachmentOpenException) {
                rows = rows + (key to AttachmentRowState(error = error.message.orEmpty()))
            } catch (error: AttachmentException) {
                rows = rows + (key to AttachmentRowState(error = error.message.orEmpty()))
            } catch (_: Exception) {
                rows = rows + (key to AttachmentRowState(error = "The attachment could not be opened. Try again."))
            } finally { jobs.remove(key) }
        }
    }

    private suspend fun load(key: String, target: Target?, file: DisplayedAttachment): AttachmentViewCache.Entry {
        val local = AttachmentIdentity.local(file)
        if (local != null) {
            val localKey = AttachmentIdentity.localKey(local)
            withContext(Dispatchers.IO) { cache.lookup(localKey) }?.let { return it }
            try { return decrypt(localKey, local) }
            catch (error: AttachmentException) { if (!AttachmentIdentity.remote(file) || target == null) throw error }
        }
        val reason = AttachmentIdentity.unavailableReason(target, file, target?.let(access) ?: AttachmentAccess.Unknown)
        if (reason != null || target == null) throw AttachmentOpenException(reason ?: "This attachment cannot be opened from the phone.")
        val remoteKey = AttachmentIdentity.remoteKey(target, file)
        withContext(Dispatchers.IO) { cache.lookup(remoteKey) }?.let { return it }
        val gateway = connection(target.connectionId) ?: throw AttachmentOpenException("Reconnect to this gateway, then try again.")
        return download(key, remoteKey, gateway, target, file)
    }

    private suspend fun decrypt(cacheKey: String, attachment: Attachment): AttachmentViewCache.Entry {
        val pending = withContext(Dispatchers.IO) { cache.begin(cacheKey, attachment.name, attachment.mime) }
        try {
            store.decrypt(attachment) { pending.write(it) }
            return withContext(Dispatchers.IO) { pending.commit(attachment.sha256) }
        } finally { withContext(NonCancellable + Dispatchers.IO) { pending.abort() } }
    }

    private suspend fun download(rowKey: String, cacheKey: String, gateway: Connection, target: Target,
                                 file: DisplayedAttachment): AttachmentViewCache.Entry {
        val pending = withContext(Dispatchers.IO) { cache.begin(cacheKey, file.name, file.mime) }
        try {
            var offset = 0L
            var first: AttachmentIdentity.Chunk? = null
            while (offset < file.bytes) {
                val length = minOf(AttachmentIdentity.CHUNK_BYTES.toLong(), file.bytes - offset).toInt()
                val id = UUID.randomUUID().toString()
                val receipt = try {
                    api.await(gateway, api.submit(gateway, target, AttachmentIdentity.OPERATION,
                        AttachmentIdentity.payload(target, file, id, offset, length), id), maxPolls = 150, pollDelayMs = 300)
                } catch (error: RelayException) { throw AttachmentOpenException(AttachmentIdentity.relayFailure(error.status)) }
                if (receipt.string("state") != "completed" || receipt.optJSONObject("result") == null)
                    throw AttachmentOpenException(AttachmentIdentity.failure(receipt))
                val chunk = withContext(Dispatchers.Default) {
                    AttachmentIdentity.chunk(receipt.getJSONObject("result"), id, target, file, offset, length, first)
                }
                if (first == null) first = chunk
                withContext(Dispatchers.IO) { pending.write(chunk.data) }
                offset += chunk.data.size
                rows = rows + (rowKey to AttachmentRowState(loading = true, progress = offset.toFloat() / file.bytes))
            }
            val digest = first?.sha256 ?: throw AttachmentOpenException("The attachment is empty.")
            return withContext(Dispatchers.IO) { pending.commit(digest) }
        } catch (error: java.io.IOException) {
            throw AttachmentOpenException("The gateway connection was interrupted. Try again.")
        } finally { withContext(NonCancellable + Dispatchers.IO) { pending.abort() } }
    }

    fun consumeExternal() { external = null }
    fun dismissPreview() { preview = null }
    fun failed(key: String, message: String) { rows = rows + (key to AttachmentRowState(error = message)) }

    /** Removes every decrypted copy, for example when a gateway is disconnected. */
    fun clear() {
        stopAudio()
        jobs.values.forEach { it.cancel() }
        preview = null; external = null; rows = emptyMap()
        scope.launch(Dispatchers.IO) { runCatching { cache.clear() } }
    }
}

object AttachmentViewing {
    private val images = setOf("image/png", "image/jpeg", "image/jpg", "image/gif", "image/webp", "image/bmp", "image/heic", "image/heif")
    fun inApp(mime: String) = mime.lowercase() in images

    /** Downsampled so the longest side stays within [maxSide]; null when the format cannot be decoded. */
    fun decode(file: File, maxSide: Int = 4096): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(file.path, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > maxSide) sample *= 2
        return BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample })
    }

    private fun uri(context: Context, opened: OpenedAttachment) =
        FileProvider.getUriForFile(context, context.packageName + ".attachments", opened.file)

    /** Opens with another app under a temporary read grant; returns an error message if none can. */
    fun view(context: Context, opened: OpenedAttachment, chooser: Boolean = false): String? {
        val type = opened.mime.ifBlank { "application/octet-stream" }
        return try {
            val intent = Intent(Intent.ACTION_VIEW).setDataAndType(uri(context, opened), type)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            context.startActivity(if (chooser) Intent.createChooser(intent, "Open with") else intent)
            null
        } catch (_: ActivityNotFoundException) { "No app on this phone can open $type files." }
        catch (_: IllegalArgumentException) { "The attachment could not be shared with another app." }
    }

    fun share(context: Context, opened: OpenedAttachment): String? = try {
        val uri = uri(context, opened)
        val intent = Intent(Intent.ACTION_SEND).setType(opened.mime.ifBlank { "application/octet-stream" })
            .putExtra(Intent.EXTRA_STREAM, uri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            .apply { clipData = ClipData.newRawUri(opened.name, uri) }
        context.startActivity(Intent.createChooser(intent, "Share attachment"))
        null
    } catch (_: ActivityNotFoundException) { "No app on this phone can receive this file." }
    catch (_: IllegalArgumentException) { "The attachment could not be shared with another app." }
}

private val RowColor = Color(0xFF243136)
private val Accent = Color(0xFF67E8CB)
private val Warning = Color(0xFFFFCB7D)
private val Secondary = Color(0xFF9AACB2)

private fun size(bytes: Long): String = if (bytes >= 1_048_576) "%.1f MiB".format(java.util.Locale.ROOT, bytes / 1_048_576.0)
    else if (bytes >= 1024) "${(bytes + 1023) / 1024} KiB" else "$bytes bytes"

/** Tappable attachment rows with progress, errors and manual retry. */
@Composable internal fun AttachmentList(attachments: List<DisplayedAttachment>, target: Target?, viewer: AttachmentViewer) {
    attachments.forEach { file ->
        val state = viewer.state(target, file)
        Surface(onClick = { viewer.open(target, file) }, enabled = state?.loading != true, color = RowColor, shape = RoundedCornerShape(10.dp),
            modifier = Modifier.fillMaxWidth().semantics {
                role = Role.Button
                contentDescription = "Open ${file.name}"
                state?.let { stateDescription = if (it.loading) "Opening" else it.error }
            }) {
            Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Icon(if (file.mime.startsWith("image/")) Icons.Default.Image else Icons.AutoMirrored.Filled.InsertDriveFile, null, tint = Accent, modifier = Modifier.size(20.dp))
                    Column(Modifier.weight(1f)) {
                        Text(file.name, maxLines = 2, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.bodyMedium)
                        Text("${size(file.bytes)}  ${file.mime}", color = Secondary, style = MaterialTheme.typography.labelSmall)
                    }
                    when {
                        state?.loading == true -> CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp, color = Accent)
                        state?.error?.isNotBlank() == true -> Icon(Icons.Default.Refresh, null, tint = Warning, modifier = Modifier.size(18.dp))
                        else -> Icon(Icons.AutoMirrored.Filled.OpenInNew, null, tint = Secondary, modifier = Modifier.size(18.dp))
                    }
                }
                if (state?.loading == true) state.progress?.let { LinearProgressIndicator(progress = { it }, Modifier.fillMaxWidth(), color = Accent) }
                if (state?.error?.isNotBlank() == true) Text(state.error + " Tap to try again.", color = Warning, style = MaterialTheme.typography.labelSmall)
            }
        }
    }
}

/** Hosts the in-app image preview and hands other files to an app that can open them. */
@Composable internal fun AttachmentViewerHost(viewer: AttachmentViewer) {
    val context = LocalContext.current
    viewer.external?.let { opened ->
        LaunchedEffect(opened) {
            viewer.consumeExternal()
            AttachmentViewing.view(context, opened)?.let { viewer.failed(opened.key, it) }
        }
    }
    viewer.preview?.let { opened -> ImagePreview(opened, viewer) }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun ImagePreview(opened: OpenedAttachment, viewer: AttachmentViewer) {
    ObscureConversation()
    val context = LocalContext.current
    var notice by remember(opened) { mutableStateOf("") }
    val image by produceState<Bitmap?>(null, opened) {
        value = withContext(Dispatchers.IO) { runCatching { AttachmentViewing.decode(opened.file) }.getOrNull() }
        if (value == null) {
            // A format this phone cannot render in-app goes to another viewer.
            viewer.dismissPreview()
            AttachmentViewing.view(context, opened)?.let { viewer.failed(opened.key, it) }
        }
    }
    var scale by remember(opened) { mutableFloatStateOf(1f) }
    var offset by remember(opened) { mutableStateOf(Offset.Zero) }
    Dialog(onDismissRequest = viewer::dismissPreview, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            image?.let { bitmap ->
                val painted = remember(bitmap) { bitmap.asImageBitmap() }
                Image(painted, opened.name, contentScale = ContentScale.Fit, modifier = Modifier.fillMaxSize()
                    .pointerInput(opened) {
                        detectTapGestures(onDoubleTap = { if (scale > 1f) { scale = 1f; offset = Offset.Zero } else scale = 2.5f })
                    }
                    .pointerInput(opened) {
                        detectTransformGestures { _, pan, zoom, _ ->
                            scale = (scale * zoom).coerceIn(1f, 8f)
                            offset = if (scale == 1f) Offset.Zero else offset + pan
                        }
                    }
                    .graphicsLayer(scaleX = scale, scaleY = scale, translationX = offset.x, translationY = offset.y))
            } ?: CircularProgressIndicator(Modifier.align(Alignment.Center), color = Accent)
            Row(Modifier.fillMaxWidth().background(Color.Black.copy(alpha = .55f)).windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top + WindowInsetsSides.Horizontal))
                .padding(horizontal = 4.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = viewer::dismissPreview) { Icon(Icons.Default.Close, "Close", tint = Color.White) }
                Text(opened.name, Modifier.weight(1f), color = Color.White, maxLines = 1, overflow = TextOverflow.Ellipsis, style = MaterialTheme.typography.titleSmall)
                IconButton(onClick = { notice = AttachmentViewing.view(context, opened, chooser = true).orEmpty() }) {
                    Icon(Icons.AutoMirrored.Filled.OpenInNew, "Open with another app", tint = Color.White)
                }
                IconButton(onClick = { notice = AttachmentViewing.share(context, opened).orEmpty() }) {
                    Icon(Icons.Default.Share, "Share", tint = Color.White)
                }
            }
            if (notice.isNotBlank()) Text(notice, Modifier.align(Alignment.BottomCenter).windowInsetsPadding(WindowInsets.safeDrawing)
                .padding(16.dp).background(Color.Black.copy(alpha = .7f), RoundedCornerShape(8.dp)).padding(10.dp), color = Warning)
        }
    }
}
