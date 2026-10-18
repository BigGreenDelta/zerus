package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class SessionProjectControllerTest {
    private val native="11111111-1111-4111-8111-111111111111"
    private val swarm="22222222-2222-4222-8222-222222222222"
    private val target=Target("computer","claude/example/task","run-one","one","workspace")
    private inner class Host:SessionProjectAccess {
        var selected=target
        var owners:Any=Any()
        var connection=Connection("workspace","Synthetic","https://relay.example.invalid","synthetic-token")
        var machine=Machine("workspace","computer","Synthetic",true,route=MachineRoute.Direct,machineId=native)
        var records=listOf<SessionAction>()
        var flights=setOf<String>()
        var barrier:CompletableDeferred<Unit>?=null
        val order=mutableListOf<String>()
        override fun capture(target:Target)=SessionProjectHost(target,connection,machine,1,owners)
        override fun current(captured:SessionProjectHost)=selected==captured.target && owners===captured.connectionGeneration && captured.connection==connection && captured.route==SessionProjectProtocol.fingerprint(connection,machine)
        override fun reason(target:Target)=""
        override fun mutationReason(target:Target,ownRequest:String)=if(records.any { it.target==target && it.blocksSending && it.requestId!=ownRequest }) "Pending" else ""
        override fun actions()=records
        override fun flight(id:String,active:Boolean) { flights=if(active) flights+id else flights-id }
        override suspend fun persist(action:SessionAction) { order+="persist-"+action.status;barrier?.await();records=records.filterNot { it.requestId==action.requestId }+action }
        override suspend fun settle(action:SessionAction,receipt:JSONObject) { order+="settle";records=records.filterNot { it.requestId==action.requestId }+SessionProjectProtocol.result(action,receipt) }
        override fun refresh() {}
    }
    private fun page(body:JSONObject,ids:List<String> = listOf("two"),epoch:String="b".repeat(64),next:String?=null)=JSONObject()
        .put("request_id",body.getString("request_id")).put("state","completed").put("result",JSONObject().put("schema",1).put("status","ok")
            .put("request_id",body.getString("request_id")).put("name",target.session).put("run_id",target.run).put("conversation_id",target.conversation)
            .put("machine_id",native).put("swarm_id",swarm).put("current_project_id","ungrouped").put("membership_token","a".repeat(64)).put("catalog_epoch",epoch)
            .put("projects",JSONArray().also { array -> ids.forEach { array.put(JSONObject().put("id",it).put("name",it).put("color","#123456")) } }).put("next_cursor",next ?: JSONObject.NULL))
    private fun assigned(body:JSONObject)=page(body).also { receipt -> val raw=receipt.getJSONObject("result")
        raw.remove("projects");raw.remove("next_cursor");raw.put("status","assigned").put("project_id",body.getString("project_id")).put("current_project_id",body.getString("project_id")) }
    private inner class Network(val host:Host):SessionProjectNetwork {
        var writes=0;var gets=0;var reads=0
        var fail=false
        var admitQueued=false
        var polls=0
        var postRejection:Int?=null
        var awaitFailure:Exception?=null
        var receiptFailure:Exception?=null
        var receiptBarrier:CompletableDeferred<Unit>?=null
        var late:CompletableDeferred<Unit>?=null
        var changedEpoch=false
        var last:JSONObject?=null
        override suspend fun submit(connection:Connection,target:Target,operation:String,payload:JSONObject,id:String):JSONObject {
            if(operation==SessionProjectProtocol.assign) {
                writes++;host.order+="post";last=JSONObject(payload.toString())
                postRejection?.let { throw RelayException(it,"Synthetic POST rejection") }
                if(fail) throw IOException("synthetic")
                return if(admitQueued) JSONObject().put("request_id",id).put("state","queued") else assigned(payload)
            }
            reads++;late?.await()
            return if(changedEpoch && !payload.has("cursor")) page(payload,(0 until 100).map { "p$it" },next="b".repeat(64)+":100")
                else page(payload,epoch=if(changedEpoch) "c".repeat(64) else "b".repeat(64))
        }
        override suspend fun await(connection:Connection,initial:JSONObject):JSONObject {
            if(initial.string("state")=="queued") {
                polls++;awaitFailure?.let { throw it };return assigned(checkNotNull(last))
            }
            return initial
        }
        override suspend fun receipt(connection:Connection,id:String):JSONObject {
            gets++;receiptBarrier?.await();receiptFailure?.let { throw it };return assigned(checkNotNull(last))
        }
    }
    private inline fun fixture(block:(Host,Network,SessionProjectController)->Unit) {
        val scope=CoroutineScope(Dispatchers.Unconfined+SupervisorJob())
        try { val host=Host();val network=Network(host);block(host,network,SessionProjectController(host,scope,network)) }
        finally { scope.cancel() }
    }
    @Test fun lateOldNavigationAndMixedEpochPagesNeverPublish() = runBlocking {
        fixture { host,network,controller ->
            network.late=CompletableDeferred();controller.open(target);host.selected=target.copy(run="new-run")
            network.late!!.complete(Unit);assertNull(controller.catalog);assertEquals(0,network.writes)
        }
        fixture { _,network,controller -> network.changedEpoch=true;controller.open(target);assertEquals(2,network.reads);assertNull(controller.catalog);assertEquals(0,network.writes) }
    }
    @Test fun intentIsDurableBeforeOnlyPostAndDuplicateTapDoesNotPost() = runBlocking {
        fixture { host,network,controller ->
            controller.open(target);val choice=checkNotNull(controller.catalog).projects.single()
            host.barrier=CompletableDeferred();controller.assign(choice);controller.assign(choice)
            assertEquals(0,network.writes);assertTrue(host.records.isEmpty())
            host.barrier!!.complete(Unit)
            assertEquals(1,network.writes);assertEquals(listOf("persist-sending","post","settle"),host.order)
            assertEquals("completed",host.records.single().status)
        }
    }
    @Test fun staleCatalogAgeCannotAuthorizeAnotherPost() {
        val scope=CoroutineScope(Dispatchers.Unconfined+SupervisorJob())
        try {
            val host=Host();val network=Network(host);var clock=0L
            val controller=SessionProjectController(host,scope,network) { clock }
            controller.open(target);val choice=checkNotNull(controller.catalog).projects.single()
            clock=60_000_000_001L;assertFalse(controller.canAssign(choice));controller.assign(choice)
            assertEquals(0,network.writes);assertTrue(controller.notice.contains("expired"))
        } finally { scope.cancel() }
    }
    @Test fun connectionAbaFailsClosedAndUnknownOnlyChecksOriginalReceipt() = runBlocking {
        fixture { host,network,controller ->
            controller.open(target);val choice=checkNotNull(controller.catalog).projects.single();host.owners=Any()
            assertFalse(controller.canAssign(choice));controller.assign(choice);assertEquals(0,network.writes)
        }
        fixture { host,network,controller ->
            controller.open(target);network.fail=true;controller.assign(checkNotNull(controller.catalog).projects.single())
            assertEquals(1,network.writes);assertEquals("uncertain",host.records.single().status)
            controller.assign(checkNotNull(controller.catalog).projects.single());assertEquals(1,network.writes)
            val original=host.records.single();host.connection=host.connection.copy(token="replacement-token")
            controller.check(original);assertEquals(0,network.gets)
            host.connection=host.connection.copy(token="synthetic-token");controller.check(original)
            assertEquals(1,network.gets);assertEquals(1,network.writes);assertEquals(original.requestId,host.records.single().requestId);assertEquals("completed",host.records.single().status)
        }
    }
    @Test fun anAcceptedPostWithReceiptGetRefusalRemainsUncertainAndOnlyChecksOriginalUuid() {
        for(status in listOf(401,404,429)) fixture { host,network,controller ->
            controller.open(target)
            val choice=checkNotNull(controller.catalog).projects.single()
            network.admitQueued=true;network.awaitFailure=RelayException(status,"Synthetic receipt GET failure")
            controller.assign(choice)
            val original=host.records.single()
            assertEquals(1,network.writes);assertEquals(1,network.polls);assertEquals("uncertain",original.status)
            assertTrue(original.blocksSending);assertFalse(controller.canAssign(choice))
            assertTrue(host.flights.isEmpty());assertTrue(controller.active.isEmpty())
            controller.assign(choice);assertEquals(1,network.writes)
            network.awaitFailure=null
            controller.check(original)
            assertEquals(1,network.gets);assertEquals(1,network.writes)
            assertEquals(original.requestId,host.records.single().requestId)
            assertEquals("completed",host.records.single().status)
            assertTrue(host.flights.isEmpty());assertTrue(controller.active.isEmpty())
        }
    }
    @Test fun onlyAnActualPostRejectionCanBeDefiniteFailure() {
        for(status in listOf(400,401,403,404,413,422,429)) fixture { host,network,controller ->
            controller.open(target);network.postRejection=status
            controller.assign(checkNotNull(controller.catalog).projects.single())
            assertEquals(1,network.writes);assertEquals(0,network.gets)
            assertEquals(0,network.polls)
            assertEquals("failed",host.records.single().status)
            assertFalse(host.records.single().blocksSending)
            assertTrue(host.flights.isEmpty());assertTrue(controller.active.isEmpty())
        }
    }
    @Test fun receiptChecksReleaseTransientFlightOnSuccessFailureAndCancellation() {
        for(failure in listOf(null,IOException("synthetic"),CancellationException("synthetic"))) fixture { host,network,controller ->
            controller.open(target);network.fail=true
            controller.assign(checkNotNull(controller.catalog).projects.single())
            val original=host.records.single()
            assertEquals("uncertain",original.status);assertTrue(host.flights.isEmpty())
            network.receiptFailure=failure;network.receiptBarrier=CompletableDeferred()
            controller.check(original)
            assertEquals(setOf(original.requestId),host.flights)
            assertEquals(setOf(original.requestId),controller.active)
            network.receiptBarrier!!.complete(Unit)
            assertTrue(host.flights.isEmpty());assertTrue(controller.active.isEmpty())
            assertEquals(1,network.writes);assertEquals(1,network.gets)
            assertEquals(original.requestId,host.records.single().requestId)
            assertEquals(if(failure==null) "completed" else "uncertain",host.records.single().status)
        }
    }
}
