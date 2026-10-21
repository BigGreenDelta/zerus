package app.zerus.mobile

import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/** Pure preparation only: races never start another inspection, action or timer. */
internal object InspectionPublication {
    const val MAX_ATTEMPTS = 3
    enum class Result { PUBLISHED, SUPERSEDED, UNSTABLE }

    suspend fun <S, P> prepare(owns: () -> Boolean, capture: () -> S, build: suspend (S) -> P,
        unchanged: (S) -> Boolean, publish: (S, P) -> Unit): Result {
        repeat(MAX_ATTEMPTS) {
            currentCoroutineContext().ensureActive()
            if (!owns()) return Result.SUPERSEDED
            val snapshot = capture()
            val prepared = build(snapshot)
            currentCoroutineContext().ensureActive()
            if (!owns()) return Result.SUPERSEDED
            if (unchanged(snapshot)) {
                publish(snapshot, prepared)
                return Result.PUBLISHED
            }
        }
        return if (owns()) Result.UNSTABLE else Result.SUPERSEDED
    }
}
