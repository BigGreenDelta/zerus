package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class AttachmentSelectionTest {
    private val target = Target("fixture-computer", "codex/fixture/files", "fixture-run", "fixture-conversation", "fixture-workspace")
    private val file = Attachment("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "retained.txt", "text/plain", 7, "a".repeat(64))
    private val origin = Draft(target, "Original text", attachments = listOf(file))
    private val unrelated = Draft(target.copy(session = "codex/fixture/other"), "Another saved draft")
    private val pending = MessageState(drafts = listOf(origin.copy(text = "Latest text", revision = "newer"), unrelated),
        outgoing = listOf(OutgoingMessage("11111111-1111-4111-8111-111111111111", unrelated.target, "Pending delivery",
            listOf(file), 1.0, status = "uncertain")), pickerDraft = origin, pickerId = "selection")

    @Test fun coldRestartRetiresOnlyTheOrphanedMarkerAndPreservesNewestDraftAndOutbox() {
        val restored = MessageCodec.state(MessageCodec.stateJson(pending))
        val recovered = AttachmentSelection.recover(restored, "selection", "", filesBusy = false)
        assertEquals(restored.copy(pickerDraft = null, pickerId = ""), recovered)
        assertEquals("Latest text", recovered.drafts.first().text)
        assertEquals(listOf(file), recovered.drafts.first().attachments)
        assertEquals("uncertain", recovered.outgoing.single().status)
    }

    @Test fun exactRestoredActivityLaunchRetainsItsOriginalResultDestination() {
        assertSame(pending, AttachmentSelection.recover(pending, "selection", "selection", false))
        assertEquals(pending.copy(pickerDraft = null, pickerId = ""),
            AttachmentSelection.recover(pending, "selection", "another-launch", false))
    }

    @Test fun preparingOrImportingFilesIsNeverRetiredByActivityRecovery() {
        assertSame(pending, AttachmentSelection.recover(pending, "selection", "", filesBusy = true))
        assertSame(pending, AttachmentSelection.recover(pending, "selection", "selection", filesBusy = true))
    }

    @Test fun delayedRecoveryAndCancelCannotClearANewerSelectionEvenForTheSameDraft() {
        val newer = pending.copy(pickerId = "new-selection")
        assertSame(newer, AttachmentSelection.recover(newer, "selection", "", false))
        assertSame(newer, AttachmentSelection.cancel(newer, "selection"))
        assertEquals(newer.copy(pickerDraft = null, pickerId = ""), AttachmentSelection.cancel(newer, "new-selection"))
    }

    @Test fun legacyMissingSelectionIdIsRecoverableWithoutLosingItsDraft() {
        val legacy = pending.copy(pickerId = "")
        assertEquals(legacy.copy(pickerDraft = null), AttachmentSelection.recover(legacy, "", "", false))
        assertSame(legacy, AttachmentSelection.recover(legacy, "", "", true))
    }

    @Test fun missingOriginDoesNotGrantAuthorityToARestoredLaunch() {
        val incomplete = pending.copy(pickerDraft = null)
        assertEquals(incomplete.copy(pickerId = ""), AttachmentSelection.recover(incomplete, "selection", "selection", false))
        val settled = pending.copy(pickerDraft = null, pickerId = "")
        assertSame(settled, AttachmentSelection.cancel(settled, ""))
    }

    @Test fun recoveredSelectionUnblocksUpdatesWhileActiveImportStillBlocksAndOutboxIsPreserved() {
        val clean = pending.copy(outgoing = emptyList())
        fun reason(state: MessageState, busy: Boolean = false) = UpdateInstallGuard.reason(state, true, busy, false, false, false)
        assertEquals("Finish file selection before installing an update.", reason(clean))
        assertEquals("", reason(AttachmentSelection.recover(clean, "selection", "", false)))
        assertEquals("Finish file selection before installing an update.", reason(clean, true))
        // Existing uncertain messages have their own recovery policy; this operation never edits them.
        assertEquals(pending.outgoing, AttachmentSelection.cancel(pending, "selection").outgoing)
    }
}
