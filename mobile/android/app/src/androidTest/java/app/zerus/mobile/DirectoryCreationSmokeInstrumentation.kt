package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowInsets
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject

/** Synthetic transport and real native controls; only an owned empty emulator is allowed. */
class DirectoryCreationSmokeInstrumentation : Instrumentation() {
    private var variant="normal"
    override fun onCreate(arguments:Bundle?) { variant=arguments?.getString("variant") ?: "normal";super.onCreate(arguments);start() }
    override fun onStart() {
        val result=Bundle()
        try {
            require(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu","goldfish"))
            require(PrivateStore(targetContext).connections().isEmpty() && PrivateStore(targetContext).messageState()==MessageState()) { "Use an empty owned emulator." }
            smoke()
            result.putString("stream","Directory creation smoke passed ($variant): browse/cancel without effects, literal typed path creation, durable-before-transport identity, physical duplicate guard, confirmed choose without launch, missing capability, uncertain receipt-only recovery, stale form suppression, existing manual-worktree post-create browse.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) { result.putString("stream","Directory creation smoke failed ($variant): ${error.stackTraceToString()}\n");finish(Activity.RESULT_CANCELED,result) }
    }
    private fun <T> main(action:()->T):T { val r=AtomicReference<Result<T>>();runOnMainSync { r.set(runCatching(action)) };return r.get().getOrThrow() }
    private fun await(description:String,condition:()->Boolean) { val end=SystemClock.elapsedRealtime()+15_000;while(!condition()) { check(SystemClock.elapsedRealtime()<end) { "Timed out: $description" };SystemClock.sleep(40) } }
    private fun find(label:String,root:AccessibilityNodeInfo?=uiAutomation.rootInActiveWindow):AccessibilityNodeInfo? {
        if(root==null)return null;root.refresh()
        if(root.isVisibleToUser && (root.text?.toString()==label || root.contentDescription?.toString()==label))return root
        for(i in 0 until root.childCount)find(label,root.getChild(i))?.let { return it };return null
    }
    private fun node(label:String):AccessibilityNodeInfo { var n:AccessibilityNodeInfo?=null;await(label) { n=find(label);n!=null };return checkNotNull(n) }
    private fun actionNode(label:String,root:AccessibilityNodeInfo?=uiAutomation.rootInActiveWindow):AccessibilityNodeInfo? {
        if(root==null)return null;root.refresh()
        if(root.isVisibleToUser && (root.text?.toString()==label || root.contentDescription?.toString()==label)) {
            var n:AccessibilityNodeInfo=root
            while(!n.isClickable && n.className?.toString()!="android.widget.Button" && n.parent!=null) { n=checkNotNull(n.parent);n.refresh() }
            if(n.isClickable || n.className?.toString()=="android.widget.Button")return n
        }
        for(i in 0 until root.childCount)actionNode(label,root.getChild(i))?.let { return it };return null
    }
    private fun button(label:String):AccessibilityNodeInfo { var n:AccessibilityNodeInfo?=null;await("action $label") { n=actionNode(label);n!=null };return checkNotNull(n) }
    private fun scrollTo(label:String,action:Boolean=true) {
        fun scroll(root:AccessibilityNodeInfo?):AccessibilityNodeInfo? {
            if(root==null)return null;root.refresh();if(root.isScrollable && root.isVisibleToUser && root.actionList.any { it.id in setOf(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD,AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD) })return root
            for(i in 0 until root.childCount)scroll(root.getChild(i))?.let { return it };return null
        }
        var direction=AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
        repeat(100) {
            val n=if(action) actionNode(label) else find(label)
            if(n!=null) { val r=Rect().also(n::getBoundsInScreen);if(!action || r.height()>=48*targetContext.resources.displayMetrics.density-1)return }
            val owner=scroll(uiAutomation.rootInActiveWindow)
            if(owner!=null && !owner.performAction(direction)) direction=if(direction==AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD else AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
            SystemClock.sleep(150)
        }
        error("Cannot expose $label by scrolling")
    }
    private fun tap(label:String) { scrollTo(label);check(button(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Cannot activate $label" } }
    private fun physical(label:String,enabled:Boolean=true) {
        scrollTo(label);SystemClock.sleep(200);val n=button(label);check(n.isEnabled==enabled);val r=Rect().also(n::getBoundsInScreen);val d=targetContext.resources.displayMetrics
        check(r.width()>=48*d.density-1 && r.height()>=48*d.density-1 && r.left>=0 && r.right<=d.widthPixels && r.top>=0 && r.bottom<=d.heightPixels) { "Invalid action bounds $label: $r" }
        val at=SystemClock.uptimeMillis()
        for(a in listOf(MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP)) MotionEvent.obtain(at,SystemClock.uptimeMillis(),a,r.exactCenterX(),r.exactCenterY(),0).let { e -> try { check(uiAutomation.injectInputEvent(e,true)) } finally { e.recycle() } }
    }
    private fun set(model:ZerusViewModel,name:String,value:Any) { model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value) }
    private fun field(value:String) {
        scrollTo("Absolute folder path",false)
        fun editable(n:AccessibilityNodeInfo?):AccessibilityNodeInfo? { if(n==null)return null;n.refresh();if(n.isEditable && n.isVisibleToUser)return n;for(i in 0 until n.childCount)editable(n.getChild(i))?.let { return it };return null }
        val n=checkNotNull(editable(uiAutomation.rootInActiveWindow));check(n.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT,Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,value) }))
        waitForIdleSync()
    }
    private fun trace(label:String) { File(targetContext.getExternalFilesDir("directory-smoke"),"$variant-trace.txt").also { it.parentFile?.mkdirs() }.appendText("${SystemClock.elapsedRealtime()} $label\n") }
    private fun shot(name:String) { SystemClock.sleep(300);val f=File(targetContext.getExternalFilesDir("directory-smoke"),"$variant-$name.png");f.parentFile?.mkdirs();checkNotNull(uiAutomation.takeScreenshot()).let { b -> f.outputStream().use { b.compress(Bitmap.CompressFormat.PNG,100,it) };b.recycle() } }
    private fun smoke() {
        val machine=MachineKey("fixture-workspace","11111111-1111-4111-8111-111111111111")
        val creates=AtomicInteger();val worktrees=AtomicInteger();val reads=AtomicInteger();val mode=AtomicReference("success");val gate=AtomicReference<CountDownLatch?>(null)
        val saved=AtomicReference<JSONObject?>(null);val requested=AtomicReference("");val created=mutableSetOf<String>()
        val store=PrivateStore(targetContext)
        fun response(request:okhttp3.Request,json:JSONObject)=Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("Fixture")
            .body(json.toString().toResponseBody("application/json".toMediaType())).build()
        val client=OkHttpClient.Builder().addInterceptor { chain ->
            val request=chain.request();check(request.url.host=="relay.example.test")
            if(request.method=="GET") {
                reads.incrementAndGet();val receipt=checkNotNull(saved.get());check(request.url.encodedPath=="/v1/requests/${receipt.string("request_id")}")
                return@addInterceptor response(request,receipt)
            }
            val input=JSONObject(okio.Buffer().also { request.body?.writeTo(it) }.readUtf8());val payload=input.getJSONObject("payload");val id=input.getString("request_id")
            check(input.string("computer_id")==machine.computerId && input.string("session").isBlank())
            val result=when(input.string("operation")) {
                "dirs" -> {
                    val path=payload.getString("path").let { if(it=="~") "/fixture" else it }
                    if(path!="/fixture" && path !in created) return@addInterceptor response(request,JSONObject().put("request_id",id).put("state","failed").put("error","No such folder"))
                    JSONObject().put("path",path).put("parent",path.substringBeforeLast('/').ifBlank { "/" }).put("home","/fixture").put("directories",JSONArray())
                }
                "worktrees" -> JSONObject().put("state","ok").put("stale",false).put("path",payload.string("path"))
                    .put("common_dir","/fixture/.git").put("worktrees",JSONArray().put(JSONObject().put("path",payload.string("path")).put("kind","linked").put("available",true)))
                "worktree_create" -> {
                    worktrees.incrementAndGet();created+=payload.getString("destination")
                    JSONObject().put("request_id",id).put("status","created").put("common_dir",payload.getString("common_dir"))
                        .put("branch",payload.getString("branch")).put("path",payload.getString("destination"))
                }
                "directory_create" -> {
                    // PrivateStore decodes persisted sending as uncertain on recovery; presence proves pre-transport durability.
                    val durable=store.messageState().sessionActions.single { it.requestId==id }
                    check(DirectoryCreation.computerTarget(durable,machine) && durable.status=="uncertain" && JSONObject(durable.arguments).getString("path")==payload.getString("path")) { "Saved computer-scoped request/arguments missing before transport" }
                    check(payload.keys().asSequence().toSet()==setOf("request_id","path"));creates.incrementAndGet();requested.set(payload.getString("path"))
                    val r=JSONObject().put("status","created").put("request_id",id).put("requested_path",payload.getString("path")).put("path",payload.getString("path")).put("parent","/fixture")
                    saved.set(JSONObject().put("request_id",id).put("state","completed").put("result",r));created+=payload.getString("path")
                    gate.get()?.let { if(!it.await(25,TimeUnit.SECONDS)) throw java.io.IOException("Synthetic pending gate timed out") }
                    if(mode.get()=="uncertain") throw java.io.IOException("Synthetic lost response after creation")
                    r
                }
                else -> error("Unexpected operation ${input.string("operation")}; no launch is allowed")
            }
            response(request,JSONObject().put("request_id",id).put("state","completed").put("result",result))
        }.build()
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
        await("private storage") { main { model.storageReady } }
        val chosen=AtomicReference("")
        main {
            model.javaClass.getDeclaredField("api").apply { isAccessible=true }.set(model,RelayApi(client))
            set(model,"setConnections",listOf(Connection(machine.connectionId,"Fixture workspace","https://relay.example.test","fixture-token")))
            set(model,"setMachines",listOf(Machine(machine.connectionId,machine.computerId,"Fixture computer",true)))
            val operations=setOf("catalog","dirs","launch","directory_create")
            set(model,"setRelayOperations",mapOf(machine.connectionId to operations));set(model,"setMachineOperations",mapOf(machine to operations))
            set(model,"setLaunchMachine",machine);set(model,"setLaunchVisible",true)
            activity.setContent { MaterialTheme { Surface { LaunchFolderDialog(model,machine,{ chosen.set(it) },{}) } } }
            model.browseLaunchDirectory(machine,"/fixture")
        }
        await("folder browser ready") { main { model.launchDirectoryState?.string("path")=="/fixture" && !model.launchDirectoryLoading } }
        physical("New folder");node("New folder path");shot("create-dialog");tap("Cancel")
        await("canceled without effect") { find("New folder path")==null };check(creates.get()==0 && store.messageState().sessionActions.isEmpty())
        val path="/fixture/New project ; $ literal"
        field(path);tap("Browse");await("missing typed path") { main { !model.launchDirectoryLoading && model.launchError=="No such folder" } }
        physical("Create this path");node("New folder path");val hold=CountDownLatch(1);gate.set(hold)
        try {
            trace("before Create tap");physical("Create folder");trace("after Create tap");await("one saved mutation: ${main { model.launchError }}") { creates.get()==1 };trace("mutation present");await("disabled pending") { !button("Creating folder…").isEnabled }
            trace("before disabled taps");physical("Creating folder…",false);trace("first disabled tap done");physical("Cancel",false);trace("Cancel disabled tap done");check(creates.get()==1);shot("pending")
        } finally { trace("release pending gate");hold.countDown();gate.set(null) }
        await("confirmed folder browse") { main { model.launchDirectoryState?.string("path")==path && !model.launchDirectoryLoading } }
        node("Use this folder");physical("Use this folder");await("explicit selected path") { chosen.get()==path };check(creates.get()==1 && requested.get()==path)
        shot("confirmed")
        main { set(model,"setMachineOperations",mapOf(machine to setOf("catalog","dirs","launch"))) }
        await("missing capability") { !button("New folder").isEnabled };physical("New folder",false);check(creates.get()==1)
        main { set(model,"setMachineOperations",mapOf(machine to setOf("catalog","dirs","launch","directory_create"))) }
        mode.set("uncertain");main { model.createLaunchDirectory(machine,"/fixture/Unknown result") }
        await("unknown durable outcome") { main { !model.directoryCreateSubmitting && model.sessionActions.any { it.status=="uncertain" && JSONObject(it.arguments).string("path")=="/fixture/Unknown result" } } }
        val unknown=main { model.sessionActions.single { JSONObject(it.arguments).string("path")=="/fixture/Unknown result" } };check(creates.get()==2)
        main { model.createLaunchDirectory(machine,"/fixture/duplicate") };SystemClock.sleep(200);check(creates.get()==2)
        scrollTo("Check original folder result");physical("Check original folder result")
        await("receipt recovery") { main { model.sessionActions.single { it.requestId==unknown.requestId }.status=="completed" && model.launchDirectoryState?.string("path")=="/fixture/Unknown result" && !model.launchDirectoryLoading } }
        check(creates.get()==2 && reads.get()==1 && main { model.sessionActions.single { it.requestId==unknown.requestId }.resultPath=="/fixture/Unknown result" });shot("recovered")
        // Exercise the production New session -> Browse -> Use callback too, without starting an agent.
        main {
            val catalog=JSONObject().put("agents",JSONArray(listOf("codex"))).put("project_launch_supported",true)
                .put("swarm_id","fixture-swarm").put("default_project","fixture-project").put("projects",JSONArray().put(JSONObject()
                    .put("id","fixture-project").put("name","New project").put("folders",JSONArray())))
                .put("accounts",JSONArray().put(JSONObject().put("id","native-codex").put("provider","codex").put("label","Native configuration")
                    .put("native",true).put("installed",true).put("is_default",true)))
            set(model,"setLaunchCatalogState",catalog);set(model,"setLaunchGeneratedName","work-fixture");set(model,"setLaunchFormId",42L)
            model.javaClass.getDeclaredField("launchCatalogEvidence").apply { isAccessible=true }.set(model,catalog.toString())
            activity.setContent { MaterialTheme { Surface { LaunchSessionDialog(model) } } }
        }
        physical("Browse folders");node("Use this folder");physical("Use this folder")
        await("New session receives confirmed path") { find("Choose folder on Fixture computer")==null && button("Create session").isEnabled }
        check(main { model.launchDirectoryState?.string("path")=="/fixture/Unknown result" });check(creates.get()==2);shot("launch-selected")
        main { activity.setContent { MaterialTheme { Surface { LaunchFolderDialog(model,machine,{ chosen.set(it) },{}) } } } }
        node("Use this folder")
        mode.set("success");val stale=CountDownLatch(1);gate.set(stale)
        main { model.createLaunchDirectory(machine,"/fixture/Stale form") };await("stale pending") { creates.get()==3 }
        main { model.javaClass.getDeclaredField("launchGeneration").apply { isAccessible=true }.setLong(model,999);set(model,"setLaunchMachine",machine.copy(computerId="other"));set(model,"setLaunchCreatedDirectory","") }
        stale.countDown();gate.set(null);await("stale settled") { main { !model.directoryCreateSubmitting && model.sessionActions.any { it.status=="completed" && JSONObject(it.arguments).string("path")=="/fixture/Stale form" } } }
        check(main { model.launchCreatedDirectory.isBlank() && model.launchDirectoryState?.string("path")=="/fixture/Unknown result" });check(creates.get()==3)
        // Preserve the existing manual-worktree callback, which browses before clearing its submitting flag.
        main {
            set(model,"setLaunchMachine",machine)
            val supported=setOf("catalog","dirs","launch","directory_create","worktrees","worktree_create")
            set(model,"setRelayOperations",mapOf(machine.connectionId to supported));set(model,"setMachineOperations",mapOf(machine to supported))
            val evidence=JSONObject().put("state","ok").put("stale",false).put("path","/fixture").put("common_dir","/fixture/.git")
                .put("worktrees",JSONArray().put(JSONObject().put("path","/fixture").put("kind","main").put("available",true)))
            set(model,"setLaunchWorktreeState",evidence)
            model.createLaunchWorktree(machine,evidence,"fixture-branch","HEAD","/fixture/Manual worktree")
        }
        await("manual worktree confirmation re-browses new path") { main { !model.worktreeSubmitting && !model.launchDirectoryLoading &&
            model.launchCreatedWorktree=="/fixture/Manual worktree" && model.launchDirectoryState?.string("path")=="/fixture/Manual worktree" } }
        check(creates.get()==3 && worktrees.get()==1)
        main { activity.finish() }
    }
}
