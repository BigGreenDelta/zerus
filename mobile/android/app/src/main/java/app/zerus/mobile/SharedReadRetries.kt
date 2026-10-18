package app.zerus.mobile

import kotlinx.coroutines.delay

/**
 * Bounded bookkeeping for read marks a computer completed without applying (`mark_read`).
 * The caller owns one timer: it sleeps until [nextDue], calls [release] and lets exactly the returned
 * marks be sent again. Times are monotonic milliseconds. At capacity a new mark is not scheduled at
 * all: it stays sent, so nothing is ever resent beyond the bound (fail closed).
 */
class SharedReadRetries {
    /** [due] is when the next attempt is allowed, null once released; a released entry is forgotten at [retireAt]. */
    private class Entry(val connectionId:String,val exponent:Int,var due:Long?,val retireAt:Long)
    private val entries=mutableMapOf<String,Entry>()
    val size get()=entries.size
    /** The earliest pending attempt, for the caller's single timer. */
    val nextDue:Long? get()=entries.values.mapNotNull { it.due }.minOrNull()
    /** Records an unapplied completion of [key] and returns when it may be sent again, or null at capacity. */
    fun unapplied(key:String,connectionId:String,now:Long):Long? {
        prune(now)
        val previous=entries[key]
        val exponent=when {
            previous!=null -> if(previous.due==null) (previous.exponent+1).coerceAtMost(MAX_EXPONENT) else previous.exponent
            entries.size>=CAPACITY -> return null
            else -> 0
        }
        val due=now+delayMs(exponent)
        entries[key]=Entry(connectionId,exponent,due,due+delayMs(MAX_EXPONENT))
        return due
    }
    /** An applied receipt ends the backoff for this mark. */
    fun applied(key:String) { entries.remove(key) }
    /** Marks whose attempt is due at [now]; they keep their exponent until retired. */
    fun release(now:Long):List<String> {
        val released=entries.filterValues { entry -> entry.due.let { it!=null && it<=now } }.keys.sorted()
        released.forEach { entries.getValue(it).due=null }
        prune(now)
        return released
    }
    /** The body of the caller's one timer: sleeps until each due mark and hands exactly the released marks to [allow]. */
    suspend fun releaseWhenDue(now:()->Long,allow:(String)->Unit) {
        while(true) {
            val due=nextDue ?: return
            delay(due-now())
            release(now()).forEach(allow)
        }
    }
    /** Forgetting a workspace drops its marks; nothing of it is released later. */
    fun forget(connectionId:String) { entries.values.removeAll { it.connectionId==connectionId } }
    fun reset() { entries.clear() }
    private fun prune(now:Long) { entries.values.removeAll { it.due==null && it.retireAt<=now } }
    companion object {
        const val CAPACITY=256
        const val MAX_EXPONENT=3
        /** Two minutes, doubling to 16. */
        fun delayMs(exponent:Int)=SharedReadPolicies.retryDelayMs(exponent)
    }
}
