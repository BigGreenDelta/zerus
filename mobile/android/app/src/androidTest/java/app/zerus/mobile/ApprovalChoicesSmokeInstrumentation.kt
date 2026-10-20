package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
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
import java.util.concurrent.atomic.AtomicReference

/** Actual Compose/card/model paths, intercepted receipts only; never a native provider or connected workspace. */
class ApprovalChoicesSmokeInstrumentation : Instrumentation() {
    private val cases = mutableListOf<String>()
    private val phases = ArrayDeque<String>()
    private data class EarlySend(val node: AccessibilityNodeInfo, val window: Int, val current: () -> Boolean)
    private var earlySend: EarlySend? = null
    private data class InterceptorFailure(val guard: String, val type: String, val method: String, val path: String)
    private val interceptorFailure = AtomicReference<InterceptorFailure?>(null)
    private fun requireHealthyFixture() {
        interceptorFailure.get()?.let { failure ->
            error("Fixture interceptor ${failure.type} at ${failure.guard}: ${failure.method} ${failure.path}")
        }
    }
    private fun boundary(event: String, name: String) {
        check(event in setOf("begin", "complete") && name in CASES)
        Log.i("ZerusApprovalUI", "$event $name completed=${cases.size} elapsedMs=${SystemClock.elapsedRealtime()}")
    }
    private fun phase(name: String) {
        val record = "phase ${name.take(100)} elapsedMs=${SystemClock.elapsedRealtime()}".take(160)
        if (phases.size == 64) phases.removeFirst()
        phases.addLast(record)
        Log.i("ZerusApprovalUI", record)
    }
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState())
            smoke(store)
            requireHealthyFixture()
            check(cases == CASES)
            result.putString("approval_cases", cases.size.toString())
            result.putString("approval_case_ids", cases.joinToString(","))
            result.putString("stream", "Approval choices smoke passed: ${cases.joinToString(",")}; synthetic receipts only.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            screenshot("failure")
            result.putString("stream", "Approval choices smoke failed: ${error.stackTraceToString()}\n${phases.joinToString("\n")}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }

    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        requireHealthyFixture()
        while (!condition()) {
            requireHealthyFixture()
            check(SystemClock.elapsedRealtime() < end) { "Timed out: $description" }
            SystemClock.sleep(30)
        }
        requireHealthyFixture()
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null) return null
        if (!root.refresh()) return null
        if (predicate(root)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), predicate)?.let { return it }
        return null
    }
    private fun exactVisible(node: AccessibilityNodeInfo, label: String) = node.isVisibleToUser &&
        (node.text?.toString() == label || node.contentDescription?.toString() == label)
    private fun visible(label: String): AccessibilityNodeInfo? {
        val root = uiAutomation.rootInActiveWindow ?: return null
        // The native search is containment, not exact equality; validate its refreshed candidates.
        root.findAccessibilityNodeInfosByText(label).orEmpty().firstOrNull { it.refresh() && exactVisible(it, label) }?.let { return it }
        // Embedded hierarchies/content descriptions may require the original safe subtree walk.
        return find(root) { exactVisible(it, label) }
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var matched: AccessibilityNodeInfo? = null
        await(label) { matched = visible(label); matched != null }
        return checkNotNull(matched)
    }
    private fun button(label: String): AccessibilityNodeInfo {
        val cached = earlySend?.takeIf { label == "Send answer" }
        var current = if (cached != null) {
            check(cached.current()) { "Early-review card identity changed" }
            check(cached.node.refresh() && cached.node.windowId == cached.window && exactVisible(cached.node, label)) {
                "Early-review Send node is stale"
            }
            cached.node // No root search or fallback for a stale scoped cache.
        } else node(label)
        while (true) {
            check(current.refresh())
            if (current.isClickable || !current.isEnabled || current.parent == null) return current
            current = checkNotNull(current.parent)
        }
    }
    private fun tap(label: String) {
        val action = button(label)
        check(action.isEnabled && action.isClickable) { "Disabled action $label" }
        val bounds = Rect().also(action::getBoundsInScreen)
        check(!bounds.isEmpty && bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= targetContext.resources.displayMetrics.widthPixels && bounds.bottom <= targetContext.resources.displayMetrics.heightPixels)
        val down = SystemClock.uptimeMillis()
        phase("touch-down:$label")
        for (type in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), type, bounds.exactCenterX(), bounds.exactCenterY(), 0)
            event.source = InputDevice.SOURCE_TOUCHSCREEN
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
        }
        phase("touch-up:$label")
    }
    private fun reveal(label: String) {
        await("option $label") {
            if (visible(label) != null) true else {
                checkNotNull(find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && it.isScrollable })
                    .performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            }
        }
    }
    private fun screenshot(name: String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            targetContext.getExternalFilesDir(null)?.resolve("approval-choices-$name.png")?.outputStream()?.use {
                check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
            bitmap.recycle()
        }
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    private fun smoke(store: PrivateStore) {
        val target = Target("fixture-computer", "kimi/fixture/approval", "fixture-run", "fixture-conversation", "fixture-workspace")
        store.saveMessageState(MessageState(drafts = listOf(Draft(target, "Keep unrelated composer"))))
        store.saveReadState(ConversationReadState().head(target, "fixture-epoch", 0, 0, true))
        val raw = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("tracked", true).put("runtime_state", "live").put("process_state", "running").put("activity", "idle").put("phase", "idle")
            .put("agent", "kimi").put("pending_questions", JSONArray()).put("events", JSONArray())
        val calls = mutableListOf<JSONObject>()
        val expected = AtomicReference<JSONObject?>(null)
        var pending: JSONObject? = null
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            var guard = "request-scope"
            try {
                check(request.url.host == "relay.example.test" && request.url.encodedPath == "/v1/requests" && request.method == "POST")
                guard = "authorization"
                check(request.header("Authorization") == "Bearer fixture-token")
                guard = "request-json"
                val input = JSONObject(okio.Buffer().also { request.body?.writeTo(it) }.readUtf8())
                guard = "operation"
                val value = when (input.string("operation")) {
                    "inspect" -> synchronized(raw) { JSONObject(raw.toString()) }
                    "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray())
                    "answer" -> {
                        guard = "answer-admission"
                        val proof = checkNotNull(expected.getAndSet(null)) { "Unadmitted answer" }
                        guard = "answer-payload"
                        val payload = input.getJSONObject("payload")
                        guard = "answer-target"
                        check(input.string("computer_id") == target.computerId && input.string("session") == target.session)
                        guard = "answer-receipt-identity"
                        check(payload.string("request_id") == input.string("request_id") && payload.string("expected_run_id") == target.run &&
                            payload.string("expected_conversation_id") == target.conversation)
                        guard = "answer-question-identity"
                        check(payload.string("question_id") == proof.string("id") && payload.string("expected_question_hash") == proof.string("hash"))
                        guard = "answer-list"
                        val answer = payload.getJSONArray("answers").objects().single()
                        guard = "answer-body"
                        check(answer.string("question_id") == "approval" && answer.getJSONArray("selected_option_ids").toString() == proof.getJSONArray("options").toString() &&
                            answer.string("text") == proof.string("text"))
                        synchronized(calls) { calls.add(JSONObject(payload.toString())) }
                        JSONObject().put("status", "submitted")
                    }
                    else -> error("Unexpected operation ${input.string("operation")}")
                }
                guard = "receipt"
                val receipt = JSONObject().put("request_id", input.string("request_id")).put("state", "completed").put("result", value)
                Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                    .body(receipt.toString().toResponseBody("application/json".toMediaType())).build()
            } catch (error: Throwable) {
                val failure = InterceptorFailure(guard, error.javaClass.simpleName.take(80), request.method.take(16), request.url.encodedPath.take(160))
                if (interceptorFailure.compareAndSet(null, failure))
                    Log.e("ZerusApprovalUI", "interceptor ${failure.type} guard=${failure.guard} ${failure.method} ${failure.path}")
                Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(500).message("Synthetic fixture refusal")
                    .body("{\"error\":{\"message\":\"Synthetic fixture interceptor refused the request.\"}}".toResponseBody("application/json".toMediaType())).build()
            }
        }.build()
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        fun count() = synchronized(calls) { calls.size }
        fun open(name: String, source: String = "kimi_tool_approval", answerable: Boolean = true) {
            phase("open-begin:$name")
            val q = JSONObject().put("question_id", name).put("question_hash", "hash-$name").put("run_id", target.run)
                .put("conversation_id", target.conversation).put("source", source).put("approval", true).put("approval_choices", true)
                .put("can_answer", answerable).put("answer_unavailable_reason", "Synthetic read-only approval")
                .put("questions", JSONArray().put(JSONObject().put("id", "approval").put("question", "Review synthetic $name")
                    .put("body", "printf fixture # *literal* [not a link](https://example.test)").put("allow_other", true)
                    .put("other_label", "Reject with feedback").put("options", JSONArray()
                        .put(JSONObject().put("id", "1").put("label", "Approve once").put("scope", "once"))
                        .put(JSONObject().put("id", "2").put("label", "Approve for this session").put("description", "Exact command for this session only").put("scope", "broader"))
                        .put(JSONObject().put("id", "3").put("label", "Reject").put("scope", "once")))))
            pending = q
            synchronized(raw) { raw.put("pending_questions", JSONArray().put(q)) }
            main { model.refreshActivity() }
            node("Review synthetic $name")
            await("fresh approval $name") { main { model.activityVerified && model.conversationQuestions.singleOrNull()?.id == name } }
            phase("open-ready:$name")
        }
        fun reviewed() { await("three real visible review seconds") { button("Send answer").isEnabled } }
        fun expect(option: String? = null, text: String = "") {
            val q = checkNotNull(pending)
            check(expected.compareAndSet(null, JSONObject().put("id", q.string("question_id")).put("hash", q.string("question_hash"))
                .put("options", JSONArray(if (option == null) emptyList<String>() else listOf(option))).put("text", text)))
        }
        fun captureEarlySend(): EarlySend {
            val q = checkNotNull(pending)
            val id = q.string("question_id"); val hash = q.string("question_hash")
            val current = {
                main {
                    val card = model.conversationQuestions.singleOrNull()
                    model.selected?.target == target && model.activityVerified && card != null && card.id == id && card.hash == hash &&
                        card.run == target.run && card.conversation == target.conversation
                }
            }
            check(earlySend == null && current())
            val send = node("Send answer")
            check(send.refresh() && exactVisible(send, "Send answer"))
            return EarlySend(send, send.windowId, current)
        }
        fun earlyReview(name: String, checkEarly: () -> Unit) {
            val captured = captureEarlySend()
            earlySend = captured
            var resumedAt: Long? = null
            try {
                phase("early-home:$name")
                sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)
                await("owned Activity lost focus before $name") { !main { activity.hasWindowFocus() } }
                // Home/foreground uses the same actual Activity and production eligibility path as the lifecycle case.
                // The timestamp precedes resume, so a late resume or accessibility read cannot qualify as early.
                val resumeStart = SystemClock.elapsedRealtime()
                resumedAt = resumeStart
                phase("early-resume-begin:$name")
                targetContext.startActivity(Intent(targetContext, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
                await("same Activity regained focus for $name") { main { activity.hasWindowFocus() } }
                check(captured.current()) { "Early-review card identity changed after resume" }
                checkEarly()
                val elapsed = SystemClock.elapsedRealtime() - resumeStart
                check(elapsed >= 0 && elapsed < 3_000) { "Fixture missed the real early-review window: $name elapsedMs=$elapsed" }
            } finally {
                earlySend = null
                phase("early-end:$name:${resumedAt?.let { SystemClock.elapsedRealtime() - it }}")
            }
        }
        try {
            await("private model storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                val session = Session(target, "Approval fixture", "kimi", "idle", "", "", 0, raw)
                set(model, "setSessions", listOf(session)); set(model, "setSelected", session)
            }
            boundary("begin", "countdown_once")
            open("countdown")
            reveal("Approve once"); tap("Approve once")
            earlyReview("countdown") {
                phase("disabled-assert-begin:countdown")
                try {
                    check(!button("Send answer").isEnabled && count() == 0)
                } finally { phase("disabled-assert-end:countdown") }
            }
            reviewed(); expect("1"); tap("Send answer"); await("once receipt") { count() == 1 }
            cases += "countdown_once"
            boundary("complete", "countdown_once")

            boundary("begin", "broader_cancel")
            open("broader-cancel", "claude_tool_approval")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed(); tap("Send answer")
            node("Confirm this option?"); node("Exact command for this session only"); check(count() == 1)
            tap("Cancel"); await("cancelled confirmation") { visible("Confirm this option?") == null }; check(count() == 1)
            cases += "broader_cancel"
            boundary("complete", "broader_cancel")
            boundary("begin", "broader_confirm")
            reviewed(); tap("Send answer"); node("Confirm this option?"); expect("2"); tap("Confirm")
            await("broader receipt") { count() == 2 }; cases += "broader_confirm"
            boundary("complete", "broader_confirm")

            boundary("begin", "card_replacement")
            open("replacement")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed(); tap("Send answer"); node("Confirm this option?")
            open("replacement-new")
            await("replacement drops old confirmation") { visible("Confirm this option?") == null }
            reveal("Reject"); tap("Reject")
            earlyReview("replacement") { check(!button("Send answer").isEnabled && count() == 2) }
            reviewed(); expect("3"); tap("Send answer"); await("new card reject receipt") { count() == 3 }
            cases += "card_replacement"
            boundary("complete", "card_replacement")

            boundary("begin", "feedback_keyboard")
            open("feedback")
            val editor = checkNotNull(find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && it.isEditable })
            check(editor.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, Bundle().apply { putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, "Use a safer command") }))
            check(editor.performAction(AccessibilityNodeInfo.ACTION_FOCUS))
            earlyReview("feedback") {
                check(!button("Send answer").isEnabled)
                sendKeyDownUpSync(KeyEvent.KEYCODE_ENTER); check(count() == 3)
            }
            reviewed(); expect(text = "Use a safer command"); sendKeyDownUpSync(KeyEvent.KEYCODE_ENTER)
            await("one feedback receipt") { count() == 4 }; check(expected.get() == null)
            cases += "feedback_keyboard"
            boundary("complete", "feedback_keyboard")

            boundary("begin", "lifecycle_no_submit")
            open("background")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed()
            val lifecycleSend = captureEarlySend() // Capture the underlying card before its native modal obscures it.
            tap("Send answer"); node("Confirm this option?")
            // Real Activity/window lifecycle, not an injected foreground Boolean.
            check(earlySend == null && lifecycleSend.current())
            earlySend = lifecycleSend
            var lifecycleResumedAt: Long? = null
            try {
                phase("early-home:lifecycle")
                sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)
                await("Activity stopped") { !main { activity.hasWindowFocus() } }
                val resumeStart = SystemClock.elapsedRealtime()
                lifecycleResumedAt = resumeStart
                phase("early-resume-begin:lifecycle")
                targetContext.startActivity(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
                await("same Activity regained focus") { main { activity.hasWindowFocus() } }
                node("Review synthetic background"); await("background retires confirmation") { visible("Confirm this option?") == null }
                check(count() == 4 && !button("Send answer").isEnabled)
                val elapsed = SystemClock.elapsedRealtime() - resumeStart
                check(elapsed >= 0 && elapsed < 3_000) { "Fixture missed the real early-review window: lifecycle elapsedMs=$elapsed" }
            } finally {
                earlySend = null
                phase("early-end:lifecycle:${lifecycleResumedAt?.let { SystemClock.elapsedRealtime() - it }}")
            }
            cases += "lifecycle_no_submit"
            boundary("complete", "lifecycle_no_submit")

            boundary("begin", "readonly_stale_identity")
            open("readonly", answerable = false); node("Synthetic read-only approval")
            check(!button("Approve once").isEnabled && count() == 4)
            open("stale-identity"); reveal("Approve once"); tap("Approve once"); reviewed()
            synchronized(raw) { raw.put("run_id", "replacement-run") }
            tap("Send answer")
            await("fresh identity refusal") { main { !model.activityVerified || model.error.isNotBlank() || model.drafts.any { it.questionId == "stale-identity" && it.status == "failed" } } }
            check(count() == 4 && expected.get() == null)
            cases += "readonly_stale_identity"
            boundary("complete", "readonly_stale_identity")
            check(main { model.draft(target).text == "Keep unrelated composer" })
            check(synchronized(calls) { calls.map { it.string("request_id") }.distinct().size == 4 })
            screenshot("complete")
        } finally { main { model.back(); activity.finish() } }
    }
    companion object {
        val CASES = listOf("countdown_once", "broader_cancel", "broader_confirm", "card_replacement", "feedback_keyboard", "lifecycle_no_submit", "readonly_stale_identity")
    }
}
