package app.zerus.mobile

import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.async
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.Dispatchers
import okhttp3.mockwebserver.Dispatcher
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import okhttp3.mockwebserver.RecordedRequest
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

internal class MemoryWorkspaceJournal:WorkspaceJournal {
    private val states=mutableMapOf<String,WorkspaceState>()
    private val heads=mutableMapOf<Pair<String,Long>,String>()
    private val outgoing=mutableMapOf<Pair<String,String>,Pair<String,WorkspaceEnvelope>>()
    private val settled=mutableSetOf<Pair<String,String>>()
    override fun create(connection:Connection,state:WorkspaceState) { check(states.putIfAbsent(connection.id,state)==null);state.membership?.let { heads[connection.id to it.revision]=it.head;heads[connection.id to state.pinnedRevision]=state.pinnedHead } }
    override fun load(connection:Connection)=checkNotNull(states[connection.id])
    override fun update(connection:Connection,expected:WorkspaceState,next:WorkspaceState,changes:List<WorkspaceMembershipChange>) {
        check(load(connection).json().toString()==expected.json().toString());states[connection.id]=next
        if(expected.membership==null && next.membership!=null) heads[connection.id to next.pinnedRevision]=next.pinnedHead
        changes.forEach { heads[connection.id to it.revision]=it.head() }
    }
    override fun head(connection:Connection,revision:Long)=heads[connection.id to revision]
    override fun reserve(connection:Connection,id:String,plainHash:String,envelope:WorkspaceEnvelope):WorkspaceEnvelope {
        val key=connection.id to id;check(key !in settled)
        outgoing[key]?.let { check(it.first==plainHash);return it.second }
        outgoing[key]=plainHash to envelope;return envelope
    }
    override fun recordedRequest(connection:Connection,id:String)=outgoing[connection.id to id]?.second?.header
    override fun settle(connection:Connection,id:String) { settled+=connection.id to id }
    override fun forget(connection:Connection) { states.remove(connection.id) }
}

class EncryptedRelayTest {
    private val f=JSONObject(javaClass.classLoader!!.getResource("encrypted-workspace-v1.json")!!.readText())
    private val gateway=WorkspaceIdentity.parse(f.getJSONObject("gateway_identity"))
    private val phone=WorkspaceIdentity.parse(f.getJSONObject("phone_identity"))
    private val roster=WorkspaceMembership.parse(f.getJSONObject("final_state"))
    private fun state(pending:Boolean=false)=WorkspaceState(phone,WorkspaceCandidate.parse(f.getJSONObject("candidate")),gateway.device,0,
        f.getJSONObject("bootstrap_snapshot").getJSONObject("state").getString("head"),if(pending) null else roster,
        mapOf("machine-fixture" to WorkspaceRoute("machine-fixture",gateway.device.deviceId,"route-fixture","machine-fixture")),
        if(pending) null else WorkspaceApproval.parse(f.getJSONObject("approval")))
    private fun history()=JSONObject().put("changes",JSONArray()).put("membership_revision",roster.revision).put("membership_head",roster.head).put("next_after",JSONObject.NULL)

    private fun catalogContent(computer:String,name:String="Cached computer",folder:String="/workspace/cached"):JSONObject {
        val session=JSONObject().put("name","codex/fixture/$computer")
            .put("run_id","run-$computer").put("conversation_id","conversation-$computer").put("state","running").put("activity","idle")
            .put("mobile_project_id","shared").put("cwd",folder)
        val account=JSONObject().put("id","fixture").put("provider","codex")
            .put("usage",JSONObject().put("status","ok").put("identity",JSONObject().put("name",name)))
        val project=JSONObject().put("id","shared").put("name",name)
            .put("folders",JSONArray().put(JSONObject().put("id","folder-$computer").put("path",folder)))
        val snapshot=JSONObject().put("sessions",JSONArray().put(session))
            .put("mobile_capabilities",JSONObject().put("protocol_version",1).put("operations",JSONArray(listOf("history"))).put("features",JSONArray(listOf("accounts_snapshot"))))
            .put("mobile_accounts",JSONObject().put("schema",1).put("available",true).put("checked_at",1000).put("accounts",JSONArray().put(account)))
            .put("mobile_projects",JSONObject().put("schema",1).put("available",true).put("swarm_id","swarm").put("projects",JSONArray().put(project)))
        return JSONObject().put("id",computer).put("machine_id",computer).put("name",name).put("online",true).put("via",JSONObject.NULL).put("snapshot",snapshot)
    }

