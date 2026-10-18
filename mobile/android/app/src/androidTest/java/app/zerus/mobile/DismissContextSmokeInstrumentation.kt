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
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import android.view.accessibility.AccessibilityWindowInfo
import android.view.inspector.WindowInspector
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.MotionDurationScale
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.layout.boundsInParent
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.ViewRootForTest
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsNode
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogWindowProvider
import kotlinx.coroutines.Job
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.SocketTimeoutException
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.coroutines.ContinuationInterceptor
import kotlin.coroutines.CoroutineContext

/** Actual context UI, ViewModel and private writer against an in-process synthetic relay only.
 * The same source compiles on the original base: its first acceptance assertion is the missing
 * Dismiss and clear action. Candidate-only entry points are reflected only after that assertion.
 */
class DismissContextSmokeInstrumentation : Instrumentation() {
    private var narrow = false
    private var fixtureCase = "all"
    private val target = Target("fixture-computer", "claude/fixture/context", "fixture-run", "fixture-conversation", "fixture-workspace")
    private val connection = Connection(target.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")
    private val feature = "clear_context_dismiss"
    private val configuration get() = targetContext.resources.configuration
    private val gateway = Gateway()
    private lateinit var activity: MainActivity
    private lateinit var model: ZerusViewModel
    private lateinit var store: PrivateStore
    private lateinit var retained: MessageState
    private lateinit var reads: ConversationReadState
    private lateinit var file: Attachment
    private lateinit var bytes: ByteArray
    private lateinit var writer: OrderedStatePersistence<MessageState>
    private lateinit var writeField: java.lang.reflect.Field
    private lateinit var originalWrite: suspend (MessageState) -> Unit
    private val nextWrite = AtomicReference<WriteGate?>(null)
    private val failureCaptured = AtomicBoolean()
    private val scene = mutableIntStateOf(0)
    private val completed = mutableListOf<String>()
    private val tapTrace = java.util.ArrayDeque<String>()
    private var scrollVersion = 0L
    private class WindowHook(val window: Window, val original: Window.Callback, val installed: Window.Callback, val label: AtomicReference<String>)
    private val windowHooks = mutableListOf<WindowHook>()
    private val dispatchLock = Any()
    private val dispatchTrace = java.util.ArrayDeque<String>()
    private var pendingSheetWindowId: Int? = null
    private class SceneToken(val generation: Int, val target: Target) {
        val publication = CountDownLatch(1)
        var capture: SceneCapture? = null
        var disposed = false
        var invalid = false
        val dialogRoots = mutableMapOf<Int, ViewRootForTest>()
    }
    private class SceneCapture(val token: SceneToken, val activityAnchor: View, val context: CoroutineContext,
        val scope: CoroutineScope, val scopeJob: Job)
    private var sceneToken: SceneToken? = null // Main-affine; a footer generation binds once.
    private val sceneCapture = AtomicReference<SceneCapture?>(null)
    private class ScrollSnapshot(val root: AccessibilityNodeInfo?, val windows: List<AccessibilityWindowInfo>)
    private class ScrollBinding(val capture: SceneCapture, val root: ViewRootForTest, val view: View,
        val provider: AccessibilityNodeProvider, val accessibility: AccessibilityNodeInfo, val id: Int, val layout: androidx.compose.ui.layout.LayoutInfo,
        val windowId: Int, val amount: Float, val callback: suspend (Offset) -> Offset,
        val sceneJob: Job, val sceneContext: CoroutineContext, val sceneGeneration: Int)

    override fun onCreate(arguments: Bundle?) {
        narrow = arguments?.getString("narrow") == "true"
        fixtureCase = arguments?.getString("case") ?: "all"
        super.onCreate(arguments); start()
    }
    override fun onStart() {
        val result = Bundle()
        try {
            val service = uiAutomation.serviceInfo
            service.flags = service.flags or android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
            uiAutomation.serviceInfo = service
            smoke()
            val scope = if (fixtureCase == "all") "checks passed" else "$fixtureCase diagnostic passed"
            val coverage = when (fixtureCase) {
                "frame" -> "actual ViewModel strict frame guards only; UI acceptance not exercised"
                "opener" -> "one accessibility opener activation and actual dialog cancel only; physical opener and full UI acceptance not exercised"
                else -> "actual footer/confirmation/review, private-save withdrawal guards and original receipt recovery"
            }
            result.putString("stream", "Dismiss context $scope: ${completed.joinToString()}; ${if (narrow) "actual280dp/font1.8" else "normal Android window"}; $coverage; original drafts/files/outbox/read state retained. Synthetic clear and inspect only; no native agent, provider or live network.\n")
            finish(Activity.RESULT_OK, result)
        } catch (failure: Throwable) {
            failureArtifacts()
            result.putString("stream", "Dismiss context ${if (fixtureCase == "all") "checks" else "$fixtureCase diagnostic"} failed after ${completed.joinToString()}: ${failure.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val value = AtomicReference<Result<T>>()
        runOnMainSync { value.set(runCatching(action)) }
        return value.get().getOrThrow()
    }
    private fun await(label: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < deadline) { "Timed out: $label" }
            SystemClock.sleep(30)
        }
    }
    private fun set(name: String, value: Any) {
        model.javaClass.declaredMethods.single { it.name == name && it.parameterTypes.size == 1 }
            .apply { isAccessible = true }.invoke(model, value)
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null || !root.refresh()) return null
        if (predicate(root)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), predicate)?.let { return it }
        return null
    }
    private fun label(node: AccessibilityNodeInfo) = node.text?.toString().orEmpty().ifBlank { node.contentDescription?.toString().orEmpty() }
    private fun matches(node: AccessibilityNodeInfo, text: String) = node.text?.toString() == text || node.contentDescription?.toString() == text
    private fun bounds(node: AccessibilityNodeInfo) = Rect().also(node::getBoundsInScreen)
    private fun nodeActions(node: AccessibilityNodeInfo) = JSONArray(node.actionList.take(32).map {
        JSONObject().put("id", it.id).put("label", it.label?.toString()?.take(128))
    })
    private fun nodeState(node: AccessibilityNodeInfo): String? = if (Build.VERSION.SDK_INT >= 30) node.stateDescription?.toString()?.take(256) else null
    private fun recordTrace(row: JSONObject) {
        tapTrace.addLast(row.toString())
        while (tapTrace.size > 32 || tapTrace.sumOf { it.length + 1 } > 128_000) tapTrace.removeFirst()
        File(targetContext.filesDir, "dismiss-context-${if (narrow) "narrow" else "normal"}-taps.jsonl").writeText(tapTrace.joinToString("\n", postfix = "\n"))
    }
    private fun installWindowDispatch(windowId: Int, text: String) = main {
        check(Build.VERSION.SDK_INT >= 29) { "Public WindowInspector diagnostics require API 29" }
        val candidates = mutableListOf(activity.window)
        val visited = Collections.newSetFromMap(java.util.IdentityHashMap<View, Boolean>())
        var count = 0
        fun visit(view: View) {
            if (count >= 400 || !visited.add(view)) return
            count++
            if (view is DialogWindowProvider && candidates.none { it === view.window }) candidates += view.window
            if (view is ViewGroup) for (index in 0 until view.childCount) {
                if (count >= 400) break
                visit(view.getChildAt(index))
            }
        }
        for (root in WindowInspector.getGlobalWindowViews()) {
            if (count >= 400) break
            if (root.context.packageName == targetContext.packageName) visit(root)
        }
        val window = candidates.singleOrNull { candidate ->
            val decor = candidate.peekDecorView()
            decor != null && decor.context.packageName == targetContext.packageName && decor.hasWindowFocus() &&
                decor.createAccessibilityNodeInfo()?.windowId == windowId
        } ?: error("No exact focused public own-window provider for accessibility window $windowId after $count bounded views")
        windowHooks.lastOrNull { it.window === window && window.callback === it.installed }?.let {
            it.label.set(text)
            return@main
        }
        val original = checkNotNull(window.callback) { "No original callback for owned window $windowId" }
        val label = AtomicReference(text)
        val wrapper = object : Window.Callback by original {
            override fun dispatchTouchEvent(event: MotionEvent): Boolean {
                val row = JSONObject().put("label", label.get()).put("window_id", windowId).put("action", event.actionMasked)
                    .put("source", event.source).put("tool_type", if (event.pointerCount > 0) event.getToolType(0) else JSONObject.NULL)
                    .put("pressure", if (event.pointerCount > 0) event.getPressure(0) else JSONObject.NULL)
                    .put("local_x", event.x).put("local_y", event.y).put("raw_x", event.rawX).put("raw_y", event.rawY)
                    .put("elapsed_ms", SystemClock.elapsedRealtime()).put("has_focus", window.decorView.hasWindowFocus())
                try {
                    val handled = original.dispatchTouchEvent(event)
                    row.put("delegate_result", handled)
                    return handled
                } catch (failure: Throwable) {
                    row.put("delegate_threw", failure.javaClass.simpleName)
                    throw failure
                } finally {
                    synchronized(dispatchLock) {
                        dispatchTrace.addLast(row.toString())
                        while (dispatchTrace.size > 128 || dispatchTrace.sumOf { it.length + 1 } > 64_000) dispatchTrace.removeFirst()
                    }
                }
            }
        }
        window.callback = wrapper
        windowHooks += WindowHook(window, original, wrapper, label)
    }
    private fun windowDispatchRows(): JSONArray = main {
        synchronized(dispatchLock) {
            JSONArray(dispatchTrace.map { JSONObject(it) }).also { dispatchTrace.clear() }
        }
    }
    private fun restoreWindowCallbacks() = main {
        for (hook in windowHooks.asReversed()) if (hook.window.callback === hook.installed) hook.window.callback = hook.original
        windowHooks.clear()
    }
    private fun awaitTitle(title: String, previousWindowId: Int? = null, requiredWindowId: Int? = null): Int {
        val deadline = SystemClock.elapsedRealtime() + 15_000
        while (SystemClock.elapsedRealtime() < deadline) {
            val root = uiAutomation.rootInActiveWindow
            if (root != null && root.packageName?.toString() == targetContext.packageName &&
                (previousWindowId == null || root.windowId != previousWindowId) &&
                (requiredWindowId == null || root.windowId == requiredWindowId)) {
                val window = uiAutomation.windows.singleOrNull { it.id == root.windowId }
                if (window?.isActive == true && window.isFocused &&
                    find(root) { it.isVisibleToUser && matches(it, title) } != null) return root.windowId
            }
            // Modal readiness is read-only: never scroll the previous window while its replacement appears.
            SystemClock.sleep(30)
        }
        error("Missing exact $title in the required owned active/focused window within 15 seconds (previous=$previousWindowId required=$requiredWindowId)")
    }
    private fun openDialog(opener: String, title: String, diagnosticClick: Boolean = false): Int {
        val before = checkNotNull(uiAutomation.rootInActiveWindow) { "No owned window before $opener" }
        check(before.packageName?.toString() == targetContext.packageName)
        val initialWindowId = before.windowId
        recordTrace(JSONObject().put("modal", "before").put("opener", opener).put("title", title).put("initial_window", initialWindowId))
        val tappedWindowId = tap(opener, diagnosticClick)
        val after = awaitTitle(title, previousWindowId = tappedWindowId)
        recordTrace(JSONObject().put("modal", "after").put("opener", opener).put("title", title)
            .put("initial_window", initialWindowId).put("old_window", tappedWindowId).put("new_window", after))
        return after
    }
    private class ScrollBudget(var accepted: Int = 0, var expansions: Int = 0, var attempts: Int = 0) {
        val used get() = attempts + expansions
        var capture: SceneCapture? = null
            private set
        fun pin(capture: SceneCapture) {
            check(Looper.myLooper() == Looper.getMainLooper()) { "Action capture pin must run on Main" }
            check(this.capture == null) { "Action capture cannot be rebound" }
            this.capture = capture
        }
    }
    private fun checkScrollMain() = check(Looper.myLooper() == Looper.getMainLooper()) { "Scroll proof must run on Main" }
    private fun checkScrollThread() = check(Looper.myLooper() != Looper.getMainLooper()) { "Never block Main awaiting a scroll" }
    private fun scrollDeadline(deadline: Long) = check(SystemClock.elapsedRealtime() < deadline) { "Original scroll/tap deadline expired" }
    private fun <T> scrollRead(deadline: Long, read: () -> T): T {
        checkScrollMain(); scrollDeadline(deadline)
        return read().also { scrollDeadline(deadline) }
    }
    private fun scrollSnapshot(deadline: Long): ScrollSnapshot {
        checkScrollThread(); scrollDeadline(deadline)
        val root = uiAutomation.rootInActiveWindow
        scrollDeadline(deadline)
        val windows = uiAutomation.windows
        scrollDeadline(deadline)
        return ScrollSnapshot(root, windows)
    }
    private fun currentScene(deadline: Long): SceneCapture {
        checkScrollMain(); scrollDeadline(deadline)
        val capture = checkNotNull(sceneCapture.get()) { "No committed same-scene context" }
        val token = capture.token
        check(sceneToken === token && scene.intValue == token.generation && token.capture === capture && !token.disposed && !token.invalid &&
            model.selected?.target == token.target && token.target == target) { "Scene or exact context target changed" }
        val scopeContext = capture.scope.coroutineContext
        check(scopeContext[Job] === capture.scopeJob && capture.scopeJob.isActive && capture.context[Job]?.isActive == true &&
            capture.context[MonotonicFrameClock] != null && capture.context[ContinuationInterceptor] != null &&
            scopeContext[MonotonicFrameClock] === capture.context[MonotonicFrameClock] &&
            scopeContext[ContinuationInterceptor] === capture.context[ContinuationInterceptor] &&
            scopeContext[MotionDurationScale] === capture.context[MotionDurationScale]) { "Captured effect elements or scene scope changed" }
        check(capture.activityAnchor.isAttachedToWindow && capture.activityAnchor.rootView === activity.window.decorView) {
            "Activity context anchor changed; it is not the separate dialog root"
        }
        return capture
    }
    private fun currentActionScene(deadline: Long, budget: ScrollBudget): SceneCapture {
        checkScrollMain()
        val capture = currentScene(deadline)
        check(capture === budget.capture) { "Pinned action scene changed" }
        return capture
    }
    private fun pinActionScene(deadline: Long, budget: ScrollBudget): SceneCapture {
        checkScrollMain()
        val capture = currentScene(deadline)
        if (budget.capture == null) budget.pin(capture)
        check(capture === budget.capture) { "Action capture cannot be rebound" }
        return capture
    }
    private fun awaitActionScene(deadline: Long, budget: ScrollBudget): SceneCapture {
        checkScrollThread()
        val gate = main {
            checkScrollMain(); scrollDeadline(deadline)
            check(budget.capture == null) { "Publication wait cannot replace a pinned action" }
            val expected = checkNotNull(sceneToken) { "No exact action scene token" }
            check(scene.intValue == expected.generation && expected.target == target && model.selected?.target == expected.target &&
                !expected.disposed && !expected.invalid) { "Action scene token or target changed" }
            val published = sceneCapture.get()
            check(published === expected.capture && (published == null || published.token === expected)) { "Foreign or lost publication" }
            expected to expected.publication
        }
        val remaining = deadline - SystemClock.elapsedRealtime()
        check(remaining > 0 && gate.second.await(remaining, TimeUnit.MILLISECONDS)) { "Exact scene publication exceeded the existing action deadline" }
        return main {
            val published = currentScene(deadline)
            check(published.token === gate.first) { "Another scene cannot satisfy this publication wait" }
            budget.pin(published)
            published
        }
    }
    private fun requireContextSheet(snapshot: ScrollSnapshot, windowId: Int, deadline: Long, requireTitle: Boolean) {
        checkScrollMain()
        val root = checkNotNull(snapshot.root) { "No active own sheet root" }
        check(scrollRead(deadline) { root.refresh() } && root.windowId == windowId && root.packageName?.toString() == targetContext.packageName)
        val window = snapshot.windows.singleOrNull { it.id == windowId } ?: error("Absent or ambiguous own sheet window")
        check(window.isActive && window.isFocused) { "Own sheet window lost focus" }
        // The title can leave the viewport after our scroll; only the first exact root binding grants this scope.
        if (!requireTitle) return
        var count = 0
        var titles = 0
        fun visit(node: AccessibilityNodeInfo, depth: Int) {
            check(++count <= 400 && depth <= 64) { "Unbounded own sheet title proof" }
            check(scrollRead(deadline) { node.refresh() } && node.windowId == windowId) { "Stale or foreign sheet title tree" }
            if (node.isVisibleToUser && matches(node, "Context and cache")) titles++
            val children = node.childCount
            check(children <= 400 - count) { "Unbounded own sheet children" }
            for (index in 0 until children) visit(checkNotNull(scrollRead(deadline) { node.getChild(index) }), depth + 1)
        }
        visit(root, 0)
        check(titles == 1) { "Awaited scrolling is limited to the actual Context and cache bottom sheet" }
    }
    private fun ownScrollRoot(windowId: Int, deadline: Long): ViewRootForTest {
        checkScrollMain()
        val roots = WindowInspector.getGlobalWindowViews()
        check(roots.size <= 16) { "Unbounded public own-window roots" }
        val visited = Collections.newSetFromMap(java.util.IdentityHashMap<View, Boolean>())
        val matches = mutableListOf<ViewRootForTest>()
        var count = 0
        fun visit(view: View, depth: Int) {
            check(++count <= 400 && depth <= 64 && visited.add(view)) { "Unbounded or repeated own-view path" }
            scrollDeadline(deadline)
            if (view is ViewRootForTest) {
                val backing = view.view
                val host = scrollRead(deadline) { backing.createAccessibilityNodeInfo() }
                if (host?.windowId == windowId && backing.isAttachedToWindow && backing.hasWindowFocus() && view.isLifecycleInResumedState)
                    matches += view
            }
            if (view is ViewGroup) {
                val children = view.childCount
                check(children <= 400 - count) { "Unbounded own-view children" }
                for (index in 0 until children) visit(view.getChildAt(index), depth + 1)
            }
        }
        for (root in roots) if (root.context.packageName == targetContext.packageName) visit(root, 0)
        return matches.singleOrNull() ?: error("Absent or ambiguous exact dialog ViewRootForTest")
    }
    private fun scrollBinding(scroller: AccessibilityNodeInfo, direction: Int, snapshot: ScrollSnapshot, deadline: Long,
        expected: ScrollBinding? = null, after: Boolean = false): ScrollBinding? {
        checkScrollMain()
        check(direction == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD || direction == AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)
        val refreshed = scrollRead(deadline) { scroller.refresh() }
        if (!after && refreshed && scroller.actionList.none { it.id == direction }) return null
        check(refreshed && scroller.isEnabled && scroller.isVisibleToUser && scroller.isScrollable)
        val capture = currentScene(deadline)
        val windowId = scroller.windowId
        requireContextSheet(snapshot, windowId, deadline, requireTitle = windowId !in capture.token.dialogRoots)
        var parent: AccessibilityNodeInfo? = scroller
        var depth = 0
        while (parent != null) {
            check(++depth <= 64) { "Unbounded nearest ancestor path" }
            val node = checkNotNull(parent)
            check(scrollRead(deadline) { node.refresh() } && node.isEnabled && node.isVisibleToUser && node.windowId == windowId)
            parent = scrollRead(deadline) { node.parent }
        }
        val key = "androidx.compose.ui.semantics.id"
        check(key in scroller.availableExtraData && scrollRead(deadline) { scroller.refreshWithExtraData(key, Bundle()) }) { "No exact public semantics ID" }
        val id = scroller.extras.get(key)
        check(id is Int) { "Missing or non-integer semantics ID" }
        val root = ownScrollRoot(windowId, deadline)
        val pinned = capture.token.dialogRoots[windowId]
        check(pinned == null || pinned === root) { "Exact dialog root was replaced" }
        check(expected == null || (expected.capture === capture && expected.root === root && expected.id == id &&
            expected.windowId == windowId && expected.accessibility == scroller)) { "Scroll identity changed" }
        val semantics = mutableListOf<SemanticsNode>()
        val ids = mutableSetOf<Int>()
        var nodes = 0
        fun visit(node: SemanticsNode, depth: Int) {
            check(++nodes <= 400 && depth <= 64 && ids.add(node.id)) { "Unbounded or ambiguous semantic tree" }
            if (node.id == id) semantics += node
            val children = node.children
            check(children.size <= 400 - nodes) { "Unbounded semantic children" }
            for (child in children) visit(child, depth + 1)
        }
        visit(root.semanticsOwner.unmergedRootSemanticsNode, 0)
        val node = semantics.singleOrNull() ?: error("No unique exact semantic ancestor")
        check(node.id != root.semanticsOwner.unmergedRootSemanticsNode.id && node.root === root && node.layoutInfo.isAttached &&
            !node.layoutInfo.isDeactivated && node.layoutInfo.semanticsId == node.id && node.layoutInfo.coordinates.isAttached)
        check(expected == null || expected.layout === node.layoutInfo) { "Exact semantic layout was replaced" }
        val backing = root.view
        val provider = checkNotNull(backing.accessibilityNodeProvider)
        check(expected == null || (expected.view === backing && expected.provider === provider)) { "Exact view or accessibility provider was replaced" }
        val provided = checkNotNull(scrollRead(deadline) { provider.createAccessibilityNodeInfo(id) })
        check(provided == scroller && provided.windowId == windowId) { "Provider does not prove the retained exact a11y ancestor" }
        val config = node.config
        check(!config.contains(SemanticsProperties.Disabled) && !config.contains(SemanticsProperties.HorizontalScrollAxisRange) &&
            !config.contains(SemanticsProperties.ProgressBarRangeInfo) && !config.contains(SemanticsActions.SetProgress) &&
            listOf(SemanticsActions.PageUp, SemanticsActions.PageDown, SemanticsActions.PageLeft, SemanticsActions.PageRight).none { config.contains(it) }) {
            "Ambiguous horizontal, page or progress action"
        }
        val range = checkNotNull(config.getOrNull(SemanticsProperties.VerticalScrollAxisRange))
        check(config.getOrNull(SemanticsActions.ScrollBy)?.action != null)
        val callback = checkNotNull(config.getOrNull(SemanticsActions.ScrollByOffset))
        val values = mutableListOf<Float>()
        val getViewport = config.getOrNull(SemanticsActions.GetScrollViewportLength)?.action
        val viewport = if (getViewport?.let { scrollRead(deadline) { it(values) } } == true) {
            check(values.size == 1) { "Invalid or ambiguous viewport callback" }
            values.single()
        } else node.layoutInfo.coordinates.boundsInParent().height
        check(viewport.isFinite() && viewport > 0f) { "Invalid exact delegate viewport" }
        val value = range.value()
        val maximum = range.maxValue()
        check(value.isFinite() && maximum.isFinite() && value >= 0f && maximum >= value)
        var amount = if (direction == AccessibilityNodeInfo.ACTION_SCROLL_FORWARD) viewport else -viewport
        if (range.reverseScrolling) amount = -amount
        if (!after) {
            check(scrollRead(deadline) { scroller.refresh() } && scroller.windowId == windowId && scroller.isEnabled && scroller.isVisibleToUser)
            if (scroller.actionList.none { it.id == direction }) return null
            check((amount < 0f && value > 0f) || (amount > 0f && value < maximum)) { "Exact semantic ancestor cannot scroll in the offered direction" }
        }
        if (pinned == null) {
            check(capture.token.dialogRoots.size < 8) { "Unbounded scene dialog bindings" }
            capture.token.dialogRoots[windowId] = root
        }
        return ScrollBinding(capture, root, backing, provider, AccessibilityNodeInfo.obtain(scroller), id, node.layoutInfo, windowId, amount, callback,
            capture.scopeJob, capture.context, capture.token.generation)
    }
    private fun scrollSynchronously(scroller: AccessibilityNodeInfo, direction: Int, deadline: Long, budget: ScrollBudget): Boolean {
        checkScrollThread()
        val before = scrollSnapshot(deadline)
        val binding = main {
            val capture = currentActionScene(deadline, budget)
            scrollBinding(scroller, direction, before, deadline).also {
                check(it == null || it.capture === capture) { "Scroll cannot adopt another action scene" }
            }
        } ?: return false
        val owned = Job(parent = binding.sceneJob)
        var listening = false
        val listener = object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(view: View) { checkScrollMain() }
            override fun onViewDetachedFromWindow(view: View) {
                checkScrollMain()
                if (view === binding.view) owned.cancel(CancellationException("Exact scroll root detached"))
            }
        }
        try {
            main {
                check(currentScene(deadline) === binding.capture && binding.view.isAttachedToWindow)
                binding.view.addOnAttachStateChangeListener(listener)
                listening = true
                check(binding.view.isAttachedToWindow) { "Exact scroll root detached during listener setup" }
            }
            val consumed = runBlocking(binding.sceneContext.minusKey(Job) + owned) {
                val remaining = deadline - SystemClock.elapsedRealtime()
                check(remaining > 0) { "No original scroll deadline remains" }
                withTimeout(remaining) {
                    ensureActive()
                    val fresh = scrollBinding(scroller, direction, before, deadline, expected = binding) ?: return@withTimeout null
                    check(fresh.amount == binding.amount && budget.used < 24) { "Viewport changed or shared scroll budget exhausted" }
                    check(currentActionScene(deadline, budget) === binding.capture) { "Action scene changed before scroll reservation" }
                    budget.attempts++ // Charge before the one direct callback; never detach or replay it.
                    scrollDeadline(deadline)
                    ensureActive()
                    fresh.callback(Offset(0f, fresh.amount)).also {
                        check(it.x.isFinite() && it.y.isFinite() && it.x == 0f && it.y != 0f && (it.y > 0f) == (fresh.amount > 0f)) {
                            "Scroll returned zero, malformed or wrong-direction consumption"
                        }
                    }
                }
            }
            if (consumed == null) return false // Direction withdrawal preserves the bounded label wait without an action.
            val after = scrollSnapshot(deadline)
            main { checkNotNull(scrollBinding(scroller, direction, after, deadline, expected = binding, after = true)) }
            recordTrace(JSONObject().put("scroll_completion", "exact-public-ScrollByOffset").put("scene", binding.sceneGeneration)
                .put("window_id", binding.windowId).put("semantics_id", binding.id).put("requested_y", binding.amount)
                .put("consumed_y", consumed.y).put("charged_scrolls", budget.attempts).put("budget_used", budget.used))
            return true
        } finally {
            try {
                owned.cancel() // A plain Job does not complete merely because its child returned.
                runBlocking(NonCancellable) { owned.join() }
            } finally {
                // Independent non-cancelled cleanup still reaches Main after scene/timeout/post-check failure.
                runBlocking(NonCancellable) {
                    withContext(binding.sceneContext.minusKey(Job)) {
                        checkScrollMain()
                        if (listening) binding.view.removeOnAttachStateChangeListener(listener)
                    }
                }
            }
        }
    }
    private data class SheetEvidence(val owner: AccessibilityNodeInfo, val handleBounds: Rect, val ownerBounds: Rect,
        val expand: Boolean, val collapse: Boolean, val ownerIdentity: AccessibilityNodeInfo, val handleVisible: Boolean)
    private fun sheetEvidence(windowId: Int): SheetEvidence? {
        val root = uiAutomation.rootInActiveWindow ?: return null
        if (root.windowId != windowId || root.packageName?.toString() != targetContext.packageName) return null
        val window = uiAutomation.windows.singleOrNull { it.id == windowId }
        if (window?.isActive != true || !window.isFocused) return null
        if (find(root) { it.isVisibleToUser && matches(it, "Context and cache") } == null) return null
        val handle = find(root) { it.isVisibleToUser && matches(it, "Drag handle") } ?: return null
        var cursor: AccessibilityNodeInfo? = handle
        var owner: AccessibilityNodeInfo? = null
        var count = 0
        while (cursor != null) {
            check(count++ < 64) { "Unbounded sheet handle action path" }
            val current = checkNotNull(cursor)
            check(current.refresh() && current.isEnabled && current.isVisibleToUser && current.windowId == windowId) { "Stale, disabled or foreign sheet action path" }
            if (owner == null && current.actionList.any { it.id == AccessibilityNodeInfo.ACTION_EXPAND || it.id == AccessibilityNodeInfo.ACTION_COLLAPSE }) owner = current
            cursor = current.parent
        }
        val actionOwner = owner ?: return null
        check(handle.refresh() && actionOwner.refresh()) { "Stale refreshed sheet handle or action owner" }
        val expand = actionOwner.actionList.any { it.id == AccessibilityNodeInfo.ACTION_EXPAND }
        val collapse = actionOwner.actionList.any { it.id == AccessibilityNodeInfo.ACTION_COLLAPSE }
        check(!(expand && collapse)) { "Ambiguous sheet offers both Expand and Collapse" }
        return SheetEvidence(actionOwner, bounds(handle), bounds(actionOwner), expand, collapse, AccessibilityNodeInfo.obtain(actionOwner), handle.isVisibleToUser)
    }
    private fun retainedSheetEvidence(windowId: Int, original: SheetEvidence): SheetEvidence {
        val root = checkNotNull(uiAutomation.rootInActiveWindow) { "No owned root after the single sheet Expand" }
        val window = uiAutomation.windows.singleOrNull { it.id == windowId }
        check(root.windowId == windowId && root.packageName?.toString() == targetContext.packageName &&
            window?.isActive == true && window.isFocused && find(root) { it.isVisibleToUser && matches(it, "Context and cache") } != null) {
            "Owned sheet window or title changed after its single Expand"
        }
        val owner = original.owner
        check(owner == original.ownerIdentity && owner.refresh() && owner == original.ownerIdentity && owner.windowId == windowId &&
            owner.isEnabled && owner.isVisibleToUser) { "Retained sheet action owner failed identity, refresh, visibility or authority checks" }
        val handles = mutableListOf<AccessibilityNodeInfo>()
        var visited = 0
        fun descendant(node: AccessibilityNodeInfo, depth: Int) {
            check(visited++ < 400 && depth <= 64) { "Unbounded retained sheet owner descendants" }
            check(node.refresh() && node.windowId == windowId) { "Stale or foreign retained sheet descendant" }
            if (matches(node, "Drag handle")) handles += node
            for (index in 0 until node.childCount) descendant(checkNotNull(node.getChild(index)), depth + 1)
        }
        // Only descendants of the retained authoritative owner are eligible after our own Expand.
        for (index in 0 until owner.childCount) descendant(checkNotNull(owner.getChild(index)), 1)
        val handle = handles.singleOrNull() ?: error("Retained sheet owner has an absent or ambiguous exact Drag handle descendant")
        check(handle.refresh() && handle.windowId == windowId && matches(handle, "Drag handle")) { "Retained sheet handle failed refresh or exact identity" }
        var parent = handle.parent
        var depth = 0
        while (parent != null && parent != original.ownerIdentity) {
            check(depth++ < 64 && parent.refresh() && parent.windowId == windowId) { "Retained handle ancestry changed" }
            parent = parent.parent
        }
        check(parent != null && parent == original.ownerIdentity) { "Exact Drag handle is no longer a descendant of the retained owner" }
        check(owner.refresh() && owner == original.ownerIdentity && owner.windowId == windowId && owner.isEnabled && owner.isVisibleToUser &&
            handle.refresh() && handle.windowId == windowId) { "Retained owner or descendant changed before geometry sampling" }
        val expand = owner.actionList.any { it.id == AccessibilityNodeInfo.ACTION_EXPAND }
        val collapse = owner.actionList.any { it.id == AccessibilityNodeInfo.ACTION_COLLAPSE }
        check(!(expand && collapse)) { "Ambiguous retained sheet offers both Expand and Collapse" }
        // The descendant supplies geometry only; its visibility never grants action authority.
        return SheetEvidence(owner, bounds(handle), bounds(owner), expand, collapse, original.ownerIdentity, handle.isVisibleToUser)
    }
    private fun prepareSheet(windowId: Int, deadline: Long, budget: ScrollBudget) {
        var original: SheetEvidence? = null
        var previous: SheetEvidence? = null
        var quiet = true
        while (SystemClock.elapsedRealtime() < deadline) {
            if (quiet) {
                val remaining = deadline - SystemClock.elapsedRealtime()
                check(remaining > 0) { "No tap budget remains for sheet event quiet" }
                uiAutomation.waitForIdle(250, remaining)
                quiet = false
                continue
            }
            val retained = original
            val current = if (retained == null) sheetEvidence(windowId) else retainedSheetEvidence(windowId, retained)
            if (current != null && original == null && current.expand && !current.collapse) {
                check(budget.used < 24) { "No shared action budget remains for the offered sheet expansion" }
                recordTrace(JSONObject().put("sheet_setup", "offered-expand-once").put("window_id", windowId)
                    .put("handle_bounds", current.handleBounds.flattenToString()).put("owner_bounds", current.ownerBounds.flattenToString())
                    .put("actions", nodeActions(current.owner)).put("state_description", nodeState(current.owner)))
                val activeRoot = uiAutomation.rootInActiveWindow
                val activeWindow = uiAutomation.windows.singleOrNull { it.id == windowId }
                check(activeRoot?.windowId == windowId && activeRoot.packageName?.toString() == targetContext.packageName &&
                    activeWindow?.isActive == true && activeWindow.isFocused) { "Sheet window changed before its single offered Expand" }
                check(SystemClock.elapsedRealtime() < deadline && current.owner.refresh() && current.owner.isEnabled && current.owner.isVisibleToUser &&
                    current.owner.windowId == windowId && current.owner.actionList.any { it.id == AccessibilityNodeInfo.ACTION_EXPAND } &&
                    current.owner.actionList.none { it.id == AccessibilityNodeInfo.ACTION_COLLAPSE }) { "Fresh sheet no longer offers exclusive Expand" }
                main { currentActionScene(deadline, budget) }
                budget.expansions++
                check(current.owner.performAction(AccessibilityNodeInfo.ACTION_EXPAND)) { "The single offered sheet Expand was refused" }
                original = current
                scrollVersion++
                quiet = true
                previous = null
                continue
            }
            val before = original
            if (current != null && before != null && !current.expand && current.collapse &&
                current.ownerBounds.top < before.ownerBounds.top && current.handleBounds.top < before.handleBounds.top) {
                val prior = previous
                if (prior != null && prior.ownerBounds == current.ownerBounds && prior.handleBounds == current.handleBounds) {
                    check(SystemClock.elapsedRealtime() < deadline) { "Expanded sheet proof exceeded the original tap deadline" }
                    recordTrace(JSONObject().put("sheet_setup", "expanded-semantics-and-stable-geometry").put("window_id", windowId)
                        .put("original_owner_bounds", before.ownerBounds.flattenToString()).put("owner_bounds", current.ownerBounds.flattenToString())
                        .put("handle_bounds", current.handleBounds.flattenToString()).put("actions", nodeActions(current.owner))
                        .put("retained_owner_same_source", current.owner == before.ownerIdentity).put("handle_is_retained_owner_descendant", true)
                        .put("handle_visible", current.handleVisible)
                        .put("state_description", nodeState(current.owner)).put("sheet_expansions", budget.expansions)
                        .put("accepted_scrolls", budget.accepted).put("budget_used", budget.used))
                    pendingSheetWindowId = null
                    return
                }
                previous = current
            } else previous = null
            SystemClock.sleep(30)
        }
        error("Fresh sheet $windowId did not offer one Expand followed by Collapse-only stable expanded geometry within the original tap deadline")
    }
    private fun reveal(text: String, deadline: Long = SystemClock.elapsedRealtime() + 15_000, budget: ScrollBudget = ScrollBudget()): AccessibilityNodeInfo {
        main { pinActionScene(deadline, budget) }
        while (SystemClock.elapsedRealtime() < deadline) {
            val root = uiAutomation.rootInActiveWindow
            find(root) { it.isVisibleToUser && matches(it, text) }?.let { return it }
            val scroller = find(root) { it.isVisibleToUser && it.isScrollable }
            if (scroller == null) {
                SystemClock.sleep(30)
                continue
            }
            if (budget.used < 24 && scrollSynchronously(scroller, AccessibilityNodeInfo.ACTION_SCROLL_FORWARD, deadline, budget)) {
                budget.accepted++
                scrollVersion++
                waitForIdleSync()
            }
            // A rejected scroll can precede the fresh semantics for its final viewport.
            // Keep requiring the exact label within the same deadline after either result.
            SystemClock.sleep(30)
        }
        error("Missing $text within 15 seconds in actual context UI (${budget.accepted} bounded scrolls)")
    }
    private fun action(text: String, deadline: Long = SystemClock.elapsedRealtime() + 15_000, budget: ScrollBudget = ScrollBudget()): AccessibilityNodeInfo {
        var cursor = reveal(text, deadline, budget)
        while (true) {
            check(cursor.refresh()) { "Stale action owner for $text" }
            if (cursor.isClickable) break
            cursor = cursor.parent ?: error("No action owner for $text")
        }
        var path: AccessibilityNodeInfo? = cursor
        repeat(64) {
            val current = path ?: run {
                check(cursor.refresh() && cursor.isEnabled && cursor.isVisibleToUser && cursor.isClickable) { "Disabled or stale refreshed action owner for $text" }
                return cursor
            }
            check(current.refresh() && current.isEnabled && current.isVisibleToUser) { "Disabled or stale action path for $text" }
            path = current.parent
        }
        error("Unbounded action path for $text")
    }
    private fun disabledAction(text: String): AccessibilityNodeInfo {
        var cursor = reveal(text)
        while (true) {
            check(cursor.refresh() && cursor.isVisibleToUser) { "Stale or hidden disabled action owner for $text" }
            if (cursor.isClickable || cursor.className?.toString() == "android.widget.Button") break
            cursor = cursor.parent ?: error("No disabled action owner for $text")
        }
        check(!cursor.isEnabled) { "Expected the actual $text action owner to be disabled" }
        var path = cursor.parent
        repeat(64) {
            val current = path ?: return cursor
            check(current.refresh() && current.isEnabled && current.isVisibleToUser) { "Disabled or stale ancestor instead of the $text action owner" }
            path = current.parent
        }
        error("Unbounded disabled action path for $text")
    }
    private fun tap(text: String, diagnosticClick: Boolean = false): Int {
        check(!diagnosticClick || (fixtureCase == "opener" && text == "Dismiss and clear…")) { "Accessibility activation is confined to the explicit opener diagnostic" }
        val deadline = SystemClock.elapsedRealtime() + 15_000
        val density = targetContext.resources.displayMetrics.density
        val minimum = 48f * density
        val scrollBudget = ScrollBudget()
        if (text == "Context and cache details") awaitActionScene(deadline, scrollBudget)
        else main { pinActionScene(deadline, scrollBudget) }
        if (text != "Context and cache details") pendingSheetWindowId?.let { prepareSheet(it, deadline, scrollBudget) }
        var originalClipped: Rect? = null
        var previous: Rect? = null
        var previousWindowId: Int? = null
        var quietScrollVersion = -1L
        var quietWindowId = -1
        var quietCompletedAt = 0L
        var last = "No exact action owner"
        while (SystemClock.elapsedRealtime() < deadline) {
            val owner = action(text, deadline, scrollBudget)
            if (quietScrollVersion != scrollVersion || quietWindowId != owner.windowId) {
                val remaining = deadline - SystemClock.elapsedRealtime()
                check(remaining > 0) { "No time remains for event quiet before $text" }
                uiAutomation.waitForIdle(250, remaining)
                quietCompletedAt = SystemClock.elapsedRealtime()
                check(quietCompletedAt < deadline) { "Event quiet exceeded the existing tap deadline for $text" }
                quietScrollVersion = scrollVersion
                quietWindowId = owner.windowId
                previous = null
                previousWindowId = null
                // Reacquire the exact owner after asynchronous scroll/render events have gone quiet.
                continue
            }
            val path = JSONArray()
            val viewports = mutableListOf<Rect>()
            var nearestScroller: AccessibilityNodeInfo? = null
            var cursor: AccessibilityNodeInfo? = owner
            while (cursor != null) {
                check(path.length() < 64) { "Unbounded fresh tap path for $text" }
                val current = checkNotNull(cursor)
                check(current.refresh() && current.isEnabled && current.isVisibleToUser) { "Disabled or stale fresh tap path for $text" }
                val rectangle = bounds(current)
                path.put(JSONObject().put("class", current.className?.toString()?.take(128)).put("bounds", rectangle.flattenToString())
                    .put("enabled", current.isEnabled).put("visible", current.isVisibleToUser).put("clickable", current.isClickable).put("scrollable", current.isScrollable))
                if (current.isScrollable) {
                    viewports += rectangle
                    if (nearestScroller == null) nearestScroller = current
                }
                cursor = current.parent
            }
            // Ancestor refreshes can move a bottom sheet. Refresh the owner last before sampling it.
            check(owner.refresh() && owner.isClickable && owner.isEnabled && owner.isVisibleToUser) { "Disabled or stale final tap owner for $text" }
            val location = bounds(owner)
            val root = checkNotNull(uiAutomation.rootInActiveWindow) { "No current window for $text" }
            check(root.refresh()) { "Stale current window for $text" }
            val window = bounds(root)
            val ownedWindow = uiAutomation.windows.singleOrNull { it.id == owner.windowId && it.root?.packageName?.toString() == targetContext.packageName }
            val focusedWindow = ownedWindow?.isActive == true && ownedWindow.isFocused
            val focusedBounds = Rect().also { ownedWindow?.getBoundsInScreen(it) }
            val full = !location.isEmpty && location.width() >= minimum && location.height() >= minimum &&
                focusedWindow && owner.windowId == root.windowId && window.contains(location) && focusedBounds.contains(location) && viewports.all { it.contains(location) }
            last = "owner=$location window=${owner.windowId}/$window active=${ownedWindow?.isActive} focused=${ownedWindow?.isFocused} scrollViewports=$viewports density=$density"
            if (full && previous == location && previousWindowId == root.windowId) {
                val trace = JSONObject().put("label", text).put("owner", location.flattenToString()).put("density", density)
                    .put("window", window.flattenToString()).put("scroll_viewports", JSONArray(viewports.map { it.flattenToString() }))
                    .put("window_id", root.windowId).put("window_active", ownedWindow?.isActive).put("window_focused", ownedWindow?.isFocused)
                    .put("enabled_path", path).put("activation", if (diagnosticClick) "diagnostic-accessibility-once" else "physical")
                    .put("event_quiet_requirement_ms", 250).put("quiet_completed_elapsed_ms", quietCompletedAt).put("quiet_scroll_version", quietScrollVersion)
                    .put("accepted_scrolls", scrollBudget.accepted)
                    .put("sheet_expansions", scrollBudget.expansions).put("budget_used", scrollBudget.used)
                    .put("source", if (diagnosticClick) JSONObject.NULL else InputDevice.SOURCE_TOUCHSCREEN)
                    .put("tool_type", if (diagnosticClick) JSONObject.NULL else MotionEvent.TOOL_TYPE_FINGER)
                    .put("pressure", if (diagnosticClick) JSONObject.NULL else 1f).put("size", if (diagnosticClick) JSONObject.NULL else .1f)
                    .put("hold_ms", if (diagnosticClick) JSONObject.NULL else 40)
                recordTrace(trace)
                if (diagnosticClick) {
                    check(owner.refresh() && owner.isEnabled && owner.isVisibleToUser && owner.isClickable && bounds(owner) == location) { "Diagnostic opener owner changed before its single activation" }
                    main { currentActionScene(deadline, scrollBudget) }
                    check(owner.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Diagnostic opener accessibility activation was rejected" }
                    waitForIdleSync()
                    return root.windowId
                }
                installWindowDispatch(root.windowId, text)
                check(SystemClock.elapsedRealtime() < deadline) { "Window dispatch setup exceeded the existing tap deadline for $text" }
                val down = SystemClock.uptimeMillis()
                val actualEvents = JSONArray()
                try {
                    for (eventAction in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
                        // Start inside the original deadline, then finish the balanced UP cooperatively.
                        if (eventAction == MotionEvent.ACTION_UP) SystemClock.sleep(40)
                        val property = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
                        val coordinate = MotionEvent.PointerCoords().apply { x = location.exactCenterX(); y = location.exactCenterY(); pressure = 1f; size = .1f }
                        val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), eventAction, 1, arrayOf(property), arrayOf(coordinate),
                            0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
                        actualEvents.put(JSONObject().put("action", event.actionMasked).put("source", event.source).put("tool_type", event.getToolType(0))
                            .put("pressure", event.getPressure(0)).put("size", event.getSize(0)).put("x", event.getX(0)).put("y", event.getY(0)))
                        try {
                            if (eventAction == MotionEvent.ACTION_DOWN) main { currentActionScene(deadline, scrollBudget) }
                            if (eventAction == MotionEvent.ACTION_DOWN) check(SystemClock.elapsedRealtime() < deadline) { "Original deadline expired before DOWN for $text" }
                            check(uiAutomation.injectInputEvent(event, true)) { "Pointer injection failed for $text action=$eventAction at $location" }
                        }
                        finally { event.recycle() }
                    }
                } finally {
                    recordTrace(JSONObject().put("label", text).put("actual_events", actualEvents).put("window_dispatch", windowDispatchRows()))
                }
                waitForIdleSync()
                return root.windowId
            }
            val scroller = nearestScroller
            if (!full && focusedWindow && owner.windowId == root.windowId && scroller != null &&
                scroller.refresh() && scroller.isEnabled && scroller.isVisibleToUser && scroller.isScrollable && scroller.windowId == owner.windowId) {
                val viewport = bounds(scroller)
                val visibleViewport = Rect(viewport)
                val viewportVisible = visibleViewport.intersect(window) && visibleViewport.intersect(focusedBounds)
                val horizontalFit = viewportVisible && location.width() >= minimum && location.left > visibleViewport.left && location.right < visibleViewport.right
                val topEdge = location.top <= visibleViewport.top
                val bottomEdge = location.bottom >= visibleViewport.bottom
                val verticalClip = horizontalFit && visibleViewport.height() >= minimum && (topEdge xor bottomEdge) &&
                    (location.height() < minimum || location.top < visibleViewport.top || location.bottom > visibleViewport.bottom)
                val proposedDirection = when {
                    verticalClip && bottomEdge && location.top > visibleViewport.top && location.top < visibleViewport.bottom -> AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
                    verticalClip && topEdge && location.bottom < visibleViewport.bottom && location.bottom > visibleViewport.top -> AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD
                    else -> null
                }
                val direction = proposedDirection?.takeIf { proposed -> scroller.actionList.any { it.id == proposed } }
                if (direction != null) {
                    if (originalClipped == null) {
                        originalClipped = Rect(location)
                        recordTrace(JSONObject().put("label", text).put("scroll_readiness", "original-clipped")
                            .put("original_owner", location.flattenToString()).put("viewport", visibleViewport.flattenToString())
                            .put("window_id", root.windowId).put("minimum_pixels", minimum).put("accepted_scrolls", scrollBudget.accepted))
                    }
                    if (scrollBudget.used < 24 && SystemClock.elapsedRealtime() < deadline && scrollSynchronously(scroller, direction, deadline, scrollBudget)) {
                        scrollBudget.accepted++
                        scrollVersion++
                        recordTrace(JSONObject().put("label", text).put("scroll_readiness", "accepted-nearest-ancestor-scroll")
                            .put("original_owner", originalClipped?.flattenToString()).put("clipped_owner", location.flattenToString())
                            .put("viewport", visibleViewport.flattenToString()).put("window_id", root.windowId)
                            .put("direction", direction).put("accepted_scrolls", scrollBudget.accepted))
                        previous = null
                        previousWindowId = null
                        waitForIdleSync()
                        SystemClock.sleep(30)
                        continue
                    }
                }
            }
            previous = if (full) Rect(location) else null
            previousWindowId = if (full) root.windowId else null
            SystemClock.sleep(30)
        }
        error("No stable full 48dp action bounds for $text within 15 seconds: $last")
    }
    private fun screenshot(name: String) {
        if (!::activity.isInitialized) return
        uiAutomation.takeScreenshot()?.let { image ->
            File(targetContext.getExternalFilesDir(null), name).outputStream().use { image.compress(Bitmap.CompressFormat.PNG, 100, it) }
            image.recycle()
        }
    }
    private fun failureArtifacts() {
        if (!::activity.isInitialized || !failureCaptured.compareAndSet(false, true)) return
        val name = "dismiss-context-${if (fixtureCase in setOf("frame", "opener")) "$fixtureCase-" else ""}${if (narrow) "narrow" else "normal"}-failure"
        runCatching { screenshot("$name.png") }
        runCatching { recordTrace(JSONObject().put("late_window_dispatch", windowDispatchRows())) }
        runCatching {
            val output = StringBuilder()
            var count = 0
            val windows = mutableSetOf<Int>()
            fun visit(node: AccessibilityNodeInfo?, depth: Int) {
                if (node == null || count >= 400 || depth > 64 || output.length >= 128_000) return
                count++
                val current = node.refresh()
                output.append(JSONObject().put("index", count).put("depth", depth).put("refreshed", current)
                    .put("class", node.className?.toString()?.take(128)).put("text", node.text?.toString()?.take(256))
                    .put("description", node.contentDescription?.toString()?.take(256)).put("bounds", bounds(node).flattenToString())
                    .put("enabled", node.isEnabled).put("clickable", node.isClickable).put("visible", node.isVisibleToUser)
                    .put("scrollable", node.isScrollable).put("children", node.childCount).put("actions", nodeActions(node))
                    .put("state_description", nodeState(node)).toString()).append('\n')
                if (current && depth < 64) for (index in 0 until node.childCount) {
                    if (count >= 400 || output.length >= 128_000) break
                    visit(node.getChild(index), depth + 1)
                }
            }
            fun ownWindow(root: AccessibilityNodeInfo?, metadata: JSONObject) {
                val owned = root ?: return
                if (owned.packageName?.toString() != targetContext.packageName || count >= 400 || output.length >= 128_000) return
                if (!windows.add(owned.windowId)) return
                output.append(metadata).append('\n')
                visit(owned, 0)
            }
            runCatching {
                val info = uiAutomation.serviceInfo
                info.flags = info.flags or android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                uiAutomation.serviceInfo = info
            }
            for (window in uiAutomation.windows) {
                if (count >= 400 || output.length >= 128_000) break
                val root = window.root
                if (root?.packageName?.toString() != targetContext.packageName) continue
                ownWindow(root, JSONObject().put("window", window.id).put("type", window.type).put("active", window.isActive)
                    .put("focused", window.isFocused).put("bounds", Rect().also(window::getBoundsInScreen).flattenToString()))
            }
            val root = uiAutomation.rootInActiveWindow
            ownWindow(root, JSONObject().put("window", root?.windowId).put("active_root_fallback", true))
            if (windows.isEmpty()) output.append("No owned fixture accessibility root is available.\n")
            output.append("Bounded owned fixture semantics: $count nodes across ${windows.size} windows; global maximum 400 nodes/64 depth/128000 characters.\n")
            File(targetContext.filesDir, "$name-semantics.txt").writeText(output.toString().take(128_000))
        }
    }
    private fun flush() = runBlocking { withTimeout(15_000) { writer.flush(); ConversationReadPersistence.get(targetContext).open().flush() } }
    private fun requestState(bodyIndex: Int, status: String): Boolean {
        val body = synchronized(gateway.bodies) { gateway.bodies.getOrNull(bodyIndex) } ?: return false
        val requestId = body.string("request_id")
        return main { model.contextOperations.any { it.requestId == requestId && it.target == target && it.status == status } } &&
            store.messageState().contextOperations.any { it.requestId == requestId && it.target == target && it.status == status }
    }
    private fun dismissFlag(operation: ContextOperation): Boolean? {
        val getter = operation.javaClass.declaredMethods.singleOrNull { it.name == "getDismissPrompt" && it.parameterTypes.isEmpty() }
            ?: error("The candidate did not retain dismissPrompt on the original context operation")
        return getter.invoke(operation) as Boolean?
    }
    private fun dismiss(): Job {
        val method = model.javaClass.declaredMethods.singleOrNull { it.name == "clearContext" && it.parameterTypes.size == 2 &&
            it.parameterTypes[0] == Target::class.java && it.parameterTypes[1] == java.lang.Boolean.TYPE }
            ?: error("The candidate has no explicit dismiss clear entry point")
        return main { method.invoke(model, target, true) as Job }
    }
    private fun replaceFrameTarget(foreign: Target) {
        val field = model.javaClass.getDeclaredField("activeFrame").apply { isAccessible = true }
        val verified = model.javaClass.getDeclaredField("activeFrameVerified").apply { isAccessible = true }
        val frame = checkNotNull(field.get(model)) { "Missing exact inspected frame" }
        val copy = frame.javaClass.declaredMethods.single { it.name == "copy\$default" }.apply { isAccessible = true }
        check(copy.parameterTypes.size in 4..34 && copy.parameterTypes[copy.parameterTypes.size - 2] == java.lang.Integer.TYPE)
        val arguments = Array<Any?>(copy.parameterTypes.size) { index ->
            val type = copy.parameterTypes[index]
            if (type.isPrimitive) java.lang.reflect.Array.get(java.lang.reflect.Array.newInstance(type, 1), 0) else null
        }
        arguments[0] = frame; arguments[1] = foreign
        // Kotlin's default-copy mask retains every original field except the first (target).
        arguments[arguments.size - 2] = -2
        val replacement = checkNotNull(copy.invoke(null, *arguments))
        val frameFields = frame.javaClass.declaredFields.filter { !java.lang.reflect.Modifier.isStatic(it.modifiers) }
        check(frameFields.any { it.name == "target" })
        for (member in frameFields) {
            member.isAccessible = true
            check(member.get(replacement) == if (member.name == "target") foreign else member.get(frame)) { "Frame injection changed ${member.name}" }
        }
        val raw = model.activity
        val activityVerified = model.activityVerified
        val frameVerified = verified.get(model)
        check(model.selected?.target == target && foreign != target)
        field.set(model, replacement)
        check(model.selected?.target == target && model.activity === raw && model.activityVerified == activityVerified && verified.get(model) == frameVerified)
    }
    private fun footer() {
        main {
            sceneToken?.publication?.countDown() // Wake only; old-token validation must still refuse.
            scene.intValue++
            val token = SceneToken(scene.intValue, checkNotNull(model.selected).target)
            sceneToken = token
            sceneCapture.set(null)
            activity.setContent {
                MaterialTheme(colorScheme = darkColorScheme()) {
                    Surface(Modifier.fillMaxSize()) {
                        Column(Modifier.fillMaxSize().systemBarsPadding().padding(8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                            Text("Synthetic context fixture", style = MaterialTheme.typography.labelSmall)
                            key(token.generation) {
                                checkScrollMain()
                                val context = rememberCompositionContext().effectCoroutineContext
                                val scope = rememberCoroutineScope()
                                val anchor = LocalView.current
                                SideEffect {
                                    checkScrollMain()
                                    if (sceneToken === token && scene.intValue == token.generation && !token.disposed) {
                                        val scopeJob = scope.coroutineContext[Job]
                                        val previous = token.capture
                                        if (scopeJob == null) {
                                            token.invalid = true
                                            token.publication.countDown()
                                        }
                                        else if (previous == null) {
                                            val capture = SceneCapture(token, anchor, context, scope, scopeJob)
                                            token.capture = capture
                                            if (!sceneCapture.compareAndSet(null, capture)) token.invalid = true
                                            token.publication.countDown()
                                        } else if (previous.scope !== scope || previous.scopeJob !== scopeJob ||
                                            previous.context !== context || previous.activityAnchor !== anchor) {
                                            token.invalid = true
                                            token.publication.countDown()
                                        }
                                    }
                                }
                                DisposableEffect(token, scope, anchor, context) {
                                    onDispose {
                                        checkScrollMain()
                                        token.disposed = true
                                        token.capture?.takeIf { it.scope === scope && it.context === context && it.activityAnchor === anchor }
                                            ?.let { sceneCapture.compareAndSet(it, null) }
                                        token.publication.countDown()
                                    }
                                }
                                model.selected?.let { ContextFooter(model, it) }
                            }
                        }
                    }
                }
            }
        }
        waitForIdleSync()
        pendingSheetWindowId = openDialog("Context and cache details", "Context and cache")
    }
    private fun confirmation(): Int {
        footer()
        // This assertion is intentionally first on both original and candidate builds.
        val windowId = openDialog("Dismiss and clear…", "Dismiss and clear?")
        val confirm = bounds(action("Dismiss and clear"))
        val cancel = bounds(action("Cancel"))
        val window = bounds(checkNotNull(uiAutomation.rootInActiveWindow))
        val minimum = 48f * targetContext.resources.displayMetrics.density
        check(confirm.width() >= minimum && confirm.height() >= minimum && cancel.width() >= minimum && cancel.height() >= minimum) {
            "Actual confirmation actions are smaller than 48dp: confirm=$confirm cancel=$cancel density=${targetContext.resources.displayMetrics.density}"
        }
        check(window.contains(confirm) && window.contains(cancel)) { "Actual confirmation actions escaped its Android window" }
        check(cancel.top >= confirm.top && (cancel.top != confirm.top || cancel.left >= confirm.right)) { "Cancellation lost its final action position" }
        if (narrow) check(configuration.screenWidthDp == 280 && kotlin.math.abs(configuration.fontScale - 1.8f) < .01f)
        return windowId
    }
    private fun confirmationCaption(dismissPrompt: Boolean): String {
        val prefix = if (dismissPrompt) "Dismiss the agent’s current prompt and clear the conversation context in " else "Clear the conversation context in "
        val caption = checkNotNull(find(uiAutomation.rootInActiveWindow) {
            it.isVisibleToUser && label(it).startsWith(prefix) && label(it).endsWith("Your unsent draft is kept.")
        }) { "Missing the captured ${if (dismissPrompt) "dismiss" else "ordinary"} confirmation caption" }
        return label(caption).also { check(it.contains(target.session) && it.contains("Fixture computer")) }
    }
    private fun disabledConfirmation(dismissPrompt: Boolean, caption: String, windowId: Int) {
        val title = if (dismissPrompt) "Dismiss and clear?" else "Clear context?"
        val otherTitle = if (dismissPrompt) "Clear context?" else "Dismiss and clear?"
        val confirmLabel = if (dismissPrompt) "Dismiss and clear" else "Clear context"
        awaitTitle(title, requiredWindowId = windowId)
        check(find(uiAutomation.rootInActiveWindow) { it.isVisibleToUser && matches(it, otherTitle) } == null) { "Open confirmation changed its captured mode" }
        check(confirmationCaption(dismissPrompt) == caption) { "Open confirmation changed its captured caption" }
        val confirm = bounds(disabledAction(confirmLabel))
        val cancel = bounds(action("Cancel"))
        val window = bounds(checkNotNull(uiAutomation.rootInActiveWindow))
        val minimum = 48f * targetContext.resources.displayMetrics.density
        check(confirm.width() >= minimum && confirm.height() >= minimum && cancel.width() >= minimum && cancel.height() >= minimum) {
            "Actual disabled confirmation actions are smaller than 48dp: confirm=$confirm cancel=$cancel density=${targetContext.resources.displayMetrics.density}"
        }
        check(window.contains(confirm) && window.contains(cancel)) { "Actual disabled confirmation actions escaped its Android window" }
        check(cancel.top >= confirm.top && (cancel.top != confirm.top || cancel.left >= confirm.right)) { "Cancellation lost its final disabled-confirmation action position" }
        if (narrow) check(configuration.screenWidthDp == 280 && kotlin.math.abs(configuration.fontScale - 1.8f) < .01f)
    }
    private fun fresh() {
        gateway.eligibility.set(true); gateway.ordinary.set(false); gateway.rawOverride.set(null)
        main {
            model.back()
            val value = Session(target, "Fixture context", "claude", "waiting", "fixture", "", 0, gateway.raw())
            set("setConnections", listOf(connection))
            set("setMachines", listOf(Machine(target.connectionId, target.computerId, "Fixture computer", true)))
            set("setSessions", listOf(value)); set("setSelected", value)
            set("setRelayOperations", mapOf(target.connectionId to setOf("inspect", "clear_context", "compact_context")))
            set("setMachineOperations", mapOf(MachineKey(target.connectionId, target.computerId) to setOf("inspect", "clear_context", "compact_context")))
            capabilities(true, true)
            model.refreshActivity(explicit = true)
        }
        await("fresh exact context inspection") { main { model.activityVerified && !model.detailBusy && model.activity?.string("conversation_id") == target.conversation } }
    }
    private fun capabilities(relay: Boolean, machine: Boolean) {
        set("setRelayFeatures", mapOf(target.connectionId to if (relay) setOf(feature) else emptySet<String>()))
        set("setMachineFeatures", mapOf(MachineKey(target.connectionId, target.computerId) to if (machine) setOf(feature) else emptySet<String>()))
    }
    private fun settled() = main { !model.sendingBlocked(target) && !model.contextBlocked(target) &&
        model.contextOperations.none { it.status == "sending" } }
    private fun assertPreserved() {
        flush()
        val saved = store.messageState()
        check(saved.drafts == retained.drafts && saved.outgoing == retained.outgoing && saved.sessionActions == retained.sessionActions) { "Original private draft, answer, outbox or action changed" }
        check(saved.contextOperations.filter { old -> retained.contextOperations.any { it.requestId == old.requestId } } == retained.contextOperations)
        check(main { model.drafts == retained.drafts && model.outgoing == retained.outgoing })
        check(store.readState().records.filter { row -> reads.records.any { it.target == row.target } } == reads.records) { "Original read positions changed" }
        val decrypted = ByteArrayOutputStream()
        runBlocking { AttachmentStore(targetContext).decrypt(file) { decrypted.write(it) } }
        check(decrypted.toByteArray().contentEquals(bytes)) { "Original encrypted attachment changed" }
        check(gateway.unexpected.get() == 0 && gateway.sends.get() == 0 && gateway.readMarks.get() == 0)
        check(!LiveConnectionService.running)
    }
    private fun startActivity() {
        activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("private storage and read writer") { main { model.storageReady && model.javaClass.getDeclaredMethod("getReadTrackingReady").apply { isAccessible = true }.invoke(model) == true } }
        main { model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(gateway.client)) }
        fresh()
    }
    private class WriteGate(val known: Set<String>) {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val claimed = AtomicBoolean()
        val operation = AtomicReference<ContextOperation?>(null)
    }
    private fun installWriteGate() {
        writer = runBlocking { MessagePersistence.get(targetContext).open() }
        writeField = writer.javaClass.getDeclaredField("write").apply { isAccessible = true }
        @Suppress("UNCHECKED_CAST")
        val save = writeField.get(writer) as suspend (MessageState) -> Unit
        originalWrite = save
        val intercepted: suspend (MessageState) -> Unit = { state ->
            nextWrite.get()?.let { gate ->
                val candidate = state.contextOperations.firstOrNull { it.requestId !in gate.known && it.status == "sending" }
                if (candidate != null && gate.claimed.compareAndSet(false, true)) {
                    gate.operation.set(candidate); gate.entered.countDown()
                    check(gate.release.await(15, TimeUnit.SECONDS)) { "Fixture did not release the owned critical private save" }
                }
            }
            save(state)
        }
        writeField.set(writer, intercepted)
    }
    private fun smoke() {
        check(fixtureCase in setOf("all", "frame", "opener")) { "Unknown fixture case: $fixtureCase" }
        check(BuildConfig.DEBUG && !BuildConfig.FIREBASE_ENABLED && Build.HARDWARE in setOf("ranchu", "goldfish"))
        check(!LiveConnectionService.running)
        if (narrow) check(configuration.screenWidthDp == 280 && kotlin.math.abs(configuration.fontScale - 1.8f) < .01f) {
            "Use actual Android width280dp/font1.8 for narrow mode; observed ${configuration.screenWidthDp}/${configuration.fontScale}"
        }
        store = PrivateStore(targetContext)
        check(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState()) { "Use fresh owned emulator app data; existing private state is never cleared" }
        bytes = VoiceProtocol.wav(ByteArray(6_400) { (it % 17).toByte() })
        file = runBlocking { AttachmentStore(targetContext).importRecording(bytes) }
        val other = target.copy(session = "claude/fixture/retained", conversation = "fixture-retained-conversation")
        retained = MessageState(
            drafts = listOf(Draft(target, "Keep the original composer exactly", attachments = listOf(file), updatedAt = 1,
                revision = "fixture-editor-revision", generation = "fixture-editor-generation"),
                Draft(target, "Original unsent answer", questionId = "fixture-question", questionHash = "fixture-question-hash",
                    answers = "[{\"question_id\":\"fixture-prompt\",\"selected_option_ids\":[],\"text\":\"Original unsent answer\"}]",
                    updatedAt = 1, revision = "fixture-answer-revision", generation = "fixture-answer-generation")),
            outgoing = listOf(OutgoingMessage("fixture-recorded-delivery", target, "Original recorded delivery", listOf(file), 1.0,
                status = "recorded", submittedAt = 1.0, submittedText = "Original recorded delivery")),
            contextOperations = listOf(ContextOperation("fixture-context-control", other, "compact_context", status = "completed", createdAt = 1)),
            sessionActions = listOf(SessionAction("fixture-action-control", other, "rename", "{\"new_name\":\"claude/fixture/retained\"}", status = "completed", createdAt = 1)))
        reads = ConversationReadState(listOf(ConversationReadRecord(other, seen = listOf("fixture-seen"), epoch = "fixture-epoch",
            readThrough = 7, headIncoming = 8, headTotal = 8, headComplete = true, established = true, reviewLater = true)))
        store.saveMessageState(retained); store.saveReadState(reads)
        writer = runBlocking { MessagePersistence.get(targetContext).open() }
        try {
            startActivity()
            if (fixtureCase == "frame") {
                main { activity.setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) { Text("Synthetic frame guard diagnostic") } } } }
                waitForIdleSync()
                strictInitialGuards()
                assertPreserved()
                completed += "frame-diagnostic-only"
                return
            }
            if (fixtureCase == "opener") {
                val before = gateway.clears.get()
                val original = store.messageState().contextOperations
                footer()
                openDialog("Dismiss and clear…", "Dismiss and clear?", diagnosticClick = true)
                confirmationCaption(true)
                check(gateway.clears.get() == before) { "Diagnostic opener created a context request" }
                tap("Cancel"); flush()
                check(gateway.clears.get() == before && store.messageState().contextOperations == original) { "Diagnostic opener or cancel created a context request" }
                assertPreserved()
                completed += "accessibility-opener-dialog-cancel-noPOST"
                return
            }
            confirmation()
            val beforeCancel = gateway.clears.get()
            tap("Cancel")
            check(gateway.clears.get() == beforeCancel && store.messageState().contextOperations == retained.contextOperations) { "Cancel created a context request" }
            assertPreserved(); completed += "cancel-noPOST"
            confirmation(); screenshot("dismiss-context-${if (narrow) "narrow" else "normal"}-confirmation.png")
            tap("Dismiss and clear")
            await("one explicit dismiss clear") { gateway.clears.get() == 1 && requestState(0, "completed") }
            val first = gateway.bodies.single()
            check(first.getJSONObject("payload").opt("dismiss_prompt") is Boolean && first.getJSONObject("payload").getBoolean("dismiss_prompt"))
            val firstOperation = store.messageState().contextOperations.single { it.requestId == first.string("request_id") }
            check(firstOperation.target == target && firstOperation.status == "completed" && dismissFlag(firstOperation) == true)
            assertPreserved(); completed += "explicit-true-confirmation"
            ordinaryClear()
            capturedConfirmationModes()
            strictInitialGuards()
            installWriteGate()
            durableWithdrawalGuards()
            uncertainRecovery()
            assertPreserved()
        } catch (failure: Throwable) {
            failureArtifacts()
            throw failure
        } finally {
            nextWrite.getAndSet(null)?.release?.countDown()
            restoreWindowCallbacks()
            if (::activity.isInitialized) main { activity.finish() }
            gateway.client.dispatcher.cancelAll()
            if (::writer.isInitialized) {
                flush()
                if (::originalWrite.isInitialized) writeField.set(writer, originalWrite)
            }
            gateway.client.connectionPool.evictAll()
        }
    }
    private fun ordinaryClear() {
        fresh(); gateway.ordinary.set(true); gateway.eligibility.set(false)
        main { model.refreshActivity(explicit = true) }
        await("ordinary clear eligibility") { main { !model.detailBusy && model.activity?.opt("clear_context_supported") == true } }
        footer(); openDialog("Clear context", "Clear context?")
        val before = gateway.clears.get(); tap("Clear context")
        await("one ordinary clear") { gateway.clears.get() == before + 1 && requestState(before, "completed") }
        val body = gateway.bodies[before]
        val original = store.messageState().contextOperations.single { it.requestId == body.string("request_id") }
        check(!body.getJSONObject("payload").has("dismiss_prompt")) { "Ordinary clear acquired dismissal authority" }
        check(original.target == target && original.status == "completed" && dismissFlag(original) != true)
        assertPreserved(); completed += "ordinary-omission"
    }
    private fun capturedConfirmationModes() {
        fresh(); gateway.ordinary.set(true); gateway.eligibility.set(false)
        main { model.refreshActivity(explicit = true) }
        await("ordinary confirmation inspection") { main { model.activityVerified && !model.detailBusy && model.activity?.opt("clear_context_supported") == true && model.activity?.opt(feature) == false } }
        footer(); val ordinaryWindowId = openDialog("Clear context", "Clear context?")
        val ordinaryCaption = confirmationCaption(false)
        var before = gateway.clears.get()
        var original = store.messageState().contextOperations
        val ordinaryInspections = gateway.inspections.get()
        gateway.ordinary.set(false); gateway.eligibility.set(true)
        main { model.refreshActivity(explicit = true) }
        await("chooser inspection while ordinary confirmation stays open") { gateway.inspections.get() > ordinaryInspections && main {
            model.activityVerified && !model.detailBusy && model.activity?.opt("clear_context_supported") == false && model.activity?.opt(feature) == true
        } }
        waitForIdleSync(); disabledConfirmation(false, ordinaryCaption, ordinaryWindowId)
        check(gateway.clears.get() == before)
        tap("Cancel"); flush()
        check(gateway.clears.get() == before && store.messageState().contextOperations == original) { "Ordinary confirmation refresh or cancel created a context request" }
        assertPreserved(); completed += "ordinary-confirmation-never-upgrades"

        fresh(); val dismissWindowId = confirmation()
        val dismissCaption = confirmationCaption(true)
        before = gateway.clears.get()
        original = store.messageState().contextOperations
        val dismissInspections = gateway.inspections.get()
        gateway.ordinary.set(true); gateway.eligibility.set(false)
        main { model.refreshActivity(explicit = true) }
        await("dismiss eligibility withdrawn while its confirmation stays open") { gateway.inspections.get() > dismissInspections && main {
            model.activityVerified && !model.detailBusy && model.activity?.opt("clear_context_supported") == true && model.activity?.opt(feature) == false
        } }
        waitForIdleSync(); disabledConfirmation(true, dismissCaption, dismissWindowId)
        check(gateway.clears.get() == before)
        tap("Cancel"); flush()
        check(gateway.clears.get() == before && store.messageState().contextOperations == original) { "Dismiss confirmation refresh or cancel created a context request" }
        assertPreserved(); completed += "dismiss-confirmation-never-falls-back"
    }
    private fun strictInitialGuards() {
        for ((name, change) in listOf<Pair<String, () -> Unit>>(
            "relay-feature" to { capabilities(false, true) },
            "computer-feature" to { capabilities(true, false) },
            "false-eligibility" to { set("setActivity", gateway.raw().put(feature, false)) },
            "string-eligibility" to { set("setActivity", gateway.raw().put(feature, "true")) },
            "integer-eligibility" to { set("setActivity", gateway.raw().put(feature, 1)) },
            "missing-eligibility" to { set("setActivity", gateway.raw().also { it.remove(feature) }) },
            "wrong-run" to { set("setActivity", gateway.raw().put("run_id", "replacement-run")) },
            "archive" to { set("setActivity", gateway.raw().put("state", "archived").put("archive_id", "fixture-archive")) },
            "wrong-frame-connection" to { replaceFrameTarget(target.copy(connectionId = "fixture-other-workspace")) },
            "wrong-frame-computer" to { replaceFrameTarget(target.copy(computerId = "fixture-other-computer")) })) {
            fresh(); main(change)
            val before = gateway.clears.get()
            val attempt = dismiss()
            runBlocking { withTimeout(15_000) { attempt.join() } }
            flush()
            check(gateway.clears.get() == before) { "$name guard permitted a context POST" }
            assertPreserved()
        }
        completed += "strict-initial-guards"
    }
    private fun durableWithdrawalGuards() {
        for ((name, withdraw) in listOf<Pair<String, () -> Unit>>(
            "relay-feature" to { capabilities(false, true) },
            "computer-feature" to { capabilities(true, false) },
            "eligibility" to { set("setActivity", gateway.raw().put(feature, false)) },
            "target" to { set("setSelected", Session(target.copy(conversation = "replacement-conversation"), "Replacement", "claude", "idle", "fixture", "", 0, gateway.raw())) },
            "wrong-frame-connection" to { replaceFrameTarget(target.copy(connectionId = "fixture-other-workspace")) },
            "wrong-frame-computer" to { replaceFrameTarget(target.copy(computerId = "fixture-other-computer")) })) {
            fresh()
            val gate = WriteGate(store.messageState().contextOperations.map { it.requestId }.toSet())
            nextWrite.set(gate)
            val before = gateway.clears.get()
            val attempt = dismiss()
            check(gate.entered.await(15, TimeUnit.SECONDS)) { "$name did not reach the real critical private write" }
            val original = checkNotNull(gate.operation.get())
            check(original.target == target && dismissFlag(original) == true)
            check(gateway.clears.get() == before)
            main(withdraw); gate.release.countDown()
            await("$name withdrawal durably refused") { store.messageState().contextOperations.any { it.requestId == original.requestId && it.status == "failed" } }
            runBlocking { withTimeout(15_000) { attempt.join() } }
            nextWrite.set(null)
            check(gateway.clears.get() == before) { "$name withdrawal was ignored after private save" }
            check(dismissFlag(store.messageState().contextOperations.single { it.requestId == original.requestId }) == true)
            assertPreserved()
        }
        completed += "durable-save-withdrawals"
    }
    private fun uncertainRecovery() {
        fresh(); gateway.timeoutNext.set(true)
        val before = gateway.clears.get()
        confirmation(); tap("Dismiss and clear")
        await("unknown delivery saved under the original ID") { gateway.clears.get() == before + 1 && requestState(before, "uncertain") }
        val body = gateway.bodies[before]
        val original = store.messageState().contextOperations.single { it.requestId == body.string("request_id") }
        check(original.requestId == body.string("request_id") && original.target == target && original.status == "uncertain" && dismissFlag(original) == true)
        assertPreserved()
        main { activity.finish() }; waitForIdleSync()
        startActivity()
        val restored = main { model.contextOperations.single { it.requestId == original.requestId } }
        check(restored.status == "uncertain" && restored.target == original.target && dismissFlag(restored) == true)
        footer(); val receiptReads = gateway.receipts.get(); tap("Check result")
        await("original receipt checked") { gateway.receipts.get() > receiptReads && main { "context:${original.requestId}" !in model.receiptFlights } }
        check(gateway.clears.get() == before + 1) { "Receipt checking repeated the clear" }
        footer(); openDialog("Review", "Review context command")
        tap("Mark reviewed")
        await("review saved without new transport") { store.messageState().contextOperations.any { it.requestId == original.requestId && it.status == "reviewed" } }
        check(gateway.clears.get() == before + 1) { "Recreation or review repeated the clear" }
        check(gateway.bodies.count { it.string("request_id") == original.requestId } == 1)
        assertPreserved(); completed += "timeout-recreation-check-review-no-rePOST"
    }
    private inner class Gateway {
        val eligibility = AtomicBoolean(true)
        val ordinary = AtomicBoolean(false)
        val rawOverride = AtomicReference<JSONObject?>(null)
        val timeoutNext = AtomicBoolean()
        val inspections = AtomicInteger()
        val clears = AtomicInteger()
        val receipts = AtomicInteger()
        val unexpected = AtomicInteger()
        val sends = AtomicInteger()
        val readMarks = AtomicInteger()
        val bodies: MutableList<JSONObject> = Collections.synchronizedList(mutableListOf())
        fun raw(): JSONObject = rawOverride.get()?.let { JSONObject(it.toString()) } ?: JSONObject()
            .put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
            .put("agent", "claude").put("runtime_state", "live").put("process_state", "running")
            .put("activity", if (ordinary.get()) "idle" else "waiting").put("phase", if (ordinary.get()) "idle" else "waiting")
            .put("clear_context_supported", ordinary.get()).put(feature, eligibility.get())
            .put("clear_context_reason", "Synthetic chooser waits for explicit dismissal")
            .put("compact_context_supported", false).put("events", JSONArray())
        private fun result(body: JSONObject, native: JSONObject) = JSONObject().put("request_id", body.string("request_id")).put("state", "completed").put("result", native)
        val client = OkHttpClient.Builder().addInterceptor { chain ->
            val request = chain.request()
            check(request.url.host == "relay.example.test") { "Only the synthetic relay is allowed" }
            val path = request.url.encodedPath
            val response = when {
                request.method == "GET" && path == "/v1/capabilities" -> JSONObject().put("operations", JSONArray(listOf("inspect", "clear_context", "compact_context"))).put("features", JSONArray(listOf(feature)))
                request.method == "GET" && path == "/v1/computers" -> JSONObject().put("computers", JSONArray())
                request.method == "GET" && path.startsWith("/v1/requests/") -> {
                    receipts.incrementAndGet()
                    val id = request.url.pathSegments.last()
                    check(bodies.any { it.string("request_id") == id }) { "Receipt for an unknown original ID" }
                    JSONObject().put("request_id", id).put("state", "uncertain").put("result", JSONObject.NULL).put("error", "Synthetic unknown original delivery")
                }
                request.method == "POST" && path == "/v1/requests" -> {
                    val body = JSONObject(okio.Buffer().also { request.body!!.writeTo(it) }.readUtf8())
                    check(body.string("computer_id") == target.computerId && body.string("session") == target.session)
                    when (body.string("operation")) {
                        "inspect" -> { inspections.incrementAndGet(); result(body, raw()) }
                        "clear_context" -> {
                            val payload = body.getJSONObject("payload")
                            val keys = payload.keys().asSequence().toSet()
                            check(keys == setOf("request_id", "expected_run_id", "expected_conversation_id") + if (payload.has("dismiss_prompt")) setOf("dismiss_prompt") else emptySet())
                            check(payload.string("request_id") == body.string("request_id") && payload.string("expected_run_id") == target.run && payload.string("expected_conversation_id") == target.conversation)
                            if (payload.has("dismiss_prompt")) check(payload.opt("dismiss_prompt") is Boolean && payload.getBoolean("dismiss_prompt"))
                            bodies += JSONObject(body.toString()); clears.incrementAndGet()
                            if (timeoutNext.compareAndSet(true, false)) throw SocketTimeoutException("Synthetic unknown context delivery")
                            result(body, JSONObject().put("request_id", body.string("request_id")).put("name", target.session)
                                .put("run_id", target.run).put("conversation_id", target.conversation).put("status", "confirmed"))
                        }
                        else -> {
                            if (body.string("operation") in setOf("send", "answer", "voice_submit")) sends.incrementAndGet()
                            if (body.string("operation") == "mark_read") readMarks.incrementAndGet()
                            unexpected.incrementAndGet(); error("Unexpected synthetic mutation ${body.string("operation")}")
                        }
                    }
                }
                else -> { unexpected.incrementAndGet(); error("Unexpected synthetic request ${request.method} $path") }
            }
            Response.Builder().request(request).protocol(Protocol.HTTP_1_1).code(200).message("Synthetic")
                .body(response.toString().toResponseBody("application/json".toMediaType())).build()
        }.build()
    }
}
