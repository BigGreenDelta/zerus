package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

/** Memory-only recorder contracts. These do not exercise Compose, Android clocks or native delivery. */
class QuestionUiProbeTest {
    private fun record(stage: QuestionUiProbe.Stage = QuestionUiProbe.Stage.ENTRY, tick: Long = 1,
        card: String = "card", selection: String = "selection") = QuestionUiProbe.Record(stage, tick, card, 1, selection,
        QuestionUiProbe.Captured(false, true, true, false, true, true, false))

    @Test fun inactiveProbeDoesNotInspectOptionsOrReadAndroidClock() {
        assertFalse(QuestionUiProbe.enabled)
        val poison = object : Map<String, Set<String>> by emptyMap() {
            override val size: Int get() = error("inactive options were inspected")
        }
        val question = Question("card", "hash", emptyList(), true, false, false, "input")
        QuestionUiProbe.entry(question, poison, false, true, true, false, true, true, false)
    }

    @Test fun sessionOwnershipCannotBeReplacedOrClosedByAnotherSession() {
        val owned = QuestionUiProbe.install()
        try {
            assertTrue(QuestionUiProbe.enabled)
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.install() }
            assertThrows(IllegalStateException::class.java) { QuestionUiProbe.close(QuestionUiProbe.Session()) }
            assertTrue(QuestionUiProbe.enabled)
        } finally { assertTrue(QuestionUiProbe.close(owned).closed) }
        assertFalse(QuestionUiProbe.enabled)
        assertThrows(IllegalStateException::class.java) { QuestionUiProbe.close(owned) }
    }

    @Test fun boundedRecorderKeepsFirst256AndMarksOverflowUnknown() {
        val session = QuestionUiProbe.Session()
        repeat(QuestionUiProbe.MAX_RECORDS) { session.append(record(tick = it.toLong())) }
        session.append(record(tick = 999))
        session.append(record(tick = 1000))
        val report = session.close()
        assertEquals(QuestionUiProbe.Fault.OVERFLOW, report.fault)
        assertEquals(256, report.records.size)
        assertEquals(255L, report.records.last().uptimeMs)
        assertThrows(IllegalStateException::class.java) { session.close() }
    }

    @Test fun commitDedupUsesValuesNotTimeAndSurvivesInterleavedStages() {
        val session = QuestionUiProbe.Session()
        val commit = record(QuestionUiProbe.Stage.CONTENT_COMMIT)
        val showing = record(QuestionUiProbe.Stage.SHOWING_COMMIT)
        session.append(commit); session.append(showing)
        session.append(commit.copy(uptimeMs = 100)); session.append(showing.copy(uptimeMs = 101))
        session.append(commit.copy(card = "new-card"))
        session.append(showing.copy(selection = "new-selection"))
        val report = session.close()
        assertEquals(QuestionUiProbe.Fault.NONE, report.fault)
        assertEquals(4, report.records.size)
    }

    @Test fun probeFaultRetainsEarlierEvidenceAndStopsObservation() {
        val session = QuestionUiProbe.Session()
        session.append(record()); session.failed(); session.append(record(tick = 2))
        val report = session.close()
        assertTrue(report.closed)
        assertEquals(QuestionUiProbe.Fault.OBSERVATION, report.fault)
        assertEquals(listOf(record()), report.records)
    }
}
