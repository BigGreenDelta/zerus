package app.zerus.mobile

import org.json.JSONObject
import java.util.UUID

internal data class SpeechSelection(val host: MachineKey? = null, val profileId: String = "", val profileRevision: Long? = null)
internal data class SpeechProfile(val id: String, val name: String, val model: String, val available: Boolean,
    val reason: String = "", val maxDurationMs: Int = 120_000, val profileRevision: Long? = null)
internal data class SpeechAttempt(val id: String, val host: MachineKey, val profileId: String, val origin: Draft,
    val state: String = "uncertain", val text: String = "", val error: String = "")
internal data class SpeechState(val selection: SpeechSelection = SpeechSelection(), val attempt: SpeechAttempt? = null)

internal object SpeechProtocol {
    val operations = setOf("speech_profiles", "speech_submit", "speech_status", "speech_cancel")
    const val maxBytes = 3_840_000
    fun revision(raw: JSONObject): Long? {
        if (!raw.has("profile_revision") || raw.isNull("profile_revision")) return null
        val value = raw.get("profile_revision")
        require(value is Int || value is Long)
        return (value as Number).toLong().also { require(it in 1..9_007_199_254_740_991L) }
    }
    fun profiles(value: JSONObject): List<SpeechProfile> {
        require(value.optInt("schema") == 1) { "This computer uses an unsupported voice protocol." }
        val rows = value.optJSONArray("profiles")?.objects().orEmpty()
        require(rows.size <= 100 && rows.map { it.string("id") }.distinct().size == rows.size)
        return rows.map {
            val id = it.getString("id")
            require(id.isNotBlank() && id.length <= 128)
            SpeechProfile(id, it.string("name").ifBlank { id }, it.string("model"), it.optBoolean("available"),
                it.string("reason"), it.optInt("max_duration_ms", 120_000).coerceIn(1_000, 120_000), revision(it))
        }
    }
    fun job(value: JSONObject, attempt: SpeechAttempt): SpeechAttempt {
        require(value.optInt("schema") == 1 && value.string("job_id") == attempt.id) { "The computer returned a different voice recording." }
        val state = value.getString("state")
        require(state in setOf("queued", "running", "completed", "failed", "cancelled", "uncertain")) { "Unknown voice recording status." }
        val text = value.string("text")
        require(text.toByteArray(Charsets.UTF_8).size <= 65_536) { "Transcription exceeded the message text limit." }
        return attempt.copy(state = state, text = text, error = value.string("error"))
    }
    fun decode(value: JSONObject): SpeechState {
        fun host(raw: JSONObject) = MachineKey(raw.getString("connection"), raw.getString("computer"))
        val selection = value.optJSONObject("selection")?.let {
            SpeechSelection(it.optJSONObject("host")?.let(::host), it.string("profile"), revision(it))
        } ?: SpeechSelection()
        val attempt = value.optJSONObject("attempt")?.let {
            val id = it.getString("id"); require(UUID.fromString(id).toString() == id)
            SpeechAttempt(id, host(it.getJSONObject("host")), it.getString("profile"), MessageCodec.draft(it.getJSONObject("origin")),
                it.string("state"), it.string("text"), it.string("error"))
        }
        return SpeechState(selection, attempt)
    }
    fun encode(value: SpeechState): JSONObject {
        fun host(key: MachineKey) = JSONObject().put("connection", key.connectionId).put("computer", key.computerId)
        return JSONObject().put("selection", JSONObject().put("host", value.selection.host?.let(::host))
            .put("profile", value.selection.profileId).put("profile_revision", value.selection.profileRevision)).put("attempt", value.attempt?.let {
                JSONObject().put("id", it.id).put("host", host(it.host)).put("profile", it.profileId)
                    .put("origin", MessageCodec.draftJson(it.origin.copy(attachments = emptyList())))
                    .put("state", it.state).put("text", it.text).put("error", it.error)
            })
    }
}

/** A completed recording can only append to a reviewed exact draft. It never sends. */
internal object SpeechDrafts {
    fun insert(state: MessageState, attempt: SpeechAttempt, expected: Draft): MessageState {
        if (attempt.id in state.speechAppliedIds) return state
        require(attempt.state == "completed" && attempt.text.isNotBlank())
        require(expected.target == attempt.origin.target && expected.status == "editing" && expected.target.archiveId.isBlank())
        val current = state.drafts.find { it.key == expected.key } ?: expected
        require(current.revision == expected.revision && current.generation == expected.generation && current.status == "editing") {
            "The draft changed. Review the transcription before inserting it."
        }
        val text = current.text + (if (current.text.isNotEmpty() && !current.text.last().isWhitespace()) " " else "") + attempt.text
        require(text.toByteArray(Charsets.UTF_8).size <= 65_536) { "The combined draft exceeds 64 KiB. Copy the transcription instead." }
        val changed = current.copy(text = text, revision = UUID.randomUUID().toString(), generation = attempt.id, updatedAt = System.currentTimeMillis())
        return MessageChanges.draft(state, changed).copy(speechAppliedIds = (state.speechAppliedIds + attempt.id).takeLast(100))
    }
    fun unchanged(attempt: SpeechAttempt, current: Draft) = current.target == attempt.origin.target &&
        current.revision == attempt.origin.revision && current.generation == attempt.origin.generation && current.status == "editing"
}
