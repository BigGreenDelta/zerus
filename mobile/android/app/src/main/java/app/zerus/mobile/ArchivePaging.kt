package app.zerus.mobile

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.time.LocalDate
import java.time.ZoneId

enum class ArchivePeriod(val caption: String) { All("All dates"), Today("Today"), Week("7 days"), Month("30 days"), Custom("Date range") }
data class ArchiveRange(val from: Long = 0, val until: Long = 253402300800L) {
    val valid get() = from in 0 until until && until <= 253402300800L
    companion object {
        fun days(first: LocalDate, last: LocalDate, zone: ZoneId = ZoneId.systemDefault()) =
            ArchiveRange(first.atStartOfDay(zone).toEpochSecond(), last.plusDays(1).atStartOfDay(zone).toEpochSecond())
        fun period(period: ArchivePeriod, first: LocalDate, last: LocalDate, today: LocalDate = LocalDate.now(), zone: ZoneId = ZoneId.systemDefault()) = when(period) {
            ArchivePeriod.All -> ArchiveRange()
            ArchivePeriod.Today -> days(today,today,zone)
            ArchivePeriod.Week -> days(today.minusDays(6),today,zone)
            ArchivePeriod.Month -> days(today.minusDays(29),today,zone)
            ArchivePeriod.Custom -> days(first,last,zone)
        }
    }
}
data class ArchivePage(val sessions: List<Session>, val cursor: String, val total: Int, val previous: String = "", val position: Int? = null)
data class ArchiveWindow(val position: Int?, val count: Int, val total: Int)

/** Generation checks apply even when a transport cannot cancel an already claimed read. */
class ArchivePaging(private val scope: CoroutineScope, private val reset: () -> Unit = {},
    private val load: suspend (MachineKey, JSONObject) -> ArchivePage) {
    var period by mutableStateOf(ArchivePeriod.All); private set
    var first by mutableStateOf(LocalDate.now().minusDays(29)); private set
    var last by mutableStateOf(LocalDate.now()); private set
    private data class Page(val rows: List<Session> = emptyList(), val cursor: String = "", val total: Int = 0,
        val loaded: Boolean = false, val error: String = "", val previous: String = "", val position: Int? = null)
    private var pages by mutableStateOf<Map<MachineKey,Page>>(emptyMap())
    private var keys: Set<MachineKey> = emptySet()
    private var search = ""
    private var active = false
    private var currentRange = ArchiveRange()
    private var generation = 0L
    private val jobs = mutableMapOf<MachineKey,Job>()
    var loading by mutableStateOf(false); private set
    val rows get() = pages.values.flatMap { it.rows }
    val total get() = pages.values.sumOf { it.total }
    val totalsKnown get() = keys.isNotEmpty() && pages.keys.containsAll(keys) && pages.values.all { it.loaded }
    val errors get() = pages.filterValues { it.error.isNotBlank() }.mapValues { it.value.error }
    val hasMore get() = keys.any { pages[it]?.let { p -> !p.loaded || p.cursor.isNotBlank() } != false }
    val hasNewer get() = pages.values.any { it.previous.isNotBlank() }
    val partial get() = !totalsKnown || rows.size < total
    val windows get() = pages.filterValues { it.loaded }.mapValues { (_,p) -> ArchiveWindow(p.position,p.rows.size,p.total) }
    val range get() = ArchiveRange.period(period,first,last)
    fun configure(machines: Set<MachineKey>, query: String, visible: Boolean) {
        val changed = keys != machines || search != query || currentRange != range
        active = visible
        if(changed || !visible && jobs.isNotEmpty()) {
            generation++;jobs.values.forEach { it.cancel() };jobs.clear();loading=false
            if(changed) { pages=emptyMap(); if(visible)reset() }
            keys=machines;search=query;currentRange=range
        }
        if(visible && !loading && currentRange.valid && keys.any { key -> pages[key]?.let { !it.loaded && it.error.isBlank() } != false }) loadMore()
    }
    fun pause() { configure(keys,search,false) }
    fun dates(value: ArchivePeriod, from: LocalDate = first, through: LocalDate = last) {
        period=value;first=from;last=through
        configure(keys,search,active)
    }
    fun refresh() {
        generation++;jobs.values.forEach { it.cancel() };jobs.clear();loading=false;pages=emptyMap();currentRange=range
        if(active){reset();loadMore()}
    }
    fun loadMore() = navigate(false)
    fun loadNewer() = navigate(true)
    private fun navigate(newer: Boolean) {
        if(!active || !currentRange.valid || loading)return
        val own=generation;val dates=currentRange;val text=search
        for(key in keys) {
            val page=pages[key] ?: Page()
            val cursor=if(newer)page.previous else page.cursor
            if(newer && cursor.isBlank() || !newer && page.loaded && cursor.isBlank()) continue
            pages=pages+(key to page.copy(error=""))
            loading=true
            jobs[key]=scope.launch(start=CoroutineStart.LAZY) {
                try {
                    val query=JSONObject().put("from",dates.from).put("until",dates.until).put("limit",50).put("cursor",cursor).put("query",text)
                    val result=load(key,query)
                    check(result.sessions.size<=50 && result.total>=result.sessions.size && result.cursor.length<=1024 && result.previous.length<=1024 &&
                        (result.position==null || result.position>=0 && result.position.toLong()+result.sessions.size<=result.total.toLong()) &&
                        (result.sessions.isNotEmpty() || result.cursor.isBlank()) && (cursor.isBlank() || result.cursor!=cursor && result.previous!=cursor)) { "Computer returned an invalid archive page. Refresh the archive." }
                    check(result.sessions.isNotEmpty() || page.rows.isEmpty()) { "Archive changed. Refresh to start a new view." }
                    if(generation==own) {
                        // Replace the visible window. No retained row/cursor history grows with browsing.
                        pages=pages+(key to Page(result.sessions.distinctBy { it.target.key },result.cursor,result.total,true,previous=result.previous,position=result.position))
                        reset()
                    }
                } catch(e: CancellationException) { throw e }
                catch(e:Exception) { if(generation==own)pages=pages+(key to page.copy(error=ReadFailure.message(e,"archive"))) }
                finally { if(generation==own){jobs.remove(key);loading=jobs.isNotEmpty()} }
            }
            jobs[key]?.start()
        }
    }
}
