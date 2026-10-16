package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException
import java.util.Collections

/** Controlled synthetic writes, no Android preferences, keys or document files. */
class PersistenceRaceTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val file = Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "fixture.txt", "text/plain", 1, "0".repeat(64))
    private val request = "11111111-1111-4111-8111-111111111111"
    private suspend fun until(condition: () -> Boolean) = withTimeout(5000) { while (!condition()) delay(1) }

    @Test fun rapidEditsCoalesceAndExplicitBarrierPersistsTheirOrder() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val writes = Collections.synchronizedList(mutableListOf<Int>())
            val actor = OrderedStatePersistence(0, scope, { writes.add(it) }, Dispatchers.Unconfined, debounceMillis = 1000)
            actor.edit { it + 1 }; actor.edit { it + 2 }; actor.edit { it + 3 }
            assertEquals(6, actor.state.value)
            assertEquals(6, actor.flush())
            assertEquals(listOf(6), writes.toList())
            assertEquals(6, actor.durableState)
        } finally { scope.cancel() }
    }
    @Test fun failedCriticalWriteNeverAcknowledgesOrReleasesTransportAndLaterFlushRecovers() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            var fail = true
            val actor = OrderedStatePersistence(0, scope, { if (fail) throw IOException("synthetic storage failure") }, Dispatchers.Unconfined)
            var transport = false
            val result = runCatching { actor.durable({ it + 1 }); transport = true }
            assertTrue(result.isFailure); assertFalse(transport)
            assertEquals(0, actor.durableState); assertEquals(0, actor.state.value)
            fail = false; actor.edit { it + 2 }
            assertEquals(2, actor.flush()); assertEquals(2, actor.durableState)
        } finally { scope.cancel() }
    }
    @Test fun canceledWaiterDoesNotCancelAnAcceptedCriticalWrite() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
            val actor = OrderedStatePersistence(0, scope, { entered.complete(Unit); release.await() }, Dispatchers.Unconfined)
            var transport = false
            val waiter = async { actor.durable({ it + 1 }); withContext(Dispatchers.Default) { transport = true } }
            entered.await(); waiter.cancel(); release.complete(Unit); waiter.join()
            until { actor.durableState == 1 && actor.state.value == 1 }
            assertFalse(transport); assertTrue(waiter.isCancelled)
        } finally { scope.cancel() }
    }
    @Test fun typingDuringSendBarrierKeepsCanonicalClearedGenerationWithoutSentFiles() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val original = Draft(target, "First message", attachments = listOf(file))
            val entered = CompletableDeferred<MessageState>(); val release = CompletableDeferred<Unit>()
            val writes = Collections.synchronizedList(mutableListOf<MessageState>())
            val actor = OrderedStatePersistence(MessageState(listOf(original)), scope, { state ->
                writes.add(state)
                if (writes.size == 1) { entered.complete(state); release.await() }
            }, Dispatchers.Unconfined, debounceMillis = 1000)
            val barrier = async { actor.durable({ it.enqueue(original, request, 1.0) }, { state, written ->
                MessageChanges.enqueueAcknowledged(state, original, written, request)
            }) }
            val accepted = entered.await()
            val cleared = accepted.drafts.single()
            assertNotEquals(original.generation, cleared.generation)
            actor.edit { MessageChanges.text(it, original.copy(text = "Next composition", revision = "next-edit")) }
            release.complete(Unit); barrier.await()
            val persisted = actor.flush()
            until { actor.state.value.drafts.single().generation == cleared.generation }
            listOf(persisted, actor.state.value).forEach { state ->
                assertEquals("Next composition", state.drafts.single().text)
                assertEquals(cleared.generation, state.drafts.single().generation)
                assertTrue(state.drafts.single().attachments.isEmpty())
                assertEquals("First message", state.outgoing.single().text)
                assertEquals(listOf(file), state.outgoing.single().attachments)
                assertEquals(request, state.outgoing.single().requestId)
            }
        } finally { scope.cancel() }
    }
    @Test fun lateTypingCannotRestoreAnEditingQuestionOverItsLockedDelivery() {
        val question = Draft(target, "Answer", questionId = "question", questionHash = "hash")
        val locked = question.begin()
        val state = MessageState(listOf(locked))
        val result = MessageChanges.text(state, question.copy(text = "Late edit", revision = "late"))
        assertEquals(locked, result.drafts.single())
    }
    @Test fun acknowledgedSendUsesWrittenGenerationAndPreservesNewFileOwnership() {
        val original = Draft(target, "First", attachments = listOf(file))
        val initial = MessageState(listOf(original))
        val written = initial.enqueue(original, request, 1.0)
        val newFile = file.copy(id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")
        val edited = original.copy(text = "Next", attachments = listOf(file, newFile), revision = "next")
        val result = MessageChanges.enqueueAcknowledged(initial.copy(drafts = listOf(edited)), original, written, request)
        assertEquals(written.drafts.single().generation, result.drafts.single().generation)
        assertEquals("Next", result.drafts.single().text)
        assertEquals(listOf(newFile), result.drafts.single().attachments)
        assertEquals(written.outgoing, result.outgoing)
    }
    @Test fun cancelDuringAcceptedSelectionWriteCannotDeleteItsCommittedAttachmentOwnership() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val origin = Draft(target, "Keep latest text")
            val initial = MessageState(listOf(origin), pickerDraft = origin, pickerId = "selection")
            val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
            var first = true
            val actor = OrderedStatePersistence(initial, scope, {
                if (first) { first = false; entered.complete(Unit); release.await() }
            }, Dispatchers.Unconfined, debounceMillis = 1000)
            var cleanupProtected: Set<String> = emptySet()
            val importer = async {
                try {
                    actor.durable({ state -> state.selectionResult(origin, listOf(file)) }, { state, written ->
                        MessageChanges.selectionAcknowledged(state, origin, written, setOf(file.id), "selection")
                    })
                } finally {
                    cleanupProtected = (actor.state.value.drafts + actor.durableState.drafts)
                        .flatMap { it.attachments }.map { it.id }.toSet()
                }
            }
            entered.await(); importer.cancel()
            actor.edit { AttachmentSelection.cancel(it, "selection") }
            release.complete(Unit); importer.join()
            assertTrue(file.id in cleanupProtected)
            val final = actor.flush()
            assertTrue(final.drafts.any { file in it.attachments })
            assertTrue(actor.state.value.drafts.any { file in it.attachments })
            assertNull(actor.state.value.pickerDraft)
        } finally { scope.cancel() }
    }
    @Test fun replacementDuringAcceptedSelectionWriteKeepsOriginalFilesAsDetachedRecovery() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val origin = Draft(target, "Original")
            val initial = MessageState(listOf(origin), pickerDraft = origin, pickerId = "selection")
            val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
            var first = true
            val actor = OrderedStatePersistence(initial, scope, {
                if (first) { first = false; entered.complete(Unit); release.await() }
            }, Dispatchers.Unconfined, debounceMillis = 1000)
            val importer = async { actor.durable({ it.selectionResult(origin, listOf(file)) }, { state, written ->
                MessageChanges.selectionAcknowledged(state, origin, written, setOf(file.id), "selection")
            }) }
            entered.await()
            val replacement = Draft(target, "Replacement composition")
            actor.edit { MessageChanges.draft(it, replacement) }
            release.complete(Unit); importer.await()
            val final = actor.flush()
            assertEquals(replacement, final.drafts.single { it.detachedId.isBlank() })
            assertTrue(final.drafts.any { it.detachedId.isNotBlank() && file in it.attachments })
            assertTrue(actor.state.value.drafts.any { it.detachedId.isNotBlank() && file in it.attachments })
        } finally { scope.cancel() }
    }
    @Test fun discardCapturedBeforeAcceptedSelectionCannotEraseItsUnseenCommittedFiles() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        try {
            val origin = Draft(target, "Original")
            val initial = MessageState(listOf(origin), pickerDraft = origin, pickerId = "selection")
            val entered = CompletableDeferred<Unit>(); val release = CompletableDeferred<Unit>()
            var first = true
            val actor = OrderedStatePersistence(initial, scope, {
                if (first) { first = false; entered.complete(Unit); release.await() }
            }, Dispatchers.Unconfined, debounceMillis = 1000)
            val importer = async { actor.durable({ it.selectionResult(origin, listOf(file)) }, { state, written ->
                MessageChanges.selectionAcknowledged(state, origin, written, setOf(file.id), "selection")
            }) }
            entered.await()
            actor.edit { MessageChanges.discard(it, origin) }
            release.complete(Unit); importer.await()
            val final = actor.flush()
            assertTrue(final.drafts.any { file in it.attachments })
            assertTrue(actor.state.value.drafts.any { file in it.attachments })
            assertTrue(final.drafts.none { it.key == origin.key && it.revision == origin.revision })
        } finally { scope.cancel() }
    }
}
