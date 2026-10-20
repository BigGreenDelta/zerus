package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.util.UUID

/** Connection generation is the immutable owning list's identity, not a saved credential. */
internal class SessionProjectHost(val target: Target, val connection: Connection, val machine: Machine,
    val navigation: Long, val connectionGeneration: Any) {
    val route = SessionProjectProtocol.fingerprint(connection,machine)
}
internal interface SessionProjectAccess {
    fun capture(target: Target): SessionProjectHost?
    fun current(captured: SessionProjectHost): Boolean
    fun reason(target: Target): String
    fun mutationReason(target: Target, ownRequest: String = ""): String
    fun actions(): List<SessionAction>
    fun flight(id: String, active: Boolean)
    suspend fun persist(action: SessionAction)
    suspend fun settle(action: SessionAction, receipt: JSONObject)
    fun refresh()
}
internal interface SessionProjectNetwork {
    /** Only the POST admission. Receipt polling is a separate transport phase. */
    suspend fun submit(connection:Connection,target:Target,operation:String,payload:JSONObject,id:String):JSONObject
    suspend fun await(connection:Connection,initial:JSONObject):JSONObject
    suspend fun receipt(connection:Connection,id:String):JSONObject
}
private class RelaySessionProjects(private val api:RelayApi):SessionProjectNetwork {
    override suspend fun submit(connection:Connection,target:Target,operation:String,payload:JSONObject,id:String)=
        api.submit(connection,target,operation,payload,id)
    override suspend fun await(connection:Connection,initial:JSONObject)=api.await(connection,initial)
    override suspend fun receipt(connection:Connection,id:String)=api.await(connection,api.receipt(connection,id))
}
/** Reads may be canceled. Once saved, one mutation is independent of dialog lifetime and never reposted. */
internal class SessionProjectController(private val host: SessionProjectAccess, private val scope: CoroutineScope, private val network: SessionProjectNetwork, private val clock:()->Long=System::nanoTime) {
    constructor(host:SessionProjectAccess,scope:CoroutineScope,api:RelayApi):this(host,scope,RelaySessionProjects(api))
    var target by mutableStateOf<Target?>(null); private set
    var catalog by mutableStateOf<SessionProjectCatalog?>(null); private set
    var loading by mutableStateOf(false); private set
    var active by mutableStateOf<Set<String>>(emptySet()); private set
    var notice by mutableStateOf(""); private set
    private var confirmed: SessionProjectHost? = null
    private var confirmedAt=0L
    private var generation=0L
    private var readJob: Job?=null
    val pending get() = host.actions().filter { it.operation==SessionProjectProtocol.assign && it.target==target && it.blocksSending }
    fun reason(target: Target) = host.reason(target)
    fun open(selected: Target) { target=selected; refresh() }
    fun dismiss() { ++generation;readJob?.cancel();readJob=null;target=null;catalog=null;confirmed=null;loading=false;notice="" }
    fun refresh() {
        val selected=target ?: return
        val version=++generation
        readJob?.cancel();catalog=null;confirmed=null;loading=false
        notice=host.reason(selected)
        if(notice.isNotBlank()) return
        val captured=host.capture(selected) ?: run { notice="The selected computer or connection changed.";return }
        loading=true
        readJob=scope.launch {
            try {
                val projects=ArrayList<SessionProjectChoice>()
                var first:SessionProjectCatalog?=null
                var cursor:String?=null
                val seen=HashSet<String>()
                do {
                    check(host.current(captured)) { "The selected session or connection changed." }
                    val id=UUID.randomUUID().toString()
                    val raw=network.await(captured.connection,network.submit(captured.connection,selected,SessionProjectProtocol.read,
                        SessionProjectProtocol.readPayload(selected,captured.machine.machineId,id,cursor),id))
                    val page=SessionProjectProtocol.catalog(raw,selected,captured.machine.machineId,id)
                    val original=first
                    check(original==null || original.swarmId==page.swarmId && original.currentProjectId==page.currentProjectId &&
                        original.membershipToken==page.membershipToken && original.epoch==page.epoch) { "Project choices changed during loading. Refresh to try again." }
                    if(first==null) first=page
                    check(page.projects.all { project -> projects.none { it.id==project.id } } && projects.size+page.projects.size<=5000)
                    projects+=page.projects;cursor=page.next
                    check(cursor==null || page.projects.size==100 && seen.add(cursor) && seen.size<50) { "Invalid or repeated project page." }
                    check(host.current(captured)) { "The selected session or connection changed." }
                } while(cursor!=null)
                if(version==generation && target==selected && host.current(captured)) {
                    catalog=checkNotNull(first).copy(projects=projects.toList(),next=null);confirmed=captured;confirmedAt=clock();notice=""
                }
            } catch(cancelled:CancellationException) { throw cancelled }
            catch(_:Exception) { if(version==generation && target==selected) notice="Could not confirm current project choices. Refresh to try again." }
            finally { if(version==generation) loading=false }
        }
    }
    fun canAssign(project: SessionProjectChoice): Boolean {
        val captured=confirmed ?: return false
        val catalog=catalog ?: return false
        return !loading && clock()-confirmedAt in 0..60_000_000_000L && target==captured.target && host.current(captured) && host.mutationReason(captured.target).isBlank() &&
            active.isEmpty() && pending.isEmpty() && project in catalog.projects
    }
    fun assign(project: SessionProjectChoice) {
        if(!canAssign(project)) { if(catalog!=null && clock()-confirmedAt !in 0..60_000_000_000L) notice="Project choices expired. Refresh and review before moving.";return }
        val captured=checkNotNull(confirmed)
        val choices=checkNotNull(catalog)
        val action=SessionAction(UUID.randomUUID().toString(),captured.target,SessionProjectProtocol.assign,
            SessionProjectProtocol.arguments(captured.machine.machineId,captured.route,choices,project).toString())
        active=active+action.requestId;host.flight(action.requestId,true);notice=""
        scope.launch {
            var posted=false
            var refused=false
            try {
                host.persist(action) // Exact scope, choice and UUID before the only POST.
                check(host.current(captured) && host.mutationReason(captured.target,action.requestId).isBlank()) { "The session or connection changed before sending." }
                val payload=SessionProjectProtocol.payload(action)
                posted=true
                val initial=try { network.submit(captured.connection,action.target,action.operation,payload,action.requestId) }
                    catch(error:RelayException) { refused=error.status in setOf(400,401,403,404,413,422,429);throw error }
                // An admitted POST stays uncertain if later GET/polling fails, including 4xx.
                val receipt=network.await(captured.connection,initial)
                host.settle(action,receipt)
                host.refresh()
                if(target==action.target) { catalog=null;confirmed=null;notice=host.actions().find { it.requestId==action.requestId }?.error.orEmpty().ifBlank { "Project grouping updated. Refresh for current choices." } }
            } catch(error:Exception) {
                withContext(NonCancellable) {
                    runCatching { host.persist(action.copy(status=if(!posted || refused) "failed" else "uncertain",
                        error=if(!posted || refused) "The project move was not sent or was refused. Refresh and review before another attempt." else SessionProjectProtocol.uncertain)) }
                    if(target==action.target) notice=if(posted && !refused) SessionProjectProtocol.uncertain else "The project move was not sent or was refused. Refresh and review before another attempt."
                }
                if(error is CancellationException) throw error
            } finally { active=active-action.requestId;host.flight(action.requestId,false) }
        }
    }
    fun check(action: SessionAction) {
        if(action.operation!=SessionProjectProtocol.assign || action.requestId in active || host.actions().none { it==action }) return
        val captured=host.capture(action.target) ?: return
        val args=runCatching { SessionProjectProtocol.arguments(JSONObject(action.arguments)) }.getOrNull() ?: return
        if(captured.machine.machineId!=args.string("expected_machine_id") || captured.route!=args.string("route_fingerprint")) {
            notice="The original connection or computer changed. This request has not been retried.";return
        }
        active=active+action.requestId;host.flight(action.requestId,true)
        scope.launch {
            try {
                // GET of the original receipt only. A later membership never settles this UUID.
                val result=network.receipt(captured.connection,action.requestId)
                host.settle(action,result);host.refresh()
                if(target==action.target) notice=host.actions().find { it.requestId==action.requestId }?.error.orEmpty()
            } catch(cancelled:CancellationException) { throw cancelled }
            catch(_:Exception) { if(target==action.target) notice="The original project receipt could not be checked. Nothing has been retried." }
            finally { active=active-action.requestId;host.flight(action.requestId,false) }
        }
    }
}
