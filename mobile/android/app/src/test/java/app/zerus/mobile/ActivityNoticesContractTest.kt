package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** activity-notice-contract.json, shared with the hgs journal, desktop and iOS. */
class ActivityNoticesContractTest {
    private val cases = JSONObject(checkNotNull(javaClass.getResourceAsStream("/activity-notice-contract.json"))
        .bufferedReader().use { it.readText() }).getJSONArray("cases").objects()

    private fun parse(row: JSONObject) = NativeParser.events(JSONObject().put("events", JSONArray().put(row))).single()

    @Test fun nativePromptsArePresentedAsTheSharedContractSays() {
        assertTrue(cases.isNotEmpty())
        cases.forEachIndexed { index, case ->
            val name = case.getString("name")
            val event = parse(JSONObject(case.getJSONObject("event").toString()).put("seq", index + 1).put("at", 1791018000 + index))
            val shown = case.getJSONObject("presentation")
            if (shown.getString("kind") == "notice") {
                assertEquals(name, ActivityNotices.ROLE, event.role)
                assertEquals(name, shown.getString("label"), ActivityNotices.label(event.origin, event.sender))
                // Nobody sent it from this phone: no delivery state, no outgoing identity, no reply.
                assertEquals(name, "", event.delivery)
                assertEquals(name, "", event.requestId)
                assertFalse(name, QuestionReplyPresentation.isOwnRole(event.role))
                assertFalse(name, ConversationReadPolicies.incoming(event))
            } else assertEquals(name, "You", event.role)
            assertEquals(name, shown.getString("body"), event.text)
            assertEquals(name, shown.getString("report"), event.report)
            assertFalse(name, (ActivityNotices.label(event.origin, event.sender) + event.text).contains("verified", ignoreCase = true))
            // The private display cache keeps the presentation.
            assertEquals(name, event, EventHistoryCodec.decode(EventHistoryCodec.encode(listOf(event))).single())
        }
    }

    @Test fun noticesKeepTheIdentitiesOfTheirNativeRows() {
        val peer = cases.first { it.getJSONObject("event").optString("origin") == "peer_message" }.getJSONObject("event")
        val marked = parse(JSONObject(peer.toString()).put("seq", 7).put("at", 1))
        val unmarked = parse(JSONObject(peer.toString()).put("seq", 7).put("at", 1).apply { remove("origin") })
        assertEquals("You", unmarked.role)
        assertEquals(unmarked.id, marked.id)
        assertEquals(unmarked.originalId, marked.originalId)
    }

    @Test fun searchNamesTheNoticeLikeItsCard() {
        val case = cases.first { it.getJSONObject("event").optString("origin") == "peer_message" }
        val event = parse(JSONObject(case.getJSONObject("event").toString()).put("seq", 1).put("at", 1))
        assertEquals(case.getJSONObject("presentation").getString("label"), ConversationSearch.prepare(listOf(event)).single().role)
    }

    @Test fun anIdlessNoticeReplacesItsLegacyCachedRowWithoutChangingIdentity() {
        val row = JSONObject().put("type", "UserPromptSubmit").put("origin", "peer_message")
            .put("sender", "x/y").put("detail", "Message from x/y").put("report", "The full peer message.").put("at", 1)
        // No seq, source_offset, message_id, original_id or history_id: exercise the fallback digest.
        val legacy = parse(JSONObject(row.toString()).apply { remove("origin") })
        assertEquals("You", legacy.role)
        assertEquals("Message from x/y", legacy.text)
        val cache = EventHistoryCodec.encode(listOf(legacy))
        cache.getJSONObject(0).apply { remove("origin"); remove("sender"); remove("report") }
        val restored = EventHistoryCodec.decode(cache)
        val notice = parse(row)
        assertEquals(legacy.id, notice.id)
        assertEquals(legacy.originalId, notice.originalId)
        val target = Target("computer", "codex/project/session", "run", "conversation")
        val merged = RollingConversationHistory.merge(target, target, restored, listOf(notice)).events.single()
        assertEquals(notice, merged)
        assertEquals(ActivityNotices.ROLE, merged.role)
        assertEquals("The full peer message.", merged.text)
        assertEquals("", merged.requestId)
        assertEquals("", merged.delivery)
        assertFalse(ConversationReadPolicies.incoming(merged))
    }

    @Test fun cachedNoticesCannotBecomeOutgoingReceipts() {
        val notice = Event("history:1", "Notice", "Peer text", "", requestId = "other-request",
            source = "hgs_delivery", delivery = "recorded", outgoingId = "other-request", submittedText = "Peer text",
            originalId = "journal:original", historyId = "1", historyCursor = "cursor", historyEpoch = "epoch",
            origin = "peer_message", sender = "claude/project/review")
        assertEquals("", notice.receiptRequestId)
        val stored = EventHistoryCodec.row(notice)
        assertEquals("", stored.getString("request_id"))
        // Also refuse stale outgoing fields in a previously written display cache.
        stored.put("request_id", "other-request").put("outgoing_id", "other-request").put("delivery", "recorded")
            .put("submitted_text", "Peer text")
        val decoded = EventHistoryCodec.decode(JSONArray().put(stored)).single()
        assertEquals(ActivityNotices.cached(notice), decoded)
        assertEquals("", decoded.requestId)
        assertEquals("", decoded.receiptRequestId)
        assertEquals("", decoded.outgoingId)
        assertEquals("", decoded.delivery)
        assertEquals("history:1", decoded.id)
        assertEquals("cursor", decoded.historyCursor)
    }

    @Test fun originCannotTurnAnAgentReplyOrAnswerIntoAPeerPrompt() {
        for ((type, role) in listOf("AgentMessage" to "AgentMessage", "QuestionAnswered" to "You (answer)")) {
            val event = parse(JSONObject().put("type", type).put("origin", "peer_message").put("sender", "x/y")
                .put("detail", "Native text").put("report", "Not the native text").put("message_id", "other-request"))
            assertEquals(role, event.role)
            assertEquals("Native text", event.text)
            assertEquals("", event.origin)
            assertEquals("", event.requestId)
        }
        assertEquals(" \n ", ActivityNotices.body("peer_message", "Summary", " \n "))
    }
}
