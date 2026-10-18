package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/**
 * Nonselected shared-read prefix regression, starting from ff1c75da.
 * This exercises the actual head/adoption/reducer policies. ViewModel evidence
 * assembly is mirrored below, so this is not an actual-ViewModel or device claim.
 * No transport or read-mark send is involved.
 */
class SharedReadNonselectedHeadContractTest {
    private val target = Target("fixture-computer", "codex/fixture/nonselected", "fixture-run", "fixture-conversation", "fixture-workspace")
    private val other = target.copy(session = "codex/fixture/other", conversation = "other-conversation")
    private val epoch = "fixture-epoch"
    private fun row(reply: String, readReply: String = reply): Session {
        val raw = JSONObject().put("run_id", target.run).put("conversation_id", target.conversation)
            .put("reply_id", reply).put("reply_at", if (reply == "R1") 1.0 else 2.0)
            .put("last_event_at", if (reply == "R1") 1.0 else 2.0).put("phase", "idle")
            .put("read", JSONObject().put("conversation_id", target.conversation).put("reply_id", readReply))
        return Session(target, "Fixture", "codex", "idle", "fixture", reply, 0, raw)
    }
    private fun page(sequence: Long, complete: Boolean = true): HistoryPage {
        val event = Event("canonical-R$sequence", "AgentMessage", "fixture reply $sequence", "",
            nativeType = "AgentMessage", historyId = "native-R$sequence", historyEpoch = epoch,
            historyCursor = "fixture-cursor-$sequence", incomingSeq = sequence)
        return HistoryPage(target, epoch, listOf(event), null, null, false, false, sequence, sequence,
            complete, !complete, !complete, JSONObject())
    }
    private fun before(): ConversationReadState {
        val initial = ConversationReadState(listOf(ConversationReadRecord(target, epoch = epoch,
            readThrough = 1, headIncoming = 1, headTotal = 1, headComplete = true,
            established = true, reviewLater = true)))
        // Same epoch, genuinely newer canonical incoming row; the new reply was never viewed.
        return ConversationReadHead.observe(initial, page(2))
    }

    /**
     * Mirrors ff1c VM captureReadAttention + readAllEvidence + adoptSharedReads for
     * one connected, online, history-capable target and no retained/loaded rows.
     * currentHead is membership in currentHistoryHeads, not a new policy.
     * observedHint is the candidate hint stored by watchHistoryHeads at VM line 468.
     */
    private fun adopt(
        state: ConversationReadState,
        fleet: Session,
        observedHint: String? = ReadAllHeadCandidates.hint(fleet),
        currentHead: Boolean = true,
        selectedTarget: Target? = other,
        selectedRaw: JSONObject? = null,
        selectedVerified: Boolean = false,
    ): ConversationReadState {
        val record = state.record(target)
        // VM unreadCount: only a current complete head supplies a count.
        val unread = if (record?.headComplete == true && currentHead)
            record.headIncoming?.let { (it - record.readThrough).coerceIn(0, Int.MAX_VALUE.toLong()).toInt() }
        else null
        if (!SharedReadPolicies.canAdoptReply(fleet, selectedTarget, selectedRaw, selectedVerified) || (unread ?: 0) <= 0) return state
        val raw = if (selectedTarget == target) selectedRaw else fleet.raw
        val captured = ReadAttentionEvidence(target, HistoryHeadHints.readFingerprint(target, raw, record),
            record?.epoch.orEmpty(), record?.headIncoming, record?.headComplete == true && currentHead,
            emptySet(), null)
        val currentHint = ReadAllHeadCandidates.hint(fleet)
        val hintMatches = observedHint == currentHint
        // historyNetworkReason is blank and machine is online for this fixture.
        val evidence = captured.copy(
            fingerprint = captured.fingerprint + JSONArray(listOf(currentHint, observedHint)).toString(),
            authoritative = captured.authoritative && hintMatches,
        )
        val captures = if (evidence.authoritative) listOf(ReadAllCapture(target, evidence)) else emptyList()
        return ReadAllPolicies.apply(state, captures, captures).state
    }

    @Test fun nonselectedNewCanonicalHeadMustNotBorrowAnOlderFleetReplyMark() {
        val state = before()
        val fleetR1 = row("R1")
        assertTrue(SharedReadPolicies.replyRead(fleetR1))
        assertEquals(2L, state.record(target)!!.headIncoming)
        assertEquals(1, ConversationReadPolicies.lowerBound(state.record(target), null))
        // The head request began from fleet R1, then returned head2; watchHistoryHeads
        // stores its captured candidate H(R1) even though the fleet remains R1.
        val after = adopt(state, fleetR1)
        assertEquals("A shared R1 mark must not consume unseen canonical R2 while another conversation is selected", 1L,
            after.record(target)!!.readThrough)
        assertTrue(after.record(target)!!.reviewLater)
    }

    @Test fun navigatingAwayMustNotGrantAnOlderFleetMarkNewReadAuthority() {
        val state = before()
        val fleetR1 = row("R1")
        val selectedR2 = row("R2", "R1")
        val guarded = adopt(state, fleetR1, selectedTarget = target, selectedRaw = selectedR2.raw, selectedVerified = true)
        assertEquals("The ff1c selected guard remains effective", state, guarded)
        // VM back() clears selected/activity but retains head and hint maps.
        val after = adopt(guarded, fleetR1, selectedTarget = null)
        assertEquals("Navigation cannot convert the same stale R1 mark into proof that R2 was read", 1L,
            after.record(target)!!.readThrough)
        assertTrue(after.record(target)!!.reviewLater)
    }

    @Test fun exactVerifiedSelectedReplyControlStillAdopts() {
        val selected = row("R2")
        val after = adopt(before(), selected, selectedTarget = target, selectedRaw = selected.raw, selectedVerified = true)
        assertEquals(2L, after.record(target)!!.readThrough)
        assertFalse(after.record(target)!!.reviewLater)
        assertEquals(0, ConversationReadPolicies.lowerBound(after.record(target), null))
    }

    @Test fun matchingNonselectedReplyAndHintsStillLackIndependentPrefixProof() {
        val state = before()
        assertEquals(state, adopt(state, row("R2")))
    }

    @Test fun missingOrStaleHintDoesNotAdopt() {
        val state = before()
        val currentR2 = row("R2")
        assertEquals(state, adopt(state, currentR2, observedHint = null, selectedTarget = target, selectedRaw = currentR2.raw, selectedVerified = true))
        assertEquals(state, adopt(state, currentR2, observedHint = ReadAllHeadCandidates.hint(row("R1")), selectedTarget = target, selectedRaw = currentR2.raw, selectedVerified = true))
    }

    @Test fun missingIncompleteOrNoncurrentHeadDoesNotAdopt() {
        val state = before()
        val currentR2 = row("R2")
        assertEquals(state, adopt(state, currentR2, currentHead = false, selectedTarget = target, selectedRaw = currentR2.raw, selectedVerified = true))
        val incomplete = state.copy(records = listOf(state.record(target)!!.copy(headComplete = false)))
        assertEquals(incomplete, adopt(incomplete, currentR2, selectedTarget = target, selectedRaw = currentR2.raw, selectedVerified = true))
        val missing = state.copy(records = listOf(state.record(target)!!.copy(headIncoming = null)))
        assertEquals(missing, adopt(missing, currentR2, selectedTarget = target, selectedRaw = currentR2.raw, selectedVerified = true))
    }

    @Test fun markForAnotherReplyIsNotAdoptedDespiteMatchingHintAndHead() {
        val state = before()
        assertEquals(state, adopt(state, row("R2", "R1")))
    }
}
