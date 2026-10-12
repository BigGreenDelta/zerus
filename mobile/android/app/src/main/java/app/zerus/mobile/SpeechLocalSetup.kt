package app.zerus.mobile

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

internal data class SpeechSetupStep(val title: String, val body: String, val commands: List<String>)
internal data class SpeechSetupGuide(val title: String, val intro: String, val steps: List<SpeechSetupStep>, val notes: List<String>) {
    companion object {
        fun parse(raw: String): SpeechSetupGuide {
            require(raw.toByteArray(Charsets.UTF_8).size <= 65_536)
            val value = JSONObject(raw); require(value.getInt("schema") == 1)
            fun text(value: String): String = value.also { require(it.isNotBlank() && it.length <= 16_384 && '\u0000' !in it) }
            fun strings(array: JSONArray?) = array?.let {
                require(it.length() <= 32)
                (0 until it.length()).map { index -> text(it.getString(index)) }
            }.orEmpty()
            val steps = value.getJSONArray("steps"); require(steps.length() in 1..32)
            return SpeechSetupGuide(text(value.getString("title")), text(value.getString("intro")), steps.objects().map {
                SpeechSetupStep(text(it.getString("title")), text(it.getString("body")), strings(it.optJSONArray("commands")))
            }, strings(value.optJSONArray("notes")))
        }
    }
}

internal fun speechServiceReadiness(host: SpeechConfigHost, configuration: SpeechConfiguration?, loading: Boolean): String = when {
    loading -> "Checking the speech service on ${host.name}…"
    configuration == null || configuration.machineId != host.machineId -> "Speech service status on ${host.name} is not confirmed. Refresh configuration to check."
    configuration.serviceAvailable -> "Last check: the speech service on ${host.name} responds. Test the saved profile to check its model and device."
    else -> "Last check: the speech service on ${host.name} is unavailable. " + configuration.serviceReason.ifBlank { "Prepare or start it on that computer, then refresh." }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun SpeechLocalSetupDialog(hostName: String, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val guide by produceState<Result<SpeechSetupGuide>?>(initialValue = null) {
        value = withContext(Dispatchers.IO) { runCatching {
            context.assets.open("speech-local-setup.json").bufferedReader().use { SpeechSetupGuide.parse(it.readText()) }
        } }
    }
    var copied by remember { mutableStateOf<String?>(null) }
    ObscureConversation()
    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            Scaffold(topBar = { TopAppBar(title = { Text("Local setup guide") }, navigationIcon = {
                IconButton(onClick = onDismiss) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Close setup guide") }
            }) }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().verticalScroll(rememberScrollState())
                    .padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(if (hostName.isNotBlank()) "Run these steps on $hostName." else "Run these steps on the processing computer you choose in provider settings.",
                        style = MaterialTheme.typography.titleMedium)
                    Text("This bundled guide works offline. Copy a command to run it yourself; opening this guide does not change the computer.", style = MaterialTheme.typography.bodySmall)
                    val loaded = guide
                    when {
                        loaded == null -> LinearProgressIndicator(Modifier.fillMaxWidth())
                        loaded.isFailure -> Text("The bundled setup guide could not be opened. Update Zerus and try again.")
                        else -> loaded.getOrThrow().let { document ->
                            Text(document.title, style = MaterialTheme.typography.headlineSmall)
                            SelectionContainer { Text(document.intro) }
                            document.steps.forEach { step ->
                                Text(step.title, style = MaterialTheme.typography.titleMedium)
                                SelectionContainer { Text(step.body) }
                                step.commands.forEachIndexed { index, command ->
                                    val label = if (step.commands.size == 1) "Copy commands: ${step.title}" else "Copy commands ${index + 1}: ${step.title}"
                                    Card { Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                        SelectionContainer { Text(command, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall) }
                                        TextButton(onClick = { clipboard.setText(AnnotatedString(command)); copied = label }) { Text(label) }
                                        if (copied == label) Text("Copied", style = MaterialTheme.typography.labelSmall)
                                    } }
                                }
                            }
                            document.notes.forEach { SelectionContainer { Text(it, style = MaterialTheme.typography.bodySmall) } }
                        }
                    }
                }
            }
        }
    }
}
