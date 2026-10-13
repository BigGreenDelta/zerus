package app.zerus.mobile

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.os.Build
import android.util.AtomicFile
import androidx.work.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONObject
import java.io.File
import java.io.OutputStream
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.TimeUnit

data class UpdateState(val status: String = "loading", val available: AndroidUpdate? = null,
    val downloaded: AndroidUpdate? = null, val lastCheck: Long = 0, val sessionId: Int = -1,
    val nonce: String = "", val error: String = "", val bytes: Long = 0, val channel: String = "dev",
    val installProgress: Float? = null)

class AndroidUpdates private constructor(private val context: Context) {
    companion object {
        private var instance: AndroidUpdates? = null
        @Synchronized fun get(context: Context): AndroidUpdates = instance ?: AndroidUpdates(context.applicationContext).also { instance = it }
    }
    val state = MutableStateFlow(UpdateState())
    @Volatile var confirmation: Intent? = null; private set
    fun callbackPersistenceFailed() { state.value=state.value.copy(error="Could not save Android's install result. Check the existing installation; it has not been repeated.") }
    private val mutex = Mutex()
    private var loaded = false
    private val directory = File(context.noBackupFilesDir,"updates-v1")
    private val record = AtomicFile(File(directory,"state.json"))
    private val apk = File(directory,"verified.apk")
    private val partial = File(directory,"download.part")
    private val network = UpdateDownload()
    private val installer get() = context.packageManager.packageInstaller
    private fun save(next: UpdateState) {
        check(directory.isDirectory || directory.mkdirs()) { "Update storage is unavailable." }
        val raw = JSONObject().put("status",next.status).put("lastCheck",next.lastCheck).put("sessionId",next.sessionId)
            .put("nonce",next.nonce).put("error",next.error).put("available",next.available?.raw ?: JSONObject.NULL)
            .put("downloaded",next.downloaded?.raw ?: JSONObject.NULL).put("channel",next.channel)
        val stream = record.startWrite()
        try { stream.write(raw.toString().toByteArray());record.finishWrite(stream) }
        catch(e:Throwable) { record.failWrite(stream);throw e }
        state.value = next
    }
    private fun load() {
        if(loaded) return
        loaded = true
        var next = try {
            if(!record.baseFile.exists() && !File(directory,"state.json.bak").exists()) UpdateState("idle") else {
                val bytes=record.openRead().use { input ->
                    val output=java.io.ByteArrayOutputStream();val buffer=ByteArray(8192)
                    while(true) { val count=input.read(buffer);if(count<0) break;require(output.size()+count<=256*1024);output.write(buffer,0,count) }
                    output.toByteArray()
                }
                val value = JSONObject(bytes.toString(Charsets.UTF_8))
                val channel = value.optString("channel", "dev")
                require(channel in setOf("stable", "dev"))
                UpdateState(value.getString("status"), (value.opt("available") as? String)?.let(UpdatePolicy::parseSaved),
                    (value.opt("downloaded") as? String)?.let(UpdatePolicy::parseSaved), value.optLong("lastCheck"),
                    value.optInt("sessionId",-1),value.optString("nonce"),value.optString("error"),channel=channel).also {
                    require(it.available?.channel in setOf(null, channel) && it.downloaded?.channel in setOf(null, channel))
                }
            }
        } catch(_:Exception) { UpdateState("idle",error="Saved update details could not be read. Check again.") }
        partial.delete()
        if(next.status in setOf("checking","downloading")) next=next.copy(status=if(next.downloaded != null && apk.isFile) "downloaded" else "idle",error="The previous update transfer was interrupted. You can try again.")
        if(next.downloaded != null && !apk.isFile) next=next.copy(downloaded=null,status="idle")
        // An unrecorded/uncertain session is never committed again.
        if(next.sessionId < 0) installer.mySessions.firstOrNull { it.appPackageName == UpdatePolicy.PACKAGE }?.let {
            next=next.copy(sessionId=it.sessionId,status="installing",error="Android has an existing update session. Cancel it before starting another.")
        }
        state.value=next
    }
    suspend fun recover() = withContext(Dispatchers.IO) { mutex.withLock {
        load();var current=state.value
        val version=installed().versionCode
        if(current.available != null && version >= current.available.versionCode) { current=current.copy(available=null);save(current) }
        if(current.downloaded != null && installed().versionCode >= current.downloaded.versionCode) {
            save(current.copy(status="installed",available=current.available?.takeIf { it.versionCode>version },downloaded=null,sessionId=-1,nonce="",error=""));apk.delete();confirmation=null
        } else if(current.sessionId >= 0) {
            val session=installer.getSessionInfo(current.sessionId)
            if(session == null && current.status=="awaiting_result") {
                // Session removal can precede the observable replacement package or its callback.
                // Keep the accepted installation pending until exact outcome or explicit cancellation.
                state.value=current.copy(installProgress=null)
            } else if(session == null) {
                save(current.copy(status=if(current.downloaded != null) "downloaded" else "idle",sessionId=-1,nonce="",error="Android ended the previous install session. No update was repeated.",installProgress=null));confirmation=null
            } else state.value=current.copy(installProgress=session.progress.takeIf { it.isFinite() }?.coerceIn(0f,1f))
        }
    } }
    suspend fun check(manual: Boolean = false) = withContext(Dispatchers.IO) { mutex.withLock {
        load();val old=state.value
        if(old.sessionId >= 0 || (!manual && !UpdatePolicy.shouldCheck(old.lastCheck,System.currentTimeMillis(),old.channel))) return@withLock
        save(old.copy(status="checking",lastCheck=System.currentTimeMillis(),error=""))
        try {
            val update=UpdatePolicy.parse(network.feed(old.channel),old.channel)
            currentCoroutineContext().ensureActive()
            val current=installed()
            require(update.signingCert in current.signers) { "This channel uses a different signing key." }
            require(update.minSdk <= Build.VERSION.SDK_INT) { "This update requires a newer Android version." }
            save(state.value.copy(available=update.takeIf { it.versionCode > current.versionCode },
                status=if(old.downloaded != null) "downloaded" else if(update.versionCode > current.versionCode) "available" else if(update.versionCode < current.versionCode) "ahead" else "current"))
            if(update.versionCode > current.versionCode) try { UpdateNotifications.available(context, update) } catch(_:Exception) { }
            else UpdateNotifications.clear(context)
        } catch(e:Exception) {
            save(state.value.copy(status=if(old.downloaded != null) "downloaded" else if(old.available != null) "available" else "idle",
                error="Update check failed. Your installed app and downloaded update are unchanged."))
            if(e is CancellationException) throw e
        }
    } }
    suspend fun setChannel(channel: String) {
        withContext(Dispatchers.IO) { mutex.withLock {
            load();require(channel in setOf("stable", "dev"))
            val old=state.value
            require(old.sessionId < 0) { "Finish or cancel the existing installation before changing channels." }
            if(channel == old.channel) return@withLock
            save(UpdateState(status="idle",channel=channel))
            UpdateNotifications.clear(context)
            apk.delete();partial.delete();confirmation=null
        } }
        check(manual=true)
    }
    suspend fun download(update: AndroidUpdate) = withContext(Dispatchers.IO) { mutex.withLock {
        load();require(state.value.sessionId < 0 && state.value.available == update) { "Update selection changed." }
        save(state.value.copy(status="downloading",error="",bytes=0))
        try {
            network.apk(update,partial) { state.value=state.value.copy(bytes=it) }
            verify(update,partial)
            currentCoroutineContext().ensureActive()
            check(partial.renameTo(apk)) { "Could not save the downloaded update." }
            save(state.value.copy(status="downloaded",downloaded=update,bytes=update.size))
        } catch(e:Exception) {
            partial.delete()
            withContext(NonCancellable) { save(state.value.copy(status=if(state.value.downloaded != null && apk.exists()) "downloaded" else "available",error=if(e is CancellationException) "Download canceled." else "Download or APK verification failed. No installation started.")) }
            if(e is CancellationException) throw e
        }
    } }
    private fun identity(info: PackageInfo): UpdateApkIdentity {
        @Suppress("DEPRECATION") val signatures = if(Build.VERSION.SDK_INT >= 28) info.signingInfo?.apkContentsSigners else info.signatures
        val hashes=signatures.orEmpty().map { MessageDigest.getInstance("SHA-256").digest(it.toByteArray()).joinToString("") { byte -> "%02x".format(byte) } }.toSet()
        @Suppress("DEPRECATION") val version=if(Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
        return UpdateApkIdentity(info.packageName,version,info.applicationInfo?.minSdkVersion ?: Int.MAX_VALUE,hashes,info.splitNames?.isNotEmpty()==true)
    }
    @Suppress("DEPRECATION") private fun flags() = if(Build.VERSION.SDK_INT >= 28) PackageManager.GET_SIGNING_CERTIFICATES else PackageManager.GET_SIGNATURES
    @Suppress("DEPRECATION") private fun installed() = identity(context.packageManager.getPackageInfo(UpdatePolicy.PACKAGE,flags()))
    @Suppress("DEPRECATION") private suspend fun verify(update: AndroidUpdate, file: File) {
        file.inputStream().use { copyVerified(it,object:OutputStream() { override fun write(b:Int) {};override fun write(b:ByteArray,off:Int,len:Int) {} },update.size,update.sha256) }
        val info=context.packageManager.getPackageArchiveInfo(file.absolutePath,flags()) ?: error("Invalid APK.")
        UpdatePolicy.verifyIdentity(update,identity(info),installed(),Build.VERSION.SDK_INT)
        require(info.versionName == update.versionName) { "Unexpected APK version name." }
    }
    suspend fun install(update: AndroidUpdate) = withContext(Dispatchers.IO) { mutex.withLock {
        load();require(UpdateInstallGuard.canStart(state.value.sessionId,state.value.downloaded,update)) { "An installation is already pending or the update changed." }
        require(context.packageManager.canRequestPackageInstalls()) { "Allow Zerus to install updates in Android settings first." }
        verify(update,apk)
        val params=PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(UpdatePolicy.PACKAGE);setSize(update.size)
            if(Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
        }
        val id=installer.createSession(params);val nonce=UUID.randomUUID().toString();var recorded=false
        try {
            installer.openSession(id).use { session ->
                session.openWrite("base.apk",0,update.size).use { output -> apk.inputStream().use { copyVerified(it,output,update.size,update.sha256) };session.fsync(output) }
                currentCoroutineContext().ensureActive()
                save(state.value.copy(status="installing",sessionId=id,nonce=nonce,error=""));recorded=true
                val callback=Intent(context,UpdateInstallReceiver::class.java).setAction("app.zerus.mobile.UPDATE_RESULT").putExtra("nonce",nonce)
                val sender=PendingIntent.getBroadcast(context,id,callback,PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE).intentSender
                session.commit(sender)
            }
        } catch(e:Exception) {
            if(!recorded) installer.abandonSession(id)
            // Once recorded, an exception around commit is uncertain: never submit again.
            if(recorded) save(state.value.copy(error="Installation status is unknown. Check or cancel the existing Android session."))
            throw e
        }
        id
    } }
    /** Consume the exact prompt before launching it, so a second tap or callback cannot reopen it. */
    suspend fun openConfirmation(sessionId: Int, onOpen: (Intent) -> Unit): Boolean = withContext(Dispatchers.IO) { mutex.withLock {
        load();val old=state.value;val intent=confirmation
        if(!UpdateInstallGuard.canOpenConfirmation(sessionId,old.sessionId,old.status,intent != null)) return@withLock false
        // Persist before the external Activity: interruption keeps this session pending, never recommits it.
        save(old.copy(status="awaiting_result",error=""))
        var opened=false
        try { withContext(Dispatchers.Main.immediate) { onOpen(checkNotNull(intent));opened=true;confirmation=null } }
        catch(e:Exception) {
            // Cancellation on the return to IO cannot revive a prompt Android already accepted.
            if(!opened) save(state.value.copy(status="confirmation"))
            throw e
        }
        true
    } }
    suspend fun cancelInstall() = withContext(Dispatchers.IO) { mutex.withLock {
        load();val old=state.value
        if(old.downloaded != null && installed().versionCode>=old.downloaded.versionCode) {
            save(old.copy(status="installed",available=null,downloaded=null,sessionId=-1,nonce="",error=""));apk.delete();confirmation=null;return@withLock
        }
        if(old.sessionId >= 0 && installer.getSessionInfo(old.sessionId) != null) try { installer.abandonSession(old.sessionId) } catch(e:Exception) {
            if(installer.getSessionInfo(old.sessionId) != null && (old.downloaded == null || installed().versionCode<old.downloaded.versionCode)) throw e
        }
        if(old.downloaded != null && installed().versionCode>=old.downloaded.versionCode) {
            save(old.copy(status="installed",available=null,downloaded=null,sessionId=-1,nonce="",error=""));apk.delete();confirmation=null;return@withLock
        }
        save(old.copy(status=if(old.downloaded != null) "downloaded" else "idle",sessionId=-1,nonce="",error="Installation canceled."));confirmation=null
    } }
    suspend fun result(intent: Intent) = withContext(Dispatchers.IO) { mutex.withLock {
        load();val old=state.value
        if(!UpdateInstallGuard.ownsCallback(old.sessionId,old.nonce,intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID,-1),intent.getStringExtra("nonce"))) return@withLock
        when(intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                if(old.status=="awaiting_result") return@withLock
                @Suppress("DEPRECATION") val action=intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                confirmation=action
                save(old.copy(status="confirmation",error=""))
            }
            PackageInstaller.STATUS_SUCCESS -> {
                if(old.downloaded != null && installed().versionCode >= old.downloaded.versionCode) {
                    save(old.copy(status="installed",available=null,downloaded=null,sessionId=-1,nonce="",error=""));apk.delete();confirmation=null
                }
            }
            else -> { save(old.copy(status=if(old.downloaded != null) "downloaded" else "idle",sessionId=-1,nonce="",error="Android did not install the update. You can review and try again."));confirmation=null }
        }
    } }
}

class UpdateInstallReceiver: BroadcastReceiver() {
    override fun onReceive(context: Context,intent: Intent) {
        val pending=goAsync()
        CoroutineScope(SupervisorJob()+Dispatchers.IO).launch { try { AndroidUpdates.get(context).result(intent) } catch(_:Exception) {
            // Keep the durable pending session recoverable; never resubmit on callback failure.
            AndroidUpdates.get(context).callbackPersistenceFailed()
        } finally { pending.finish() } }
    }
}
class UpdateDiscoveryWorker(context: Context,params: WorkerParameters): CoroutineWorker(context,params) {
    override suspend fun doWork(): Result {
        return try { AndroidUpdates.get(applicationContext).check();Result.success() } catch(e:Exception) { if(e is CancellationException) throw e;Result.failure() }
    }
    companion object {
        fun schedule(context: Context) = WorkManager.getInstance(context).enqueueUniquePeriodicWork("zerus-update-discovery",
            ExistingPeriodicWorkPolicy.UPDATE,PeriodicWorkRequestBuilder<UpdateDiscoveryWorker>(15,TimeUnit.MINUTES)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build()).build())
    }
}
