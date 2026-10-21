package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class OpenCodeModel(val value: String, val label: String, val provider: String, val providerLabel: String,
    val free: Boolean, val favorite: Boolean, val isDefault: Boolean, val efforts: List<NativeEffort>)
data class OpenCodeProvider(val id: String, val label: String, val kind: String, val credentials: Int?, val activeLabel: String, val models: Int?)
data class OpenCodeGroup(val provider: String, val label: String, val models: List<OpenCodeModel>)
data class OpenCodeAccountCatalog(val state: String, val reason: String, val catalogAt: Double?, val providers: List<OpenCodeProvider>,
    val providersTruncated: Boolean, val modelsTruncated: Boolean)

/** The bounded display projection shared by launch, session settings and account details. */
object OpenCodeCatalog {
    const val MAX_MODELS = 256
    const val MAX_PROVIDERS = 64
    const val PERSISTED_PROVIDERS = 16
    private val providerId = Regex("[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}")
    private val kinds = setOf("free", "key", "oauth", "env", "config", "command", "external", "local")
    private fun text(raw: JSONObject, key: String, max: Int): String = (raw.opt(key) as? String)
        ?.takeIf { it.isNotBlank() && it.length <= max && it.none(Char::isISOControl) }.orEmpty()
    private fun count(raw: JSONObject, key: String, max: Int): Int? = (raw.opt(key) as? Number)?.toDouble()
        ?.takeIf { it.isFinite() && it >= 0 && it <= max && it % 1.0 == 0.0 }?.toInt()

    fun models(raw: JSONArray?): List<OpenCodeModel> {
        if (raw == null) return emptyList()
        return (0 until raw.length()).asSequence().mapNotNull { index ->
            val row = raw.optJSONObject(index) ?: return@mapNotNull null
            val value = ((row.opt("value") ?: row.opt("id")) as? String)
                ?.takeIf(LaunchPresentation::validModel) ?: return@mapNotNull null
            val provider = text(row, "provider", 80).takeIf { providerId.matches(it) }.orEmpty()
            OpenCodeModel(value, text(row, "label", 160).ifBlank { value }, provider,
                text(row, "provider_label", 160).ifBlank { provider }, row.opt("free") == true,
                row.opt("favorite") == true, row.opt("default") == true, NativeEfforts.parse(row))
        }.distinctBy { it.value }.take(MAX_MODELS).toList()
    }

    fun providers(raw: JSONArray?, limit: Int = MAX_PROVIDERS): List<OpenCodeProvider> {
        if (raw == null) return emptyList()
        return (0 until raw.length()).asSequence().mapNotNull { index ->
            val row = raw.optJSONObject(index) ?: return@mapNotNull null
            val id = text(row, "id", 80).takeIf { providerId.matches(it) } ?: return@mapNotNull null
            OpenCodeProvider(id, text(row, "label", 160).ifBlank { id }, text(row, "kind", 32).takeIf { it in kinds }.orEmpty(),
                count(row, "credential_count", 64), text(row, "active_label", 80), count(row, "model_count", 10_000))
        }.distinctBy { it.id }.take(limit.coerceIn(0, MAX_PROVIDERS)).toList()
    }

    fun account(raw: JSONObject): OpenCodeAccountCatalog? {
        if (raw.opt("provider") != "opencode") return null
        val at = (raw.opt("catalog_at") as? Number)?.toDouble()?.takeIf { it.isFinite() && it >= 0 && it <= 9_007_199_254_740_991.0 }
        return OpenCodeAccountCatalog(text(raw, "catalog_state", 32), text(raw, "catalog_reason", 500), at,
            providers(raw.optJSONArray("providers")), raw.opt("providers_truncated") == true || (raw.optJSONArray("providers")?.length() ?: 0) > MAX_PROVIDERS,
            raw.opt("model_options_truncated") == true || (raw.optJSONArray("model_options")?.length() ?: 0) > MAX_MODELS)
    }

    fun group(models: List<OpenCodeModel>): List<OpenCodeGroup> = models.groupBy { it.provider }.map { (id, entries) ->
        OpenCodeGroup(id, entries.first().providerLabel.ifBlank { id.ifBlank { "Other models" } }, entries)
    }
    fun filter(models: List<OpenCodeModel>, query: String): List<OpenCodeModel> {
        val words = query.trim().split(Regex("\\s+")).filter(String::isNotEmpty)
        return models.filter { model -> words.all { word ->
            listOf(model.label, model.value, model.providerLabel, model.provider).any { it.contains(word, ignoreCase = true) }
        } }
    }
    fun marks(model: OpenCodeModel): List<String> = buildList {
        if (model.isDefault) add("Default")
        if (model.favorite) add("Favorite")
        if (model.free) add("Free")
    }
    fun kindLabel(kind: String): String = when (kind) {
        "free" -> "Free, no key"; "key" -> "API key"; "oauth" -> "Signed in"; "env" -> "Environment variable"
        "config" -> "Configuration"; "command" -> "Command"; "external" -> "External credentials"; "local" -> "Detected locally"
        else -> "Connected"
    }
    fun stateMessage(state: String, reason: String): String = when (state) {
        "ready" -> ""
        "loading" -> "OpenCode is still loading providers."
        else -> reason.ifBlank { when (state) {
            "not_running" -> "OpenCode is not running on this computer."
            "disabled" -> "The OpenCode service is disabled on this computer."
            "incompatible" -> "This OpenCode version is not supported."
            "unreachable" -> "OpenCode did not answer."
            else -> ""
        } }
    }
    fun truncationMessage(models: Boolean, providers: Boolean): String = listOf(
        if (models) "Showing a shortened list. Choose other models on the desktop." else "",
        if (providers) "Some providers are not shown." else ""
    ).filter(String::isNotBlank).joinToString(" ")
}
