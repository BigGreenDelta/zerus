package app.zerus.mobile

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import androidx.compose.runtime.mutableStateMapOf
import android.net.Uri
import java.util.UUID
import org.json.JSONArray
import org.json.JSONObject

class ZerusViewModel(application: Application) : AndroidViewModel(application) {
    private val store = PrivateStore(application)
    private val api = RelayApi()
    var connections by mutableStateOf<List<Connection>>(emptyList()); private set
    var machines by mutableStateOf<List<Machine>>(emptyList()); private set
    var sessions by mutableStateOf<List<Session>>(emptyList()); private set
    private var accountsWriter:OrderedStatePersistence<List<AccountCatalog>>?=null
    var accountCatalogs by mutableStateOf<List<AccountCatalog>>(emptyList()); private set
    var accountsError by mutableStateOf(""); private set
    val displayedAccounts get() = if(demo) previewAccounts else accountCatalogs
    private val previewAccounts by lazy { AccountSnapshots.demo(System.currentTimeMillis()/1000.0) }
    var projects by mutableStateOf<List<ProjectSummary>>(emptyList()); private set
    var projectWarnings by mutableStateOf<List<String>>(emptyList()); private set
    var selectedProject by mutableStateOf<ProjectSummary?>(null); private set
    var sessionProjectScope by mutableStateOf<ProjectSummary?>(null); private set
    var sessionFilter by mutableStateOf(SessionFilter.All); private set
    var sessionQuery by mutableStateOf(""); private set
    var selectedMachines by mutableStateOf<Set<MachineKey>>(emptySet()); private set
    var collapsedProjects by mutableStateOf<Set<String>>(emptySet()); private set
    var sessionListIndex = 0; private set
    var sessionListOffset = 0; private set
    var sessionScrollReset by mutableStateOf(0L); private set
    private var unscopedScroll = 0 to 0
    private var unscopedFilter = SessionFilter.All
    private var unscopedQuery = ""
    fun chooseSessionFilter(value: SessionFilter) { sessionFilter = value; resetSessionScroll() }
    fun changeSessionQuery(value: String) { sessionQuery = value; resetSessionScroll() }
    fun toggleMachine(key: MachineKey) { selectedMachines = if (key in selectedMachines) selectedMachines - key else selectedMachines + key; resetSessionScroll() }
    fun allMachines() { selectedMachines = emptySet(); resetSessionScroll() }
    fun toggleProject(key: ProjectKey) { collapsedProjects = if (key.key in collapsedProjects) collapsedProjects - key.key else collapsedProjects + key.key }
    fun saveSessionScroll(index: Int, offset: Int, scope: String, reset: Long) {
        if (scope != sessionProjectScope?.key?.key.orEmpty() || reset != sessionScrollReset) return
        sessionListIndex = index; sessionListOffset = offset
    }
    private fun resetSessionScroll(index: Int = 0, offset: Int = 0) { sessionListIndex = index; sessionListOffset = offset; sessionScrollReset++ }
    fun viewProjectSessions(project: ProjectSummary) {
        if (sessionProjectScope == null) { unscopedFilter = sessionFilter; unscopedQuery = sessionQuery; unscopedScroll = sessionListIndex to sessionListOffset }
        sessionProjectScope = project; sessionFilter = SessionFilter.All; sessionQuery = ""; resetSessionScroll()
    }
    fun clearSessionScope() {
        if (sessionProjectScope != null) { sessionProjectScope = null; sessionFilter = unscopedFilter; sessionQuery = unscopedQuery; resetSessionScroll(unscopedScroll.first, unscopedScroll.second) }
    }
    private var continuation by mutableStateOf<CompactContinuation?>(null)
    private data class ClearExpectation(val requestId: String, val target: Target, val navigation: Long, val confirmed: Boolean = false)
    private var clearExpectation: ClearExpectation? = null
    private var continuationEnqueuing = ""
    private var actionFlights by mutableStateOf<Set<String>>(emptySet())
    val sessionActions get() = messageState.sessionActions.map { if(it.requestId !in actionFlights) it.recover() else it }
    private var contextFlights by mutableStateOf<Set<String>>(emptySet())
    private val pendingSettingsAttempts = mutableSetOf<String>()
    private val scheduledReadScheduler=HistoryHeadScheduler()
    private val acknowledgedContexts = mutableSetOf<String>()
    private val contextObservations = mutableSetOf<String>()
    private var relayFeatures = emptyMap<String,Set<String>>()
    private var machineFeatures = emptyMap<MachineKey,Set<String>>()
    private var parentSelection:Session?=null
    private var relayOperations by mutableStateOf<Map<String, Set<String>>>(emptyMap())
    private var machineOperations by mutableStateOf<Map<MachineKey, Set<String>>>(emptyMap())
    private var contextNoticeValue by mutableStateOf("")
    private var contextNoticeOwner:String?=null
    var contextNotice:String
        get()=contextNoticeValue
        private set(value) { contextNoticeValue=value;contextNoticeOwner=null }
    private fun setContextNotice(requestId:String,text:String) { if(contextNoticeOwner == requestId) contextNoticeValue=text }
    val contextOperations get() = messageState.contextOperations.map { if (it.requestId !in contextFlights) it.recover() else it }
    val compactContinuationActive get() = continuation?.draft?.target == selected?.target && continuation != null
    fun contextOperation(target: Target) = contextOperations.lastOrNull { ContextPolicies.sameSessionRun(it.target, target) && it.blocksSending }
        ?: contextOperations.lastOrNull { ContextPolicies.sameSessionRun(it.target, target) }
    fun contextBlocked(target: Target) = sessionActions.any { it.target == target && it.blocksSending } || clearExpectation?.let { ContextPolicies.sameSessionRun(it.target, target) } == true || contextOperations.any { ContextPolicies.sameSessionRun(it.target, target) && it.blocksSending }
    private var messageState by mutableStateOf(MessageState())
    private val messageRepository = MessagePersistence.get(application)
    private var persistence: OrderedStatePersistence<MessageState>? = null
    private var readWriter:OrderedStatePersistence<ConversationReadState>?=null
    private var readState by mutableStateOf(ConversationReadState())
    private var readTrackingReady by mutableStateOf(false)
    var readPositionReady by mutableStateOf(false);private set
    var firstUnreadEventId by mutableStateOf("");private set
    private var unreadBoundaryCaptured=false
    // Marks submitted by this process; a failed submission is forgotten so the next view retries.
    private val sharedReadsSent=mutableSetOf<String>()
    fun conversationViewport(target:Target)=readState.record(target)?.viewport
    fun saveConversationViewport(target:Target,eventId:String,offset:Int) {
        if(selected?.target != target || activeFrame?.target != target || conversationEvents.none { it.id == eventId }) return
        val event=conversationEvents.find { it.id==eventId } ?: return
        readWriter?.let { readState=it.edit { state -> state.viewport(target,eventId,offset,event.historyCursor,event.historyEpoch) } }
        if(event.historyCursor.isNotBlank()) historyWindows[target.key]?.let { window ->
            viewportPageJob?.cancel();viewportPageJob=viewModelScope.launch {
                delay(250)
                runCatching {
                    val retained=withContext(Dispatchers.Default) { HistoryPage.retained(window,event.historyCursor) to HistoryPage.neighbors(window,event.historyCursor) }
                    retained.first?.let { pageCache.write(target,"around",event.historyCursor,it) }
                    retained.second.forEach { (direction,cursor,raw) -> pageCache.write(target,direction,cursor,raw) }
                }
            }
        }
    }
    fun visibleMessages(target:Target,eventIds:Set<String>) {
        if(selected?.target != target || activeFrame?.target != target) return
        val incoming=conversationEvents.filter { it.id in eventIds && ConversationReadPolicies.incoming(it) }
        if(incoming.isEmpty()) return
        readWriter?.let { readState=it.edit { state -> state.visible(target,incoming) } };refreshReadAttention();shareRead(target)
    }
    fun loadedUnreadCount(target:Target):Int? {
        val record=readState.record(target)
        if(!readTrackingReady) return null
        if(record?.incomplete == true || readState.records.size >= 500 && record == null) return null
        val rows=if(selected?.target==target) conversationEvents else historyWindows[target.key]?.events ?: frames[target.key]?.events
        return ConversationReadPolicies.lowerBound(record,rows)
    }
    fun unreadCount(target:Target):Int? {
        val record=readState.record(target) ?: return null
        if(!readTrackingReady || !record.headComplete || ConversationReadPolicies.key(target) !in currentHistoryHeads) return null
        return record.headIncoming?.let { (it-record.readThrough).coerceIn(0,Int.MAX_VALUE.toLong()).toInt() }
    }
    private fun captureUnreadBoundary(target:Target,events:List<Event>) {
        if(unreadBoundaryCaptured || !readPositionReady) return
        val decision=OpeningUnreadBoundary.decide(readTrackingReady,readState.record(target),events)
        if(!decision.captured) return
        firstUnreadEventId=decision.eventId
        unreadBoundaryCaptured=true
    }
    var olderLoading by mutableStateOf(false);private set
    var olderError by mutableStateOf("");private set
    var hasOlder by mutableStateOf(false);private set
    var hasNewer by mutableStateOf(false);private set
    var historyIndexing by mutableStateOf(false);private set
    var historyEpoch by mutableStateOf("");private set
    var historyComplete by mutableStateOf(false);private set
    private var lastHistoryRequest:Triple<Target,String,String>?=null
    fun retryHistory(target:Target) { lastHistoryRequest?.takeIf { it.first==target }?.let { loadHistory(target,it.second,it.third) } ?: loadHistory(target,"") }
    private val historyWindows=LinkedHashMap<String,HistoryWindow>(20,0.75f,true)
    private var currentHistoryHeads by mutableStateOf<Set<String>>(emptySet())
    private val historyFlights=mutableMapOf<String,Pair<Target,Deferred<HistoryPage>>>()
    private var olderJob:Job?=null
    private var viewportPageJob:Job?=null
    private var historyHeadJob:Job?=null
    private val historySnapshotHints=mutableMapOf<String,String>()
    private val historyHeadScheduler=HistoryHeadScheduler()
    private var restoreHistoryAttempt=""
    private var pendingHistoryPosition=""
    private var pendingHistoryPublication=""
    private var lastHistoryObservation=0L
    private fun historyNetworkReason(target:Target):String = when {
        target.run.isBlank() || target.conversation.isBlank() -> "History is still starting."
        connections.none { it.id==target.connectionId } -> "No connection. Showing saved conversation."
        target.connectionId !in relayOperations || MachineKey(target.connectionId,target.computerId) !in machineOperations -> "Connect to check earlier history availability."
        "history" !in relayOperations[target.connectionId].orEmpty() || "history" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty() -> "This computer does not support earlier history yet."
        else -> ""
    }
    fun historyReason(target:Target):String {
        if(target.run.isBlank() || target.conversation.isBlank() || connections.none { it.id==target.connectionId }) return historyNetworkReason(target)
        if(readState.record(target)?.epoch?.isNotBlank()==true || historyWindows.containsKey(target.key)) return ""
        return historyNetworkReason(target)
    }
    private suspend fun historyOnce(target:Target,direction:String,cursor:String,limit:Int=100):HistoryPage {
        val key=target.key+JSONArray(listOf(direction,cursor,limit))
        historyFlights[key]?.let { return it.second.await() }
        while(historyFlights.size>=4) { runCatching { historyFlights.values.first().second.await() };kotlinx.coroutines.currentCoroutineContext().ensureActive();historyFlights.entries.removeAll { it.value.second.isCompleted } }
        val connection=connections.find { it.id==target.connectionId } ?: error("Workspace disconnected.")
        val flight=viewModelScope.async {
            val reason=historyNetworkReason(target)
            if(reason.isNotBlank()) throw java.io.IOException(reason)
            val page=api.history(connection,target,direction,cursor,limit)
            viewModelScope.launch { runCatching { HistoryPageRetention.retain(limit,page,direction) { pageCache.write(target,direction,cursor,it.raw) } } }
            page
        }
        historyFlights[key]=target to flight
        flight.invokeOnCompletion { viewModelScope.launch { if(historyFlights[key]?.second===flight) historyFlights.remove(key) } }
        return flight.await()
    }
    private fun observeHistoryHead(page:HistoryPage,verified:Boolean=true) {
        if(connections.none { it.id==page.target.connectionId }) return
        readWriter?.let { readState=it.edit { state ->
            val next=state.head(page.target,page.epoch,page.latestIncoming,page.totalIncoming,page.complete)
            next.visible(page.target,page.events.filter { it.id in next.record(page.target)?.seen.orEmpty() })
        } }
        if(page.complete && verified) currentHistoryHeads+=ConversationReadPolicies.key(page.target) else currentHistoryHeads-=ConversationReadPolicies.key(page.target)
        refreshReadAttention()
    }
    private fun publishWindow(target:Target,window:HistoryWindow,complete:Boolean=false,indexing:Boolean=false,boundary:String?=null) {
        historyWindows[target.key]=window
        while(historyWindows.size>20) historyWindows.remove(historyWindows.keys.first())
        if(selected?.target != target) return
        if(!readPositionReady) pendingHistoryPosition=target.key
        pendingHistoryPublication=target.key
        val head=frames[target.key] ?: activeFrame?.takeIf { it.target==target } ?: return
        installFrame(head.copy(events=window.events,presentation=null,truncated=head.truncated || window.hasBefore || window.hasAfter,window=window,indexComplete=complete,indexing=indexing,unreadBoundary=boundary),activityVerified)
    }
    private fun loadHistory(target:Target,direction:String,cursor:String="") {
        if(selected?.target != target || olderLoading) return
        if(target.run.isBlank() || target.conversation.isBlank() || connections.none { it.id==target.connectionId }) { olderError=historyReason(target);return }
        val navigation=navigationId
        lastHistoryRequest=Triple(target,direction,cursor)
        olderLoading=true;olderError=""
        if(direction in setOf("","around","unread") && !unreadBoundaryCaptured) readPositionReady=false
        olderJob=viewModelScope.launch {
            var published=false
            try {
                val result=HistoryReadPaths.read(target,network={ historyOnce(target,direction,cursor) },cached={ pageCache.read(target,direction,cursor) })
                val page=result.page;val verifiedPage=result.verified
                if(!verifiedPage) currentHistoryHeads-=ConversationReadPolicies.key(target)
                val knownIds=readState.record(target)?.let { it.seen.toSet()+it.viewport?.eventId.orEmpty()+firstUnreadEventId }.orEmpty()
                val aliases=withContext(Dispatchers.Default) { RollingConversationHistory.merge(target,target,activeFrame?.takeIf { it.target==target }?.events.orEmpty(),page.events).supersededIds+HistoryAliases.exact(page.events,knownIds) }
                if(aliases.isNotEmpty()) readWriter?.let { readState=it.edit { state -> state.supersede(target,aliases) } }
                observeHistoryHead(page,verifiedPage)
                if(selected?.target != target || navigationId != navigation) return@launch
                if(direction=="" && verifiedPage && !page.complete) {
                    androidx.compose.runtime.snapshots.Snapshot.withMutableSnapshot {
                        historyIndexing=page.indexing;historyComplete=false;olderLoading=false;readPositionReady=true
                        captureUnreadBoundary(target,conversationEvents)
                    }
                    return@launch
                }
                val marker=readState.record(target)
                val earliest=page.events.filter(ConversationReadPolicies::incoming).mapNotNull { it.incomingSeq }.minOrNull()
                val saved=marker?.viewport
                val noSavedAnchor=saved==null || saved.epoch!=page.epoch || saved.cursor.isBlank()
                val unreadBeforePage=page.latestIncoming!=null && marker!=null && page.latestIncoming>marker.readThrough && page.hasBefore && (earliest==null || earliest>marker.readThrough+1)
                if(direction=="" && verifiedPage && page.complete && marker?.established==true && noSavedAnchor && unreadBeforePage && !unreadBoundaryCaptured) {
                    readPositionReady=false
                    val unread=historyOnce(target,"unread",JSONArray(listOf(page.epoch,marker!!.readThrough)).toString())
                    observeHistoryHead(unread)
                    if(selected?.target!=target || navigationId!=navigation) return@launch
                    publishWindow(target,HistoryWindow.from(unread),unread.complete,unread.indexing);published=true
                    lastHistoryObservation=activeFrame?.observedAt ?: 0L
                    return@launch
                }
                val old=historyWindows[target.key]
                val window=withContext(Dispatchers.Default) { if(direction in setOf("before","after") && old?.epoch==page.epoch) old.merge(page,direction) else HistoryWindow.from(page) }
                publishWindow(target,window,page.complete && verifiedPage,page.indexing,aliases[firstUnreadEventId]);lastHistoryObservation=activeFrame?.observedAt ?: 0L;published=true
            } catch(e:CancellationException) { throw e }
            catch(e:Exception) { if(selected?.target==target && navigationId==navigation) { olderError=ReadFailure.message(e,"conversation");currentHistoryHeads-=ConversationReadPolicies.key(target) } }
            finally { if(!published && selected?.target==target && navigationId==navigation) { olderLoading=false;if(pendingHistoryPosition!=target.key) readPositionReady=true } }
        }
    }
    fun loadOlder(target:Target) {
        val window=historyWindows[target.key]
        if(window==null) loadHistory(target,"") else if(window.hasBefore && window.before!=null) loadHistory(target,"before",window.before)
    }
    fun loadNewer(target:Target) { historyWindows[target.key]?.takeIf { it.hasAfter && it.after!=null }?.let { loadHistory(target,"after",it.after!!) } }
    fun jumpToLatest(target:Target) { loadHistory(target,"") }
    private fun restoreSavedWindow(target:Target,navigation:Long) {
        val saved=readState.record(target)?.viewport ?: return
        if(saved.cursor.isBlank()) return
        readPositionReady=false
        viewModelScope.launch {
            var published=false
            try {
                val page=HistoryReadPaths.saved(target) { pageCache.read(target,"around",saved.cursor) } ?: return@launch
                if(selected?.target==target && navigationId==navigation && conversationEvents.none { it.id==saved.eventId }) {
                    publishWindow(target,HistoryWindow.from(page));restoreHistoryAttempt=target.key+saved.cursor;published=true
                }
            } finally { if(!published && selected?.target==target && navigationId==navigation) readPositionReady=true }
        }
    }
    private fun watchHistoryHeads() {
        if(historyHeadJob?.isActive==true || !readTrackingReady) return
        val eligible=sessions.filter { row -> historyNetworkReason(row.target).isBlank() && SessionFilters.online(row,machines) }
        val candidates=ReadAllHeadCandidates.candidates(eligible,readState).mapNotNull { row ->
            val hint=ReadAllHeadCandidates.hint(row)
            val key=ConversationReadPolicies.key(row.target)
            if(historySnapshotHints[key]==hint && key in currentHistoryHeads) return@mapNotNull null
            currentHistoryHeads-=key
            row.target to hint
        }
        val chosen=historyHeadScheduler.choose(candidates.map { it.first },android.os.SystemClock.elapsedRealtime())
        if(chosen.isEmpty()) return
        historyHeadJob=viewModelScope.launch {
            chosen.forEach { target ->
                val result=runCatching {
                    val page=historyOnce(target,"","",1)
                    if(sessions.none { it.target==target } || historyNetworkReason(target).isNotBlank() ||
                        machines.none { it.connectionId==target.connectionId && it.id==target.computerId && it.online }) return@runCatching
                    observeHistoryHead(page)
                    if(selected?.target==target && historyWindows[target.key]==null) {
                        if(page.complete && !olderLoading) { restoreHistoryAttempt="";loadHistory(target,"") }
                        else if(page.indexing) historyIndexing=true
                    }
                    historySnapshotHints[ConversationReadPolicies.key(target)]=candidates.first { it.first==target }.second
                    adoptSharedReads();if(selected?.target==target) shareRead(target)
                }
                historyHeadScheduler.result(target,android.os.SystemClock.elapsedRealtime(),result.isSuccess)
            }
        }
    }
    private fun restoreHistory(target:Target) {
        if(historyNetworkReason(target).isNotBlank() || olderLoading) return
        val saved=readState.record(target)?.viewport
        val key=target.key+saved?.cursor.orEmpty()
        if(restoreHistoryAttempt==key) return
        restoreHistoryAttempt=key
        if(saved?.cursor?.isNotBlank()==true && conversationEvents.none { it.id==saved.eventId }) {
            readPositionReady=false;loadHistory(target,"around",saved.cursor)
        } else if(historyWindows[target.key]==null) loadHistory(target,"")
    }
    fun reviewLater(target:Target)=readState.record(target)?.reviewLater==true
    fun captureReadAttention(target:Target):ReadAttentionEvidence {
        val record=readState.record(target)
        val raw=if(selected?.target==target) activity else sessions.find { it.target==target }?.raw
        val fingerprint=HistoryHeadHints.readFingerprint(target,raw,record)
        val rows=if(selected?.target==target) conversationEvents else historyWindows[target.key]?.events ?: frames[target.key]?.events.orEmpty()
        return ReadAttentionEvidence(target,fingerprint,record?.epoch.orEmpty(),record?.headIncoming,
            readTrackingReady && record?.headComplete==true && ConversationReadPolicies.key(target) in currentHistoryHeads,
            rows.filter(ConversationReadPolicies::incoming).map { it.id }.toSet(),
            rows.filter { ConversationReadPolicies.incoming(it) && record?.epoch?.isNotBlank()==true && it.historyEpoch==record.epoch }.mapNotNull { it.incomingSeq }.maxOrNull())
    }
    fun markRead(target:Target,evidence:ReadAttentionEvidence) {
        if(evidence.target!=target || !readTrackingReady || captureReadAttention(target)!=evidence) { error="The displayed reply evidence changed. Review it before marking read.";return }
        readWriter?.let { readState=it.edit { state -> ReadAttentionPolicies.mark(state,evidence) } };refreshReadAttention();shareRead(target);flushDrafts()
    }
    private fun readAllEvidence(target:Target):ReadAttentionEvidence? {
        if(!readTrackingReady || readWriter==null || demo || target.run.isBlank() || target.conversation.isBlank() ||
            connections.none { it.id==target.connectionId } ||
            sessions.none { it.target==target } && (selected?.target!=target || activeFrame?.target!=target)) return null
        val captured=captureReadAttention(target)
        val row=sessions.find { it.target==target }
        val currentHint=row?.let(ReadAllHeadCandidates::hint)
        val observedHint=historySnapshotHints[ConversationReadPolicies.key(target)]
        val hintMatches=currentHint!=null && observedHint==currentHint
        val freshHead=captured.authoritative && hintMatches && historyNetworkReason(target).isBlank() &&
            machines.any { it.connectionId==target.connectionId && it.id==target.computerId && it.online }
        return captured.copy(fingerprint=captured.fingerprint+JSONArray(listOf(currentHint,observedHint)).toString(),authoritative=freshHead)
    }
    fun canMarkRead(target:Target):Boolean=ReadAllPolicies.available(readAllEvidence(target))
    fun markAllRead(targets:List<Target>):ReadAllResult {
        val distinct=targets.distinctBy(ConversationReadPolicies::key)
        val captured=distinct.map { ReadAllCapture(it,readAllEvidence(it)) }
        val current=distinct.map { ReadAllCapture(it,readAllEvidence(it)) }
        val writer=readWriter ?: return ReadAllResult(skipped=distinct.size)
        var result=ReadAllResult(skipped=distinct.size)
        readState=writer.edit { state -> ReadAllPolicies.apply(state,captured,current).also { result=it.result }.state }
        refreshReadAttention();writer.flushAsync()
        distinct.forEach { shareRead(it) }
        return result
    }
    fun reviewLater(target:Target,evidence:ReadAttentionEvidence) {
        if(evidence.target!=target || !readTrackingReady || captureReadAttention(target)!=evidence) { error="The displayed reply evidence changed. Review it again.";return }
        readWriter?.let { readState=it.edit { state -> state.review(target,true) } };refreshReadAttention();flushDrafts()
    }
    private fun refreshReadAttention() {
        sessions=sessions.map { row ->
            val later=reviewLater(row.target)
            // A reply or request read on another device is read here too.
            val unread=!SharedReadPolicies.replyRead(row) && (unreadCount(row.target) ?: loadedUnreadCount(row.target) ?: 0)>0
            val acknowledged=SharedReadPolicies.attentionAcknowledged(row)
            if(row.raw.optBoolean("mobile_review_later")==later && row.raw.optBoolean("mobile_unread_reply")==unread &&
                row.raw.optBoolean("attention_acknowledged")==acknowledged) row
            else row.copy(raw=JSONObject(row.raw.toString()).put("mobile_review_later",later).put("mobile_unread_reply",unread)
                .put("attention_acknowledged",acknowledged))
        }
    }
    /** Tell the session's computer that this phone showed its current reply (C4 `mark_read`). */
    private fun shareRead(target:Target) {
        if(demo || !readTrackingReady) return
        val row=sessions.find { it.target==target } ?: return
        if("mark_read" !in relayOperations[target.connectionId].orEmpty() ||
            "mark_read" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) return
        val payload=SharedReadPolicies.payload(row,readAllEvidence(target)?.authoritative==true,unreadCount(target)) ?: return
        val connection=connections.find { it.id==target.connectionId } ?: return
        val key=target.key+"\n"+payload
        if(!sharedReadsSent.add(key)) return
        viewModelScope.launch {
            val completed=try { api.markRead(connection,target,payload).string("state")=="completed" }
                catch(e:Exception) { if(e is CancellationException) { sharedReadsSent.remove(key);throw e };false }
            if(!completed) sharedReadsSent.remove(key)
        }
    }
    /** A reply read on another device advances this phone's watermark, using only a fresh exact head. */
    private fun adoptSharedReads() {
        val writer=readWriter ?: return
        if(demo || !readTrackingReady) return
        val captured=sessions.filter { SharedReadPolicies.replyRead(it) && (unreadCount(it.target) ?: 0)>0 }
            .mapNotNull { row -> readAllEvidence(row.target)?.takeIf { it.authoritative }?.let { ReadAllCapture(row.target,it) } }
        if(captured.isEmpty()) return
        readState=writer.edit { state -> ReadAllPolicies.apply(state,captured,captured).state }
        refreshReadAttention()
    }
    private var preparingTargets by mutableStateOf<Set<String>>(emptySet())
    var attachmentPreparing by mutableStateOf(false); private set
    private var lastOwnSend by mutableStateOf<Pair<Target, String>?>(null)
    fun ownSendId(target: Target): String = lastOwnSend?.takeIf { it.first == target || it.first.resolvesTo(target) }?.second.orEmpty()
    var conversationEvents by mutableStateOf<List<Event>>(emptyList()); private set
    var conversationQuestions by mutableStateOf<List<Question>>(emptyList()); private set
    var conversationOutgoing by mutableStateOf<List<OutgoingMessage>>(emptyList()); private set
    var activityVerified by mutableStateOf(false); private set
    var historyCached by mutableStateOf(false); private set
    var conversationHistoryTruncated by mutableStateOf(false); private set
    private data class Frame(val target: Target, val raw: JSONObject, val events: List<Event>, val questions: List<Question>, val observedAt: Long, val bytes: Int, val truncated: Boolean = false, var presentation: Presentation? = null,val window:HistoryWindow?=null,val indexComplete:Boolean=false,val indexing:Boolean=false,val unreadBoundary:String?=null,val inspectedEvents:List<Event> = emptyList())
    private data class Presentation(val messages: List<OutgoingMessage>, val events: List<Event>, val questions: List<Question>)
    private val pageCache=ConversationPageCache(application)
    private val historyCache = ConversationCache(application)
    private val frames = LinkedHashMap<String, Frame>(24, 0.75f, true)
    private val inspections = mutableMapOf<String, Pair<Target, Deferred<Frame>>>()
    private var presentationJob: Job? = null
    private var presentationRevision = 0L
    private var activeFrame: Frame? = null
    private var activeFrameVerified = false
    val drafts get() = messageState.drafts
    val terminalBuffers get()=messageState.terminalBuffers
    private var deliveryFlights by mutableStateOf<Set<String>>(emptySet())
    private var normalizedOutgoing by mutableStateOf<List<OutgoingMessage>>(emptyList())
    val outgoing get() = normalizedOutgoing
    private val emptyDrafts = mutableMapOf<Triple<Target, String, String>, Draft>()
    val pickerDraft get() = messageState.pickerDraft
    val pickerId get() = messageState.pickerId
    var attachmentImporting by mutableStateOf(false); private set
    private val attachmentStore = AttachmentStore(application)
    private var importJob: Job? = null
    var selected by mutableStateOf<Session?>(null); private set
    var activity by mutableStateOf<JSONObject?>(null); private set
    var busy by mutableStateOf(false); private set
    var detailBusy by mutableStateOf(false); private set
    var catalogBusy by mutableStateOf(false); private set
    var catalogProgress by mutableStateOf(false); private set
    var detailProgress by mutableStateOf(false); private set
    private val catalogGate = LoadingGate(android.os.SystemClock::elapsedRealtime)
    private val detailGate = LoadingGate(android.os.SystemClock::elapsedRealtime)
    private var catalogJob: Job? = null
    private var detailJob: Job? = null
    private var catalogTimer: Job? = null
    private var detailTimer: Job? = null
    private var navigationId = 0L
    private var catalogErrorOwner: String? = null
    private var detailReadErrorOwner: Pair<Target,Long>? = null
    private var errorValue by mutableStateOf("")
    var error: String
        get() = errorValue
        private set(value) { errorValue = value; catalogErrorOwner = null;detailReadErrorOwner=null }
    var demo by mutableStateOf(false); private set
    var storageReady by mutableStateOf(false); private set
    var receiptFlights by mutableStateOf<Set<String>>(emptySet()); private set
    var notifications by mutableStateOf(store.notificationEnabled()); private set
    var pushStatuses by mutableStateOf<Map<String, String>>(emptyMap()); private set
    private var pendingNotification: Triple<String, String, String>? = null
    init {
        viewModelScope.launch {
            try {
                connections = withContext(Dispatchers.IO) { store.connections() }
                persistence = messageRepository.open()
                accountsWriter=AccountsPersistence.get(application).open()
                accountsWriter!!.edit { rows -> rows.filter { row -> connections.any { it.id==row.key.connectionId } } }
                accountCatalogs=accountsWriter!!.state.value
                launch { accountsWriter!!.state.collectLatest { accountCatalogs=it } }
                launch { AccountsPersistence.get(application).error.collectLatest { failure -> accountsError=if(failure!=null) "Account snapshots could not be saved. Existing private data is preserved." else "" } }
                try {
                    readWriter=ConversationReadPersistence.get(application).open();readState=readWriter!!.state.value;readTrackingReady=true
                    launch { readWriter!!.state.collectLatest { readState=it;refreshReadAttention() } }
                    launch { ConversationReadPersistence.get(application).error.collectLatest { failure -> if(failure!=null) error="Could not save private reading positions. They remain available here; restart may restore the earlier saved position." } }
                } catch(_:Exception) { error="Private read positions could not be opened. They have been preserved; unread totals remain unknown." }
                readPositionReady=true
                selected?.target?.let { restoreSavedWindow(it,navigationId) }
                activeFrame?.takeIf { selected?.target == it.target }?.presentation?.let { captureUnreadBoundary(selected!!.target,it.events) }
                updateMessageState(persistence!!.state.value)
                syncOutgoing()
                val indexed=messageState.conversationIndex.filter { row -> connections.any { it.id == row.target.connectionId } }
                sessions=indexed.map(IndexedConversation::session)
                refreshReadAttention()
                machines=indexed.distinctBy { MachineKey(it.target.connectionId,it.target.computerId) }.map { MachineNames.apply(Machine(it.target.connectionId,it.target.computerId,it.computerName,false,lastKnown=true),messageState.machineAliases) }
                storageReady = true
                if (connections.isNotEmpty()) refresh()
                launch { while(true) { delay(10_000);runCatching { watchScheduledSettings() } } }
                launch { persistence!!.state.collectLatest { updateMessageState(it) } }
                launch { messageRepository.error.collectLatest { failure ->
                    if (failure != null) error = "Could not save private changes. Your text remains available here; a message is sent only after its outgoing record is saved."
                } }
            } catch (e: Exception) { AppTelemetry.failure(DiagnosticFailure.PrivateStorage); error = "Private storage could not be opened. Existing data has been preserved. ${e.message.orEmpty()}" }
        }
    }
    private fun updateMessageState(value: MessageState) {
        val changed = messageState.outgoing != value.outgoing || messageState.drafts.filter { it.status == "submitted" && it.questionId.isNotBlank() } != value.drafts.filter { it.status == "submitted" && it.questionId.isNotBlank() }
        val namesChanged = messageState.machineAliases != value.machineAliases
        messageState = value
        if(namesChanged) machines = machines.map { MachineNames.apply(it,value.machineAliases) }
        continuation?.let { intent ->
            if (continuationEnqueuing != intent.requestId && !intent.unchanged(draft(intent.draft.target))) cancelCompactContinuation("Draft changed. Compaction may finish, but your message has not been sent.")
        }
        if (changed) syncOutgoing()
    }
    private fun syncOutgoing() {
        normalizedOutgoing = messageState.outgoing.map { if (it.status == "sending" && it.requestId !in deliveryFlights) it.recover() else it }
        preparePresentation()
    }
    private fun editState(reduce: (MessageState) -> MessageState) {
        updateMessageState(checkNotNull(persistence) { "Private storage is not ready." }.edit(reduce))
    }
    private suspend fun durable(reduce: (MessageState) -> MessageState,
        acknowledge: (MessageState, MessageState) -> MessageState = { current, _ -> reduce(current) }): MessageState {
        val writer = checkNotNull(persistence) { "Private storage is not ready." }
        val written = writer.durable(reduce, acknowledge)
        updateMessageState(writer.state.value)
        return written
    }
    var machineNameSaving by mutableStateOf<Set<MachineKey>>(emptySet()); private set
    fun machineName(connectionId:String, id:String, fallback:String=""):String = machines.find { it.connectionId==connectionId && it.id==id }?.name
        ?: messageState.machineAliases.find { it.key==MachineKey(connectionId,id) }?.name ?: fallback
    fun machineAlias(key:MachineKey)=messageState.machineAliases.find { it.key==key }?.name.orEmpty()
    fun renameMachine(key:MachineKey,name:String?,onSaved:()->Unit={}) {
        if(!storageReady || key in machineNameSaving || demo) return
        if(machines.none { MachineKey(it.connectionId,it.id)==key } || connections.none { it.id==key.connectionId }) { error="This machine is no longer connected to this workspace.";return }
        val checked=try { name?.let(MachineNames::checked) } catch(e:IllegalArgumentException) { error=e.message.orEmpty();return }
        machineNameSaving=machineNameSaving+key
        viewModelScope.launch {
            try {
                durable({ state -> check(connections.any { it.id==key.connectionId }) { "Workspace disconnected before saving the name." };MachineNames.set(state,key,checked) },
                    { current,_ -> MachineNames.set(current,key,checked) })
                onSaved()
            } catch(e:Exception) { error="Could not save the machine name. ${e.message.orEmpty()}" }
            finally { machineNameSaving=machineNameSaving-key }
        }
    }
    var machineColorSaving by mutableStateOf<Set<MachineKey>>(emptySet()); private set
    fun machineColor(key:MachineKey):String = MachineColors.color(messageState,key)
    fun machineColorOverride(key:MachineKey):String? = MachineColors.overrideHex(messageState,key)
    fun setMachineColor(key:MachineKey,hex:String?,onSaved:()->Unit={}) {
        if(!storageReady || key in machineColorSaving || demo) return
        if(machines.none { MachineKey(it.connectionId,it.id)==key } || connections.none { it.id==key.connectionId }) { error="This machine is no longer connected to this workspace.";return }
        val checked=try { hex?.let(MachineColors::checked) } catch(e:IllegalArgumentException) { error=e.message.orEmpty();return }
        machineColorSaving=machineColorSaving+key
        viewModelScope.launch {
            try {
                durable({ state -> check(connections.any { it.id==key.connectionId }) { "Workspace disconnected before saving the color." };MachineColors.set(state,key,checked) },
                    { current,_ -> MachineColors.set(current,key,checked) })
                onSaved()
            } catch(e:Exception) { error="Could not save the machine color. ${e.message.orEmpty()}" }
            finally { machineColorSaving=machineColorSaving-key }
        }
    }
    fun flushDrafts() { persistence?.flushAsync();readWriter?.flushAsync();accountsWriter?.flushAsync() }
    var updateInstallPreparing by mutableStateOf(false); private set
    fun updateInstallReason(): String = UpdateInstallGuard.reason(messageState,storageReady,
        attachmentImporting || attachmentPreparing,preparingTargets.isNotEmpty(),
        deliveryFlights.isNotEmpty() || contextFlights.isNotEmpty() || actionFlights.isNotEmpty(),compactContinuationActive)
    suspend fun prepareUpdateInstall() {
        check(!updateInstallPreparing && updateInstallReason().isBlank()) { updateInstallReason().ifBlank { "Update preparation is already running." } }
        updateInstallPreparing = true
        try {
            persistence!!.flush();readWriter?.flush()
            check(updateInstallReason().isBlank()) { updateInstallReason() }
        } catch(e:Throwable) { updateInstallPreparing=false;throw e }
    }
    fun finishUpdateInstallPreparation() { updateInstallPreparing=false }
    private fun rememberFrame(frame: Frame) {
        frames[frame.target.key] = frame
        while (frames.size > 20 || frames.values.sumOf { it.bytes.toLong() } > 20L * 1024 * 1024) frames.remove(frames.keys.first())
    }
    private fun installFrame(incoming: Frame, verified: Boolean) {
        val window=historyWindows[incoming.target.key]
        val priorInspection=activeFrame?.takeIf { previous -> previous.target==incoming.target }?.inspectedEvents.orEmpty()
        val changed=window!=null && incoming.window!=window && HistoryInspection.changed(window,incoming.inspectedEvents,priorInspection)
        if(changed) currentHistoryHeads-=ConversationReadPolicies.key(incoming.target)
        val guarded=window?.let { HistoryInspection.retain(it,incoming.window,incoming.inspectedEvents,priorInspection) }
        val frame=if(guarded!=null && (incoming.window!=guarded || incoming.events!=guarded.events)) incoming.copy(events=guarded.events,presentation=null,truncated=incoming.truncated || guarded.hasBefore || guarded.hasAfter,window=guarded,indexComplete=historyComplete && !changed,indexing=historyIndexing) else incoming
        if(guarded!=null) historyWindows[incoming.target.key]=guarded
        activeFrame = frame; activeFrameVerified = verified
        if (frame.presentation == null) { activityVerified = false; historyCached = activity != null }
        frame.presentation?.let { prepared ->
            androidx.compose.runtime.snapshots.Snapshot.withMutableSnapshot {
                conversationEvents = prepared.events; conversationOutgoing = prepared.messages; conversationQuestions = prepared.questions
                activity = frame.raw; activityVerified = verified; historyCached = !verified; conversationHistoryTruncated = frame.truncated
                publishPagingMetadata(frame)
                captureUnreadBoundary(frame.target,prepared.events)
            }
        }
        preparePresentation()
    }
    private fun publishPagingMetadata(frame:Frame) {
        frame.unreadBoundary?.let { firstUnreadEventId=it }
        frame.window?.let { window -> hasOlder=window.hasBefore;hasNewer=window.hasAfter;historyEpoch=window.epoch;historyComplete=frame.indexComplete;historyIndexing=frame.indexing }
        if(pendingHistoryPublication==frame.target.key) { pendingHistoryPublication="";olderLoading=false;readPositionReady=true }
    }
    private fun preparePresentation() {
        val frame = activeFrame ?: return
        val target = selected?.target ?: return
        if (frame.target != target) return
        val messages = outgoing.filter { it.belongsTo(target) }
        val questionState = messageState
        val version = ++presentationRevision
        presentationJob?.cancel()
        presentationJob = viewModelScope.launch {
            val (merged, questions) = withContext(Dispatchers.Default) {
                (if (target.archiveId.isBlank()) ConversationMerge.merge(target, frame.events, messages)
                else ConversationMerge.Result(frame.events, emptySet())) to QuestionTracking.restore(questionState, target, frame.questions)
            }
            if (version != presentationRevision || selected?.target != target || activeFrame !== frame) return@launch
            frame.presentation = Presentation(messages, merged.events, questions)
            androidx.compose.runtime.snapshots.Snapshot.withMutableSnapshot {
                if (conversationQuestions != questions) conversationQuestions = questions
                if (conversationEvents != merged.events) conversationEvents = merged.events
                if (conversationOutgoing != messages) conversationOutgoing = messages
                activity = frame.raw; activityVerified = activeFrameVerified; historyCached = !activeFrameVerified; conversationHistoryTruncated = frame.truncated
                publishPagingMetadata(frame)
                if(pendingHistoryPosition==frame.target.key) { pendingHistoryPosition="";readPositionReady=true }
                captureUnreadBoundary(frame.target,merged.events)
            }
            if (activityVerified) {
                if(frame.raw.optJSONArray("pending_questions")!=null) editState { state -> QuestionTracking.reconcile(state,frame.target,frame.questions) }
                val tracked = QuestionTracking.remember(messageState, frame.target, frame.questions)
                if (tracked != messageState) editState { state -> QuestionTracking.remember(state, frame.target, frame.questions) }
                observeContext(frame)
                observeScheduledSettings(frame)
                restoreHistory(frame.target)
                val window=historyWindows[frame.target.key]
                if(window?.atHead==true && !olderLoading && frame.observedAt>0 && frame.observedAt!=lastHistoryObservation) {
                    lastHistoryObservation=frame.observedAt
                    window.events.lastOrNull { it.historyCursor.isNotBlank() }?.historyCursor?.takeIf { it.isNotBlank() }?.let { loadHistory(frame.target,"after",it) }
                }
            }
            if (activityVerified && messages.any { it.requestId in merged.recordedRequestIds && it.status != "recorded" }) {
                editState { state -> state.copy(outgoing = state.outgoing.map { if (it.requestId in merged.recordedRequestIds && it.status != "reviewed") it.copy(status = "recorded", error = "") else it }) }
            }
        }
    }
    private suspend fun inspectOnce(connection: Connection, target: Target): Deferred<Frame> {
        while (inspections.size >= 4 && inspections[target.key] == null) {
            runCatching { inspections.values.first().second.await() }
            kotlinx.coroutines.currentCoroutineContext().ensureActive()
            inspections.entries.removeAll { it.value.second.isCompleted }
        }
        inspections[target.key]?.let { return it.second }
        val observedAt = System.currentTimeMillis()
        val task = viewModelScope.async {
            val raw = api.inspect(connection, target)
            val resolved = if (target.archiveId.isNotBlank()) target else target.copy(run = raw.string("run_id"), conversation = raw.string("conversation_id"))
            val previous = frames[resolved.key]
            val currentWindow=historyWindows[resolved.key]
            val (frame,aliases,nextWindow)=withContext(Dispatchers.Default) {
                val incoming=NativeParser.events(raw)
                val retained=RollingConversationHistory.merge(resolved,previous?.target,previous?.events.orEmpty(),incoming)
                val window=currentWindow?.takeIf { it.atHead }?.inspection(incoming,previous?.inspectedEvents.orEmpty())
                val events=window?.events ?: retained.events
                val nativeUnknown=currentWindow?.let { HistoryInspection.changed(it,incoming,previous?.inspectedEvents.orEmpty()) }==true
                Triple(Frame(resolved,raw,events,NativeParser.questions(raw),observedAt,
                    raw.toString().toByteArray(Charsets.UTF_8).size+EventHistoryCodec.encode(events).toString().toByteArray(Charsets.UTF_8).size,
                    retained.truncated || previous?.truncated==true || raw.optBoolean("mobile_history_truncated"),window=window,indexComplete=historyComplete && !nativeUnknown,indexing=historyIndexing,inspectedEvents=incoming),retained.supersededIds,window)
            }
            // A page can publish during Default parsing. Rebase before mutating the
            // window map, so the older inspection snapshot cannot erase that page.
            val rebased=HistoryInspection.rebase(currentWindow,frame to nextWindow,{ historyWindows[resolved.key] }) { latest ->
                val complete=historyComplete;val indexing=historyIndexing
                val guard=withContext(Dispatchers.Default) {
                    latest?.let { HistoryInspection.retain(it,null,frame.inspectedEvents,previous?.inspectedEvents.orEmpty()) }
                }
                val changed=latest?.let { HistoryInspection.changed(it,frame.inspectedEvents,previous?.inspectedEvents.orEmpty()) }==true
                frame.copy(events=guard?.events ?: frame.events,window=guard,presentation=null,indexComplete=complete && !changed,indexing=indexing) to guard
            }
            val expectedWindow=rebased.base
            val (finalFrame,finalWindow)=rebased.value
            if (connections.none { it.id == connection.id }) throw CancellationException("Workspace disconnected.")
            if(finalWindow!=null) {
                historyWindows[resolved.key]=finalWindow
                if(expectedWindow?.let { HistoryInspection.changed(it,frame.inspectedEvents,previous?.inspectedEvents.orEmpty()) }==true) currentHistoryHeads-=ConversationReadPolicies.key(resolved)
            }
            if(aliases.isNotEmpty()) readWriter?.let { writer -> readState=writer.edit { state -> state.supersede(resolved,aliases) } }
            rememberFrame(finalFrame)
            viewModelScope.launch { runCatching {
                val cached = withContext(Dispatchers.Default) { ConversationDisplayCache.encode(resolved, raw, finalFrame.events, finalFrame.truncated) }
                historyCache.write(resolved, cached, observedAt)
            } }
            finalFrame
        }
        inspections[target.key] = target to task
        task.invokeOnCompletion { viewModelScope.launch { if (inspections[target.key]?.second === task) inspections.remove(target.key) } }
        return task
    }
    private fun authorizedSettingsReason(target:Target,args:JSONObject,authorized:SessionAction,frame:Frame?):String {
        if(updateInstallPreparing) return "Saving drafts for an app update."
        val raw=frame?.raw ?: return "No fresh settings inspection."
        if(frame.target != target || target.agentId.isNotBlank() || target.archiveId.isNotBlank() || authorized.operation != "settings" || authorized.status != "scheduled" || authorized.applyAttemptId.isNotBlank() ||
            authorized.target.connectionId != target.connectionId || authorized.target.computerId != target.computerId || authorized.target.session != target.session || authorized.target.conversation != target.conversation ||
            raw.string("name") != target.session || raw.string("run_id") != target.run || raw.string("conversation_id") != target.conversation ||
            raw.string("pending_settings_id") != authorized.pendingId || args.string("expected_pending_id") != authorized.pendingId || raw.string("settings_apply_when") != "now" || raw.string("phase") != "idle" || raw.string("activity") != "idle") return "The authorized settings identity changed."
        if("settings" !in relayOperations[target.connectionId].orEmpty() || "settings" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty() || contextBlocked(target) || target.key in preparingTargets || outgoing.any { it.belongsTo(target) && it.blocksSending }) return "Resolve pending delivery first."
        return SessionActionPolicies.inspectReason(target,"settings",args,raw)
    }
    private suspend fun watchScheduledSettings() {
        val authorized=messageState.sessionActions.filter { it.operation == "settings" && it.status == "scheduled" && it.applyAttemptId.isBlank() && it.pendingId.isNotBlank() &&
            runCatching { JSONObject(it.arguments).string("expected_pending_id").isBlank() }.getOrDefault(false) }
        if(authorized.isEmpty()) return
        val candidates=mutableListOf<Triple<Connection,SessionAction,Target>>()
        for(connection in connections.filter { c -> authorized.any { it.target.connectionId==c.id } }) {
            val computers=runCatching { api.computers(connection).optJSONArray("computers")?.objects().orEmpty() }.getOrNull() ?: continue
            for(intent in authorized.filter { it.target.connectionId==connection.id }) {
                val computer=computers.find { it.string("id")==intent.target.computerId && it.optBoolean("online") } ?: continue
                val row=computer.optJSONObject("snapshot")?.optJSONArray("sessions")?.objects().orEmpty().find { it.string("name")==intent.target.session && it.string("conversation_id")==intent.target.conversation && it.string("archive_id").isBlank() } ?: continue
                if(row.string("activity")!="idle" || row.string("phase")!="idle") continue
                candidates+=Triple(connection,intent,intent.target.copy(run=row.string("run_id")))
            }
        }
        val chosen=scheduledReadScheduler.choose(candidates.map { it.third },android.os.SystemClock.elapsedRealtime())
        for(target in chosen) {
            val (connection,intent,_)=candidates.first { it.third==target }
            val frame=runCatching { inspectOnce(connection,target).await() }
            scheduledReadScheduler.result(target,android.os.SystemClock.elapsedRealtime(),frame.isSuccess)
            val inspected=frame.getOrNull() ?: continue
            if(inspected.raw.string("pending_settings_id")!=intent.pendingId) {
                editState { state -> state.sessionActions.find { it.requestId==intent.requestId && it.applyAttemptId.isBlank() }?.let { state.action(it.copy(status="cancelled",error="Native pending settings changed; the old intent was not applied.")) } ?: state }
            }
            observeScheduledSettings(inspected)
        }
    }
    private fun observeScheduledSettings(frame: Frame) {
        val raw=frame.raw; val pending=raw.string("pending_settings_id")
        if(pending.isBlank() || raw.string("settings_apply_when") != "now" || raw.string("activity") != "idle" || raw.string("phase") != "idle") return
        val key=frame.target.key + ":" + pending
        if(key in pendingSettingsAttempts) return
        val authorized=messageState.sessionActions.lastOrNull { action -> action.operation == "settings" && action.status == "scheduled" && action.applyAttemptId.isBlank() && action.pendingId == pending &&
            runCatching { JSONObject(action.arguments).string("expected_pending_id").isBlank() }.getOrDefault(false) &&
            action.target.connectionId == frame.target.connectionId && action.target.computerId == frame.target.computerId && action.target.session == frame.target.session && action.target.conversation == frame.target.conversation } ?: return
        if(messageState.sessionActions.any { action -> action.operation == "settings" && action.target.connectionId == frame.target.connectionId &&
            action.target.computerId == frame.target.computerId && action.target.session == frame.target.session && action.target.conversation == frame.target.conversation &&
            runCatching { JSONObject(action.arguments).string("expected_pending_id") == pending }.getOrDefault(false) }) return
        val args=JSONObject(authorized.arguments)
        if(listOf("model","effort").any { args.has(it) && raw.string("pending_" + it) != args.string(it) }) return
        args.put("expected_pending_id",pending)
        if(authorizedSettingsReason(frame.target,args,authorized,frame).isNotBlank()) return
        pendingSettingsAttempts += key
        submitSessionAction(frame.target,"settings",args,authorized,frame)
    }
    var launchMachine by mutableStateOf<MachineKey?>(null);private set
    var launchVisible by mutableStateOf(false);private set
    var launchCatalogState by mutableStateOf<JSONObject?>(null);private set
    var launchDirectoryState by mutableStateOf<JSONObject?>(null);private set
    var launchWorktreeState by mutableStateOf<JSONObject?>(null);private set
    var launchWorktreeLoading by mutableStateOf(false);private set
    var worktreeSubmitting by mutableStateOf(false);private set
    private var launchWorktreeJob:Job?=null
    var launchCatalogLoading by mutableStateOf(false);private set
    var launchDirectoryLoading by mutableStateOf(false);private set
    var launchError by mutableStateOf("");private set
    var launchSubmitting by mutableStateOf(false);private set
    private var launchGeneration=0L
    private var launchCatalogJob:Job?=null
    private var launchDirectoryJob:Job?=null
    private var launchCatalogEvidence=""
    fun launchCatalogSignature()=launchCatalogEvidence
    fun launchReason(machine:MachineKey):String {
        if(updateInstallPreparing) return "Saving drafts for an app update."
        if(!storageReady || demo || connections.none { it.id == machine.connectionId }) return "Connect to this workspace before creating a session."
        if(machines.none { it.connectionId == machine.connectionId && it.id == machine.computerId && it.online }) return "Refresh the computer list; this computer is offline or last known."
        if(listOf("catalog","dirs","launch").any { it !in relayOperations[machine.connectionId].orEmpty() || it !in machineOperations[machine].orEmpty() }) return "This computer does not support safe mobile session creation."
        if(sessionActions.any { it.operation == "launch" && it.target.connectionId == machine.connectionId && it.target.computerId == machine.computerId && it.blocksSending }) return "Check the previous launch receipt before creating another session."
        return ""
    }
    fun prepareLaunch(machine:MachineKey) {
        val reason=launchReason(machine);if(reason.isNotBlank()) { launchError=reason;return }
        launchGeneration++;launchMachine=machine;launchVisible=true;launchCatalogState=null;launchDirectoryState=null;launchWorktreeState=null;launchError="";launchCatalogEvidence=""
        launchCatalogJob?.cancel();launchDirectoryJob?.cancel();launchWorktreeJob?.cancel();launchWorktreeLoading=false;launchDirectoryLoading=false
        val connection=connections.find { it.id == machine.connectionId } ?: return
        val generation=launchGeneration;launchCatalogLoading=true
        launchCatalogJob=viewModelScope.launch {
            try {
                val target=Target(machine.computerId,"","","",machine.connectionId);val id=UUID.randomUUID().toString()
                val receipt=api.await(connection,api.submit(connection,target,"catalog",JSONObject().put("request_id",id),id))
                check(receipt.string("state") == "completed") { receipt.string("error").ifBlank { "The native account catalog is unavailable." } }
                val raw=receipt.getJSONObject("result")
                val sanitized=withContext(Dispatchers.Default) { LaunchPresentation.sanitizeCatalog(raw) }
                if(launchGeneration == generation && launchMachine == machine) { launchCatalogState=sanitized;launchCatalogEvidence=sanitized.toString() }
            } catch(e:Exception) { if(e is CancellationException) throw e;if(launchGeneration == generation) launchError=e.message.orEmpty() }
            finally { if(launchGeneration == generation) launchCatalogLoading=false }
        }
        browseLaunchDirectory(machine,"~")
    }
    fun closeLaunch() { if(worktreeSubmitting) return;launchWorktreeJob?.cancel();launchWorktreeLoading=false;launchVisible=false;launchGeneration++;launchCatalogJob?.cancel();launchDirectoryJob?.cancel();launchCatalogLoading=false;launchDirectoryLoading=false }
    fun browseLaunchDirectory(machine:MachineKey,path:String) {
        if(launchMachine != machine || launchDirectoryLoading || !launchVisible) return
        if(path != "~" && (!path.startsWith('/') || path.length > 4096 || path.any(Char::isISOControl))) { launchError="Choose an absolute native folder.";return }
        val connection=connections.find { it.id == machine.connectionId } ?: return
        launchWorktreeJob?.cancel();launchWorktreeState=null;launchWorktreeLoading=false;launchDirectoryState=null
        val generation=launchGeneration;launchDirectoryLoading=true;launchError=""
        launchDirectoryJob=viewModelScope.launch {
            try {
                val target=Target(machine.computerId,"","","",machine.connectionId);val id=UUID.randomUUID().toString()
                val receipt=api.await(connection,api.submit(connection,target,"dirs",JSONObject().put("request_id",id).put("path",path),id))
                check(receipt.string("state") == "completed") { receipt.string("error").ifBlank { "This folder is unavailable." } }
                val raw=receipt.getJSONObject("result")
                check(raw.string("path").startsWith('/')) { "Computer returned an invalid folder." }
                if(launchGeneration == generation && launchMachine == machine) {
                    launchDirectoryState=raw.put("requested_path",path)
                    loadLaunchWorktrees(machine,raw.getString("path"),path)
                }
            } catch(e:Exception) { if(e is CancellationException) throw e;if(launchGeneration == generation) launchError=e.message.orEmpty() }
            finally { if(launchGeneration == generation) launchDirectoryLoading=false }
        }
    }
    fun worktreeReason(machine:MachineKey):String {
        if(launchReason(machine).isNotBlank()) return launchReason(machine)
        if(listOf("worktrees","worktree_create").any { it !in relayOperations[machine.connectionId].orEmpty() || it !in machineOperations[machine].orEmpty() })
            return "Update Zerus on this computer to use worktree actions."
        if(sessionActions.any { it.operation=="worktree_create" && it.target.connectionId==machine.connectionId && it.target.computerId==machine.computerId && it.blocksSending })
            return "Check the previous worktree creation receipt before creating another."
        return ""
    }
    private fun loadLaunchWorktrees(machine:MachineKey,path:String,requested:String) {
        launchWorktreeJob?.cancel();launchWorktreeState=null;launchWorktreeLoading=false
        if("worktrees" !in relayOperations[machine.connectionId].orEmpty() || "worktrees" !in machineOperations[machine].orEmpty()) return
        val connection=connections.find { it.id==machine.connectionId } ?: return
        val generation=launchGeneration;launchWorktreeLoading=true
        launchWorktreeJob=viewModelScope.launch {
            try {
                val id=UUID.randomUUID().toString();val target=Target(machine.computerId,"","","",machine.connectionId)
                val receipt=api.await(connection,api.submit(connection,target,"worktrees",JSONObject().put("request_id",id).put("path",path),id))
                check(receipt.string("state")=="completed") { receipt.string("error").ifBlank { "Worktree catalog unavailable." } }
                val raw=receipt.getJSONObject("result").put("requested_path",requested)
                if(launchGeneration==generation && launchMachine==machine) launchWorktreeState=raw
            } catch(e:Exception) { if(e is CancellationException) throw e }
            finally { if(launchGeneration==generation && launchMachine==machine) launchWorktreeLoading=false }
        }
    }
    fun createLaunchWorktree(machine:MachineKey,evidence:JSONObject,branch:String,base:String,destination:String,onCreated:(String)->Unit) {
        val problem=worktreeReason(machine)
        if(problem.isNotBlank() || worktreeSubmitting || !launchVisible || launchMachine!=machine || !WorktreePresentation.healthy(evidence) ||
            launchWorktreeState?.string("common_dir")!=evidence.string("common_dir")) { launchError=problem.ifBlank { "Refresh the current worktrees before creation." };return }
        val arguments=JSONObject().put("path",evidence.getString("path")).put("common_dir",evidence.getString("common_dir"))
            .put("branch",branch).put("base",base).put("destination",destination)
        try { SessionActionPolicies.arguments("worktree_create",arguments) } catch(e:Exception) { launchError=e.message.orEmpty();return }
        val action=SessionAction(UUID.randomUUID().toString(),Target(machine.computerId,"","","",machine.connectionId),"worktree_create",arguments.toString())
        val connection=connections.find { it.id==machine.connectionId } ?: return
        val generation=launchGeneration;worktreeSubmitting=true;actionFlights+=action.requestId;launchError=""
        viewModelScope.launch {
            var attempted=false;var rejected=false
            try {
                durable({ state -> check(state.sessionActions.none { it.operation=="worktree_create" && it.target==action.target && it.blocksSending });state.action(action) },
                    { current,written -> current.action(written.sessionActions.first { it.requestId==action.requestId }) })
                check(launchVisible && launchGeneration==generation && launchMachine==machine && launchWorktreeState?.string("common_dir")==evidence.string("common_dir")) { "The selected repository changed. Worktree creation was not sent." }
                attempted=true
                val initial=try { api.submit(connection,action.target,"worktree_create",JSONObject(arguments.toString()).put("request_id",action.requestId),action.requestId) }
                    catch(e:RelayException) { rejected=e.status in listOf(400,401,403,404,409,413,429);throw e }
                settleSessionAction(action,api.await(connection,initial))
                val completed=messageState.sessionActions.find { it.requestId==action.requestId }
                if(completed?.status=="completed" && completed.resultPath.isNotBlank() && launchVisible && launchGeneration==generation && launchMachine==machine)
                    onCreated(completed.resultPath)
                else launchError=completed?.error.orEmpty()
            } catch(e:Exception) {
                try { durable({ state -> state.sessionActions.find { it.requestId==action.requestId }?.let { existing ->
                    state.action(existing.copy(status=if(!attempted || rejected) "failed" else "uncertain",error=if(!attempted || rejected) e.message.orEmpty() else "Worktree creation is unconfirmed. Check the original receipt and folder; nothing has been retried.")) } ?: state }) }
                catch(_:Exception) { launchError="Could not save the creation outcome. The original request remains recoverable." }
                if(e is CancellationException) throw e
                launchError=if(!attempted || rejected) e.message.orEmpty() else "Worktree creation is unconfirmed. Check its original receipt; nothing has been retried."
            } finally { actionFlights-=action.requestId;worktreeSubmitting=false }
        }
    }
    fun launchSession(machine:MachineKey,agent:String,directory:String,tag:String,accountId:String?=null,expectedCatalog:String=launchCatalogSignature(),projectId:String="",worktrees:JSONObject?=null) {
        val reason=launchReason(machine);if(reason.isNotBlank()) { launchError=reason;return }
        if(launchSubmitting || worktreeSubmitting || !launchVisible || launchMachine != machine || launchCatalogLoading || launchDirectoryLoading || launchWorktreeLoading || expectedCatalog != launchCatalogEvidence) { launchError="The creation details changed. Review the current choices.";return }
        val catalog=launchCatalogState ?: return
        if(catalog.optJSONArray("agents")?.let { array -> (0 until array.length()).any { array.optString(it) == agent } } != true) { launchError="This native provider is unavailable.";return }
        if(accountId == null || LaunchPresentation.accounts(catalog,agent).none { it.id == accountId }) { launchError="Choose an offered account for the selected provider.";return }
        if(!LaunchPresentation.selectedFolder(launchDirectoryState,directory)) { launchError="Select a folder from the current native browser.";return }
        val args=JSONObject().put("agent",agent).put("directory",directory).put("tag",tag).put("account_id",accountId)
        if(catalog.opt("project_launch_supported")==true) {
            try { LaunchPresentation.projectArguments(catalog,projectId,directory,worktrees).let { fields -> fields.keys().forEach { key -> args.put(key,fields.get(key)) } } }
            catch(e:Exception) { launchError=e.message.orEmpty();return }
        } else if(projectId.isNotBlank()) { launchError="This computer does not support scoped project assignment.";return }
        try { SessionActionPolicies.arguments("launch",args) } catch(e:Exception) { launchError=e.message.orEmpty();return }
        val target=Target(machine.computerId,"","","",machine.connectionId)
        val action=SessionAction(UUID.randomUUID().toString(),target,"launch",args.toString())
        val connection=connections.find { it.id == machine.connectionId } ?: return
        val generation=launchGeneration;launchSubmitting=true;actionFlights+=action.requestId
        viewModelScope.launch {
            var attempted=false;var rejected=false
            try {
                durable({ state -> check(state.sessionActions.none { it.operation == "launch" && it.target.computerId == target.computerId && it.target.connectionId == target.connectionId && it.blocksSending });state.action(action) }, { current,written -> current.action(written.sessionActions.first { it.requestId == action.requestId }) })
                check(launchVisible && launchGeneration == generation && launchMachine == machine && launchCatalogEvidence == expectedCatalog && connections.any { it.id == machine.connectionId }) { "The launch form changed before creating the session." }
                attempted=true
                val initial=try { api.submit(connection,target,"launch",JSONObject(args.toString()).put("request_id",action.requestId),action.requestId) } catch(e:RelayException) { rejected=e.status in listOf(400,401,403,404,409,413,429);throw e }
                val receipt=api.await(connection,initial);settleSessionAction(action,receipt)
                val completed=messageState.sessionActions.find { it.requestId == action.requestId }
                if(completed?.status == "completed" && completed.resultTarget != null && launchVisible && launchGeneration == generation && launchMachine == machine) {
                    if(completed.error.isBlank()) {
                        val created=completed.resultTarget;closeLaunch()
                        open(Session(created,created.session,agent,"starting",directory,"",0,JSONObject().put("name",created.session).put("run_id",created.run).put("conversation_id",created.conversation)))
                        refresh()
                    } else { launchError=completed.error;refresh() }
                } else if(completed?.status != "completed") launchError=completed?.error.orEmpty()
            } catch(e:Exception) {
                val status=if(!attempted || rejected) "failed" else "uncertain"
                try { durable({ state -> state.sessionActions.find { it.requestId == action.requestId }?.let { if(it.status in setOf("sending","uncertain")) state.action(it.copy(status=status,error=if(status=="failed") e.message.orEmpty() else "Launch is unconfirmed. Check its original receipt; it has not been retried.")) else state } ?: state }) } catch(_:Exception) { launchError="Could not save the launch result. Its original request remains recoverable." }
                if(e is CancellationException) throw e
                launchError=if(status=="failed") e.message.orEmpty() else "Launch is unconfirmed. Check its original receipt before creating another session."
            } finally { actionFlights-=action.requestId;launchSubmitting=false }
        }
    }
    var terminalState by mutableStateOf<JSONObject?>(null);private set
    var terminalLoading by mutableStateOf(false);private set
    var terminalError by mutableStateOf("");private set
    var terminalVisible by mutableStateOf(false);private set
    private var terminalVerified=false
    private var terminalReceivedAt=0L
    private var terminalJob:Job?=null
    private val emptyTerminalBuffers=mutableMapOf<String,TerminalBuffer>()
    fun terminalOpenReason(target:Target):String {
        if(selected?.target != target || target.agentId.isNotBlank() || target.archiveId.isNotBlank()) return "Terminal is available only for the live parent session."
        if("terminal_snapshot" !in relayOperations[target.connectionId].orEmpty() || "terminal_snapshot" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) return "This native adapter does not provide remote Terminal."
        return ""
    }
    private fun terminalCurrent(target:Target,binding:String):Boolean {
        val raw=terminalState ?: return false
        return selected?.target == target && terminalVisible && terminalVerified && android.os.SystemClock.elapsedRealtime()-terminalReceivedAt in 0..5000 &&
            raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation && raw.string("terminal_binding_id") == binding && binding.isNotBlank()
    }
    fun terminalFresh(target:Target)=terminalCurrent(target,terminalState?.string("terminal_binding_id").orEmpty())
    fun terminalReason(target:Target):String {
        if(updateInstallPreparing) return "Saving drafts for an app update."
        val unavailable=terminalOpenReason(target);if(unavailable.isNotBlank()) return unavailable
        if(!storageReady || "terminal_input" !in relayOperations[target.connectionId].orEmpty() || "terminal_input" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) return "This computer does not support verified Terminal input."
        if(!terminalCurrent(target,terminalState?.string("terminal_binding_id").orEmpty())) return "Refresh Terminal before sending input."
        if(contextBlocked(target) || target.key in preparingTargets || outgoing.any { it.belongsTo(target) && it.blocksSending } || attachmentImporting || attachmentPreparing) return "Resolve the pending delivery before typing into Terminal."
        return ""
    }
    fun setTerminalVisible(target:Target,visible:Boolean) {
        if(selected?.target != target) return
        terminalVisible=visible
        if(visible) terminalSnapshot(target) else { terminalJob?.cancel();terminalLoading=false;terminalVerified=false;activityVerified=false;activeFrameVerified=false;refreshActivity() }
    }
    fun terminalSnapshot(target:Target) {
        val reason=terminalOpenReason(target);if(reason.isNotBlank()) { terminalError=reason;return }
        if(terminalLoading) return
        val connection=connections.find { it.id == target.connectionId } ?: return
        val navigation=navigationId
        terminalLoading=true;terminalError=""
        terminalJob=viewModelScope.launch {
            try {
                val id=UUID.randomUUID().toString()
                val payload=target.json().put("request_id",id)
                val receipt=api.await(connection,api.submit(connection,target,"terminal_snapshot",payload,id),50,100)
                check(receipt.string("state") == "completed") { receipt.string("error").ifBlank { "Terminal is not available yet." } }
                val result=receipt.getJSONObject("result")
                check(result.string("request_id") == id && result.string("name") == target.session && result.string("run_id") == target.run && result.string("conversation_id") == target.conversation &&
                    result.string("terminal_binding_id").isNotBlank() && result.string("screen").toByteArray(Charsets.UTF_8).size <= 512*1024) { "Computer returned a different Terminal binding." }
                if(navigationId == navigation && selected?.target == target && terminalVisible) { terminalState=result;terminalVerified=true;terminalReceivedAt=android.os.SystemClock.elapsedRealtime() }
            } catch(e:Exception) { if(e is CancellationException) throw e; if(navigationId == navigation && selected?.target == target) { terminalError=e.message.orEmpty();terminalVerified=false } }
            finally { if(navigationId == navigation) terminalLoading=false }
        }
    }
    fun terminalBuffer(target:Target):TerminalBuffer? {
        val raw=terminalState ?: return null
        if(raw.string("name") != target.session || raw.string("run_id") != target.run || raw.string("conversation_id") != target.conversation) return null
        val binding=raw.string("terminal_binding_id");if(binding.isBlank()) return null
        val key=target.key + ":terminal:" + binding
        return messageState.terminalBuffers.find { it.key == key } ?: emptyTerminalBuffers.getOrPut(key) { TerminalBuffer(target,binding) }
    }
    fun editTerminal(target:Target,text:String) {
        val buffer=terminalBuffer(target) ?: return
        if(!storageReady || text.toByteArray(Charsets.UTF_8).size > 65536 || text == buffer.text) return
        val editor=buffer.copy(text=text,revision=UUID.randomUUID().toString())
        editState { state -> TerminalBuffers.edit(state,editor) }
    }
    fun restoreTerminalInput(action:SessionAction) {
        if(action.operation != "terminal_input" || action.status !in setOf("failed","reviewed")) return
        val current=terminalBuffer(action.target) ?: return
        val args=runCatching { JSONObject(action.arguments) }.getOrNull() ?: return
        if(args.string("terminal_binding_id") != current.binding || !args.has("text") || current.text.isNotEmpty()) { terminalError="The original Terminal input remains saved. Restore it only into its empty matching Terminal buffer.";return }
        editTerminal(action.target,args.getString("text"));flushDrafts()
    }
    fun terminalInput(target:Target,key:String?=null,enter:Boolean=false) {
        val reason=terminalReason(target);if(reason.isNotBlank()) { terminalError=reason;return }
        val buffer=terminalBuffer(target) ?: return
        val args=JSONObject().put("terminal_binding_id",buffer.binding)
        if(key != null) args.put("key",key) else args.put("text",buffer.text).put("enter",enter)
        submitSessionAction(target,"terminal_input",args,terminalBuffer=if(key == null) buffer else null)
    }
    var processOutputResult by mutableStateOf<JSONObject?>(null); private set
    var processOutputId by mutableStateOf(""); private set
    var processOutputLoading by mutableStateOf(false); private set
    var processOutputError by mutableStateOf(""); private set
    private var processOutputJob:Job?=null
    fun processOutputReason(target:Target,processId:String):String {
        if(selected?.target != target || !activityVerified || target.agentId.isNotBlank()) return "Refresh the parent session before reading process output."
        if("process_output" !in relayOperations[target.connectionId].orEmpty() || "process_output" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) return "This computer does not support remote process output."
        val process=activity?.optJSONObject("processes")?.optJSONArray("items")?.objects().orEmpty().find { it.string("id") == processId } ?: return "This process is no longer present."
        return if(process.optJSONObject("capabilities")?.optBoolean("output") == true) "" else "This process does not provide output."
    }
    fun processOutput(target: Target, processId: String, generation: String? = null) {
        val reason=processOutputReason(target,processId);if(reason.isNotBlank()) { processOutputError=reason;return }
        if("process_output" !in relayOperations[target.connectionId].orEmpty() || "process_output" !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) { processOutputError="This computer does not support remote process output.";return }
        val process=activity?.optJSONObject("processes")?.optJSONArray("items")?.objects().orEmpty().find { it.string("id") == processId } ?: return
        if(process.optJSONObject("capabilities")?.optBoolean("output") != true) return
        if(processOutputLoading && processOutputId == processId) return
        val connection=connections.find { it.id == target.connectionId } ?: return
        val navigation=navigationId
        processOutputJob?.cancel();processOutputId=processId;processOutputResult=null;processOutputError="";processOutputLoading=true
        processOutputJob=viewModelScope.launch {
            try {
                val id=UUID.randomUUID().toString()
                val payload=JSONObject().put("request_id",id).put("expected_run_id",target.run).put("expected_conversation_id",target.conversation).put("process_id",processId)
                if(target.archiveId.isNotBlank()) payload.put("archive_id",target.archiveId)
                if(generation != null) payload.put("generation",generation)
                val receipt=api.await(connection,api.submit(connection,target,"process_output",payload,id))
                check(receipt.string("state") == "completed") { receipt.string("error").ifBlank { "Process output is not available yet." } }
                val result=receipt.getJSONObject("result")
                check(result.string("request_id") == id && result.string("name") == target.session && result.string("id") == processId && result.string("run_id") == target.run && result.string("conversation_id") == target.conversation && result.string("archive_id") == target.archiveId) { "Computer returned output for another process." }
                if(navigationId == navigation && selected?.target == target && processOutputId == processId) processOutputResult=result
            } catch(e:Exception) { if(e is CancellationException) throw e; if(navigationId == navigation && selected?.target == target) processOutputError=e.message.orEmpty() }
            finally { if(navigationId == navigation && processOutputId == processId) processOutputLoading=false }
        }
    }
    fun actionReason(target: Target, operation: String): String {
        if(updateInstallPreparing) return "Saving drafts for an app update."
        if(operation == "terminal_input") return terminalReason(target)
        if(terminalVisible) return "Return to Activity before using session controls."
        if(target.agentId.isNotBlank()) return "Session controls belong to the parent conversation."
        if (!storageReady || demo || selected?.target != target || !activityVerified) return "Refresh this exact session before taking an action."
        val raw = activity ?: return "Refresh this session."
        if (raw.string("name") != target.session || raw.string("run_id") != target.run || raw.string("conversation_id") != target.conversation || raw.string("archive_id") != target.archiveId) return "The session identity changed. Refresh before taking an action."
        if (operation !in SessionActionPolicies.operations || operation !in relayOperations[target.connectionId].orEmpty() || operation !in machineOperations[MachineKey(target.connectionId,target.computerId)].orEmpty()) return "This gateway or computer does not support this action."
        if (target.archiveId.isNotBlank() && operation !in setOf("restore", "forget", "rename", "fork")) return "This archived conversation is read-only."
        if (target.archiveId.isBlank() && operation == "restore") return "Select an archived version first."
        if(operation in SessionActionPolicies.lifecycle && raw.optJSONArray("allowed_actions")?.let { value -> (0 until value.length()).any { value.optString(it) == operation } } != true) return raw.optJSONObject("action_reasons")?.string(operation).orEmpty().ifBlank { "Native session controls are unavailable for this action." }
        if (contextBlocked(target) || outgoing.any { it.belongsTo(target) && it.blocksSending } || target.key in preparingTargets || compactContinuationActive) return "Resolve the pending delivery or action first."
        if (attachmentImporting || attachmentPreparing || pickerDraft?.target == target || drafts.any { it.target == target && it.status in listOf("submitting", "uncertain") }) return "Wait for the current answer or file selection."
        return ""
    }
    fun sendNow(target: Target, queueId: String) = sessionAction(target,"send_now",JSONObject().put("queue_id",queueId))
    fun updateSettings(target: Target, model: String? = null, effort: String? = null, expectedPendingId: String = "") = sessionAction(target,"settings",JSONObject().also {
        if(model != null) it.put("model",model); if(effort != null) it.put("effort",effort)
        if(expectedPendingId.isNotBlank()) it.put("expected_pending_id",expectedPendingId)
    })
    fun processStop(target: Target, processId: String, generation: String? = null) = sessionAction(target,"process_stop",JSONObject().put("process_id",processId).also { if(generation != null) it.put("generation",generation) })
    fun sessionAction(target: Target, operation: String, args: JSONObject = JSONObject())=submitSessionAction(target,operation,args)
    private fun submitSessionAction(target:Target,operation:String,args:JSONObject,authorized:SessionAction?=null,frame:Frame?=null,terminalBuffer:TerminalBuffer?=null) {
        val inspected=frame?.raw ?: activity ?: JSONObject()
        val reason = if(authorized == null) actionReason(target,operation) else authorizedSettingsReason(target,args,authorized,frame); if(reason.isNotBlank()) { error=reason; return }
        val arguments = try { SessionActionPolicies.arguments(operation,args) } catch(e:Exception) { error=e.message.orEmpty(); return }
        val eligibility = SessionActionPolicies.inspectReason(target,operation,arguments,inspected)
        if(eligibility.isNotBlank()) { error=eligibility; return }
        val connection = connections.find { it.id == target.connectionId } ?: return
        val action = SessionAction(UUID.randomUUID().toString(),target,operation,arguments.toString())
        val evidence=SessionActionPolicies.evidence(operation,inspected,arguments)
        val navigation = navigationId
        preparingTargets += target.key; actionFlights += action.requestId
        viewModelScope.launch {
            var attempted=false; var rejected=false
            try {
                durable({ state ->
                    if(terminalBuffer != null) TerminalBuffers.enqueue(state,terminalBuffer,action) else if(authorized == null) state.action(action) else {
                        ScheduledSettingsAttempt.begin(state,authorized,action)
                    }
                }, { current,written ->
                    var next=if(terminalBuffer == null) current.action(written.sessionActions.first { it.requestId == action.requestId }) else TerminalBuffers.acknowledge(current,written,terminalBuffer,action)
                    if(authorized != null) written.sessionActions.find { it.requestId == authorized.requestId }?.let { next=next.action(it) }
                    next
                })
                preparingTargets -= target.key
                val live=if(authorized == null) activity ?: JSONObject() else inspected
                if(authorized == null) check(selected?.target == target && navigationId == navigation && (operation == "terminal_input" || activityVerified)) { "The session changed before the action was sent." }
                else check(frame != null && frame.target == target && System.currentTimeMillis()-frame.observedAt in 0..10_000 && connections.any { it.id == target.connectionId } &&
                    messageState.sessionActions.any { it.requestId == authorized.requestId && it.applyAttemptId == action.requestId }) { "The authorized native settings changed before delivery." }
                if(operation == "terminal_input") check(terminalCurrent(target,arguments.string("terminal_binding_id"))) { "Terminal binding expired before delivery. Input was not sent." }
                if(operation != "terminal_input") check(SessionActionPolicies.evidence(operation,live,arguments) == evidence) { "The displayed native action details changed before delivery." }
                check(SessionActionPolicies.inspectReason(target,operation,arguments,live).isBlank()) { "The native action target or catalog changed before delivery." }
                val payload = JSONObject(arguments.toString()).put("request_id",action.requestId).put("expected_run_id",target.run).put("expected_conversation_id",target.conversation)
                if(target.archiveId.isNotBlank()) payload.put("archive_id",target.archiveId)
                attempted=true
                val initial=try { api.submit(connection,target,operation,payload,action.requestId) }
                    catch(e:RelayException) { rejected=e.status in listOf(400,401,403,404,409,413,429); throw e }
                val receipt = if(operation == "terminal_input") api.await(connection,initial,50,100) else api.await(connection,initial)
                settleSessionAction(action,receipt)
                val completed=messageState.sessionActions.find { it.requestId == action.requestId }
                if(operation == "terminal_input") { if(selected?.target == target && terminalVisible) terminalSnapshot(target) }
                else if(completed != null) when(SessionActionFollowUp.kind(completed,selected?.target,navigationId==navigation)) {
                    SessionActionFollowUp.Kind.Sessions -> { back();returnToSessionsSequence++;refresh() }
                    SessionActionFollowUp.Kind.OpenResult -> {
                        activityVerified=false;activeFrameVerified=false;historyCached=activity != null
                        resolveActionResult(completed,navigation,target)
                    }
                    SessionActionFollowUp.Kind.Refresh -> refreshActivity()
                    SessionActionFollowUp.Kind.None -> Unit
                }
            } catch(e:Exception) {
                val status = if(!attempted || rejected) "failed" else "uncertain"
                try { durable({ state -> state.sessionActions.find { it.requestId == action.requestId }?.let { current ->
                    if(current.status in listOf("sending","uncertain")) state.action(current.copy(status=status,error=if(status=="failed") e.message.orEmpty() else "Action delivery is unconfirmed. Check its original receipt; it has not been retried.")) else state
                } ?: state }) } catch(_:Exception) { error="Could not save the action result. Its original request remains recoverable." }
                if(e is CancellationException) throw e
            } finally { preparingTargets -= target.key; actionFlights -= action.requestId }
        }
    }
    private var actionResultJob:Job?=null
    private val resolvingActionResults=mutableSetOf<String>()
    var actionResultNotice by mutableStateOf("");private set
    var returnToSessionsSequence by mutableStateOf(0L);private set
    fun pendingActionResult(target:Target)=messageState.sessionActions.lastOrNull { action ->
        action.target==target && SessionActionFollowUp.openable(action)
    }
    fun openSessionActionResult(action:SessionAction) {
        val stored=messageState.sessionActions.find { it.requestId==action.requestId && it.status=="completed" && it.resultTarget!=null } ?: return
        if(actionResultJob?.isActive==true) return
        val original=selected?.target
        val navigation=navigationId
        actionResultJob=viewModelScope.launch { resolveActionResult(stored,navigation,original) }
    }
    fun confirmedDraftAction(draft:Draft)=messageState.sessionActions.lastOrNull { action ->
        action.resultTarget?.let { ActionDraftPolicies.proof(action,draft.target,it) }==true && draft.status=="editing" && draft.questionId.isBlank() && draft.detachedId.isBlank()
    }
    fun restoreDraftToConfirmedResult(draft:Draft,action:SessionAction) {
        val stored=confirmedDraftAction(draft)?.takeIf { it.requestId==action.requestId } ?: return
        val source=messageState.drafts.find { it.key==draft.key && it.generation==draft.generation && it.revision==draft.revision && it.attachments==draft.attachments } ?: return
        val destination=stored.resultTarget ?: return
        if(!ActionDraftPolicies.destinationReady(messageState,destination)) { actionResultNotice="The confirmed session already has a composition. Your original draft remains in Saved drafts.";return }
        if(actionResultJob?.isActive==true) return
        val navigation=navigationId;val original=selected?.target
        actionResultJob=viewModelScope.launch { resolveActionResult(stored,navigation,original,source) }
    }
    private suspend fun resolveActionResult(action:SessionAction,navigation:Long,original:Target?,restore:Draft?=null) {
        val destination=action.resultTarget ?: return
        val connection=connections.find { it.id==destination.connectionId } ?: return
        if(!resolvingActionResults.add(action.requestId)) return
        try {
            val frame=ConfirmedTargetResolution.read(active={ navigationId==navigation && selected?.target==original && connections.any { it.id==destination.connectionId } },
                pause={ delay(750);runCatching { api.computers(connection) } },inspect={ inspectOnce(connection,destination).await().also {
                    check(it.target==destination || destination.resolvesTo(it.target)) { "The confirmed destination has changed. The original draft remains saved." }
                } })
            if(navigationId!=navigation || selected?.target!=original) return
            val messages=outgoing.filter { it.belongsTo(frame.target) };val state=messageState
            val prepared=withContext(Dispatchers.Default) { Presentation(messages,ConversationMerge.merge(frame.target,frame.events,messages).events,QuestionTracking.restore(state,frame.target,frame.questions)) }
            if(navigationId!=navigation || selected?.target!=original) return
            androidx.compose.runtime.snapshots.Snapshot.withMutableSnapshot {
                if((original==action.target || restore!=null) && ActionDraftPolicies.proof(action,action.target,frame.target)) {
                    editState { if(restore==null) ActionDraftPolicies.promote(it,action) else ActionDraftPolicies.restore(it,action,restore) }
                    readWriter?.let { readState=it.edit { state -> state.promote(action) } }
                }
                selected=Session(frame.target,frame.raw.string("title","label","name"),frame.raw.string("agent"),frame.raw.string("activity","state"),frame.raw.string("project","cwd"),"",0,frame.raw)
                frame.presentation=prepared;installFrame(frame,true)
                actionResultNotice=if(restore!=null && drafts.any { it.key==restore.key }) "Your original draft remains in Saved drafts because the composition changed or the destination is occupied." else ""
            }
            if(action.operation=="launch") closeLaunch()
        } catch(error:CancellationException) { throw error }
        catch(_:Exception) { if(navigationId==navigation && selected?.target==original) actionResultNotice="The computer confirmed the action. Its destination is not reachable yet. Open the confirmed result when connected; your original draft remains saved." }
        finally { resolvingActionResults-=action.requestId }
    }
    private suspend fun settleSessionAction(action: SessionAction,receipt: JSONObject) {
        val result = SessionActionPolicies.result(action,receipt)
        durable({ state -> state.sessionActions.find { it.requestId == action.requestId }?.let { current ->
            if(current.status in listOf("sending","uncertain")) {
                var next=state.action(result)
                if(result.operation == "settings" && result.status == "applied") state.sessionActions.find { it.applyAttemptId == action.requestId }?.let { next=next.action(it.copy(status="applied")) }
                next
            } else state
        } ?: state }, { current,written ->
            var next=written.sessionActions.find { it.requestId == action.requestId }?.let(current::action) ?: current
            written.sessionActions.find { it.applyAttemptId == action.requestId }?.let { next=next.action(it) }
            next
        })
    }
    fun recoveryActionReason(target:Target,jobId:String,choice:String):String {
        val reason=actionReason(target,"recovery_action");if(reason.isNotBlank()) return reason
        val args=runCatching { SessionActionPolicies.arguments("recovery_action",JSONObject().put("job_id",jobId).put("action",choice)) }.getOrNull() ?: return "Invalid native recovery action."
        return SessionActionPolicies.inspectReason(target,"recovery_action",args,activity ?: JSONObject())
    }
    fun recoveryAction(target:Target,jobId:String,choice:String) {
        val reason=recoveryActionReason(target,jobId,choice);if(reason.isNotBlank()) { error=reason;return }
        sessionAction(target,"recovery_action",JSONObject().put("job_id",jobId).put("action",choice))
    }
    fun checkSessionAction(action: SessionAction) {
        if(action.requestId in actionFlights) return
        val connection=connections.find { it.id == action.target.connectionId } ?: return
        actionFlights += action.requestId
        viewModelScope.launch { try { settleSessionAction(action,api.await(connection,api.receipt(connection,action.requestId))) }
            catch(e:Exception) { if(e is CancellationException) throw e; error="The original action receipt could not be checked. Nothing has been retried." }
            finally { actionFlights -= action.requestId } }
    }
    fun reviewSessionAction(action: SessionAction) {
        if(action.requestId in actionFlights) return
        editState { state -> state.sessionActions.find { it.requestId == action.requestId && (it.blocksSending || it.needsProjectReview) }?.let { state.action(it.copy(status="reviewed")) } ?: state }
        flushDrafts()
    }
    fun cancelCompactContinuation(reason: String = "") {
        if (continuation != null && reason.isNotBlank()) contextNotice = reason
        continuation = null; continuationEnqueuing = ""
    }
    fun contextActionReason(target: Target, operation: String): String {
        if (target.agentId.isNotBlank() || target.archiveId.isNotBlank() || !fresh(target)) return "Refresh the live conversation before changing its context."
        if (target.run.isBlank() || target.conversation.isBlank()) return "Wait for the conversation to finish starting."
        if (operation !in relayOperations[target.connectionId].orEmpty() || operation !in machineOperations[MachineKey(target.connectionId, target.computerId)].orEmpty()) return "This gateway or computer does not support native context commands."
        if (compactContinuationActive || contextBlocked(target) || outgoing.any { it.belongsTo(target) && it.blocksSending } || target.key in preparingTargets) return "Resolve the pending delivery or context command first."
        if (pickerDraft?.target == target || attachmentImporting || attachmentPreparing) return "Wait for the selected files to finish importing."
        if (drafts.any { it.target == target && it.status in listOf("submitting", "uncertain") }) return "Wait for the pending answer or interrupt."
        if (activity?.optBoolean(operation + "_supported") != true || activity?.string("activity") != "idle" || activity?.string("phase") != "idle") return "Native context changes are unavailable while the agent is busy or waiting for input."
        return ""
    }
    fun compactContext(target: Target, continueDraft: Boolean = false, expectedDraft: Draft? = null) = startContext(target, "compact_context", continueDraft, expectedDraft)
    fun clearContext(target: Target) = startContext(target, "clear_context", false, null)
    private fun startContext(target: Target, operation: String, continueDraft: Boolean, expectedDraft: Draft?) = viewModelScope.launch {
        val reason = contextActionReason(target, operation)
        if (demo || !storageReady || reason.isNotBlank()) { if (reason.isNotBlank()) contextNotice = reason; return@launch }
        val original = draft(target)
        if (expectedDraft != null && !ContextPolicies.sameDraft(expectedDraft, original)) { contextNotice = "Your composition changed. Review it before choosing a context action."; return@launch }
        if (continueDraft && original.text.isBlank() && original.attachments.isEmpty()) return@launch
        val connection = connections.find { it.id == target.connectionId } ?: return@launch
        val op = ContextOperation(UUID.randomUUID().toString(), target, operation)
        val navigation = navigationId
        preparingTargets += target.key; contextFlights += op.requestId
        if (continueDraft) continuation = CompactContinuation(op.requestId, original.copy(attachments = original.attachments.toList()), navigation, android.os.SystemClock.elapsedRealtime())
        contextNoticeOwner=op.requestId;contextNoticeValue=""
        if (operation == "clear_context") clearExpectation = ClearExpectation(op.requestId, target, navigation)
        var attempted = false
        var rejection: RelayException? = null
        try {
            durable({ state ->
                check(state.contextOperations.none { ContextPolicies.sameSessionRun(it.target, target) && it.blocksSending })
                state.context(op)
            }, { current, written -> current.context(written.contextOperations.first { it.requestId == op.requestId }) })
            check(fresh(target) && navigationId == navigation && (!continueDraft || continuation?.requestId == op.requestId) && (operation != "clear_context" || clearExpectation?.requestId == op.requestId)) { "The selected conversation changed before transport." }
            val payload = target.json().put("request_id", op.requestId)
            attempted = true
            val initial = try { api.submit(connection, target, operation, payload, op.requestId) }
            catch (error: RelayException) { if (error.status in listOf(400, 401, 403, 404, 409, 413, 429)) rejection = error; throw error }
            val receipt = api.await(connection, initial)
            val result = ContextPolicies.result(op, receipt)
            durable({ state -> updateContext(state, op.requestId) { current ->
                if (current.status in listOf("completed", "failed", "cancelled", "unchanged", "reviewed")) current else result
            } })
            if (result.status == "submitted") acknowledgedContexts += op.requestId
            if (result.status !in listOf("submitted", "completed")) { contextFlights -= op.requestId; cancelCompactContinuation("Context delivery was not confirmed. Your message was not sent.") }
            if (operation == "clear_context" && clearExpectation?.requestId == op.requestId) {
                if (result.status == "completed") clearExpectation = clearExpectation?.copy(confirmed = true)
                else if (result.status != "submitted") clearExpectation = null
            }
            if (result.status == "completed") contextFlights -= op.requestId
            setContextNotice(op.requestId,if (result.status == "submitted") "Native command submitted. Waiting for the agent to confirm the result." else result.error)
            if (selected?.target?.let { ContextPolicies.sameSessionRun(it, target) } == true) refreshActivity()
            activeFrame?.takeIf { it.target == target && activityVerified }?.let(::observeContext)
        } catch (failure: Exception) {
            val status = if (attempted && rejection == null) "uncertain" else "failed"
            try { durable({ state -> updateContext(state, op.requestId) { current -> if (current.status in listOf("completed", "reviewed", "cancelled", "unchanged")) current else current.copy(status = status,
                error = if (status == "uncertain") "Delivery is unconfirmed. Check the original context receipt before trying again." else "The context command was not sent or was rejected. Your draft is preserved.") } }) }
            catch (_: Exception) { contextNotice = "Could not save the command outcome. Its original identity remains recoverable." }
            cancelCompactContinuation("Compaction was not confirmed. Your message was not sent."); clearExpectation = null; contextFlights -= op.requestId
            if (failure is CancellationException) throw failure
        } finally { preparingTargets -= target.key }
    }
    private fun updateContext(state: MessageState, requestId: String, transform: (ContextOperation) -> ContextOperation): MessageState {
        val current = state.contextOperations.find { it.requestId == requestId } ?: return state
        return state.context(transform(current))
    }
    private fun observeContext(frame: Frame) {
        val intent = continuation
        if (intent != null && !intent.allowed(selected?.target, navigationId, android.os.SystemClock.elapsedRealtime())) cancelCompactContinuation("Continuation was canceled. Your draft has not been sent.")
        messageState.contextOperations.filter { it.target == frame.target && it.operation == "compact_context" && it.status != "reviewed" }.forEach { op ->
            val status = ContextPolicies.compactStatus(op, frame.raw) ?: return@forEach
            if (status != "completed" && continuation?.requestId == op.requestId) cancelCompactContinuation("Compaction did not complete. Your draft was kept without sending.")
            if (op.requestId in contextObservations) return@forEach
            contextObservations += op.requestId
            viewModelScope.launch {
                try {
                    if (op.status != status) durable({ state -> updateContext(state, op.requestId) { current -> if (current.status == "reviewed") current else current.copy(status = status, error = if (status == "completed") "" else "Native compaction is $status. Your draft was not sent.") } })
                    contextFlights -= op.requestId
                    setContextNotice(op.requestId,if(status == "completed") "" else "Native compaction is $status. Your draft was not sent.")
                    val current = continuation?.takeIf { it.requestId == op.requestId } ?: return@launch
                    if (status == "completed" && op.requestId in acknowledgedContexts && continuationEnqueuing.isBlank() && fresh(current.draft.target) &&
                        current.allowed(selected?.target, navigationId, android.os.SystemClock.elapsedRealtime()) && current.unchanged(draft(current.draft.target)) && !sendingBlocked(current.draft.target)) {
                        continuationEnqueuing = current.requestId
                        sendMessageInternal(current.draft.target, current.draft, current.requestId, true, current.requestId)
                    }
                } catch (_: Exception) { cancelCompactContinuation("Could not save compaction confirmation. Your message was not sent.") }
                finally { contextObservations -= op.requestId }
            }
        }
    }
    fun checkContextOperation(operation: ContextOperation) = viewModelScope.launch {
        val connection = connections.find { it.id == operation.target.connectionId } ?: return@launch
        val flight = "context:${operation.requestId}"
        if (flight in receiptFlights) return@launch
        receiptFlights += flight
        try {
            val result = ContextPolicies.result(operation, api.await(connection, api.receipt(connection, operation.requestId)))
            durable({ state -> updateContext(state, operation.requestId) { current -> if (current.status in listOf("completed", "reviewed")) current else result } })
            val frame = inspectOnce(connection, operation.target).await()
            ContextPolicies.compactStatus(operation, frame.raw)?.let { status -> durable({ state -> updateContext(state, operation.requestId) { current -> if (current.status == "reviewed") current else current.copy(status = status, error = "") } }) }
            contextNotice = "Checked the original command. No command or message was retried."
            if (selected?.target?.let { ContextPolicies.sameSessionRun(it, operation.target) } == true) refreshActivity()
        } catch (failure: CancellationException) { throw failure }
        catch (_: Exception) { contextNotice = "The original receipt is unavailable. Its saved identity has been preserved." }
        finally { receiptFlights -= flight }
    }
    fun reviewContextOperation(operation: ContextOperation) {
        if ("context:${operation.requestId}" in receiptFlights) return
        if (continuation?.requestId == operation.requestId) cancelCompactContinuation()
        if (clearExpectation?.requestId == operation.requestId) clearExpectation = null
        try { editState { state -> updateContext(state, operation.requestId) { if (it.status == "completed") it else it.copy(status = "reviewed") } }; flushDrafts() }
        catch (_: Exception) { contextNotice = "Could not save the review. No command was retried." }
    }
    fun contextDrafts(target: Target) = drafts.filter { it.target != target && ContextPolicies.sameSession(it.target, target) && it.questionId.isBlank() && it.detachedId.isBlank() && it.status == "editing" && (it.text.isNotBlank() || it.attachments.isNotEmpty()) }
    fun canRestoreContextDraft(draft: Draft, target: Target) = fresh(target) && ContextPolicies.sameSession(draft.target, target) && draft.target != target && draft.questionId.isBlank() && draft.detachedId.isBlank() && draft.status == "editing"
    fun restoreContextDraft(draft: Draft, target: Target) {
        if (!canRestoreContextDraft(draft, target) || target.key in preparingTargets) return
        cancelCompactContinuation()
        val restored = draft.copy(target = target, generation = UUID.randomUUID().toString(), revision = UUID.randomUUID().toString())
        val parkedId = UUID.randomUUID().toString()
        try { editState { state ->
            val original = state.drafts.find { it.key == draft.key && it.revision == draft.revision && it.generation == draft.generation } ?: return@editState state
            val editor = state.drafts.find { it.target == target && it.questionId.isBlank() && it.detachedId.isBlank() }
            if (editor != null && editor.status != "editing") return@editState state
            val parked = editor?.takeIf { it.text.isNotBlank() || it.attachments.isNotEmpty() }?.copy(detachedId = parkedId)
            state.copy(drafts = state.drafts.filterNot { it.key == original.key || it.key == restored.key } + listOfNotNull(parked) + restored)
        }; flushDrafts() } catch (_: Exception) { contextNotice = "The original draft remains saved." }
    }
    fun clearError() { error = "" }
    fun preview() {
        demo = true
        val previewMachines = linkedMapOf("demo" to Demo.computer.label,"demo-laptop" to "Preview laptop","demo-build" to "Preview build machine with a long display name")
        val project = ProjectSummary(ProjectKey("demo", "demo", "zerus"), "Zerus", "#67E8CB", listOf(
            ProjectFolder("demo", "Zerus", "/workspace/zerus", "demo", previewMachines.getValue("demo")),
            ProjectFolder("demo-design", "Design sandbox", "/workspace/zerus/design", "demo", previewMachines.getValue("demo")),
            ProjectFolder("demo-long", "mobile", "/workspace/research/experiments/a-long-folder-name-for-previewing-wrapped-paths/mobile", "demo", previewMachines.getValue("demo")),
            ProjectFolder("demo-laptop-main", "zerus", "/workspace/zerus", "demo-laptop", previewMachines.getValue("demo-laptop")),
            ProjectFolder("demo-laptop-mobile", "mobile", "/workspace/zerus/mobile", "demo-laptop", previewMachines.getValue("demo-laptop")),
            ProjectFolder("demo-laptop-test", "mobile", "/workspace/testing/mobile", "demo-laptop", previewMachines.getValue("demo-laptop")),
            ProjectFolder("demo-build-main", "zerus", "/workspace/build/zerus", "demo-build", previewMachines.getValue("demo-build"))
        ), previewMachines)
        projects = listOf(project,ProjectSummary(ProjectKey("demo","demo","empty-preview"),"Empty preview project","#8DA9E8",emptyList(),emptyMap()))
        sessions = Demo.sessions.map { it.copy(projectKey = project.key) }
        machines = previewMachines.map { (id,name) -> Machine("demo",id,name,true) }
    }
    fun stopPreview() { demo = false; back(); selectedProject = null; sessions = emptyList(); projects = emptyList(); machines = emptyList(); refresh() }
    fun pair(url: String, code: String, onDone: () -> Unit) = viewModelScope.launch {
        if (!storageReady || busy) return@launch
        busy = true
        try {
            val connection = api.pair(url, code)
            val next = connections + connection
            withContext(Dispatchers.IO) { store.saveConnections(next) }; connections = next; demo = false
            onDone(); refresh(); AppTelemetry.registerPush(getApplication())
            if (LiveConnectionService.running) getApplication<Application>().startService(android.content.Intent(getApplication(), LiveConnectionService::class.java))
        } catch (e: Exception) { error = e.message ?: "Pairing failed." }
        finally { busy = false }
    }
    fun disconnect(connection: Connection) = viewModelScope.launch {
        if (busy) return@launch
        busy = true
        try {
            try { api.call(connection.url, connection.token, "/v1/device", delete = true) }
            catch (e: RelayException) { if (e.status != 401) throw e }
            val next = connections.filterNot { it.id == connection.id }
            withContext(Dispatchers.IO) { store.saveConnections(next) }; connections = next
            accountsWriter?.edit { rows -> rows.filterNot { it.key.connectionId==connection.id } }; accountsWriter?.flushAsync()
            inspections.values.filter { it.first.connectionId == connection.id }.forEach { it.second.cancel() }
            inspections.entries.removeAll { it.value.first.connectionId == connection.id }
            frames.entries.removeAll { it.value.target.connectionId == connection.id }
            editState { state -> state.copy(conversationIndex=state.conversationIndex.filterNot { it.target.connectionId == connection.id }) }
            val removedReadKeys=readState.records.filter { it.target.connectionId==connection.id }.map { ConversationReadPolicies.key(it.target) }.toSet()
            readWriter?.let { readState=it.edit { state -> state.copy(records=state.records.filterNot { it.target.connectionId == connection.id }) } };flushDrafts()
            historyWindows.entries.removeAll { it.value.target.connectionId==connection.id }
            historySnapshotHints.keys.removeAll { it in removedReadKeys };currentHistoryHeads-=removedReadKeys
            historyFlights.values.filter { it.first.connectionId==connection.id }.forEach { it.second.cancel() }
            historyFlights.entries.removeAll { it.value.first.connectionId==connection.id }
            runCatching { historyCache.purgeWorkspace(connection.id);pageCache.purgeWorkspace(connection.id) }
            sessions = sessions.filterNot { it.target.connectionId == connection.id }
            projects = projects.filterNot { it.key.connectionId == connection.id }
            if (selectedProject?.key?.connectionId == connection.id) selectedProject = null
            if (sessionProjectScope?.key?.connectionId == connection.id) clearSessionScope()
            machines = machines.filterNot { it.connectionId == connection.id }
            selectedMachines = selectedMachines.filterNot { it.connectionId == connection.id }.toSet()
            relayOperations = relayOperations - connection.id; machineOperations = machineOperations.filterKeys { it.connectionId != connection.id }
            if (selected?.target?.connectionId == connection.id) { navigationId++; cancelDetail(); selected = null; activity = null; activeFrame = null; activityVerified = false; conversationEvents = emptyList(); conversationQuestions = emptyList(); conversationOutgoing = emptyList(); cancelCompactContinuation(); clearExpectation = null }
        } catch (e: Exception) { error = "Could not revoke this phone on the gateway. The connection is preserved. ${e.message}" }
        finally { busy = false }
    }
    fun refresh(explicit: Boolean = false) {
        if (demo) return
        val start = catalogGate.begin("catalog", explicit)
        catalogProgress = catalogGate.visible()
        if (!start.newRequest) return
        val operation = start.operation
        val originalConnections = connections
        catalogBusy = true
        catalogTimer?.cancel()
        catalogTimer = viewModelScope.launch {
            delay(catalogGate.remaining(operation.id))
            if (catalogGate.owns(operation.id)) catalogProgress = catalogGate.visible()
        }
        catalogJob = viewModelScope.launch {
        pushStatuses = withContext(Dispatchers.IO) { connections.associate { it.id to store.pushStatus(it.id) } }
        val newSessions = mutableListOf<Session>(); val newMachines = mutableListOf<Machine>(); val newAccounts=mutableListOf<AccountCatalog>()
        val successfulConnections=mutableSetOf<String>()
        val newProjects = mutableListOf<ProjectSummary>(); val newOperations = mutableMapOf<MachineKey, Set<String>>(); val warnings = mutableListOf<String>()
        try {
            for (connection in originalConnections) {
                try {
                    if (explicit || connection.id !in relayOperations) {
                        try {
                            val capabilities = api.call(connection.url, connection.token, "/v1/capabilities")
                            val operations = capabilities.optJSONArray("operations")
                            relayFeatures=relayFeatures + (connection.id to capabilities.optJSONArray("features")?.let { value -> (0 until value.length()).map { value.optString(it) }.toSet() }.orEmpty())
                            relayOperations = relayOperations + (connection.id to operations?.let { value -> (0 until value.length()).map { value.optString(it) }.toSet() }.orEmpty())
                        } catch (e: CancellationException) { throw e }
                        catch (_: Exception) { /* Session discovery remains available if optional capability discovery fails. */ }
                    }
                    val computerRecords = api.computers(connection).optJSONArray("computers")?.objects().orEmpty()
                    newAccounts += withContext(Dispatchers.Default) { computerRecords.map { AccountSnapshots.parse(connection.id,it) } }
                    val connectionSessions = mutableListOf<Session>()
                    computerRecords.forEach { raw ->
                        val capabilities = raw.optJSONObject("snapshot")?.optJSONObject("mobile_capabilities")
                        newOperations[MachineKey(connection.id, raw.getString("id"))] = if (capabilities?.optInt("protocol_version") == 1)
                            capabilities.optJSONArray("operations")?.let { value -> (0 until value.length()).map { value.optString(it) }.toSet() }.orEmpty() else emptySet()
                        machineFeatures=machineFeatures + (MachineKey(connection.id,raw.getString("id")) to capabilities?.optJSONArray("features")?.let { value -> (0 until value.length()).map { value.optString(it) }.toSet() }.orEmpty())
                        newMachines += MachineCatalog.parse(connection.id, raw)
                        connectionSessions += NativeParser.sessions(connection, raw)
                    }
                    val catalog = ProjectParser.parse(connection, computerRecords, connectionSessions)
                    newSessions += catalog.sessions; newProjects += catalog.projects; warnings += catalog.unavailableComputers
                    successfulConnections += connection.id
                    if (catalogGate.owns(operation.id) && connections == originalConnections && catalogErrorOwner == connection.id) error = ""
                } catch (e: CancellationException) { throw e }
                catch (e: Exception) { if (catalogGate.owns(operation.id) && selected == null) {
                    error = "${connection.displayName}: ${ReadFailure.message(e, "sessions")}"; catalogErrorOwner = connection.id
                } }
            }
            if (!catalogGate.owns(operation.id) || connections != originalConnections) return@launch
            val unavailable=originalConnections.map { it.id }.toSet() - successfulConnections
            newSessions += sessions.filter { it.target.connectionId in unavailable }.map { old -> old.copy(raw=JSONObject(old.raw.toString()).put("last_known",true)) }
            newMachines += machines.filter { it.connectionId in unavailable }.map { it.copy(online=false,lastKnown=true) }
            newProjects += projects.filter { it.key.connectionId in unavailable }
            accountsWriter?.edit { old -> AccountSnapshots.merge(old,newAccounts,successfulConnections,originalConnections.map { it.id }.toSet()) }
            sessions = newSessions; machines = newMachines.map { MachineNames.apply(it,messageState.machineAliases) }; machineOperations = newOperations
            refreshReadAttention();adoptSharedReads()
            val index=withContext(Dispatchers.Default) { ConversationIndex.capture(newSessions,newMachines) }
            if(connections == originalConnections) editState { state -> state.copy(conversationIndex=index) }
            projects = newProjects; projectWarnings = warnings
            watchHistoryHeads()
            selectedProject?.let { chosen -> selectedProject = projects.find { it.key == chosen.key } ?: chosen }
            sessionProjectScope?.let { chosen -> sessionProjectScope = projects.find { it.key == chosen.key } ?: chosen }
            pendingNotification?.let { (connection, computer, name) ->
                sessions.find { it.target.connectionId == connection && it.target.computerId == computer && it.target.session == name && it.target.archiveId.isBlank() }?.let {
                    pendingNotification = null; open(it)
                }
            }
        } finally {
            if (catalogGate.finish(operation.id)) {
                catalogTimer?.cancel(); catalogProgress = false; catalogBusy = false
                if (connections != originalConnections) refresh()
            }
        }
        }
    }
    fun openProject(project: ProjectSummary) { selectedProject = project }
    fun clearProject() { selectedProject = null }
    fun open(session: Session, preserveProject: Boolean = false) {
        contextNotice="";actionResultNotice="";firstUnreadEventId="";unreadBoundaryCaptured=false;restoreHistoryAttempt="";pendingHistoryPosition="";pendingHistoryPublication="";lastHistoryObservation=0L
        hasOlder=false;hasNewer=false;historyEpoch="";historyIndexing=false;historyComplete=false;olderError=""
        if(session.target.agentId.isBlank()) parentSelection=null
        cancelCompactContinuation(); clearExpectation = null
        flushDrafts()
        if (!preserveProject) selectedProject = null
        navigationId++; cancelDetail()
        selected = session; activityVerified = false; historyCached = false
        activeFrame = null; activity = null; conversationEvents = emptyList(); conversationQuestions = emptyList(); conversationOutgoing = emptyList(); conversationHistoryTruncated = false
        val warm = frames[session.target.key]?.takeIf { if (it.observedAt == 0L) historyCache.getMemory(it.target) != null else System.currentTimeMillis() - it.observedAt in 0..7L * 24 * 60 * 60 * 1000 }
        if (warm == null) frames.remove(session.target.key)
        if (warm != null) installFrame(warm, false)
        else if (!demo) {
            val navigation = navigationId
            viewModelScope.launch {
                val raw = runCatching { historyCache.read(session.target) }.getOrNull() ?: return@launch
                val frame = withContext(Dispatchers.Default) {
                    val decoded = ConversationDisplayCache.decode(raw)
                    Frame(session.target, decoded.raw, decoded.events, NativeParser.questions(decoded.raw), 0,
                        raw.toString().toByteArray(Charsets.UTF_8).size, decoded.truncated)
                }
                if (navigationId == navigation && selected?.target == session.target && !activityVerified && activeFrame == null) {
                    rememberFrame(frame); installFrame(frame, false)
                }
            }
        }
        if(!demo && readTrackingReady) {
            if(historyReason(session.target).isBlank() && readState.record(session.target)?.viewport==null) readPositionReady=false
            restoreSavedWindow(session.target,navigationId)
        }
        if (demo) { conversationEvents = Demo.events; conversationQuestions = emptyList() }
        else refreshActivity()
    }
    fun childSendReason(target:Target):String {
        if(target.agentId.isBlank()) return ""
        if(!fresh(target)) return "Refresh this child conversation before sending."
        if("send_agent" !in relayFeatures[target.connectionId].orEmpty() || "send_agent" !in machineFeatures[MachineKey(target.connectionId,target.computerId)].orEmpty()) return "This native adapter supports child history only."
        return if(activity?.optBoolean("can_send") == true) "" else activity?.string("send_reason","can_send_reason").orEmpty().ifBlank { "This child conversation is read-only." }
    }
    fun agentInspectReason(parent:Target,agentId:String):String {
        if(parent.agentId.isNotBlank() || !fresh(parent)) return "Refresh the live parent conversation first."
        if("inspect_agent" !in relayFeatures[parent.connectionId].orEmpty() || "inspect_agent" !in machineFeatures[MachineKey(parent.connectionId,parent.computerId)].orEmpty()) return "This computer does not support child inspection."
        if(agentId.isBlank() || agentId == "main" || agentId.length > 256 || agentId.any { it.isISOControl() || it == '/' || it == '\\' }) return "Invalid child identity."
        if(activity?.optJSONObject("subagents")?.has(agentId) != true && activity?.optJSONArray("subagents")?.objects().orEmpty().none { it.string("id","agent_id") == agentId }) return "This child agent is no longer present."
        return ""
    }
    fun inspectAgent(parent: Target, agentId: String) {
        val reason=agentInspectReason(parent,agentId);if(reason.isNotBlank()) { error=reason;return }
        if(parent.agentId.isNotBlank() || !fresh(parent) || agentId.isBlank() || agentId == "main" || agentId.length > 256 || agentId.any { it.isISOControl() || it == '/' || it == '\\' }) return
        if("inspect_agent" !in relayFeatures[parent.connectionId].orEmpty() || "inspect_agent" !in machineFeatures[MachineKey(parent.connectionId,parent.computerId)].orEmpty()) { error="This computer does not support child inspection.";return }
        val roster=activity?.optJSONObject("subagents")
        if(roster?.has(agentId) != true && activity?.optJSONArray("subagents")?.objects().orEmpty().none { it.string("id","agent_id") == agentId }) { error="The child agent is no longer present.";return }
        val current=selected ?: return
        parentSelection=current
        open(current.copy(target=parent.copy(agentId=agentId,parentConversation=parent.conversation,conversation=parent.conversation + "/" + agentId),title=agentId),true)
    }
    fun openFromNotification(connection: String, computer: String, session: String) {
        pendingNotification = Triple(connection, computer, session); refresh()
    }
    private fun cancelDetail() {
        if(detailReadErrorOwner!=null) error=""
        detailGate.cancel(); detailJob?.cancel(); detailTimer?.cancel()
        detailBusy = false; detailProgress = false
        presentationRevision++; presentationJob?.cancel()
        olderJob?.cancel();olderLoading=false;readPositionReady=readTrackingReady
        processOutputJob?.cancel();processOutputLoading=false;processOutputResult=null;processOutputError=""
        terminalJob?.cancel();terminalVisible=false;terminalLoading=false;terminalVerified=false;terminalState=null
    }
    fun back() {
        if(selected?.target?.agentId?.isNotBlank() == true && parentSelection != null) { val parent=parentSelection!!;parentSelection=null;open(parent,true);return }
        cancelCompactContinuation(); clearExpectation = null
        flushDrafts()
        if (selected != null) { navigationId++; cancelDetail(); selected = null; activity = null; activeFrame = null; activityVerified = false }
        else selectedProject = null
    }
    fun refreshActivity(explicit: Boolean = false) {
        if (demo || terminalVisible) return
        val original = selected ?: return
        pendingActionResult(original.target)?.let { action -> openSessionActionResult(action);return }
        val connection = connections.find { it.id == original.target.connectionId } ?: return
        val navigation = navigationId
        val start = detailGate.begin("$navigation:${original.target.key}", explicit)
        detailProgress = detailGate.visible()
        if (!start.newRequest) return
        val operation = start.operation
        detailBusy = true
        detailJob?.cancel(); detailTimer?.cancel()
        detailTimer = viewModelScope.launch {
            delay(detailGate.remaining(operation.id))
            if (detailGate.owns(operation.id) && navigationId == navigation) detailProgress = detailGate.visible()
        }
        detailJob = viewModelScope.launch {
        try {
            val frame = inspectOnce(connection, original.target).await()
            if (detailGate.owns(operation.id) && navigationId == navigation && selected?.target == original.target) {
                if(detailReadErrorOwner==(original.target to navigation)) error=""
                val clear = clearExpectation?.takeIf { it.navigation == navigation && ContextPolicies.sameSessionRun(it.target, frame.target) }
                if (clear != null && frame.target.conversation != clear.target.conversation && !clear.confirmed) {
                    activityVerified = false; activeFrameVerified = false; historyCached = activity != null
                    return@launch
                }
                val clearOp = clear?.takeIf { it.confirmed && frame.target.conversation.isNotBlank() && frame.target.conversation != it.target.conversation }
                    ?.let { expectation -> messageState.contextOperations.find { it.requestId == expectation.requestId && it.status == "completed" } }
                if (clearOp != null) {
                    val messages = outgoing.filter { it.belongsTo(frame.target) }
                    val questionState = messageState
                    val (merged, questions) = withContext(Dispatchers.Default) {
                        ConversationMerge.merge(frame.target, frame.events, messages) to QuestionTracking.restore(questionState, frame.target, frame.questions)
                    }
                    if (!detailGate.owns(operation.id) || navigationId != navigation || selected?.target != original.target || clearExpectation?.requestId != clearOp.requestId) return@launch
                    frame.presentation = Presentation(messages, merged.events, questions)
                    androidx.compose.runtime.snapshots.Snapshot.withMutableSnapshot {
                        editState { state -> ContextPolicies.promoteClearDraft(state, clearOp, frame.target) }
                        selected = original.copy(target = frame.target, status = frame.raw.string("activity", "phase", "state"), raw = frame.raw)
                        installFrame(frame, true)
                    }
                    clearExpectation = null
                    contextNotice = if (drafts.any { it.target == clearOp.target && it.questionId.isBlank() && it.detachedId.isBlank() })
                        "Context cleared. The previous composition remains in Saved drafts; restore it explicitly when ready." else "Context cleared. Your composition was kept."
                } else {
                    if (clearExpectation?.let { !ContextPolicies.sameSessionRun(it.target, frame.target) } == true) clearExpectation = null
                    selected = original.copy(target = frame.target, status = frame.raw.string("activity", "phase", "state"), raw = frame.raw)
                    editState { state -> state.resolveConversation(original.target, frame.target) }
                    installFrame(frame, true)
                }
            }
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { if (detailGate.owns(operation.id) && navigationId == navigation) { activeFrameVerified = false; activityVerified = false; historyCached = activity != null; error = ReadFailure.message(e, "conversation");detailReadErrorOwner=original.target to navigation;if(!olderLoading) { readPositionReady=true;captureUnreadBoundary(original.target,conversationEvents) } } }
        finally {
            if (detailGate.finish(operation.id)) {
                detailTimer?.cancel(); detailBusy = false; detailProgress = false
            }
        }
        }
    }
    fun draft(target: Target, question: Question? = null): Draft {
        val questionId = question?.id.orEmpty(); val hash = question?.hash.orEmpty()
        return drafts.find { it.target == target && it.questionId == questionId && it.questionHash == hash && it.detachedId.isBlank() }
            ?: emptyDrafts.getOrPut(Triple(target, questionId, hash)) { Draft(target, "", questionId = questionId, questionHash = hash) }
    }
    private fun save(draft: Draft) { editState { state -> MessageChanges.draft(state, draft) } }
    private fun fresh(target: Target): Boolean {
        if(updateInstallPreparing) return false
        val raw = activity ?: return false
        return !terminalVisible && activityVerified && selected?.target == target && target.archiveId.isBlank() &&
            raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation &&
            raw.string("archive_id").isBlank() && raw.string("state") != "archived" && raw.string("agent_id") == target.agentId &&
            (target.agentId.isBlank() || raw.string("parent_conversation_id") == target.parentConversation) &&
            (raw.string("name").isBlank() || raw.string("name") == target.session)
    }
    fun edit(target: Target, text: String, question: Question? = null, answers: String = "") {
        val current = draft(target, question)
        if (target.archiveId.isNotBlank() || current.status != "editing" || !storageReady) return
        if (continuation?.draft?.target == target && (current.text != text || current.answers != answers)) cancelCompactContinuation("Draft changed. Your message will not be sent automatically.")
        val editor = current.copy(text = text, answers = answers, updatedAt = System.currentTimeMillis(), revision = UUID.randomUUID().toString())
        try { editState { state -> MessageChanges.text(state, editor) } } catch (e: Exception) { error = e.message.orEmpty() }
    }
    fun discard(draft: Draft) {
        if (draft.key in receiptFlights || drafts.find { it.key==draft.key }?.let(QuestionTracking::locked)==true || answerInFlight(draft)) return
        if (continuation?.draft?.target == draft.target) cancelCompactContinuation()
        emptyDrafts.remove(Triple(draft.target, draft.questionId, draft.questionHash))
        try { editState { state -> MessageChanges.discard(state, draft) }; flushDrafts() }
        catch (e: Exception) { error = e.message.orEmpty() }
    }
    fun review(draft: Draft) {
        if (draft.key in receiptFlights || drafts.find { it.key==draft.key }?.let(QuestionTracking::locked)==true ||
            answerInFlight(draft) || draft.status == "submitting" && draft.questionId.isBlank()) return
        try { editState { state -> state.copy(drafts = state.drafts.map { if (it.key == draft.key && it.requestId == draft.requestId) it.copy(status = "editing", requestId = "") else it }) }; flushDrafts() }
        catch (e: Exception) { error = e.message.orEmpty() }
    }
    private val answerFlights=mutableStateMapOf<String,String>()
    fun answerInFlight(draft:Draft)=answerFlights[draft.key]==draft.requestId && draft.requestId.isNotBlank()
    fun answerStatus(draft:Draft)=if(draft.status=="submitting" && !answerInFlight(draft)) "uncertain" else draft.status
    private val questionSendGate = QuestionSendGate()
    private var questionSendRefusal by mutableStateOf<QuestionSendRefusal?>(null)
    internal fun questionSendNotice(target: Target, question: Question): String =
        questionSendRefusal?.textFor(target, question).orEmpty()
    private fun refuseQuestionSend(intent: QuestionSendIntent, text: String) {
        questionSendRefusal = QuestionSendRefusal(intent.target, intent.question.id, intent.question.hash, text)
        contextNotice = text
        if (selected?.target == intent.target && conversationQuestions.none { it.id == intent.question.id && it.hash == intent.question.hash })
            error = text
    }
    internal var pendingQuestionSend by mutableStateOf<QuestionSendIntent?>(null); private set
    internal fun questionSendValid(intent: QuestionSendIntent): Boolean =
        fresh(intent.target) && !demo && storageReady && !contextBlocked(intent.target) &&
            intent.target.key !in preparingTargets && connections.any { it.id == intent.target.connectionId } &&
            intent.matches(selected?.target, navigationId,
                conversationQuestions.find { it.id == intent.question.id && it.hash == intent.question.hash },
                draft(intent.target, intent.question)) && intent.draft.key !in answerFlights
    internal fun cancelQuestionSend(intent: QuestionSendIntent) {
        questionSendGate.cancel(intent); pendingQuestionSend = questionSendGate.pending
    }
    internal fun confirmQuestionSend(intent: QuestionSendIntent) {
        if (!questionSendGate.active(intent)) return
        val confirmed = questionSendGate.confirm(intent, questionSendValid(intent))
        pendingQuestionSend = questionSendGate.pending
        if (confirmed == null) { refuseQuestionSend(intent, "The question or draft changed. Nothing was sent. Review it before submitting."); return }
        sendQuestion(intent.target, intent.question, intent.answers, confirmed)
    }
    fun send(target: Target, question: Question? = null, answers: JSONArray? = null) =
        if (question == null) sendMessage(target) else sendQuestion(target, question, answers?.toString())
    private fun sendQuestion(target: Target, question: Question, answerText: String?, consent: QuestionSendIntent? = null) = viewModelScope.launch {
        if(target.agentId.isNotBlank() || !fresh(target) || demo || !storageReady || contextBlocked(target) || target.key in preparingTargets) return@launch
        val native=conversationQuestions.find { it.id==question.id && it.hash==question.hash } ?: return@launch
        if(QuestionSendPolicies.schema(question) != QuestionSendPolicies.schema(native)) return@launch
        val connection=connections.find { it.id==target.connectionId } ?: return@launch
        val original=draft(target,question)
        if(original.status!="editing" || target.run.isBlank() || original.key in answerFlights) return@launch
        val frozenAnswers = answerText ?: original.answers
        if (!QuestionSendPolicies.available(native, target, frozenAnswers)) return@launch
        val intent = consent ?: QuestionSendPolicies.capture(target, navigationId, native, original, frozenAnswers)
        if (!questionSendValid(intent) || intent.answers != frozenAnswers) return@launch
        questionSendRefusal = null
        if (consent == null) {
            val authorized = questionSendGate.request(intent, ContextPresentation.isCold(activity))
            pendingQuestionSend = questionSendGate.pending
            if (authorized == null) return@launch
        }
        val sending=original.begin().copy(answers=frozenAnswers)
        var attempted=false;var refused=false
        answerFlights[sending.key]=sending.requestId;preparingTargets+=target.key
        try {
            save(sending);persistence!!.flush()
            if (consent == null && ContextPresentation.isCold(activity)) {
                durable({ state -> intent.restoreAfterExpiry(state, sending.requestId) })
                refuseQuestionSend(intent, "The prompt cache expired before sending. Review your answer and confirm full context; nothing was sent.")
                return@launch
            }
            val currentDraft = draft(target, question)
            val currentQuestion = conversationQuestions.find { it.id == question.id && it.hash == question.hash }
            check(fresh(target) && !contextBlocked(target) && !demo && storageReady && connections.any { it == connection } &&
                intent.permitsPrepared(selected?.target, navigationId, currentQuestion, currentDraft,
                    sending.requestId, ContextPresentation.isCold(activity), consent != null)) {
                "The selected question changed before sending."
            }
            preparingTargets-=target.key;lastOwnSend=target to sending.requestId
            val payload=target.json().put("request_id",sending.requestId).put("question_id",question.id).put("expected_question_hash",question.hash).put("answers",JSONArray(frozenAnswers))
            val receipt=withTimeout(45_000) {
                attempted=true
                val initial=try { api.submit(connection,target,"answer",payload,sending.requestId) }
                    catch(e:RelayException) { refused=e.status in listOf(400,401,403,404,409,413,429);throw e }
                api.await(connection,initial)
            }
            settle(sending,receipt)
        } catch(e:Exception) {
            val status=if(!attempted || refused) "failed" else "uncertain"
            withContext(NonCancellable) {
                try { durable({ state -> state.copy(drafts=state.drafts.map { existing ->
                    if(existing.key==sending.key && existing.requestId==sending.requestId && existing.generation==sending.generation && !QuestionTracking.locked(existing)) existing.copy(status=status) else existing }) }) }
                catch(_:Exception) { error="Could not save the answer outcome. Its original request remains recoverable." }
            }
            error=if(status=="failed") "Answer not sent. ${e.message.orEmpty()}" else "Answer delivery is unknown. Check the original receipt; nothing has been retried."
            if(e is CancellationException && e !is kotlinx.coroutines.TimeoutCancellationException) throw e
        } finally {
            if(answerFlights[sending.key]==sending.requestId) answerFlights.remove(sending.key)
            preparingTargets-=target.key
        }
    }
    fun sendingBlocked(target: Target) = contextBlocked(target) || target.key in preparingTargets || outgoing.any { it.belongsTo(target) && it.blocksSending } || pickerDraft?.target == target
    fun sendMessage(target: Target, expectedDraft: Draft? = null, expectedCompactionId: String = "", allowColdCache: Boolean = false) =
        sendMessageInternal(target, expectedDraft, expectedCompactionId, allowColdCache, "")
    private fun sendMessageInternal(target: Target, expectedDraft: Draft?, expectedCompactionId: String, allowColdCache: Boolean, continuationId: String) = viewModelScope.launch {
        if (childSendReason(target).isNotBlank() || !fresh(target) || demo || !storageReady || NativeParser.messageBlockReason(activity).isNotBlank() || sendingBlocked(target)) { if (continuationId.isNotBlank()) cancelCompactContinuation("The conversation is not ready. Your draft was kept without sending."); return@launch }
        val connection = connections.find { it.id == target.connectionId } ?: return@launch
        val original = draft(target)
        if (expectedDraft != null && !ContextPolicies.sameDraft(expectedDraft, original)) { contextNotice = "Your composition changed. Review it before sending."; return@launch }
        if (expectedCompactionId.isNotBlank() && (continuationId != expectedCompactionId || continuation?.requestId != continuationId ||
            ContextPolicies.compactStatus(messageState.contextOperations.find { it.requestId == continuationId } ?: return@launch, activity ?: return@launch) != "completed")) return@launch
        if (expectedCompactionId.isBlank() && !allowColdCache && ContextPresentation.isCold(activity)) { contextNotice = "Choose how to continue with the cold cache before sending."; return@launch }
        val intent = continuation?.takeIf { it.requestId == continuationId }
        if (intent != null) continuationEnqueuing = intent.requestId
        val id = UUID.randomUUID().toString(); val at = System.currentTimeMillis() / 1000.0
        preparingTargets += target.key; deliveryFlights += id
        val written = try { durable({ state -> state.enqueue(original, id, at) }, { state, committed -> MessageChanges.enqueueAcknowledged(state, original, committed, id) }) }
        catch (e: Exception) { error = "Could not save the outgoing message. It was not sent. Your composition is preserved."; deliveryFlights -= id; syncOutgoing(); preparingTargets -= target.key; continuationEnqueuing = ""; cancelCompactContinuation(); return@launch }
        preparingTargets -= target.key
        val message = written.outgoing.first { it.requestId == id }
        lastOwnSend = target to id
        var attempted = false; var rejected: RelayException? = null
        try {
            val files = attachmentStore.payload(message.attachments)
            check(fresh(target) && connections.any { it.id == connection.id }) { "The session changed before transport. Review the saved message." }
            if (intent != null) {
                val cleared = written.drafts.first { it.key == original.key }
                check(continuation?.requestId == intent.requestId && intent.permitsTransport(selected?.target, navigationId, android.os.SystemClock.elapsedRealtime(), cleared, draft(target)) &&
                    ContextPolicies.compactStatus(messageState.contextOperations.first { it.requestId == intent.requestId }, activity!!) == "completed") { "Compact continuation was canceled. The captured message was not sent and remains recoverable." }
            }
            val payload = target.json().put("request_id", id).put("text", message.text).put("attachments", files)
            if (expectedCompactionId.isNotBlank()) payload.put("expected_compaction_id", expectedCompactionId)
            attempted = true
            val initial = try { api.submit(connection, target, "send", payload, id) }
            catch (e: RelayException) { if (e.status in listOf(400, 401, 403, 404, 409, 413, 429)) rejected = e; throw e }
            settleOutgoing(message, api.await(connection, initial))
        } catch (e: Exception) {
            val failure = rejected?.message ?: if (attempted) "Delivery is unconfirmed. Check its receipt before sending again." else
                (e as? AttachmentException)?.message ?: "The saved message was not sent. Review it before trying again."
            try { durable({ state -> MessageChanges.result(state, id) { it.copy(status = if (attempted && rejected == null) "uncertain" else "failed", error = failure) } }) }
            catch (_: Exception) { error = "Could not save the delivery result. The original outgoing message remains recoverable. Check its receipt." }
            if (e is CancellationException) throw e
        } finally { if (intent != null) { continuationEnqueuing = ""; continuation = null }; deliveryFlights -= id; syncOutgoing() }
    }
    private suspend fun settleOutgoing(message: OutgoingMessage, receipt: JSONObject) {
        val result = receipt.optJSONObject("result"); val state = receipt.string("state")
        val confirmed = receipt.string("request_id") == message.requestId && state == "completed" && result?.string("status") in listOf("submitted", "queued")
        val rejected = state == "failed" && receipt.string("request_id") == message.requestId
        try { durable({ current -> MessageChanges.result(current, message.requestId) { existing ->
            if (confirmed) existing.copy(status = "submitted", submittedAt = result!!.optDouble("submitted_at", existing.createdAt), submittedText = result.string("submitted_text").ifBlank { existing.text.trim() }, error = "")
            else existing.copy(status = if (rejected) "failed" else "uncertain", error = if (rejected) "The computer rejected this message. Review it before trying again." else "Delivery is unconfirmed. Check its receipt before sending again.")
        } }) } catch (_: Exception) { error = "Could not save the delivery result. Check its receipt; the outgoing message remains recoverable."; return }
        if (selected?.target?.let(message::belongsTo) == true) refreshActivity()
    }
    fun checkOutgoing(message: OutgoingMessage) = viewModelScope.launch {
        val connection = connections.find { it.id == message.target.connectionId } ?: return@launch
        val flight = "outgoing:${message.requestId}"
        if (flight in receiptFlights || message.status == "sending") return@launch
        receiptFlights += flight
        try { settleOutgoing(message, api.await(connection, api.receipt(connection, message.requestId))) }
        catch (e: CancellationException) { throw e }
        catch (_: Exception) { error = "Receipt unavailable. The outgoing message remains saved." }
        finally { receiptFlights -= flight }
    }
    fun prepareOutgoing(message: OutgoingMessage) {
        if ("outgoing:${message.requestId}" in receiptFlights) return
        val restored = Draft(message.target, message.text, attachments = message.attachments)
        val parkedId = UUID.randomUUID().toString()
        try { editState { state ->
            val current = state.outgoing.find { it.requestId == message.requestId }
            if (current == null || current.status !in listOf("failed", "uncertain")) state else {
                val editor = state.drafts.find { it.target == message.target && it.questionId.isBlank() && it.detachedId.isBlank() }
                val parked = editor?.takeIf { it.text.isNotBlank() || it.attachments.isNotEmpty() }?.copy(detachedId = parkedId)
                state.copy(drafts = state.drafts.filterNot { it.key == restored.key } + listOfNotNull(parked) + restored,
                    outgoing = state.outgoing.map { if (it.requestId == message.requestId) it.copy(status = "reviewed") else it })
            }
        }; flushDrafts() } catch (e: Exception) { error = e.message.orEmpty() }
    }
    fun beginAttachmentPick(target: Target, onReady: (String) -> Unit) {
        if (target.archiveId.isNotBlank() || childSendReason(target).isNotBlank() || demo || !storageReady || target.key in preparingTargets || attachmentPreparing || pickerDraft != null || importJob?.isActive == true || attachmentImporting) return
        if (continuation?.draft?.target == target) cancelCompactContinuation("Selecting files canceled the automatic continuation. Your composition is preserved.")
        val origin = draft(target); if (origin.status != "editing") return
        val id = UUID.randomUUID().toString()
        save(origin); attachmentPreparing = true; preparingTargets += target.key
        viewModelScope.launch {
            try {
                durable({ state -> state.copy(pickerDraft = origin, pickerId = id) })
                onReady(id)
            } catch (_: Exception) { error = "Could not save the file selection. Your draft is preserved." }
            finally { attachmentPreparing = false; preparingTargets -= target.key }
        }
    }
    fun finishAttachmentPick(uris: List<Uri>, selectionId: String) {
        if (selectionId != pickerId) return
        val origin = pickerDraft ?: return
        if (importJob?.isActive == true) return
        importJob = viewModelScope.launch {
            attachmentImporting = true
            val added = mutableListOf<Attachment>()
            val revision = UUID.randomUUID().toString(); val detachedId = UUID.randomUUID().toString()
            try {
                require(origin.attachments.size + uris.size <= AttachmentPolicy.MAX_FILES) { "Attach at most 8 files." }
                for (uri in uris) added += attachmentStore.importDocument(uri, origin.attachments + added)
                val newIds = added.map { it.id }.toSet()
                val result = withContext(NonCancellable) { durable({ state ->
                    check(state.pickerId == selectionId) { "File selection was canceled." }
                    state.selectionResult(origin, added).let { next -> next.copy(drafts = next.drafts.map { draft ->
                        if (draft.attachments.any { it.id in newIds }) draft.copy(revision = revision, detachedId = if (draft.detachedId.isBlank()) "" else detachedId) else draft
                    }) }
                }, { current, written -> MessageChanges.selectionAcknowledged(current, origin, written, newIds, selectionId) }) }
                if (result.drafts.any { it.detachedId == detachedId }) error = "The original composition changed. Its selected files are preserved in Saved drafts."
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) { error = (e as? AttachmentException)?.message ?: e.message ?: "Could not import the selected files. The original draft is preserved." }
            finally {
                withContext(NonCancellable) {
                    runCatching { if (pickerId == selectionId) editState { state -> if (state.pickerId == selectionId) state.copy(pickerDraft = null, pickerId = "") else state } }
                    flushDrafts()
                    val protected = protectedAttachmentIds()
                    added.forEach { runCatching { attachmentStore.remove(it.id, protected) } }
                }
                attachmentImporting = false
            }
        }
    }
    fun cancelAttachmentPick() {
        importJob?.cancel()
        try { editState { it.copy(pickerDraft = null, pickerId = "") }; flushDrafts() } catch (e: Exception) { error = e.message.orEmpty() }
    }
    private fun protectedAttachmentIds(): Set<String> {
        fun refs(state: MessageState) = state.drafts.flatMap { it.attachments } + state.outgoing.flatMap { it.attachments } + state.pickerDraft?.attachments.orEmpty()
        return (refs(messageState) + persistence?.durableState?.let(::refs).orEmpty()).map { it.id }.toSet()
    }
    fun removeAttachment(target: Target, id: String) = viewModelScope.launch {
        if (target.archiveId.isNotBlank() || pickerDraft?.target == target) return@launch
        if (continuation?.draft?.target == target) cancelCompactContinuation()
        val current = draft(target); if (current.status != "editing") return@launch
        try {
            editState { state -> state.copy(drafts = state.drafts.map { if (it.key == current.key && it.generation == current.generation && it.status == "editing") it.copy(attachments = it.attachments.filterNot { file -> file.id == id }) else it }) }
            persistence!!.flush()
            attachmentStore.remove(id, protectedAttachmentIds())
        } catch (e: Exception) { error = e.message.orEmpty() }
    }
    fun restoreDetached(draft: Draft) {
        if(QuestionTracking.locked(draft)) return
        if (draft.detachedId.isBlank()) return
        val restored = draft.copy(detachedId = "", generation = UUID.randomUUID().toString(), revision = UUID.randomUUID().toString())
        val parkedId = UUID.randomUUID().toString()
        try { editState { state ->
            val editor = state.drafts.find { it.key == restored.key }
            val parked = editor?.takeIf { it.text.isNotBlank() || it.attachments.isNotEmpty() }?.copy(detachedId = parkedId)
            state.copy(drafts = state.drafts.filterNot { it.key == draft.key || it.key == restored.key } + listOfNotNull(parked) + restored)
        }; flushDrafts() } catch (e: Exception) { error = e.message.orEmpty() }
    }
    private suspend fun settle(draft: Draft, receipt: JSONObject) {
        durable({ state -> QuestionTracking.settle(state,draft,receipt) })
        val latest=drafts.find { it.key==draft.key && it.requestId==draft.requestId }
        if(latest?.status=="submitted" || draft.questionId=="__interrupt" && latest==null) refreshActivity()
        else error=receipt.string("error").ifBlank { "Delivery is unknown. Check the original receipt and saved answer." }
    }
    fun checkReceipt(draft: Draft) = viewModelScope.launch {
        val connection=connections.find { it.id==draft.target.connectionId } ?: return@launch
        if(draft.requestId.isBlank() || draft.key in receiptFlights || answerInFlight(draft)) return@launch
        receiptFlights+=draft.key
        try { settle(draft,withTimeout(45_000) { api.await(connection,api.receipt(connection,draft.requestId)) }) }
        catch(e:Exception) { if(e is CancellationException && e !is kotlinx.coroutines.TimeoutCancellationException) throw e;error="Receipt unavailable. The answer remains saved." }
        finally { receiptFlights-=draft.key }
    }
    fun interrupt(target: Target, turn: Double) = viewModelScope.launch {
        if(target.agentId.isNotBlank()) return@launch
        val raw = activity ?: return@launch
        if (!fresh(target) || contextBlocked(target) || !raw.optBoolean("interrupt_supported") || raw.optDouble("turn_started") != turn || target.key in preparingTargets) return@launch
        val connection = connections.find { it.id == target.connectionId } ?: return@launch
        val draft = Draft(target, "Stop current turn", questionId = "__interrupt", questionHash = turn.toString()).begin()
        if (drafts.any { it.target == target && it.questionId == "__interrupt" && it.status != "editing" }) return@launch
        preparingTargets += target.key; save(draft)
        try { persistence!!.flush() }
        catch (_: Exception) { error = "Could not save the interruption request. It was not sent."; preparingTargets -= target.key; return@launch }
        preparingTargets -= target.key
        try {
            check(fresh(target) && activity?.optDouble("turn_started") == turn)
            val payload = target.json().put("request_id", draft.requestId).put("expected_turn_started", turn)
            settle(draft, api.await(connection, api.submit(connection, target, "interrupt", payload, draft.requestId)))
        } catch (_: Exception) {
            editState { state -> state.copy(drafts = state.drafts.map { if (it.key == draft.key && it.requestId == draft.requestId) it.copy(status = "uncertain") else it }) }
            flushDrafts(); error = "Interruption is unconfirmed. Check the conversation before trying again."
        }
    }
    fun updateNotifications(value: Boolean) = viewModelScope.launch {
        try { withContext(Dispatchers.IO) { store.setNotificationEnabled(value) }; notifications = value }
        catch (_: Exception) { notifications = store.notificationEnabled(); error = "Notification choices could not be saved. Try again." }
    }
}
