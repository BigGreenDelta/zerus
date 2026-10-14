package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class DirectoryCreationTest {
    private val machine=MachineKey("workspace","computer")
    private val action=SessionAction("11111111-1111-4111-8111-111111111111",Target(machine.computerId,"","","",machine.connectionId),DirectoryCreation.operation,
        JSONObject().put("path","/alias/New project ; $ literal").toString())
    private fun receipt()=JSONObject().put("request_id",action.requestId).put("state","completed").put("result",JSONObject()
        .put("request_id",action.requestId).put("status","created").put("requested_path",JSONObject(action.arguments).getString("path"))
        .put("path","/canonical/New project ; $ literal").put("parent","/canonical"))
    @Test fun absoluteLiteralPathIsPreservedAndUtf8Bounded() {
        val literal=JSONObject(action.arguments).getString("path")
        assertEquals(literal,SessionActionPolicies.arguments(DirectoryCreation.operation,JSONObject(action.arguments)).getString("path"))
        assertEquals("",DirectoryCreation.pathError("/"+"я".repeat(2047)))
        assertTrue(DirectoryCreation.pathError("/"+"я".repeat(2048)).isNotBlank())
        listOf("relative","~/new","/","/new/","/new/../other","/new//other","/new\nother","/new\u007fother").forEach { assertTrue(it,DirectoryCreation.pathError(it).isNotBlank()) }
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments(DirectoryCreation.operation,JSONObject().put("path","/new").put("agent","codex")) }
    }
    @Test fun confirmedCanonicalFolderHasNoSessionResult() {
        val settled=SessionActionPolicies.result(action,receipt())
        assertEquals("completed",settled.status);assertEquals("/canonical/New project ; $ literal",settled.resultPath)
        assertNull(settled.resultTarget);assertFalse(settled.blocksSending)
    }
    @Test fun wrongReceiptPathParentStatusAndTargetRemainUncertain() {
        val wrong=receipt().put("request_id","other")
        assertEquals("uncertain",SessionActionPolicies.result(action,wrong).status)
        listOf("request_id" to "other","requested_path" to "/different","parent" to "/wrong","path" to "/canonical/../other","path" to "/canonical/another-leaf","status" to "exists").forEach { (field,value) ->
            val r=receipt();r.getJSONObject("result").put(field,value)
            assertEquals(field,"uncertain",SessionActionPolicies.result(action,r).status)
        }
        assertEquals("uncertain",SessionActionPolicies.result(action.copy(target=action.target.copy(session="codex/project/existing")),receipt()).status)
        val invalid=action.copy(requestId="request")
        val invalidReceipt=receipt().put("request_id",invalid.requestId).also { it.getJSONObject("result").put("request_id",invalid.requestId) }
        assertEquals("uncertain",SessionActionPolicies.result(invalid,invalidReceipt).status)
    }
    @Test fun interruptionBlocksOnlyExactComputerAndDoesNotBecomeReplay() {
        val interrupted=MessageCodec.state(MessageCodec.stateJson(MessageState(sessionActions=listOf(action)))).sessionActions.single()
        assertEquals("uncertain",interrupted.status);assertEquals(action.requestId,interrupted.requestId)
        assertEquals(interrupted,DirectoryCreation.pending(listOf(interrupted),machine))
        assertNull(DirectoryCreation.pending(listOf(interrupted),machine.copy(computerId="other")))
        assertNull(DirectoryCreation.pending(listOf(interrupted),machine.copy(connectionId="other")))
        assertNull(DirectoryCreation.pending(listOf(action.copy(status="failed")),machine))
        assertEquals("failed",SessionActionPolicies.result(interrupted,JSONObject().put("request_id",action.requestId).put("state","failed").put("error","Already exists")).status)
    }
    @Test fun childNamesAreSingleLiteralComponents() {
        assertEquals("/New project",DirectoryCreation.child("/","New project"))
        assertEquals("/fixture/New $ project",DirectoryCreation.child("/fixture","New $ project"))
        listOf("","..",".","nested/name","bad\nname").forEach { assertEquals("",DirectoryCreation.child("/fixture",it)) }
    }
}
