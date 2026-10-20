package app.zerus.mobile

data class SearchableMessage(val eventId: String,val role: String,val text: String,val at: Double)
data class MessageSearchHit(val eventId: String,val role: String,val snippet: String,val at: Double)
data class MessageSearchResults(val matches: Int,val hits: List<MessageSearchHit>)
data class ConversationMessageJump(val target: Target,val eventId: String)

/** Loaded public messages only. Search is presentation and never advances read state. */
object ConversationSearch {
    fun prepare(events: List<Event>): List<SearchableMessage> = events.map { event ->
        val replies = if(QuestionReplyPresentation.isOwnRole(event.role)) QuestionReplyPresentation.parse(event.text) else null
        val body = replies?.joinToString("\n\n") { "Question\n${it.question}\nYour answer\n${it.answer}" } ?: event.text
        SearchableMessage(event.id,if(replies != null) "You (answer)"
            else if(event.role == ActivityNotices.ROLE) ActivityNotices.label(event.origin,event.sender) else event.role,
            listOf(body,event.attachments.joinToString("\n") { it.name }).filter { it.isNotBlank() }.joinToString("\n"),event.at)
    }
    fun find(messages: List<SearchableMessage>,query: String,limit: Int = 100): MessageSearchResults {
        if(query.isBlank() || query.length > 2048) return MessageSearchResults(0,emptyList())
        val hits = mutableListOf<MessageSearchHit>()
        var count = 0
        for(message in messages) {
            val index = message.text.indexOf(query,ignoreCase = true)
            if(index < 0) continue
            count++
            if(hits.size >= limit.coerceIn(1,100)) continue
            var start = (index - 45).coerceAtLeast(0)
            var end = (start + 180).coerceAtMost(message.text.length)
            if(start > 0 && Character.isLowSurrogate(message.text[start])) start--
            if(end < message.text.length && end > 0 && Character.isHighSurrogate(message.text[end - 1])) end--
            val snippet = (if(start > 0) "…" else "") + message.text.substring(start,end).replace('\n',' ') + if(end < message.text.length) "…" else ""
            hits += MessageSearchHit(message.eventId,message.role,snippet,message.at)
        }
        return MessageSearchResults(count,hits)
    }
    fun resolve(events: List<Event>,id: String): Event? = events.singleOrNull { it.id == id }
}
