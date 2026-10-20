package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionProjectProtocolTest {
    private val native="11111111-1111-4111-8111-111111111111"
    private val swarm="22222222-2222-4222-8222-222222222222"
    private val id="33333333-3333-4333-8333-333333333333"
    private val target=Target("computer","claude/example/task","run-one","one","workspace")
    private fun page()=JSONObject().put("request_id",id).put("state","completed").put("result",JSONObject()
        .put("schema",1).put("status","ok").put("request_id",id).put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
        .put("machine_id",native).put("swarm_id",swarm).put("current_project_id","ungrouped").put("membership_token","a".repeat(64)).put("catalog_epoch","b".repeat(64))
        .put("projects",JSONArray().put(JSONObject().put("id","two").put("name","Два ' $(literal)").put("color","#123456"))).put("next_cursor",JSONObject.NULL))
    private fun action():SessionAction {
        val catalog=SessionProjectProtocol.catalog(page(),target,native,id)
        return SessionAction(id,target,SessionProjectProtocol.assign,SessionProjectProtocol.arguments(native,"c".repeat(64),catalog,catalog.projects.single()).toString())
    }
    private fun assigned(action:SessionAction)=page().also { receipt ->
        val raw=receipt.getJSONObject("result");val args=JSONObject(action.arguments)
        raw.remove("projects");raw.remove("next_cursor");raw.put("status","assigned").put("project_id",args.getString("project_id")).put("current_project_id",args.getString("project_id"))
    }
    @Test fun literalCatalogRequiresExactScopeAndNoExtraAuthority() {
        val catalog=SessionProjectProtocol.catalog(page(),target,native,id)
        assertEquals("Два ' $(literal)",catalog.projects.single().name)
        for(field in listOf("request_id","name","run_id","conversation_id","machine_id")) {
            val raw=page();raw.getJSONObject("result").put(field,"foreign")
            assertThrows(IllegalArgumentException::class.java) { SessionProjectProtocol.catalog(raw,target,native,id) }
        }
        val raw=page();raw.getJSONObject("result").put("directory","/unexpected")
        assertThrows(IllegalArgumentException::class.java) { SessionProjectProtocol.catalog(raw,target,native,id) }
        val args=JSONObject(action().arguments).put("directory","/unexpected")
        assertThrows(IllegalArgumentException::class.java) { SessionProjectProtocol.arguments(args) }
    }
    @Test fun assignmentProofCannotBeInferredFromLaterMembershipOrAnotherReceipt() {
        val action=action();assertEquals("completed",SessionActionPolicies.result(action,assigned(action)).status)
        for(field in listOf("request_id","name","run_id","conversation_id","machine_id","swarm_id","membership_token","catalog_epoch","project_id","current_project_id")) {
            val receipt=assigned(action);receipt.getJSONObject("result").put(field,"foreign")
            assertEquals("uncertain",SessionActionPolicies.result(action,receipt).status)
        }
        assertEquals("uncertain",SessionActionPolicies.result(action,page()).status)
        val failed=assigned(action);failed.getJSONObject("result").put("status","failed").put("error","Membership changed")
        assertEquals("failed",SessionActionPolicies.result(action,failed).status)
    }
    @Test fun intentSurvivesProcessRecoveryWithoutModifyingDraftOrCreatingAnotherUuid() {
        val action=action();val draft=Draft(target,"unsent exact text",revision="revision",generation="generation")
        val state=MessageState(drafts=listOf(draft),sessionActions=listOf(action))
        val restored=MessageCodec.state(MessageCodec.stateJson(state))
        assertEquals(draft,restored.drafts.single());assertEquals(action.requestId,restored.sessionActions.single().requestId)
        assertEquals(action.arguments,restored.sessionActions.single().arguments);assertEquals("uncertain",restored.sessionActions.single().status)
        assertThrows(IllegalArgumentException::class.java) { restored.enqueue(draft,"another-send",1.0) }
        val payload=SessionProjectProtocol.payload(action)
        assertFalse(payload.has("route_fingerprint"));assertFalse(payload.has("project_name"));assertFalse(payload.has("directory"))
        assertEquals(setOf("request_id","expected_run_id","expected_conversation_id","expected_machine_id","swarm_id","membership_token","catalog_epoch","project_id"),payload.keys().asSequence().toSet())
    }
}
