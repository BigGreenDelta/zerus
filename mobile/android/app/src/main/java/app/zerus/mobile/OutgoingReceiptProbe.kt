package app.zerus.mobile

/** One automatic receipt lookup for the current navigation, never a message submission. */
internal class OutgoingReceiptProbe {
    private var attempted: Triple<Target, Long, String>? = null
    fun claim(target: Target, navigation: Long, message: OutgoingMessage, busy: Boolean): Boolean {
        if (!message.belongsTo(target) || message.status != "uncertain" || busy) return false
        val key = Triple(target, navigation, message.requestId)
        if (attempted == key) return false
        attempted = key
        return true
    }
}
