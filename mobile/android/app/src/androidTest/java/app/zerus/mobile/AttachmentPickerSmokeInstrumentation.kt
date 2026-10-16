package app.zerus.mobile

import android.app.Activity
import android.app.Application
import android.app.Instrumentation
import android.content.ContentValues
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.provider.MediaStore
import android.view.InputDevice
import android.view.MotionEvent
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.activity.result.ActivityResultRegistry
import androidx.activity.result.contract.ActivityResultContract
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityOptionsCompat
import androidx.core.content.FileProvider
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Fresh owned emulator only. Real private storage, AndroidX result state, DocumentsUI and physical recovery taps. */
class AttachmentPickerSmokeInstrumentation : Instrumentation() {
    private var narrow = false
    private var observedModel: ZerusViewModel? = null
    private val target = Target("fixture-computer", "codex/fixture/files", "fixture-run", "fixture-conversation", "fixture-workspace")
    private val other = target.copy(session = "codex/fixture/other", conversation = "other-conversation")
    private val selection = "11111111-1111-4111-8111-111111111111"
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); narrow = arguments?.getString("narrow") == "true"; start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG && Build.HARDWARE in setOf("ranchu", "goldfish") && Build.VERSION.SDK_INT >= 29)
            val store = PrivateStore(targetContext)
            check(store.connections().isEmpty() && store.messageState() == MessageState()) { "Use fresh disposable emulator app data." }
            val input = File(targetContext.cacheDir, "attachment-view/picker-native-source.txt").apply {
                parentFile!!.mkdirs(); writeText("Retained original file\n")
            }
            val uri = FileProvider.getUriForFile(targetContext, "${targetContext.packageName}.attachments", input)
            registryRestore(uri)
            smoke(store, uri)
            result.putString("stream", "Attachment picker passed (${if(narrow) "narrow" else "normal"}): cold orphan recovery, retained text/files, actual DocumentsUI cancel/recreation/import, delayed restored result identity, stale callbacks, active import guard, cross-conversation cancellation and update recovery physical48dp controls. No native sends or update installations.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            screenshot("failure")
            result.putString("stream", "Attachment picker failed: ${error.stackTraceToString()}\n")
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
            check(SystemClock.elapsedRealtime() < deadline) {
                val state = main { observedModel?.let { "picker=${it.pickerId},importing=${it.attachmentImporting},files=${it.draft(target).attachments.size},error=${it.error}" } }
                "Timed out: $description; package=${uiAutomation.rootInActiveWindow?.packageName}; $state"
            }
            SystemClock.sleep(40)
        }
    }
    private fun find(root: AccessibilityNodeInfo?, predicate: (AccessibilityNodeInfo) -> Boolean): AccessibilityNodeInfo? {
        root ?: return null
        root.refresh(); if (predicate(root)) return root
        for (i in 0 until root.childCount) find(root.getChild(i), predicate)?.let { return it }
        return null
    }
    private fun button(label: String, reveal: Boolean = false): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await(label) {
            found = find(uiAutomation.rootInActiveWindow) { it.text?.toString() == label || it.contentDescription?.toString() == label }
            if (reveal) found?.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SHOW_ON_SCREEN.id)
            found?.isVisibleToUser == true
        }
        var action = checkNotNull(found)
        while (true) {
            action.refresh()
            if (action.isClickable || !action.isEnabled || action.parent == null) break
            action = checkNotNull(action.parent)
        }
        check(action.isClickable || !action.isEnabled) { "No button owner for $label" }
        return action
    }
    private fun physical(node: AccessibilityNodeInfo, minimum: Boolean = true) {
        node.refresh()
        val rect = Rect().also(node::getBoundsInScreen)
        val display = targetContext.resources.displayMetrics
        if (minimum) check(rect.width() >= 48 * display.density - 1 && rect.height() >= 48 * display.density - 1) { "Target below48dp: $rect" }
        check(rect.left >= 0 && rect.top >= 0 && rect.right <= display.widthPixels && rect.bottom <= display.heightPixels) { "Target outside display: $rect" }
        val down = SystemClock.uptimeMillis()
        for (kind in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val properties = MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER }
            val coordinates = MotionEvent.PointerCoords().apply {
                x = rect.exactCenterX(); y = rect.exactCenterY(); pressure = if(kind == MotionEvent.ACTION_DOWN) 1f else 0f; size = 0.1f
            }
            val event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), kind, 1, arrayOf(properties), arrayOf(coordinates),
                0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { check(uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
            if(kind == MotionEvent.ACTION_DOWN) SystemClock.sleep(40)
        }
    }
    private fun screenshot(name: String) {
        uiAutomation.takeScreenshot()?.let { bitmap ->
            File(targetContext.getExternalFilesDir(null), "picker-${if(narrow) "narrow" else "normal"}-$name.png")
                .outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
            bitmap.recycle()
        }
    }
    private class Registry : ActivityResultRegistry() {
        var request = -1
        override fun <I, O> onLaunch(requestCode: Int, contract: ActivityResultContract<I, O>, input: I, options: ActivityOptionsCompat?) {
            request = requestCode
        }
    }
    private fun registryRestore(uri: Uri) = main {
        val received = mutableListOf<Pair<String, List<Uri>>>()
        val first = Registry()
        val host = AttachmentPickerHost(first, "") { files, id -> received += id to files }
        host.launch(selection)
        val code = first.request
        val saved = Bundle().also(first::onSaveInstanceState)
        val owned = host.launchedId
        host.close()
        val recreated = Registry().apply { onRestoreInstanceState(saved) }
        check(recreated.dispatchResult(code, Activity.RESULT_OK, Intent().setData(uri)))
        var storageReady = false
        val restored = AttachmentPickerHost(recreated, owned) { files, id ->
            check(storageReady); received += id to files
        }
        check(received.isEmpty()) // Android buffers the result while encrypted storage is loading.
        check(!restored.restore("foreign-selection") && received.isEmpty())
        storageReady = true
        check(restored.restore(selection))
        check(received == listOf(selection to listOf(uri)))
        check(!restored.restore(selection))
        restored.retain("")
        restored.launch("new-selection")
        val newerCode = recreated.request
        check(newerCode != code)
        recreated.dispatchResult(code, Activity.RESULT_OK, Intent().setData(uri))
        check(received.size == 1) // Old results cannot be delivered through a new launcher's callback.
        recreated.dispatchResult(newerCode, Activity.RESULT_CANCELED, null)
        check(received == listOf(selection to listOf(uri), "new-selection" to emptyList<Uri>()))
        restored.close()
        val refusing = object : ActivityResultRegistry() {
            override fun <I, O> onLaunch(requestCode: Int, contract: ActivityResultContract<I, O>, input: I, options: ActivityOptionsCompat?) {
                throw android.content.ActivityNotFoundException("Synthetic unavailable document picker")
            }
        }
        val unavailable = AttachmentPickerHost(refusing, "") { _, _ -> error("Failed launch delivered a result") }
        check(runCatching { unavailable.launch("unavailable") }.exceptionOrNull() is android.content.ActivityNotFoundException)
        check(unavailable.launchedId.isEmpty())
        unavailable.close()
    }
    private fun model(activity: MainActivity) = main {
        activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel
    }
    private fun smoke(store: PrivateStore, uri: Uri) {
        val files = AttachmentStore(targetContext)
        val retained = runBlocking { files.importDocument(uri) }
        val origin = Draft(target, "Retained original draft", attachments = listOf(retained))
        val unrelated = Draft(other, "Retained other draft")
        store.saveMessageState(MessageState(drafts = listOf(origin, unrelated), pickerDraft = origin, pickerId = selection))
        var activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = model(activity)
        observedModel = model
        await("cold orphan released after storage load") { main { model.storageReady && model.pickerDraft == null && model.pickerId.isEmpty() } }
        check(main { model.drafts == listOf(origin, unrelated) && model.outgoing.isEmpty() && model.updateInstallReason().isEmpty() })
        main { model.open(Session(target, "File picker fixture", "codex", "idle", "", "", 0, JSONObject())) }
        physical(button("Attach files"))
        await("actual DocumentsUI") { uiAutomation.rootInActiveWindow?.packageName?.toString()?.endsWith("documentsui") == true }
        val oldSelection = main { model.pickerId }
        check(oldSelection.isNotBlank())
        screenshot("system-picker")
        sendKeyDownUpSync(android.view.KeyEvent.KEYCODE_BACK)
        await("native cancellation") { main { model.pickerDraft == null } }
        check(main { model.draft(target) == origin })
        physical(button("Attach files"))
        await("second actual DocumentsUI") { uiAutomation.rootInActiveWindow?.packageName?.toString()?.endsWith("documentsui") == true }
        val activeId = main { model.pickerId }
        val latest = AtomicReference<MainActivity>()
        val callbacks = object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(value: Activity, state: Bundle?) { if (value is MainActivity && value !== activity) latest.set(value) }
            override fun onActivityStarted(value: Activity) {}
            override fun onActivityResumed(value: Activity) {}
            override fun onActivityPaused(value: Activity) {}
            override fun onActivityStopped(value: Activity) {}
            override fun onActivitySaveInstanceState(value: Activity, state: Bundle) {}
            override fun onActivityDestroyed(value: Activity) {}
        }
        val application = targetContext.applicationContext as Application
        application.registerActivityLifecycleCallbacks(callbacks)
        try {
            main { activity.recreate() }
            await("Activity recreated under native picker") { latest.get() != null }
            activity = latest.get()
            check(model(activity) === model)
            check(main { model.pickerId == activeId && model.pickerDraft != null })
            sendKeyDownUpSync(android.view.KeyEvent.KEYCODE_BACK)
            await("restored native cancel callback") { main { model.pickerDraft == null } }
        } finally { application.unregisterActivityLifecycleCallbacks(callbacks) }

        val name = "zerus-picker-${UUID.randomUUID()}.txt"
        val content = "Selected native fixture\nПривет\n".toByteArray()
        val selectedUri = checkNotNull(targetContext.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI,
            ContentValues().apply { put(MediaStore.Downloads.DISPLAY_NAME, name); put(MediaStore.Downloads.MIME_TYPE, "text/plain"); put(MediaStore.Downloads.IS_PENDING, 1) }))
        try {
            targetContext.contentResolver.openOutputStream(selectedUri)!!.use { it.write(content) }
            targetContext.contentResolver.update(selectedUri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
            physical(button("Attach files"))
            await("third actual DocumentsUI") { uiAutomation.rootInActiveWindow?.packageName?.toString()?.endsWith("documentsui") == true }
            val currentId = main { model.pickerId }
            main { model.finishAttachmentPick(listOf(uri), oldSelection); model.cancelAttachmentPick(oldSelection) }
            check(main { model.pickerId == currentId && model.draft(target).attachments == listOf(retained) })
            // The standard picker opens its Recent view; use its explicit Downloads root if needed.
            if (find(uiAutomation.rootInActiveWindow) { it.text?.toString() == name } == null) {
                physical(button("Show roots"), minimum = false)
                physical(button("Downloads"), minimum = false)
            }
            // Hit the exact owned filename, avoiding thumbnail/preview actions
            // or a generic ancestor in the system document grid.
            var document: AccessibilityNodeInfo? = null
            await("owned document title") {
                document = find(uiAutomation.rootInActiveWindow) { it.text?.toString() == name && it.isVisibleToUser }
                document != null
            }
            physical(checkNotNull(document), minimum = false)
            try {
                await("native file imported") { main { model.pickerDraft == null && !model.attachmentImporting && model.draft(target).attachments.size == 2 } }
            } catch (error: Throwable) {
                screenshot("document-before-cleanup")
                val labels = mutableListOf<String>()
                fun describe(node: AccessibilityNodeInfo?) {
                    node ?: return
                    if (node.isVisibleToUser && (node.text != null || node.contentDescription != null)) {
                        labels += "${node.viewIdResourceName} ${node.text} ${node.contentDescription} enabled=${node.isEnabled}"
                    }
                    for (i in 0 until node.childCount) describe(node.getChild(i))
                }
                describe(uiAutomation.rootInActiveWindow)
                File(targetContext.getExternalFilesDir(null), "picker-document-before-cleanup.txt").writeText(labels.joinToString("\n"))
                throw error
            }
            val imported = main { model.draft(target).attachments.single { it.id != retained.id } }
            check(imported.name == name && imported.sha256 == MessageDigest.getInstance("SHA-256").digest(content).joinToString("") { "%02x".format(it) })
            val plaintext = ByteArrayOutputStream()
            runBlocking { files.decrypt(imported) { plaintext.write(it) } }
            check(plaintext.toByteArray().contentEquals(content))
            check(main { model.draft(target).text == origin.text && model.draft(other) == unrelated && model.outgoing.isEmpty() })
        } finally { targetContext.contentResolver.delete(selectedUri, null, null) }
        recoveryUi(activity, model)
        runBlocking { MessagePersistence.get(targetContext).open().flush() }
        check(store.messageState().drafts == main { model.drafts })
        main { activity.finish() }
    }
    private fun recoveryUi(activity: MainActivity, model: ZerusViewModel) {
        val writer = runBlocking { MessagePersistence.get(targetContext).open() }
        val origin = main { model.draft(target) }
        val updates = mutableStateOf(true)
        val clicks = AtomicInteger()
        val raw = """{"schemaVersion":1,"product":"zerus","platform":"android","channel":"dev","versionName":"0.1.33-dev.999","versionCode":20000,"releaseId":42,"publishedAt":"2026-10-16T10:00:00Z","releaseUrl":"https://github.com/ufna/zerus/releases/tag/android-dev-fixture","sourceCommit":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","artifacts":[{"kind":"apk","name":"zerus.apk","url":"https://zerus.dev/downloads/android-dev-fixture/zerus.apk","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":12345,"os":"android","arch":"universal","minSdk":26,"packageName":"app.zerus.mobile","signingCertSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]}"""
        val update = UpdatePolicy.parse(raw)
        main {
            activity.setContent {
                MaterialTheme(colorScheme = darkColorScheme()) {
                    val density = LocalDensity.current
                    CompositionLocalProvider(LocalDensity provides Density(density.density, if (narrow) 1.8f else 1f)) {
                        Surface(color = MaterialTheme.colorScheme.background) {
                        Column(Modifier.fillMaxSize().systemBarsPadding()
                            .verticalScroll(rememberScrollState()).padding(16.dp)) {
                            Column(if(narrow) Modifier.width(280.dp) else Modifier.fillMaxWidth()) {
                                if (updates.value) {
                                    val capturedId = model.pickerId
                                    UpdateProgressPanel(UpdateState(status="downloaded", available=update, downloaded=update),
                                        busy=false, preparing=false, confirmationAvailable=false, installReason=model.updateInstallReason(), error="",
                                        onChannel={}, onAction={ clicks.incrementAndGet() },
                                        onCancelSelection=if(model.pickerDraft!=null) ({ model.cancelAttachmentPick(capturedId, pendingOnly=true) }) else null)
                                } else PendingAttachmentSelection(model, other)
                            }
                        }
                        }
                    }
                }
            }
        }
        waitForIdleSync()
        for (page in listOf(true, false)) {
            main {
                updates.value = page
                writer.edit { it.copy(pickerDraft=origin, pickerId=selection) }
            }
            await("pending recovery state") { main { model.pickerId == selection } }
            if (page) {
                val disabled = button("Install update", reveal = true)
                check(!disabled.isEnabled)
                disabled.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                physical(disabled)
                check(clicks.get() == 0)
            }
            val cancel = button("Cancel selection", reveal = true)
            check(cancel.isEnabled)
            if (page) {
                // Model a result that has just reserved its import job before
                // the UI has recomposed away the pending-only recovery action.
                val field = model.javaClass.getDeclaredField("importJob").apply { isAccessible = true }
                val previous = main { field.get(model) }
                val inFlight = kotlinx.coroutines.Job()
                try {
                    main { field.set(model, inFlight) }
                    physical(cancel)
                    waitForIdleSync()
                    check(main { model.pickerId == selection && inFlight.isActive })
                } finally { main { field.set(model, previous) }; inFlight.complete() }
            }
            screenshot(if(page) "updates-pending" else "other-conversation-pending")
            physical(button("Cancel selection", reveal = true))
            await("recovery clears exact pending marker") { main { model.pickerDraft == null && model.updateInstallReason().isEmpty() } }
            check(main { model.draft(target) == origin && model.draft(other).text == "Retained other draft" && model.outgoing.isEmpty() })
            if (page) check(button("Install update", reveal = true).isEnabled)
        }
    }
}
