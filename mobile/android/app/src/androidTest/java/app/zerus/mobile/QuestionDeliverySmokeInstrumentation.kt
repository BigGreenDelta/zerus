package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Real UI and receipt reads against a reserved, intercepted endpoint. No native delivery. */
class QuestionDeliverySmokeInstrumentation : Instrumentation() {
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState())
            smoke(store)
            result.putString("stream", "Question delivery passed: physical Next/Enter/Send all, retained per-question answers, exact single synthetic answer, recovered receipt, unresolved receipt actions/no replay, native-to-canonical read identity and unseen reply retention.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            screenshot("failure")
            result.putString("stream", "Question delivery failed: ${error.stackTraceToString()}\n")
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
    private fun visible(label: String) = find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser &&
        (it.text?.toString() == label || it.contentDescription?.toString() == label) }
    private fun node(label: String): AccessibilityNodeInfo {
        await(label) { visible(label) != null }
        return checkNotNull(visible(label))
    }
    private fun button(label: String): AccessibilityNodeInfo {
        var result = node(label)
        while (true) {
            result.refresh()
            if (result.isClickable || !result.isEnabled || result.parent == null) return result
            result = checkNotNull(result.parent)
        }
    }
    private fun physicalTap(label: String) {
        val node = button(label)
        check(node.isEnabled && node.isClickable) { "$label is unavailable" }
        val bounds = Rect().also(node::getBoundsInScreen)
        val minimum = 48 * targetContext.resources.displayMetrics.density - 1
        check(bounds.width() >= minimum && bounds.height() >= minimum) { "Small $label target: $bounds" }
        check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= targetContext.resources.displayMetrics.widthPixels &&
            bounds.bottom <= targetContext.resources.displayMetrics.heightPixels) { "$label is outside the display: $bounds" }
        val down = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, bounds.exactCenterX(), bounds.exactCenterY(), 0)
            event.source = InputDevice.SOURCE_TOUCHSCREEN
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
        }
    }
    private fun revealOption(label: String) {
        // Large system text can require the card's existing disclosure scroll.
        val header = Rect().also(node("Optional question")::getBoundsInScreen)
        await("scroll to $label") {
            if (visible(label) != null) true else {
                find(uiAutomation.rootInActiveWindow) { row ->
                    val bounds = Rect().also(row::getBoundsInScreen)
                    row.isVisibleToUser && row.isScrollable && row.className == "android.widget.ScrollView" && bounds.top >= header.bottom
                }?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            }
        }
    }
    private fun screenshot(name: String) {
        uiAutomation.waitForIdle(100, 2_000)
        uiAutomation.takeScreenshot()?.let { bitmap ->
            targetContext.getExternalFilesDir(null)?.resolve("question-delivery-$name.png")?.outputStream()?.use {
                check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
            bitmap.recycle()
        }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun smoke(store: PrivateStore) {
        val target = Target("fixture-computer", "codex/fixture/questions", "fixture-run", "fixture-conversation", "fixture-workspace")
        val confirmedId = "00000000-0000-4000-8000-000000000001"
        val unknownId = "00000000-0000-4000-8000-000000000002"
        val original = OutgoingMessage(confirmedId, target, "Original receipt-confirmed message", emptyList(), 1.0, status = "uncertain")
        store.saveMessageState(MessageState(outgoing = listOf(original), drafts = listOf(Draft(target, "Next composition retained"))))
        store.saveReadState(ConversationReadState().head(target, "fixture-epoch", 63, 63, true))
        val raw = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "idle").put("phase", "idle")
            .put("agent", "codex").put("pending_questions", JSONArray()).put("events", JSONArray().put(JSONObject()
                .put("type", "AgentMessage").put("message_id", "fixture-reply").put("source", "codex_transcript")
                .put("at", 10.0).put("detail", "Latest synthetic reply read on this phone.")))
        val session = Session(target, "Question delivery fixture", "codex", "idle", "", "", 0, raw)
        val confirmedReads = AtomicInteger(); val unknownReads = AtomicInteger(); val answers = AtomicInteger(); val sends = AtomicInteger()
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            check(request.url.host == "relay.example.test" && request.header("Authorization") == "Bearer fixture-token")
            val receipt = if (request.method == "GET") {
                val id = request.url.pathSegments.last()
                check(request.url.encodedPath == "/v1/requests/$id" && id in setOf(confirmedId, unknownId))
                if (id == unknownId) {
                    unknownReads.incrementAndGet()
                    return@addInterceptor Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(404).message("Synthetic missing receipt")
                        .body("{}".toResponseBody("application/json".toMediaType())).build()
                }
                confirmedReads.incrementAndGet()
                JSONObject().put("request_id", id).put("state", "completed").put("result", JSONObject().put("status", "submitted").put("submitted_at", 1.0))
            } else {
                check(request.method == "POST" && request.url.encodedPath == "/v1/requests")
                val input = JSONObject(okio.Buffer().also { request.body?.writeTo(it) }.readUtf8())
                val payload = input.optJSONObject("payload") ?: JSONObject()
                val result = when (input.string("operation")) {
                    "inspect" -> synchronized(raw) { JSONObject(raw.toString()) }
                    "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray())
                    "answer" -> {
                        check(input.string("computer_id") == target.computerId && input.string("session") == target.session)
                        check(payload.string("request_id") == input.string("request_id") && payload.string("expected_run_id") == target.run &&
                            payload.string("expected_conversation_id") == target.conversation)
                        check(payload.string("question_id") == "fixture-question" && payload.string("expected_question_hash") == "fixture-hash")
                        val submitted = payload.getJSONArray("answers").objects()
                        check(submitted.map { it.string("question_id") } == listOf("first", "second"))
                        check(submitted.all { it.getJSONArray("selected_option_ids").toString() == "[\"yes\"]" && it.string("text").isEmpty() })
                        check(answers.incrementAndGet() == 1)
                        JSONObject().put("status", "submitted")
                    }
                    "send" -> { sends.incrementAndGet(); error("Message replay is forbidden") }
                    else -> error("Unexpected operation ${input.string("operation")}")
                }
                JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", result)
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        fun observeHead(rows: List<Event>, total: Long) = main {
            val page = HistoryPage(target, "fixture-epoch", rows, null, null, false, false, total, total, true, false, false, JSONObject())
            model.javaClass.getDeclaredMethod("observeHistoryHead", HistoryPage::class.java, Boolean::class.javaPrimitiveType)
                .apply { isAccessible = true }.invoke(model, page, true)
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
                set(model, "setSelected", session); model.refreshActivity()
            }
            await("recovered confirmed receipt") { main { model.outgoing.single().status == "submitted" && model.activityVerified } }
            check(confirmedReads.get() == 1 && sends.get() == 0)
            node("Latest synthetic reply read on this phone.")
            await("dwell recorded the native reply") { main { model.conversationViewport(target) != null } }
            val native = main { model.conversationEvents.single { it.nativeType == "AgentMessage" } }
            val canonical = native.copy(id = "history:fixture-reply", historyId = "fixture-reply", historyEpoch = "fixture-epoch",
                historyCursor = "fixture-cursor", incomingSeq = 74, originalId = native.id)
            observeHead(listOf(canonical), 74)
            await("canonical badge cleared") { main { model.unreadCount(target) == 0 } }
            main { model.back() }
            await("Sessions retains read position") { main { model.selected == null && model.unreadCount(target) == 0 && model.loadedUnreadCount(target) == 0 } }
            await("Sessions clears reply attention") { main { model.sessions.none { it.raw.optBoolean("mobile_unread_reply") } } }
            observeHead(emptyList(), 75)
            check(main { model.unreadCount(target) == 1 }) { "Unseen later reply was acknowledged" }
            main { set(model, "setSelected", session); model.refreshActivity() }
            await("reopened conversation") { main { model.activityVerified } }
            val unknown = original.copy(requestId = unknownId, text = "Unconfirmed original message", status = "uncertain")
            main {
                model.javaClass.getDeclaredMethod("editState", kotlin.jvm.functions.Function1::class.java).apply { isAccessible = true }
                    .invoke(model, { state: MessageState -> state.copy(outgoing = state.outgoing + unknown) })
            }
            await("one automatic unresolved receipt read") { unknownReads.get() == 1 && main { model.receiptFlights.isEmpty() } }
            node("Previous message not confirmed"); node("Unconfirmed original message")
            check(visible("Delivery unknown. Check the previous message before sending again.") == null)
            screenshot("recovery")
            physicalTap("Check previous message delivery")
            await("explicit receipt retry") { unknownReads.get() == 2 && main { model.receiptFlights.isEmpty() } }
            physicalTap("Review previous message"); node("Review delivery"); physicalTap("Cancel")
            check(main { model.draft(target).text == "Next composition retained" && model.outgoing.last().status == "uncertain" })
            physicalTap("Dismiss error")
            val question = JSONObject().put("question_id", "fixture-question").put("question_hash", "fixture-hash")
                .put("run_id", target.run).put("conversation_id", target.conversation).put("source", "codex_async")
                .put("optional", true).put("can_answer", true).put("can_skip", false)
                .put("questions", JSONArray(listOf("first", "second").map { id -> JSONObject().put("id", id)
                    .put("question", "Choose the $id answer").put("allow_other", true).put("options", JSONArray()
                        .put(JSONObject().put("id", "yes").put("label", "Yes $id"))) }))
            synchronized(raw) { raw.put("pending_questions", JSONArray().put(question)) }
            main { model.refreshActivity() }
            // The existing full-height editor is the supported large-text path;
            // recovery controls above were exercised in the inline conversation.
            if (targetContext.resources.configuration.screenWidthDp < 360 ||
                targetContext.resources.configuration.fontScale > 1.3f) physicalTap("Expand")
            node("Choose the first answer")
            check(!button("Next question").isEnabled)
            revealOption("Yes first"); physicalTap("Yes first")
            await("enabled Next") { button("Next question").isEnabled }
            screenshot("first-next")
            physicalTap("Next question")
            node("Choose the second answer")
            check(answers.get() == 0 && !button("Send all").isEnabled)
            physicalTap("Previous part"); node("Choose the first answer")
            val other = checkNotNull(find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && it.isEditable })
            check(other.performAction(AccessibilityNodeInfo.ACTION_FOCUS))
            sendKeyDownUpSync(KeyEvent.KEYCODE_ENTER)
            node("Send all")
            check(answers.get() == 0) { "Enter on an earlier question submitted the request" }
            node("Choose the second answer")
            check(answers.get() == 0) { "Enter on an earlier question submitted the request" }
            revealOption("Yes second"); physicalTap("Yes second")
            physicalTap("Previous part")
            node("Choose the first answer")
            check(main { JSONArray(model.draft(target, model.conversationQuestions.single()).answers).objects().all {
                it.getJSONArray("selected_option_ids").toString() == "[\"yes\"]" } })
            physicalTap("Next question"); node("Choose the second answer")
            screenshot("final-send")
            physicalTap("Send all")
            await("one exact complete answer") { answers.get() == 1 }
            check(unknownReads.get() == 2 && confirmedReads.get() == 1 && sends.get() == 0)
            check(main { model.draft(target).text == "Next composition retained" })
        } finally { main { model.back(); activity.finish() } }
    }
}
