package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import java.io.File
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONArray
import org.json.JSONObject

/** Real client controls with synthetic native evidence and no configured connection or mutation transport. */
class OpenCodeClientSmokeInstrumentation : Instrumentation() {
    private var variant="normal"
    override fun onCreate(arguments:Bundle?) { variant=arguments?.getString("variant") ?: "normal";super.onCreate(arguments);start() }
    override fun onStart() {
        val result=Bundle()
        try {
            require(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu","goldfish"))
            require(PrivateStore(targetContext).connections().isEmpty()) { "Use an owned fixture with no configured connection." }
            smoke()
            result.putString("stream","OpenCode client smoke passed ($variant): native model/account chooser, exact IDs, disabled creation physical tap without delivery, Terminal native reason, disabled Attach physical tap without picker, retained voice control and draft. No native or relay mutation.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            result.putString("stream","OpenCode client smoke failed ($variant): ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun <T> main(action:()->T):T {
        val result=AtomicReference<Result<T>>();runOnMainSync { result.set(runCatching(action)) };return result.get().getOrThrow()
    }
    private fun await(description:String,condition:()->Boolean) {
        val deadline=SystemClock.elapsedRealtime()+15_000
        while(!condition()) { check(SystemClock.elapsedRealtime()<deadline) { "Timed out: $description" };SystemClock.sleep(40) }
    }
    private fun find(label:String,root:AccessibilityNodeInfo?=uiAutomation.rootInActiveWindow):AccessibilityNodeInfo? {
        if(root==null)return null
        root.refresh()
        if(root.isVisibleToUser && (root.text?.toString()==label || root.contentDescription?.toString()==label))return root
        for(index in 0 until root.childCount)find(label,root.getChild(index))?.let { return it }
        return null
    }
    private fun node(label:String):AccessibilityNodeInfo { var value:AccessibilityNodeInfo?=null;await(label) { value=find(label);value!=null };return checkNotNull(value) }
    private fun owner(label:String):AccessibilityNodeInfo { var value=node(label);while(!value.isClickable && value.parent!=null)value=value.parent;return value }
    private fun tap(label:String) { check(owner(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Cannot activate $label" } }
    private fun physicalTap(value:AccessibilityNodeInfo) {
        val bounds=Rect().also(value::getBoundsInScreen);check(!bounds.isEmpty);val at=SystemClock.uptimeMillis()
        val down=MotionEvent.obtain(at,at,MotionEvent.ACTION_DOWN,bounds.exactCenterX(),bounds.exactCenterY(),0)
        val up=MotionEvent.obtain(at,at+80,MotionEvent.ACTION_UP,bounds.exactCenterX(),bounds.exactCenterY(),0)
        try { check(uiAutomation.injectInputEvent(down,true));check(uiAutomation.injectInputEvent(up,true)) } finally { down.recycle();up.recycle() }
        waitForIdleSync()
    }
    private fun set(model:ZerusViewModel,name:String,value:Any) {
        model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value)
    }
    private fun screenshot(name:String) {
        waitForIdleSync();SystemClock.sleep(350)
        val file=File(targetContext.getExternalFilesDir("opencode-smoke"),"$variant-$name.png");file.parentFile?.mkdirs()
        checkNotNull(uiAutomation.takeScreenshot()).let { bitmap -> file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) };bitmap.recycle() }
    }
    private fun scrollTo(label:String) {
        fun scroll(root:AccessibilityNodeInfo?):AccessibilityNodeInfo? {
            if(root==null)return null;root.refresh();if(root.isScrollable && root.isVisibleToUser)return root
            for(index in 0 until root.childCount)scroll(root.getChild(index))?.let { return it };return null
        }
        repeat(20) {
            if(find(label)!=null)return
            check(scroll(uiAutomation.rootInActiveWindow)?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)==true) { "Cannot scroll to $label" }
            SystemClock.sleep(150)
        }
        node(label)
    }
    private fun smoke() {
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
        await("private storage") { main { model.storageReady } }
        val target=Target("11111111-1111-4111-8111-111111111111","opencode/fixture/local","native-run","native-session","fixture")
        val raw=JSONObject().put("name",target.session).put("cmd","opencode").put("agent","opencode").put("state","running")
            .put("run_id",target.run).put("conversation_id",target.conversation).put("tracked",true)
            .put("activity","idle").put("phase","idle").put("runtime_state","live").put("process_state","running")
            .put("terminal_supported",false).put("terminal_reason","OpenCode runs through its native server. Use the conversation instead of Terminal.")
            .put("attachments_supported",false).put("model","ollama/qwen3:8b").put("settings_model","ollama/qwen3:8b")
            .put("allowed_actions",JSONArray(listOf("pause","rename")))
        val session=Session(target,"OpenCode fixture","opencode","idle","/fixture/project","",0,raw)
        main {
            check(model.connections.isEmpty())
            set(model,"setMachines",listOf(Machine(target.connectionId,target.computerId,"Fixture computer",true)))
            set(model,"setSessions",listOf(session));set(model,"setActivity",raw);set(model,"setActivityVerified",true);set(model,"setSelected",session)
            // Reset only this runner's exact synthetic draft between its two owned configurations.
            val draft=model.draft(target)
            require(draft.text.isBlank() || draft.text=="Retained OpenCode draft") { "The fixture target already has unrelated text." }
            if(draft.text.isNotBlank()) model.discard(draft)
        }
        node("Attach files");node("Voice input")
        var attach=node("Attach files")
        while(attach.isEnabled && attach.parent!=null)attach=attach.parent
        check(!attach.isEnabled) { "OpenCode Attach is enabled" }
        physicalTap(attach)
        check(main { model.pickerDraft==null && model.pickerId.isBlank() && model.outgoing.isEmpty() })
        tap("Terminal");node("Terminal unavailable");node(raw.getString("terminal_reason"));screenshot("terminal-reason");tap("Close");await("Terminal explanation dismissed") { find("Terminal unavailable")==null };SystemClock.sleep(350);waitForIdleSync()
        node("Voice input");screenshot("conversation")
        main { model.edit(target,"Retained OpenCode draft") }
        await("draft") { main { model.draft(target).text=="Retained OpenCode draft" } }
        val catalog=JSONObject().put("agents",JSONArray(listOf("opencode"))).put("project_launch_supported",false)
            .put("accounts",JSONArray().put(JSONObject().put("id","native-opencode").put("provider","opencode").put("label","Native configuration")
                .put("native",true).put("installed",true).put("is_default",true)
                .put("model_options",JSONArray().put(JSONObject().put("value","ollama/qwen3:8b").put("label","Qwen 3"))
                    .put(JSONObject().put("value","ollama/qwen3:14b").put("label","Qwen 3 large")))))
        main {
            set(model,"setLaunchMachine",MachineKey(target.connectionId,target.computerId));set(model,"setLaunchCatalogState",catalog)
            model.javaClass.getDeclaredField("launchCatalogEvidence").apply { isAccessible=true }.set(model,"synthetic-opencode-catalog")
            set(model,"setLaunchGeneratedName","work-fixture");set(model,"setLaunchVisible",true)
        }
        node("New session");scrollTo("Model, Native default model");tap("Model, Native default model")
        node("Qwen 3");node("ollama/qwen3:8b");screenshot("model-choices");tap("Qwen 3")
        node("Model, Qwen 3, ollama/qwen3:8b")
        val create=owner("Create session");check(!create.isEnabled) { "Creation is enabled without a connection" }
        physicalTap(create);check(main { model.sessionActions.isEmpty() && model.outgoing.isEmpty() && model.draft(target).text=="Retained OpenCode draft" })
        screenshot("launch-selected")
        main { set(model,"setLaunchVisible",false) }
        activity.finish()
    }
}
