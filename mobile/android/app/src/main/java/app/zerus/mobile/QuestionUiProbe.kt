package app.zerus.mobile

import android.os.SystemClock
import java.security.MessageDigest

/** Inactive unless the isolated DEBUG instrumentation owns a session. Never invokes a UI action. */
internal object QuestionUiProbe {
    const val MAX_RECORDS = 256
    enum class Stage { ENTRY, REQUEST_RESULT, CONTENT_COMMIT, SHOWING_COMMIT, VERIFICATION_WRITE, AVAILABILITY_COMMIT }
    enum class VerificationWriter { INSTALL_WAIT_PRESENTATION, INSTALL_PRESENTATION, PREPARE_PRESENTATION,
        TERMINAL_CLOSED, ACTION_RESULT, CONNECTION_FORGOTTEN, OPEN_SESSION, BACK, CLEAR_UNCONFIRMED, INSPECT_FAILED }
    enum class Fault { NONE, OBSERVATION, OVERFLOW }
    sealed interface Detail
    data class Captured(val next: Boolean, val advance: Boolean, val editing: Boolean, val pending: Boolean,
        val choices: Boolean, val reviewed: Boolean, val open: Boolean) : Detail
    data class Committed(val eligible: Boolean, val remaining: Int, val reviewed: Boolean, val enabled: Boolean,
        val available: Boolean, val editing: Boolean, val pending: Boolean, val complete: Boolean,
        val disclosure: Boolean, val foreground: Boolean, val focused: Boolean, val next: Boolean,
        val advance: Boolean, val choices: Boolean, val open: Boolean, val dialog: Boolean,
        val visible: Boolean, val obscured: Boolean, val answerable: Boolean, val supported: Boolean,
        val reviewStartedAt: Long? = null) : Detail
    data class Requested(val open: Boolean, val sameCard: Boolean, val option: String) : Detail
    data class Showing(val open: Boolean, val eligible: Boolean, val showing: Boolean,
        val sameCard: Boolean, val option: String) : Detail
    data class Verification(val writer: VerificationWriter, val verified: Boolean, val frameVerified: Boolean,
        val hasFrame: Boolean, val hasPresentation: Boolean, val frameMatchesSelected: Boolean) : Detail
    data class Availability(val verified: Boolean, val demo: Boolean, val online: Boolean, val blocked: Boolean,
        val actionBlocked: Boolean, val clearBlocked: Boolean, val operationBlocked: Boolean,
        val selected: Boolean, val available: Boolean) : Detail
    data class Record(val stage: Stage, val uptimeMs: Long, val card: String, val questionObject: Int,
        val selection: String, val detail: Detail)
    data class Report(val closed: Boolean, val fault: Fault, val records: List<Record>)
    data class ContentSnapshot(val generation: Int, val record: Record)

    /** Contains only immutable scalar records; no model, question, option map or action reference is retained. */
    class Session internal constructor() {
        private val records = ArrayDeque<Record>(MAX_RECORDS)
        private var content: Record? = null
        private var contentGeneration = 0
        private var showing: Record? = null
        private var availability: Record? = null
        private var closed = false
        private var fault = Fault.NONE
        @Synchronized internal fun healthy() = !closed && fault == Fault.NONE
        private fun same(left: Record?, right: Record) = left != null && left.stage == right.stage &&
            left.card == right.card && left.questionObject == right.questionObject &&
            left.selection == right.selection && left.detail == right.detail
        @Synchronized internal fun append(record: Record) {
            if (!healthy()) return
            // A review's 100ms recomposition does not grow the log when none of the observed values changed.
            if (record.stage == Stage.CONTENT_COMMIT && same(content, record) ||
                record.stage == Stage.SHOWING_COMMIT && same(showing, record) ||
                record.stage == Stage.AVAILABILITY_COMMIT && same(availability, record)) return
            if (records.size == MAX_RECORDS) { fault = Fault.OVERFLOW; return }
            records.addLast(record)
            if (record.stage == Stage.CONTENT_COMMIT) { content = record; contentGeneration++ }
            if (record.stage == Stage.SHOWING_COMMIT) showing = record
            if (record.stage == Stage.AVAILABILITY_COMMIT) availability = record
        }
        @Synchronized internal fun failed() { fault = Fault.OBSERVATION }
        @Synchronized internal fun contentSnapshot(): ContentSnapshot {
            check(healthy())
            return ContentSnapshot(contentGeneration, checkNotNull(content))
        }
        @Synchronized internal fun firstReviewSnapshot(afterGeneration: Int): ContentSnapshot? {
            check(healthy() && afterGeneration in 0..contentGeneration)
            var generation = 0
            for (record in records) if (record.stage == Stage.CONTENT_COMMIT) {
                generation++
                val detail = record.detail as Committed
                if (generation > afterGeneration && detail.eligible && detail.reviewStartedAt != null)
                    return ContentSnapshot(generation, record)
            }
            return null
        }
        @Synchronized internal fun close(): Report {
            check(!closed)
            closed = true
            return Report(true, fault, records.toList())
        }
    }

