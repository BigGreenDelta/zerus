package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionActionsMenuTest {
    private val target = Target("00000000-0000-4000-8000-000000000001","codex/project/review","run","conversation","workspace")
    private val supported = SessionActionPolicies.lifecycle + "terminal_snapshot" + SessionProjectProtocol.operations
    private fun row(vararg allowed: String) = JSONObject().put("allowed_actions",JSONArray(allowed.toList()))
        .put("action_reasons",JSONObject().put("pause","Wait for a confirmed idle live session before pausing"))
    private fun facts(state: String = "running", raw: JSONObject = row("pause","rename","fork","terminate","forget"), online: Boolean = true,
        read: SessionMenuRead = SessionMenuRead(), attention: Boolean = false, t: Target = target) =
        SessionMenuFacts(t,raw,state,t.archiveId.isNotBlank(),online,supported,needsAttention = attention,read = read,reference = "zerus://session?x",folder = "/work/project")
    private fun items(f: SessionMenuFacts) = SessionActionsMenu.items(f).flatten()
    private fun item(f: SessionMenuFacts, id: String) = items(f).single { it.id == id }

    @Test fun liveRunningSessionMirrorsDesktopOrder() {
        assertEquals(listOf("terminal","review_later","session_notifications","project_assign","pause","rename","fork","copy_name","copy_reference","copy_folder","terminate"),items(facts()).map { it.id })
        assertTrue(items(facts()).filterNot { it.id==SessionNotificationPreferences.operation }.all { it.enabled })
        assertFalse(item(facts(),SessionNotificationPreferences.operation).enabled)
    }
    @Test fun savedSessionOffersResumeArchiveAndForgetButNotTerminate() {
        val saved = facts("paused",row("resume","archive","rename","fork","forget"))
        assertEquals(listOf("terminal","review_later","session_notifications","project_assign","resume","rename","fork","archive","copy_name","copy_reference","copy_folder","forget"),items(saved).map { it.id })
        assertEquals("Forget saved session…",item(saved,"forget").label)
        assertEquals("Move to archive…",item(saved,"archive").label)
        assertTrue(items(facts("stopped",row("resume","archive"))).any { it.id == "archive" })
    }
    @Test fun archivedVersionHasNoTerminalOrReadItemsAndCannotPause() {
        val archived = facts("archived",row("restore","rename","fork","forget"),t = target.copy(archiveId = "11111111-1111-4111-8111-111111111111"))
        assertEquals(listOf("session_notifications","restore","rename","fork","copy_name","copy_reference","copy_folder","forget"),items(archived).map { it.id })
        assertFalse(item(archived,SessionNotificationPreferences.operation).enabled)
        assertEquals("Forget archived version…",item(archived,"forget").label)
        assertEquals("This archived conversation is read-only.",SessionActionsMenu.operationReason(archived,"pause"))
        assertEquals("Select an archived version first.",SessionActionsMenu.operationReason(facts(),"restore"))
    }
    @Test fun disabledItemsExplainNativeAndPhoneGates() {
        assertEquals("Wait for a confirmed idle live session before pausing",item(facts(raw = row("rename")),"pause").reason)
        assertEquals("The machine is offline.",item(facts(online = false),"pause").reason)
        assertEquals("The machine is offline.",item(facts(online = false),"terminal").reason)
        assertEquals("This gateway or computer does not support this action.",
            item(facts().copy(supported = setOf("terminal_snapshot")),"rename").reason)
        assertEquals("Unavailable in preview.",item(facts().copy(blocked = "Unavailable in preview."),"terminate").reason)
        assertEquals("Resolve the pending delivery or action first.",item(facts().copy(busy = "Resolve the pending delivery or action first."),"fork").reason)
        // A pending delivery blocks native controls, not Terminal or copying.
        assertTrue(item(facts().copy(busy = "busy"),"terminal").enabled)
        assertTrue(SessionActionsMenu.operationReason(facts(raw = JSONObject()),"pause").isNotBlank())
        assertEquals("Session controls belong to the parent conversation.",
            SessionActionsMenu.operationReason(facts(t = target.copy(agentId = "child",parentConversation = "conversation")),"pause"))
    }
    @Test fun terminalUsesNativeTerminalSupportAndAdapterCapability() {
        val unsupported = facts(raw = row("pause").put("terminal_supported",false).put("terminal_reason","Terminal requires this exact live tracked tmux pane"))
        assertEquals("Terminal requires this exact live tracked tmux pane",item(unsupported,"terminal").reason)
        assertEquals("This native adapter does not provide remote Terminal.",item(facts().copy(supported = SessionActionPolicies.lifecycle),"terminal").reason)
        assertTrue(item(facts(raw = row().put("terminal_supported",true)),"terminal").enabled)
        assertEquals(TerminalAvailability.unknownComputer,
            item(facts().copy(terminalAccessReason = TerminalAvailability.unknownComputer),"terminal").reason)
        assertEquals("This gateway does not support remote Terminal. Update the gateway.",
            item(facts().copy(terminalAccessReason = "This gateway does not support remote Terminal. Update the gateway."),"terminal").reason)
    }
    @Test fun projectGroupingRequiresExactCurrentCapabilitiesAndLiveParent() {
        assertTrue(item(facts(),SessionProjectProtocol.assign).enabled)
        assertFalse(item(facts().copy(supported=supported-SessionProjectProtocol.read),SessionProjectProtocol.assign).enabled)
        assertEquals("Catalog identity changed",item(facts().copy(projectReason="Catalog identity changed"),SessionProjectProtocol.assign).reason)
        assertEquals("The machine is offline.",item(facts(online=false),SessionProjectProtocol.assign).reason)
        assertFalse(items(facts(t=target.copy(agentId="child"))).any { it.id==SessionProjectProtocol.assign })
        assertFalse(items(facts(t=target.copy(archiveId="archive"))).any { it.id==SessionProjectProtocol.assign })
        assertTrue(item(facts(),SessionProjectProtocol.assign).hint.contains("working folder"))
    }
    @Test fun copyItemsNeedReportedValues() {
        val missing = facts().copy(reference = null,folder = "")
        assertFalse(item(missing,"copy_reference").enabled)
        assertFalse(item(missing,"copy_folder").enabled)
        assertTrue(item(missing,"copy_name").enabled)
    }
    @Test fun readItemMarksOnlyWithReadAllEvidenceOtherwiseExplains() {
        fun read(read: SessionMenuRead, attention: Boolean = true) = SessionActionsMenu.readItem(facts(read = read,attention = attention))
        assertEquals(SessionMenuItem("review_later","Review later",hint = "Keep a reminder on this phone to review this session later."),read(SessionMenuRead(),false))
        assertEquals("Read state is still loading on this phone.",read(SessionMenuRead("Read state is still loading on this phone."),false).reason)
        val complete = read(SessionMenuRead(canMark = true,replies = true))
        assertEquals("mark_read" to "Mark as read",complete.id to complete.label);assertTrue(complete.enabled)
        val partial = read(SessionMenuRead(canMark = true,partial = true,replies = true))
        assertEquals("Mark loaded replies read",partial.label);assertTrue(partial.enabled);assertTrue(partial.hint.isNotBlank())
        // A phone-local reminder without usable reply evidence is cleared alone; replies stay unread.
        val reminder = read(SessionMenuRead(reminder = true,replies = true))
        assertEquals("clear_reminder",reminder.id);assertTrue(reminder.enabled);assertTrue(reminder.hint.contains("stay unread"))
        val unknown = read(SessionMenuRead(replies = true))
        assertEquals("mark_read",unknown.id);assertFalse(unknown.enabled);assertTrue(unknown.reason.startsWith("Needs verified history"))
        assertTrue(read(SessionMenuRead(replies = true,checking = true)).reason.startsWith("Waiting for verified history"))
        val requests = read(SessionMenuRead())
        assertFalse(requests.enabled);assertTrue(requests.reason.contains("covers replies only"))
        assertFalse(read(SessionMenuRead("Unavailable in preview.",canMark = true)).enabled)
    }
}
