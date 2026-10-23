package app.zerus.mobile

import org.json.JSONObject
import org.json.JSONArray
import org.junit.Assert.*
import org.junit.Test

class RepositoryAcquisitionTest {
    private val id="11111111-1111-4111-8111-111111111111"
    private val nativeId="22222222-2222-4222-8222-222222222222"
    private val machine=MachineKey("workspace","computer")
    private val args=RepositoryAcquisition.arguments(nativeId,"git@forge:owner/source.git","/fixture/source")
    private val action=SessionAction(id,Target("computer","","","","workspace"),RepositoryAcquisition.clone,args.toString())
    private fun receipt(readId:String=id)=JSONObject().put("request_id",readId).put("state","completed").put("result",JSONObject()
        .put("schema",1).put("request_id",readId).put("clone_request_id",id).put("machine_id",nativeId).put("status","cloned")
        .put("parent","/fixture").put("name","source").put("path","/fixture/source"))
    @Test fun suggestionsAndManualDestinationAreIndependent() {
        assertEquals("source",RepositoryAcquisition.suggestedName("https://example.test/owner/source.git"))
        assertEquals("source",RepositoryAcquisition.suggestedName("git@forge:owner/source.git"))
        assertEquals("repo",RepositoryAcquisition.suggestedName("forge:repo"))
        assertEquals("/source",RepositoryAcquisition.destination("/","source"))
        assertEquals("source",args.getString("name"));assertEquals("/fixture",args.getString("parent"))
        assertEquals("custom",RepositoryAcquisition.arguments(nativeId,"git@forge:other.git","/fixture/custom").getString("name"))
        listOf("C:/source","C:\\source","/source","file:///source","ext::command","https://token@example.test/repo","ssh://user:password@example.test/repo","https://example.test/repo?token=value").forEach { url ->
            assertThrows(IllegalArgumentException::class.java) { RepositoryAcquisition.arguments(nativeId,url,"/fixture/source") }
        }
    }
    @Test fun receiptBindsComputerOriginalUuidAndDestination() {
        assertEquals("completed",SessionActionPolicies.result(action,receipt()).status)
        assertEquals("/fixture/source",SessionActionPolicies.result(action,receipt()).resultPath)
        for(key in listOf("request_id","clone_request_id","machine_id","parent","path","name")) {
            val wrong=receipt();wrong.getJSONObject("result").put(key,"other")
            assertEquals(key,"uncertain",SessionActionPolicies.result(action,wrong).status)
        }
        assertEquals("uncertain",SessionActionPolicies.result(action,receipt().put("request_id",nativeId)).status)
        assertEquals("uncertain",SessionActionPolicies.result(action.copy(target=action.target.copy(session="existing")),receipt()).status)
    }
    @Test fun restartKeepsOriginalAttemptAndRecoveryHasNoSessionAuthority() {
        val recovered=MessageCodec.state(MessageCodec.stateJson(MessageState(sessionActions=listOf(action)))).sessionActions.single()
        assertEquals("uncertain",recovered.status);assertEquals(id,recovered.requestId)
        assertNotNull(RepositoryAcquisition.pending(listOf(recovered),machine))
        assertNotNull(RepositoryAcquisition.pending(listOf(recovered),machine,"/fixture/source"))
        assertNull(RepositoryAcquisition.pending(listOf(recovered),machine,"/fixture/different"))
        assertNotNull(RepositoryAcquisition.pending(listOf(recovered.copy(arguments="{}")),machine,"/fixture/different"))
        assertNull(RepositoryAcquisition.pending(listOf(recovered),machine.copy(connectionId="other")))
        val confirmed=RepositoryAcquisition.result(recovered,receipt(nativeId),nativeId)
        assertEquals("completed",confirmed.status);assertNull(confirmed.resultTarget)
        val wrong=receipt(nativeId);wrong.getJSONObject("result").put("clone_request_id",nativeId)
        assertEquals("uncertain",RepositoryAcquisition.result(recovered,wrong,nativeId).status)
        assertEquals("uncertain",RepositoryAcquisition.result(recovered,JSONObject().put("request_id",nativeId).put("state","failed"),nativeId).status)
    }
    @Test fun uncertainOrFailedCloneNeverBecomesReady() {
        val unknown=receipt();unknown.getJSONObject("result").put("status","unconfirmed")
        assertTrue(SessionActionPolicies.result(action,unknown).blocksSending)
        val failed=receipt();failed.getJSONObject("result").put("status","failed").put("detail","Git refused")
        assertEquals("failed",SessionActionPolicies.result(action,failed).status)
        assertEquals("failed",SessionActionPolicies.result(action,JSONObject().put("request_id",id).put("state","failed")).status)
    }
    @Test fun canonicalReadOnlyCheckProposesVisibleDestinationButMutationStaysExact() {
        val requested=RepositoryAcquisition.arguments(nativeId,"git@forge:owner/source.git","/var/tmp/source")
        val ready=receipt(nativeId);ready.getJSONObject("result").put("clone_request_id",nativeId).put("status","ready")
            .put("parent","/private/var/tmp").put("path","/private/var/tmp/source")
        val decision=RepositoryAcquisition.checkedDestination(requested,ready,nativeId)
        assertTrue(decision.needsConfirmation);assertEquals("/private/var/tmp/source",decision.path)
        val canonical=decision.arguments
        val rechecked=RepositoryAcquisition.checkedDestination(canonical,ready,nativeId)
        assertFalse(rechecked.needsConfirmation)
        assertEquals("/private/var/tmp",canonical.getString("parent"))
        assertEquals(requested.getString("url"),canonical.getString("url"))
        assertNotEquals(RepositoryAcquisition.destination(requested.getString("parent"),requested.getString("name")),
            RepositoryAcquisition.destination(canonical.getString("parent"),canonical.getString("name")))
        val original=action.copy(arguments=requested.toString())
        val changed=receipt();changed.getJSONObject("result").put("parent","/private/var/tmp").put("path","/private/var/tmp/source")
        assertEquals("uncertain",RepositoryAcquisition.result(original,changed).status)
        changed.put("request_id",nativeId);changed.getJSONObject("result").put("request_id",nativeId)
        assertEquals("uncertain",RepositoryAcquisition.result(original,changed,nativeId).status)
        val wrong=JSONObject(ready.toString());wrong.getJSONObject("result").put("name","other")
        assertThrows(IllegalArgumentException::class.java) { RepositoryAcquisition.checkedDestination(requested,wrong,nativeId) }
    }
    @Test fun everyUnresolvedDestinationHasOriginalStatusRecoveryAndBoundedStablePages() {
        fun attempt(index:Int)=action.copy(requestId="00000000-0000-4000-8000-%012d".format(index),status="uncertain",
            arguments=RepositoryAcquisition.arguments(nativeId,"git@forge:owner/source.git","/fixture/source$index").toString())
        val actions=(1..13).map(::attempt)
        for(index in listOf(1,2)) {
            val page=RepositoryAcquisition.recovery(actions,machine,"/fixture/source$index")
            assertEquals(actions[index-1].requestId,page.selected?.requestId)
            val read=RepositoryAcquisition.statusPayload(page.selected!!,nativeId)
            assertEquals(actions[index-1].requestId,read.string("clone_request_id"));assertEquals("source$index",read.string("name"))
            assertEquals(nativeId,read.string("request_id"));assertEquals(5,page.others.size)
        }
        val found=mutableSetOf<String>();var cursor=""
        do {
            val page=RepositoryAcquisition.recovery(actions.reversed(),machine,"/fixture/new",cursor)
            assertTrue(page.others.size<=5);page.others.forEach { assertTrue(found.add(it.requestId)) };cursor=page.next.orEmpty()
        } while(cursor.isNotEmpty())
        assertEquals(actions.map { it.requestId }.toSet(),found)
        assertNull(RepositoryAcquisition.pending(actions,machine,"/fixture/new"))
        assertTrue(RepositoryAcquisition.recovery(actions,machine.copy(connectionId="other"),"/fixture/source1").others.isEmpty())
    }
    @Test fun freshSelectedCheckoutProofIsRequiredBeforeLaunch() {
        val raw=JSONObject().put("state","ok").put("stale",false).put("partial",true)
            .put("path","/fixture/source").put("requested_path","/fixture/source").put("common_dir","/fixture/source/.git")
            .put("worktrees",JSONArray().put(JSONObject().put("path","/fixture/source").put("kind","main").put("available",true)))
        assertTrue(RepositoryAcquisition.verifiedCheckout(raw,"/fixture/source"))
        assertFalse(RepositoryAcquisition.verifiedCheckout(raw,"/fixture/other"))
        assertFalse(RepositoryAcquisition.verifiedCheckout(JSONObject(raw.toString()).put("stale",true),"/fixture/source"))
        val missing=JSONObject(raw.toString());missing.getJSONArray("worktrees").getJSONObject(0).put("available",false)
        assertFalse(RepositoryAcquisition.verifiedCheckout(missing,"/fixture/source"))
    }
}
