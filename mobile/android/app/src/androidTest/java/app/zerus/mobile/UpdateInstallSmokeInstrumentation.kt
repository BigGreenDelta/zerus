package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.launch
import kotlinx.coroutines.Job
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Disposable app fixture only: exact installer callbacks and real native action/layout semantics. */
class UpdateInstallSmokeInstrumentation:Instrumentation() {
    private var arguments=Bundle()
    override fun onCreate(arguments:Bundle?) { super.onCreate(arguments);this.arguments=arguments ?: Bundle();start() }
    override fun onStart() {
        val result=Bundle()
        try {
            check(BuildConfig.DEBUG)
            if(arguments.getString("mode")=="verify-ota") {
                runBlocking { AndroidUpdates.get(targetContext).recover() }
                check(AndroidUpdates.get(targetContext).state.value.status=="installed")
                check(PrivateStore(targetContext).messageState().drafts.any { it.text=="Retained OTA fixture draft" })
                result.putString("stream","Actual Android self-update preserved and decrypted the retained synthetic draft; installed package observation completed the same session.\n")
                finish(Activity.RESULT_OK,result);return
            }
            if(arguments.getString("mode")=="stage-ota") {
                stageOta()
                result.putString("stream","Staged synthetic offline APK and encrypted draft for external system-installer qualification.\n")
                finish(Activity.RESULT_OK,result);return
            }
            callbacks();geometry()
            result.putString("stream","Update installation passed: exact one-shot confirmation, failed-launch recovery, duplicate/foreign callback guards, durable result waiting, cancellation, stable normal/narrow action geometry and disabled repeated tap.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            screenshot("update-install-failure.png")
            result.putString("stream","Update installation failed: $error\n${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun stageOta() {
        val metadata=File(targetContext.getExternalFilesDir(null),"update-fixture.json").readText()
        val update=UpdatePolicy.parse(metadata)
        val directory=File(targetContext.noBackupFilesDir,"updates-v1").apply { mkdirs() }
        File(targetContext.getExternalFilesDir(null),"update-fixture.apk").copyTo(File(directory,"verified.apk"),overwrite=true)
        File(directory,"state.json").writeText(JSONObject().put("status","downloaded").put("downloaded",update.raw)
            .put("available",update.raw).put("sessionId",-1).put("channel","dev").put("lastCheck",System.currentTimeMillis()).toString())
        val store=PrivateStore(targetContext)
        val target=Target("fixture-computer","codex/fixture/update","fixture-run","fixture-conversation","fixture-workspace")
        store.saveMessageState(MessageState(drafts=listOf(Draft(target,"Retained OTA fixture draft"))))
    }
    private fun callbacks()=runBlocking {
        val installer=targetContext.packageManager.packageInstaller
        val id=installer.createSession(PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(UpdatePolicy.PACKAGE)
        })
        val directory=File(targetContext.noBackupFilesDir,"updates-v1").apply { mkdirs() }
        val record=File(directory,"state.json")
        // Session is synthetic and uncommitted; no APK is installed by this runner.
        record.writeText(JSONObject().put("status","installing").put("sessionId",id).put("nonce","synthetic-nonce")
            .put("channel","dev").put("lastCheck",System.currentTimeMillis()).toString())
        val updates=AndroidUpdates.get(targetContext)
        try {
            updates.recover()
            fun callback(status:Int,nonce:String="synthetic-nonce")=Intent()
                .putExtra(PackageInstaller.EXTRA_SESSION_ID,id).putExtra("nonce",nonce)
                .putExtra(PackageInstaller.EXTRA_STATUS,status)
                .putExtra(Intent.EXTRA_INTENT,Intent(targetContext,MainActivity::class.java))
            updates.result(callback(PackageInstaller.STATUS_PENDING_USER_ACTION,"foreign"))
            check(updates.state.value.status=="installing" && updates.confirmation==null)
            updates.result(callback(PackageInstaller.STATUS_PENDING_USER_ACTION))
            check(updates.state.value.status=="confirmation" && updates.confirmation!=null)
            try {
                updates.openConfirmation(id) { throw ActivityNotFoundException("Synthetic failed launch") }
                error("Failed launch was accepted")
            } catch(_:ActivityNotFoundException) { }
            check(updates.state.value.status=="confirmation" && updates.confirmation!=null)
            val opened=AtomicInteger()
            check(updates.openConfirmation(id) {
                check(updates.state.value.status=="awaiting_result")
                check(JSONObject(record.readText()).getString("status")=="awaiting_result")
                opened.incrementAndGet()
            })
            check(!updates.openConfirmation(id) { opened.incrementAndGet() })
            updates.result(callback(PackageInstaller.STATUS_PENDING_USER_ACTION))
            check(updates.state.value.status=="awaiting_result" && updates.confirmation==null && opened.get()==1)
            // A new instance models the process boundary, with the real retained Android session.
            val recovered=AndroidUpdates::class.java.getDeclaredConstructor(Context::class.java).apply { isAccessible=true }.newInstance(targetContext)
            recovered.recover()
            check(recovered.state.value.sessionId==id && recovered.state.value.status=="awaiting_result")
            check(!recovered.openConfirmation(id) { opened.incrementAndGet() })
            installer.abandonSession(id)
            updates.recover()
            check(updates.state.value.sessionId==id && updates.state.value.status=="awaiting_result")
            check(!updates.openConfirmation(id) { error("Missing session reopened confirmation") })
            updates.cancelInstall()
            check(updates.state.value.sessionId<0 && installer.getSessionInfo(id)==null)
            // Cancel while returning from a confirmed Main-thread launch. The prompt must stay consumed.
            val second=installer.createSession(PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                setAppPackageName(UpdatePolicy.PACKAGE)
            })
            try {
                record.writeText(JSONObject().put("status","installing").put("sessionId",second).put("nonce","synthetic-second")
                    .put("channel","dev").put("lastCheck",System.currentTimeMillis()).toString())
                val restarted=AndroidUpdates::class.java.getDeclaredConstructor(Context::class.java).apply { isAccessible=true }.newInstance(targetContext)
                restarted.recover()
                restarted.result(callback(PackageInstaller.STATUS_PENDING_USER_ACTION,"synthetic-second").putExtra(PackageInstaller.EXTRA_SESSION_ID,second))
                lateinit var flight:Job
                flight=launch { restarted.openConfirmation(second) { flight.cancel() } }
                flight.join()
                check(restarted.state.value.status=="awaiting_result" && restarted.confirmation==null)
                check(JSONObject(record.readText()).getString("status")=="awaiting_result")
                check(!restarted.openConfirmation(second) { error("Cancelled return reopened the accepted prompt") })
                restarted.cancelInstall()
            } finally { if(installer.getSessionInfo(second)!=null) installer.abandonSession(second) }
        } finally {
            if(installer.getSessionInfo(id)!=null) installer.abandonSession(id)
        }
    }
    private fun await(condition:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+10_000
        while(!condition()) { check(SystemClock.elapsedRealtime()<end) { "Update fixture timed out" };SystemClock.sleep(40) }
    }
    private fun find(root:AccessibilityNodeInfo?,text:String):AccessibilityNodeInfo? {
        root ?: return null
        root.refresh();if(root.text?.toString()==text) return root
        for(i in 0 until root.childCount) find(root.getChild(i),text)?.let { return it }
        return null
    }
    private fun button(text:String):AccessibilityNodeInfo {
        var node:AccessibilityNodeInfo?=null
        await { node=find(uiAutomation.rootInActiveWindow,text);node!=null }
        var action=checkNotNull(node)
        while(!action.isClickable && action.isEnabled && action.parent!=null) action=action.parent
        check(action.isClickable || !action.isEnabled) { "No action owner for $text" }
        return action
    }
    private fun screenshot(name:String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null),name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
            bitmap.recycle()
        }
    }
    private fun geometry() {
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val scene=mutableIntStateOf(0);val narrow=mutableStateOf(false);val clicks=AtomicInteger()
        val about=AtomicReference(Rect());val action=AtomicReference(Rect());val density=AtomicReference(1f)
        val raw="""{"schemaVersion":1,"product":"zerus","platform":"android","channel":"dev","versionName":"0.1.32-dev.300","versionCode":20000,"releaseId":42,"publishedAt":"2026-10-13T10:00:00Z","releaseUrl":"https://github.com/ufna/zerus/releases/tag/android-dev-fixture","sourceCommit":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","artifacts":[{"kind":"apk","name":"zerus.apk","url":"https://zerus.dev/downloads/android-dev-fixture/zerus.apk","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":12345,"os":"android","arch":"universal","minSdk":26,"packageName":"app.zerus.mobile","signingCertSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]}"""
        val update=UpdatePolicy.parse(raw)
        runOnMainSync {
            activity.setContent {
                MaterialTheme(colorScheme=darkColorScheme(primary=Color(0xFF67E8CB),background=Color(0xFF101517),onPrimary=Color(0xFF101517))) {
                    val original=LocalDensity.current;density.set(original.density)
                    CompositionLocalProvider(LocalDensity provides Density(original.density,if(narrow.value) 1.8f else 1f),
                        LocalContentColor provides MaterialTheme.colorScheme.onSurface) {
                        Column(Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).systemBarsPadding().padding(8.dp)) {
                            Column(if(narrow.value) Modifier.width(280.dp) else Modifier.fillMaxWidth()) {
                                val status=listOf("downloaded","downloaded","installing","confirmation","awaiting_result")[scene.intValue]
                                val state=UpdateState(status=status,available=update,downloaded=update,sessionId=if(scene.intValue>=2) 7 else -1)
                                UpdateProgressPanel(state,busy=scene.intValue==1,preparing=scene.intValue==1,
                                    confirmationAvailable=scene.intValue==3,installReason="",error="",onChannel={},onAction={ clicks.incrementAndGet() })
                                Text("About fixture",Modifier.onGloballyPositioned { coordinates ->
                                    val origin=coordinates.positionInWindow()
                                    about.set(Rect(origin.x.toInt(),origin.y.toInt(),origin.x.toInt()+coordinates.size.width,origin.y.toInt()+coordinates.size.height))
                                })
                            }
                        }
                    }
                }
            }
        }
        for(small in listOf(false,true)) {
            runOnMainSync { narrow.value=small;scene.intValue=0 }
            if(small) await {
                val measured=Rect().also { button("Install update").getBoundsInScreen(it) }
                kotlin.math.abs(measured.width()-280*density.get())<2
            }
            val baseline=Rect().also { button("Install update").getBoundsInScreen(it) }
            val originalAbout=Rect(about.get())
            val labels=listOf("Install update","Preparing installation…","Opening confirmation…","Open confirmation","Waiting for Android…")
            for(i in labels.indices) {
                runOnMainSync { scene.intValue=i }
                val node=button(labels[i]);val rect=Rect().also(node::getBoundsInScreen);action.set(rect)
                await { about.get()==originalAbout }
                check(rect==baseline) { "Update action moved at scene$i narrow=$small: $baseline/$rect" }
                if(i in listOf(1,2,4)) {
                    await { !button(labels[i]).isEnabled }
                    val disabled=button(labels[i])
                    check(!disabled.isEnabled) { "Pending update button stayed enabled at scene$i narrow=$small (${disabled.className}, clickable=${disabled.isClickable})" }
                    val before=clicks.get();disabled.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                    val now=SystemClock.uptimeMillis()
                    for(kind in listOf(MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP)) {
                        val event=MotionEvent.obtain(now,now+if(kind==MotionEvent.ACTION_UP) 30 else 0,kind,rect.exactCenterX(),rect.exactCenterY(),0)
                        try { uiAutomation.injectInputEvent(event,true) } finally { event.recycle() }
                    }
                    SystemClock.sleep(100)
                    check(clicks.get()==before) { "Disabled pending action repeated" }
                }
            }
            screenshot(if(small) "update-install-narrow.png" else "update-install-normal.png")
        }
        runOnMainSync { activity.finish() }
    }
}
