package app.zerus.mobile

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.viewmodel.compose.viewModel

/** A separate full window keeps the current app content and reading anchors composed. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun AppSettingsScreen(model: ZerusViewModel, updater: AndroidUpdateViewModel, onDismiss: () -> Unit) {
    ObscureConversation()
    val context = LocalContext.current
    val preferences: AppSettingsViewModel = viewModel()
    var notices by rememberSaveable { mutableStateOf(false) }
    var details by rememberSaveable { mutableStateOf(false) }
    var linkError by rememberSaveable { mutableStateOf(false) }
    val settingsScroll = rememberScrollState()
    val noticesScroll = rememberScrollState()
    fun back() { if (notices) notices = false else if (!preferences.saving) onDismiss() }
    LaunchedEffect(preferences) { preferences.refresh(); AppTelemetry.screen(TelemetryScreen.Settings) }
    Dialog(onDismissRequest = ::back, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            Scaffold(topBar = {
                TopAppBar(title = { Text(if (notices) "Third-party notices" else "Settings", maxLines = 1, overflow = TextOverflow.Ellipsis) }, navigationIcon = {
                    IconButton(onClick = ::back, enabled = !preferences.saving) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
                })
            }) { padding ->
                val body = Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().padding(horizontal = 16.dp)
                if (notices) ThirdPartyContent(noticesScroll, body.padding(vertical = 16.dp))
                else Column(body.verticalScroll(settingsScroll).padding(vertical = 16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    Text("Privacy", style = MaterialTheme.typography.titleLarge)
                    if (BuildConfig.FIREBASE_ENABLED) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) { Text("Usage analytics"); Text("Share app usage, without conversation content.", style = MaterialTheme.typography.bodySmall) }
                            Switch(preferences.value.analytics, { if (!model.updateInstallPreparing) preferences.change(true, it) }, enabled = !preferences.saving && !model.updateInstallPreparing)
                        }
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) { Text("Crash reports"); Text("Share technical crash diagnostics. Changes apply after restart; reports usually upload on the next launch.", style = MaterialTheme.typography.bodySmall) }
                            Switch(preferences.value.crashes, { if (!model.updateInstallPreparing) preferences.change(false, it) }, enabled = !preferences.saving && !model.updateInstallPreparing)
                        }
                        Text("These switches do not change push notifications.", style = MaterialTheme.typography.bodySmall)
                        TextButton(onClick = { details = !details }) { Text(if (details) "Hide privacy details" else "Privacy details") }
                        if (details) {
                            Text("Firebase receives app and device information with SDK installation identifiers. Session text, names, paths, account details and connection credentials are excluded from custom events and custom diagnostic fields.", style = MaterialTheme.typography.bodySmall)
                            Text("Automatic crash diagnostics include exception stacks, thread names and technical network metadata, which may include gateway addresses.", style = MaterialTheme.typography.bodySmall)
                            Text("Opting out discards retained reports. After re-enabling, diagnostics resume once a startup confirms the earlier reports have been removed.", style = MaterialTheme.typography.bodySmall)
                        }
                    } else Text("This build does not include Firebase analytics or crash reporting.")
                    if (preferences.error) Text("Could not fully apply this setting. Check the switch and try again.", color = MaterialTheme.colorScheme.error)
                    HorizontalDivider()
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("About Zerus", style = MaterialTheme.typography.titleMedium)
                            Text("${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        val connectedMachines = model.machines.filter { machine -> model.connections.any { it.id == machine.connectionId } }
                        if (connectedMachines.isEmpty()) Text("Connect a computer to see its hgs CLI version.", style = MaterialTheme.typography.bodySmall)
                        else model.connections.forEach { connection ->
                            val computers = connectedMachines.filter { it.connectionId == connection.id }
                            if (computers.isNotEmpty() && model.connections.size > 1) Text(connection.displayName, style = MaterialTheme.typography.labelLarge)
                            computers.forEach { machine ->
                                val version = when {
                                    machine.hgsVersion.isNotBlank() -> "hgs CLI ${machine.hgsVersion}"
                                    !machine.online -> "hgs CLI version unavailable while offline"
                                    else -> "hgs CLI version unavailable"
                                }
                                val suffix = if (machine.hgsVersion.isNotBlank() && (!machine.online || machine.lastKnown)) " (last known)" else ""
                                SelectionContainer { Text("${machine.name}: $version$suffix", style = MaterialTheme.typography.bodySmall) }
                            }
                        }
                        TextButton(onClick = {
                            try {
                                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("https://github.com/ufna/zerus")))
                                linkError = false
                            } catch (_: Exception) { linkError = true }
                        }, modifier = Modifier.heightIn(min = 48.dp).semantics { contentDescription = "Open ufna/zerus on GitHub" }, contentPadding = PaddingValues(horizontal = 0.dp, vertical = 8.dp)) {
                            Icon(painterResource(R.drawable.ic_github), contentDescription = null, modifier = Modifier.size(20.dp))
                            Spacer(Modifier.width(8.dp))
                            Text("ufna/zerus")
                        }
                        if (linkError) Text("No browser could open GitHub. Try again after installing a browser.", color = MaterialTheme.colorScheme.error)
                    }
                    HorizontalDivider()
                    Text("Updates", style = MaterialTheme.typography.titleLarge)
                    UpdatesContent(model, updater, interactionBlocked = { preferences.saving })
                    HorizontalDivider()
                    TextButton(onClick = { notices = true }, enabled = !preferences.saving) { Text("Third-party notices") }
                }
            }
        }
    }
}
