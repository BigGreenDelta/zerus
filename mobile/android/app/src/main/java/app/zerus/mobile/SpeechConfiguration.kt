package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.net.URI
import java.util.UUID

internal data class SpeechConfigHost(val key: MachineKey, val machineId: String, val name: String)
internal data class SpeechConfigProfile(val id: String, val revision: Long, val name: String, val adapter: String,
    val model: String, val language: String, val device: String = "", val baseUrl: String = "",
    val allowInsecureHttp: Boolean = false, val credentialConfigured: Boolean = false,
    val available: Boolean = false, val reason: String = "") {
    fun fields() = JSONObject().put("id", id).put("name", name).put("adapter", adapter).put("model", model)
        .put("language", language).apply {
            if (adapter == "gigaam") put("device", device)
            else { put("base_url", baseUrl); put("allow_insecure_http", allowInsecureHttp) }
        }
}
internal data class SpeechConfigAdapter(val id: String, val models: List<String>, val devices: List<String>)
internal data class SpeechConfigKey(val id: String, val algorithm: String, val publicSpki: String)
internal data class SpeechConfiguration(val machineId: String, val revision: Long, val serviceAvailable: Boolean,
    val serviceReason: String, val adapters: List<SpeechConfigAdapter>, val profiles: List<SpeechConfigProfile>, val key: SpeechConfigKey?)
internal data class SpeechConfigOperation(val host: SpeechConfigHost, val id: String, val operation: String, val payload: String)
internal data class SpeechConfigResult(val id: String, val state: String, val revision: Long?, val profileId: String,
    val profileRevision: Long?, val errorCode: String, val error: String) {
    val terminal get() = state in setOf("completed", "failed")
}

