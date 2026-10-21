package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

/** Actual recorder/snapshot methods over memory only; no Compose, Android clock or native UI proof. */
class QuestionReviewProbeTest {
    private fun record(stage: QuestionUiProbe.Stage = QuestionUiProbe.Stage.ENTRY, tick: Long = 1,
        card: String = "card", selection: String = "selection") = QuestionUiProbe.Record(stage, tick, card, 1, selection,
        QuestionUiProbe.Captured(false, true, true, false, true, true, false))

    private fun committed(start: Long?) = QuestionUiProbe.Committed(
        eligible = true, remaining = 3, reviewed = false, enabled = false, available = true, editing = true,
        pending = false, complete = true, disclosure = true, foreground = true, focused = true, next = false,
        advance = false, choices = true, open = false, dialog = false, visible = true, obscured = false,
        answerable = true, supported = true, reviewStartedAt = start)

    @Test fun currentContentSnapshotIsImmutableBoundedAndDoesNotRecordReviewTicks() {
        val session = QuestionUiProbe.Session()
        assertThrows(IllegalStateException::class.java) { session.contentSnapshot() }
        val first = record(QuestionUiProbe.Stage.CONTENT_COMMIT).copy(detail = committed(1000))
        session.append(first)
        val snapshot = session.contentSnapshot()
        assertEquals(1, snapshot.generation)
        repeat(200) { session.append(first.copy(uptimeMs = it.toLong() + 2)) }
        assertEquals(snapshot, session.contentSnapshot())
        session.append(first.copy(detail = committed(2000)))
        assertEquals(2, session.contentSnapshot().generation)
        assertEquals(1000L, (snapshot.record.detail as QuestionUiProbe.Committed).reviewStartedAt)
        assertEquals(2000L, (session.contentSnapshot().record.detail as QuestionUiProbe.Committed).reviewStartedAt)
        val report = session.close()
        assertEquals(2, report.records.size)
        assertEquals(QuestionUiProbe.Fault.NONE, report.fault)
        assertThrows(IllegalStateException::class.java) { session.contentSnapshot() }
        val failed = QuestionUiProbe.Session()
        failed.append(first); failed.failed()
        assertThrows(IllegalStateException::class.java) { failed.contentSnapshot() }
    }

