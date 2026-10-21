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
}
