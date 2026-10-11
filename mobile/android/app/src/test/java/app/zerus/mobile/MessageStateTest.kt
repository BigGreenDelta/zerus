package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class MessageStateTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val request = "11111111-1111-4111-8111-111111111111"
    private val secondRequest = "22222222-2222-4222-8222-222222222222"
    private val attachment = Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "fixture.txt", "text/plain", 7, "a".repeat(64))
    private fun message(status: String = "submitted", id: String = request, destination: Target = target) =
        OutgoingMessage(id, destination, "Repeat this", listOf(attachment), 99.0, status, 100.0)
    private fun native(id: String = "row", text: String = "Repeat this", at: Double = 100.0,
                       requestId: String = "", source: String = "provider", role: String = "You") =
        Event(id, role, text, "", at, requestId, source = source)

    @Test fun enqueueAtomicallyPreservesPayloadAndStartsANewComposerGeneration() {
        val draft = Draft(target, "Send fixture", attachments = listOf(attachment))
        val unrelated = Draft(target.copy(session = "other"), "Keep me")
        val before = MessageState(listOf(draft, unrelated))
        val next = before.enqueue(draft, request, 123.5)
        assertEquals(listOf(draft, unrelated), before.drafts)
        assertTrue(before.outgoing.isEmpty())
        val outgoing = next.outgoing.single()
        assertEquals(request, outgoing.requestId)
        assertEquals(target, outgoing.target)
        assertEquals(draft.text, outgoing.text)
        assertEquals(listOf(attachment), outgoing.attachments)
        val cleared = next.drafts.single { it.target == target }
        assertEquals("", cleared.text)
        assertTrue(cleared.attachments.isEmpty())
        assertNotEquals(draft.revision, cleared.revision)
        assertNotEquals(draft.generation, cleared.generation)
        assertEquals(unrelated, next.drafts.single { it.target == unrelated.target })
    }

    @Test fun outboxSnapshotsAttachmentReferencesRatherThanSharingAMutableCallerList() {
        val selected = mutableListOf(attachment)
        val draft = Draft(target, "", attachments = selected)
        val next = MessageState(listOf(draft)).enqueue(draft, request, 100.0)
        selected.clear()
        assertEquals(listOf(attachment), next.outgoing.single().attachments)
    }

    @Test fun attachmentOnlyMessageIsValidButAnEmptyComposerIsRejected() {
        assertEquals("", MessageState().enqueue(Draft(target, "", attachments = listOf(attachment)), request, 100.0).outgoing.single().text)
        assertThrows(IllegalArgumentException::class.java) { MessageState().enqueue(Draft(target, "  "), request, 100.0) }
    }

    @Test fun sendingAndUncertainOutboxEntriesBlockNewRequestsForTheirExactLane() {
        listOf("sending", "uncertain").forEach { status ->
            val state = MessageState(outgoing = listOf(message(status)))
            assertThrows(IllegalArgumentException::class.java) { state.enqueue(Draft(target, "Next"), secondRequest, 102.0) }
            assertEquals(2, state.enqueue(Draft(target.copy(run = "another-run"), "Next"), secondRequest, 102.0).outgoing.size)
        }
    }

    @Test fun requestIdCannotBeReusedEvenAfterACompletedDelivery() {
        val state = MessageState(outgoing = listOf(message("recorded")))
        assertThrows(IllegalArgumentException::class.java) { state.enqueue(Draft(target, "Next"), request, 102.0) }
    }

    @Test fun staleComposerRevisionAndUnreviewedDraftsCannotBeEnqueued() {
        val original = Draft(target, "Old")
        val changed = original.copy(text = "New", revision = "new-revision")
        assertThrows(IllegalArgumentException::class.java) { MessageState(listOf(changed)).enqueue(original, request, 100.0) }
        listOf(original.copy(status = "uncertain"), original.copy(questionId = "question"), original.copy(detachedId = "saved")).forEach { draft ->
            assertThrows(IllegalArgumentException::class.java) { MessageState().enqueue(draft, request, 100.0) }
        }
    }

    @Test fun activeFilePickerBlocksSendingItsOriginalComposition() {
        val draft = Draft(target, "Keep composition", attachments = listOf(attachment))
        val state = MessageState(listOf(draft), pickerDraft = draft)
        assertThrows(IllegalArgumentException::class.java) { state.enqueue(draft, request, 100.0) }
        assertEquals(draft, state.pickerDraft)
    }

    @Test fun settlingAnOutgoingMessageDoesNotReplaceFutureDraftOrItsAttachments() {
        val draft = Draft(target, "First", attachments = listOf(attachment))
        val enqueued = MessageState(listOf(draft)).enqueue(draft, request, 100.0)
        val future = enqueued.drafts.single().copy(text = "Future composition", attachments = listOf(attachment))
        val state = enqueued.copy(drafts = listOf(future))
        listOf("submitted", "recorded", "failed", "uncertain").forEach { status ->
            val settled = state.update(state.outgoing.single().copy(status = status))
            assertEquals(listOf(future), settled.drafts)
            assertEquals(listOf(attachment), settled.outgoing.single().attachments)
        }
    }

    @Test fun codecRetainsMetadataAndPickerOwnershipWithoutEmbeddingDocumentBytes() {
        val draft = Draft(target, "Draft", attachments = listOf(attachment), detachedId = "saved")
        val state = MessageState(listOf(draft), listOf(message("failed")), draft)
        val json = MessageCodec.stateJson(state)
        assertFalse(json.toString().contains("data_base64"))
        assertEquals(state, MessageCodec.state(JSONObject(json.toString())))
        assertEquals(attachment, Attachment.fromJson(attachment.toJson()))
    }

    @Test fun legacyDraftAndQuestionJsonRecoverWithoutLosingTargetTextOrAnswer() {
        val legacy = MessageCodec.targetJson(target).put("text", "Legacy composition").put("updated", 123L)
        val draft = MessageCodec.draft(legacy)
        assertEquals(target, draft.target)
        assertEquals("Legacy composition", draft.text)
        assertEquals("editing", draft.status)
        assertTrue(draft.attachments.isEmpty())
        assertTrue(draft.revision.isNotBlank())
        assertTrue(draft.generation.isNotBlank())
        val answer = MessageCodec.draft(JSONObject(legacy.toString()).put("status", "submitting").put("request", request)
            .put("question", "question-id").put("hash", "question-hash").put("answers", "[{\"answer\":\"fixture\"}]"))
        assertEquals("uncertain", answer.status)
        assertEquals(request, answer.requestId)
        assertEquals("question-id", answer.questionId)
        assertEquals("question-hash", answer.questionHash)
        assertEquals("[{\"answer\":\"fixture\"}]", answer.answers)
        assertEquals(target, answer.target)
        assertEquals("Legacy composition", answer.text)
    }

    @Test fun restartRecoversSendingOutboxAsUncertainWithoutChangingItsPayload() {
        val original = message("sending")
        val recovered = MessageCodec.state(MessageCodec.stateJson(MessageState(outgoing = listOf(original)))).outgoing.single()
        assertEquals("uncertain", recovered.status)
        assertTrue(recovered.blocksSending)
        assertEquals(original.requestId, recovered.requestId)
        assertEquals(original.target, recovered.target)
        assertEquals(original.text, recovered.text)
        assertEquals(original.attachments, recovered.attachments)
        assertEquals(original.createdAt, recovered.createdAt, 0.0)
        listOf("submitted", "recorded", "failed", "reviewed").forEach { status ->
            val settled = original.copy(status = status)
            assertEquals(settled, MessageCodec.outgoing(MessageCodec.outgoingJson(settled)))
        }
    }

    @Test fun exactNativeUserDeliveryRequestWinsEvenWhenTextAndTimestampDiffer() {
        val row = native(text = "Native transformed prompt", at = 300.0, requestId = request, source = "hgs_delivery")
        val merged = ConversationMerge.merge(target, listOf(row), listOf(message("uncertain")))
        assertEquals(setOf(request), merged.recordedRequestIds)
        assertEquals(1, merged.events.size)
        assertEquals("Native transformed prompt", merged.events.single().text)
        assertEquals("recorded", merged.events.single().delivery)
        assertEquals(listOf(attachment.displayed()), merged.events.single().attachments) // Opens from the private copy.
    }

    @Test fun requestIdOnAgentOrUntrustedSourceCannotConfirmAUserDelivery() {
        val rows = listOf(native("agent", requestId = request, source = "hgs_delivery", role = "AgentMessage"),
            native("provider", requestId = request), native("other", requestId = secondRequest, source = "hgs_delivery"))
        val merged = ConversationMerge.merge(target, rows, listOf(message("uncertain")))
        assertTrue(merged.recordedRequestIds.isEmpty())
        assertEquals(1, merged.events.count { it.source == "mobile_outbox" })
    }

    @Test fun repeatedNativeTextWithinTimeWindowRemainsAmbiguous() {
        val rows = listOf(native("first", at = 99.0), native("second", at = 101.0))
        val merged = ConversationMerge.merge(target, rows, listOf(message()))
        assertTrue(merged.recordedRequestIds.isEmpty())
        assertEquals(3, merged.events.size)
        assertEquals("submitted", merged.events.single { it.source == "mobile_outbox" }.delivery)
    }

    @Test fun oneUnkeyedNativeRowCannotArbitrarilyChooseBetweenRepeatedPhoneSends() {
        val merged = ConversationMerge.merge(target, listOf(native()), listOf(message(), message(id = secondRequest).copy(submittedAt = 101.0)))
        assertTrue(merged.recordedRequestIds.isEmpty())
        assertEquals(2, merged.events.count { it.source == "mobile_outbox" })
    }

    @Test fun uniqueTextTimeFallbackRequiresPreviouslyConfirmedSubmission() {
        val row = native(at = 103.0)
        assertEquals(setOf(request), ConversationMerge.merge(target, listOf(row), listOf(message())).recordedRequestIds)
        assertTrue(ConversationMerge.merge(target, listOf(row), listOf(message("uncertain"))).recordedRequestIds.isEmpty())
        assertTrue(ConversationMerge.merge(target, listOf(row.copy(at = 111.0)), listOf(message())).recordedRequestIds.isEmpty())
    }

    @Test fun anotherWorkspaceComputerSessionRunOrConversationNeverMerges() {
        val alternatives = listOf(target.copy(connectionId = "another-workspace"), target.copy(computerId = "another-computer"),
            target.copy(session = "another-session"), target.copy(run = "another-run"), target.copy(conversation = "another-conversation"))
        alternatives.forEach { destination ->
            val row = native(requestId = request, source = "hgs_delivery")
            val merged = ConversationMerge.merge(destination, listOf(row), listOf(message()))
            assertEquals(listOf(row), merged.events)
            assertTrue(merged.recordedRequestIds.isEmpty())
        }
    }

    @Test fun firstMessageResolvedConversationUsesSameRunLaneWithoutMutatingOriginalIdentity() {
        val first = message("uncertain", destination = target.copy(conversation = ""))
        val row = native(requestId = request, source = "hgs_delivery")
        val merged = ConversationMerge.merge(target, listOf(row), listOf(first))
        assertEquals(setOf(request), merged.recordedRequestIds)
        assertEquals("", first.target.conversation)
        assertThrows(IllegalArgumentException::class.java) {
            MessageState(outgoing = listOf(first)).enqueue(Draft(target, "Another"), secondRequest, 102.0)
        }
        listOf(target.copy(run = "new-run"), target.copy(connectionId = "other"), target.copy(computerId = "other"), target.copy(session = "other")).forEach {
            assertFalse(first.belongsTo(it))
        }
    }

    @Test fun reviewedMessagesStayHiddenButRecordedRowsOutsideNativeWindowRemainConfirmed() {
        val merged = ConversationMerge.merge(target, emptyList(), listOf(message("reviewed"), message("recorded", secondRequest)))
        assertEquals("outgoing:$secondRequest", merged.events.single().id)
        assertEquals("recorded", merged.events.single().delivery)
        assertTrue(merged.recordedRequestIds.isEmpty())
    }

    @Test fun conversationResolutionMovesNextComposerWithoutChangingItsFilesOrGeneration() {
        val initial = target.copy(conversation = "")
        val nextDraft = Draft(initial, "Next composition", attachments = listOf(attachment))
        val sent = message("submitted", destination = initial)
        val before = MessageState(listOf(nextDraft), listOf(sent), nextDraft)
        val resolved = before.resolveConversation(initial, target)
        assertEquals(listOf(nextDraft.copy(target = target)), resolved.drafts)
        assertEquals(nextDraft.generation, resolved.drafts.single().generation)
        assertEquals(nextDraft.revision, resolved.drafts.single().revision)
        assertEquals(listOf(attachment), resolved.drafts.single().attachments)
        assertEquals(listOf(sent), resolved.outgoing)
        assertEquals(nextDraft, resolved.pickerDraft)
        assertEquals(initial, before.drafts.single().target)
    }

    @Test fun conversationResolutionRejectsExistingConversationAndDifferentRunOrWorkspace() {
        val draft = Draft(target, "Existing conversation", attachments = listOf(attachment))
        val state = MessageState(listOf(draft))
        assertEquals(state, state.resolveConversation(target, target.copy(conversation = "another")))
        val initial = target.copy(conversation = "")
        val first = MessageState(listOf(draft.copy(target = initial)))
        listOf(initial, target.copy(run = "another"), target.copy(connectionId = "another"),
            target.copy(computerId = "another"), target.copy(session = "another")).forEach { destination ->
            assertEquals(first, first.resolveConversation(initial, destination))
        }
    }

    @Test fun conversationResolutionNeverOverwritesOccupiedDestinationOrMigratesQuestionRecovery() {
        val initial = target.copy(conversation = "")
        val source = Draft(initial, "Source", attachments = listOf(attachment))
        listOf("editing", "uncertain").forEach { status ->
            val destination = Draft(target, "Occupied destination", status = status)
            val state = MessageState(listOf(source, destination))
            assertEquals(state, state.resolveConversation(initial, target))
        }
        listOf(source.copy(questionId = "question"), source.copy(status = "uncertain"), source.copy(detachedId = "saved")).forEach { protected ->
            val state = MessageState(listOf(protected))
            assertEquals(state, state.resolveConversation(initial, target))
        }
    }

    @Test fun pickerSelectionFollowsSameGenerationResolvedComposerAndPreservesNewestText() {
        val initial = target.copy(conversation = "")
        val origin = Draft(initial, "At picker launch", attachments = listOf(attachment))
        val added = attachment.copy(id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", name = "selected.txt")
        val migrated = MessageState(listOf(origin), pickerDraft = origin).resolveConversation(initial, target)
        val edited = migrated.drafts.single().copy(text = "Edited while picker open", revision = "newest-revision")
        val selected = migrated.copy(drafts = listOf(edited)).selectionResult(origin, listOf(added))
        val restored = selected.drafts.single()
        assertEquals(target, restored.target)
        assertEquals(edited.text, restored.text)
        assertEquals(origin.generation, restored.generation)
        assertNotEquals(edited.revision, restored.revision)
        assertEquals(listOf(attachment, added), restored.attachments)
        assertTrue(restored.detachedId.isBlank())
        assertNull(selected.pickerDraft)
        assertEquals(initial, origin.target)
    }

    @Test fun pickerSelectionPreservesReplacedComposerAndRecoversOriginalFilesAsDetachedDraft() {
        val origin = Draft(target, "Original", attachments = listOf(attachment))
        val replacement = Draft(target, "Replacement composition")
        val added = attachment.copy(id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")
        val state = MessageState(listOf(replacement), pickerDraft = origin)
        val selected = state.selectionResult(origin, listOf(added))
        assertEquals(replacement, selected.drafts.single { it.detachedId.isBlank() })
        val recovered = selected.drafts.single { it.detachedId.isNotBlank() }
        assertEquals(origin.target, recovered.target)
        assertEquals(origin.text, recovered.text)
        assertEquals(origin.generation, recovered.generation)
        assertEquals(listOf(attachment, added), recovered.attachments)
        assertNull(selected.pickerDraft)
    }

    @Test fun canceledOrDifferentPickerCannotCommitSelectedObjects() {
        val origin = Draft(target, "Original")
        listOf(MessageState(listOf(origin)), MessageState(listOf(origin), pickerDraft = origin.copy(generation = "other")),
            MessageState(listOf(origin), pickerDraft = origin.copy(target = target.copy(run = "another")))).forEach { state ->
            assertThrows(IllegalArgumentException::class.java) { state.selectionResult(origin, listOf(attachment)) }
            assertEquals(listOf(origin), state.drafts)
        }
    }

    @Test fun emptyPickerResultClearsOnlyPickerAndLeavesCompositionsUnchanged() {
        val origin = Draft(target, "Original", attachments = listOf(attachment))
        val newer = origin.copy(text = "Latest text", revision = "latest")
        val state = MessageState(listOf(newer), listOf(message("failed")), origin)
        assertEquals(state.copy(pickerDraft = null), state.selectionResult(origin, emptyList()))
    }
    @Test fun canonicalRowsKeepNativeOrderWithNonmonotonicProviderClocks() {
        val rows = listOf(native("first", "One", 300.0), native("second", "Two", 100.0), native("third", "Three", 200.0))
        val merged = ConversationMerge.merge(target, rows, listOf(message("recorded").copy(text = "Absent", attachments = emptyList(), createdAt = 150.0, submittedAt = 150.0)))
        assertEquals(listOf("first", "second", "third"), merged.events.filter { !it.id.startsWith("outgoing:") }.map { it.id })
        assertEquals(4, merged.events.size)
    }
}
