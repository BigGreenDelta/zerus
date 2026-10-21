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
    private var failureEvidence = "owned-window-evidence UNKNOWN not captured"
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
            result.putString("stream", "Approval choices smoke failed: ${error.stackTraceToString()}\n${phases.joinToString("\n")}\n$failureEvidence\n")
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
        check(action.refresh())
        check(action.isEnabled && action.isClickable) { "Disabled action $label" }
        val bounds = Rect().also(action::getBoundsInScreen)
        check(!bounds.isEmpty && bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= targetContext.resources.displayMetrics.widthPixels && bounds.bottom <= targetContext.resources.displayMetrics.heightPixels)
        phase("touch-node:$label:w=${action.windowId}:v=${action.isVisibleToUser}:s=${action.isSelected}:c=${action.isChecked}")
        phase("touch-geometry:$bounds:x=${bounds.exactCenterX()}:y=${bounds.exactCenterY()}")
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
    private fun evidenceLine(lines: MutableList<String>, text: String) {
        if (lines.size < 63) lines.add(text.take(160))
        else if (lines.size == 63) lines.add("owned-evidence UNKNOWN line-limit")
    }
    private fun diagnosticStage(lines: MutableList<String>, name: String, inspect: () -> Unit) {
        try { inspect() } catch (error: Throwable) {
            evidenceLine(lines, "owned-evidence UNKNOWN stage=$name type=${error.javaClass.simpleName.take(80)}")
        }
    }
    private fun ownedTreeEvidence(lines: MutableList<String>, root: AccessibilityNodeInfo, kind: String) {
        check(root.refresh() && root.packageName?.toString() == targetContext.packageName) { "owned-root" }
        val rootBounds = Rect(); root.getBoundsInScreen(rootBounds)
        evidenceLine(lines, "owned-root kind=$kind window=${root.windowId} focused=${root.isFocused} bounds=$rootBounds")
        val labels = setOf("Approve once", "Approve for this session", "Reject", "Send answer", "Confirm this option?", "Confirm", "Cancel",
            "Exact command for this session only", "printf fixture # *literal* [not a link](https://example.test)", "Scroll for options ↓",
            "Choose one of the agent's options.") + (1..3).map { "Review the request, then choose one of the agent's options ($it)." } +
            listOf("countdown", "broader-cancel", "replacement", "replacement-new", "feedback", "background", "readonly", "stale-identity").map { "Review synthetic $it" }
        var visited = 0; var matched = 0; var scrolls = 0
        fun visit(node: AccessibilityNodeInfo?, depth: Int) {
            check(node != null && depth <= 16 && ++visited <= 128 && node.refresh()) { "window-traversal" }
            check(node.packageName?.toString() == targetContext.packageName) { "window-package" }
            val rectangle = Rect(); node.getBoundsInScreen(rectangle)
            if (node.isScrollable) {
                check(++scrolls <= 4) { "window-scroll-count" }
                evidenceLine(lines, "owned-scroll kind=$kind window=${node.windowId} visible=${node.isVisibleToUser} bounds=$rectangle")
            }
            val label = sequenceOf(node.text?.toString(), node.contentDescription?.toString()).firstOrNull { it in labels }
            if (label != null) {
                check(++matched <= 12) { "window-label-count" }
                evidenceLine(lines, "owned-label kind=$kind index=$matched literal=$label")
                evidenceLine(lines, "owned-node kind=$kind index=$matched window=${node.windowId} visible=${node.isVisibleToUser} enabled=${node.isEnabled} selected=${node.isSelected} checked=${node.isChecked}")
                evidenceLine(lines, "owned-bounds kind=$kind index=$matched bounds=$rectangle")
            }
            for (index in 0 until node.childCount) visit(node.getChild(index), depth + 1)
        }
        visit(root, 0)
    }
    private fun windowEvidence(): String {
        val lines = mutableListOf<String>()
        diagnosticStage(lines, "active-root") {
            ownedTreeEvidence(lines, checkNotNull(uiAutomation.rootInActiveWindow), "active")
        }
        diagnosticStage(lines, "window-list") {
            val windows = uiAutomation.windows
            check(windows.size <= 4) { "window-count" }
            evidenceLine(lines, "owned-window-list count=${windows.size}")
            for (window in windows) diagnosticStage(lines, "listed-root") {
                val root = checkNotNull(window.root)
                if (root.packageName?.toString() == targetContext.packageName) {
                    val bounds = Rect(); window.getBoundsInScreen(bounds)
                    evidenceLine(lines, "owned-window id=${window.id} type=${window.type} active=${window.isActive} focused=${window.isFocused} bounds=$bounds")
                    ownedTreeEvidence(lines, root, "listed")
                }
            }
        }
        return lines.joinToString("\n")
    }
    private fun modelEvidence(model: ZerusViewModel, target: Target, activity: MainActivity, offered: JSONObject?): String = main {
        val question = model.conversationQuestions.singleOrNull()
        val draft = question?.let { QuestionPolicies.savedDraft(model.drafts, target, it) }
        val saved = QuestionPolicies.savedAnswers(draft?.answers.orEmpty())
        val options = saved.mapValues { it.value.optionIds.toSet() }
        val texts = saved.mapValues { it.value.text }
        val selected = saved["approval"]?.optionIds?.singleOrNull()?.takeIf { it in setOf("1", "2", "3") } ?: "missing-or-multiple"
        val complete = question?.let { QuestionPolicies.complete(it.prompts, options, texts, QuestionPolicies.formReplyText(it)) } ?: false
        val lines = mutableListOf<String>()
        evidenceLine(lines, "owned-model focused=${activity.hasWindowFocus()} lifecycle=${activity.lifecycle.currentState.name} selected=${model.selected?.target == target}")
        evidenceLine(lines, "owned-model verified=${model.activityVerified} detailBusy=${model.detailBusy} pendingConsent=${model.pendingQuestionSend != null}")
        evidenceLine(lines, "owned-card id=${question?.id?.take(48)} current=${question?.id == offered?.string("question_id") && question?.hash == offered?.string("question_hash")} run=${question?.run == target.run} conversation=${question?.conversation == target.conversation}")
        evidenceLine(lines, "owned-draft present=${draft != null} status=${draft?.status?.take(16)} inFlight=${draft?.let(model::answerInFlight)} answerable=${question?.canAnswer}")
        evidenceLine(lines, "owned-answer rows=${saved.size} option=$selected emptyText=${saved["approval"]?.text?.isEmpty()} complete=$complete nativeChoices=${question?.let(QuestionPolicies::nativeChoices)}")
        lines.joinToString("\n")
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
        val metadataCalls = mutableMapOf<String, Triple<Int, Long, Long>>() // count, first and last monotonic observation, two fixed keys only.
        var pending: JSONObject? = null
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            var guard = "request-scope"
            try {
                if (request.method == "GET") {
                    guard = "metadata-origin"
                    check(request.url.scheme == "https" && request.url.host == "relay.example.test" && request.url.port == 443 &&
                        request.url.encodedUsername.isEmpty() && request.url.encodedPassword.isEmpty() &&
                        request.url.encodedQuery == null && request.url.encodedFragment == null && request.body == null &&
                        request.header("Authorization") == "Bearer fixture-token")
                    guard = "metadata-path"
                    check(request.url.encodedPath in setOf("/v1/capabilities", "/v1/computers"))
                    synchronized(metadataCalls) {
                        val now = SystemClock.elapsedRealtime()
                        val previous = metadataCalls[request.url.encodedPath]
                        val count = (previous?.first ?: 0) + 1
                        check(count <= 64) { "Metadata observation bound" }
                        val first = previous?.second ?: now
                        metadataCalls[request.url.encodedPath] = Triple(count, first, now)
                        Log.i("ZerusApprovalUI", "metadata ${request.url.encodedPath} count=$count firstMs=$first lastMs=$now")
                    }
                    val capabilities = JSONObject().put("protocol_version", 1).put("operations", JSONArray(listOf("inspect", "answer"))).put("features", JSONArray())
                    val metadata = if (request.url.encodedPath == "/v1/capabilities") capabilities else {
                        val current = synchronized(raw) { JSONObject(raw.toString()) }
                        JSONObject().put("computers", JSONArray().put(JSONObject().put("id", target.computerId).put("name", "Fixture computer").put("online", true)
                            .put("snapshot", JSONObject().put("mobile_capabilities", capabilities).put("sessions", JSONArray().put(current)))))
                    }
                    return@addInterceptor Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("OK")
                        .body(metadata.toString().toResponseBody("application/json".toMediaType())).build()
                }
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
        fun receiptApplied(number: Int) {
            val payload = synchronized(calls) {
                check(calls.size == number)
                JSONObject(calls[number - 1].toString())
            }
            val requestId = payload.getString("request_id")
            val questionId = payload.getString("question_id")
            val questionHash = payload.getString("expected_question_hash")
            val answers = payload.getJSONArray("answers").toString()
            check(requestId.isNotBlank() && questionId.isNotBlank() && questionHash.isNotBlank() &&
                payload.string("expected_run_id") == target.run && payload.string("expected_conversation_id") == target.conversation)
            await("exact receipt applied $number") {
                main {
                    val question = model.conversationQuestions.singleOrNull()
                    val draft = model.drafts.singleOrNull { it.target == target && it.questionId == questionId && it.questionHash == questionHash }
                    val applied = model.selected?.target == target && question?.id == questionId && question?.hash == questionHash &&
                        question?.run == target.run && question?.conversation == target.conversation && draft != null &&
                        draft.target == target && draft.questionId == questionId && draft.questionHash == questionHash &&
                        draft.requestId == requestId && draft.answers == answers && draft.status == "submitted" && !model.answerInFlight(draft) &&
                        model.pendingQuestionSend == null && model.activityVerified && !model.detailBusy && expected.get() == null
                    applied
                }
            }
            phase("receipt-applied:$number:$questionId")
        }
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
        fun proveBroaderDraft() {
            main {
                val question = checkNotNull(model.conversationQuestions.singleOrNull())
                val offered = checkNotNull(pending)
                val draft = model.draft(target, question)
                val answer = JSONArray(draft.answers).objects().single()
                val selected = answer.getJSONArray("selected_option_ids")
                check(selected.length() == 1)
                val ids = setOf(selected.getString(0))
                val scope = QuestionPolicies.broaderChoice(question, mapOf("approval" to ids))
                phase("broader-card:id=${question.id}:hash=${question.hash}:runMatches=${question.run == target.run}:conversationMatches=${question.conversation == target.conversation}")
                phase("broader-draft:status=${draft.status}:ids=${ids.joinToString()}:emptyText=${answer.string("text").isEmpty()}:verified=${model.activityVerified}:answerable=${question.canAnswer}:scope=${scope?.id}")
                check(model.selected?.target == target && question.id == offered.string("question_id") && question.hash == offered.string("question_hash") &&
                    question.run == target.run && question.conversation == target.conversation && draft.target == target && draft.questionId == question.id && draft.questionHash == question.hash &&
                    draft.status == "editing" && answer.string("question_id") == "approval" && ids == setOf("2") && answer.string("text").isEmpty() &&
                    model.activityVerified && question.canAnswer && scope?.id == "2") { "Broader Send must preserve its exact selected draft and card" }
            }
        }
        fun proveRejectDraft() {
            val offered = checkNotNull(pending)
            await("exact selected Reject draft") {
                main {
                    val question = model.conversationQuestions.singleOrNull()
                    val draft = question?.let { QuestionPolicies.savedDraft(model.drafts, target, it) }
                    val answer = runCatching { JSONArray(draft?.answers.orEmpty()).objects().single() }.getOrNull()
                    val selected = answer?.optJSONArray("selected_option_ids")
                    val proven = question != null && question.id == offered.string("question_id") && question.hash == offered.string("question_hash") &&
                        question.run == target.run && question.conversation == target.conversation && model.selected?.target == target &&
                        draft != null && draft.target == target && draft.questionId == question.id && draft.questionHash == question.hash &&
                        draft.status == "editing" && draft.requestId.isEmpty() && !model.answerInFlight(draft) &&
                        answer != null && answer.opt("question_id") == "approval" && selected != null && selected.length() == 1 &&
                        selected.opt(0) == "3" && answer.opt("text") == "" && model.pendingQuestionSend == null &&
                        model.activityVerified && question.canAnswer && QuestionPolicies.nativeChoices(question)
                    proven
                }
            }
            phase("reject-draft:${offered.string("question_id")}:option=3:empty:editing:current")
        }
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
        var primaryFailure: Throwable? = null
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
            receiptApplied(1)
            cases += "countdown_once"
            boundary("complete", "countdown_once")

            boundary("begin", "broader_cancel")
            open("broader-cancel", "claude_tool_approval")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed(); proveBroaderDraft(); tap("Send answer")
            node("Confirm this option?"); node("Exact command for this session only"); check(count() == 1)
            tap("Cancel"); await("cancelled confirmation") { visible("Confirm this option?") == null }; check(count() == 1)
            cases += "broader_cancel"
            boundary("complete", "broader_cancel")
            boundary("begin", "broader_confirm")
            reviewed(); proveBroaderDraft(); tap("Send answer"); node("Confirm this option?"); expect("2"); tap("Confirm")
            await("broader receipt") { count() == 2 }
            receiptApplied(2)
            cases += "broader_confirm"
            boundary("complete", "broader_confirm")

            boundary("begin", "card_replacement")
            open("replacement")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed(); proveBroaderDraft(); tap("Send answer"); node("Confirm this option?")
            open("replacement-new")
            await("replacement drops old confirmation") { visible("Confirm this option?") == null }
            reveal("Reject"); tap("Reject")
            proveRejectDraft()
            earlyReview("replacement") { check(!button("Send answer").isEnabled && count() == 2) }
            proveRejectDraft()
            reviewed(); expect("3"); tap("Send answer"); await("new card reject receipt") { count() == 3 }
            receiptApplied(3)
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
            receiptApplied(4)
            cases += "feedback_keyboard"
            boundary("complete", "feedback_keyboard")

            boundary("begin", "lifecycle_no_submit")
            open("background")
            reveal("Approve for this session"); tap("Approve for this session"); reviewed()
            proveBroaderDraft()
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
        } catch (error: Throwable) {
            primaryFailure = error
            // Observe the still-owned Activity before the unchanged back/finish cleanup.
            try { failureEvidence = modelEvidence(model, target, activity, pending) } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            try { failureEvidence += "\n" + windowEvidence() } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            try { screenshot("failure") } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            throw error
        } finally {
            try { main { model.back(); activity.finish() } }
            catch (cleanup: Throwable) {
                val original = primaryFailure
                if (original == null) throw cleanup else if (cleanup !== original) original.addSuppressed(cleanup)
            }
        }
    }
    companion object {
        val CASES = listOf("countdown_once", "broader_cancel", "broader_confirm", "card_replacement", "feedback_keyboard", "lifecycle_no_submit", "readonly_stale_identity")
    }
}
