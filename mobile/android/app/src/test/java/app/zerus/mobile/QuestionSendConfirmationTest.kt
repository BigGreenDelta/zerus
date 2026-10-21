package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.withTimeout
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class QuestionSendConfirmationTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val prompt = Prompt("prompt", "Choose", listOf(Choice("allow", "Allow", ""), Choice("deny", "Deny", "")), false, true, true)
    private val question = Question("question", "hash", listOf(prompt), true, true, false, "pending",
        source = "claude", run = target.run, conversation = target.conversation, toolCallId = "tool")
    private val answers = QuestionPolicies.answers(question.prompts, emptyMap(), mapOf("prompt" to "Exact  Other answer")).toString()
    private val draft = Draft(target, "Exact  Other answer", questionId = question.id, questionHash = question.hash,
        answers = answers, generation = "generation", revision = "revision")
    private fun intent(q: Question = question, payload: String = answers) = QuestionSendPolicies.capture(target, 1, q, draft, payload)

    @Test fun completeAnswerAndSkipCannotCrossQuestionRunOrConversationBindings() {
        val selected = mapOf("prompt" to setOf("allow"))
        val complete = QuestionPolicies.answers(question.prompts, selected, emptyMap()).toString()
        val skip = JSONArray().put(JSONObject().put("question_id", "prompt").put("skip", true)
            .put("selected_option_ids", JSONArray()).put("text", "")).toString()
        assertTrue(QuestionPolicies.complete(question.prompts, selected, emptyMap()))
        assertTrue(QuestionSendPolicies.bound(question, target))
        assertTrue(QuestionSendPolicies.available(question, target, complete))
        assertTrue(QuestionSendPolicies.available(question, target, skip))
        for ((changedTarget, changedQuestion) in listOf(
            target.copy(run = "replacement-run") to question,
            target.copy(conversation = "replacement-conversation") to question,
            target to question.copy(run = "replacement-run"),
            target to question.copy(conversation = "replacement-conversation"),
            target to question.copy(run = ""),
            target to question.copy(conversation = ""),
            target.copy(run = "") to question.copy(run = ""),
            target.copy(run = " \t") to question.copy(run = " \t"))) {
            assertFalse(QuestionSendPolicies.bound(changedQuestion, changedTarget))
            assertFalse(QuestionSendPolicies.available(changedQuestion, changedTarget, complete))
            assertFalse(QuestionSendPolicies.available(changedQuestion, changedTarget, skip))
            val changedDraft = draft.copy(target = changedTarget, answers = complete)
            val pending = QuestionSendPolicies.capture(changedTarget, 1, changedQuestion, changedDraft, complete)
            assertFalse(pending.matches(changedTarget, 1, changedQuestion, changedDraft))
            val prepared = changedDraft.copy(status = "submitting", requestId = "request")
            assertFalse(pending.permitsPrepared(changedTarget, 1, changedQuestion, prepared, "request", false, false))
            assertFalse(pending.permitsPrepared(changedTarget, 1, changedQuestion, prepared, "request", true, true))
        }
    }

    @Test fun exactOpaqueStartupRunAndMatchingEmptyConversationRetainExistingConsentRules() {
        val startupTarget = target.copy(run = "fixture-run", conversation = "")
        val startup = question.copy(id = "claude-trust:fixture-hash", source = "claude_folder_trust",
            run = startupTarget.run, conversation = "", canSkip = false)
        val complete = QuestionPolicies.answers(startup.prompts, mapOf("prompt" to setOf("allow")), emptyMap()).toString()
        val original = draft.copy(target = startupTarget, questionId = startup.id, answers = complete)
        assertTrue(QuestionSendPolicies.bound(startup, startupTarget))
        assertTrue(QuestionSendPolicies.available(startup, startupTarget, complete))
        val pending = QuestionSendPolicies.capture(startupTarget, 1, startup, original, complete)
        assertTrue(pending.matches(startupTarget, 1, startup, original))
        val prepared = original.copy(status = "submitting", requestId = "request")
        assertTrue(pending.permitsPrepared(startupTarget, 1, startup, prepared, "request", false, false))
        assertFalse(pending.permitsPrepared(startupTarget, 1, startup, prepared, "request", true, false))
        assertTrue(pending.permitsPrepared(startupTarget, 1, startup, prepared, "request", true, true))
        assertFalse(QuestionSendPolicies.available(startup.copy(canAnswer = false), startupTarget, complete))
        assertFalse(QuestionSendPolicies.bound(startup.copy(conversation = "started"), startupTarget))
        assertFalse(QuestionSendPolicies.bound(startup, startupTarget.copy(conversation = "started")))
        assertFalse(QuestionSendPolicies.bound(startup.copy(run = ""), startupTarget.copy(run = "")))
    }

    @Test fun nativeKimiCacheChoicesKeepExactAnswersWithoutSecondConsent() {
        val cachePrompt = prompt.copy(id = "cache", choices = (0..3).map { Choice("cache_$it", "Native choice $it", "") })
        val cache = question.copy(source = "kimi_cache_hint", toolCallId = "kimi_cache_hint", prompts = listOf(cachePrompt))
        for (choice in 0..3) {
            val payload = QuestionPolicies.answers(cache.prompts, mapOf("cache" to setOf("cache_$choice")), emptyMap()).toString()
            val pending = intent(cache, payload); val gate = QuestionSendGate()
            assertSame(pending, gate.request(pending, true)); assertNull(gate.pending)
            val prepared = draft.copy(status = "submitting", requestId = "request", answers = payload)
            assertTrue(pending.permitsPrepared(target, 1, cache, prepared, "request", true, false))
            assertEquals("cache_$choice", JSONArray(pending.answers).getJSONObject(0).getJSONArray("selected_option_ids").getString(0))
            assertFalse(pending.permitsPrepared(target, 1, cache.copy(hash = "changed"), prepared, "request", true, false))
            assertFalse(pending.permitsPrepared(target.copy(run = "changed"), 1, cache, prepared, "request", true, false))
            assertFalse(pending.permitsPrepared(target, 1, cache.copy(source = "claude"), prepared, "request", true, false))
        }
        assertTrue(QuestionSendPolicies.requiresCacheConsent(cache.copy(toolCallId = "other"), true))
        assertTrue(QuestionSendPolicies.requiresCacheConsent(question, true))
    }

    @Test fun defaultColdRequestCannotReleaseTransportOrChangeSavedForm() {
        val gate = QuestionSendGate(); val original = draft
        val pending = intent()
        assertNull(gate.request(pending, true))
        assertSame(pending, gate.pending)
        assertEquals(original, pending.draft)
        assertEquals("editing", pending.draft.status)
        assertEquals("", pending.draft.requestId)
        // A second Enter/IME/icon event must not replace the first captured form.
        assertNull(gate.request(intent(payload = answers.replace("Exact", "Changed")), true))
        assertSame(pending, gate.pending)
        gate.cancel(pending)
        assertNull(gate.pending)
        assertNull(gate.confirm(pending, true))
        assertEquals(original, draft)
    }
    @Test fun confirmationPostsExactCapturedBytesAtMostOnce() {
        val gate = QuestionSendGate(); val pending = intent(); val posted = mutableListOf<String>()
        gate.request(pending, true)
        fun confirm() { gate.confirm(pending, pending.matches(target, 1, question, draft))?.let { posted += it.answers } }
        confirm(); confirm()
        assertEquals(listOf(answers), posted)
        assertTrue(JSONArray(posted.single()).getJSONObject(0).getString("text").contains("Exact  Other"))
        assertNull(gate.pending)
    }
    @Test fun inactiveDuplicateCannotReportRefusalWhileAcceptedAnswerIsSending() {
        val gate = QuestionSendGate(); val pending = intent(); var accepted = 0; var refused = 0
        fun callback(intent: QuestionSendIntent, valid: Boolean) {
            if (!gate.active(intent)) return
            if (gate.confirm(intent, valid) == null) refused++ else accepted++
        }
        gate.request(pending, true)
        callback(pending, true); callback(pending, false)
        assertEquals(1, accepted); assertEquals(0, refused)
        val next = intent(); gate.request(next, true)
        callback(next, false); callback(next, false)
        assertEquals(1, accepted); assertEquals(1, refused)
    }
    @Test fun retainedOwnerSurvivesRecompositionButNewProcessCannotRestoreConsent() {
        val retained = QuestionSendGate(); val pending = intent()
        retained.request(pending, true)
        val afterRotation = retained
        assertSame(pending, afterRotation.pending)
        val restarted = QuestionSendGate()
        assertNull(restarted.confirm(pending, true))
        val recoveredDraft = MessageCodec.state(MessageCodec.stateJson(MessageState(drafts = listOf(draft)))).drafts.single()
        assertTrue(ContextPolicies.sameDraft(draft, recoveredDraft))
        assertNull(restarted.request(QuestionSendPolicies.capture(target, 1, question, recoveredDraft, answers), true))
    }
    @Test fun navigationTargetQuestionSchemaAndRawDraftChangesInvalidateConsent() {
        val pending = intent()
        assertTrue(pending.matches(target, 1, question, draft))
        assertFalse(pending.matches(target, 2, question, draft))
        for (changed in listOf(target.copy(computerId = "other"), target.copy(connectionId = "other"),
            target.copy(run = "other"), target.copy(conversation = "other"), target.copy(session = "other")))
            assertFalse(pending.matches(changed, 1, question, draft))
        for (changed in listOf(question.copy(id = "other"), question.copy(hash = "other"), question.copy(toolCallId = "other"),
            question.copy(source = "other"), question.copy(run = "other"), question.copy(conversation = "other"),
            question.copy(prompts = listOf(prompt.copy(text = "Changed", choices = prompt.choices.reversed()))),
            question.copy(canAnswer = false), question.copy(delivery = "pending", canAnswer = false, canSkip = true)))
            assertFalse(pending.matches(target, 1, changed, draft))
        for (changed in listOf(draft.copy(generation = "other"), draft.copy(revision = "other"),
            draft.copy(text = "Changed"), draft.copy(answers = answers.replace("  ", " ")), draft.copy(status = "uncertain")))
            assertFalse(pending.matches(target, 1, question, changed))
        val gate = QuestionSendGate(); gate.request(pending, true)
        assertNull(gate.confirm(pending, pending.matches(target, 1, question, draft.copy(revision = "changed"))))
        assertNull(gate.confirm(pending, true))
    }
    @Test fun approvalAndSkipUseTheSameColdConsentWithTheirOwnCapability() {
        val approval = question.copy(approval = true, canSkip = false)
        val allow = QuestionPolicies.answers(approval.prompts, mapOf("prompt" to setOf("allow")), emptyMap()).toString()
        val skip = JSONArray().put(JSONObject().put("question_id", "prompt").put("skip", true)
            .put("selected_option_ids", JSONArray()).put("text", "")).toString()
        for ((q, payload) in listOf(approval to allow, question.copy(canAnswer = false) to skip)) {
            val gate = QuestionSendGate(); val pending = intent(q, payload)
            assertTrue(pending.matches(target, 1, q, draft))
            assertNull(gate.request(pending, true))
            assertEquals(payload, gate.confirm(pending, true)?.answers)
        }
        assertFalse(QuestionSendPolicies.available(question.copy(canSkip = false), target, skip))
        assertFalse(QuestionSendPolicies.available(question.copy(canAnswer = false), target, allow))
    }
    @Test fun nativeCacheHintAndElapsedWarmTtlRequireConsentWhileUnknownCacheDoesNot() {
        val nativeCold = JSONObject().put("cache_hint", JSONObject().put("status", "cold"))
        val expires = JSONObject().put("session_usage", JSONObject().put("prompt_cache", JSONObject().put("status", "warm").put("expires_at", 100)))
        for (raw in listOf(nativeCold, expires)) {
            val gate = QuestionSendGate()
            assertNull(gate.request(intent(), ContextPresentation.isCold(raw, 100.0)))
            assertNotNull(gate.pending)
        }
        val gate = QuestionSendGate(); val warm = intent()
        assertSame(warm, gate.request(warm, ContextPresentation.isCold(expires, 99.0)))
        assertNull(gate.pending)
        assertFalse(ContextPresentation.isCold(JSONObject(), 100.0))
    }
    @Test fun expiryOrStateChangeDuringPersistenceCannotReleasePreparedAnswer() {
        val pending = intent(); val prepared = draft.copy(status = "submitting", requestId = "request", answers = answers)
        assertTrue(pending.permitsPrepared(target, 1, question, prepared, "request", false, false))
        assertFalse(pending.permitsPrepared(target, 1, question, prepared, "request", true, false))
        assertTrue(pending.permitsPrepared(target, 1, question, prepared, "request", true, true))
        for (changed in listOf(prepared.copy(requestId = "other"), prepared.copy(generation = "other"),
            prepared.copy(revision = "other"), prepared.copy(answers = answers.replace("  ", " ")),
            prepared.copy(status = "uncertain")))
            assertFalse(pending.permitsPrepared(target, 1, question, changed, "request", true, true))
        assertFalse(pending.permitsPrepared(target, 2, question, prepared, "request", true, true))
        assertFalse(pending.permitsPrepared(target, 1, question.copy(toolCallId = "other"), prepared, "request", true, true))
    }
    @Test fun coldCancelMakesZeroApiRequestsAndExplicitConsentPostsOnlyOnePinnedAnswer() = runBlocking {
        MockWebServer().use { server ->
            server.start()
            server.enqueue(MockResponse().setBody("{\"request_id\":\"request\",\"state\":\"completed\"}"))
            val connection = Connection(target.connectionId, "Fixture", server.url("/").toString().trimEnd('/'), "fixture-token")
            val api = RelayApi(); val gate = QuestionSendGate()
            val canceled = intent(); gate.request(canceled, true); gate.cancel(canceled)
            assertNull(gate.confirm(canceled, true)); assertEquals(0, server.requestCount)
            val pending = intent(); assertNull(gate.request(pending, true)); assertEquals(0, server.requestCount)
            repeat(2) {
                gate.confirm(pending, pending.matches(target, 1, question, draft))?.let { approved ->
                    api.submit(connection, approved.target, "answer", approved.target.json().put("request_id", "request")
                        .put("question_id", approved.question.id).put("expected_question_hash", approved.question.hash)
                        .put("answers", JSONArray(approved.answers)), "request")
                }
            }
            assertEquals(1, server.requestCount)
            val payload = JSONObject(server.takeRequest().body.readUtf8()).getJSONObject("payload")
            assertEquals(target.run, payload.getString("expected_run_id"))
            assertEquals(question.hash, payload.getString("expected_question_hash"))
            assertEquals(answers, payload.getJSONArray("answers").toString())
        }
    }
    @Test fun refusalCannotLeakToAnotherComputerRunOrQuestion() {
        val refusal = QuestionSendRefusal(target, question.id, question.hash, "Cache expired; nothing was sent.")
        assertEquals(refusal.text, refusal.textFor(target, question))
        assertEquals("", refusal.textFor(target.copy(computerId = "other"), question))
        assertEquals("", refusal.textFor(target.copy(run = "other"), question))
        assertEquals("", refusal.textFor(target, question.copy(hash = "other")))
        assertEquals("", refusal.textFor(target, question.copy(id = "other")))
    }
    @Test fun benignPollingMetadataDoesNotInvalidateThePinnedQuestion() {
        assertTrue(intent().matches(target, 1, question.copy(createdAt = 500.0,
            unavailableReason = "Previous observation", delivery = "refreshed"), draft))
    }
    @Test fun expiryRestoresExactOriginalApprovalFormOnlyWhilePreparedRequestIsOwned() {
        val allow = QuestionPolicies.answers(question.prompts, mapOf("prompt" to setOf("allow")), emptyMap()).toString()
        val pending = intent(question.copy(approval = true), allow)
        val prepared = draft.copy(status = "submitting", requestId = "request", answers = allow)
        assertEquals(draft, pending.restoreAfterExpiry(MessageState(drafts = listOf(prepared)), "request").drafts.single())
        for (changed in listOf(prepared.copy(requestId = "other"), prepared.copy(revision = "other"),
            prepared.copy(generation = "other"), prepared.copy(text = "changed"), prepared.copy(answers = answers),
            prepared.copy(status = "uncertain"), prepared.copy(target = target.copy(run = "other"))))
            assertEquals(changed, pending.restoreAfterExpiry(MessageState(drafts = listOf(changed)), "request").drafts.single())
    }
    private fun expiryDuringFlush(changeDuringRestore: Boolean) = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val flushEntered = CompletableDeferred<Unit>(); val releaseFlush = CompletableDeferred<Unit>()
        val restoreEntered = CompletableDeferred<Unit>(); val releaseRestore = CompletableDeferred<Unit>()
        val pending = intent(); val prepared = draft.copy(status = "submitting", requestId = "request")
        var posts = 0; var cold = false
        val writer = OrderedStatePersistence(MessageState(drafts = listOf(draft)), scope, { state ->
            if (state.drafts.single().status == "submitting") { flushEntered.complete(Unit); releaseFlush.await() }
            else if (flushEntered.isCompleted) { restoreEntered.complete(Unit); releaseRestore.await() }
        }, Dispatchers.Unconfined)
        val sending = scope.async {
            writer.edit { it.copy(drafts = listOf(prepared)) }; writer.flush()
            if (cold) writer.durable({ pending.restoreAfterExpiry(it, "request") })
            else posts++
        }
        try {
            withTimeout(5_000) { flushEntered.await() }
            cold = true; releaseFlush.complete(Unit)
            withTimeout(5_000) { restoreEntered.await() }
            val changed = draft.copy(text = "Later saved form", revision = "later")
            if (changeDuringRestore) writer.edit { it.copy(drafts = listOf(changed)) }
            releaseRestore.complete(Unit)
            withTimeout(5_000) { sending.await(); writer.flush() }
            assertEquals(0, posts)
            val expected = if (changeDuringRestore) changed else draft
            assertEquals(expected, writer.state.value.drafts.single())
            assertEquals(expected, writer.durableState.drafts.single())
            val gate = QuestionSendGate()
            assertNull(gate.request(QuestionSendPolicies.capture(target, 1, question, expected, answers), true))
            assertEquals(0, posts)
        } finally { releaseFlush.complete(Unit); releaseRestore.complete(Unit); scope.cancel() }
    }
    @Test fun warmCacheExpiringDuringSavedStatusFlushRestoresEditingWithoutTransport() = expiryDuringFlush(false)
    @Test fun changedFormDuringExpiryRestorationIsNeverOverwritten() = expiryDuringFlush(true)
    @Test fun payloadAndQuestionSchemaAreFrozenBeforeModal() {
        val choices = prompt.choices.toMutableList(); val prompts = mutableListOf(prompt.copy(choices = choices))
        val mutable = question.copy(prompts = prompts)
        val pending = intent(mutable)
        choices.clear(); prompts.clear()
        assertEquals(question.prompts, pending.question.prompts)
        assertTrue(pending.matches(target, 1, question, draft))
        val foreign = QuestionSendPolicies.capture(target, 1, question, draft, answers.replace("prompt", "foreign"))
        assertFalse(foreign.matches(target, 1, question, draft))
    }
}
