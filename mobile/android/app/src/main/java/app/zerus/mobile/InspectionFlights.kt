package app.zerus.mobile

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Shared readers keep their flight; an explicit fresh reader waits and retires it without cancelling it. */
internal object InspectionFlights {
    suspend fun <K, O, V> existing(flights: MutableMap<K, Pair<O, Deferred<V>>>, key: K, fresh: Boolean = false,
        current: () -> Boolean = { true }, limit: Int = 4): Deferred<V>? {
        val context = currentCoroutineContext()
        fun checkCurrent() {
            context.ensureActive()
            if (!current()) throw CancellationException("Inspection target changed.")
        }
        suspend fun settle(task: Deferred<V>) {
            // A failed/cancelled prior RPC is not the result of this fresh read.
            try { task.await() } catch (_: Exception) { context.ensureActive() }
            checkCurrent()
        }
        while (true) {
            checkCurrent()
            val previous = flights[key]?.second
            if (previous != null) {
                if (!fresh) return previous
                settle(previous)
                retire(flights, key, previous)
                // Completion cleanup can be queued, or a replacement can own the key.
                // Recheck every replacement rather than treating it as this read's evidence.
                continue
            }
            if (flights.size < limit) return null
            settle(flights.values.first().second)
            flights.entries.removeAll { it.value.second.isCompleted }
            // Waiting for capacity may have admitted a same-target shared flight.
        }
    }

    fun <K, O, V> retire(flights: MutableMap<K, Pair<O, Deferred<V>>>, key: K, expected: Deferred<V>) {
        if (flights[key]?.second === expected) flights.remove(key)
    }
}
