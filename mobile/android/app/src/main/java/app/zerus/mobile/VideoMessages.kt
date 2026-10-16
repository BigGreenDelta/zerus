package app.zerus.mobile

import org.json.JSONObject

data class VideoMessage(val file: DisplayedAttachment, val durationMs: Int, val width: Int, val height: Int, val posterBase64: String = "")
internal data class VideoDraft(val file: Attachment, val durationMs: Int, val width: Int, val height: Int, val posterBase64: String = "") {
    fun displayed(id: String = "") = VideoMessage(file.displayed().copy(messageId = id, index = 1), durationMs, width, height, posterBase64)
}

internal object VideoProtocol {
    const val FEATURE = "voice_video"
    const val MAX_BYTES = 10 * 1024 * 1024
    const val MAX_DURATION_MS = 60_000
    private fun geometry(raw: JSONObject) {
        require(raw.getInt("duration_ms") in 100..MAX_DURATION_MS)
        require(raw.getInt("width") in 16..1920 && raw.getInt("height") in 16..1920)
        require(raw.string("poster_base64").length <= 10_924)
    }
    fun message(raw: JSONObject, id: String): VideoMessage {
        geometry(raw)
        require(raw.getString("request_id") == id && raw.getInt("index") == 1 && raw.getString("mime") == "video/mp4")
        require(raw.getString("name") == "video.mp4" && raw.getLong("bytes") in 1..MAX_BYTES.toLong())
        return VideoMessage(DisplayedAttachment("video.mp4", "video/mp4", raw.getLong("bytes"), messageId = id, index = 1),
            raw.getInt("duration_ms"), raw.getInt("width"), raw.getInt("height"), raw.string("poster_base64"))
    }
    fun json(video: VideoMessage) = JSONObject().put("request_id", video.file.messageId).put("index", 1)
        .put("name", "video.mp4").put("mime", "video/mp4").put("bytes", video.file.bytes)
        .put("duration_ms", video.durationMs).put("width", video.width).put("height", video.height)
        .put("poster_base64", video.posterBase64.takeIf { it.isNotBlank() })
    fun draftJson(video: VideoDraft) = JSONObject().put("file", video.file.toJson()).put("duration_ms", video.durationMs)
        .put("width", video.width).put("height", video.height).put("poster_base64", video.posterBase64)
    fun draft(raw: JSONObject): VideoDraft {
        geometry(raw)
        val file = Attachment.fromJson(raw.getJSONObject("file")); require(file.mime == "video/mp4")
        return VideoDraft(file, raw.getInt("duration_ms"), raw.getInt("width"), raw.getInt("height"), raw.string("poster_base64"))
    }
    fun wire(video: VideoDraft, bytes: ByteArray): JSONObject {
        require(bytes.size.toLong() == video.file.bytes && bytes.size in 1..MAX_BYTES)
        return JSONObject().put("format", "mp4").put("data_base64", java.util.Base64.getEncoder().encodeToString(bytes))
            .put("duration_ms", video.durationMs).put("width", video.width).put("height", video.height)
            .put("poster_base64", video.posterBase64.takeIf { it.isNotBlank() })
    }
}
