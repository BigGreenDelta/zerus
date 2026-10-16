package app.zerus.mobile

/** A later canonical page can identify rows already read from the bounded native inspection. */
internal object ConversationReadHead {
    fun observe(state: ConversationReadState, page: HistoryPage): ConversationReadState {
        val canonical = page.events.filter { it.historyEpoch == page.epoch && it.historyId.isNotBlank() && it.historyCursor.isNotBlank() }
        val aliases = HistoryAliases.exact(canonical, state.record(page.target)?.seen.orEmpty().toSet())
        val next = state.head(page.target, page.epoch, page.latestIncoming, page.totalIncoming, page.complete)
            .supersede(page.target, aliases)
        return next.visible(page.target, canonical.filter { it.id in next.record(page.target)?.seen.orEmpty() })
    }
}
