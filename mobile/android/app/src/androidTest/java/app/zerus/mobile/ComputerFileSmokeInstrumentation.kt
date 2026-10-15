package app.zerus.mobile

import android.app.Activity
import android.app.Application
import android.app.Instrumentation
import android.content.ClipboardManager
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Native Markdown taps, file grants and DocumentsUI save in an empty owned emulator. */
class ComputerFileSmokeInstrumentation : Instrumentation() {
    private var variant = "normal"
    override fun onCreate(arguments: Bundle?) { variant=arguments?.getString("variant") ?: "normal";super.onCreate(arguments);start() }
    override fun onStart() {
        val result=Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu","goldfish"))
            val store=PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState()==MessageState()) { "Use an empty owned emulator." }
            smoke()
            result.putString("stream","Computer file smoke passed ($variant): real Markdown tap, no read on disclosure/copy, capability failure, source switch, exact bytes/read grant, actual DocumentsUI save after target switch.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            screenshot("failure")
            result.putString("stream","Computer file smoke failed ($variant): ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun <T> main(action:()->T):T { val value=AtomicReference<Result<T>>();runOnMainSync { value.set(runCatching(action)) };return value.get().getOrThrow() }
    private fun await(label:String,predicate:()->Boolean) { val end=SystemClock.elapsedRealtime()+20_000;while(!predicate()) {if(SystemClock.elapsedRealtime()>=end) { dump(label);screenshot("timeout");error("Timed out: $label") };SystemClock.sleep(40)} }
    private fun find(label:String,node:AccessibilityNodeInfo?=uiAutomation.rootInActiveWindow):AccessibilityNodeInfo? {
        if(node==null)return null;node.refresh()
        for(i in 0 until node.childCount)find(label,node.getChild(i))?.let { return it }
        if(node.isVisibleToUser && (node.text?.toString()?.equals(label,ignoreCase=true)==true || node.contentDescription?.toString()==label))return node
        return null
    }
    private fun node(label:String):AccessibilityNodeInfo { var result:AccessibilityNodeInfo?=null;await(label){result=find(label);result!=null};return checkNotNull(result) }
    private fun dump(label:String) {
        val lines=mutableListOf("$label")
        fun visit(node:AccessibilityNodeInfo?) {if(node==null)return;node.refresh();lines+="${node.className} ${node.text} ${node.contentDescription} ${bounds(node)} clickable=${node.isClickable}";for(i in 0 until node.childCount)visit(node.getChild(i))}
        visit(uiAutomation.rootInActiveWindow)
        File(targetContext.getExternalFilesDir("file-link-smoke"),"$variant-tree.txt").also{it.parentFile?.mkdirs()}.writeText(lines.joinToString("\n"))
    }
    private fun bounds(node:AccessibilityNodeInfo)=Rect().also(node::getBoundsInScreen)
    private fun tap(label:String) {
        await("enabled $label") {find(label)?.isEnabled==true}
        SystemClock.sleep(150)
        val rectangle=bounds(node(label));check(!rectangle.isEmpty)
        // Selectable Text exposes its full line box, including blank space
        // after the link. Physically tap the first word's glyph area.
        val x=if(label=="Download spreadsheet")rectangle.left+12*targetContext.resources.displayMetrics.density else rectangle.exactCenterX()
        val now=SystemClock.uptimeMillis()
        for(action in listOf(MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP)) {
            val event=MotionEvent.obtain(now,SystemClock.uptimeMillis(),action,x,rectangle.exactCenterY(),0)
            event.source=android.view.InputDevice.SOURCE_TOUCHSCREEN
            try {check(uiAutomation.injectInputEvent(event,true))} finally {event.recycle()}
            if(action==MotionEvent.ACTION_DOWN)SystemClock.sleep(32)
        }
        SystemClock.sleep(150)
    }
    private fun screenshot(name:String) {
        val file=File(targetContext.getExternalFilesDir("file-link-smoke"),"$variant-$name.png");file.parentFile?.mkdirs()
        uiAutomation.takeScreenshot()?.let { image->file.outputStream().use { image.compress(Bitmap.CompressFormat.PNG,100,it) };image.recycle() }
    }
    private fun shell(command:String):ByteArray = uiAutomation.executeShellCommand(command).use { descriptor ->
        ParcelFileDescriptor.AutoCloseInputStream(descriptor).use { it.readBytes() }
    }
    private fun hash(data:ByteArray)=MessageDigest.getInstance("SHA-256").digest(data).joinToString(""){"%02x".format(it.toInt() and 255)}
    private fun smoke() {
        val application=targetContext.applicationContext as Application
        val scope=CoroutineScope(SupervisorJob()+Dispatchers.Main)
        val a=Target("fixture-a","codex/fixture/files","run-a","conversation-a","fixture-gateway")
        val b=a.copy(computerId="fixture-b",run="run-b",conversation="conversation-b")
        val reference=ComputerFileReference("/workspace/example/Пример_документов/Сводная итог-$variant.xlsx")
        val current=mutableStateOf(a)
        val access=AtomicReference(AttachmentAccess.Supported)
        val calls=AtomicInteger()
        val bytesA=ByteArray(700_021){(it%251).toByte()};val bytesB=ByteArray(700_021){((it+17)%251).toByte()}
        val opens=AtomicReference<Intent?>()
        val saves=AtomicReference<Intent?>()
        val monitor=object:ActivityMonitor() {
            override fun onStartActivity(intent:Intent):ActivityResult? {
                if(intent.action==Intent.ACTION_VIEW) {opens.set(Intent(intent));return ActivityResult(Activity.RESULT_OK,null)}
                if(intent.action==Intent.ACTION_CREATE_DOCUMENT)saves.set(Intent(intent))
                return null
            }
        }
        addMonitor(monitor)
        val client=OkHttpClient.Builder().addInterceptor { chain ->
            val request=chain.request();check(request.url.host=="relay.example.test")
            val input=JSONObject(okio.Buffer().also {request.body?.writeTo(it)}.readUtf8())
            check(input.string("operation")==ComputerFileProtocol.OPERATION)
            val payload=input.getJSONObject("payload")
            val target=if(input.string("computer_id")==a.computerId)a else b
            check(input.string("session")==target.session && payload.string("expected_run_id")==target.run)
            check(payload.string("expected_conversation_id")==target.conversation && payload.string("path")==reference.path)
            val data=if(target==a)bytesA else bytesB
            val offset=payload.getLong("offset").toInt();val end=minOf(data.size,offset+payload.getInt("length"))
            calls.incrementAndGet()
            val result=JSONObject().put("request_id",input.string("request_id")).put("name",target.session)
                .put("run_id",target.run).put("conversation_id",target.conversation).put("requested_path",reference.path)
                .put("path",reference.path).put("file_name",reference.name)
                .put("mime","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
                .put("bytes",data.size).put("sha256",hash(data)).put("offset",offset).put("length",end-offset)
                .put("data_base64",Base64.getEncoder().encodeToString(data.copyOfRange(offset,end)))
            val receipt=JSONObject().put("request_id",input.string("request_id")).put("state","completed").put("result",result)
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("Fixture")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val files=main {ComputerFiles(application,RelayApi(client),scope,
            {Connection(a.connectionId,"Fixture gateway","https://relay.example.test","fixture-token")},{access.get()})}
        val obscuration=main { ConversationObscuration() }
        main { files.clear() }
        await("fresh private download cache") { main { !files.clearing } }
        // executeShellCommand tokenizes arguments directly; it does not run a
        // shell or interpret quotes. Let find handle the Unicode/spaced name.
        shell("find /sdcard/Download -type f -name *-$variant.xlsx -delete")
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        try {
            main {activity.setContent {
                CompositionLocalProvider(LocalConversationObscuration provides obscuration) {
                MaterialTheme(colorScheme=darkColorScheme(primary=Color(0xFF67E8CB),background=Color(0xFF101517),surface=Color(0xFF1A2225))) {
                    Surface(Modifier.fillMaxSize()) {
                        Column(Modifier.fillMaxSize().systemBarsPadding().padding(16.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
                            Text("Synthetic computer file links")
                            val shown=current.value
                            MarkdownText("[Download spreadsheet](<${reference.path}>)",fileScope=shown.key) {
                                files.show(shown,it,if(shown==a)"fixture A" else "fixture B")
                            }
                        }
                        ComputerFileHost(files)
                    }
                }
                }
            }}
            tap("Download spreadsheet");node("On fixture A");check(calls.get()==0)
            check(main { obscuration.obscured }) { "File disclosure must suspend conversation read evidence" }
            screenshot("disclosure")
            tap("Copy file path")
            val clip=targetContext.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString()
            check(clip==reference.path && calls.get()==0)
            for(label in listOf("Open","Save…","Close")) {
                val rectangle=bounds(node(label));check(rectangle.left>=0 && rectangle.right<=targetContext.resources.displayMetrics.widthPixels)
            }
            tap("Close")
            access.set(AttachmentAccess.Unsupported);tap("Download spreadsheet");tap("Open")
            node(ComputerFileProtocol.UPDATE_REQUIRED);check(calls.get()==0);tap("Close")
            access.set(AttachmentAccess.Supported);main {current.value=b};SystemClock.sleep(400)
            tap("Download spreadsheet");node("On fixture B");tap("Open")
            await("external document intent"){opens.get()!=null}
            val opened=checkNotNull(opens.get());check(opened.type=="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet")
            check(opened.flags and Intent.FLAG_GRANT_READ_URI_PERMISSION!=0)
            val uri=checkNotNull(opened.data);check(uri.scheme=="content")
            val delivered=targetContext.contentResolver.openInputStream(uri)?.use {it.readBytes()}
            check(delivered!=null && delivered.contentEquals(bytesB)) {"Wrong computer's document was opened"}
            screenshot("downloaded");tap("Close")
            main {current.value=a};SystemClock.sleep(400);tap("Download spreadsheet");node("On fixture A");tap("Save…")
            await("native Android document picker"){saves.get()!=null && main {files.export?.launched==true}}
            val savedId=main {checkNotNull(files.export).id}
            check(saves.get()?.getStringExtra(Intent.EXTRA_TITLE)==reference.name)
            // Deliberately switch the app's current file while DocumentsUI owns
            // focus. Its eventual result must still export the original bytes A.
            main {current.value=b;files.show(b,reference,"fixture B")}
            if(find("Downloads")==null) {tap("Show roots");tap("Downloads")}
            screenshot("before-system-save")
            tap("Save")
            await("original save complete"){main {files.export==null}}
            val copied=shell("find /sdcard/Download -type f -name *-$variant.xlsx -exec cat {} ;")
            check(copied.contentEquals(bytesA)) {"The Android save result exported another computer's file"}
            main {files.finishExport(savedId,uri)}
            SystemClock.sleep(4000) // Let the native Saved toast leave the raster.
            node("On fixture B");screenshot("saved-original")
            check(calls.get()==4) {"Unexpected repeated reads: ${calls.get()}"}
            main { files.clear() }
            await("private copies removed") { main { !files.clearing && files.selection==null && files.export==null } }
            await("conversation no longer obscured") { main { !obscuration.obscured } }
            check(File(application.cacheDir,"computer-file-view").listFiles().orEmpty().isEmpty())
        } finally {
            removeMonitor(monitor);main {files.close();activity.finish()};scope.cancel()
            client.dispatcher.executorService.shutdown();client.connectionPool.evictAll()
        }
    }
}