    @Test fun currentContentRequiresTheSameActiveOwnerAndExactCurrentQuestionIdentity() {
        val question = Question("card", "hash", emptyList(), true, false, false, "input")
        val hash = java.security.MessageDigest.getInstance("SHA-256")
        for (part in listOf(question.id, question.hash, question.run, question.conversation)) {
            val bytes = part.toByteArray(Charsets.UTF_8)
            hash.update(byteArrayOf((bytes.size ushr 8).toByte(), bytes.size.toByte())); hash.update(bytes)
        }
        val card = hash.digest().joinToString("") { "%02x".format(it) }
        val owned = QuestionUiProbe.install()
        try {
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(owned, question) }
            owned.append(record(QuestionUiProbe.Stage.CONTENT_COMMIT).copy(card = card,
                questionObject = System.identityHashCode(question), detail = committed(1000)))
            assertEquals(1, checkNotNull(QuestionUiProbe.contentSnapshot(owned, question)).generation)
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(QuestionUiProbe.Session(), question) }
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(owned, question.copy()) }
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(owned, question.copy(hash = "other")) }
            owned.failed()
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(owned, question) }
        } finally { QuestionUiProbe.close(owned) }
        assertThrows(IllegalStateException::class.java) { QuestionUiProbe.contentSnapshot(owned, question) }
    }

    @Test fun firstEligibleReviewSnapshotCannotSilentlySelectALaterReset() {
        val session = QuestionUiProbe.Session()
        val first = record(QuestionUiProbe.Stage.CONTENT_COMMIT).copy(detail = committed(null))
        session.append(first)
        assertNull(session.firstReviewSnapshot(0))
        session.append(first.copy(detail = committed(1000).copy(eligible = false)))
        assertNull(session.firstReviewSnapshot(0))
        session.append(first.copy(detail = committed(2000)))
        val bound = checkNotNull(session.firstReviewSnapshot(2))
        session.append(first.copy(detail = committed(3000)))
        assertEquals(bound, session.firstReviewSnapshot(2))
        assertEquals(2000L, (bound.record.detail as QuestionUiProbe.Committed).reviewStartedAt)
        assertEquals(3000L, (session.contentSnapshot().record.detail as QuestionUiProbe.Committed).reviewStartedAt)
        assertNull(session.firstReviewSnapshot(4))
        assertThrows(IllegalStateException::class.java) { session.firstReviewSnapshot(-1) }
        assertThrows(IllegalStateException::class.java) { session.firstReviewSnapshot(5) }
        session.close()
        assertThrows(IllegalStateException::class.java) { session.firstReviewSnapshot(2) }
    }

    private fun questionRecord(question: Question, detail: QuestionUiProbe.Committed): QuestionUiProbe.Record {
        val hash = java.security.MessageDigest.getInstance("SHA-256")
        for (part in listOf(question.id, question.hash, question.run, question.conversation)) {
            val bytes = part.toByteArray(Charsets.UTF_8)
            hash.update(byteArrayOf((bytes.size ushr 8).toByte(), bytes.size.toByte())); hash.update(bytes)
        }
        return record(QuestionUiProbe.Stage.CONTENT_COMMIT).copy(
            card = hash.digest().joinToString("") { "%02x".format(it) },
            questionObject = System.identityHashCode(question), detail = detail)
    }

    @Test fun forwardReviewResetRemainsInsideTheFirstImmutableWindow() {
        val question = Question("card", "hash", emptyList(), true, false, false, "input")
        val owned = QuestionUiProbe.install()
        try {
            val first = questionRecord(question, committed(1000))
            owned.append(first)
            val bound = checkNotNull(QuestionUiProbe.contentSnapshot(owned, question, 0))
            assertTrue(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 1000, 4000))
            owned.append(first.copy(detail = committed(1000).copy(eligible = false)))
            assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000))
            owned.append(first.copy(detail = committed(null)))
            assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2100, 4000))
            owned.append(first.copy(detail = committed(2200)))
            val latest = checkNotNull(QuestionUiProbe.contentSnapshot(owned, question))
            // The original fixture's exact-start comparison rejects this controlled forward reset.
            assertFalse((latest.record.detail as QuestionUiProbe.Committed).reviewStartedAt ==
                (bound.record.detail as QuestionUiProbe.Committed).reviewStartedAt)
            assertTrue(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2300, 4000))
            assertEquals(bound, owned.firstReviewSnapshot(0))
            assertEquals(1000L, (bound.record.detail as QuestionUiProbe.Committed).reviewStartedAt)
            assertTrue(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 3999, 4000))
            assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 4000, 4000))
            assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 4001, 5200))
        } finally { QuestionUiProbe.close(owned) }
    }

    @Test fun earlyReviewRefusesUnknownBackwardFutureExpiredAndOverflowingClocks() {
        data class Case(val first: Long?, val current: Long?, val now: Long, val end: Long,
            val eligible: Boolean = true)
        val cases = listOf(
            Case(null, 1000, 2300, 4000),
            Case(1000, null, 2300, 4000), Case(1000, 999, 2300, 4000),
            Case(1000, 2301, 2300, 4000), Case(1000, 1000, 999, 4000),
            Case(1000, 2200, 4000, 4000), Case(1000, 2200, 4001, 4000),
            Case(1000, 2200, 2300, 5200), Case(1000, 2200, 2300, 3999),
            Case(1000, 2200, 2300, 4000, false), Case(-1, 0, 1, 2999),
            Case(Long.MAX_VALUE - 1000, Long.MAX_VALUE - 500, Long.MAX_VALUE, Long.MAX_VALUE))
        for (case in cases) {
            val question = Question("card", "hash", emptyList(), true, false, false, "input")
            val owned = QuestionUiProbe.install()
            try {
                val first = questionRecord(question, committed(case.first))
                owned.append(first)
                val bound = owned.contentSnapshot()
                owned.append(first.copy(detail = committed(case.current).copy(eligible = case.eligible)))
                assertFalse(case.toString(), QuestionUiProbe.earlyReviewCurrent(owned, question, bound, case.now, case.end))
            } finally { QuestionUiProbe.close(owned) }
        }
    }

    @Test fun earlyReviewRefusesChangedIdentitySelectionGenerationAndUnrecordedBinding() {
        val question = Question("card", "hash", emptyList(), true, false, false, "input")
        val owned = QuestionUiProbe.install()
        try {
            val first = questionRecord(question, committed(1000))
            owned.append(first)
            val bound = owned.contentSnapshot()
            for (changed in listOf(bound.copy(generation = 0), bound.copy(generation = 2),
                bound.copy(record = first.copy(card = "other")),
                bound.copy(record = first.copy(questionObject = first.questionObject + 1)),
                bound.copy(record = first.copy(selection = "other")),
                bound.copy(record = first.copy(detail = committed(900))),
                bound.copy(record = first.copy(detail = committed(1000).copy(eligible = false))),
                bound.copy(record = first.copy(stage = QuestionUiProbe.Stage.ENTRY)))) {
                assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, changed, 2000, 4000))
            }
            owned.append(first.copy(selection = "other", detail = committed(1500)))
            assertFalse(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000))
            for (changed in listOf(first.copy(card = "other"),
                first.copy(questionObject = first.questionObject + 1))) {
                owned.append(changed)
                assertThrows(IllegalStateException::class.java) {
                    QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000)
                }
            }
        } finally { QuestionUiProbe.close(owned) }
    }

    @Test fun earlyReviewRequiresTheActiveHealthyOwnerAndExactQuestionObject() {
        val question = Question("card", "hash", emptyList(), true, false, false, "input")
        val owned = QuestionUiProbe.install()
        val bound: QuestionUiProbe.ContentSnapshot
        try {
            owned.append(questionRecord(question, committed(1000)))
            bound = owned.contentSnapshot()
            assertTrue(QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000))
            assertThrows(IllegalStateException::class.java) {
                QuestionUiProbe.earlyReviewCurrent(QuestionUiProbe.Session(), question, bound, 2000, 4000)
            }
            for (changed in listOf(question.copy(), question.copy(hash = "other"))) {
                assertThrows(IllegalStateException::class.java) {
                    QuestionUiProbe.earlyReviewCurrent(owned, changed, bound, 2000, 4000)
                }
            }
            owned.failed()
            assertThrows(IllegalStateException::class.java) {
                QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000)
            }
        } finally { QuestionUiProbe.close(owned) }
        assertThrows(IllegalStateException::class.java) {
            QuestionUiProbe.earlyReviewCurrent(owned, question, bound, 2000, 4000)
        }
    }
}
