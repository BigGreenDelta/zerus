package app.zerus.mobile

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import androidx.core.content.ContextCompat

internal object SpeechMicrophoneAccess {
    fun granted(context: Context) = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** App details is the public, portable route; Android exposes no public single-permission page. */
    fun openSettings(context: Context): Boolean = try {
        context.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", context.packageName, null)))
        true
    } catch (_: Exception) { false }
}
