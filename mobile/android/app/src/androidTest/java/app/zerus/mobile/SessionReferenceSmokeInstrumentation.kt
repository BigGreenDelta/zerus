package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.ClipboardManager
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import java.io.File
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject

/** Synthetic header/clipboard fixtures on an owned emulator; no connection or native agent operation. */
class SessionReferenceSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result=Bundle()
        try {
            check(BuildConfig.DEBUG); smoke()
            result.putString("stream","Session reference smoke passed: compact machine/status header, header details, menu-only search, busy parent and child menu copying, exact native machine/host, missing-identity explanation, narrow large-font header/menu.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error: Throwable) {
            result.putString("stream","Session reference smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result=AtomicReference<Result<T>>();runOnMainSync { result.set(runCatching(action)) };return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val deadline=SystemClock.elapsedRealtime()+10_000
        while(!condition()) { check(SystemClock.elapsedRealtime()<deadline) { "Timed out: $description" };SystemClock.sleep(40) }
    }
    private fun find(label: String, root: AccessibilityNodeInfo?=uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        if(root==null)return null
        root.refresh()
        if(root.text?.toString()?.split('\n')?.contains(label)==true || root.contentDescription?.toString()==label ||
            root.actionList.any { it.id==AccessibilityNodeInfo.ACTION_CLICK && it.label?.toString()==label })return root
        for(index in 0 until root.childCount)find(label,root.getChild(index))?.let { return it }
        return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo?=null
        await(label) { found=find(label);found!=null };return checkNotNull(found)
    }
    private fun action(label: String): AccessibilityNodeInfo {
        var found=node(label)
        while(!found.isClickable && found.parent!=null)found=found.parent
        return found
    }
    private fun tap(label: String) { check(action(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Cannot activate $label" } }
    private fun set(model: ZerusViewModel,name: String,value: Any) {
        model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value)
    }
    private fun screenshot(name: String) {
        val file=File(targetContext.getExternalFilesDir("reference-smoke"),"$name.png")
        file.parentFile?.mkdirs();checkNotNull(uiAutomation.takeScreenshot()).let { bitmap ->
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) };bitmap.recycle()
        }
    }
    private fun smoke() {
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
        await("private storage") { main { model.storageReady } }
        val target=Target("11111111-1111-4111-8111-111111111111","claude/fixture/working","run","parent","fixture")
        val machine=Machine(target.connectionId,target.computerId,"Renamed office computer",true,
            machineId="22222222-2222-4222-8222-222222222222",nativeHost="fixture-host")
        val raw=JSONObject().put("state","running").put("activity","busy").put("name",target.session)
            .put("run_id",target.run).put("conversation_id",target.conversation)
        val session=mutableStateOf(Session(target,"Working fixture","Claude","busy","","",0,raw))
        val narrow=mutableStateOf(false)
        val details=mutableStateOf(false)
        val clipboard=targetContext.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        fun clip()=main { clipboard.primaryClip?.getItemAt(0)?.text?.toString() }
        main {
            check(model.connections.isEmpty()) { "Use an isolated fixture with no configured connections" }
            set(model,"setMachines",listOf(machine));set(model,"setActivity",raw)
            activity.setContent {
                MaterialTheme {
                    val density=LocalDensity.current
                    key(narrow.value) {
                        CompositionLocalProvider(LocalDensity provides Density(density.density,if(narrow.value)1.8f else 1f)) {
                            Surface { Column(Modifier.width(if(narrow.value)280.dp else 390.dp)) {
                                SessionHeaderTitle(model,session.value) { details.value=true }
                                SessionHeaderTools(model,session.value,onDetails={ details.value=true }) {}
                            } }
                            if(details.value) SessionDetailsSheet(model,session.value) { details.value=false }
                        }
                    }
                }
            }
        }
        node("Session details")
        SystemClock.sleep(250)
        screenshot("working-parent-header")
        tap("Session details");node("Overview")
        sendKeyDownUpSync(KeyEvent.KEYCODE_BACK)
        await("closed details") { find("Overview")==null }
        check(find("Search loaded messages")==null) { "Search still has a toolbar button" }
        node("Session actions");check(find("Copy session reference")==null) { "Toolbar still contains Copy" }
        tap("Session actions");node("Search loaded messages");check(action("Copy session reference").isEnabled)
        screenshot("working-parent-menu")
        tap("Copy session reference")
        val expected=checkNotNull(SessionReference.resolve(target,listOf(machine)).uri)
        await("parent clipboard") { clip()==expected }
        check(expected.contains("machine=${machine.machineId}&host=fixture-host"))
        main { set(model,"setMachines",listOf(machine.copy(machineId=""))) }
        tap("Session actions")
        node("This computer has not reported its Zerus identity. Refresh its connection.")
        screenshot("missing-identity-menu")
        // Text keeps its own enabled flag; the menu item's ancestor owns disabled semantics.
        val unavailable=generateSequence(node("Copy session reference")) { it.parent }.firstOrNull { !it.isEnabled }
        check(unavailable!=null) { "Missing native identity remained copyable" }
        main { clipboard.setPrimaryClip(ClipData.newPlainText("fixture","keep clipboard")) }
        val bounds=Rect().also(unavailable::getBoundsInScreen)
        val down=SystemClock.uptimeMillis()
        sendPointerSync(MotionEvent.obtain(down,down,MotionEvent.ACTION_DOWN,bounds.exactCenterX(),bounds.exactCenterY(),0))
        sendPointerSync(MotionEvent.obtain(down,down+80,MotionEvent.ACTION_UP,bounds.exactCenterX(),bounds.exactCenterY(),0))
        SystemClock.sleep(150)
        check(clip()=="keep clipboard" && find("Copy session name")!=null) { "Disabled copy changed the clipboard or dismissed its menu" }
        sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);await("closed menu") { find("Copy session name")==null }
        check(clip()=="keep clipboard")
        val child=target.copy(conversation="",agentId="child",parentConversation="parent")
        main { set(model,"setMachines",listOf(machine));session.value=session.value.copy(target=child);narrow.value=true }
        await("narrow large-font header") {
            val header=find("Session details") ?: return@await false
            val frame=Rect().also(header::getBoundsInScreen)
            frame.width() <= 281 * targetContext.resources.displayMetrics.density && frame.height() >= 60 * targetContext.resources.displayMetrics.density
        }
        screenshot("child-narrow-header")
        node("Session actions");check(find("Copy session reference")==null)
        tap("Session actions");check(action("Copy session reference").isEnabled)
        check(find("Pause")==null && find("Review later on this phone")==null) { "Parent operations leaked into child menu" }
        screenshot("child-narrow-menu")
        tap("Copy session reference")
        val childReference=SessionReference.resolve(child,listOf(machine)).uri
        await("child clipboard") { clip()==childReference }
        main { activity.finish() }
    }
}
