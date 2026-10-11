package app.zerus.mobile

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat

/** One replaceable, silent notice. Update checks never download or install. */
internal object UpdateNotifications {
    fun available(context: Context, update: AndroidUpdate) {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel("zerus_updates", "App updates", NotificationManager.IMPORTANCE_LOW))
        if(!manager.areNotificationsEnabled() || manager.getNotificationChannel("zerus_updates")?.importance == NotificationManager.IMPORTANCE_NONE) return
        if(Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        val key = "${update.releaseId}:${update.versionCode}"
        val preferences = context.getSharedPreferences("zerus-update-notices", Context.MODE_PRIVATE)
        if(preferences.getString(update.channel, "") == key) return
        val intent = Intent(context, MainActivity::class.java).putExtra("open_updates", true)
        val pending = PendingIntent.getActivity(context, 42, intent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        if(!preferences.edit().putString(update.channel, key).commit()) return
        val notification = NotificationCompat.Builder(context, "zerus_updates")
            .setSmallIcon(R.drawable.ic_zerus).setContentTitle("Zerus ${update.versionName} is available")
            .setContentText("Open Settings to review the ${update.channel} update.")
            .setContentIntent(pending).setAutoCancel(true).setOnlyAlertOnce(true).build()
        manager.notify("zerus:app-update", 42, notification)
    }
    fun clear(context: Context) = context.getSystemService(NotificationManager::class.java).cancel("zerus:app-update", 42)
}
