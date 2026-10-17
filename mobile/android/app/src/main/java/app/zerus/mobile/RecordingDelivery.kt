package app.zerus.mobile

/** Native submission is confirmed delivery to the agent, never evidence of reading or completion. */
internal enum class RecordingDelivery(val description: String) {
    Uploading("Sending recording"), Processing("Processing recording"), Sent("Sent to agent"),
    Failed("Recording failed"), Cancelled("Not sent"), Uncertain("Delivery unconfirmed");

    val explanation get() = when (this) {
        Uploading -> "Your recording is being uploaded. Its recognized text has not been sent to the agent yet."
        Processing -> "Your recording is being processed. Its recognized text is not confirmed as sent to the agent yet."
        Sent -> "The recognized text was sent to the agent. The original recording stays in this conversation."
        Failed -> "The recording could not be sent. Your recording and any recognized text are kept for review."
        Cancelled -> "Delivery was cancelled. This recording was not sent to the agent."
        Uncertain -> "Delivery has not been confirmed. Check this recording's status before trying to send again."
    }

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
