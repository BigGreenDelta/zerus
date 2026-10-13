package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import java.util.UUID

@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun SpeechConfigurationScreen(model: ZerusViewModel, initial: SpeechSelection, onDismiss: () -> Unit) {
    val manager = model.speechConfiguration
    var hostMenu by remember { mutableStateOf(false) }
    var editor by remember { mutableStateOf<Pair<SpeechConfiguration, SpeechConfigProfile>?>(null) }
    var removal by remember { mutableStateOf<Pair<SpeechConfiguration, SpeechConfigProfile>?>(null) }
    var actionError by remember { mutableStateOf("") }
    val hosts = model.machines.filter { machine -> model.connections.any { it.id == machine.connectionId } }
        .map { SpeechConfigHost(MachineKey(it.connectionId, it.id), it.machineId, it.name) }
    val selected = manager.host
    val config = manager.configuration
    LaunchedEffect(manager.ready) {
        if (manager.ready) {
            hosts.find { it.key.connectionId == initial.host?.connectionId && it.machineId == initial.host?.computerId }
                ?.let(manager::select)
            if (manager.host != null && manager.configuration == null) manager.refresh()
        }
    }
    ObscureConversation()
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            Scaffold(topBar = { TopAppBar(title = { Text("Manage speech providers") }, navigationIcon = {
                IconButton(onClick = onDismiss) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Back") }
            }) }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().verticalScroll(rememberScrollState())
                    .padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("Providers are saved on the processing computer. Changing them does not select a different provider for a saved recording.", style = MaterialTheme.typography.bodySmall)
                    Box {
                        OutlinedButton(onClick = { hostMenu = true }, enabled = manager.ready && !model.updateInstallPreparing, modifier = Modifier.fillMaxWidth()) {
                            Text(selected?.name ?: "Choose processing computer")
                        }
                        DropdownMenu(expanded = hostMenu, onDismissRequest = { hostMenu = false }) {
                            hosts.forEach { host -> DropdownMenuItem(text = { Column {
                                Text(host.name)
                                if (model.connections.size > 1) Text(model.connections.find { it.id == host.key.connectionId }?.displayName.orEmpty(), style = MaterialTheme.typography.bodySmall)
                            } }, onClick = { hostMenu = false; editor = null; removal = null; actionError = ""; manager.select(host) }) }
                        }
                    }
                    SpeechDocumentationButton()
                    if (hosts.isEmpty()) Text("Connect a computer to manage its speech providers.")
                    if (selected != null) {
                        manager.reason(selected).takeIf { it.isNotBlank() }?.let { Text(it) }
                        TextButton(onClick = { manager.refresh() }, enabled = manager.ready && !manager.loading && manager.reason(selected).isBlank()) { Text("Refresh configuration") }
                    }
                    if (manager.loading) LinearProgressIndicator(Modifier.fillMaxWidth())
                    if (manager.notice.isNotBlank()) Text(manager.notice, style = MaterialTheme.typography.bodySmall)
                    if (actionError.isNotBlank()) Text(actionError, color = MaterialTheme.colorScheme.error)
                    manager.pending.filter { it.host.key == selected?.key && it.host.machineId == selected?.machineId }.forEach { operation ->
                        Card { Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("Unconfirmed configuration operation", style = MaterialTheme.typography.titleSmall)
                            Text("This operation has not been repeated.", style = MaterialTheme.typography.bodySmall)
                            TextButton(onClick = { manager.checkStatus(operation) }, enabled = operation.id !in manager.active && manager.reason(operation.host).isBlank()) { Text("Check operation status") }
                        } }
                    }
                    if (selected != null && config != null) {
                        Text(speechServiceReadiness(selected, config, manager.loading), style = MaterialTheme.typography.bodySmall)
                        config.profiles.forEach { profile ->
                            Card { Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                Text(profile.name, style = MaterialTheme.typography.titleMedium)
                                Text("${if (profile.adapter == "gigaam") "Local GigaAM" else if (profile.adapter == "openai_http") "OpenAI-compatible server" else profile.adapter} (${profile.model})", style = MaterialTheme.typography.bodySmall)
                                if (profile.adapter == "openai_http") Text(profile.baseUrl, style = MaterialTheme.typography.bodySmall)
                                Text(if (profile.available) "Available" else profile.reason.ifBlank { "Unavailable" }, style = MaterialTheme.typography.bodySmall)
                                if (profile.credentialConfigured) Text("API key configured", style = MaterialTheme.typography.labelSmall)
                                val isDefault = model.speech.selection == SpeechSelection(MachineKey(selected.key.connectionId, selected.machineId), profile.id, profile.revision)
                                if (isDefault) Text("Used for new recordings", style = MaterialTheme.typography.labelSmall)
                                else TextButton(onClick = { if (!manager.useForNewRecordings(profile)) actionError = "Refresh and review this provider before selecting it." },
                                    enabled = manager.canMutate(selected) && profile.available && !model.speech.recording && !model.speech.savingRecording) { Text("Use for new recordings") }

                                Row {
                                    TextButton(onClick = { editor = config to profile }, enabled = manager.canMutate(selected)) { Text("Edit") }
                                    TextButton(onClick = { if (!manager.apply(SpeechConfigProtocol.test(selected, profile))) actionError = "Refresh and review this provider before testing." },
                                        enabled = manager.canMutate(selected) && config.serviceAvailable) { Text("Test") }
                                    TextButton(onClick = { removal = config to profile }, enabled = manager.canMutate(selected)) { Text("Remove") }
                                }
                            } }
                        }
                        Text("Test sends a short synthetic recording from this computer to the saved provider. It does not use your microphone or saved recordings.", style = MaterialTheme.typography.bodySmall)
                        OutlinedButton(onClick = {
                            val adapter = config.adapters.firstOrNull { it.id in setOf("gigaam", "openai_http") } ?: return@OutlinedButton
                            editor = config to SpeechConfigProfile(UUID.randomUUID().toString(), 0, "", adapter.id,
                                adapter.models.firstOrNull().orEmpty(), "ru", adapter.devices.firstOrNull().orEmpty())
                        }, enabled = manager.canMutate(selected) && config.profiles.size < 32 && config.adapters.any { it.id in setOf("gigaam", "openai_http") }) { Text("Add provider") }
                    }
                }
            }
        }
    }
    if (selected != null) editor?.let { (baseline, profile) ->
        SpeechConfigurationEditor(manager, selected, baseline, profile) { editor = null }
    }
    if (selected != null) removal?.let { (baseline, profile) ->
        ZerusAlertDialog(onDismissRequest = { removal = null }, title = { Text("Remove speech provider?") },
            text = { Text("Remove ${profile.name} from ${selected.name}? Saved recordings and voice history are kept. Clients using this provider will need an explicit new selection.") },
            confirmButton = { TextButton(onClick = {
                removal = null
                if (!manager.apply(SpeechConfigProtocol.remove(selected, baseline, profile))) actionError = "The configuration changed. Refresh and confirm removal again."
            }, enabled = manager.canMutate(selected)) { Text("Remove provider") } },
            dismissButton = { TextButton(onClick = { removal = null }) { Text("Keep provider") } })
    }
}

