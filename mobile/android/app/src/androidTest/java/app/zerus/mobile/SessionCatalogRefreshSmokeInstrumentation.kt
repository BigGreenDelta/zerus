package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.Looper
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/** Owned emulator, synthetic reports and no native operations or deliveries. */
class SessionCatalogRefreshSmokeInstrumentation:Instrumentation() {
    override fun onCreate(arguments:Bundle?) { super.onCreate(arguments);start() }
    private fun <T> main(action:()->T):T {
        val result=AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) };return result.get().getOrThrow()
    }
    private fun set(model:ZerusViewModel,name:String,value:Any) {
        model.javaClass.declaredMethods.single { it.name==name }.apply { isAccessible=true }.invoke(model,value)
    }
    private fun await(label:String,condition:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+20_000
        while(!condition()) { check(SystemClock.elapsedRealtime()<end) { "Timed out: $label" };SystemClock.sleep(30) }
    }
    private fun find(root:AccessibilityNodeInfo?,text:String):AccessibilityNodeInfo? {
        if(root==null) return null
        root.refresh()
        if(root.isVisibleToUser && (root.text?.toString()==text || root.contentDescription?.toString()==text)) return root
        for(i in 0 until root.childCount) find(root.getChild(i),text)?.let { return it }
        return null
    }
    private fun visible(text:String)=find(uiAutomation.rootInActiveWindow,text)!=null
    private fun click(text:String) {
        var node=find(uiAutomation.rootInActiveWindow,text) ?: error("Missing $text")
        while(!node.isClickable) node=node.parent ?: error("No action for $text")
        check(node.performAction(AccessibilityNodeInfo.ACTION_CLICK))
    }
    private fun screenshot(name:String) {
        val image=checkNotNull(uiAutomation.takeScreenshot())
        File(targetContext.getExternalFilesDir(null),name).outputStream().use { check(image.compress(Bitmap.CompressFormat.PNG,100,it)) }
        image.recycle()
    }
    override fun onStart() {
        val result=Bundle()
        val release=CountDownLatch(1)
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu","goldfish"))
            val store=PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState()==MessageState() && store.readState()==ConversationReadState()) { "Use fresh owned emulator data" }
            val targets=(0 until 24).map { Target("computer","claude/example/session-$it","run-$it","conversation-$it","fixture") }
            val reads=targets.map { ConversationReadRecord(it,reviewLater=true) }
            store.saveReadState(ConversationReadState(reads))
            val now=System.currentTimeMillis()/1000.0
            val rows=targets.mapIndexed { i,t -> JSONObject().put("name",t.session).put("tag","Session $i").put("run_id",t.run)
                .put("conversation_id",t.conversation).put("agent","claude").put("state","running").put("process_state","running")
                .put("tracked",true).put("activity","idle").put("phase","idle").put("cwd","/srv/example/project").put("last_message","Synthetic reply")
                .put("attention_signature",JSONObject.NULL).put("read",JSONObject().put("conversation_id",t.conversation).put("reply_id","7:100.5")) }
            val accounts=JSONArray().put(JSONObject().put("id","example").put("provider","claude").put("label","Example Claude").put("installed",true)
                .put("usage",JSONObject().put("status","unavailable").put("auth_status","signed_in").put("refresh_error",true).put("checked_at",now-7200)
                    .put("identity",JSONObject().put("email","person@example.com").put("plan","Pro"))
                    .put("windows",JSONArray().put(JSONObject().put("id","weekly").put("label","Weekly").put("window_minutes",10080).put("used_percent",42).put("resets_at",now+7200)))))
                .put(JSONObject().put("id","api").put("provider","dsh").put("label","Example API").put("installed",true)
                    .put("usage",JSONObject().put("status","error").put("auth_status","signed_in").put("refresh_error",true)))
                .put(JSONObject().put("id","signed-out").put("provider","codex").put("label","Example sign-in").put("installed",true)
                    .put("usage",JSONObject().put("status","signed_out").put("auth_status","signed_out")))
            val computer=JSONObject().put("id","computer").put("name","Example computer").put("online",true).put("snapshot",JSONObject()
                .put("sessions",JSONArray(rows)).put("mobile_capabilities",JSONObject().put("schema",1).put("operations",JSONArray())
                    .put("features",JSONArray().put("accounts_snapshot")))
                .put("mobile_accounts",JSONObject().put("schema",1).put("available",true).put("checked_at",now).put("accounts",accounts)))
            val client=OkHttpClient.Builder().addInterceptor { chain ->
                check(chain.request().url.host=="relay.example.test" && chain.request().url.encodedPath=="/v1/computers") { "Unexpected transport" }
                Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                    .body(JSONObject().put("computers",JSONArray().put(computer)).toString().toResponseBody("application/json".toMediaType())).build()
            }.build()
            val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
            val model=main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible=true }.invoke(activity) as ZerusViewModel }
            await("storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible=true }.set(model,RelayApi(client))
                set(model,"setConnections",listOf(Connection("fixture","Example workspace","https://relay.example.test","synthetic")))
                set(model,"setRelayOperations",mapOf("fixture" to emptySet<String>()))
                model.refresh()
            }
            await("decorated catalog") { main { !model.catalogBusy && model.sessions.size==24 && model.sessions.all { it.raw.optBoolean("mobile_review_later") } } && visible("Session 0") }
            uiAutomation.waitForIdle(300,5_000)
            val before=checkNotNull(find(uiAutomation.rootInActiveWindow,"Session 0"))
            val bounds=Rect().also(before::getBoundsInScreen)
            val initial=main { model.sessions.first() }
            val entered=CountDownLatch(1)
            val refreshedReads=reads.map { it.copy(established=true) }
            val heldReads=object:AbstractList<ConversationReadRecord>() {
                override val size=refreshedReads.size
                override fun get(index:Int):ConversationReadRecord {
                    if(Looper.myLooper()!=Looper.getMainLooper()) { entered.countDown();check(release.await(15,TimeUnit.SECONDS)) }
                    return refreshedReads[index]
                }
            }
            main { set(model,"setReadState",ConversationReadState(heldReads));model.refresh() }
            check(entered.await(10,TimeUnit.SECONDS)) { "Read decoration did not run on its worker" }
            SystemClock.sleep(200)
            check(main { model.sessions.all { it.raw.optBoolean("mobile_review_later") } }) { "Refresh exposed undecorated rows" }
            val during=checkNotNull(find(uiAutomation.rootInActiveWindow,"Session 0"))
            check(during==before && Rect().also(during::getBoundsInScreen)==bounds) { "Refresh replaced or moved the mounted card" }
            release.countDown()
            await("refresh complete") { main { !model.catalogBusy && model.sessions.first()===initial } }
            check(Rect().also(checkNotNull(find(uiAutomation.rootInActiveWindow,"Session 0"))::getBoundsInScreen)==bounds)
            main {
                set(model,"setReadState",ConversationReadState().head(targets.first(),"epoch",20,20,true))
                set(model,"setCurrentHistoryHeads",setOf(ConversationReadPolicies.key(targets.first())))
                model.refresh()
            }
            await("shared owner cleared old history badge") { main { !model.catalogBusy && !model.sessions.first().raw.optBoolean("mobile_review_later") } }
            uiAutomation.waitForIdle(300,5_000)
            check(!visible("New reply") && !visible("20 unread messages"))
            screenshot("refresh-sessions.png")
            click("Accounts")
            await("account cards") { visible("Example Claude") && visible("Example API") }
            check(!visible("Provider refresh failed") && !visible("Signed in / Usage unavailable"))
            check(visible("Signed out")) { "A real sign-in problem was hidden" }
            screenshot("refresh-accounts.png")
            click("Example Claude")
            await("retained report diagnostics") { visible("Account details") }
            screenshot("refresh-account-details.png")
            main { activity.finish() }
            result.putString("stream","Catalog refresh passed: held worker retains card identity/geometry and read decoration; pruned shared reply does not revive old history badge; compact accounts retain real sign-in errors and diagnostic details. Zero native operations.\n")
            finish(Activity.RESULT_OK,result)
        } catch(error:Throwable) {
            release.countDown()
            runCatching { screenshot("refresh-failure.png") }
            result.putString("stream","Catalog refresh failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED,result)
        }
    }
}
