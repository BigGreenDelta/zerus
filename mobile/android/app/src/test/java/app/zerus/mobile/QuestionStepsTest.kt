package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class QuestionStepsTest {
    private val prompts = listOf("first", "second").map {
        Prompt(it, "Question $it", listOf(Choice("yes", "Yes", "")), false, true, true)
    }
    private fun step(page: Int, options: Map<String, Set<String>> = emptyMap(), texts: Map<String, String> = emptyMap()) =
        QuestionSteps.current(prompts, page, options, texts)

    @Test fun answeringFirstQuestionEnablesNavigationBeforeWholeRequestIsComplete() {
        assertFalse(step(0).canAdvance)
        val next = step(0, mapOf("first" to setOf("yes")))
        assertEquals(1, next.nextPage)
        assertTrue(next.canAdvance)
        assertFalse(next.complete)
        assertFalse(next.reviewing)
    }

    @Test fun finalSubmissionIsOfferedOnlyAtTheLastCompleteQuestion() {
        val answers = prompts.associate { it.id to setOf("yes") }
        assertEquals(1, step(0, answers).nextPage)
        val final = step(1, answers)
        assertNull(final.nextPage)
        assertTrue(final.complete)
    }

    @Test fun browsingToLastQuestionReturnsToAnEarlierMissingOrInvalidAnswer() {
        val review = step(1, mapOf("second" to setOf("yes")))
        assertEquals(0, review.nextPage)
        assertTrue(review.canAdvance)
        assertTrue(review.reviewing)
        assertFalse(review.complete)
        assertEquals(0, step(1, prompts.associate { it.id to setOf("yes") }, mapOf("first" to "/invalid")).nextPage)
    }

    @Test fun currentAnswerMustBeValidButAnUnseenInvalidAnswerDoesNotBlockNavigation() {
        assertFalse(step(0, mapOf("first" to setOf("changed"))).canAdvance)
        assertFalse(step(0, texts = mapOf("first" to "/command")).canAdvance)
        assertTrue(step(0, texts = mapOf("first" to "Custom answer", "second" to "/invalid")).canAdvance)
        assertFalse(step(1, texts = mapOf("first" to "Custom answer", "second" to "/invalid")).complete)
    }

    @Test fun restoredPageIsBoundedAndEmptyRequestsNeverBecomeComplete() {
        assertEquals(0, step(-10).page)
        assertEquals(1, step(20).page)
        val empty = QuestionSteps.current(emptyList(), 20, emptyMap(), emptyMap())
        assertEquals(0, empty.page)
        assertNull(empty.nextPage)
        assertFalse(empty.complete)
        assertFalse(empty.canAdvance)
    }
}