    @Volatile private var session: Session? = null
    val enabled get() = BuildConfig.DEBUG && session?.healthy() == true
    @Synchronized fun install(): Session {
        check(BuildConfig.DEBUG && session == null)
        return Session().also { session = it }
    }
    @Synchronized fun close(owned: Session): Report {
        check(session === owned)
        val report = owned.close()
        session = null
        return report
    }
    /** Read immutable evidence from the currently owned DEBUG session; never affects a UI gate. */
    @Synchronized fun contentSnapshot(owned: Session, question: Question, afterGeneration: Int? = null): ContentSnapshot? {
        check(BuildConfig.DEBUG && session === owned)
        val snapshot = if (afterGeneration == null) owned.contentSnapshot() else owned.firstReviewSnapshot(afterGeneration)
        if (snapshot == null) return null
        check(snapshot.record.card == fingerprint(listOf(question.id, question.hash, question.run, question.conversation)) &&
            snapshot.record.questionObject == System.identityHashCode(question))
        check(snapshot.record.detail is Committed)
        return snapshot
    }

    // BEGIN EARLY_REVIEW_FOCUS_WAIT classification
    enum class EarlyReviewState { CURRENT, WAIT, INVALID }

    /** Read-only fixture evidence. WAIT never authorizes input or renews the first bound clock. */
    @Synchronized fun classifyEarlyReview(owned: Session, question: Question, bound: ContentSnapshot,
        elapsedNow: Long, originalEnd: Long): EarlyReviewState {
        val latest = checkNotNull(contentSnapshot(owned, question))
        if (bound.generation <= 0 || latest.generation < bound.generation ||
            owned.firstReviewSnapshot(bound.generation - 1) != bound) return EarlyReviewState.INVALID
        val first = bound.record.detail as? Committed ?: return EarlyReviewState.INVALID
        val current = latest.record.detail as? Committed ?: return EarlyReviewState.INVALID
        val start = first.reviewStartedAt ?: return EarlyReviewState.INVALID
        val currentStart = current.reviewStartedAt
        if (bound.record.stage != Stage.CONTENT_COMMIT || latest.record.stage != Stage.CONTENT_COMMIT ||
            bound.record.card != latest.record.card || bound.record.questionObject != latest.record.questionObject ||
            bound.record.selection != latest.record.selection || !first.eligible ||
            start < 0 || start > Long.MAX_VALUE - 3_000 || originalEnd != start + 3_000 ||
            elapsedNow < start || elapsedNow >= originalEnd ||
            currentStart != null && (currentStart < start || currentStart > elapsedNow)) return EarlyReviewState.INVALID
        if (earlyReviewCurrent(owned, question, bound, elapsedNow, originalEnd)) return EarlyReviewState.CURRENT
        return if (!current.eligible || currentStart == null) EarlyReviewState.WAIT else EarlyReviewState.INVALID
    }
    // END EARLY_REVIEW_FOCUS_WAIT classification

    /** Negative-input evidence only: a forward reset cannot renew the first bound review window. */
    @Synchronized fun earlyReviewCurrent(owned: Session, question: Question, bound: ContentSnapshot,
        elapsedNow: Long, originalEnd: Long): Boolean {
        val latest = checkNotNull(contentSnapshot(owned, question))
        if (bound.generation <= 0 || latest.generation < bound.generation ||
            owned.firstReviewSnapshot(bound.generation - 1) != bound) return false
        val first = bound.record.detail as? Committed ?: return false
        val current = latest.record.detail as? Committed ?: return false
        val start = first.reviewStartedAt ?: return false
        val currentStart = current.reviewStartedAt ?: return false
        return bound.record.stage == Stage.CONTENT_COMMIT && latest.record.stage == Stage.CONTENT_COMMIT &&
            bound.record.card == latest.record.card && bound.record.questionObject == latest.record.questionObject &&
            bound.record.selection == latest.record.selection && first.eligible && current.eligible &&
            start >= 0 && start <= Long.MAX_VALUE - 3_000 && originalEnd == start + 3_000 &&
            elapsedNow >= start && elapsedNow < originalEnd && currentStart >= start && currentStart <= elapsedNow
    }