    private fun snapshotRow(computer:String,expires:Long,name:String="Fresh computer",privateId:String=computer,clearOverride:String?=null,route:String="route-$computer"):JSONObject {
        val clear=catalogContent(privateId,name).put("encrypted_transport",JSONObject().put("gateway_id",gateway.device.deviceId).put("route_id",route).put("machine_id",privateId))
        val header=WorkspaceHeader(roster.workspaceId,gateway.device.deviceId,phone.device.deviceId,1,1,gateway.device.deviceId,route,
            "snapshot-$computer","snapshot","read",expires,roster.revision,roster.head)
        return JSONObject().put("computer_id",computer).put("gateway_id",gateway.device.deviceId).put("route_id",route).put("online",true)
            .put("envelope",WorkspaceEnvelope.seal(gateway,phone.device,header,(clearOverride ?: clear.toString()).toByteArray()).json())
    }

    private fun serveCatalog(server:MockWebServer,pages:List<List<JSONObject>>,onPage:(Int)->Unit={},onPost:(WorkspaceEnvelope)->Unit={}) {
        server.dispatcher=object:Dispatcher() {
            override fun dispatch(r:RecordedRequest):MockResponse {
                if(r.path!!.startsWith("/v2/membership")) return MockResponse().setBody(history().toString())
                if(r.path!!.startsWith("/v2/computers")) {
                    val index=r.requestUrl!!.queryParameter("after")?.removePrefix("page-")?.toInt() ?: 0
                    onPage(index)
                    return MockResponse().setBody(JSONObject().put("computers",JSONArray(pages[index])).put("next_after",if(index+1<pages.size) "page-${index+1}" else JSONObject.NULL).toString())
                }
                if(r.method=="POST" && r.path=="/v2/requests") {
                    val envelope=WorkspaceEnvelope.parse(JSONObject(r.body.readUtf8()).getJSONObject("envelope"));onPost(envelope)
                    return MockResponse().setBody(JSONObject().put("request_id",envelope.header.requestId).put("state","queued").put("envelope",JSONObject.NULL).toString())
                }
                return MockResponse().setResponseCode(404).setBody("{}")
            }
        }
        server.start()
    }

