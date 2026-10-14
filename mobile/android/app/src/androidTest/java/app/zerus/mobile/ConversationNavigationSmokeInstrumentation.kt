package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.KeyEvent
import android.view.accessibility.AccessibilityNodeInfo
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Real conversation controls with synthetic retained history; no relay or agent mutations. */
class ConversationNavigationSmokeInstrumentation : Instrumentation() {
    private var fixtureModel:ZerusViewModel?=null
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState()) {
                "Use fresh isolated emulator app data; this runner never clears existing state."
            }
            smoke(store)
            result.putString("stream", "Conversation navigation passed: actual latest taps, owned pending read and duplicate lock, installed page tail, incomplete-head retry, same-key read metadata resampling, retained unseen replies, opening divider retirement/alias non-resurrection, draft preservation, and Sessions Read all visibility. Zero deliveries.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            result.putString("stream", "Conversation navigation failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
    private fun <T> main(action:()->T):T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(description:String,condition:()->Boolean) {
        val deadline = SystemClock.elapsedRealtime()+15_000
        while(!condition()) {
            check(SystemClock.elapsedRealtime()<deadline) { "Timed out: $description; ${diagnostic()}" }
            SystemClock.sleep(40)
        }
    }
    private fun diagnostic():String {
        val state=fixtureModel?.let { model -> main {
            "loading=${model.olderLoading},newer=${model.hasNewer},error=${model.olderError},count=${model.selected?.target?.let(model::unreadCount)}"
        } }.orEmpty()
        var current=find(uiAutomation.rootInActiveWindow) { it.contentDescription?.toString()=="Jump to latest messages" }
        val chain=mutableListOf<String>()
        repeat(5) {
            current?.let { node ->
                node.refresh()
                chain+="${node.className}[enabled=${node.isEnabled},clickable=${node.isClickable},text=${node.text},description=${node.contentDescription}]"
                current=node.parent
            }
        }
        return "$state; action=${chain.joinToString(" -> ")}"
    }
    private fun find(root:AccessibilityNodeInfo?,predicate:(AccessibilityNodeInfo)->Boolean):AccessibilityNodeInfo? {
        if(root==null) return null
        root.refresh()
        if(predicate(root)) return root
        for(i in 0 until root.childCount) find(root.getChild(i),predicate)?.let { return it }
        return null
    }
    private fun node(label:String):AccessibilityNodeInfo {
        var found:AccessibilityNodeInfo?=null
        await(label) { found=find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser &&
            (it.contentDescription?.toString()==label || it.text?.toString()==label ||
                it.actionList.any { action -> action.id==AccessibilityNodeInfo.ACTION_CLICK && action.label?.toString()==label }) };found!=null }
        return checkNotNull(found)
    }
    private fun button(label:String):AccessibilityNodeInfo {
        var result=node(label)
        // Refresh each ancestor: Android can cache an enabled action owner
        // while its freshly queried label already belongs to the pending UI.
        while(true) {
            result.refresh()
            if(result.isClickable || !result.isEnabled || result.parent==null) break
            result=checkNotNull(result.parent)
        }
        check(result.isClickable || !result.isEnabled) { "No action owner for $label" }
        return result
    }
    private fun tap(label:String) { check(button(label).performAction(AccessibilityNodeInfo.ACTION_CLICK)) }
    private fun physicalTap(node:AccessibilityNodeInfo) {
        val bounds=Rect().also(node::getBoundsInScreen)
        val density=targetContext.resources.displayMetrics.density
        val minimum=48*density-1
        check(bounds.width()>=minimum && bounds.height()>=minimum) { "Action target is too small: $bounds" }
        check(bounds.left>=0 && bounds.top>=0 && bounds.right<=targetContext.resources.displayMetrics.widthPixels &&
            bounds.bottom<=targetContext.resources.displayMetrics.heightPixels) { "Action target is outside the screen: $bounds" }
        val down=SystemClock.uptimeMillis()
        for(action in listOf(MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP)) {
            val event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,bounds.exactCenterX(),bounds.exactCenterY(),0)
            event.source=InputDevice.SOURCE_TOUCHSCREEN
            try { check(uiAutomation.injectInputEvent(event,true)) { "Physical action tap failed" } }
            finally { event.recycle() }
        }
    }
    private fun visibleText(text:String)=find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && it.text?.contains(text)==true }!=null
    private fun set(model:ZerusViewModel,name:String,value:Any) {
        model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value)
    }
    private fun smoke(store:PrivateStore) {
        val target=Target("fixture-computer","codex/fixture/navigation","fixture-run","fixture-conversation","fixture-workspace")
        val raw=JSONObject().put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("tracked",true).put("runtime_state","live").put("process_state","running").put("activity","idle").put("phase","idle")
            .put("history_epoch","fixture-epoch").put("agent","codex")
        fun row(i:Int)=JSONObject().put("type","AgentMessage").put("seq",i).put("at",i.toDouble())
            .put("detail","Synthetic navigation row $i. "+"Retained conversation text for a bounded scrolling fixture. ".repeat(4))
            .put("history_id","fixture-$i").put("history_cursor","fixture-cursor-$i").put("incoming_seq",i)
        raw.put("events",JSONArray((1..70).map(::row)))
        val session=Session(target,"Navigation fixture","codex","idle","","",0,raw)
        store.saveReadState(ConversationReadState().head(target,"fixture-epoch",null,null,false)
            .viewport(target,"history:fixture-1",0))
        store.saveMessageState(MessageState(drafts=listOf(Draft(target,"Retained fixture draft"))))
        val reads=AtomicInteger();val mutations=AtomicInteger();val mode=AtomicReference("complete")
        val gate=AtomicReference<CountDownLatch?>(null)
        val client=OkHttpClient.Builder().addInterceptor { chain ->
            check(chain.request().url.host=="relay.example.test")
            val input=JSONObject(okio.Buffer().also { chain.request().body?.writeTo(it) }.readUtf8())
            val payload=input.optJSONObject("payload") ?: JSONObject()
            val result=when(input.string("operation")) {
                "inspect" -> JSONObject(raw.toString())
                "history" -> {
                    reads.incrementAndGet()
                    check(!payload.has("around_incoming_seq")) { "Explicit Latest reopened unread history" }
                    gate.get()?.let { check(it.await(30,TimeUnit.SECONDS)) }
                    val complete=mode.get()!="incomplete"
                    val start=if(mode.get() in setOf("larger","reindexed")) 11 else 71
                    val epoch=if(mode.get()=="reindexed") "fixture-rebuilt-epoch" else "fixture-epoch"
                    JSONObject().put("request_id",input.string("request_id")).put("name",target.session)
                        .put("run_id",target.run).put("conversation_id",target.conversation).put("history_epoch",epoch)
                        .put("events",JSONArray((start..80).map(::row))).put("next_before","fixture-cursor-$start")
                        .put("next_after","fixture-cursor-80").put("has_more_before",true).put("has_more_after",false)
                        .put("head",JSONObject().put("complete",complete).put("incoming_seq",80).put("total_incoming",80))
                        .put("indexing",!complete)
                }
                "voice_profiles" -> JSONObject().put("schema",1).put("computers",JSONArray())
                else -> { mutations.incrementAndGet();error("Unexpected fixture operation ${input.string("operation")}") }
            }
            val receipt=JSONObject().put("request_id",input.string("request_id")).put("state","completed").put("result",result)
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
        fixtureModel=model
        try {
            await("private storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible=true }.set(model,RelayApi(client))
                set(model,"setConnections",listOf(Connection(target.connectionId,"Fixture workspace","https://relay.example.test","fixture-token")))
                set(model,"setMachines",listOf(Machine(target.connectionId,target.computerId,"Fixture computer",true)))
                set(model,"setSessions",listOf(session))
                set(model,"setRelayOperations",mapOf(target.connectionId to emptySet<String>()))
                set(model,"setMachineOperations",mapOf(MachineKey(target.connectionId,target.computerId) to emptySet<String>()))
                set(model,"setSelected",session)
                model.refreshActivity()
            }
            await("opened retained position") { main { model.activityVerified && model.readPositionReady } && visibleText("Synthetic navigation row 1.") }
            check(!visibleText("Messages") && find(uiAutomation.rootInActiveWindow) { it.contentDescription?.toString()=="Search loaded messages" }==null)
            tap("Session details");node("Overview")
            sendKeyDownUpSync(KeyEvent.KEYCODE_BACK)
            await("closed details") { !visibleText("Overview") }
            check(main { model.selected?.target==target && model.draft(target).text=="Retained fixture draft" })
            tap("Session actions");tap("Search loaded messages");node("Close")
            tap("Close")
            check(!visibleText("Synthetic navigation row 70."))
            tap("Jump to latest messages")
            await("unchanged content tail") { visibleText("Synthetic navigation row 70.") }
            check(reads.get()==0)
            val originalEvents=main { model.conversationEvents }
            fun middleWindow()=main {
                @Suppress("UNCHECKED_CAST")
                val windows=model.javaClass.getDeclaredField("historyWindows").apply { isAccessible=true }.get(model) as MutableMap<String,HistoryWindow>
                val events=model.conversationEvents
                windows[target.key]=HistoryWindow(target,"fixture-epoch",events,events.first().historyCursor,
                    events.last().historyCursor,true,true,false)
                set(model,"setHasNewer",true)
                set(model,"setRelayOperations",mapOf(target.connectionId to setOf("history")))
                set(model,"setMachineOperations",mapOf(MachineKey(target.connectionId,target.computerId) to setOf("history")))
            }
            // A delayed accepted read publishes no new keys yet: the physical
            // button must stay disabled, and a second activation cannot add a read.
            middleWindow();gate.set(CountDownLatch(1))
            val before=reads.get();tap("Jump to latest messages")
            await("accepted latest read") { reads.get()==before+1 }
            // Network dispatch can precede Compose/accessibility publication.
            // Inspect the actual disabled action owner, not an enabled text child
            // or window ancestor, after its pending label has reached Android.
            node("Loading latest…")
            await("disabled pending latest action") { !button("Jump to latest messages").isEnabled }
            val pending=button("Jump to latest messages")
            check(!pending.isEnabled && find(pending) { it.contentDescription?.toString()=="Jump to latest messages" }!=null &&
                find(pending) { it.text?.toString()=="Loading latest…" }!=null) {
                "Pending Latest does not own its action and progress labels: ${pending.className}, enabled=${pending.isEnabled}"
            }
            physicalTap(pending)
            SystemClock.sleep(100);check(reads.get()==before+1)
            pending.performAction(AccessibilityNodeInfo.ACTION_CLICK)
            SystemClock.sleep(100);check(reads.get()==before+1)
            gate.getAndSet(null)?.countDown()
            await("newly installed tail") { main { !model.olderLoading && !model.hasNewer } && visibleText("Synthetic navigation row 80.") }
            check(main { model.conversationEvents.first().id=="history:fixture-71" })
            check(main { model.draft(target).text=="Retained fixture draft" && model.outgoing.isEmpty() })

            val retained=main { model.conversationEvents }
            middleWindow();mode.set("incomplete")
            tap("Jump to latest messages")
            await("specific incomplete-head failure") { main { !model.olderLoading && model.olderError.contains("still indexing") } }
            check(main { model.conversationEvents==retained && model.hasNewer })
            await("retry enabled") { button("Jump to latest messages").isEnabled }
            mode.set("larger");tap("Jump to latest messages")
            await("retry reaches latest") { main { !model.olderLoading && model.olderError.isBlank() && !model.hasNewer } && visibleText("Synthetic navigation row 80.") }
            check(main { model.conversationEvents.size==70 && model.conversationEvents.first().id=="history:fixture-11" })
            // Viewing the tail may already have read its replies. A genuine
            // local review reminder keeps this explicit action eligible without
            // racing the automatic 350 ms read or resetting its watermark.
            main { model.reviewLater(target,model.captureReadAttention(target)) }
            await("enabled conversation Read all") { button("Read all").isEnabled }
            tap("Read all")
            await("cleared conversation unread replies") { main { !model.readAllPending(target) } && !button("Read all").isEnabled }
            check(main { model.firstUnreadEventId.isBlank() }) { "Read opening divider remained after the complete head was read" }
            check(main { model.draft(target).text=="Retained fixture draft" && model.outgoing.isEmpty() })
            fun replayRetainedAlias(accepted:Boolean=false,live:Boolean=false)=main {
                val frame=checkNotNull(model.javaClass.getDeclaredField("activeFrame").apply { isAccessible=true }.get(model))
                frame.javaClass.getDeclaredField("unreadBoundary").apply { isAccessible=true }.set(frame,"history:fixture-71")
                if(live) set(model,"setFirstUnreadEventId","journal:fixture:legacy-71")
                if(accepted) model.javaClass.getDeclaredField("pendingHistoryPublication").apply { isAccessible=true }.set(model,target.key)
                model.javaClass.declaredMethods.single { it.name=="publishPagingMetadata" }
                    .apply { isAccessible=true }.invoke(model,frame)
                if(live) {
                    check(model.firstUnreadEventId=="history:fixture-71") { "An accepted page lost its live opening divider alias" }
                    set(model,"setFirstUnreadEventId","")
                } else check(model.firstUnreadEventId.isBlank()) { "A retained frame alias resurrected the read divider" }
            }
            replayRetainedAlias()
            replayRetainedAlias(accepted=true)

            // A verified reindex may change read metadata while retaining every
            // display key and pixel. Keep an unread strip mounted throughout,
            // so a changed viewport height cannot accidentally resample it.
            fun observeHead(epoch:String)=main {
                val page=HistoryPage(target,epoch,emptyList(),null,null,false,false,100,100,true,false,false,JSONObject())
                model.javaClass.getDeclaredMethod("observeHistoryHead",HistoryPage::class.java,Boolean::class.javaPrimitiveType)
                    .apply { isAccessible=true }.invoke(model,page,true)
            }
            observeHead("fixture-epoch")
            await("retained visible read watermark") { main { model.unreadCount(target)==20 } && visibleText("Synthetic navigation row 80.") }
            check(main { model.firstUnreadEventId.isBlank() }) { "A newer unread head resurrected the old opening divider" }
            replayRetainedAlias()
            replayRetainedAlias(accepted=true)
            replayRetainedAlias(accepted=true,live=true)
            check(main { model.unreadCount(target)==20 }) { "Divider alias handling changed unread evidence" }
            SystemClock.sleep(600)
            val stableRows=main { model.conversationEvents }
            mode.set("reindexed")
            observeHead("fixture-rebuilt-epoch")
            main {
                val rows=stableRows.map { it.copy(historyEpoch="fixture-rebuilt-epoch") }
                val window=HistoryWindow(target,"fixture-rebuilt-epoch",rows,rows.first().historyCursor,
                    rows.last().historyCursor,true,true,false)
                model.javaClass.getDeclaredMethod("publishWindow",Target::class.java,HistoryWindow::class.java,
                    Boolean::class.javaPrimitiveType,Boolean::class.javaPrimitiveType,String::class.java)
                    .apply { isAccessible=true }.invoke(model,target,window,true,false,null)
            }
            await("same-key canonical metadata resampled") { main { model.unreadCount(target)==20 && model.historyEpoch=="fixture-rebuilt-epoch" } }
            check(main { model.conversationEvents.map { it.id }==stableRows.map { it.id } })
            check(main { model.draft(target).text=="Retained fixture draft" && model.outgoing.isEmpty() })

            middleWindow();gate.set(CountDownLatch(1))
            val leavingRead=reads.get();tap("Jump to latest messages")
            await("accepted read before navigation") { reads.get()==leavingRead+1 }
            // Sessions must not clear the hidden intermediate replies of working
            // sessions. Explicit Review later still remains eligible.
            main {
                model.back()
                set(model,"setSessions",listOf(session.copy(raw=JSONObject(raw.toString()).put("activity","busy")
                    .put("phase","busy").put("mobile_unread_reply",true))))
                // Keep genuine partial read evidence eligible in the reducer;
                // only the list's hidden intermediate presentation excludes it.
                val read=store.readState()
                set(model,"setReadState",read.copy(records=read.records.map {
                    if(it.target==target) it.copy(readThrough=0,seen=emptyList()) else it
                }))
            }
            gate.getAndSet(null)?.countDown()
            val unavailable="Read all unavailable. No unread replies or review reminders are shown in the current scope"
            await("disabled Sessions Read all") { !button(unavailable).isEnabled }
            check(!button(unavailable).isEnabled)
            check(main { model.selected==null && !model.olderLoading && model.canReadAll(target) && !model.listReadAllCandidate(target) })
            check(mutations.get()==0 && originalEvents.size==70)
        } finally {
            gate.getAndSet(null)?.countDown()
            main { model.back();activity.finish() }
        }
    }
}