@Composable private fun ProviderMenu(label: String, value: String, choices: List<Pair<String, String>>, enabled: Boolean, onChange: (String) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { open = true }, enabled = enabled && choices.isNotEmpty(), modifier = Modifier.fillMaxWidth()) {
            Text("$label: ${choices.find { it.first == value }?.second ?: value.ifBlank { "Choose" }}")
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            choices.forEach { (id, name) -> DropdownMenuItem(text = { Text(name) }, onClick = { open = false; onChange(id) }) }
        }
    }
}

@Composable private fun SpeechConfigurationEditor(manager: SpeechConfigurationController, host: SpeechConfigHost,
    initial: SpeechConfiguration, original: SpeechConfigProfile, onDismiss: () -> Unit) {
    var baseline by remember(host, original.id) { mutableStateOf(initial) }
    var profile by remember(host, original.id) { mutableStateOf(original) }
    var credentialMode by remember(host, original.id) { mutableStateOf("keep") }
    var secret by remember(host, original.id) { mutableStateOf("") } // Never saved into instance state or private preferences.
    var error by remember(host, original.id) { mutableStateOf("") }
    var operationId by remember(host, original.id) { mutableStateOf<String?>(null) }
    val pending = manager.pending.find { it.id == operationId }
    val editable = pending == null && manager.canMutate(host)
    val latest = manager.configuration
    LaunchedEffect(manager.result, operationId) {
        manager.result?.takeIf { it.id == operationId }?.let {
            if (it.state == "completed") { secret = ""; onDismiss() }
            else if (it.state == "failed") error = manager.notice
        }
    }
    fun dismiss() { secret = ""; onDismiss() }
    ZerusAlertDialog(onDismissRequest = ::dismiss, title = { Text(if (original.revision == 0L) "Add speech provider" else "Edit speech provider") },
        text = { Column(Modifier.heightIn(max = 560.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Processing computer: ${host.name}", style = MaterialTheme.typography.labelMedium)
            Text(speechServiceReadiness(host, latest, manager.loading), style = MaterialTheme.typography.bodySmall)
            Text("Provider ID: ${profile.id}", style = MaterialTheme.typography.labelSmall)
            OutlinedTextField(profile.name, { profile = profile.copy(name = it) }, label = { Text("Name") }, singleLine = true, enabled = editable)
            ProviderMenu("Type", profile.adapter, baseline.adapters.filter { it.id in setOf("gigaam", "openai_http") }.map {
                it.id to if (it.id == "gigaam") "Local GigaAM" else "OpenAI-compatible server"
            }, editable) { id ->
                val adapter = baseline.adapters.first { it.id == id }
                profile = profile.copy(adapter = id, model = adapter.models.firstOrNull().orEmpty(), device = adapter.devices.firstOrNull().orEmpty(), baseUrl = "", allowInsecureHttp = false)
                secret = ""; credentialMode = if (id == "gigaam") "remove" else "keep"
            }
            SpeechDocumentationButton(profile.adapter)
            if (profile.adapter == "gigaam") {
                val adapter = baseline.adapters.find { it.id == "gigaam" }
                Text("Local inference runs on ${host.name}. Prepare its Python/PyTorch speech service before testing this profile.", style = MaterialTheme.typography.bodySmall)
                ProviderMenu("Model", profile.model, adapter?.models.orEmpty().map { it to it }, editable) { profile = profile.copy(model = it) }
                Text("v3_e2e_rnnt is the GigaAM v3 end-to-end model used by the standard Russian setup.", style = MaterialTheme.typography.bodySmall)
                ProviderMenu("Device", profile.device, adapter?.devices.orEmpty().map { it to it }, editable) { profile = profile.copy(device = it) }
                Text(when {
                    profile.device == "cpu" -> "CPU runs without a GPU. It is the baseline setup choice."
                    profile.device.startsWith("cuda") -> "CUDA requires an NVIDIA GPU, compatible driver and PyTorch build on ${host.name}. A numbered device selects that GPU index."
                    profile.device == "mps" -> "MPS requires a compatible Apple GPU and installed runtime. Verify this model with Test before using it."
                    else -> "Choose a device prepared on ${host.name}, then verify the saved profile with Test."
                }, style = MaterialTheme.typography.bodySmall)
                Text("Saving does not install packages or download model weights.", style = MaterialTheme.typography.bodySmall)
            } else if (profile.adapter == "openai_http") {
                OutlinedTextField(profile.baseUrl, { profile = profile.copy(baseUrl = it) }, label = { Text("Server base URL") },
                    placeholder = { Text("https://speech.example.test/v1") }, keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri), singleLine = true, enabled = editable)
                Text("${host.name} sends WAV audio to this server’s /audio/transcriptions endpoint. Local GigaAM and GPU setup are not required for this provider.", style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(profile.model, { profile = profile.copy(model = it) }, label = { Text("Model") }, singleLine = true, enabled = editable)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(profile.allowInsecureHttp, { profile = profile.copy(allowInsecureHttp = it) }, enabled = editable)
                    Text("Allow HTTP on local/private networks", style = MaterialTheme.typography.bodySmall)
                }
                ProviderMenu("API key", credentialMode, listOf("keep" to if (original.credentialConfigured) "Keep saved key" else "No key", "replace" to "Enter new key", "remove" to "Remove saved key"), editable) {
                    credentialMode = it; secret = ""
                }
                if (credentialMode == "replace") OutlinedTextField(secret, { secret = it }, label = { Text("New API key") }, singleLine = true,
                    visualTransformation = PasswordVisualTransformation(), keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), enabled = editable)
                Text("The key is encrypted for ${host.name} before upload. Saved keys are never returned to this phone.", style = MaterialTheme.typography.bodySmall)
            }
            OutlinedTextField(profile.language, { profile = profile.copy(language = it) }, label = { Text("Language") }, singleLine = true, enabled = editable)
            if (profile.adapter == "gigaam") Text("Use ru for Russian speech.", style = MaterialTheme.typography.bodySmall)
            if (latest != null && latest.machineId == baseline.machineId && latest.revision != baseline.revision) {
                Text("The configuration has changed. Review your edits against revision ${latest.revision} before saving.")
                TextButton(onClick = { baseline = latest; error = "" }, enabled = pending == null) { Text("Use refreshed revision") }
            }
            if (error.isNotBlank()) Text(error, color = MaterialTheme.colorScheme.error)
            if (pending != null) {
                Text(manager.notice.ifBlank { "Waiting for the computer. This operation will not be repeated." })
                TextButton(onClick = { manager.checkStatus(pending) }, enabled = pending.id !in manager.active) { Text("Check operation status") }
            } else TextButton(onClick = { manager.refresh() }, enabled = !manager.loading) { Text("Refresh configuration") }
            if (manager.loading || pending?.id in manager.active) LinearProgressIndicator(Modifier.fillMaxWidth())
        } },
        confirmButton = { TextButton(onClick = {
            try {
                val operation = SpeechConfigProtocol.upsert(host, baseline, profile, credentialMode, secret)
                secret = "" // Only the sealed envelope may survive transport or recovery.
                if (manager.apply(operation)) { operationId = operation.id; error = "" }
                else error = "The configuration changed. Refresh and review your edits before saving."
            } catch (failure: Exception) { error = failure.message ?: "Check the provider fields." }
        }, enabled = editable) { Text("Save provider") } },
        dismissButton = { TextButton(onClick = ::dismiss) { Text("Close") } })
}
