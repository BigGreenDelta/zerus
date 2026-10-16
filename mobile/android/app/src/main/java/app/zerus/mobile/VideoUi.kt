package app.zerus.mobile

import android.graphics.BitmapFactory
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FlipCameraAndroid
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Base64

@Composable internal fun LiveVideoCircle(recorder: RoundVideoRecorder) {
    Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
        AndroidView(factory = { context -> PreviewView(context).apply {
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            scaleType = PreviewView.ScaleType.FILL_CENTER
            recorder.previewSurface(surfaceProvider)
        } }, onRelease = { recorder.previewSurface(null) },
            modifier = Modifier.size(200.dp).clip(CircleShape))
        IconButton(onClick = { recorder.switchCamera() }, modifier = Modifier.align(Alignment.BottomEnd)) {
            Icon(Icons.Default.FlipCameraAndroid, "Switch camera")
        }
    }
}

/** Only an explicit tap creates a decoder; the shared attachment player owns audio focus. */
@Composable internal fun VideoCircle(model: ZerusViewModel, target: Target?, video: VideoMessage, diameter: androidx.compose.ui.unit.Dp = 200.dp) {
    val viewer = model.attachments
    val key = AttachmentIdentity.playbackKey(target, video.file)
    val active = viewer.playingKey == key
    val row = viewer.state(target, video.file)
    val poster by produceState<android.graphics.Bitmap?>(null, video.posterBase64) {
        value = withContext(Dispatchers.Default) {
            runCatching {
                val bytes = Base64.getDecoder().decode(video.posterBase64)
                if (bytes.size !in 4..8192) return@runCatching null
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
                if (bounds.outWidth !in 1..512 || bounds.outHeight !in 1..512) return@runCatching null
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            }.getOrNull()
        }
    }
    Column {
        Box(Modifier.size(diameter).clip(CircleShape).background(MaterialTheme.colorScheme.surfaceVariant)) {
            poster?.let { Image(it.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop) }
            if (active) AndroidView(factory = { context -> TextureView(context).apply {
                var owned: Surface? = null
                fun crop(width: Int, height: Int) {
                    if (width <= 0 || height <= 0) return
                    val source = video.width.toFloat() / video.height
                    val viewport = width.toFloat() / height
                    setTransform(android.graphics.Matrix().apply {
                        setScale(if (source > viewport) source / viewport else 1f,
                            if (source < viewport) viewport / source else 1f, width / 2f, height / 2f)
                    })
                }
                surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                        crop(width, height); owned = Surface(texture).also { viewer.bindVideoSurface(key, it) }
                    }
                    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) = crop(width, height)
                    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
                    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                        viewer.bindVideoSurface(key, null); owned?.release(); owned = null; return true
                    }
                }
            } }, onRelease = { if (viewer.playingKey == key) viewer.stopAudio() }, modifier = Modifier.fillMaxSize())
            Box(Modifier.fillMaxSize().clickable(role = Role.Button,
                onClickLabel = if (active && viewer.playing) "Pause video message" else "Play video message") {
                viewer.toggleVideo(target, video)
            }, contentAlignment = Alignment.Center) {
                if (row?.loading == true) CircularProgressIndicator(Modifier.size(32.dp))
                else if (!active || !viewer.playing) Icon(Icons.Default.PlayArrow, "Play video message", Modifier.size(48.dp))
            }
        }
        Text("${if (active) viewer.audioPositionMs / 1000 else video.durationMs / 1000}s", style = MaterialTheme.typography.labelSmall)
        row?.error?.takeIf { it.isNotBlank() }?.let { Text(it, color = MaterialTheme.colorScheme.error) }
    }
}
