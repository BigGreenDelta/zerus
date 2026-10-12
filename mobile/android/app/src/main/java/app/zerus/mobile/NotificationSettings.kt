package app.zerus.mobile

import android.content.Context
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object NotificationSettings {
    internal val deliveryLock = Any()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    val state = MutableStateFlow(NotificationPreferences())
    suspend fun load(context: Context) = withContext(Dispatchers.IO) {
        synchronized(deliveryLock) { state.value = PrivateStore(context).notificationPreferences() }
    }
    fun masterChanged(context: Context, enabled: Boolean) {
        val store = PrivateStore(context)
        val old = store.notificationPreferences()
        val generation = nextGeneration(old.generation)
        val next = old.copy(master = enabled, generation = generation,
            inputEnabledAt = if (enabled && old.input) generation else old.inputEnabledAt,
            errorsEnabledAt = if (enabled && old.errors) generation else old.errorsEnabledAt,
            finishedEnabledAt = if (enabled && old.finished) generation else old.finishedEnabledAt,
            decisionsEnabledAt = if (enabled && old.decisions) generation else old.decisionsEnabledAt)
        store.saveNotificationPreferences(next)
        state.value = next
        SessionNotifications.cancelDisabled(context, next)
        scope.launch { store.connections().forEach { SessionNotifications.enqueue(context, it.id) } }
    }
    fun set(context: Context, kind: SessionAlertKind, enabled: Boolean) = synchronized(deliveryLock) {
        val store = PrivateStore(context)
        val old = store.notificationPreferences()
        val generation = nextGeneration(old.generation)
        val next = when (kind) {
            SessionAlertKind.Input -> old.copy(input = enabled, inputEnabledAt = if (enabled && !old.input) generation else old.inputEnabledAt)
            SessionAlertKind.Error -> old.copy(errors = enabled, errorsEnabledAt = if (enabled && !old.errors) generation else old.errorsEnabledAt)
            SessionAlertKind.Finished -> old.copy(finished = enabled, finishedEnabledAt = if (enabled && !old.finished) generation else old.finishedEnabledAt)
            SessionAlertKind.Decision -> old.copy(decisions = enabled, decisionsEnabledAt = if (enabled && !old.decisions) generation else old.decisionsEnabledAt)
        }.copy(generation = generation)
        store.saveNotificationPreferences(next)
        state.value = next
        SessionNotifications.cancelDisabled(context, next)
        scope.launch { store.connections().forEach { SessionNotifications.enqueue(context, it.id) } }
    }
    private fun nextGeneration(value: Long) = if (value == Long.MAX_VALUE) 0 else value + 1
}

@Composable internal fun NotificationTypeSettings(master: Boolean) {
    val context = LocalContext.current.applicationContext
    val preferences by NotificationSettings.state.collectAsState()
    val scope = rememberCoroutineScope()
    var ready by remember { mutableStateOf(false) }
    var saving by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        try { NotificationSettings.load(context); ready = true }
        catch (_: Exception) { error = true }
    }
    listOf(Triple(SessionAlertKind.Input, "Input and approvals", preferences.input),
        Triple(SessionAlertKind.Error, "Errors", preferences.errors),
        Triple(SessionAlertKind.Finished, "Turn finished", preferences.finished),
        Triple(SessionAlertKind.Decision, "Task decisions (Your Call)", preferences.decisions)).forEach { (kind, caption, enabled) ->
        Row(Modifier.fillMaxWidth().padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(caption, Modifier.weight(1f))
            Switch(enabled, onCheckedChange = { value ->
                saving = true
                scope.launch {
                    try { withContext(Dispatchers.IO) { NotificationSettings.set(context, kind, value) }; error = false }
                    catch (_: Exception) { error = true }
                    finally { saving = false }
                }
            }, enabled = master && ready && !saving)
        }
    }
    if (error) Text("Notification choices could not be saved. Try again.", style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.error)
}

/** One shared choice per workspace; the gateway computer's hgs stores it for every swarm desktop. */
@Composable internal fun MobileDeliverySettings(model: ZerusViewModel) {
    if (model.connections.isEmpty()) return
    Text("When I'm not at a computer, send notifications to my phone", Modifier.padding(start = 16.dp))
    model.connections.forEach { connection ->
        val gateway = model.mobileDeliveryGateway(connection.id)
        val action = model.mobileDeliveryAction(connection.id)
        val reason = model.mobileDeliveryReason(connection.id)
        val now = System.currentTimeMillis()
        val selected = MobileDelivery.shown(gateway?.mobileDelivery.orEmpty(), action, now)
        Column(Modifier.fillMaxWidth().padding(start = 16.dp).selectableGroup(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            if (model.connections.size > 1) Text(connection.displayName, style = MaterialTheme.typography.labelLarge)
            MobileDelivery.values.forEach { value ->
                Row(Modifier.fillMaxWidth().heightIn(min = 48.dp).selectable(selected = value == selected, enabled = reason.isBlank(),
                    role = Role.RadioButton, onClick = { model.setMobileDelivery(connection.id, value) }),
                    verticalAlignment = Alignment.CenterVertically) {
                    RadioButton(selected = value == selected, onClick = null, enabled = reason.isBlank())
                    Spacer(Modifier.width(8.dp))
                    Text(MobileDelivery.label(value))
                }
            }
            Text(MobileDelivery.status(gateway?.name.orEmpty(), gateway?.mobileDelivery.orEmpty(), action, reason, now),
                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
    if (model.mobileDeliveryError.isNotBlank()) Row(Modifier.padding(start = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(model.mobileDeliveryError, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.error)
        TextButton(onClick = { model.dismissMobileDeliveryError() }) { Text("Dismiss") }
    }
}
