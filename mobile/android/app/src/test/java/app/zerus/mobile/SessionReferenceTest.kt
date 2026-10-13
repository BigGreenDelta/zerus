package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test
import org.json.JSONObject

class SessionReferenceTest {
    private val target=Target("11111111-1111-4111-8111-111111111111","claude/project/a & б","run","parent")
    private val machineId="22222222-2222-4222-8222-222222222222"
    private val machine=Machine(target.connectionId,target.computerId,"Work computer",true,machineId=machineId,nativeHost="fixture")
    private fun reference(target: Target = this.target, computers: List<Machine> = listOf(machine)) = SessionReference.resolve(target,computers)
    @Test fun displayIdentitiesDistinguishConversationRunAndChild() {
        assertEquals(listOf("Session ID: parent"), SessionReference.identifiers(target))
        assertEquals(listOf("Run ID: run"), SessionReference.identifiers(target.copy(conversation="")))
        assertEquals(listOf("Session ID: native-child", "Agent ID: child"), SessionReference.identifiers(target.copy(conversation="native-child", agentId="child", parentConversation="parent")))
        assertEquals(listOf("Agent ID: child"), SessionReference.identifiers(target.copy(conversation="",agentId="child",parentConversation="parent")))
        assertTrue(SessionReference.identifiers(target.copy(conversation="",run="")).isEmpty())
    }
    @Test fun referencePreservesMachineConversationAndEscapedName() {
        val uri=reference().uri!!
        assertTrue(uri.startsWith("zerus://session?machine=$machineId&host=fixture&"))
        assertTrue(uri.contains("name=claude%2Fproject%2Fa%20%26%20%D0%B1"))
        assertTrue(uri.contains("conversation=parent&run=run"))
    }
    @Test fun childReferencesUseParentConversationAndExactArchive() {
        val uri=reference(target.copy(conversation="child",parentConversation="parent",agentId="external:codex:child",archiveId="archive")).uri!!
        assertTrue(uri.contains("conversation=parent"));assertTrue(uri.contains("agent=external%3Acodex%3Achild"));assertTrue(uri.contains("archive=archive"))
    }
    @Test fun missingOrInvalidIdentityCannotBecomeAnAmbiguousReference() {
        val invalid = listOf(reference(target.copy(computerId="demo")),reference(computers=listOf(machine.copy(nativeHost="-option"))),
            reference(target.copy(conversation="",run="")),reference(target.copy(agentId="child",parentConversation="")),
            reference(target.copy(session="claude/project/\n")),reference(target.copy(session="unqualified")))
        invalid.forEach { assertNull(it.uri);assertTrue(it.reason.isNotBlank()) }
    }
    @Test fun catalogIdentityAndNativeHostSurviveDisplayNamesAndWorkingState() {
        val parsed=MachineCatalog.parse(target.connectionId,JSONObject().put("id",target.computerId).put("name","Office computer")
            .put("machine_id",machineId).put("online",true).put("snapshot",JSONObject().put("host","fixture")))
        val renamed=MachineNames.apply(parsed,listOf(MachineAlias(MachineKey(target.connectionId,target.computerId),"My renamed computer")))
        assertEquals(reference().uri,reference(computers=listOf(renamed)).uri)
        assertEquals(reference().uri,reference(computers=listOf(renamed.copy(online=false,lastKnown=true))).uri)
        val working=Session(target,"Working","Claude","busy","","",0,JSONObject().put("state","running").put("activity","busy"))
        val resolved=reference(working.target,listOf(renamed))
        val facts=SessionMenuFacts(target,working.raw,"running",false,true,emptySet(),busy="An action is pending.",
            reference=resolved.uri,referenceReason=resolved.reason)
        assertTrue(SessionActionsMenu.items(facts).flatten().single { it.id=="copy_reference" }.enabled)
    }
    @Test fun relayEnrollmentCannotSubstituteForNativeIdentityOrHost() {
        listOf("",target.computerId.uppercase().replace('1','A'),"00000000-0000-0000-0000-000000000000","relay-node").forEach { id ->
            assertNull(reference(computers=listOf(machine.copy(machineId=id))).uri)
        }
        assertNull(reference(computers=listOf(machine.copy(nativeHost="",nativeName="fixture"))).uri)
        assertNull(reference(computers=listOf(machine.copy(nativeHost="Display label"))).uri)
        val legacy=machine.copy(id="opaque-enrollment")
        assertEquals(reference().uri,reference(target.copy(computerId=legacy.id),listOf(legacy)).uri)
    }
    @Test fun onlyUnambiguousAdvertisedAliasesInTheSameWorkspaceResolve() {
        val alias=machine.copy(id="canonical-computer",serverAliases=setOf(target.computerId))
        assertEquals(reference().uri,reference(computers=listOf(alias)).uri)
        assertNull(reference(computers=listOf(alias.copy(connectionId="other-workspace"))).uri)
        assertNull(reference(computers=listOf(alias,machine)).uri)
        assertNull(reference(computers=listOf(alias.copy(serverAliases=emptySet()))).uri)
    }
    @Test fun parentAndRunOnlyReferencesRemainCopyableWithoutChildNativeId() {
        assertTrue(reference(target.copy(conversation="")).uri!!.contains("run=run"))
        val child=reference(target.copy(run="",conversation="",agentId="child",parentConversation="parent")).uri!!
        assertTrue(child.contains("conversation=parent"));assertTrue(child.contains("agent=child"))
    }
    @Test fun invalidAndOversizedFieldsStayUnavailable() {
        listOf(target.copy(conversation="bad\u0000id"),target.copy(run="x".repeat(513)),target.copy(session="claude/"+"я".repeat(257))).forEach {
            assertNull(reference(it).uri)
        }
    }
}