    private fun fingerprint(parts: List<String>): String {
        check(parts.size <= 40 && parts.all { it.length <= 256 })
        val hash = MessageDigest.getInstance("SHA-256")
        for (part in parts) {
            val bytes = part.toByteArray(Charsets.UTF_8)
            hash.update(byteArrayOf((bytes.size ushr 8).toByte(), bytes.size.toByte()))
            hash.update(bytes)
        }
        return hash.digest().joinToString("") { "%02x".format(it) }
    }
    private fun selection(options: Map<String, Set<String>>): String {
        check(options.size <= 4 && options.keys.all { it.length <= 256 } &&
            options.values.all { it.size <= 8 && it.all { id -> id.length <= 256 } })
        return fingerprint(options.toSortedMap().flatMap { (prompt, ids) -> listOf(prompt, ids.size.toString()) + ids.sorted() })
    }
    private inline fun observe(stage: Stage, question: Question, options: Map<String, Set<String>>, detail: () -> Detail) {
        val owned = session ?: return
        if (!BuildConfig.DEBUG || !owned.healthy()) return
        // Catch only probe construction/storage failures. No original UI callback/request/send is inside this boundary.
        try {
            val now = SystemClock.uptimeMillis()
            val card = fingerprint(listOf(question.id, question.hash, question.run, question.conversation))
            val selected = selection(options)
            owned.append(Record(stage, now, card, System.identityHashCode(question), selected, detail()))
        } catch (_: Throwable) { owned.failed() }
    }
    fun entry(question: Question, options: Map<String, Set<String>>, next: Boolean, advance: Boolean,
        editing: Boolean, pending: Boolean, choices: Boolean, reviewed: Boolean, open: Boolean) {
        observe(Stage.ENTRY, question, options) { Captured(next, advance, editing, pending, choices, reviewed, open) }
    }
    fun requested(question: Question, options: Map<String, Set<String>>, confirmation: BroaderChoiceConfirmation) {
        observe(Stage.REQUEST_RESULT, question, options) { Requested(confirmation.open,
            confirmation.questionId == question.id && confirmation.questionHash == question.hash,
            fingerprint(listOf(confirmation.optionId))) }
    }
    fun committed(question: Question, options: Map<String, Set<String>>, detail: Committed) {
        observe(Stage.CONTENT_COMMIT, question, options) { detail }
    }
    fun showing(question: Question, options: Map<String, Set<String>>, confirmation: BroaderChoiceConfirmation,
        eligible: Boolean, choice: Choice?) {
        observe(Stage.SHOWING_COMMIT, question, options) { Showing(confirmation.open, eligible, choice != null,
            confirmation.questionId == question.id && confirmation.questionHash == question.hash,
            fingerprint(listOf(choice?.id.orEmpty()))) }
    }

    /** Model records use a target digest in card, zero questionObject and an empty selection digest. */
    private inline fun observeTarget(stage: Stage, target: Target?, detail: () -> Detail) {
        val owned = session ?: return
        if (!BuildConfig.DEBUG || !owned.healthy()) return
        try {
            val now = SystemClock.uptimeMillis()
            val identity = target?.let { listOf(it.computerId, it.session, it.run, it.conversation,
                it.connectionId, it.archiveId, it.agentId, it.parentConversation) }.orEmpty()
            owned.append(Record(stage, now, fingerprint(identity), 0, fingerprint(emptyList()), detail()))
        } catch (_: Throwable) { owned.failed() }
    }
    fun verification(target: Target?, writer: VerificationWriter, verified: Boolean, frameVerified: Boolean,
        hasFrame: Boolean, hasPresentation: Boolean, frameMatchesSelected: Boolean) {
        observeTarget(Stage.VERIFICATION_WRITE, target) {
            Verification(writer, verified, frameVerified, hasFrame, hasPresentation, frameMatchesSelected)
        }
    }
    fun availability(target: Target, detail: Availability) {
        observeTarget(Stage.AVAILABILITY_COMMIT, target) { detail }
    }
    fun observationFailed() { if (BuildConfig.DEBUG) session?.failed() }
}
