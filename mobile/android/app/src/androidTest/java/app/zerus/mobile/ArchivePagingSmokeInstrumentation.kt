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
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.ui.Modifier
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Production Sessions controls and real transport parsing, on an owned empty emulator. */
@android.annotation.TargetApi(33)
class ArchivePagingSmokeInstrumentation:Instrumentation(){
    private var variant="normal"
    override fun onCreate(arguments:Bundle?){variant=arguments?.getString("variant")?:"normal";super.onCreate(arguments);start()}
    override fun onStart(){val result=Bundle();try{
        check(Build.VERSION.SDK_INT>=33&&BuildConfig.DEBUG&&Build.HARDWARE in setOf("ranchu","goldfish"));check(PrivateStore(targetContext).connections().isEmpty())
        smoke();result.putString("stream","Archive paging passed ($variant): bounded <=50 retained rows, physical older/newer beyond cap, date/cursor reset and custom date geometry.\n");finish(Activity.RESULT_OK,result)
    }catch(e:Throwable){dump();shot("failure");result.putString("stream","Archive paging failed ($variant): ${e.stackTraceToString()}\n");finish(Activity.RESULT_CANCELED,result)}}
    private fun <T> main(action:()->T):T{val r=AtomicReference<Result<T>>();runOnMainSync{r.set(runCatching(action))};return r.get().getOrThrow()}
    private fun await(label:String,condition:()->Boolean){val until=SystemClock.elapsedRealtime()+15000;while(!condition()){check(SystemClock.elapsedRealtime()<until){"Timed out: $label"};SystemClock.sleep(40)}}
    private fun find(root:AccessibilityNodeInfo?,label:String):AccessibilityNodeInfo?{if(root==null)return null;root.refresh();if(root.isVisibleToUser&&(root.text?.toString()==label||root.contentDescription?.toString()==label))return root;for(i in 0 until root.childCount)find(root.getChild(i),label)?.let{return it};return null}
    private fun node(label:String):AccessibilityNodeInfo{var n:AccessibilityNodeInfo?=null;await(label){n=find(uiAutomation.rootInActiveWindow,label);n!=null};return checkNotNull(n)}
    private fun scroll(root:AccessibilityNodeInfo?,action:Int):Boolean{if(root==null)return false;root.refresh();if(root.isVisibleToUser&&root.actionList.any{it.id==action}&&root.performAction(action))return true;return (0 until root.childCount).any{scroll(root.getChild(it),action)}}
    private fun reveal(label:String){repeat(120){
        try { uiAutomation.waitForIdle(80,2000) } catch(_:java.util.concurrent.TimeoutException) {}
        uiAutomation.clearCache()
        var n=find(uiAutomation.rootInActiveWindow,label)
        if(n!=null){while(!n!!.isClickable&&n!!.parent!=null)n=n!!.parent;val r=Rect().also(n!!::getBoundsInScreen);if(r.height()>=48*targetContext.resources.displayMetrics.density-1)return}
        scroll(uiAutomation.rootInActiveWindow,AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);SystemClock.sleep(150)
    };error("Cannot reveal $label")}

