package app.zerus.mobile

/** UI-only reading position. Message keys survive incoming rows and transcript windows. */
data class ConversationAnchor(val itemKey: String, val offset: Int, val following: Boolean)
data class ConversationFollowState(val opened: Boolean = false, val following: Boolean = true,
    val ownSendId: String = "", val anchor: ConversationAnchor? = null)
data class ConversationScroll(val state: ConversationFollowState, val index: Int? = null, val offset: Int = 0,
    val failure: String = "")

object ConversationViewport {
    /** The row of this phone's latest send, to bring into view. */
    fun own(events: List<Event>, ownSendId: String): Event? = events.lastOrNull { it.outgoingId == ownSendId || it.receiptRequestId == ownSendId }
    fun content(state: ConversationFollowState, keys: List<String>, ownSendId: String, ownItemKey: String?): ConversationScroll {
        if (keys.isEmpty()) return ConversationScroll(state)
        val ownIndex = ownItemKey?.let(keys::indexOf)?.takeIf { it >= 0 }
        if (ownSendId.isNotBlank() && ownSendId != state.ownSendId && ownIndex != null)
            return ConversationScroll(state.copy(opened = true, following = true, ownSendId = ownSendId, anchor = null), ownIndex)
        if (!state.opened) {
            val anchor = state.anchor
            val index = anchor?.takeIf { !it.following }?.let { keys.indexOf(it.itemKey) }?.takeIf { it >= 0 }
            return if (index != null) ConversationScroll(state.copy(opened = true, following = false, ownSendId = ownSendId), index, anchor!!.offset)
                else ConversationScroll(state.copy(opened = true, following = true, ownSendId = ownSendId, anchor = null), keys.lastIndex)
        }
        return ConversationScroll(state, if (state.following) keys.lastIndex else null)
    }
    fun userScrolled(state: ConversationFollowState, atEnd: Boolean) = state.copy(following = atEnd)
    fun settled(state: ConversationFollowState,atEnd: Boolean,hasNewer: Boolean): ConversationFollowState =
        if(state.opened && atEnd && !hasNewer) state.copy(following = true,anchor = null) else state
    fun latest(state: ConversationFollowState) = state.copy(following = true, anchor = null)
    /** An explicit jump consumes its own read result, even when no message or loading key changed. */
    fun loadedLatest(state: ConversationFollowState, keys: List<String>, hasNewer: Boolean,
        error: String): ConversationScroll = when {
        error.isNotBlank() -> ConversationScroll(state, failure = "Could not reach the latest messages. $error")
        hasNewer -> ConversationScroll(state, failure = "The latest messages are not available in the loaded history. Refresh to try again.")
        keys.isEmpty() -> ConversationScroll(state, failure = "No messages are available to jump to. Refresh to try again.")
        else -> ConversationScroll(latest(state).copy(opened = true), keys.lastIndex)
    }
}
