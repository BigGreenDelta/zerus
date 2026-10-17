package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class OutgoingReceiptProbeTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val message = OutgoingMessage("request", target, "Original message", emptyList(), 1.0, status = "uncertain")
    @Test fun samePresentationOnlyChecksOnceButReopeningOrANewRequestCanCheck() {
        val probe = OutgoingReceiptProbe()
        assertTrue(probe.claim(target, 1, message, false))
        repeat(20) { assertFalse(probe.claim(target, 1, message, false)) }
        assertTrue(probe.claim(target, 2, message, false))
        assertTrue(probe.claim(target, 2, message.copy(requestId = "new request"), false))
    }
    @Test fun otherScopesAndActiveOrSettledDeliveriesNeverClaim() {
        val probe = OutgoingReceiptProbe()
        assertFalse(probe.claim(target, 1, message, true))
        for (status in listOf("sending", "submitted", "recorded", "reviewed", "failed"))
            assertFalse(probe.claim(target, 1, message.copy(status = status), false))
        for (other in listOf(target.copy(run = "other"), target.copy(conversation = "other"),
            target.copy(connectionId = "other"), target.copy(agentId = "child")))
            assertFalse(probe.claim(other, 1, message, false))
        assertTrue(probe.claim(target, 1, message, false))
    }
}
