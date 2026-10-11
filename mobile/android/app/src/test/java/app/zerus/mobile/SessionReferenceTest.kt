package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class SessionReferenceTest {
    private val target=Target("11111111-1111-4111-8111-111111111111","claude/project/a & б","run","parent")
    @Test fun displayIdentitiesDistinguishConversationRunAndChild() {
        assertEquals(listOf("Session ID: parent"), SessionReference.identifiers(target))
        assertEquals(listOf("Run ID: run"), SessionReference.identifiers(target.copy(conversation="")))
        assertEquals(listOf("Session ID: native-child", "Agent ID: child"), SessionReference.identifiers(target.copy(conversation="native-child", agentId="child", parentConversation="parent")))
        assertEquals(listOf("Agent ID: child"), SessionReference.identifiers(target.copy(conversation="",agentId="child",parentConversation="parent")))
        assertTrue(SessionReference.identifiers(target.copy(conversation="",run="")).isEmpty())
    }
    @Test fun referencePreservesMachineConversationAndEscapedName() {
        val uri=SessionReference.create(target,"fixture")!!
        assertTrue(uri.startsWith("zerus://session?machine=${target.computerId}&host=fixture&"))
        assertTrue(uri.contains("name=claude%2Fproject%2Fa%20%26%20%D0%B1"))
        assertTrue(uri.contains("conversation=parent&run=run"))
    }
    @Test fun childReferencesUseParentConversationAndExactArchive() {
        val uri=SessionReference.create(target.copy(conversation="child",parentConversation="parent",agentId="external:codex:child",archiveId="archive"),"fixture")!!
        assertTrue(uri.contains("conversation=parent"));assertTrue(uri.contains("agent=external%3Acodex%3Achild"));assertTrue(uri.contains("archive=archive"))
    }
    @Test fun missingOrInvalidIdentityCannotBecomeAnAmbiguousReference() {
        assertNull(SessionReference.create(target.copy(computerId="demo"),"fixture"))
        assertNull(SessionReference.create(target,"-option"))
        assertNull(SessionReference.create(target.copy(conversation="",run=""),"fixture"))
        assertNull(SessionReference.create(target.copy(agentId="child",parentConversation=""),"fixture"))
        assertNull(SessionReference.create(target.copy(session="claude/project/\n"),"fixture"))
    }
}
