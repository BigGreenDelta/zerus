package app.zerus.mobile

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.activity.compose.setContent
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.atomic.AtomicReference

/** The real picker/controller with memory transport on a fresh disposable profile; no gateway or agent. */
class SessionProjectSmokeInstrumentation : Instrumentation() {
    private var end = 0L
    private val completed = mutableListOf<String>()
    override fun onCreate(arguments: Bundle?) { super.onCreate(arguments); start() }
    override fun onStart() {
        val result = Bundle()
        try {
            check(BuildConfig.DEBUG)
            end = SystemClock.elapsedRealtime() + 60_000
            smoke()
            check(completed == listOf("current_selection", "close_no_write", "durable_single_post", "route_refusal", "original_receipt", "stale_navigation"))
            result.putInt("project_cases", completed.size)
            result.putString("project_case_ids", completed.joinToString(","))
            result.putString("stream", "Session project smoke passed: literal current project, selection and Close are read-only, durable one-time Move, uncertain receipt GET without POST replay, changed-route refusal, and stale navigation read rejection.\n")
            finish(Activity.RESULT_OK, result)
        } catch (error: Throwable) {
            result.putString("stream", "Session project smoke failed: ${error.stackTraceToString()}\n")
            finish(Activity.RESULT_CANCELED, result)
        }
    }
    private fun <T> main(action: () -> T): T {
        val result = AtomicReference<Result<T>>()
        runOnMainSync { result.set(runCatching(action)) }
        return result.get().getOrThrow()
    }
    private fun await(description: String, condition: () -> Boolean) {
        val until = minOf(end, SystemClock.elapsedRealtime() + 10_000)
        while (!condition()) {
            check(SystemClock.elapsedRealtime() < until) { "Timed out: $description" }
            SystemClock.sleep(40)
        }
    }
    private fun find(label: String, root: AccessibilityNodeInfo? = uiAutomation.rootInActiveWindow): AccessibilityNodeInfo? {
        if (root == null) return null
        root.refresh()
        if (root.text?.toString() == label || root.contentDescription?.toString() == label) return root
        for (index in 0 until root.childCount) find(label, root.getChild(index))?.let { return it }
        return null
    }
    private fun node(label: String): AccessibilityNodeInfo {
        var found: AccessibilityNodeInfo? = null
        await(label) { found = find(label); found != null }
        return checkNotNull(found)
    }
    private fun action(label: String): AccessibilityNodeInfo {
        var found = node(label)
        while (true) {
            found.refresh()
            if (found.isClickable || !found.isEnabled || found.parent == null) break
            found = checkNotNull(found.parent)
        }
        return found
    }
    private fun tap(label: String) {
        val current = action(label)
        check(current.isEnabled && current.isClickable && current.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { "Cannot activate $label" }
    }
    private val target = Target("computer", "claude/fixture/task", "run-one", "conversation-one", "workspace")
    private val native = "11111111-1111-4111-8111-111111111111"
    private val swarm = "22222222-2222-4222-8222-222222222222"
    private val literalName = "Mobile ' \$(literal)"
    private inner class Host : SessionProjectAccess {
        var selected = target
        var owners: Any = Any()
        var connection = Connection("workspace", "Synthetic", "https://relay.example.invalid", "synthetic-token")
        val machine = Machine("workspace", "computer", "Synthetic", true, route = MachineRoute.Direct, machineId = native)
        var records by mutableStateOf(listOf<SessionAction>())
        var barrier: CompletableDeferred<Unit>? = null
        var persistEntered = false
        var refreshes = 0
        val order = mutableListOf<String>()
        override fun capture(target: Target) = SessionProjectHost(target, connection, machine, 1, owners)
        override fun current(captured: SessionProjectHost) = selected == captured.target && owners === captured.connectionGeneration &&
            connection == captured.connection && captured.route == SessionProjectProtocol.fingerprint(connection, machine)
        override fun reason(target: Target) = ""
        override fun mutationReason(target: Target, ownRequest: String) =
            if (records.any { it.target == target && it.blocksSending && it.requestId != ownRequest }) "Pending" else ""
        override fun actions() = records
        override fun flight(id: String, active: Boolean) = Unit
        override suspend fun persist(action: SessionAction) {
            persistEntered = true
            barrier?.await()
            order += "persist-${action.status}"
            records = records.filterNot { it.requestId == action.requestId } + action
        }
        override suspend fun settle(action: SessionAction, receipt: JSONObject) {
            records = records.filterNot { it.requestId == action.requestId } + SessionProjectProtocol.result(action, receipt)
        }
        override fun refresh() { refreshes++ }
    }
    private inner class Network(private val host: Host) : SessionProjectNetwork {
        private val expected = target
        var writes = 0
        var gets = 0
        var reads = 0
        var lost = false
        var heldRead: CompletableDeferred<Unit>? = null
        var original: JSONObject? = null
        private fun reply(body: JSONObject, assigned: Boolean): JSONObject {
            val raw = JSONObject().put("schema", 1).put("request_id", body.getString("request_id"))
                .put("status", if (assigned) "assigned" else "ok").put("name", target.session)
                .put("run_id", target.run).put("conversation_id", target.conversation).put("machine_id", native)
                .put("swarm_id", swarm).put("membership_token", "a".repeat(64)).put("catalog_epoch", "b".repeat(64))
                .put("current_project_id", if (assigned) "mobile" else "current")
            if (assigned) raw.put("project_id", "mobile") else raw.put("next_cursor", JSONObject.NULL).put("projects", JSONArray()
                .put(JSONObject().put("id", "current").put("name", "Current fixture").put("color", "#123456"))
                .put(JSONObject().put("id", "mobile").put("name", literalName).put("color", "#654321")))
            return JSONObject().put("request_id", body.getString("request_id")).put("state", "completed").put("result", raw)
        }
        override suspend fun submit(connection: Connection, target: Target, operation: String, payload: JSONObject, id: String): JSONObject {
            check(connection == host.connection && target == expected)
            check(payload.getString("request_id") == id)
            if (operation == SessionProjectProtocol.read) {
                reads++
                heldRead?.await()
                return reply(payload, false)
            }
            check(operation == SessionProjectProtocol.assign)
            check(host.records.single().requestId == id && host.records.single().arguments.contains("\"project_id\":\"mobile\""))
            check(payload.getString("project_id") == "mobile" && !payload.has("directory") && !payload.has("route_fingerprint"))
            writes++
            host.order += "post"
            original = JSONObject(payload.toString())
            if (lost) throw IOException("Synthetic acknowledgement loss")
            return reply(payload, true)
        }
        override suspend fun await(connection: Connection, initial: JSONObject) = initial
        override suspend fun receipt(connection: Connection, id: String): JSONObject {
            check(connection == host.connection && id == checkNotNull(original).getString("request_id"))
            gets++
            return reply(checkNotNull(original), true)
        }
    }
    private fun smoke() {
        val activity = startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        val model = main { activity.javaClass.getDeclaredMethod("getModel").apply { isAccessible = true }.invoke(activity) as ZerusViewModel }
        await("private storage") { main { model.storageReady } }
        check(main { model.connections.isEmpty() }) { "The fixture requires a fresh private profile with no configured connection" }
        val scope = CoroutineScope(Dispatchers.Main.immediate + SupervisorJob())
        val host = Host()
        val network = Network(host)
        val controller = SessionProjectController(host, scope, network)
        var visible by mutableStateOf(true)
        try {
            main { activity.setContent { MaterialTheme { if (visible) SessionProjectDialog(controller, target) { visible = false } else Text("Picker closed") } } }
            node("Current project")
            node(literalName)
            check(!action("Move").isEnabled)
            tap(literalName)
            await("selected Move") { action("Move").isEnabled }
            check(main { network.writes == 0 && host.records.isEmpty() })
            completed += "current_selection"
            val move = Rect().also { action("Move").getBoundsInScreen(it) }
            val close = Rect().also { action("Close").getBoundsInScreen(it) }
            check(close.centerY() > move.centerY() || close.centerX() > move.centerX()) { "Close is not last in the dialog actions" }
            tap("Close")
            node("Picker closed")
            check(main { network.writes == 0 && host.records.isEmpty() })
            completed += "close_no_write"

            main { visible = true; host.barrier = CompletableDeferred(); network.lost = true }
            node(literalName)
            tap(literalName)
            tap("Move")
            await("durable intent barrier") { main { host.persistEntered } }
            check(main { network.writes == 0 && host.records.isEmpty() })
            check(!action("Move").isEnabled)
            main { host.barrier!!.complete(Unit) }
            node("Check original request")
            val original = main { host.records.single().also { check(it.status == "uncertain") }.requestId }
            check(main { network.writes == 1 && host.order == listOf("persist-sending", "post", "persist-uncertain") })
            completed += "durable_single_post"
            main { host.connection = host.connection.copy(token = "changed-route") }
            tap("Check original request")
            node("The original connection or computer changed. This request has not been retried.")
            check(main { network.gets == 0 && network.writes == 1 })
            completed += "route_refusal"
            main { host.connection = host.connection.copy(token = "synthetic-token") }
            tap("Check original request")
            await("same receipt completed") { main { host.records.single().status == "completed" } }
            check(main { network.gets == 1 && network.writes == 1 && host.records.single().requestId == original && host.refreshes == 1 })
            completed += "original_receipt"

            tap("Close")
            node("Picker closed")
            main { network.heldRead = CompletableDeferred(); visible = true }
            await("held read") { main { controller.loading } }
            main { host.selected = target.copy(run = "replacement-run"); network.heldRead!!.complete(Unit) }
            await("stale read rejected") { main { !controller.loading } }
            check(main { controller.catalog == null && network.writes == 1 && host.records.single().requestId == original })
            check(!action("Move").isEnabled)
            completed += "stale_navigation"
        } finally {
            main { visible = false; controller.dismiss(); scope.cancel() }
            waitForIdleSync()
            main { activity.finish() }
        }
    }
}
