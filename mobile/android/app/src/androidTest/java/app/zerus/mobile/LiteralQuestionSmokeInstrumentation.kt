package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.text.Spanned
import android.text.style.ClickableSpan
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import okhttp3.OkHttpClient
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CyclicBarrier
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Actual read-only question rendering, using only private emulator state and no connected workspace. */
class LiteralQuestionSmokeInstrumentation : Instrumentation() {
    private var narrow = false
    override fun onCreate(arguments: Bundle?) { narrow = arguments?.getString("narrow") == "true"; super.onCreate(arguments); start() }
    private data class Scene(val name: String, val source: String, val literal: Boolean = true,
        val approval: Boolean = false, val trust: Boolean = false)
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }; return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val until = SystemClock.elapsedRealtime() + 10_000
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < until) { "Timed out: $description" }
            SystemClock.sleep(40)
        }
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (predicate(root)) return root
        for (index in 0 until root.childCount) find(root.getChild(index), predicate)?.let { return it }
        return null
    }
    private fun bounds(node: AccessibilityNodeInfo) = Rect().also(node::getBoundsInScreen)
    private fun text(field: String) = "$field *star* _under_ \\*escape\\_ `code` [label](https://example.test/path)"
    private fun parsed(field: String) = "$field star under *escape_ code label"
    private fun question(scene: Scene, target: Target) = Question("fixture-${scene.name}", "fixture-hash-${scene.name}", listOf(
        Prompt("fixture-prompt", text("Prompt"),
            if(scene.approval) listOf(Choice("allow",text("Choice"),text("Description")),Choice("deny","Deny fixture",""))
            else listOf(Choice("fixture-choice", text("Choice"), text("Description"))),
            multi = false, required = true, other = true, body = text("Body"), otherDescription = text("Other"))),
        canAnswer = false, canSkip = false, optional = false, delivery = "", source = scene.source,
        approval = scene.approval, trustRequest = scene.trust, run = target.run, conversation = target.conversation,
        unavailableReason = "Synthetic read-only fixture")

    /** Prewarm the real shared renderer cache through its existing API; do not inject prepared values. */
    private fun prepareMarkdown() {
        val renderer = Class.forName("app.zerus.mobile.MarkdownUiKt")
        val lazyCache = renderer.getDeclaredField("markdownDocuments\$delegate").apply { isAccessible = true }.get(null) as Lazy<*>
        val cache = lazyCache.value as MarkdownDocumentCache
        runBlocking { withTimeout(10_000) { for (field in listOf("Prompt", "Body", "Other", "Description")) cache.document(text(field)) } }
    }
    /** Drain existing preparation work so a transient Markdown literal fallback cannot pass a literal assertion. */
    private fun settleMarkdown() {
        waitForIdleSync()
        val dispatcher = Class.forName("app.zerus.mobile.MarkdownUiKt")
            .getDeclaredField("markdownAnnotationDispatcher").apply { isAccessible = true }.get(null) as CoroutineDispatcher
        // Two rendezvousing workers drain both lanes of the existing two-worker annotation dispatcher.
        // This is synchronization, not a delay or a change to production scheduling/cache state.
        runBlocking {
            withTimeout(10_000) {
                val barrier = CyclicBarrier(2)
                List(2) { async(dispatcher) { barrier.await(5, TimeUnit.SECONDS) } }.awaitAll()
            }
        }
        waitForIdleSync()
        uiAutomation.waitForIdle(100, 3_000)
    }
    private fun reveal(field: String, host: Rect): AccessibilityNodeInfo {
        var result: AccessibilityNodeInfo? = null
        await("visible $field disclosure") {
            val root = uiAutomation.rootInActiveWindow
            result = find(root) { node ->
                val value = node.text?.toString().orEmpty()
                node.isVisibleToUser && Rect.intersects(bounds(node), host) &&
                    (value.contains(text(field)) || value.contains(parsed(field)))
            }
            if (result == null) {
                val scroll = find(root) { it.isVisibleToUser && it.className?.toString() == "android.widget.ScrollView" &&
                    Rect.intersects(bounds(it), host) }
                checkNotNull(scroll) { "No disclosure scroller for $field" }
                check(scroll.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)) { "Disclosure ended before $field was visible" }
                waitForIdleSync()
            }
            result != null
        }
        return checkNotNull(result)
    }
    private fun assertNoEnabledDelivery() {
        var diagnostic = ""
        val unsafe = find(uiAutomation.rootInActiveWindow) { node ->
            val label = node.contentDescription?.toString().orEmpty().ifBlank { node.text?.toString().orEmpty() }
            if (!node.isVisibleToUser || !(label in setOf("Send", "Send answer", "Send all", "Skip", "Deny") || label.startsWith("Approve once"))) false
            else {
                // Compose can expose enabled Text beneath a disabled Button. Its whole owning path must allow input.
                val path = mutableListOf<String>()
                var effectiveEnabled = true
                var cursor: AccessibilityNodeInfo? = node
                while (cursor != null) {
                    val ancestor = checkNotNull(cursor)
                    check(path.size < 64 && ancestor.refresh()) { "Cannot refresh delivery path for <$label>: ${path.joinToString(" -> ")}" }
                    path += "${ancestor.className}(enabled=${ancestor.isEnabled},clickable=${ancestor.isClickable})"
                    effectiveEnabled = effectiveEnabled && ancestor.isEnabled
                    cursor = ancestor.parent
                }
                if (effectiveEnabled) diagnostic = "label=<$label>; path=${path.joinToString(" -> ")}"
                effectiveEnabled
            }
        }
        check(unsafe == null) { "Read-only fixture exposed an enabled delivery action: $diagnostic" }
    }
    private fun screenshot(name: String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null), name).outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
            bitmap.recycle()
        }
    }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish"))
            val report = smoke()
            result.putString("stream", "Literal question rendering passed: $report; transport0, exact saved answers/composer/read state retained. No delivery or link activation.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            screenshot("literal-question-failure.png")
            result.putString("stream", "Literal question rendering failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun smoke(): String {
        val store = PrivateStore(targetContext)
        require(store.connections().isEmpty() && store.messageState() == MessageState() && store.readState() == ConversationReadState()) {
            "Use fresh owned emulator app data; this fixture never clears existing state."
        }
        val target = Target("fixture-computer", "codex/fixture/literal", "fixture-run", "fixture-conversation", "fixture-workspace")
        val scenes = listOf(Scene("authored", "hook", literal = false),
            Scene("terminal-choice", "claude_terminal_choice"), Scene("terminal-safety", "claude_terminal_safety"),
            Scene("startup-choice", "startup_terminal_choice"), Scene("unknown", "fixture_unknown_source"),
            Scene("trust", "hook", trust = true), Scene("approval", "hook", approval = true))
        val questions = scenes.map { question(it, target) }
        val drafts = questions.map { q ->
            val answer = JSONArray().put(JSONObject().put("question_id", q.prompts.single().id)
                .put("selected_option_ids", JSONArray()).put("text", "Saved answer for ${q.id}"))
            Draft(target, "Saved answer for ${q.id}", questionId = q.id, questionHash = q.hash,
                answers = answer.toString(), revision = "fixture-revision-${q.id}", generation = "fixture-generation-${q.id}")
        } + Draft(target, "Keep composer text *exactly*", revision = "fixture-composer-revision", generation = "fixture-composer-generation")
        val savedMessages = MessageState(drafts = drafts)
        val readTarget = target.copy(session = "codex/fixture/read-control", conversation = "fixture-read-control")
        val savedReads = ConversationReadState(listOf(ConversationReadRecord(readTarget, epoch = "fixture-epoch", readThrough = 7,
            headIncoming = 8, headTotal = 8, headComplete = true, established = true, reviewLater = true)))
        store.saveMessageState(savedMessages); store.saveReadState(savedReads)
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        val transport = AtomicInteger()
        val client = OkHttpClient.Builder().addInterceptor { transport.incrementAndGet(); error("No fixture transport is allowed") }.build()
        val sceneIndex = mutableIntStateOf(-1)
        val appliedScene = AtomicInteger(-1)
        val hostBounds = AtomicReference(Rect())
        val density = AtomicReference(0f)
        val scale = AtomicReference(0f)
        val mode = if (narrow) "narrow" else "normal"
        try {
            await("private model storage") { main {
                model.storageReady && model.javaClass.getDeclaredMethod("getReadTrackingReady").apply { isAccessible = true }.invoke(model) == true
            } }
            main {
                model.javaClass.getDeclaredField("api").apply { isAccessible = true }.set(model, RelayApi(client))
                check(model.connections.isEmpty() && model.drafts == savedMessages.drafts)
                activity.setContent {
                    MaterialTheme(colorScheme = darkColorScheme(primary = Color(0xFF67E8CB), background = Color(0xFF101517),
                        surface = Color(0xFF1A2225), onSurface = Color(0xFFE9F0F2))) {
                        val originalDensity = LocalDensity.current
                        CompositionLocalProvider(LocalDensity provides Density(originalDensity.density, if (narrow) 1.8f else 1f)) {
                            val current = sceneIndex.intValue
                            val view = LocalView.current
                            density.set(LocalDensity.current.density); scale.set(LocalDensity.current.fontScale)
                            SideEffect { appliedScene.set(current) }
                            Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                                Box(Modifier.fillMaxSize().systemBarsPadding(), contentAlignment = Alignment.TopCenter) {
                                    Column(Modifier.width(if (narrow) 280.dp else 320.dp).fillMaxHeight()
                                        .onGloballyPositioned { coordinates ->
                                            val value = coordinates.boundsInWindow()
                                            val screen = IntArray(2); val window = IntArray(2)
                                            view.getLocationOnScreen(screen); view.getLocationInWindow(window)
                                            hostBounds.set(Rect((value.left + screen[0] - window[0]).toInt(), (value.top + screen[1] - window[1]).toInt(),
                                                (value.right + screen[0] - window[0]).toInt(), (value.bottom + screen[1] - window[1]).toInt()))
                                        }.padding(8.dp)) {
                                        if (current >= 0) key(current) {
                                            val q = questions[current]
                                            Text("Synthetic question ${scenes[current].name}", style = MaterialTheme.typography.labelSmall)
                                            var display by remember(q.id, q.hash) { mutableStateOf(QuestionDisplayState(target.key)) }
                                            BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                                                QuestionsPane(model, target, listOf(q), available = false, visible = true, viewport = null,
                                                    maxHeight = maxHeight, display = display, onDisplay = { update -> display = update(display) },
                                                    onReview = { error("No review action is allowed in a rendering fixture") })
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            val completed = mutableListOf<String>()
            for ((index, scene) in scenes.withIndex()) {
                prepareMarkdown()
                main { sceneIndex.intValue = index }
                await("fresh ${scene.name} layout") { appliedScene.get() == index && find(uiAutomation.rootInActiveWindow) {
                    it.isVisibleToUser && it.text?.toString() == "Synthetic question ${scene.name}"
                } != null }
                settleMarkdown()
                val host = hostBounds.get()
                val expectedWidth = if (narrow) 280 else 320
                check(kotlin.math.abs(host.width() / density.get() - expectedWidth) <= 1f) { "Unexpected native question viewport $host" }
                check(scale.get() == if (narrow) 1.8f else 1f)
                assertNoEnabledDelivery()
                for (field in listOf("Prompt", "Body", "Other", "Choice", "Description")) {
                    val node = reveal(field, host)
                    val literal = scene.literal || field == "Choice"
                    val expected = if (literal) text(field) else parsed(field)
                    val observed = node.text?.toString().orEmpty()
                    check(observed.contains(expected)) { "${scene.name}/$field changed disclosure: expected <$expected>, observed <$observed>" }
                    if (!literal) check(!observed.contains(text(field))) { "${scene.name}/$field did not render authored Markdown" }
                    if (scene.literal) {
                        val spans = node.text as? Spanned
                        check(spans == null || spans.getSpans(0, spans.length, ClickableSpan::class.java).isEmpty()) {
                            "${scene.name}/$field created a clickable span in a literal disclosure"
                        }
                    }
                    assertNoEnabledDelivery()
                }
                main {
                    check(questions[index] == question(scene, target)) { "Question identity or disclosure changed" }
                    check(model.connections.isEmpty() && model.drafts == savedMessages.drafts && model.outgoing.isEmpty())
                    check(model.pendingQuestionSend == null)
                    check(model.javaClass.getDeclaredMethod("getReadState").apply { isAccessible = true }.invoke(model) == savedReads)
                }
                check(store.messageState() == savedMessages && store.readState() == savedReads && store.connections().isEmpty())
                check(transport.get() == 0) { "Renderer attempted transport" }
                if (scene.name in setOf("authored", "terminal-choice", "trust", "approval")) screenshot("literal-question-$mode-${scene.name}.png")
                completed += scene.name
            }
            return "$mode width${if (narrow) 280 else 320}dp/font${scale.get()}, ${completed.joinToString()}, five exact disclosure paths each"
        } finally { main { activity.finish() } }
    }
}
