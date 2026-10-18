package app.zerus.mobile

/** The delivery caption under one user bubble. */
object MessageDeliveryPresentation {
    fun label(status: String) = when (status) {
        "" -> ""
        "sending", "submitting" -> "Sending…"
        "uncertain" -> "Delivery unknown"
        "failed" -> "Not sent"
        "submitted", "recorded" -> "Sent"
        "queued" -> "Queued"
        "not_delivered" -> "Not delivered"
        else -> status.replaceFirstChar { it.uppercase() }
    }
    /** Native inbox states of a user row: waiting for the agent, or left undelivered after Stop. */
    val inboxStates = setOf("queued", "not_delivered")
    fun inboxState(row: org.json.JSONObject) = row.optString("delivery_state").takeIf { it in inboxStates }.orEmpty()
    /** Captions that need the user's attention use the warning color. */
    fun warning(status: String) = status in listOf("failed", "uncertain", "not_delivered")
}
