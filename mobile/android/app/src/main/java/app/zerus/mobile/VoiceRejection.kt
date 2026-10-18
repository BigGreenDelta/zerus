package app.zerus.mobile

import org.json.JSONObject
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID

/** A durable admission barrier, never inferred from transport errors or missing status. */
internal object VoiceRejection {
    private val fields = setOf("schema", "state", "request_id", "message_id", "run_id", "conversation_id", "intent",
        "payload_sha256", "generation", "retry_token", "code", "detail")
    private fun uuid(value: String) = runCatching { UUID.fromString(value).let { it.toString() == value && it != UUID(0, 0) } }.getOrDefault(false)
    private fun hex(value: String) = value.length == 64 && value.all { it in '0'..'9' || it in 'a'..'f' }
    private fun keys(value: JSONObject) = value.keys().asSequence().toSet()
    private fun same(a: JSONObject, b: JSONObject): Boolean = keys(a) == keys(b) && keys(a).all { key ->
        val left = a.get(key); val right = b.get(key)
        if (left is JSONObject && right is JSONObject) same(left, right)
        else if (left is Number && right is Number) left.toString() == right.toString() else left == right
    }
    private fun hash(bytes: ByteArray) = MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    fun intent(entry: VoiceOutgoing): JSONObject {
        val audio = requireNotNull(entry.audio)
        require(audio.bytes in 46..3_840_044 && audio.bytes % 2 == 0L && hex(audio.sha256))
        val route = JSONObject().put("machine_id", entry.machineId).put("profile_id", entry.profileId).put("profile_revision", entry.profileRevision)
        return JSONObject().put("speech", route).put("audio", JSONObject().put("format", "pcm_s16le").put("sample_rate", 16000)
            .put("channels", 1).put("bytes", audio.bytes).put("sha256", audio.sha256)).also { result ->
            entry.video?.let { video ->
                val media = JSONObject().put("format", "mp4").put("duration_ms", video.durationMs).put("width", video.width)
                    .put("height", video.height).put("bytes", video.file.bytes).put("sha256", video.file.sha256)
                if (video.posterBase64.isNotEmpty()) media.put("poster_sha256", hash(Base64.getDecoder().decode(video.posterBase64)))
                result.put("video", media)
            }
        }
    }
    fun generation(proof: String): Long = if (proof.isEmpty()) 0 else JSONObject(proof).getLong("generation")
    fun accept(raw: JSONObject, entry: VoiceOutgoing, operationId: String): VoiceOutgoing {
        // An accepted native receipt always takes precedence over an older refusal callback.
        if (entry.message != null) return entry
        val proof = raw.getJSONObject("rejection")
        require(raw.get("schema") == 1 && keys(proof) == fields && proof.toString().toByteArray().size <= 4096)
        require(proof.get("schema") == 1 && proof.getString("state") == "rejected_before_acceptance")
        require(uuid(operationId) && proof.getString("request_id") == operationId && proof.getString("message_id") == entry.id)
        require(proof.getString("run_id") == entry.target.run && proof.getString("conversation_id") == entry.target.conversation)
        require(uuid(proof.getString("retry_token")) && hex(proof.getString("payload_sha256")))
        require(proof.getString("code") in setOf("route_unavailable", "storage_full", "queue_full"))
        val detail = proof.getString("detail")
        require(detail.toByteArray().size in 1..2000 && detail.none { Character.isISOControl(it) })
        require(proof.get("generation") is Long || proof.get("generation") is Int)
        val generation = proof.getLong("generation")
        require(generation > entry.attemptedRejectionGeneration && generation >= generation(entry.rejection)) {
            "This refusal does not settle the attempted retry. Check status; it has not been resent."
        }
        if (entry.rejection.isNotEmpty()) {
            val old = JSONObject(entry.rejection)
            require(old.getString("payload_sha256") == proof.getString("payload_sha256"))
            if (generation == old.getLong("generation")) require(old.getString("retry_token") == proof.getString("retry_token"))
        }
        require(same(proof.getJSONObject("intent"), intent(entry))) { "The refusal describes a different recording or provider." }
        return entry.copy(uploadState = "failed", rejection = proof.toString(), error = "The computer did not accept this recording. Return to recording to send the same recording again.")
    }
}