internal object SpeechConfigProtocol {
    val operations = setOf("speech_config", "speech_profile_upsert", "speech_profile_remove", "speech_profile_test", "speech_config_status")
    val mutations = setOf("speech_profile_upsert", "speech_profile_remove", "speech_profile_test")
    private fun strings(raw: JSONArray?) = raw?.let { array ->
        require(array.length() <= 32)
        (0 until array.length()).map { bounded(array.getString(it), 128) }
    }.orEmpty()
    fun bounded(value: String, max: Int, allowEmpty: Boolean = false): String {
        require((allowEmpty || value.isNotBlank()) && value.toByteArray(Charsets.UTF_8).size <= max && value.none { it.code < 32 || it.code == 127 }) { "Check the provider fields." }
        return value
    }
    fun revision(raw: JSONObject, field: String): Long {
        val value = raw.get(field)
        require(value is Int || value is Long) { "Invalid configuration revision." }
        return (value as Number).toLong().also { require(it in 0..9_007_199_254_740_991L) }
    }
    fun parse(raw: JSONObject, host: SpeechConfigHost): SpeechConfiguration {
        require(raw.getInt("schema") == 1 && raw.getString("machine_id") == host.machineId) { "The computer identity changed. Refresh its configuration." }
        val revision = revision(raw, "revision")
        val adapters = raw.getJSONArray("adapters").objects().map { row ->
            SpeechConfigAdapter(bounded(row.getString("id"), 64), strings(row.optJSONArray("models")), strings(row.optJSONArray("devices")))
        }; require(adapters.size <= 8 && adapters.map { it.id }.distinct().size == adapters.size)
        val profiles = raw.getJSONArray("profiles").objects().map { row ->
            val id = row.getString("id"); require(id.matches(Regex("[A-Za-z0-9_-]{1,64}")))
            val profileRevision = revision(row, "revision"); require(profileRevision > 0)
            SpeechConfigProfile(id, profileRevision, bounded(row.getString("name"), 128), bounded(row.getString("adapter"), 64),
                bounded(row.getString("model"), 128), bounded(row.getString("language"), 32, true),
                bounded(row.string("device"), 128, true), bounded(row.string("base_url"), 2048, true), row.optBoolean("allow_insecure_http"),
                row.getJSONObject("credential").getBoolean("configured"), row.getBoolean("available"), bounded(row.string("reason"), 512, true))
        }; require(profiles.size <= 32 && profiles.map { it.id }.distinct().size == profiles.size)
        val key = raw.optJSONObject("credential_key")?.let {
            SpeechConfigKey(it.getString("key_id"), it.getString("algorithm"), it.getString("public_key_spki"))
        }
        val service = raw.getJSONObject("service")
        return SpeechConfiguration(host.machineId, revision, service.getBoolean("available"), bounded(service.string("reason"), 512, true), adapters, profiles, key)
    }
    fun validate(profile: SpeechConfigProfile, config: SpeechConfiguration) {
        require(profile.id.matches(Regex("[A-Za-z0-9_-]{1,64}"))) { "Use letters, numbers, underscores or hyphens for the provider ID." }
        bounded(profile.name, 128); bounded(profile.model, 128); bounded(profile.language, 32, true)
        require(profile.language.isEmpty() || profile.language.matches(Regex("[a-z]{2}"))) { "Use a two-letter language code such as ru, or leave it empty." }
        val adapter = config.adapters.find { it.id == profile.adapter } ?: error("This computer does not support that provider type.")
        when (adapter.id) {
            "gigaam" -> require(profile.model in adapter.models && profile.device in adapter.devices) { "Choose a supported local model and device." }
            "openai_http" -> {
                val uri = try { URI(bounded(profile.baseUrl, 2048)) } catch (_: Exception) { error("Enter a valid server base URL.") }
                require(uri.scheme in setOf("http", "https") && !uri.host.isNullOrBlank() && uri.rawUserInfo == null && uri.rawQuery == null && uri.rawFragment == null) {
                    "Use an HTTP(S) base URL without credentials, query or fragment."
                }
                require(uri.scheme == "https" || profile.allowInsecureHttp) { "Allow local/private HTTP explicitly, or use HTTPS." }
            }
            else -> error("Update Zerus to edit this provider type.")
        }
    }
    fun upsert(host: SpeechConfigHost, config: SpeechConfiguration, profile: SpeechConfigProfile, credentialMode: String, secret: String): SpeechConfigOperation {
        require(config.machineId == host.machineId); validate(profile, config)
        require(credentialMode in setOf("keep", "replace", "remove"))
        require(profile.adapter == "openai_http" || credentialMode != "replace") { "Local GigaAM does not use an API key." }
        val id = UUID.randomUUID().toString()
        val payload = JSONObject().put("request_id", id).put("expected_machine_id", host.machineId)
            .put("expected_revision", config.revision).put("profile", profile.fields())
        val credential = JSONObject().put("mode", credentialMode)
        if (credentialMode == "replace") credential.put("sealed_value", SpeechCredential.seal(checkNotNull(config.key) { "This computer cannot receive API keys yet." }, payload, secret))
        return SpeechConfigOperation(host, id, "speech_profile_upsert", payload.put("credential", credential).toString())
    }
    fun remove(host: SpeechConfigHost, config: SpeechConfiguration, profile: SpeechConfigProfile) = mutation(host, "speech_profile_remove",
        JSONObject().put("expected_revision", config.revision).put("profile_id", profile.id))
    fun test(host: SpeechConfigHost, profile: SpeechConfigProfile) = mutation(host, "speech_profile_test",
        JSONObject().put("expected_profile_revision", profile.revision).put("profile_id", profile.id))
    private fun mutation(host: SpeechConfigHost, operation: String, payload: JSONObject): SpeechConfigOperation {
        val id = UUID.randomUUID().toString()
        return SpeechConfigOperation(host, id, operation, payload.put("request_id", id).put("expected_machine_id", host.machineId).toString())
    }
    fun result(raw: JSONObject, operation: SpeechConfigOperation): SpeechConfigResult {
        require(raw.getInt("schema") == 1 && raw.getString("machine_id") == operation.host.machineId && raw.getString("request_id") == operation.id) { "The computer returned a different configuration operation." }
        val state = raw.getString("state"); require(state in setOf("completed", "failed", "running", "uncertain"))
        return SpeechConfigResult(operation.id, state, if (raw.has("revision")) revision(raw, "revision") else null, raw.string("profile_id"),
            if (raw.has("profile_revision")) revision(raw, "profile_revision") else null, bounded(raw.string("error_code"), 128, true), bounded(raw.string("error"), 512, true))
    }
    fun encode(pending: List<SpeechConfigOperation>) = JSONArray(pending.map { op ->
        JSONObject().put("connection", op.host.key.connectionId).put("computer", op.host.key.computerId).put("machine", op.host.machineId)
            .put("name", op.host.name).put("id", op.id).put("operation", op.operation).put("payload", JSONObject(op.payload))
    })
    fun decode(raw: JSONArray): List<SpeechConfigOperation> {
        require(raw.length() <= 32)
        return raw.objects().map { row ->
            val id = row.getString("id"); val machine = row.getString("machine")
            require(AttachmentPolicy.validId(id) && AttachmentPolicy.validId(machine))
            val operation = row.getString("operation"); require(operation in mutations)
            val payload = row.getJSONObject("payload")
            require(payload.getString("request_id") == id && payload.getString("expected_machine_id") == machine)
            SpeechConfigOperation(SpeechConfigHost(MachineKey(row.getString("connection"), row.getString("computer")), machine, row.getString("name")), id, operation, payload.toString())
        }.also { require(it.map { op -> op.id }.distinct().size == it.size) }
    }
}