    @Test fun expiredComputerKeepsFreshComputerUsableAcrossRowsPagesAndRepeatedRefresh()=runBlocking {
        val old="22222222-2222-4222-8222-222222222222";val fresh="33333333-3333-4333-8333-333333333333"
        val now=2_000_000_000L
        val expired=snapshotRow(old,now-60,"Expired private content")
        val current=snapshotRow(fresh,now+300)
        for(pages in listOf(listOf(listOf(expired,current)),listOf(listOf(current,expired)),listOf(listOf(expired),listOf(current)),listOf(listOf(current),listOf(expired)))) {
            MockWebServer().use { server ->
                val posted=java.util.Collections.synchronizedList(mutableListOf<String>())
                serveCatalog(server,pages) { posted+=it.header.routeId }
                val connection=Connection("mixed-catalog","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
                val store=MemoryWorkspaceJournal();store.create(connection,state().withRoutes(mapOf(old to WorkspaceRoute(old,gateway.device.deviceId,"route-$old",old))))
                val api=RelayApi(workspace=store,catalogNowSeconds={ now })
                repeat(2) {
                    val outcome=runCatching { api.computers(connection) }
                    assertTrue("Expired A must not block fresh B",outcome.isSuccess)
                    val result=outcome.getOrThrow()
                    assertEquals(listOf(fresh),result.getJSONArray("computers").objects().map { row -> row.string("id") })
                    assertEquals(old,result.getJSONArray("unavailable_computers").getString(0))
                    assertFalse(result.toString().contains("Expired private content"))
                    assertEquals(setOf(fresh),store.load(connection).routes.keys)
                }
                assertTrue(runCatching { api.post(connection,old,operation="inspect",payload=JSONObject(),id="expired-action") }.isFailure)
                api.post(connection,fresh,operation="inspect",payload=JSONObject(),id="fresh-action")
                assertEquals(listOf("route-$fresh"),posted.toList())
            }
        }
    }

    @Test fun catalogAdmissionRechecksExpiryAfterHeldPageAcrossRowAndPageOrders()=runBlocking {
        val now=2_000_000_000L;val expiring=snapshotRow("old",now+1,"Expiring private content")
        val fresh=snapshotRow("fresh",now+300)
        for(pages in listOf(listOf(listOf(expiring,fresh),emptyList()),listOf(listOf(fresh,expiring),emptyList()),listOf(listOf(expiring),listOf(fresh)),listOf(listOf(fresh),listOf(expiring),emptyList()))) {
            MockWebServer().use { server ->
                val clock=java.util.concurrent.atomic.AtomicLong(now)
                val entered=java.util.concurrent.CountDownLatch(1);val release=java.util.concurrent.CountDownLatch(1)
                serveCatalog(server,pages,onPage={ index -> if(index==pages.lastIndex) {
                    entered.countDown();check(release.await(5,java.util.concurrent.TimeUnit.SECONDS))
                } })
                val connection=Connection("held-page","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
                val store=MemoryWorkspaceJournal();store.create(connection,state())
                val api=RelayApi(workspace=store,catalogNowSeconds=clock::get)
                val pending=async(Dispatchers.Default) { runCatching { api.computers(connection) } }
                try {
                    assertTrue(withContext(Dispatchers.IO) { entered.await(5,java.util.concurrent.TimeUnit.SECONDS) })
                    clock.set(now+2);release.countDown()
                    val outcome=pending.await();assertTrue("Fresh B must remain usable",outcome.isSuccess)
                    val result=outcome.getOrThrow()
                    assertEquals(listOf("fresh"),result.getJSONArray("computers").objects().map { it.string("id") })
                    assertEquals(listOf("old"),result.getJSONArray("unavailable_computers").let { (0 until it.length()).map(it::getString) })
                    assertFalse(result.toString().contains("Expiring private content"))
                    assertEquals(setOf("fresh"),store.load(connection).routes.keys)
                    assertTrue(runCatching { api.post(connection,"old",operation="inspect",payload=JSONObject(),id="expired-after-page") }.isFailure)
                } finally { release.countDown();pending.cancelAndJoin() }
            }
        }
    }

    @Test fun finalAdmissionKeepsFreshAlternativeWhenPreferredRouteExpiresDuringHeldPage()=runBlocking {
        val now=2_000_000_000L;val computer="same"
        val preferred=snapshotRow(computer,now+1,"Expiring preferred",route="preferred-route")
        val alternative=snapshotRow(computer,now+300,"Usable alternative",route="alternative-route")
        for(pages in listOf(listOf(listOf(preferred,alternative),emptyList()),listOf(listOf(alternative,preferred),emptyList()),listOf(listOf(preferred),listOf(alternative)),listOf(listOf(alternative),listOf(preferred),emptyList()))) {
            MockWebServer().use { server ->
                val clock=java.util.concurrent.atomic.AtomicLong(now)
                val entered=java.util.concurrent.CountDownLatch(1);val release=java.util.concurrent.CountDownLatch(1)
                val posted=java.util.Collections.synchronizedList(mutableListOf<String>())
                serveCatalog(server,pages,onPage={ index -> if(index==pages.lastIndex) {
                    entered.countDown();check(release.await(5,java.util.concurrent.TimeUnit.SECONDS))
                } },onPost={ posted+=it.header.routeId })
                val connection=Connection("held-alternative","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
                val store=MemoryWorkspaceJournal();store.create(connection,state().withRoutes(mapOf(computer to WorkspaceRoute(computer,gateway.device.deviceId,"preferred-route",computer))))
                val api=RelayApi(workspace=store,catalogNowSeconds=clock::get)
                val pending=async(Dispatchers.Default) { runCatching { api.computers(connection) } }
                try {
                    assertTrue(withContext(Dispatchers.IO) { entered.await(5,java.util.concurrent.TimeUnit.SECONDS) })
                    clock.set(now+2);release.countDown()
                    val outcome=pending.await();assertTrue("A fresh same-machine alternative must survive",outcome.isSuccess)
                    val result=outcome.getOrThrow()
                    assertEquals("Usable alternative",result.getJSONArray("computers").getJSONObject(0).string("name"))
                    assertEquals(0,result.getJSONArray("unavailable_computers").length())
                    assertEquals("alternative-route",store.load(connection).routes[computer]?.routeId)
                    api.post(connection,computer,operation="inspect",payload=JSONObject(),id="admitted-alternative")
                    assertEquals(listOf("alternative-route"),posted.toList())
                } finally { release.countDown();pending.cancelAndJoin() }
            }
        }
    }

    @Test fun catalogAdmissionClockRunsAfterFinalJournalLoad()=runBlocking {
        val now=2_000_000_000L;val clock=java.util.concurrent.atomic.AtomicLong(now)
        MockWebServer().use { server ->
            serveCatalog(server,listOf(listOf(snapshotRow("old",now+1,"Expiring private content"),snapshotRow("fresh",now+300))))
            val connection=Connection("load-admission","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            val store=MemoryWorkspaceJournal();store.create(connection,state())
            val opened=java.util.concurrent.atomic.AtomicInteger()
            val guarded=object:WorkspaceJournal by store {
                override fun open(connection:Connection,envelope:WorkspaceEnvelope,expected:WorkspaceHeader):ByteArray {
                    val clear=store.open(connection,envelope,expected);opened.incrementAndGet();return clear
                }
                override fun load(connection:Connection):WorkspaceState {
                    val loaded=store.load(connection)
                    if(opened.get()==2) clock.set(now+2)
                    return loaded
                }
            }
            val result=RelayApi(workspace=guarded,catalogNowSeconds=clock::get).computers(connection)
            assertEquals(listOf("fresh"),result.getJSONArray("computers").objects().map { it.string("id") })
            assertEquals(setOf("fresh"),store.load(connection).routes.keys)
        }
    }

    @Test fun freshSameComputerWinsOverExpiredRouteInBothPageOrders()=runBlocking {
        val computer="22222222-2222-4222-8222-222222222222"
        val expired=snapshotRow(computer,System.currentTimeMillis()/1000-60,"Expired private content")
        val fresh=snapshotRow(computer,System.currentTimeMillis()/1000+300,"Current private content")
        for(pages in listOf(listOf(listOf(expired,fresh)),listOf(listOf(fresh,expired)),listOf(listOf(expired),listOf(fresh)),listOf(listOf(fresh),listOf(expired)))) {
            MockWebServer().use { server ->
                serveCatalog(server,pages)
                val connection=Connection("same-computer","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
                val store=MemoryWorkspaceJournal();store.create(connection,state());val result=RelayApi(workspace=store).computers(connection)
                assertEquals("Current private content",result.getJSONArray("computers").getJSONObject(0).string("name"))
                assertEquals(0,result.getJSONArray("unavailable_computers").length())
                assertEquals(setOf(computer),store.load(connection).routes.keys)
            }
        }
    }

    @Test fun expiredMalformedOrUnauthenticatedSnapshotStillRefusesWholeCatalog()=runBlocking {
        val computer="22222222-2222-4222-8222-222222222222";val now=System.currentTimeMillis()/1000
        val invalidSignature=snapshotRow(computer,now-60).also { it.getJSONObject("envelope").put("signature",WorkspaceProtocol.encode(ByteArray(64))) }
        val invalidHead=snapshotRow(computer,now-60).also { it.getJSONObject("envelope").getJSONObject("header").put("membership_head",WorkspaceProtocol.encode(ByteArray(32))) }
        for(bad in listOf(invalidSignature,invalidHead,snapshotRow(computer,now-60,privateId="substituted-native"),snapshotRow(computer,now-60,clearOverride="not JSON"))) {
            MockWebServer().use { server ->
                serveCatalog(server,listOf(listOf(snapshotRow("fresh-computer",now+300),bad)))
                val connection=Connection("bad-catalog","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
                val store=MemoryWorkspaceJournal();store.create(connection,state());val previous=store.load(connection).routes
                assertTrue(runCatching { RelayApi(workspace=store).computers(connection) }.isFailure)
                assertEquals(previous,store.load(connection).routes)
            }
        }
    }

    @Test fun unavailableCachePreservesOnlyPriorScopedDisplayDataWithoutCapabilities() {
        val connection=Connection("cache","Fixture","https://relay.example","fixture-token",2)
        val previous=SessionCatalogParser.parse(connection,listOf(catalogContent("old","Cached A"),catalogContent("fresh","Cached B"),catalogContent("removed","Removed C")))
        val incoming=SessionCatalogParser.parse(connection,listOf(catalogContent("fresh","Current B","/workspace/current")))
        val response=JSONObject().put("unavailable_computers",JSONArray(listOf("old","fresh","unknown")))
        val first=WorkspaceCatalogCache.preserve(connection,response,incoming,previous.projects.sessions,previous.machines,previous.projects.projects,previous.accounts)
        val repeated=WorkspaceCatalogCache.preserve(connection,response,incoming,first.projects.sessions,first.machines,first.projects.projects,first.accounts)
        for(result in listOf(first,repeated)) {
            assertEquals(setOf("old","fresh"),result.machines.map { it.id }.toSet())
            assertTrue(result.machines.single { it.id=="old" }.let { !it.online && it.lastKnown })
            assertEquals("Current B",result.machines.single { it.id=="fresh" }.name)
            assertTrue(result.projects.sessions.single { it.target.computerId=="old" }.raw.getBoolean("last_known"))
            assertTrue(result.accounts.single { it.key.computerId=="old" }.let { it.stale && !it.online && !it.available && it.accounts.single().identity.name=="Cached A" })
            val project=result.projects.projects.single()
            assertTrue(project.stale);assertEquals(setOf("old","fresh"),project.computers.keys)
            assertEquals("/workspace/current",project.folders.single { it.computerId=="fresh" }.path)
            assertEquals(setOf(MachineKey(connection.id,"fresh")),result.operations.keys)
            assertEquals(setOf(MachineKey(connection.id,"fresh")),result.features.keys)
        }
        val foreign=Connection("foreign","Fixture",connection.url,connection.token,2)
        val foreignPrior=SessionCatalogParser.parse(foreign,listOf(catalogContent("old")))
        assertEquals(incoming.machines,WorkspaceCatalogCache.preserve(connection,response,incoming,foreignPrior.projects.sessions,foreignPrior.machines,foreignPrior.projects.projects,foreignPrior.accounts).machines)
    }

    @Test fun catalogJournalWorkLeavesCallerDispatcherAndMembershipChangePreventsCommit()=runBlocking {
        MockWebServer().use { server ->
            serveCatalog(server,listOf(listOf(snapshotRow("fresh-computer",System.currentTimeMillis()/1000+300))))
            val connection=Connection("worker-catalog","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            val store=MemoryWorkspaceJournal();store.create(connection,state())
            val updates=java.util.concurrent.atomic.AtomicInteger();val revoke=java.util.concurrent.atomic.AtomicBoolean(false)
            fun worker()=assertFalse("Synchronous journal work ran on the caller dispatcher",Thread.currentThread().name.startsWith("synthetic-ui-caller"))
            val guarded=object:WorkspaceJournal by store {
                override fun load(connection:Connection):WorkspaceState { worker();return store.load(connection) }
                override fun head(connection:Connection,revision:Long):String? { worker();return store.head(connection,revision) }
                override fun update(connection:Connection,expected:WorkspaceState,next:WorkspaceState,changes:List<WorkspaceMembershipChange>) { worker();updates.incrementAndGet();store.update(connection,expected,next,changes) }
                override fun open(connection:Connection,envelope:WorkspaceEnvelope,expected:WorkspaceHeader):ByteArray {
                    worker();val clear=store.open(connection,envelope,expected)
                    if(revoke.get()) {
                        val current=store.load(connection);val membership=checkNotNull(current.membership)
                        val unsigned=WorkspaceMembershipChange(membership.workspaceId,membership.revision+1,membership.head,gateway.device.deviceId,1,"revoke",null,phone.device.deviceId,"")
                        val change=unsigned.copy(signature=WorkspaceProtocol.encode(WorkspaceCrypto.sign(gateway.keys,unsigned.frame())))
                        store.update(connection,current,current.withMembership(membership.apply(change)),listOf(change))
                    }
                    return clear
                }
            }
            java.util.concurrent.Executors.newSingleThreadExecutor { Thread(it,"synthetic-ui-caller") }.asCoroutineDispatcher().use { caller ->
                val api=RelayApi(workspace=guarded)
                withContext(caller) { assertEquals(1,api.computers(connection).getJSONArray("computers").length()) }
                assertEquals(1,updates.get());updates.set(0);revoke.set(true)
                val routes=store.load(connection).routes
                assertTrue(withContext(caller) { runCatching { api.computers(connection) }.isFailure })
                assertEquals(0,updates.get());assertEquals(routes,store.load(connection).routes)
            }
        }
    }

    @Test fun cancelledCatalogDoesNotCommitOrPublishDecodedRows()=runBlocking {
        MockWebServer().use { server ->
            serveCatalog(server,listOf(listOf(snapshotRow("fresh-computer",System.currentTimeMillis()/1000+300))))
            val connection=Connection("cancel-catalog","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            val store=MemoryWorkspaceJournal();store.create(connection,state());val routes=store.load(connection).routes
            val entered=java.util.concurrent.CountDownLatch(1);val release=java.util.concurrent.CountDownLatch(1)
            val updates=java.util.concurrent.atomic.AtomicInteger();val published=java.util.concurrent.atomic.AtomicBoolean(false)
            val guarded=object:WorkspaceJournal by store {
                override fun update(connection:Connection,expected:WorkspaceState,next:WorkspaceState,changes:List<WorkspaceMembershipChange>) { updates.incrementAndGet();store.update(connection,expected,next,changes) }
                override fun open(connection:Connection,envelope:WorkspaceEnvelope,expected:WorkspaceHeader):ByteArray {
                    val clear=store.open(connection,envelope,expected);entered.countDown()
                    check(release.await(5,java.util.concurrent.TimeUnit.SECONDS));return clear
                }
            }
            val pending=launch { RelayApi(workspace=guarded).computers(connection);published.set(true) }
            try {
                assertTrue(withContext(Dispatchers.IO) { entered.await(5,java.util.concurrent.TimeUnit.SECONDS) })
                pending.cancel();release.countDown();pending.join()
                assertFalse(published.get());assertEquals(0,updates.get());assertEquals(routes,store.load(connection).routes)
            } finally { release.countDown();pending.cancelAndJoin() }
        }
    }

    @Test fun onlineCatalogRouteWinsForNewRequestsWhilePendingCiphertextKeepsOriginalRoute()=runBlocking {
        MockWebServer().use { server ->
            val computer="22222222-2222-4222-8222-222222222222"
            val pendingId="11111111-1111-4111-8111-111111111111";val newId="33333333-3333-4333-8333-333333333333"
            fun snapshot(route:String):JSONObject {
                val binding=JSONObject().put("gateway_id",gateway.device.deviceId).put("route_id",route).put("machine_id",computer)
                val clear=JSONObject().put("id",computer).put("machine_id",computer).put("name","Private computer").put("via",JSONObject.NULL)
                    .put("snapshot",JSONObject().put("sessions",JSONArray())).put("encrypted_transport",binding)
                val header=WorkspaceHeader(roster.workspaceId,gateway.device.deviceId,phone.device.deviceId,1,1,gateway.device.deviceId,route,
                    "snapshot-$route","snapshot","read",System.currentTimeMillis()/1000+300,roster.revision,roster.head)
                return JSONObject().put("computer_id",computer).put("gateway_id",gateway.device.deviceId).put("route_id",route)
                    .put("envelope",WorkspaceEnvelope.seal(gateway,phone.device,header,clear.toString().toByteArray()).json())
            }
            val preferred=snapshot("preferred-route");val alternative=snapshot("online-route")
            val mode=java.util.concurrent.atomic.AtomicInteger(0)
            val saved=java.util.Collections.synchronizedList(mutableListOf<String>())
            server.dispatcher=object:Dispatcher() {
                override fun dispatch(r:RecordedRequest):MockResponse {
                    if(r.path!!.startsWith("/v2/membership")) return MockResponse().setBody(history().toString())
                    if(r.path!!.startsWith("/v2/computers")) {
                        fun row(value:JSONObject,online:Boolean)=JSONObject(value.toString()).put("online",online)
                        val rows=when(mode.get()) {
                            0 -> listOf(row(alternative,true),row(preferred,false))
                            1 -> listOf(row(alternative,true),row(preferred,true))
                            2 -> listOf(row(preferred,false),row(alternative,false))
                            else -> listOf(row(preferred,false),row(alternative,true))
                        }
                        return MockResponse().setBody(JSONObject().put("computers",JSONArray(rows)).put("next_after",JSONObject.NULL).toString())
                    }
                    check(r.method=="POST" && r.path=="/v2/requests")
                    val wire=r.body.readUtf8();saved+=wire
                    val request=WorkspaceEnvelope.parse(JSONObject(wire).getJSONObject("envelope"))
                    val clear=JSONObject(String(request.open(gateway,phone.device,request.header)))
                    assertEquals(request.header.routeId,clear.getJSONObject("encrypted_transport").string("route_id"))
                    return MockResponse().setBody(JSONObject().put("request_id",request.header.requestId).put("state","queued").put("envelope",JSONObject.NULL).toString())
                }
            }
            server.start();val connection=Connection("route-choice","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            val store=MemoryWorkspaceJournal();store.create(connection,state().withRoutes(mapOf(computer to WorkspaceRoute(computer,gateway.device.deviceId,"preferred-route",computer))))
            val api=RelayApi(workspace=store);val target=Target(computer,"native-session","run","conversation",connection.id);val payload=target.json().put("text","Private message")
            api.submit(connection,target,"send",payload,pendingId)
            val computers=api.computers(connection).getJSONArray("computers")
            assertEquals(1,computers.length());assertTrue(computers.getJSONObject(0).getBoolean("online"))
            assertEquals("online-route",store.load(connection).routes.getValue(computer).routeId)
            api.submit(connection,target,"send",payload,newId)
            api.submit(connection,target,"send",payload,pendingId)
            assertEquals("preferred-route",WorkspaceEnvelope.parse(JSONObject(saved[0]).getJSONObject("envelope")).header.routeId)
            assertEquals("online-route",WorkspaceEnvelope.parse(JSONObject(saved[1]).getJSONObject("envelope")).header.routeId)
            assertEquals(saved[0],saved[2])
            for(choice in 1..3) {
                val current=store.load(connection)
                store.update(connection,current,current.withRoutes(mapOf(computer to WorkspaceRoute(computer,gateway.device.deviceId,"preferred-route",computer))))
                mode.set(choice);api.computers(connection)
                assertEquals(if(choice==3) "online-route" else "preferred-route",store.load(connection).routes.getValue(computer).routeId)
            }
        }
    }

    @Test fun ciphertextRetryKeepsExactEnvelopeAndReceiptRecoveryNeverPosts()=runBlocking {
        MockWebServer().use { server ->
            val saved=mutableListOf<String>();var outerOnly=true;var wrongRoute=false;var header:WorkspaceHeader?=null
            server.dispatcher=object:Dispatcher() {
                override fun dispatch(r:RecordedRequest):MockResponse {
                    if(r.path!!.startsWith("/v2/membership")) return MockResponse().setBody(history().toString())
                    if(r.method=="POST") {
                        val wire=r.body.readUtf8();saved+=wire
                        assertFalse(wire.contains("private message"));assertFalse(wire.contains("native-session"));assertFalse(wire.contains("expected_run_id"))
                        val envelope=WorkspaceEnvelope.parse(JSONObject(wire).getJSONObject("envelope"));header=envelope.header
                        val clear=JSONObject(String(envelope.open(gateway,phone.device,envelope.header)))
                        assertEquals("private message",clear.getJSONObject("payload").getString("text"))
                        assertEquals("native-session",clear.getString("session"))
                        assertEquals("machine-fixture",clear.getJSONObject("encrypted_transport").getString("machine_id"))
                        return MockResponse().setBody("""{"request_id":"request-one","state":"queued","envelope":null}""")
                    }
                    if(outerOnly) return MockResponse().setBody("""{"request_id":"request-one","state":"completed","envelope":null}""")
                    val request=checkNotNull(header)
                    val resultHeader=request.copy(senderDeviceId=gateway.device.deviceId,recipientDeviceId=phone.device.deviceId,
                        kind="result",routeId=if(wrongRoute) "substituted-route" else request.routeId)
                    val result=WorkspaceEnvelope.seal(gateway,phone.device,resultHeader,"""{"request_id":"request-one","state":"completed","result":{"status":"submitted"},"error":null}""".toByteArray())
                    return MockResponse().setBody(JSONObject().put("request_id","request-one").put("state","completed").put("envelope",result.json()).toString())
                }
            }
            server.start();val connection=Connection("connection-fixture","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            val store=MemoryWorkspaceJournal();store.create(connection,state());val api=RelayApi(workspace=store)
            val target=Target("machine-fixture","native-session","exact-run","exact-conversation",connection.id)
            val payload=target.json().put("text","private message")
            assertEquals("queued",api.submit(connection,target,"send",payload,"request-one").string("state"))
            val first=store.load(connection)
            store.update(connection,first,first.withRoutes(mapOf("machine-fixture" to WorkspaceRoute("machine-fixture",gateway.device.deviceId,"changed-catalog-route","machine-fixture"))))
            assertEquals("queued",api.submit(connection,target,"send",payload,"request-one").string("state"))
            assertEquals(saved[0],saved[1]);val restarted=RelayApi(workspace=store)
            assertEquals("uncertain",restarted.receipt(connection,"request-one").string("state"));assertEquals(2,saved.size)
            outerOnly=false;wrongRoute=true
            assertTrue(runCatching { restarted.receipt(connection,"request-one") }.isFailure);assertEquals(2,saved.size)
            wrongRoute=false
            assertEquals("completed",restarted.receipt(connection,"request-one").string("state"));assertEquals(2,saved.size)
            assertTrue(runCatching { restarted.submit(connection,target,"send",payload,"request-one") }.isFailure);assertEquals(2,saved.size)
        }
    }
    @Test fun missingProtectedStateRefusesBeforeAnyNetworkAndCannotUseV1()=runBlocking {
        MockWebServer().use { server ->
            server.start();val connection=Connection("missing","Fixture",server.url("/").toString().trimEnd('/'),"fixture-token",2)
            assertTrue(runCatching { RelayApi().submit(connection,Target("machine","private-session","run","conversation",connection.id),"send",JSONObject(),"request-one") }.isFailure)
            assertEquals(0,server.requestCount)
        }
    }
    @Test fun candidateAndBearerRemainStableAcrossExplicitEnrollmentResume()=runBlocking {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setResponseCode(503).setBody("{}"));server.enqueue(MockResponse().setBody("""{"state":"pending"}"""));server.start()
            val connection=Connection("pending","Fixture",server.url("/").toString().trimEnd('/'),"saved-service-bearer",2)
            val journal=MemoryWorkspaceJournal();journal.create(connection,state(true));val api=RelayApi(workspace=journal)
            assertTrue(runCatching { api.enrollPrepared(connection) }.isFailure)
            api.enrollPrepared(connection)
            val first=server.takeRequest();val second=server.takeRequest()
            assertEquals(first.body.readUtf8(),second.body.readUtf8());assertEquals("/v2/enrollments",second.path)
            val state=journal.load(connection)
            assertEquals(phone.device,state.identity.device);assertNull(state.membership)
            assertFalse(state.candidate.json().toString().contains(f.getJSONObject("invitation").getString("secret")))
        }
    }
}
