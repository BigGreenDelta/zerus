package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import java.io.File
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject

/** Full Sessions chooser with synthetic data on an isolated emulator; no transport. */
class MachineFilterSmokeInstrumentation : Instrumentation() {
    private var prefix="android"
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments);prefix=arguments?.getString("screenshot_prefix") ?: prefix;start()
    }
    override fun onStart() {
        val result=Bundle()
        try {
            check(BuildConfig.DEBUG);smoke()
            result.putString("stream","Machine filter passed: per-machine counts, zero machines, workspace identities, selection-independent totals, live inventory/state updates, search independence and visible 48dp rows.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            screenshot("failure")
            result.putString("stream","Machine filter failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun <T> main(action:()->T):T {
        val result=AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) };return result.get().getOrThrow()
    }
    private fun await(condition:()->Boolean) {
        val deadline=SystemClock.elapsedRealtime()+10_000
        while(!condition()) { check(SystemClock.elapsedRealtime()<deadline) { "Machine chooser timed out" };SystemClock.sleep(40) }
    }
    private fun find(root:AccessibilityNodeInfo?,label:String):AccessibilityNodeInfo? {
        if(root==null)return null
        root.refresh()
        if(root.text?.toString()==label || root.contentDescription?.toString()==label)return root
        for(index in 0 until root.childCount)find(root.getChild(index),label)?.let { return it }
        return null
    }
    private fun node(label:String):AccessibilityNodeInfo {
        var result:AccessibilityNodeInfo?=null
        await { result=find(uiAutomation.rootInActiveWindow,label);result!=null };return checkNotNull(result)
    }
    private fun button(label:String):AccessibilityNodeInfo {
        var result=node(label)
        while(!result.isClickable && result.parent!=null)result=result.parent
        check(result.isClickable) { "No clickable owner for $label" };return result
    }
    private fun scrollRight(root:AccessibilityNodeInfo?):Boolean {
        if(root==null)return false
        val action=AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT.id
        if(root.actionList.any { it.id==action } && root.performAction(action))return true
        if(root.className?.toString()?.endsWith("HorizontalScrollView")==true &&
            root.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD))return true
        return (0 until root.childCount).any { scrollRight(root.getChild(it)) }
    }
    private fun tap(label:String) { check(button(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) }
    private fun bounds(node:AccessibilityNodeInfo)=Rect().also(node::getBoundsInScreen)
    private fun set(model:ZerusViewModel,name:String,value:Any) {
        model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value)
    }
    private fun screenshot(suffix:String) {
        uiAutomation.waitForIdle(100,3_000)
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null),"machine-counts-$prefix-$suffix.png").outputStream().use {
                bitmap.compress(Bitmap.CompressFormat.PNG,100,it)
            };bitmap.recycle()
        }
    }
    private fun smoke() {
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        if(prefix=="android-large") {
            check(activity.resources.configuration.fontScale>=1.7f) { "Large-text fixture did not scale the Android window" }
            check(activity.resources.configuration.screenWidthDp<=280) { "Narrow fixture exceeds 280dp" }
        }
        val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
        await { main { model.storageReady } }
        val hosts=listOf(Machine("fixture-a","same","Fixture workstation with a long name",true),
            Machine("fixture-b","same","Fixture laptop",false),Machine("fixture-a","empty","Empty fixture",true))
        fun session(index:Int,machine:Machine,busy:Boolean)=Session(
            Target(machine.id,"codex/fixture/session-$index","run-$index","conversation-$index",machine.connectionId),
            "Synthetic chooser fixture $index","codex","","Fixture","",0,
            JSONObject().put("state","running").put("activity",if(busy) "busy" else "idle").put("phase",if(busy) "tool" else "idle"))
        val sessions=listOf(session(1,hosts[0],true),session(2,hosts[0],false),session(3,hosts[1],false))
        main {
            check(model.connections.isEmpty()) { "Use an isolated emulator with no connections" }
            model.preview();set(model,"setMachines",hosts);set(model,"setSessions",sessions);set(model,"setProjects",emptyList<ProjectSummary>())
            model.chooseSessionFilter(SessionFilter.All);model.allMachines()
        }
        node("All sessions, 3 sessions, selected")
        if(find(uiAutomation.rootInActiveWindow,"Filter machines")==null)
            check(scrollRight(uiAutomation.rootInActiveWindow)) { "Cannot reveal the machine filter in its scrolling toolbar" }
        tap("Filter machines");node("All machines (3)")
        val screen=checkNotNull(uiAutomation.takeScreenshot()).let { bitmap -> Rect(0,0,bitmap.width,bitmap.height).also { bitmap.recycle() } }
        for((index,count) in listOf(0 to 2,1 to 1,2 to 0)) {
            val row=button("${hosts[index].name}, $count sessions")
            val number=checkNotNull(find(row,count.toString())) { "Missing trailing count" }
            check(number.isVisibleToUser && screen.contains(bounds(number)) && bounds(row).contains(bounds(number))) { "Count is clipped" }
            check(bounds(row).height()>=48*targetContext.resources.displayMetrics.density-1) { "Machine target is too small" }
        }
        screenshot("all")
        tap("Fixture laptop, 1 sessions")
        await { main { model.selectedMachines==setOf(MachineKey("fixture-b","same")) } }
        node("${hosts[0].name}, 2 sessions");node("All machines (3)");node("Empty fixture, 0 sessions")
        main { set(model,"setSessions",sessions.filterNot { it.target==sessions[1].target }) }
        node("${hosts[0].name}, 1 sessions");node("All machines (2)")
        main { model.chooseSessionFilter(SessionFilter.Active) }
        node("Active before search");node("All machines (1)");node("Fixture laptop, 0 sessions")
        main { model.changeSessionQuery("unmatched fixture query") }
        node("All machines (1)");node("${hosts[0].name}, 1 sessions")
        tap("All machines (1)");await { main { model.selectedMachines.isEmpty() } }
        tap("Empty fixture, 0 sessions");await { main { model.selectedMachines==setOf(MachineKey("fixture-a","empty")) } }
        node("${hosts[0].name}, 1 sessions");tap("Done")
        await { find(uiAutomation.rootInActiveWindow,"Active before search")==null }
        main { activity.finish() }
    }
}
