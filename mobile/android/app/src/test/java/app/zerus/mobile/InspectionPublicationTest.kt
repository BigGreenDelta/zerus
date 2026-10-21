package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test

/** Uses the production preparation owner. No Android/Compose/native execution is claimed. */
class InspectionPublicationTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val question = Question("question", "hash", listOf(Prompt("approval", "Review", listOf(
        Choice("1", "Once", "", "once"), Choice("2", "Session", "This session only", "broader")), false, true, false)),
        true, false, false, "", source = "kimi_tool_approval", approval = true,
        run = target.run, conversation = target.conversation, approvalChoices = true)
    private val options = mapOf("approval" to setOf("2"))

    @Test fun identicalCardStaysEligibleUntilOnePreparedPublication() = runBlocking {
        val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        val confirmation = BroaderChoiceConfirmation().request(question, options, true)
        assertTrue(confirmation.open)
        // Policy replay of the original intermediate false value, not execution of an old model binary.
        assertNull(confirmation.showing(question, options, false))
        var visible = question; var available = true; val publications = mutableListOf<Question>()
        val work = async(start = CoroutineStart.UNDISPATCHED) {
            InspectionPublication.prepare(owns = { true }, capture = { question }, build = {
                entered.complete(Unit); release.await(); it.copy()
            }, unchanged = { true }, publish = { _, prepared -> visible = prepared; available = true; publications += prepared })
        }
        entered.await()
        assertTrue(available); assertSame(question, visible)
        assertNotNull(confirmation.showing(visible, options, available)); assertTrue(publications.isEmpty())
        release.complete(Unit)
        assertEquals(InspectionPublication.Result.PUBLISHED, work.await())
        assertEquals(listOf(question), publications)
        assertNotNull(confirmation.showing(visible, options, available))
    }

    @Test fun changedRunOrHashPublishesCompleteIdentityAndRefusesTheOldIntent() = runBlocking {
        val answers = QuestionPolicies.answers(question.prompts, options, emptyMap()).toString()
        val draft = Draft(target, "", questionId = question.id, questionHash = question.hash, answers = answers)
        val intent = QuestionSendPolicies.capture(target, 1, question, draft, draft.answers)
        val confirmation = BroaderChoiceConfirmation().request(question, options, true)
        assertTrue(intent.matches(target, 1, question, draft))
        assertNotNull(confirmation.showing(question, options, true))
        val sending = draft.copy(status = "submitting", requestId = "request")
        assertTrue(intent.permitsPrepared(target, 1, question, sending, "request", false, false))
        assertFalse(intent.permitsPrepared(target, 1, question, sending, "request", true, false))
        for ((nextTarget, nextQuestion) in listOf(target.copy(run = "new-run") to question.copy(run = "new-run"),
            target to question.copy(hash = "new-hash"))) {
            var visible = target to question
            assertEquals(InspectionPublication.Result.PUBLISHED, InspectionPublication.prepare(owns = { true },
                capture = { nextTarget to nextQuestion }, build = { it }, unchanged = { true }, publish = { _, prepared -> visible = prepared }))
            assertEquals(nextTarget to nextQuestion, visible)
            assertFalse(intent.matches(visible.first, 1, visible.second, draft))
            assertFalse(intent.permitsPrepared(visible.first, 1, visible.second, sending, "request", true, true))
            assertNotEquals(Triple(target.key, question.id, question.hash), Triple(visible.first.key, visible.second.id, visible.second.hash))
            if (nextQuestion.hash != question.hash) assertNull(confirmation.showing(visible.second, options, true))
        }
        // Actual Compose remember-key retirement remains an unchanged source/native UI contract.
    }

    @Test fun changedDraftRevisionRebuildsFromTheLatestImmutableState() = runBlocking {
        val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
        var state = MessageState(drafts = listOf(Draft(target, "old", revision = "r1")))
        var builds = 0; var published: MessageState? = null
        val work = async(start = CoroutineStart.UNDISPATCHED) {
            InspectionPublication.prepare(owns = { true }, capture = { state }, build = {
                builds++; if (builds == 1) { entered.complete(Unit); release.await() }; it
            }, unchanged = { it === state }, publish = { _, prepared -> published = prepared })
        }
        entered.await(); state = state.copy(drafts = listOf(state.drafts.single().copy(text = "new", revision = "r2"))); release.complete(Unit)
        assertEquals(InspectionPublication.Result.PUBLISHED, work.await())
        assertEquals(2, builds); assertSame(state, published); assertEquals("r2", published!!.drafts.single().revision)
    }

    @Test fun changedHistoryIdentityIsRebasedBeforePublication() = runBlocking {
        var history = listOf(Event("one", "AgentMessage", "old", "")); var builds = 0
        var published: List<Event>? = null
        assertEquals(InspectionPublication.Result.PUBLISHED, InspectionPublication.prepare(owns = { true }, capture = { history },
            build = { captured -> builds++; if (builds == 1) history = history + Event("two", "AgentMessage", "new", ""); captured },
            unchanged = { it === history }, publish = { _, prepared -> published = prepared }))
        assertEquals(2, builds); assertSame(history, published); assertEquals(listOf("one", "two"), published!!.map { it.id })
    }

    @Test fun threeRacesRefuseWithoutPublishingAnUnpreparedFallback() = runBlocking {
        var revision = 0; var builds = 0; var publications = 0
        val result = InspectionPublication.prepare(owns = { true }, capture = { revision },
            build = { builds++; revision++; it }, unchanged = { it == revision }, publish = { _, _ -> publications++ })
        assertEquals(InspectionPublication.Result.UNSTABLE, result)
        assertEquals(3, builds); assertEquals(0, publications)
    }

    @Test fun newerOwnerDuringPreparationCannotBePublishedOrInvalidatedByTheOldOwner() = runBlocking {
        var owner = 1; var newerVerified = true; var publications = 0
        val result = InspectionPublication.prepare(owns = { owner == 1 }, capture = { owner },
            build = { owner = 2; it }, unchanged = { true }, publish = { _, _ -> publications++ })
        // Model uses UNSTABLE only for its owner-bound error path; SUPERSEDED returns before it.
        if (result == InspectionPublication.Result.UNSTABLE && owner == 1) newerVerified = false
        assertEquals(InspectionPublication.Result.SUPERSEDED, result)
        assertEquals(0, publications); assertTrue(newerVerified)
    }

    @Test fun cancellationDoesNotPublishOrConvertIntoAnOwnerFailure() = runBlocking {
        val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>(); var publications = 0
        val work = async(start = CoroutineStart.UNDISPATCHED) {
            InspectionPublication.prepare(owns = { true }, capture = { 1 }, build = {
                entered.complete(Unit); release.await(); it
            }, unchanged = { true }, publish = { _, _ -> publications++ })
        }
        entered.await(); work.cancelAndJoin()
        assertTrue(work.isCancelled); assertEquals(0, publications)
    }

    @Test fun buildErrorsRemainErrorsAndNeverBecomeAQualifiedPublication() = runBlocking {
        var publications = 0
        try {
            InspectionPublication.prepare<Int, Int>(owns = { true }, capture = { 1 },
                build = { throw IllegalStateException("preparation failure") }, unchanged = { true }, publish = { _, _ -> publications++ })
            fail("The original build failure must propagate")
        } catch (expected: IllegalStateException) { assertEquals("preparation failure", expected.message) }
        assertEquals(0, publications)
    }
}
