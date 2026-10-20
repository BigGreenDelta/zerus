package app.zerus.mobile

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** A pre-existing automatic catalog read cannot satisfy an explicit Terminal availability check. */
internal object TerminalAvailabilityRefresh {
    suspend fun run(current: () -> Boolean, catalogIdle: suspend () -> Unit, refreshCatalog: () -> Unit,
        detailIdle: suspend () -> Unit, refreshDetail: () -> Unit): Boolean {
        val context = currentCoroutineContext()
        fun stillCurrent(): Boolean { context.ensureActive();return current() }
        catalogIdle()
        if (!stillCurrent()) return false
        refreshCatalog()
        catalogIdle()
        if (!stillCurrent()) return false
        detailIdle()
        if (!stillCurrent()) return false
        refreshDetail()
        detailIdle()
        return stillCurrent()
    }
}
