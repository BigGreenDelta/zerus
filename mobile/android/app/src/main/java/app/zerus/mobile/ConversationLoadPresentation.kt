package app.zerus.mobile

/** A resolved empty inspection is content too: polling must not restore the cold placeholder. */
object ConversationLoadPresentation {
    data class Status(val text: String, val initial: Boolean, val spinner: Boolean)

    fun status(hasContent: Boolean, detailLoading: Boolean, detailProgress: Boolean,
        historyLoading: Boolean, historyProgress: Boolean, demo: Boolean, resolved: Boolean = false): Status? {
        if (demo || !detailLoading && !historyLoading) return null
        val progress = detailLoading && detailProgress || historyLoading && historyProgress
        val initial = !hasContent && !resolved
        if (!initial && !progress) return null
        val text = when {
            initial -> "Loading conversation…"
            historyLoading && detailLoading -> "Refreshing conversation and history…"
            historyLoading -> "Loading message history…"
            else -> "Refreshing conversation…"
        }
        return Status(text, initial, progress)
    }

    fun readAllOutcome(marked: Int, skipped: Int, partial: Int): String = buildList {
        add(if (marked == 0) "No conversations marked read." else
            "${if (marked == 1) "1 conversation" else "$marked conversations"} marked read on this phone.")
        if (partial > 0) add("$partial marked loaded replies only.")
        if (skipped > 0) add("$skipped could not be checked.")
    }.joinToString(" ")
}
