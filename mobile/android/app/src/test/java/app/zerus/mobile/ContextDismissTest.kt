package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ContextDismissTest {
    private val target = Target("computer", "claude/project/session", "run", "conversation", "workspace")
    private val features = setOf(ContextPolicies.DISMISS_FEATURE)
    private val operation = ContextOperation("clear-id", target, "clear_context", status = "completed", createdAt = 1)
    private val file = Attachment("10000000-0000-4000-8000-000000000001", "synthetic.txt", "text/plain", 4, "0".repeat(64))
    private val draft = Draft(target, "Keep this unsent text", updatedAt = 1, attachments = listOf(file), revision = "revision", generation = "generation")

    private fun chooser() = JSONObject().put("name", target.session).put("run_id", target.run).put("conversation_id", target.conversation)
        .put("runtime_state", "live").put("process_state", "running").put("state", "running")
        .put("phase", "approval").put("activity", "needs_input").put("clear_context_supported", false)
        .put(ContextPolicies.DISMISS_FEATURE, true)
    private fun reason(raw: JSONObject? = chooser(), selected: Target = target, relay: Set<String> = features, machine: Set<String> = features) =
        ContextPolicies.dismissReason(selected, raw, relay, machine)
    private fun saved(value: ContextOperation = operation) = MessageCodec.stateJson(MessageState(drafts = listOf(draft), contextOperations = listOf(value)))
    private fun row(value: JSONObject) = value.getJSONArray("context_operations").getJSONObject(0)
    private fun receipt(value: ContextOperation, status: String = "confirmed") = JSONObject().put("request_id", value.requestId).put("state", "completed")
        .put("result", JSONObject().put("request_id", value.requestId).put("name", value.target.session).put("run_id", value.target.run)
            .put("conversation_id", value.target.conversation).put("status", status))

    @Test fun ordinaryClearAndCompactPayloadsKeepTheOriginalOmissionAndIdentity() {
        for (kind in listOf("clear_context", "compact_context")) {
            val current = operation.copy(operation = kind)
            val expected = target.json().put("request_id", current.requestId)
            assertEquals(expected.toString(), ContextPolicies.payload(current).toString())
            assertFalse(ContextPolicies.payload(current).has("dismiss_prompt"))
        }
    }

    @Test fun legacyStoredContextRowIsByteIdenticalWithoutANewField() {
        val expected = MessageCodec.targetJson(target).put("request", operation.requestId).put("operation", operation.operation)
            .put("status", operation.status).put("created", operation.createdAt).put("error", operation.error).put("resolved_conversation", operation.resolvedConversation)
        val encoded = saved()
        assertEquals(expected.toString(), row(encoded).toString())
        assertFalse(row(encoded).has("dismiss_prompt"))
        val restored = MessageCodec.state(encoded)
        assertEquals(operation, restored.contextOperations.single())
        assertEquals(draft, restored.drafts.single())
        assertFalse(row(MessageCodec.stateJson(restored)).has("dismiss_prompt"))
    }

    @Test fun explicitFalseAndTrueKeepTheirExactTypeAcrossPayloadAndStorage() {
        for (flag in listOf(false, true)) {
            val current = operation.copy(dismissPrompt = flag)
            val payload = ContextPolicies.payload(current)
            assertTrue(payload.opt("dismiss_prompt") is Boolean)
            assertEquals(flag, payload.opt("dismiss_prompt"))
            assertEquals(current.requestId, payload.getString("request_id"))
            assertEquals(target.run, payload.getString("expected_run_id"))
            assertEquals(target.conversation, payload.getString("expected_conversation_id"))
            val encoded = saved(current)
            assertTrue(row(encoded).opt("dismiss_prompt") is Boolean)
            assertEquals(flag, row(encoded).opt("dismiss_prompt"))
            val restored = MessageCodec.state(encoded)
            assertEquals(current, restored.contextOperations.single())
            assertEquals(draft, restored.drafts.single())
        }
    }

    @Test fun compactCannotAcquireDismissMetadataInPayloadOrStoredRecords() {
        for (flag in listOf(false, true)) {
            val invalid = operation.copy(operation = "compact_context", dismissPrompt = flag)
            assertThrows(IllegalArgumentException::class.java) { ContextPolicies.payload(invalid) }
            assertThrows(IllegalArgumentException::class.java) { saved(invalid) }
            val encoded = saved(operation.copy(operation = "compact_context"))
            row(encoded).put("dismiss_prompt", flag)
            val before = encoded.toString()
            assertThrows(IllegalArgumentException::class.java) { MessageCodec.state(encoded) }
            assertEquals(before, encoded.toString())
        }
    }

    @Test fun malformedStoredDismissMetadataIsRejectedWithoutMutatingTheSourceOrDrafts() {
        for (flag in listOf(JSONObject.NULL, "true", "false", 0, 1, 1.0, JSONArray(), JSONObject())) {
            val encoded = saved()
            row(encoded).put("dismiss_prompt", flag)
            val before = encoded.toString()
            assertThrows(IllegalArgumentException::class.java) { MessageCodec.state(encoded) }
            assertEquals(before, encoded.toString())
            assertEquals(MessageCodec.draftJson(draft).toString(), encoded.getJSONArray("drafts").getJSONObject(0).toString())
        }
    }

    @Test fun waitingChooserCanUseOnlyTheExplicitNativeEligibilityFlag() {
        for (phase in listOf("approval", "input")) {
            val raw = chooser().put("phase", phase)
            assertFalse(raw.getBoolean("clear_context_supported"))
            assertEquals("", reason(raw))
        }
    }

    @Test fun eligibilityIsNotInferredFromQuestionSourceOrAgentName() {
        val raw = chooser().put("name", "another/fixture/session").put("agent", "another-agent")
            .put("pending_questions", JSONArray().put(JSONObject().put("source", "synthetic-source")))
        assertEquals("", reason(raw, target.copy(session = "another/fixture/session")))
        raw.remove(ContextPolicies.DISMISS_FEATURE)
        assertTrue(reason(raw, target.copy(session = "another/fixture/session")).isNotBlank())
    }

    @Test fun missingFalseNullAndCoercibleNativeFlagsNeverEnableDismissal() {
        for (flag in listOf(JSONObject.NULL, false, "true", "false", 0, 1, JSONArray(), JSONObject())) {
            assertTrue(reason(chooser().put(ContextPolicies.DISMISS_FEATURE, flag)).isNotBlank())
        }
        val absent = chooser().apply { remove(ContextPolicies.DISMISS_FEATURE) }
        assertTrue(reason(absent).isNotBlank())
    }

    @Test fun bothRelayAndExactMachineMustAdvertiseTheFeature() {
        assertTrue(reason(relay = emptySet()).isNotBlank())
        assertTrue(reason(machine = emptySet()).isNotBlank())
        assertTrue(reason(relay = setOf("clear_context"), machine = setOf("clear_context")).isNotBlank())
        assertEquals("", reason())
    }

    @Test fun nativeIdentityMustMatchEverySessionRunConversationField() {
        assertTrue(reason(null).isNotBlank())
        for (key in listOf("name", "run_id", "conversation_id")) {
            assertTrue(reason(chooser().put(key, "different")).isNotBlank())
            assertTrue(reason(chooser().apply { remove(key) }).isNotBlank())
        }
        assertTrue(reason(chooser(), target.copy(session = "different")).isNotBlank())
        assertTrue(reason(chooser(), target.copy(run = "different")).isNotBlank())
        assertTrue(reason(chooser(), target.copy(conversation = "different")).isNotBlank())
    }

    @Test fun numericNativeIdentityIsNotCoercedToText() {
        assertTrue(reason(chooser().put("name", 7), target.copy(session = "7")).isNotBlank())
        assertTrue(reason(chooser().put("run_id", 7), target.copy(run = "7")).isNotBlank())
        assertTrue(reason(chooser().put("conversation_id", 7), target.copy(conversation = "7")).isNotBlank())
    }

    @Test fun archivedOrChildTargetsAndNativeFramesCannotEnableDismissal() {
        for (selected in listOf(target.copy(archiveId = "archive"), target.copy(agentId = "child", parentConversation = "parent"),
            target.copy(parentConversation = "parent"))) assertTrue(reason(selected = selected).isNotBlank())
        for ((key, value) in listOf("state" to "archived", "archive_id" to "archive", "agent_id" to "child", "parent_conversation_id" to "parent"))
            assertTrue(reason(chooser().put(key, value)).isNotBlank())
    }

    @Test fun missingTargetPinsAndUnavailableNativeProcessesCannotEnableDismissal() {
        for (selected in listOf(target.copy(connectionId = ""), target.copy(computerId = ""), target.copy(session = ""), target.copy(run = ""), target.copy(conversation = "")))
            assertTrue(reason(selected = selected).isNotBlank())
        for ((key, value) in listOf("runtime_state" to "offline", "process_state" to "stopped")) {
            assertTrue(reason(chooser().put(key, value)).isNotBlank())
            assertTrue(reason(chooser().apply { remove(key) }).isNotBlank())
        }
    }

    @Test fun interruptedDismissalRetainsItsOriginalIdentityAndBlocksNewMessageDelivery() {
        for (flag in listOf(false, true)) for (status in listOf("sending", "submitted")) {
            val original = operation.copy(status = status, dismissPrompt = flag)
            val restored = MessageCodec.state(saved(original))
            val pending = restored.contextOperations.single()
            assertEquals("uncertain", pending.status)
            assertTrue(pending.blocksSending)
            assertEquals(original.requestId, pending.requestId)
            assertEquals(original.target, pending.target)
            assertEquals(flag, pending.dismissPrompt)
            assertEquals(ContextPolicies.payload(original).toString(), ContextPolicies.payload(pending).toString())
            assertEquals(listOf(draft), restored.drafts)
            assertTrue(restored.outgoing.isEmpty())
            assertThrows(IllegalArgumentException::class.java) { restored.enqueue(draft, "new-message", 1.0) }
        }
    }

    @Test fun receiptUpdatesRetainDismissMetadataAndNeverInventANewIdentity() {
        for (flag in listOf(false, true)) {
            val original = operation.copy(status = "uncertain", dismissPrompt = flag)
            for (status in listOf("submitted", "confirmed")) {
                val updated = ContextPolicies.result(original, receipt(original, status))
                assertEquals(if (status == "confirmed") "completed" else "submitted", updated.status)
                assertEquals(original.requestId, updated.requestId)
                assertEquals(original.target, updated.target)
                assertEquals(flag, updated.dismissPrompt)
            }
            val wrong = receipt(original).put("request_id", "another-request")
            val uncertain = ContextPolicies.result(original, wrong)
            assertEquals("uncertain", uncertain.status)
            assertEquals(original.requestId, uncertain.requestId)
            assertEquals(original.target, uncertain.target)
            assertEquals(flag, uncertain.dismissPrompt)
        }
    }

    @Test fun restoringAPendingDismissalPreservesQuestionDraftAndUnrelatedDeliveryIdentities() {
        val question = draft.copy(text = "Question notes", questionId = "question", questionHash = "question-hash", answers = "[[\"Keep\"]]", generation = "question-generation", revision = "question-revision")
        val delivered = OutgoingMessage("recorded-message", target, "Earlier message", emptyList(), 1.0, status = "recorded")
        val ordinary = operation.copy(requestId = "ordinary-clear", status = "completed")
        val pending = operation.copy(requestId = "pending-dismiss", status = "submitted", dismissPrompt = true)
        val state = MessageState(drafts = listOf(draft, question), outgoing = listOf(delivered), contextOperations = listOf(ordinary, pending))
        val restored = MessageCodec.state(MessageCodec.stateJson(state))
        assertEquals(state.drafts, restored.drafts)
        assertEquals(state.outgoing, restored.outgoing)
        assertEquals(ordinary, restored.contextOperations.first())
        val recovered = restored.contextOperations.last()
        assertEquals("uncertain", recovered.status)
        assertEquals(pending.requestId, recovered.requestId)
        assertEquals(pending.target, recovered.target)
        assertEquals(pending.dismissPrompt, recovered.dismissPrompt)
        assertEquals(pending.createdAt, recovered.createdAt)
    }

    @Test fun whitespaceTargetArchiveAndChildMarkersCannotEnableDismissal() {
        for (marker in listOf(" ", "\t", "\n")) {
            for (selected in listOf(target.copy(archiveId = marker), target.copy(agentId = marker), target.copy(parentConversation = marker)))
                assertTrue("Nonempty target markers must refuse dismissal", reason(selected = selected).isNotBlank())
        }
    }

    @Test fun everyNonemptyNativeArchiveAndChildMarkerCannotEnableDismissal() {
        for (key in listOf("archive_id", "agent_id", "parent_conversation_id"))
            for (marker in listOf(" ", "\t", "\n", "null", "child"))
                assertTrue("$key nonempty marker must refuse dismissal", reason(chooser().put(key, marker)).isNotBlank())
    }

    @Test fun absentNullAndEmptyNativeParentMarkersRemainCompatible() {
        for (key in listOf("archive_id", "agent_id", "parent_conversation_id")) {
            assertEquals("", reason(chooser().apply { remove(key) }))
            assertEquals("", reason(chooser().put(key, JSONObject.NULL)))
            assertEquals("", reason(chooser().put(key, "")))
        }
    }

    @Test fun malformedNativeParentMarkersCannotBeCoercedToAbsent() {
        for (key in listOf("archive_id", "agent_id", "parent_conversation_id"))
            for (marker in listOf(false, true, 0, 1, 1.0, JSONObject(), JSONArray()))
                assertTrue("$key malformed marker must refuse dismissal", reason(chooser().put(key, marker)).isNotBlank())
    }
}