    private fun physical(label:String){reveal(label);SystemClock.sleep(250);var n=node(label);while(!n.isClickable&&n.parent!=null)n=n.parent;check(n.isEnabled&&n.isClickable);val r=Rect().also(n::getBoundsInScreen);val dm=targetContext.resources.displayMetrics;check(r.left>=0&&r.right<=dm.widthPixels&&r.height()>=48*dm.density-1){"Bad target $label: $r"};val at=SystemClock.uptimeMillis();for(a in listOf(MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP)){val e=MotionEvent.obtain(at,SystemClock.uptimeMillis(),a,r.exactCenterX(),r.exactCenterY(),0);try{check(uiAutomation.injectInputEvent(e,true))}finally{e.recycle()}}}
    private fun dump(){val text=StringBuilder();fun walk(n:AccessibilityNodeInfo?){if(n==null)return;val r=Rect().also(n::getBoundsInScreen);text.append("${n.text}|${n.contentDescription}|$r|visible=${n.isVisibleToUser}|click=${n.isClickable}\n");for(i in 0 until n.childCount)walk(n.getChild(i))};walk(uiAutomation.rootInActiveWindow);File(targetContext.getExternalFilesDir("archive-smoke"),"$variant-tree.txt").writeText(text.toString())}
    private fun shot(name:String){SystemClock.sleep(150);val f=File(targetContext.getExternalFilesDir("archive-smoke"),"$variant-$name.png");f.parentFile?.mkdirs();uiAutomation.takeScreenshot()?.let{b->f.outputStream().use{b.compress(Bitmap.CompressFormat.PNG,100,it)};b.recycle()}}
    private fun set(model:ZerusViewModel,name:String,value:Any){model.javaClass.declaredMethods.single{it.name==name}.apply{isAccessible=true}.invoke(model,value)}
    private fun published(model:ZerusViewModel):Boolean=main {
        val expected=model.archive.rows;val actual=model.sessions.filter(SessionFilters::archived)
        check(model.sessions===model.sessions){"Published catalog identity must stay stable between reads"}
        actual.size==expected.size && actual.zip(expected).all{(row,page)->row===page}
    }
    private fun smoke(){
        val key=MachineKey("fixture-workspace","11111111-1111-4111-8111-111111111111");val calls=AtomicInteger();val last=AtomicReference<JSONObject>()
        val client=OkHttpClient.Builder().addInterceptor{chain->val request=chain.request();check(request.url.host=="relay.example.test"&&request.method=="POST"&&request.url.encodedPath=="/v1/requests")
            val body=JSONObject(okio.Buffer().also{request.body?.writeTo(it)}.readUtf8());check(body.string("operation")=="archive_list"&&body.string("session").isEmpty()&&body.string("computer_id")==key.computerId)
            val q=body.getJSONObject("payload");check(q.getInt("limit")==50);last.set(q);calls.incrementAndGet();val start=q.string("cursor").removePrefix("page_").toIntOrNull()?:0;val rows=JSONArray()
            for(i in start until start+50)rows.put(JSONObject().put("name","codex/example/archived-$i").put("cmd","codex").put("state","archived").put("run_id","run-$i").put("conversation_id","conversation-$i").put("archive_id","10000000-0000-4000-8000-${(i+1).toString().padStart(12,'0')}").put("archived_at",q.getLong("from")+2043-i))
            val result=JSONObject().put("version",1).put("request_id",body.string("request_id")).put("sessions",rows).put("next_cursor","page_${start+50}").put("previous_cursor",if(start>0)"page_${start-50}"else"").put("position",start).put("total",2043).put("from",q.getLong("from")).put("until",q.getLong("until")).put("query",q.string("query"))
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("Fixture").body(JSONObject().put("request_id",body.string("request_id")).put("state","completed").put("result",result).toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity=startActivitySync(Intent(targetContext,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model=main{activity.javaClass.getDeclaredMethod("getModel").apply{isAccessible=true}.invoke(activity) as ZerusViewModel};await("private storage"){main{model.storageReady}}
        val legacyRaw=JSONObject().put("name","codex/example/legacy").put("state","archived").put("archive_id","10000000-0000-4000-8000-000000009999").put("archived_at",100)
        val legacy=Session(Target(key.computerId,legacyRaw.string("name"),"legacy-run","legacy-conversation",key.connectionId,legacyRaw.string("archive_id")),"Legacy","codex","archived","example","",0,legacyRaw)
        val live=Session(Target(key.computerId,"codex/example/live","live-run","live-conversation",key.connectionId),"Live","codex","running","example","",0,JSONObject().put("state","running"))
        main{
            model.javaClass.getDeclaredField("api").apply{isAccessible=true}.set(model,RelayApi(client));set(model,"setConnections",listOf(Connection(key.connectionId,"Fixture","https://relay.example.test","synthetic-token")))
            set(model,"setMachines",listOf(Machine(key.connectionId,key.computerId,"Fixture computer",true)));set(model,"setRelayOperations",mapOf(key.connectionId to setOf("archive_list")));set(model,"setMachineOperations",mapOf(key to setOf("archive_list")))
            set(model,"setSessions",listOf(live,legacy))
            activity.setContent{MaterialTheme(colorScheme=androidx.compose.material3.darkColorScheme()){Surface(modifier=Modifier.systemBarsPadding()){SessionsScreen(model,{})}}}
        }
        await("published upgraded catalog excludes legacy preview"){main{model.sessions.any{it.target==live.target} && model.sessions.none{SessionFilters.archived(it)}}}
        main{set(model,"setRelayOperations",mapOf(key.connectionId to emptySet<String>()))}
        await("published older relay retains legacy archive"){main{model.sessions.any{it.target==live.target} && model.sessions.any{it.target==legacy.target}}}
        main{set(model,"setRelayOperations",mapOf(key.connectionId to setOf("archive_list")))}
        await("published paging catalog restored"){main{model.sessions.any{it.target==live.target} && model.sessions.none{SessionFilters.archived(it)}}}
        SystemClock.sleep(300);check(calls.get()==0){"Eager archive fetch"};main{model.chooseSessionFilter(SessionFilter.Archive)}
        await("first page"){main{!model.archive.loading&&model.archive.rows.size==50}&&published(model)};check(calls.get()==1);shot("first")
        for(start in listOf(50,100)){
            physical("Older archived sessions");await("older window $start"){main{!model.archive.loading&&model.archive.rows.size==50&&model.archive.rows.first().target.run=="run-$start"}&&published(model)}
            check(last.get().string("cursor")=="page_$start");check(main{model.sessions.count{SessionFilters.archived(it)}==50})
        };check(calls.get()==3);shot("older")
        for(start in listOf(50,0)){
            physical("Newer archived sessions");await("newer window $start"){main{!model.archive.loading&&model.archive.rows.size==50&&model.archive.rows.first().target.run=="run-$start"}&&published(model)}
            check(last.get().string("cursor")=="page_$start")
        };check(calls.get()==5&&!main{model.archive.hasNewer});shot("newer")
        main{model.archive.dates(ArchivePeriod.Today)};await("date reset"){calls.get()==6&&main{!model.archive.loading&&model.archive.rows.size==50}&&published(model)};check(last.get().string("cursor").isBlank()&&last.get().getLong("from")>0);shot("today")
        main{model.archive.dates(ArchivePeriod.Custom)};await("custom range"){calls.get()==7&&main{!model.archive.loading}&&published(model)}
        val original=main{model.archive.first};val dateLabel="From $original";physical(dateLabel);shot("calendar")
        var cancel=node("CANCEL");while(!cancel.isClickable&&cancel.parent!=null)cancel=cancel.parent;check(cancel.performAction(AccessibilityNodeInfo.ACTION_CLICK));check(calls.get()==7)
        await("calendar dismissed"){uiAutomation.clearCache();find(uiAutomation.rootInActiveWindow,dateLabel)!=null}
        physical(dateLabel);val chosen=original.withDayOfMonth(if(original.dayOfMonth<28)original.dayOfMonth+1 else original.dayOfMonth-1)
        var day=node(chosen.dayOfMonth.toString());while(!day.isClickable&&day.parent!=null)day=day.parent;check(day.performAction(AccessibilityNodeInfo.ACTION_CLICK))
        var okay=node("OK");while(!okay.isClickable&&okay.parent!=null)okay=okay.parent;check(okay.performAction(AccessibilityNodeInfo.ACTION_CLICK))
        await("applied native date"){calls.get()==8&&main{model.archive.first==chosen&&!model.archive.loading}&&published(model)};check(last.get().string("cursor").isBlank());shot("custom");main{activity.finish()}
    }
}
