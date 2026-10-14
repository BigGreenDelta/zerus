package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Inline question card geometry in a real conversation with synthetic history; no answers are sent. */
class QuestionLayoutSmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState()) {
                "Use fresh isolated emulator app data; this runner never clears existing state."
            }
            val report = smoke(store)
            result.putString("stream", "Question layout passed: $report. Zero deliveries.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Question layout failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $description" }
            SystemClock.sleep(40)
        }
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (predicate(root)) return root
        for (i in 0 until root.childCount) find(root.getChild(i), predicate)?.let { return it }
        return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await(label) { found = find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser &&
            (it.contentDescription?.toString() == label || it.text?.toString() == label) }; found != null }
        return checkNotNull(found)
    }
    private fun owner(label: String): AccessibilityNodeInfo {
        var result = node(label)
        while (!result.isClickable && result.parent != null) result = checkNotNull(result.parent)
        return result
    }
    private fun bounds(node: AccessibilityNodeInfo) = Rect().also(node::getBoundsInScreen)
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun save(bitmap: Bitmap, name: String) =
        File(targetContext.getExternalFilesDir(null), name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    /** The card is the only surface in this exact color. Accessibility bounds are not clipped to it. */
    private fun cardRows(bitmap: Bitmap, x: Int): IntRange {
        val card = 0xFF2B2923.toInt()
        val rows = (0 until bitmap.height).filter { bitmap.getPixel(x, it) == card }
        check(rows.isNotEmpty()) { "No question card pixels at x=$x" }
        return rows.first()..rows.last()
    }
    private fun smoke(store: PrivateStore): String {
        val target = Target("fixture-computer", "codex/fixture/questions", "fixture-run", "fixture-conversation", "fixture-workspace")
        val prompt = "Предлагаю кнопку «Worktrees…» в шапке Projects. Она откроет общий список по проектам и компьютерам: " +
            "ветка, путь, связанные сессии, состояние и причина запрета удаления. Существующий диалог проекта останется " +
            "для быстрых действий. Synthetic layout fixture."
        val question = JSONObject().put("question_id", "fixture-question").put("question_hash", "fixture-hash")
            .put("run_id", target.run).put("conversation_id", target.conversation).put("source", "codex_async")
            .put("optional", true).put("can_answer", true).put("can_skip", false).put("created_at", 1_790_000_000.0)
            .put("questions", JSONArray().put(JSONObject().put("id", "placement").put("question", prompt)
                .put("allow_other", true).put("options", JSONArray(listOf(
                    "Projects header" to "One shared list for every project and computer.",
                    "Project dialog only" to "Keep worktrees inside each project's existing dialog.",
                    "Both" to "Add the shared list and keep the project dialog actions.",
                ).mapIndexed { i, (label, description) ->
                    JSONObject().put("id", "option-$i").put("label", label).put("description", description)
                }))))
        val raw = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "busy").put("phase", "working")
            .put("history_epoch", "fixture-epoch").put("agent", "codex").put("pending_questions", JSONArray().put(question))
        fun row(i: Int) = JSONObject().put("type", "AgentMessage").put("seq", i).put("at", i.toDouble())
            .put("detail", "Synthetic question layout row $i. " + "Retained conversation text above a pending question. ".repeat(4))
            .put("history_id", "fixture-$i").put("history_cursor", "fixture-cursor-$i").put("incoming_seq", i)
        raw.put("events", JSONArray((1..12).map(::row)))
        val session = Session(target, "Question fixture", "codex", "busy", "", "", 0, raw)
        store.saveReadState(ConversationReadState().head(target, "fixture-epoch", null, null, false))
        val mutations = AtomicInteger()
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            check(chain.request().url.host == "relay.example.test")
            val input = JSONObject(okio.Buffer().also { chain.request().body?.writeTo(it) }.readUtf8())
            val result = when (input.string("operation")) {
                "inspect" -> synchronized(raw) { JSONObject(raw.toString()) }
                "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray())
                else -> { mutations.incrementAndGet(); error("Unexpected fixture operation ${input.string("operation")}") }
            }
            val receipt = JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", result)
            Response.Builder().request(chain.request()).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        val density = targetContext.resources.displayMetrics.density
        fun dp(px: Int) = px / density
        /** The whole selectable option row, including its description. */
        fun visible(label: String) = find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && it.text?.toString() == label }
            ?.let { var row = it; while (!row.isClickable && row.parent != null) row = checkNotNull(row.parent).also { parent -> parent.refresh() }; bounds(row) }
            ?.takeIf { !it.isEmpty }
        fun hint() = find(uiAutomation.rootInActiveWindow) { it.text?.toString() == "Scroll for options ↓" } != null
        fun layout(name: String, lastOption: String): String {
            node("Optional question")
            // The Other field is the only editor above the context footer.
            val footer = bounds(owner("Context and cache details"))
            var field: AccessibilityNodeInfo? = null
            await("Other answer field") { field = find(uiAutomation.rootInActiveWindow) { it.isEditable && it.isVisibleToUser &&
                bounds(it).top < footer.top }; field != null }
            SystemClock.sleep(1_000) // Let the opening jump and card layout settle.
            val strip = bounds(owner("Read all"))
            val other = bounds(checkNotNull(field))
            val overflow = hint()
            val screenshot = checkNotNull(uiAutomation.takeScreenshot()) { "No screenshot" }
            val card = try {
                save(screenshot, "question-layout-$name.png")
                // Inside the card's right padding and clear of its rounded corners.
                cardRows(screenshot, screenshot.width - (27 * density).toInt())
            } finally { screenshot.recycle() }
            // The conversation viewport ends at the composer's 4 dp top padding.
            val viewport = footer.top - (4 * density).toInt() - strip.bottom
            val height = card.last + 1 - card.first
            val report = "$name: viewport=${dp(viewport)}dp card=${dp(height)}dp gapBelowCard=${dp(footer.top - card.last - 1)}dp " +
                "otherToCardBottom=${dp(card.last + 1 - other.bottom)}dp other=${dp(other.height())}dp overflow=$overflow"
            check(other.height() >= 56 * density - 1 && card.last + 1 - other.bottom >= 11 * density) { "Other answer is clipped: $report" }
            check(footer.top - card.last - 1 <= 18 * density) { "Empty space below the card: $report" }
            if (name == "long") {
                check(overflow && height >= viewport * .74f) { "Long question card is too small: $report" }
                // Only the disclosure scrolls; the answer field stays where it was.
                val body = checkNotNull(find(uiAutomation.rootInActiveWindow) { it.className == "android.widget.ScrollView" &&
                    bounds(it).top >= card.first && bounds(it).bottom <= card.last }) { "No question disclosure scroller: $report" }
                await("disclosure end") { body.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD); !hint() }
                check(bounds(checkNotNull(field)) == other) { "Answer field moved while the disclosure scrolled: $report" }
            } else check(!overflow && height < viewport * .74f) { "Short question card is not compact: $report" }
            val option = checkNotNull(visible(lastOption)) { "$lastOption is not reachable: $report" }
            // Only the disclosure's end padding, the column gap and the field label separate them.
            check(option.top > card.first && option.bottom <= other.top && other.top - option.bottom <= 40 * density) {
                "$lastOption is not shown directly above the answer field: $report option=$option other=$other"
            }
            return report
        }
        try {
            await("private storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                set(model, "setSessions", listOf(session))
                set(model, "setRelayOperations", mapOf(target.connectionId to emptySet<String>()))
                set(model, "setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to emptySet<String>()))
                set(model, "setSelected", session)
                model.refreshActivity()
            }
            await("question card") { main { model.activityVerified && model.conversationQuestions.isNotEmpty() } }
            val long = layout("long", "Both")
            synchronized(raw) { raw.put("pending_questions", JSONArray().put(JSONObject(question.toString())
                .put("question_id", "fixture-short").put("question_hash", "fixture-short-hash")
                .put("questions", JSONArray().put(JSONObject().put("id", "next").put("question", "Continue with the shared list?")
                    .put("allow_other", true).put("options", JSONArray()
                        .put(JSONObject().put("id", "yes").put("label", "Yes").put("description", ""))
                        .put(JSONObject().put("id", "no").put("label", "No").put("description", ""))))))) }
            main { model.refreshActivity() }
            await("short question") { main { model.conversationQuestions.singleOrNull()?.id == "fixture-short" } }
            val short = layout("short", "No")
            check(mutations.get() == 0) { "Unexpected mutation: $long; $short" }
            return "$long; $short"
        } catch (error: Throwable) {
            uiAutomation.takeScreenshot()?.let { bitmap -> save(bitmap, "question-layout-failure.png"); bitmap.recycle() }
            throw error
        } finally {
            main { model.back(); activity.finish() }
        }
    }
}
