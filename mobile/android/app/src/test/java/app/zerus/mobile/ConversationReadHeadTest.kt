package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class ConversationReadHeadTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val native = Event("provider:reply", "AgentMessage", "Latest reply", "", 1.0,
        source = "codex_transcript", nativeType = "AgentMessage")
    private fun canonical(sequence: Long = 74) = native.copy(id = "history:reply", historyId = "reply",
        historyCursor = "cursor:reply", historyEpoch = "epoch", incomingSeq = sequence,
        originalId = native.id, originalIds = listOf(native.id))
    private fun page(vararg rows: Event, total: Long = 74, target: Target = this.target) =
        HistoryPage(target, "epoch", rows.toList(), null, null, false, false, total, total, true, false, false, JSONObject())

    @Test fun canonicalHeadReconcilesTheExactNativeReplyAlreadyReadOnScreen() {
        val read = ConversationReadState().head(target, "epoch", 63, 63, true).visible(target, listOf(native))
        assertEquals(63, ConversationReadPolicies.lowerBound(read.record(target), listOf(native)))
        val reconciled = ConversationReadHead.observe(read, page(canonical()))
        assertEquals(74L, reconciled.record(target)!!.readThrough)
        assertEquals(0, ConversationReadPolicies.lowerBound(reconciled.record(target), listOf(canonical())))
        assertEquals(reconciled, ConversationReadPolicies.decode(ConversationReadPolicies.encode(reconciled)))
    }

    @Test fun laterRepliesAndUnseenHeadsRemainUnread() {
        val read = ConversationReadState().visible(target, listOf(native))
        val reconciled = ConversationReadHead.observe(read, page(canonical(), total = 75))
        assertEquals(1, ConversationReadPolicies.lowerBound(reconciled.record(target), emptyList()))
        assertEquals(74, ConversationReadPolicies.lowerBound(
            ConversationReadHead.observe(ConversationReadState(), page(canonical())).record(target), emptyList()))
    }

    @Test fun identicalTextOrAnAmbiguousAliasNeverProvesReading() {
        val read = ConversationReadState().visible(target, listOf(native))
        for (rows in listOf(listOf(canonical().copy(originalId = "", originalIds = emptyList())),
            listOf(canonical(), canonical().copy(id = "history:other", historyId = "other", historyCursor = "cursor:other")))) {
            assertEquals(0L, ConversationReadHead.observe(read, page(*rows.toTypedArray())).record(target)!!.readThrough)
        }
    }

    @Test fun foreignScopeOrNoncanonicalMetadataCannotTransferAReadPosition() {
        val read = ConversationReadState().visible(target, listOf(native))
        for (foreign in listOf(target.copy(connectionId = "other"), target.copy(conversation = "other"),
            target.copy(agentId = "child", parentConversation = target.conversation))) {
            assertEquals(0L, ConversationReadHead.observe(read, page(canonical(), target = foreign)).record(foreign)!!.readThrough)
        }
        for (row in listOf(canonical().copy(historyEpoch = "other"), canonical().copy(historyCursor = ""))) {
            assertEquals(0L, ConversationReadHead.observe(read, page(row)).record(target)!!.readThrough)
        }
    }
}
