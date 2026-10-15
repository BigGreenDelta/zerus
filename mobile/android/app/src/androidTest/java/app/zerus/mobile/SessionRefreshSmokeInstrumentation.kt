package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
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
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Isolated synthetic catalog and empty inspections; no agents, credentials or deliveries. */
class SessionRefreshSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            smoke()
            result.putString("stream", "Session refresh passed: 5,000-session projection off main, filtering, retained scroll, empty conversation across repeated held background inspections, explicit refresh and draft retention. Zero deliveries.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Session refresh failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }
            SystemClock.sleep(40)
        }
    }
    private fun find(root: AccessibilityNodeInfo?, text: String): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (root.isVisibleToUser && (root.text?.toString() == text || root.contentDescription?.toString() == text)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), text)?.let { return it }
        return null
    }
    private fun visible(text: String) = find(uiAutomation.rootInActiveWindow, text) != null
    private fun scrollDown(root: AccessibilityNodeInfo?): Boolean {
        if (root == null) return false
        root.refresh()
        val action = AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN.id
        if (root.actionList.any { it.id == action } && root.performAction(action)) return true
        return (0 until root.childCount).any { scrollDown(root.getChild(it)) }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun smoke() {
        val store = PrivateStore(targetContext)
        check(store.connections().isEmpty() && store.messageState() == MessageState()) { "Use fresh isolated emulator app data" }
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("storage") { main { model.storageReady } }
        val mainReads = AtomicInteger()
        val workerReads = AtomicInteger()
        val host = Machine("fixture", "computer", "Fixture computer", true)
        val project = ProjectSummary(ProjectKey("fixture", "swarm", "project"), "Fixture project", "", emptyList(), emptyMap())
        val fixtureRows = (0 until 5_000).map { index ->
            Session(Target(host.id, "codex/fixture/session-$index", "run-$index", "conversation-$index", host.connectionId),
                "Session $index", "codex", "", project.name, "Synthetic session $index", 0,
                JSONObject().put("activity", "idle").put("phase", "idle").put("state", "running"), project.key)
        }
        val rows = object : AbstractList<Session>() {
            override val size = fixtureRows.size
            override fun get(index: Int): Session {
                if (Looper.myLooper() == Looper.getMainLooper()) mainReads.incrementAndGet() else workerReads.incrementAndGet()
                return fixtureRows[index]
            }
        }
        main {
            model.preview()
            set(model, "setMachines", listOf(host)); set(model, "setProjects", listOf(project)); set(model, "setSessions", rows)
        }
        await("large catalog") { visible("All sessions, 5000 sessions, selected") }
        check(workerReads.get() >= 5_000) { "The catalog was not prepared on a worker" }
        check(mainReads.get() < 100) { "Catalog-wide scans on main: ${mainReads.get()}" }
        main { model.changeSessionQuery("session-4999") }
        await("last catalog row search") { visible("session-4999") }
        main { model.changeSessionQuery("") }
        await("reset catalog search") { visible("All sessions, 5000 sessions, selected") }
        // Accessibility can report the new count before Compose finishes its query-reset
        // scroll. Settle that reset before requesting a real page movement.
        uiAutomation.waitForIdle(300, 5_000)
        repeat(3) {
            if (visible("session-0")) {
                check(scrollDown(uiAutomation.rootInActiveWindow)) { "Could not scroll the large catalog" }
                uiAutomation.waitForIdle(300, 5_000)
            }
        }
        await("scrolled catalog") { !visible("session-0") }
        val scrollAnchor = (1..30).map { "session-$it" }.firstOrNull(::visible) ?: error("No visible scroll anchor")
        main { model.open(fixtureRows[20], preserveProject = true) }
        await("saved list position") { main { model.sessionListIndex > 0 } }
        val position = main { model.sessionListIndex to model.sessionListOffset }
        main { model.back() }
        await("restored list") { visible(scrollAnchor) }
        check(!visible("session-0")) { "Restored list returned to the top" }
        check(main { model.sessionListIndex to model.sessionListOffset } == position)
        main { model.chooseSessionFilter(SessionFilter.Active) }
        await("active empty") { visible("No sessions match this view.") }
        check(mainReads.get() < 100) { "Filtering scanned the catalog on main: ${mainReads.get()}" }

        val target = Target(host.id, "codex/fixture/new-session", "run", "", host.connectionId)
        val session = fixtureRows.first().copy(target = target)
        val gate = AtomicReference<CountDownLatch?>(null)
        val reads = AtomicInteger()
        val raw = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", "")
            .put("state", "running").put("activity", "unknown")
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            check(chain.request().url.host == "relay.example.test")
            val input = JSONObject(okio.Buffer().also { chain.request().body?.writeTo(it) }.readUtf8())
            check(input.string("operation") == "inspect") { "Unexpected operation ${input.string("operation")}" }
            reads.incrementAndGet()
            gate.get()?.let { check(it.await(20, TimeUnit.SECONDS)) }
            val receipt = JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", raw)
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        try {
            main {
                model.back(); set(model, "setDemo", false)
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection("fixture", "Fixture workspace", "https://relay.example.test", "fixture")))
                set(model, "setRelayOperations", mapOf("fixture" to emptySet<String>()))
                set(model, "setMachineOperations", mapOf(MachineKey("fixture", host.id) to emptySet<String>()))
                set(model, "setSessions", listOf(session))
                model.open(session); model.edit(target, "Retained unsent text")
            }
            await("resolved empty conversation") { visible("No messages yet.") && main { !model.detailBusy && model.activityVerified } }
            repeat(3) {
                val latch = CountDownLatch(1); gate.set(latch)
                val before = reads.get()
                main { model.refreshActivity() }
                await("held background inspection") { reads.get() > before && main { model.detailBusy } }
                check(visible("No messages yet.") && !visible("Loading conversation…") && !visible("History is still starting."))
                latch.countDown(); gate.set(null)
                await("settled background inspection") { main { !model.detailBusy } }
            }
            val latch = CountDownLatch(1); gate.set(latch)
            main { model.refreshActivity(explicit = true) }
            await("explicit refresh") { visible("Refreshing conversation…") }
            check(visible("No messages yet.") && !visible("Loading conversation…"))
            latch.countDown(); gate.set(null)
            await("explicit refresh complete") { main { !model.detailBusy } }
            check(main { model.draft(target).text == "Retained unsent text" })
        } finally {
            gate.get()?.countDown()
            main { activity.finish() }
        }
    }
}
