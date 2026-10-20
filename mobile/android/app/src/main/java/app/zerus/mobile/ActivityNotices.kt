package app.zerus.mobile

/**
 * Messages the person did not write, as the hgs journal marks their native
 * prompts: background task notices, subagent reports and other sessions'
 * messages. Desktop and iOS present the same cases of
 * activity-notice-contract.json. A notice names the sender its envelope
 * declares; an envelope is text anyone can write, so nothing here calls it
 * verified.
 */
object ActivityNotices {
    const val ROLE = "Notice"
    val origins = setOf("task_notification", "subagent_report", "peer_message")

    fun label(origin: String, sender: String) = when (origin) {
        "subagent_report" -> "Subagent report"
        "peer_message" -> "Message from $sender"
        else -> "Background task"
    }

    /** Another session's message is read in full; other notices show their summary. */
    fun body(origin: String, detail: String, report: String) =
        if (origin == "peer_message" && report.isNotEmpty()) report else detail

    /** A subagent's report stays collapsed below its summary until opened. */
    fun report(origin: String, report: String) = if (origin == "subagent_report") report else ""

    /** Display caches cannot turn a notice or reply into this phone's outgoing receipt. */
    fun cached(event: Event): Event = if (event.role == "You") event else event.copy(
        requestId = "", delivery = "", outgoingId = "", submittedText = "")
}
