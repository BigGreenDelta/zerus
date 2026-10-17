package app.zerus.mobile

/** Native submission is confirmed delivery to the agent, never evidence of reading or completion. */
internal enum class RecordingDelivery(val description: String) {
    Uploading("Sending recording"), Processing("Processing recording"), Sent("Sent to agent"),
    Failed("Recording failed"), Cancelled("Delivery cancelled"), Uncertain("Delivery unconfirmed");

    companion object {
        fun of(state: String) = when (state) {
            "uploading" -> Uploading
            "queued", "transcribing", "ready", "waiting_agent", "delivering" -> Processing
            "submitted" -> Sent
            "failed" -> Failed
            "cancelled" -> Cancelled
            else -> Uncertain
        }
    }
}
