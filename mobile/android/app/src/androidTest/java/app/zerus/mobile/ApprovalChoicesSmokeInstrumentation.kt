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
    // BEGIN STALE_OPTION_EVIDENCE state
    private var staleOptionFailureEvidence = "stale-option-evidence status=UNKNOWN outcome=NOT_REACHED"
    // END STALE_OPTION_EVIDENCE state
    private var touchTrace: OwnedWindowTouchTrace? = null
    private var staleSendEvidence = "stale-send status=UNKNOWN reason=not-completed"
    // BEGIN QUESTION_UI_PROBE fixture-report
    private var actionProbeReport: QuestionUiProbe.Report? = null
    private var actionProbeSession: QuestionUiProbe.Session? = null
    private fun actionProbeEvidence(): String = try {
        val report = checkNotNull(actionProbeReport)
        check(report.records.size <= QuestionUiProbe.MAX_RECORDS)
        val lines = report.records.map { "compose-action $it" }
        check(lines.all { it.length <= 1024 })
        val status = if (report.closed && report.fault == QuestionUiProbe.Fault.NONE) "observed" else "UNKNOWN"
        ("compose-action status=$status closed=${report.closed} fault=${report.fault} records=${lines.size} schema=1\n" +
            lines.joinToString("\n")).also { check(it.length <= 512 * 1024) }
    } catch (_: Throwable) { "compose-action status=UNKNOWN reason=result-unavailable" }
    private fun smoke(store: PrivateStore) {
        val owned = QuestionUiProbe.install()
        actionProbeSession = owned
        var primary: Throwable? = null
        try { smokeBody(store) }
        catch (error: Throwable) { primary = error; throw error }
        finally {
            try { actionProbeReport = main { QuestionUiProbe.close(owned) } }
            catch (cleanup: Throwable) {
                val original = primary
                if (original == null) throw cleanup else if (cleanup !== original) original.addSuppressed(cleanup)
            } finally { actionProbeSession = null }
        }
    }
    // END QUESTION_UI_PROBE fixture-report
    // BEGIN QUESTION_SEND_DIAGNOSTIC definition
    private val postStages = java.util.concurrent.atomic.AtomicIntegerArray(7)
    @Volatile private var postStagesUnknown = false
    private fun postStage(index: Int) {
        try { if (postStages.updateAndGet(index) { if (it < 128) it + 1 else 128 } == 128) postStagesUnknown = true }
        catch (_: Throwable) { postStagesUnknown = true }
    }
    private fun postEvidence(): String = try {
        val values = IntArray(7) { postStages.get(it) }
        val status = if (postStagesUnknown || values.any { it >= 128 }) "UNKNOWN" else "observed"
        "owned-post status=$status scoped=${values[0]} inspect=${values[1]} answerEntry=${values[2]} admitted=${values[3]} recorded=${values[4]} voice=${values[5]} other=${values[6]}"
    } catch (_: Throwable) { "owned-post status=UNKNOWN" }
    private fun failureText(value: String): String = try {
        val kind = when {
            value.isEmpty() -> "empty"
            value == "Answer not sent. The selected question changed before sending." -> "selected-question-changed"
            value == "Could not save the answer outcome. Its original request remains recoverable." -> "outcome-persist-failed"
            value == "The prompt cache expired before sending. Review your answer and confirm full context; nothing was sent." -> "cache-expired"
            value == "Answer delivery is unknown. Check the original receipt; nothing has been retried." -> "delivery-unknown"
            value.startsWith("Answer not sent. ") -> "answer-failed-unknown"
            else -> "unknown"
        }
        val prefix = value.take(4096)
        val digest = java.security.MessageDigest.getInstance("SHA-256").digest(prefix.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
        "k=$kind n=${value.length} p=${prefix.length} sha256Prefix=$digest"
    } catch (_: Throwable) { "k=UNKNOWN" }
    // END QUESTION_SEND_DIAGNOSTIC definition
    private data class EarlySend(private val initialNode: AccessibilityNodeInfo, val window: Int, val currentCard: () -> Boolean,
        val observation: (Int?) -> QuestionUiProbe.ContentSnapshot?,
        val currentReview: (QuestionUiProbe.ContentSnapshot, Long, Long) -> Boolean,
        val reviewState: (QuestionUiProbe.ContentSnapshot, Long, Long) -> QuestionUiProbe.EarlyReviewState) {
        // BEGIN EARLY_SEND_LIVE_ANCHOR state
        var node: AccessibilityNodeInfo = initialNode
            private set
        private var anchorPreparationAttempted = false
        fun prepareAnchor(acquire: () -> AccessibilityNodeInfo) {
            check(!anchorPreparationAttempted) { "Early-review Send anchor preparation repeated" }
            anchorPreparationAttempted = true
            node = acquire()
        }
        // END EARLY_SEND_LIVE_ANCHOR state
        private var resumeStart: Long? = null
        private var beforeGeneration: Int? = null
        private var reviewStart: Long? = null
        private var boundGeneration: Int? = null
        private var boundSnapshot: QuestionUiProbe.ContentSnapshot? = null
        var resumeEnd: Long? = null
            private set
        fun begin(resumeStart: Long) {
            check(this.resumeStart == null && resumeEnd == null)
            check(resumeStart >= 0 && resumeStart <= SystemClock.elapsedRealtime() && currentCard())
            beforeGeneration = checkNotNull(observation(null)).generation
            this.resumeStart = resumeStart
        }
        fun bindReview(): Boolean {
            check(reviewStart == null && resumeEnd == null && currentCard())
            val snapshot = observation(checkNotNull(beforeGeneration)) ?: return false
            val detail = snapshot.record.detail as QuestionUiProbe.Committed
            val start = checkNotNull(detail.reviewStartedAt)
            val now = SystemClock.elapsedRealtime()
            check(snapshot.generation > checkNotNull(beforeGeneration) && detail.eligible)
            check(checkNotNull(resumeStart) <= start && start <= now)
            val end = Math.addExact(start, 3_000)
            check(now < end) { "Fixture missed the real early-review window" }
            reviewStart = start
            boundGeneration = snapshot.generation
            boundSnapshot = snapshot
            resumeEnd = end
            return true
        }
        fun current(): Boolean {
            if (!currentCard()) return false
            if (reviewStart == null) return true
            val bound = checkNotNull(boundSnapshot)
            // BEGIN EARLY_REVIEW_FOCUS_WAIT current
            if (!awaitCurrentReview(bound)) return false
            // END EARLY_REVIEW_FOCUS_WAIT current
            return bound.generation == checkNotNull(boundGeneration) &&
                currentReview(bound, SystemClock.elapsedRealtime(), checkNotNull(resumeEnd))
        }
        // BEGIN EARLY_REVIEW_FOCUS_WAIT observation
        private fun awaitCurrentReview(bound: QuestionUiProbe.ContentSnapshot): Boolean {
            val end = checkNotNull(resumeEnd)
            while (true) {
                if (!currentCard() || bound.generation != checkNotNull(boundGeneration)) return false
                val now = SystemClock.elapsedRealtime()
                check(now < end) { "Early-review deadline expired while awaiting eligibility" }
                when (reviewState(bound, now, end)) {
                    QuestionUiProbe.EarlyReviewState.INVALID -> return false
                    QuestionUiProbe.EarlyReviewState.CURRENT -> {
                        check(SystemClock.elapsedRealtime() < end) { "Early-review deadline expired after eligibility observation" }
                        return true
                    }
                    QuestionUiProbe.EarlyReviewState.WAIT -> {
                        val remaining = end - SystemClock.elapsedRealtime()
                        check(remaining > 0) { "Early-review deadline expired while ineligible" }
                        SystemClock.sleep(minOf(30L, remaining))
                    }
                }
            }
        }
        // END EARLY_REVIEW_FOCUS_WAIT observation
        fun elapsed() = SystemClock.elapsedRealtime() - checkNotNull(reviewStart)
    }
    private var earlySend: EarlySend? = null
    private data class InterceptorFailure(val guard: String, val type: String, val method: String, val path: String)
    private val interceptorFailure = AtomicReference<InterceptorFailure?>(null)
    private fun requireHealthyFixture() {
        interceptorFailure.get()?.let { failure ->
            error("Fixture interceptor ${failure.type} at ${failure.guard}: ${failure.method} ${failure.path}")
        }
    }
    // BEGIN APPROVAL_PROGRESS definition
    private enum class ProgressStage { ON_START, STORE_BEGIN, STORE_READY, BOOTSTRAP_BEGIN,
        ACTIVITY_LAUNCH_BEGIN, ACTIVITY_LAUNCH_READY, BOOTSTRAP_READY, CASE_BEGIN, CASE_COMPLETE }
    private var progressSequence = 0
    /** Only fixed instrumentation-thread boundaries; never a UI callback, input authority or success result. */
    private fun progress(stage: ProgressStage, caseId: String = "-") {
        check(progressSequence in 0 until 32)
        check(if (stage in setOf(ProgressStage.CASE_BEGIN, ProgressStage.CASE_COMPLETE)) caseId in CASES else caseId == "-")
        val now = SystemClock.elapsedRealtime()
        check(now >= 0 && cases.size in 0..CASES.size)
        val sequence = ++progressSequence
        val record = "1|$sequence|${stage.name}|$caseId|${cases.size}|$now"
        check(record.length <= 160)
        sendStatus(1, Bundle().apply { putString("zerus_approval_progress", record) })
    }
    // END APPROVAL_PROGRESS definition
    private fun boundary(event: String, name: String) {
        check(event in setOf("begin", "complete") && name in CASES)
        Log.i("ZerusApprovalUI", "$event $name completed=${cases.size} elapsedMs=${SystemClock.elapsedRealtime()}")
        // BEGIN APPROVAL_PROGRESS case-boundary
        progress(if (event == "begin") ProgressStage.CASE_BEGIN else ProgressStage.CASE_COMPLETE, name)
        // END APPROVAL_PROGRESS case-boundary
    }
    private fun phase(name: String) {
        val record = "phase ${name.take(100)} elapsedMs=${SystemClock.elapsedRealtime()}".take(160)
        if (phases.size == 64) phases.removeFirst()
        phases.addLast(record)
        Log.i("ZerusApprovalUI", record)
    }
    // BEGIN LIFECYCLE_READ_DIAGNOSTIC definition
    private enum class LifecycleCheckpoint { RESUMED_MAIN_RETURNED, BODY_RESTORE_BEGIN, BODY_RESTORE_PASSED,
        CONFIRM_RETIRE_BEGIN, CONFIRM_RETIRE_PASSED, DISABLED_SEND_BEGIN, DISABLED_SEND_PASSED,
        PRIMARY_FAILURE_CAPTURE_BEGIN, FINALLY_BEGIN }
    private var lifecycleCheckpointCount = 0
    private var lifecycleCheckpointUnknown = false
    /** Fixed call-region observations only; logger IO is cooperative and never bounds the observed read. */
    private fun lifecycleCheckpoint(stage: LifecycleCheckpoint) {
        if (lifecycleCheckpointUnknown) return
        if (lifecycleCheckpointCount >= 64) {
            lifecycleCheckpointUnknown = true
            try { phase("lifecycle-checkpoint:UNKNOWN_CAP") } catch (_: Throwable) { }
            return
        }
        lifecycleCheckpointCount++
        try { phase("lifecycle-checkpoint:${stage.name}") }
        catch (_: Throwable) {
            lifecycleCheckpointUnknown = true
            try { phase("lifecycle-checkpoint:UNKNOWN_FAULT") } catch (_: Throwable) { }
        }
    }
    // END LIFECYCLE_READ_DIAGNOSTIC definition
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            // BEGIN APPROVAL_PROGRESS on-start
            progress(ProgressStage.ON_START)
            // END APPROVAL_PROGRESS on-start
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            // BEGIN APPROVAL_PROGRESS store-begin
            progress(ProgressStage.STORE_BEGIN)
            // END APPROVAL_PROGRESS store-begin
            val store = PrivateStore(targetContext)
            require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState())
            // BEGIN APPROVAL_PROGRESS store-ready
            progress(ProgressStage.STORE_READY)
            // END APPROVAL_PROGRESS store-ready
            smoke(store)
            requireHealthyFixture()
            check(cases == CASES)
            result.putString("approval_cases", cases.size.toString())
            result.putString("approval_case_ids", cases.joinToString(","))
            result.putString("stream", "Approval choices smoke passed: ${cases.joinToString(",")}; synthetic receipts only. ${touchResultEvidence().replace("\n", " | ")} | ${actionProbeEvidence().replace("\n", " | ")} | $staleSendEvidence\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            // BEGIN STALE_OPTION_EVIDENCE failure-stream
            try {
                val addition = "\n" + staleOptionFailureEvidence
                check(addition.length <= 25_600)
                val retained = failureEvidence + addition
                failureEvidence = retained
            } catch (_: Throwable) { /* Keep prior evidence and the original failure if diagnostic copying fails. */ }
            // END STALE_OPTION_EVIDENCE failure-stream
            result.putString("stream", "Approval choices smoke failed: ${error.stackTraceToString()}\n${phases.joinToString("\n")}\n$failureEvidence\n${touchResultEvidence()}\n$staleSendEvidence\n${actionProbeEvidence()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }

    /** Final memory snapshot works after teardown; never replace the original result with an observation failure. */
    private fun touchResultEvidence(): String = try {
        main { touchTrace?.evidence() ?: "owned-touch UNKNOWN not installed" }
    } catch (_: Throwable) { "owned-touch UNKNOWN result-capture-failed" }

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
    private fun button(label: String, visibleLabel: Rect? = null): AccessibilityNodeInfo {
        val cached = earlySend?.takeIf { label == "Send answer" }
        if (cached != null) {
            check(visibleLabel == null) { "Early-review Send cache is read-only" }
            return cachedSendButton(cached)
        }
        var current = node(label)
        val labelWindow = if (visibleLabel != null) {
            check(current.refresh())
            check(exactVisible(current, label))
            current.getBoundsInScreen(visibleLabel)
            val sourceWindow = current.windowId
            check(sourceWindow >= 0)
            sourceWindow
        } else null
        while (true) {
            check(current.refresh())
            if (labelWindow != null) check(current.windowId == labelWindow)
            if (current.isClickable || !current.isEnabled || current.parent == null) return current
            current = checkNotNull(current.parent)
        }
    }
    // BEGIN EARLY_SEND_LIVE_ANCHOR preparation
    private fun prepareEarlySendAnchor(cached: EarlySend) {
        cached.prepareAnchor {
            val end = checkNotNull(cached.resumeEnd)
            check(cached.current()) { "Early-review card identity changed before label acquisition" }
            check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired before label acquisition" }
            val fresh = checkNotNull(visible("Send answer")) { "Early-review live Send label is unavailable" }
            check(cached.current()) { "Early-review card identity changed after label acquisition" }
            check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired after label acquisition" }
            check(fresh.refresh()) { "Early-review live Send label is obsolete" }
            check(cached.window >= 0 && fresh.windowId == cached.window) { "Early-review live Send window changed" }
            check(fresh.packageName?.toString() == targetContext.packageName) { "Early-review live Send package changed" }
            check(fresh.isVisibleToUser) { "Early-review live Send label is not visible" }
            check(fresh.text?.toString() == "Send answer" || fresh.contentDescription?.toString() == "Send answer") {
                "Early-review live Send label changed"
            }
            check(cached.current()) { "Early-review card identity changed after label validation" }
            check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired after label validation" }
            fresh
        }
    }
    // END EARLY_SEND_LIVE_ANCHOR preparation
    private fun cachedSendButton(cached: EarlySend): AccessibilityNodeInfo {
        val end = checkNotNull(cached.resumeEnd)
        while (true) {
            check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired" }
            check(cached.current()) { "Early-review card identity changed" }
            check(cached.node.refresh() && cached.window >= 0 && cached.node.windowId == cached.window &&
                cached.node.packageName?.toString() == targetContext.packageName && exactVisible(cached.node, "Send answer")) {
                "Early-review Send node is stale"
            }
            var current = cached.node
            var staleAncestor = false
            for (depth in 0 until 16) {
                // The bound label was refreshed once above. Only a stale ancestor permits another bounded walk.
                if (depth > 0 && !current.refresh()) { staleAncestor = true; break }
                check(current.windowId == cached.window && current.packageName?.toString() == targetContext.packageName)
                check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired" }
                if (current.isClickable || !current.isEnabled) {
                    check(cached.current()) { "Early-review card identity changed" }
                    check(SystemClock.elapsedRealtime() < end) { "Early-review Send deadline expired" }
                    return current
                }
                current = checkNotNull(current.parent) { "Early-review Send action is missing" }
            }
            check(staleAncestor) { "Early-review Send ancestry exceeds its bound" }
            val remaining = end - SystemClock.elapsedRealtime()
            check(remaining > 0) { "Early-review Send deadline expired" }
            SystemClock.sleep(minOf(30L, remaining))
        }
    }
    private fun tap(label: String) {
        val visibleLabel = Rect()
        val action = button(label, visibleLabel)
        check(action.refresh())
        check(action.isEnabled && action.isClickable) { "Disabled action $label" }
        val bounds = Rect().also(action::getBoundsInScreen)
        check(!bounds.isEmpty && bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= targetContext.resources.displayMetrics.widthPixels && bounds.bottom <= targetContext.resources.displayMetrics.heightPixels)
        val parentBounds = Rect(bounds)
        check(!visibleLabel.isEmpty && visibleLabel.left >= 0 && visibleLabel.top >= 0 &&
            visibleLabel.right <= targetContext.resources.displayMetrics.widthPixels && visibleLabel.bottom <= targetContext.resources.displayMetrics.heightPixels)
        check(bounds.intersect(visibleLabel) && !bounds.isEmpty) { "Visible label has no action interior" }
        phase("touch-node:$label:w=${action.windowId}:v=${action.isVisibleToUser}:s=${action.isSelected}:c=${action.isChecked}")
        phase("touch-parent:$parentBounds")
        phase("touch-label:$visibleLabel:boundWindow=${action.windowId}")
        phase("touch-geometry:$bounds:x=${bounds.exactCenterX()}:y=${bounds.exactCenterY()}")
        val down = SystemClock.uptimeMillis()
        var traceToken: Int? = null
        if (label == "Send answer") try { traceToken = main { touchTrace?.begin(label, action.windowId, down, bounds.exactCenterX(), bounds.exactCenterY()) } }
        catch (_: Throwable) { touchTrace?.observationFailed() }
        try {
        phase("touch-down:$label")
        for (type in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), type, bounds.exactCenterX(), bounds.exactCenterY(), 0)
            event.source = InputDevice.SOURCE_TOUCHSCREEN
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
            if (type == MotionEvent.ACTION_DOWN) SystemClock.sleep(100)
        }
        phase("touch-up:$label")
        } finally {
            try { traceToken?.let { token -> main { touchTrace?.end(token) } } }
            catch (_: Throwable) { touchTrace?.observationFailed() }
        }
    }
    private fun reveal(label: String, disclosureLabel: String, captureDisclosure: Boolean = false): DisclosureScroll? {
        var disclosureScroll: DisclosureScroll? = null
        await("option $label") {
            if (visible(label) != null && !captureDisclosure) true else {
                // Keep the bound inner node when its label scrolls offscreen; never rebind to the history.
                val disclosure = disclosureScroll ?: bindDisclosureScroll(disclosureLabel).also { disclosureScroll = it }
                check(disclosureScrollReady(disclosure)) { "Bound disclosure is not in the active window" }
                if (captureDisclosure && visible(label) != null) true else {
                    disclosure.node.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                    false
                }
            }
        }
        return disclosureScroll
    }
    private data class DisclosureScroll(val node: AccessibilityNodeInfo, val window: Int)
    private fun bindDisclosureScroll(label: String): DisclosureScroll {
        val anchor = checkNotNull(visible(label)) { "Current disclosure anchor is not visible" }
        check(anchor.refresh() && exactVisible(anchor, label) &&
            anchor.packageName?.toString() == targetContext.packageName)
        val window = anchor.windowId
        check(window >= 0)
        var current = anchor
        var parents = 0
        while (true) {
            check(current.refresh() && current.windowId == window &&
                current.packageName?.toString() == targetContext.packageName)
            if (current.isScrollable) {
                check(current.isVisibleToUser)
                return DisclosureScroll(current, window)
            }
            check(parents++ < 16) { "Disclosure scroll ancestor is unknown" }
            current = checkNotNull(current.parent) { "Disclosure has no scroll ancestor" }
        }
    }
    private fun disclosureScrollReady(disclosure: DisclosureScroll): Boolean {
        val scroll = disclosure.node
        val window = disclosure.window
        check(scroll.refresh() && scroll.windowId == window && scroll.isScrollable &&
            scroll.packageName?.toString() == targetContext.packageName) { "Bound disclosure scroll is stale" }
        // The same node may be temporarily hidden while the dialog/Activity accessibility window settles.
        val active = uiAutomation.rootInActiveWindow ?: return false
        if (!active.refresh() || active.windowId != window ||
            active.packageName?.toString() != targetContext.packageName) return false
        val bounds = Rect().also(scroll::getBoundsInScreen)
        return scroll.isVisibleToUser && !bounds.isEmpty && bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= targetContext.resources.displayMetrics.widthPixels && bounds.bottom <= targetContext.resources.displayMetrics.heightPixels
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
        // BEGIN QUESTION_SEND_DIAGNOSTIC model
        evidenceLine(lines, "owned-failure error=${failureText(model.error)}")
        evidenceLine(lines, "owned-failure notice=${failureText(model.contextNotice)}")
        evidenceLine(lines, postEvidence())
        // END QUESTION_SEND_DIAGNOSTIC model
        evidenceLine(lines, "owned-card id=${question?.id?.take(48)} current=${question?.id == offered?.string("question_id") && question?.hash == offered?.string("question_hash")} run=${question?.run == target.run} conversation=${question?.conversation == target.conversation}")
        evidenceLine(lines, "owned-draft present=${draft != null} status=${draft?.status?.take(16)} inFlight=${draft?.let(model::answerInFlight)} answerable=${question?.canAnswer}")
        evidenceLine(lines, "owned-answer rows=${saved.size} option=$selected emptyText=${saved["approval"]?.text?.isEmpty()} complete=$complete nativeChoices=${question?.let(QuestionPolicies::nativeChoices)}")
        lines.joinToString("\n")
    }
    private fun set(model: ZerusViewModel, name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name }.apply { isAccessible = true }.invoke(model, value)
    }
    /** Called on main by the observer; fixed owned memory only, no accessibility/window-tree query. */
    private fun touchModelState(model: ZerusViewModel, target: Target, activity: MainActivity, offered: JSONObject?): String {
        val question = model.conversationQuestions.singleOrNull()
        check(model.drafts.size <= 8)
        val draft = question?.let { QuestionPolicies.savedDraft(model.drafts, target, it) }
        val id = question?.id?.takeIf { it in setOf("countdown", "broader-cancel", "replacement", "replacement-new", "feedback", "background", "readonly", "stale-identity") } ?: "UNKNOWN"
        val current = question != null && question.id == offered?.string("question_id") && question.hash == offered?.string("question_hash") &&
            question.run == target.run && question.conversation == target.conversation
        return "id=$id sel=${model.selected?.target == target} own=$current edit=${draft?.status == "editing"} v=${model.activityVerified} busy=${model.detailBusy} consent=${model.pendingQuestionSend != null} focus=${activity.hasWindowFocus()} resumed=${activity.lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.RESUMED)}"
    }
    private fun smokeBody(store: PrivateStore) {
        // BEGIN APPROVAL_PROGRESS bootstrap-begin
        progress(ProgressStage.BOOTSTRAP_BEGIN)
        // END APPROVAL_PROGRESS bootstrap-begin
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
                // BEGIN QUESTION_SEND_DIAGNOSTIC post
                postStage(0)
                // END QUESTION_SEND_DIAGNOSTIC post
                guard = "authorization"
                check(request.header("Authorization") == "Bearer fixture-token")
                guard = "request-json"
                val input = JSONObject(okio.Buffer().also { request.body?.writeTo(it) }.readUtf8())
                guard = "operation"
                // BEGIN QUESTION_SEND_DIAGNOSTIC operation
                postStage(when (input.string("operation")) { "inspect" -> 1; "answer" -> 2; "voice_profiles" -> 5; else -> 6 })
                // END QUESTION_SEND_DIAGNOSTIC operation
                val value = when (input.string("operation")) {
                    "inspect" -> synchronized(raw) { JSONObject(raw.toString()) }
                    "voice_profiles" -> JSONObject().put("schema", 1).put("computers", JSONArray())
                    "answer" -> {
                        guard = "answer-admission"
                        val proof = checkNotNull(expected.getAndSet(null)) { "Unadmitted answer" }
                        // BEGIN QUESTION_SEND_DIAGNOSTIC admitted
                        postStage(3)
                        // END QUESTION_SEND_DIAGNOSTIC admitted
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
                        // BEGIN QUESTION_SEND_DIAGNOSTIC recorded
                        postStage(4)
                        // END QUESTION_SEND_DIAGNOSTIC recorded
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
        // BEGIN APPROVAL_PROGRESS activity-launch-begin
        progress(ProgressStage.ACTIVITY_LAUNCH_BEGIN)
        // END APPROVAL_PROGRESS activity-launch-begin
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        // BEGIN APPROVAL_PROGRESS activity-launch-ready
        progress(ProgressStage.ACTIVITY_LAUNCH_READY)
        // END APPROVAL_PROGRESS activity-launch-ready
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
            // BEGIN STALE_OPEN_PHASE refresh-main-before
            val traceStaleOpen = name == "stale-identity"
            if (traceStaleOpen) try { phase("stale-open:REFRESH_MAIN_BEFORE") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE refresh-main-before
            main { model.refreshActivity() }
            // BEGIN STALE_OPEN_PHASE refresh-main-returned
            if (traceStaleOpen) try { phase("stale-open:REFRESH_MAIN_RETURNED") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE refresh-main-returned
            // BEGIN STALE_OPEN_PHASE title-before
            if (traceStaleOpen) try { phase("stale-open:TITLE_BEFORE") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE title-before
            node("Review synthetic $name")
            // BEGIN STALE_OPEN_PHASE title-returned
            if (traceStaleOpen) try { phase("stale-open:TITLE_RETURNED") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE title-returned
            // BEGIN STALE_OPEN_PHASE verified-before
            if (traceStaleOpen) try { phase("stale-open:VERIFIED_BEFORE") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE verified-before
            await("fresh approval $name") { main { model.activityVerified && model.conversationQuestions.singleOrNull()?.id == name } }
            // BEGIN STALE_OPEN_PHASE verified-returned
            if (traceStaleOpen) try { phase("stale-open:VERIFIED_RETURNED") } catch (_: Throwable) { }
            // END STALE_OPEN_PHASE verified-returned
            phase("open-ready:$name")
        }
        fun reviewed() { await("three real visible review seconds") { button("Send answer").isEnabled } }
        fun broaderDraftMatches(record: Boolean = false): Boolean = main {
                val question = checkNotNull(model.conversationQuestions.singleOrNull())
                val offered = checkNotNull(pending)
                if (!record) checkNotNull(QuestionPolicies.savedDraft(model.drafts, target, question))
                val draft = model.draft(target, question)
                val answer = JSONArray(draft.answers).objects().single()
                val selected = answer.getJSONArray("selected_option_ids")
                check(selected.length() == 1)
                val ids = setOf(selected.getString(0))
                val scope = QuestionPolicies.broaderChoice(question, mapOf("approval" to ids))
                if (record) {
                    phase("broader-card:id=${question.id}:hash=${question.hash}:runMatches=${question.run == target.run}:conversationMatches=${question.conversation == target.conversation}")
                    phase("broader-draft:status=${draft.status}:ids=${ids.joinToString()}:emptyText=${answer.string("text").isEmpty()}:verified=${model.activityVerified}:answerable=${question.canAnswer}:scope=${scope?.id}")
                }
                model.selected?.target == target && question.id == offered.string("question_id") && question.hash == offered.string("question_hash") &&
                    question.run == target.run && question.conversation == target.conversation && draft.target == target && draft.questionId == question.id && draft.questionHash == question.hash &&
                    draft.status == "editing" && answer.string("question_id") == "approval" && ids == setOf("2") && answer.string("text").isEmpty() &&
                    model.activityVerified && question.canAnswer && scope?.id == "2"
        }
        fun proveBroaderDraft() {
            check(broaderDraftMatches(true)) { "Broader Send must preserve its exact selected draft and card" }
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
            fun currentCard(): Boolean {
                val card = model.conversationQuestions.singleOrNull()
                return model.selected?.target == target && model.activityVerified && card != null && card.id == id && card.hash == hash &&
                    card.run == target.run && card.conversation == target.conversation
            }
            val current = { main { currentCard() } }
            check(earlySend == null && current())
            val send = node("Send answer")
            check(send.refresh() && exactVisible(send, "Send answer"))
            val observation = { afterGeneration: Int? -> main {
                check(currentCard())
                QuestionUiProbe.contentSnapshot(checkNotNull(actionProbeSession), checkNotNull(model.conversationQuestions.singleOrNull()), afterGeneration)
            } }
            val currentReview = { bound: QuestionUiProbe.ContentSnapshot, now: Long, end: Long -> main {
                check(currentCard())
                QuestionUiProbe.earlyReviewCurrent(checkNotNull(actionProbeSession),
                    checkNotNull(model.conversationQuestions.singleOrNull()), bound, now, end)
            } }
            // BEGIN EARLY_REVIEW_FOCUS_WAIT callback
            val reviewState = { bound: QuestionUiProbe.ContentSnapshot, now: Long, end: Long -> main {
                check(currentCard())
                QuestionUiProbe.classifyEarlyReview(checkNotNull(actionProbeSession),
                    checkNotNull(model.conversationQuestions.singleOrNull()), bound, now, end)
            } }
            // END EARLY_REVIEW_FOCUS_WAIT callback
            return EarlySend(send, send.windowId, current, observation, currentReview, reviewState)
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
                // Resume is only a lower bound: the real three-second clock starts in production's eligibility effect.
                val resumeStart = SystemClock.elapsedRealtime()
                resumedAt = resumeStart
                captured.begin(resumeStart)
                phase("early-resume-begin:$name")
                targetContext.startActivity(Intent(targetContext, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
                await("same Activity regained focus for $name") { main { activity.hasWindowFocus() } && captured.bindReview() }
                check(captured.current()) { "Early-review card identity changed after resume" }
                // BEGIN EARLY_SEND_LIVE_ANCHOR early-review
                prepareEarlySendAnchor(captured)
                // END EARLY_SEND_LIVE_ANCHOR early-review
                checkEarly()
                val elapsed = captured.elapsed()
                check(elapsed >= 0 && elapsed < 3_000) { "Fixture missed the real early-review window: $name elapsedMs=$elapsed" }
            } finally {
                earlySend = null
                phase("early-end:$name:${resumedAt?.let { SystemClock.elapsedRealtime() - it }}")
            }
        }
        var primaryFailure: Throwable? = null
        try {
            main {
                touchTrace = OwnedWindowTouchTrace(activity.window) { touchModelState(model, target, activity, pending) }
                touchTrace!!.install()
            }
            await("private model storage") { main { model.storageReady } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                set(model, "setConnections", listOf(Connection(target.connectionId, "Fixture", "https://relay.example.test", "fixture-token")))
                set(model, "setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
                val session = Session(target, "Approval fixture", "kimi", "idle", "", "", 0, raw)
                set(model, "setSessions", listOf(session)); set(model, "setSelected", session)
            }
            // BEGIN APPROVAL_PROGRESS bootstrap-ready
            progress(ProgressStage.BOOTSTRAP_READY)
            // END APPROVAL_PROGRESS bootstrap-ready
            boundary("begin", "countdown_once")
            open("countdown")
            reveal("Approve once", "Review synthetic countdown"); tap("Approve once")
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
            reveal("Approve for this session", "Review synthetic broader-cancel"); tap("Approve for this session"); reviewed(); proveBroaderDraft(); tap("Send answer")
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
            reveal("Approve for this session", "Review synthetic replacement"); tap("Approve for this session"); reviewed(); proveBroaderDraft(); tap("Send answer"); node("Confirm this option?")
            open("replacement-new")
            await("replacement drops old confirmation") { visible("Confirm this option?") == null }
            reveal("Reject", "Review synthetic replacement-new"); tap("Reject")
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
            val lifecycleDisclosure = checkNotNull(reveal("Approve for this session", "Review synthetic background", captureDisclosure = true))
            tap("Approve for this session"); reviewed()
            proveBroaderDraft()
            val lifecycleSend = captureEarlySend() // Capture the underlying card before its native modal obscures it.
            tap("Send answer"); node("Confirm this option?")
            // Real Activity/window lifecycle, not an injected foreground Boolean.
            check(earlySend == null && lifecycleSend.current())
            earlySend = lifecycleSend
            var lifecycleResumedAt: Long? = null
            try {
                val lifecycleActivity = activity
                main {
                    check(lifecycleActivity === activity && !activity.isFinishing && !activity.isDestroyed)
                    check(activity.lifecycle.currentState == androidx.lifecycle.Lifecycle.State.RESUMED)
                    phase("lifecycle-before-home:id=${System.identityHashCode(activity)}:state=${activity.lifecycle.currentState.name}:focus=${activity.hasWindowFocus()}")
                }
                phase("early-home:lifecycle")
                sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)
                // The open Confirm dialog already removes Activity focus; only onStop proves Home completed.
                await("Activity stopped") { main {
                    check(lifecycleActivity === activity && !activity.isFinishing && !activity.isDestroyed)
                    activity.lifecycle.currentState == androidx.lifecycle.Lifecycle.State.CREATED && !activity.hasWindowFocus()
                } }
                main { phase("lifecycle-stopped:id=${System.identityHashCode(activity)}:state=${activity.lifecycle.currentState.name}:focus=${activity.hasWindowFocus()}") }
                val resumeStart = SystemClock.elapsedRealtime()
                lifecycleResumedAt = resumeStart
                lifecycleSend.begin(resumeStart)
                phase("early-resume-begin:lifecycle")
                targetContext.startActivity(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
                await("same Activity regained focus") { main {
                    check(lifecycleActivity === activity && !activity.isFinishing && !activity.isDestroyed)
                    activity.lifecycle.currentState == androidx.lifecycle.Lifecycle.State.RESUMED && activity.hasWindowFocus()
                } && lifecycleSend.bindReview() }
                main { phase("lifecycle-resumed:id=${System.identityHashCode(activity)}:state=${activity.lifecycle.currentState.name}:focus=${activity.hasWindowFocus()}") }
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC resumed-main-returned
                lifecycleCheckpoint(LifecycleCheckpoint.RESUMED_MAIN_RETURNED)
                // END LIFECYCLE_READ_DIAGNOSTIC resumed-main-returned
                // Preserve legitimate inner scrolling: bind the same native card, not a now-offscreen top label.
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC body-restore-begin
                lifecycleCheckpoint(LifecycleCheckpoint.BODY_RESTORE_BEGIN)
                // END LIFECYCLE_READ_DIAGNOSTIC body-restore-begin
                await("same current card body restored") {
                    check(lifecycleSend.current()) { "Lifecycle card identity changed after resume" }
                    check(broaderDraftMatches(false)) { "Lifecycle broader draft changed after resume" }
                    disclosureScrollReady(lifecycleDisclosure)
                }
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC body-restore-passed
                lifecycleCheckpoint(LifecycleCheckpoint.BODY_RESTORE_PASSED)
                // END LIFECYCLE_READ_DIAGNOSTIC body-restore-passed
                // BEGIN EARLY_SEND_LIVE_ANCHOR lifecycle
                prepareEarlySendAnchor(lifecycleSend)
                // END EARLY_SEND_LIVE_ANCHOR lifecycle
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC disabled-send-begin
                lifecycleCheckpoint(LifecycleCheckpoint.DISABLED_SEND_BEGIN)
                // END LIFECYCLE_READ_DIAGNOSTIC disabled-send-begin
                check(count() == 4 && !button("Send answer").isEnabled)
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC disabled-send-passed
                lifecycleCheckpoint(LifecycleCheckpoint.DISABLED_SEND_PASSED)
                // END LIFECYCLE_READ_DIAGNOSTIC disabled-send-passed
                val elapsed = lifecycleSend.elapsed()
                check(elapsed >= 0 && elapsed < 3_000) { "Fixture missed the real early-review window: lifecycle elapsedMs=$elapsed" }
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC confirm-retire-begin
                lifecycleCheckpoint(LifecycleCheckpoint.CONFIRM_RETIRE_BEGIN)
                // END LIFECYCLE_READ_DIAGNOSTIC confirm-retire-begin
                await("background retires confirmation") { visible("Confirm this option?") == null }
                // BEGIN LIFECYCLE_READ_DIAGNOSTIC confirm-retire-passed
                lifecycleCheckpoint(LifecycleCheckpoint.CONFIRM_RETIRE_PASSED)
                // END LIFECYCLE_READ_DIAGNOSTIC confirm-retire-passed
            } finally {
                earlySend = null
                phase("early-end:lifecycle:${lifecycleResumedAt?.let { SystemClock.elapsedRealtime() - it }}")
            }
            cases += "lifecycle_no_submit"
            boundary("complete", "lifecycle_no_submit")

            boundary("begin", "readonly_stale_identity")
            open("readonly", answerable = false); node("Synthetic read-only approval")
            check(!button("Approve once").isEnabled && count() == 4)
            open("stale-identity"); reveal("Approve once", "Review synthetic stale-identity"); tap("Approve once"); reviewed()
            val (staleQuestion, staleDraft, staleComposer) = main {
                val question = checkNotNull(model.conversationQuestions.singleOrNull())
                val draft = checkNotNull(QuestionPolicies.savedDraft(model.drafts, target, question))
                val composer = checkNotNull(QuestionPolicies.savedDraft(model.drafts, target, null))
                check(model.selected?.target == target && model.activityVerified && !model.detailBusy &&
                    question.id == "stale-identity" && question.hash == "hash-stale-identity" && question.run == target.run &&
                    question.conversation == target.conversation && draft.status == "editing" && draft.requestId.isEmpty() &&
                    !model.answerInFlight(draft) && model.pendingQuestionSend == null && composer.text == "Keep unrelated composer")
                val answer = QuestionPolicies.savedAnswers(draft.answers).getValue("approval")
                check(answer.optionIds == listOf("1") && answer.text.isEmpty())
                check(!question.prompts.single().multi)
                check(QuestionPolicies.complete(question.prompts, mapOf("approval" to setOf("1")), emptyMap(), QuestionPolicies.formReplyText(question)))
                check(QuestionSendPolicies.bound(question, target) && QuestionSendPolicies.available(question, target, draft.answers))
                Triple(question, draft, composer)
            }
            val answerEntries = postStages.get(2)
            check(!postStagesUnknown && answerEntries == 4 && count() == 4 && expected.get() == null)
            val replacementTarget = target.copy(run = "replacement-run")
            fun staleInspectionCurrent(): Boolean = main {
                val question = model.conversationQuestions.singleOrNull()
                val draft = QuestionPolicies.savedDraft(model.drafts, target, staleQuestion)
                val composer = QuestionPolicies.savedDraft(model.drafts, target, null)
                val proven = activity.hasWindowFocus() && model.activityVerified && !model.detailBusy &&
                    model.selected?.target == replacementTarget && model.activity?.string("run_id") == replacementTarget.run &&
                    model.activity?.string("conversation_id") == target.conversation && question != null && question == staleQuestion &&
                    question.id == "stale-identity" && question.hash == "hash-stale-identity" && question.run == target.run &&
                    question.run != replacementTarget.run && question.conversation == target.conversation && draft == staleDraft &&
                    composer == staleComposer && !model.answerInFlight(staleDraft) && model.pendingQuestionSend == null &&
                    !QuestionSendPolicies.bound(question, replacementTarget) &&
                    !QuestionSendPolicies.available(question, replacementTarget, staleDraft.answers)
                proven
            } && !postStagesUnknown && postStages.get(2) == answerEntries && count() == 4 && expected.get() == null
            synchronized(raw) { raw.put("run_id", "replacement-run") }
            // Deliver the changed inspection through production; a server-only mutation is not a fresh UI observation.
            main { model.refreshActivity() }
            await("delivered replacement-run inspection") { staleInspectionCurrent() }
            check(earlySend == null && staleInspectionCurrent())
            // The production refresh may reset this card's inner disclosure viewport.
            reveal("Approve once", "Review synthetic stale-identity")
            check(earlySend == null && staleInspectionCurrent()) { "Known-stale disclosure reveal changed the inspected run, draft or answer count" }
            val ownedWindow = main {
                check(activity.hasWindowFocus())
                val root = checkNotNull(activity.window.decorView.createAccessibilityNodeInfo())
                try { root.windowId.also { check(it >= 0 && root.packageName?.toString() == targetContext.packageName) } }
                finally { root.recycle() }
            }
            val root = checkNotNull(uiAutomation.rootInActiveWindow)
            check(root.refresh() && root.windowId == ownedWindow && root.packageName?.toString() == targetContext.packageName)
            // BEGIN STALE_COMPOSE_ROLE definition
            // Compose reports the owner with its 48 dp minimum touch target; the role
            // child keeps its layout bounds, centred in that target on each short axis.
            fun touchTarget(child: Rect, owner: Rect): Boolean {
                val minimum = 48f * targetContext.resources.displayMetrics.density
                fun axis(innerStart: Int, innerEnd: Int, outerStart: Int, outerEnd: Int): Boolean {
                    val inner = innerEnd - innerStart
                    val outer = outerEnd - outerStart
                    if (inner == outer) return innerStart == outerStart
                    return inner < outer && kotlin.math.abs(outer - minimum) <= 1f &&
                        kotlin.math.abs((innerStart - outerStart) - (outerEnd - innerEnd)) <= 1
                }
                return !child.isEmpty && owner.contains(child) &&
                    axis(child.left, child.right, owner.left, owner.right) &&
                    axis(child.top, child.bottom, owner.top, owner.bottom)
            }
            // A disabled Material Button with children has the same shape: the owner
            // keeps the plain View class and click semantics, a role-only Button child
            // carries the role and a content-description child carries the label.
            var composeSendOwner: AccessibilityNodeInfo? = null
            var composeSendWitness: AccessibilityNodeInfo? = null
            fun composeStaleButtonAction(label: AccessibilityNodeInfo): AccessibilityNodeInfo? {
                val facts = mutableListOf<String>()
                fun fact(kind: String, index: Int, node: AccessibilityNodeInfo) {
                    // Diagnostic work cannot alter admission or replace its original failure.
                    try {
                        if (facts.size >= 48) return
                        val actions = node.actionList.joinToString(",") { it.id.toString() }.take(120)
                        facts += "stale-send-node kind=$kind index=$index class=${node.className?.toString()?.take(120)} visible=${node.isVisibleToUser} enabled=${node.isEnabled} clickable=${node.isClickable} checkable=${node.isCheckable} children=${node.childCount} actions=$actions bounds=${Rect().also(node::getBoundsInScreen)}"
                    } catch (_: Throwable) { }
                }
                fun roleOnlyButton(child: AccessibilityNodeInfo): Boolean =
                    child.className?.toString() == "android.widget.Button" && child.isVisibleToUser &&
                    child.text.isNullOrEmpty() && child.contentDescription.isNullOrEmpty() && child.childCount == 0 &&
                    !child.isCheckable && !child.isChecked && !child.isSelected &&
                    !child.isClickable && !child.isLongClickable && child.actionList.none {
                        it.id == AccessibilityNodeInfo.ACTION_CLICK || it.id == AccessibilityNodeInfo.ACTION_LONG_CLICK
                    }
                var accepted: AccessibilityNodeInfo? = null
                try {
                    check(staleInspectionCurrent()) { "Known-stale Send identity changed before role proof" }
                    check(label.refresh() && label.windowId == ownedWindow &&
                        label.packageName?.toString() == targetContext.packageName && exactVisible(label, "Send answer"))
                    val labelBounds = Rect().also(label::getBoundsInScreen)
                    check(!labelBounds.isEmpty)
                    val display = targetContext.resources.displayMetrics
                    val seen = mutableListOf<AccessibilityNodeInfo>()
                    val owners = mutableListOf<Pair<AccessibilityNodeInfo, AccessibilityNodeInfo>>()
                    var candidate: AccessibilityNodeInfo? = label
                    for (depth in 0 until 16) {
                        val owner = candidate ?: break
                        check(owner !in seen) { "Known-stale Send ancestry is ambiguous" }
                        seen += owner
                        check(owner.refresh() && owner.windowId == ownedWindow &&
                            owner.packageName?.toString() == targetContext.packageName)
                        fact("ancestor", depth, owner)
                        // A direct Button role stays with the original walker and its disabled check.
                        if (owner.className?.toString() == "android.widget.Button") return null
                        if (owner.className?.toString() == "android.view.View" && owner.isVisibleToUser &&
                            !owner.isEnabled && owner.isClickable) {
                            val ownerBounds = Rect().also(owner::getBoundsInScreen)
                            check(!ownerBounds.isEmpty && ownerBounds.left >= 0 && ownerBounds.top >= 0 &&
                                ownerBounds.right <= display.widthPixels && ownerBounds.bottom <= display.heightPixels &&
                                ownerBounds.contains(labelBounds)) { "Known-stale Send owner geometry is unproven" }
                            val children = owner.childCount
                            check(children in 1..32) { "Known-stale Send child enumeration is unknown or exhausted" }
                            val witnesses = mutableListOf<AccessibilityNodeInfo>()
                            for (index in 0 until children) {
                                val child = checkNotNull(owner.getChild(index)) { "Known-stale Send child is unknown" }
                                check(child.refresh() && child.windowId == ownedWindow &&
                                    child.packageName?.toString() == targetContext.packageName && child.parent == owner)
                                fact("child", index, child)
                                if (roleOnlyButton(child) && touchTarget(Rect().also(child::getBoundsInScreen), ownerBounds)) witnesses += child
                            }
                            check(owner.childCount == children) { "Known-stale Send child enumeration changed" }
                            check(witnesses.size <= 1) { "Known-stale Send role witness is ambiguous" }
                            if (witnesses.size == 1) owners += owner to witnesses.single()
                        }
                        candidate = owner.parent
                    }
                    check(candidate == null) { "Known-stale Send exceeded the action ancestor bound" }
                    check(owners.size <= 1) { "Known-stale Send owner is ambiguous" }
                    check(staleInspectionCurrent()) { "Known-stale Send identity changed after role proof" }
                    val found = owners.singleOrNull() ?: return null
                    check(found.first.refresh() && !found.first.isEnabled && found.first.isVisibleToUser && found.first.isClickable &&
                        found.first.actionList.none { it.id == AccessibilityNodeInfo.ACTION_CLICK } &&
                        found.first.windowId == ownedWindow && found.first.packageName?.toString() == targetContext.packageName)
                    check(found.second.refresh() && found.second.parent == found.first && roleOnlyButton(found.second))
                    if (composeSendOwner == null) {
                        composeSendOwner = found.first
                        composeSendWitness = found.second
                    } else check(composeSendOwner == found.first && composeSendWitness == found.second) {
                        "Known-stale Send owner or role witness changed"
                    }
                    accepted = found.first
                    return accepted
                } finally {
                    // Fixed bounded descriptors from this walk, emitted only on refusal. Log I/O is cooperative.
                    if (accepted == null) try {
                        for (line in facts) Log.i("ZerusApprovalUI", line.take(512))
                    } catch (_: Throwable) { /* Diagnostic failure never converts a refusal to admission. */ }
                }
            }
            // Compose UI 1.8.3 emits a role-only child for a merging selectable owner with children.
            // This is one fixed single-choice shape, not permission to use any disabled ancestor.
            var composeOptionOwner: AccessibilityNodeInfo? = null
            var composeOptionWitness: AccessibilityNodeInfo? = null
            var composeOptionBounds: Pair<Rect, Rect>? = null
            fun composeStaleOptionAction(label: AccessibilityNodeInfo): AccessibilityNodeInfo? {
                val facts = mutableListOf<String>()
                var factsOverflow = false
                fun fact(kind: String, index: Int, node: AccessibilityNodeInfo) {
                    // Diagnostic work cannot alter admission or replace its original failure.
                    try {
                        if (facts.size >= 48) { factsOverflow = true; return }
                        val type = node.className?.toString()?.take(120)?.map {
                            if (it.isLetterOrDigit() || it in ".$" || it == '_') it else '?'
                        }?.joinToString("") ?: "UNKNOWN"
                        val rect = Rect().also(node::getBoundsInScreen)
                        facts += "stale-option-node kind=$kind index=$index class=$type ownedWindow=${node.windowId == ownedWindow} ownedPackage=${node.packageName?.toString() == targetContext.packageName} visible=${node.isVisibleToUser} enabled=${node.isEnabled} checkable=${node.isCheckable} checked=${node.isChecked} children=${node.childCount} bounds=$rect"
                    } catch (_: Throwable) { factsOverflow = true }
                }
                fun roleOnly(child: AccessibilityNodeInfo): Boolean =
                    child.className?.toString() == "android.widget.RadioButton" && child.isVisibleToUser &&
                    child.text.isNullOrEmpty() && child.contentDescription.isNullOrEmpty() && child.childCount == 0 &&
                    !child.isCheckable && !child.isChecked && !child.isSelected &&
                    !child.isClickable && !child.isLongClickable && child.actionList.none {
                        it.id == AccessibilityNodeInfo.ACTION_CLICK || it.id == AccessibilityNodeInfo.ACTION_LONG_CLICK
                    }
                var accepted: AccessibilityNodeInfo? = null
                var directRole = false
                // BEGIN STALE_OPTION_EVIDENCE completed-state
                var diagnosticCompleted = false
                // END STALE_OPTION_EVIDENCE completed-state
                try {
                    check(staleInspectionCurrent()) { "Known-stale option identity changed before role proof" }
                    check(label.refresh() && label.windowId == ownedWindow &&
                        label.packageName?.toString() == targetContext.packageName && exactVisible(label, "Approve once"))
                    val labelBounds = Rect().also(label::getBoundsInScreen)
                    check(!labelBounds.isEmpty)
                    val seen = mutableListOf<AccessibilityNodeInfo>()
                    val owners = mutableListOf<Triple<AccessibilityNodeInfo, AccessibilityNodeInfo, Rect>>()
                    val witnessBounds = mutableMapOf<AccessibilityNodeInfo, Rect>()
                    var candidate: AccessibilityNodeInfo? = label
                    for (depth in 0 until 16) {
                        val owner = candidate ?: break
                        check(owner !in seen) { "Known-stale option ancestry is ambiguous" }
                        seen += owner
                        check(owner.refresh() && owner.windowId == ownedWindow &&
                            owner.packageName?.toString() == targetContext.packageName)
                        fact("ancestor", depth, owner)
                        // Retain the original direct-role admission, including its disabled check.
                        if (owner.className?.toString() == "android.widget.RadioButton") {
                            check(composeOptionOwner == null) { "Known-stale option role shape changed" }
                            directRole = true
                            // BEGIN STALE_OPTION_EVIDENCE delegate-complete
                            diagnosticCompleted = true
                            // END STALE_OPTION_EVIDENCE delegate-complete
                            return null
                        }
                        if (owner.className?.toString() == "android.view.View" && owner.isVisibleToUser &&
                            !owner.isEnabled && owner.isCheckable) {
                            val ownerBounds = Rect().also(owner::getBoundsInScreen)
                            val display = targetContext.resources.displayMetrics
                            check(!ownerBounds.isEmpty && ownerBounds.left >= 0 && ownerBounds.top >= 0 &&
                                ownerBounds.right <= display.widthPixels && ownerBounds.bottom <= display.heightPixels &&
                                ownerBounds.contains(labelBounds)) { "Known-stale option owner geometry is unproven" }
                            val children = owner.childCount
                            check(children in 1..32) { "Known-stale option child enumeration is unknown or exhausted" }
                            val witnesses = mutableListOf<Pair<AccessibilityNodeInfo, Rect>>()
                            for (index in 0 until children) {
                                val child = checkNotNull(owner.getChild(index)) { "Known-stale option child is unknown" }
                                check(child.refresh() && child.windowId == ownedWindow &&
                                    child.packageName?.toString() == targetContext.packageName)
                                val parent = checkNotNull(child.parent)
                                check(parent.refresh() && parent == owner && parent.windowId == ownedWindow &&
                                    parent.packageName?.toString() == targetContext.packageName)
                                if (child.className?.toString() == "android.widget.RadioButton") {
                                    fact("role-child", index, child)
                                    val childBounds = Rect().also(child::getBoundsInScreen)
                                    if (roleOnly(child) && touchTarget(childBounds, ownerBounds)) witnesses += child to childBounds
                                }
                            }
                            check(owner.childCount == children) { "Known-stale option child enumeration changed" }
                            check(witnesses.size <= 1) { "Known-stale option role witness is ambiguous" }
                            if (witnesses.size == 1) {
                                owners += Triple(owner, witnesses.single().first, Rect(ownerBounds))
                                witnessBounds[witnesses.single().first] = witnesses.single().second
                            }
                        }
                        candidate = owner.parent
                    }
                    check(candidate == null) { "Known-stale option exceeded the action ancestor bound" }
                    check(owners.size <= 1) { "Known-stale option owner is ambiguous" }
                    check(staleInspectionCurrent()) { "Known-stale option identity changed after role proof" }
                    val found = owners.singleOrNull()
                    if (found != null) {
                        check(found.first.refresh() && !found.first.isEnabled && found.first.isVisibleToUser &&
                            found.first.className?.toString() == "android.view.View" && found.first.isCheckable &&
                            found.first.windowId == ownedWindow && found.first.packageName?.toString() == targetContext.packageName)
                        check(found.second.refresh() && found.second.windowId == ownedWindow &&
                            found.second.packageName?.toString() == targetContext.packageName && found.second.parent == found.first && roleOnly(found.second))
                        check(label.refresh() && label.windowId == ownedWindow &&
                            label.packageName?.toString() == targetContext.packageName && exactVisible(label, "Approve once") &&
                            Rect().also(label::getBoundsInScreen) == labelBounds)
                        check(Rect().also(found.second::getBoundsInScreen) == witnessBounds[found.second] &&
                            Rect().also(found.first::getBoundsInScreen) == found.third)
                        val geometry = Rect(found.third) to Rect(labelBounds)
                        if (composeOptionOwner == null) {
                            check(composeOptionWitness == null && composeOptionBounds == null)
                            composeOptionOwner = found.first
                            composeOptionWitness = found.second
                            composeOptionBounds = geometry
                        } else check(composeOptionOwner == found.first && composeOptionWitness == found.second &&
                            composeOptionBounds == geometry) { "Known-stale option owner, role witness or label geometry changed" }
                    }
                    accepted = found?.first
                    // BEGIN STALE_OPTION_EVIDENCE return-complete
                    diagnosticCompleted = true
                    // END STALE_OPTION_EVIDENCE return-complete
                    return accepted
                } finally {
                    // BEGIN STALE_OPTION_EVIDENCE publish
                    // Copy only existing memory observations; never inspect a node during failure reporting.
                    try {
                        val rows = facts.take(48)
                        val unknown = !diagnosticCompleted || factsOverflow || facts.size > 48 || rows.any { it.length > 512 }
                        val status = if (unknown) "UNKNOWN" else "observed"
                        val outcome = when {
                            !diagnosticCompleted -> "UNKNOWN_INTERRUPTED"
                            directRole -> "DIRECT_ROLE_DELEGATED"
                            accepted != null -> "OWNER_ACCEPTED"
                            else -> "NO_OWNER"
                        }
                        val header = "stale-option-evidence status=$status outcome=$outcome completed=$diagnosticCompleted directRole=$directRole accepted=${accepted != null} rows=${rows.size}\n"
                        val marker = if (unknown) "stale-option-evidence UNKNOWN cap-or-fault\n" else ""
                        val captured = header + rows.joinToString("\n") { it.take(512) } + "\n" + marker
                        check(captured.length <= 25_600)
                        staleOptionFailureEvidence = captured
                    } catch (_: Throwable) {
                        staleOptionFailureEvidence = "stale-option-evidence status=UNKNOWN outcome=CAPTURE_FAULT"
                    }
                    // END STALE_OPTION_EVIDENCE publish
                    // Fixed bounded descriptors from this walk, emitted only on refusal. Log I/O is cooperative.
                    if (accepted == null && !directRole) try {
                        for (line in facts) Log.i("ZerusApprovalUI", line)
                        if (factsOverflow) Log.i("ZerusApprovalUI", "stale-option-node UNKNOWN descriptor-cap-or-fault")
                    } catch (_: Throwable) { /* Diagnostic failure never converts a refusal to admission. */ }
                }
            }
            // END STALE_COMPOSE_ROLE definition
            fun staleDisabledAction(label: AccessibilityNodeInfo, text: String): AccessibilityNodeInfo {
                val role = when (text) {
                    "Approve once" -> "android.widget.RadioButton"
                    "Send answer" -> "android.widget.Button"
                    else -> error("Unknown stale action label")
                }
                check(label.refresh() && label.windowId == ownedWindow && label.packageName?.toString() == targetContext.packageName && exactVisible(label, text))
                var candidate = label
                for (depth in 0 until 16) {
                    check(candidate.refresh() && candidate.windowId == ownedWindow && candidate.packageName?.toString() == targetContext.packageName)
                    if (candidate.className?.toString() == role) {
                        check(!candidate.isEnabled && candidate.isVisibleToUser) { "Known-stale $text $role must remain disabled" }
                        return candidate
                    }
                    // BEGIN STALE_COMPOSE_ROLE admission
                    if (depth == 0 && text == "Approve once") composeStaleOptionAction(label)?.let { return it }
                    if (depth == 0 && text == "Send answer") composeStaleButtonAction(label)?.let { return it }
                    // END STALE_COMPOSE_ROLE admission
                    candidate = checkNotNull(candidate.parent) { "Known-stale $text has no $role ancestor" }
                }
                error("Known-stale $text exceeded the action ancestor bound")
            }
            val choice = checkNotNull(find(root) { exactVisible(it, "Approve once") }) { "Stale native choice is unavailable" }
            val choiceAction = staleDisabledAction(choice, "Approve once")
            val choiceBounds = Rect().also(choiceAction::getBoundsInScreen)
            check(!choiceBounds.isEmpty)
            val label = checkNotNull(find(root) { exactVisible(it, "Send answer") }) { "Stale Send label is unavailable" }
            check(label.refresh() && label.windowId == ownedWindow && exactVisible(label, "Send answer"))
            val labelBounds = Rect().also(label::getBoundsInScreen)
            val action = staleDisabledAction(label, "Send answer")
            val bounds = Rect().also(action::getBoundsInScreen)
            val screen = targetContext.resources.displayMetrics
            check(!bounds.isEmpty && bounds.left >= 0 && bounds.top >= 0 && bounds.right <= screen.widthPixels && bounds.bottom <= screen.heightPixels)
            check(!labelBounds.isEmpty && labelBounds.left >= 0 && labelBounds.top >= 0 &&
                labelBounds.right <= screen.widthPixels && labelBounds.bottom <= screen.heightPixels)
            val actionBounds = Rect(bounds)
            check(bounds.intersect(labelBounds) && !bounds.isEmpty)
            check(label.refresh() && label.windowId == ownedWindow && exactVisible(label, "Send answer") &&
                staleDisabledAction(label, "Send answer") == action && staleInspectionCurrent())
            check(staleDisabledAction(choice, "Approve once") == choiceAction && Rect().also(choiceAction::getBoundsInScreen) == choiceBounds)
            check(Rect().also(label::getBoundsInScreen) == labelBounds && Rect().also(action::getBoundsInScreen) == actionBounds)
            val currentRoot = checkNotNull(uiAutomation.rootInActiveWindow)
            check(currentRoot.refresh() && currentRoot.windowId == ownedWindow && currentRoot.packageName?.toString() == targetContext.packageName)
            phase("stale-disabled-send:window=$ownedWindow:bounds=$bounds:answerEntry=$answerEntries")
            staleSendEvidence = "stale-send status=UNKNOWN reason=attempt-started"
            val down = SystemClock.uptimeMillis()
            var traceToken: Int? = null
            try { traceToken = main { touchTrace?.begin("Send answer", ownedWindow, down, bounds.exactCenterX(), bounds.exactCenterY()) } }
            catch (_: Throwable) { touchTrace?.observationFailed() }
            try {
                // This sole negative gesture deliberately targets a freshly proven disabled control; normal tap stays strict.
                for (type in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
                    val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), type, bounds.exactCenterX(), bounds.exactCenterY(), 0)
                    event.source = InputDevice.SOURCE_TOUCHSCREEN
                    try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
                    if (type == MotionEvent.ACTION_DOWN) SystemClock.sleep(100)
                }
            } finally {
                try { traceToken?.let { token -> main { touchTrace?.end(token) } } }
                catch (_: Throwable) { touchTrace?.observationFailed() }
            }
            requireHealthyFixture()
            check(staleInspectionCurrent()) { "Known-stale physical attempt changed the draft, identity or answer count" }
            check(count() == 4 && expected.get() == null && postStages.get(2) == answerEntries)
            staleSendEvidence = "stale-send status=observed inspectedRun=replacement-run cardRun=fixture-run card=stale-identity hash=hash-stale-identity binding=false completeAnswerAllowed=false choiceRole=android.widget.RadioButton sendRole=android.widget.Button nativeChoiceDisabled=true disabled=true physical=DOWN,UP holdMs=100 attempts=1 answerEntryBefore=$answerEntries answerEntryAfter=${postStages.get(2)} receipts=4 draft=unchanged composer=unchanged"
            cases += "readonly_stale_identity"
            boundary("complete", "readonly_stale_identity")
            check(main { model.draft(target).text == "Keep unrelated composer" })
            check(synchronized(calls) { calls.map { it.string("request_id") }.distinct().size == 4 })
            screenshot("complete")
        } catch (error: Throwable) {
            primaryFailure = error
            // BEGIN LIFECYCLE_READ_DIAGNOSTIC primary-failure-capture-begin
            lifecycleCheckpoint(LifecycleCheckpoint.PRIMARY_FAILURE_CAPTURE_BEGIN)
            // END LIFECYCLE_READ_DIAGNOSTIC primary-failure-capture-begin
            // Observe the still-owned Activity before the unchanged back/finish cleanup.
            try { failureEvidence = modelEvidence(model, target, activity, pending) } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            try { failureEvidence += "\n" + windowEvidence() } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            try { screenshot("failure") } catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }
            throw error
        } finally {
            // BEGIN LIFECYCLE_READ_DIAGNOSTIC finally-begin
            lifecycleCheckpoint(LifecycleCheckpoint.FINALLY_BEGIN)
            // END LIFECYCLE_READ_DIAGNOSTIC finally-begin
            var observerCleanup: Throwable? = null
            try { main { touchTrace?.close() } }
            catch (cleanup: Throwable) {
                observerCleanup = cleanup
                val original = primaryFailure
                if (original != null && cleanup !== original) original.addSuppressed(cleanup)
            }
            try { main { model.back(); activity.finish() } }
            catch (cleanup: Throwable) {
                val original = primaryFailure ?: observerCleanup
                if (original == null) throw cleanup else if (cleanup !== original) original.addSuppressed(cleanup)
            }
            if (primaryFailure == null) observerCleanup?.let { throw it }
        }
    }
    companion object {
        val CASES = listOf("countdown_once", "broader_cancel", "broader_confirm", "card_replacement", "feedback_keyboard", "lifecycle_no_submit", "readonly_stale_identity")
    }
}
