package app.zerus.mobile

import android.content.Intent
import android.net.Uri
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.*
import androidx.compose.ui.platform.LocalContext

internal data class SpeechDocumentation(val label: String, val url: String)

internal fun speechDocumentation(adapter: String? = null): SpeechDocumentation {
    val base = "https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"
    return when (adapter) {
        "gigaam" -> SpeechDocumentation("Local GigaAM setup", "$base#local-gigaam")
        "openai_http" -> SpeechDocumentation("OpenAI-compatible server setup", "$base#openai-compatible-server")
        else -> SpeechDocumentation("Speech provider documentation", base)
    }
}

internal fun openSpeechDocumentation(adapter: String?, launch: (String) -> Unit): String = try {
    launch(speechDocumentation(adapter).url)
    ""
} catch (_: Exception) {
    "Could not open the documentation. Check that a browser is available and try again."
}

@Composable internal fun SpeechDocumentationButton(adapter: String? = null) {
    val context = LocalContext.current
    var error by remember(adapter) { mutableStateOf("") }
    Column {
        TextButton(onClick = {
            error = openSpeechDocumentation(adapter) { url ->
                context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE))
            }
        }) { Text(speechDocumentation(adapter).label) }
        if (error.isNotEmpty()) Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
    }
}

internal fun speechServiceReadiness(host: SpeechConfigHost, configuration: SpeechConfiguration?, loading: Boolean): String = when {
    loading -> "Checking the speech service on ${host.name}…"
    configuration == null || configuration.machineId != host.machineId -> "Speech service status on ${host.name} is not confirmed. Refresh configuration to check."
    configuration.serviceAvailable -> "Last check: the speech service on ${host.name} responds. Test the saved provider before using it."
    else -> "Last check: the speech service on ${host.name} is unavailable. " + configuration.serviceReason.ifBlank { "Prepare or start it on that computer, then refresh." }
}
