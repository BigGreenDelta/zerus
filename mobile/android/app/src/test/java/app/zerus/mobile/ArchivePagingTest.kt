package app.zerus.mobile

import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import java.time.LocalDate
import java.time.ZoneId

class ArchivePagingTest {
    private val key=MachineKey("workspace","computer")
    private fun row(index:Int):Session { val raw=org.json.JSONObject().put("name","codex/example/$index").put("archive_id","10000000-0000-4000-8000-${index.toString().padStart(12,'0')}").put("state","archived")
        return Session(Target(key.computerId,raw.getString("name"),"r$index","c$index",key.connectionId,raw.getString("archive_id")),"Title","codex","archived","example","",0,raw) }
    @Test fun inclusiveDaysRespectDstAndInvalidRange() {
        val zone=ZoneId.of("America/New_York")
        val day=LocalDate.of(2026,3,8);val range=ArchiveRange.days(day,day,zone)
        assertTrue(range.valid);assertEquals(23*3600L,range.until-range.from)
        assertEquals(25*3600L,ArchiveRange.days(LocalDate.of(2026,11,1),LocalDate.of(2026,11,1),zone).let{it.until-it.from})
        assertFalse(ArchiveRange.days(day.plusDays(1),day,zone).valid)
        assertEquals(ArchiveRange(),ArchiveRange.period(ArchivePeriod.All,day,day,day,zone))
    }
    @Test fun archiveIsLazyAndExplicitMoreIsDeduplicated()=runBlocking {
        var calls=0
        val pager=ArchivePaging(this){_,q -> calls++;assertEquals(50,q.getInt("limit"));if(q.getString("cursor").isBlank())ArchivePage(listOf(row(1)),"next",2)else ArchivePage(listOf(row(1),row(2)),"",2)}
        pager.configure(setOf(key),"",false);yield();assertEquals(0,calls)
        pager.configure(setOf(key),"",true);yield();assertEquals(1,calls);assertEquals(1,pager.rows.size);assertTrue(pager.hasMore)
        pager.configure(setOf(key),"",true);yield();assertEquals(1,calls)
        pager.loadMore();pager.loadMore();yield();assertEquals(2,calls);assertEquals(2,pager.rows.size);assertFalse(pager.hasMore)
    }
    @Test fun changedRangeIgnoresLateUncancellableResponseAndResetsCursor()=runBlocking {
        val requests=mutableListOf<org.json.JSONObject>();val started=CompletableDeferred<Unit>();val gate=CompletableDeferred<Unit>()
        val pager=ArchivePaging(this){_,q -> requests+=q;val call=requests.size;if(call==1){started.complete(Unit);withContext(NonCancellable){gate.await()}};ArchivePage(listOf(row(call)),"",1)}
        pager.configure(setOf(key),"",true);started.await()
        pager.dates(ArchivePeriod.Today);yield();assertEquals(2,requests.size);assertEquals("",requests.last().getString("cursor"))
        gate.complete(Unit);yield();assertEquals(1,pager.rows.size);assertEquals("r2",pager.rows.first().target.run)
    }
    @Test fun reopeningAfterCancelledInitialPageResumesWithoutLoadingAll()=runBlocking {
        var calls=0
        val pager=ArchivePaging(this){_,_ -> calls++;if(calls==1)delay(10000);ArchivePage(listOf(row(calls)),"next",2043)}
        pager.configure(setOf(key),"",true);yield();assertEquals(1,calls)
        pager.pause();yield();assertFalse(pager.loading)
        pager.configure(setOf(key),"",true);yield();assertEquals(2,calls);assertEquals(1,pager.rows.size);assertTrue(pager.hasMore)
    }
    @Test fun errorsRetrySameCursorAndDisconnectDropsOldRows()=runBlocking {
        var calls=0
        val pager=ArchivePaging(this){_,q -> calls++;when(calls){1->ArchivePage(listOf(row(1)),"older",2);2->throw IllegalStateException("offline");else->{assertEquals("older",q.getString("cursor"));ArchivePage(listOf(row(2)),"",2)}}}
        pager.configure(setOf(key),"",true);yield();pager.loadMore();yield();assertEquals(1,pager.rows.size);assertTrue(pager.errors.isNotEmpty())
        pager.loadMore();yield();assertEquals(1,pager.rows.size);assertEquals("r2",pager.rows.single().target.run);assertTrue(pager.errors.isEmpty())
        pager.configure(emptySet(),"",true);assertTrue(pager.rows.isEmpty())
    }
    @Test fun boundedWindowsReachAllRecordsAndReturnWithoutGrowingCursorHistory()=runBlocking {
        val total=257;var calls=0
        val pager=ArchivePaging(this){_,q ->
            calls++;val start=q.getString("cursor").removePrefix("page_").toIntOrNull()?:0
            ArchivePage((start until minOf(start+50,total)).map(::row),if(start+50<total)"page_${start+50}"else"",total,if(start>0)"page_${maxOf(0,start-50)}"else"",start)
        }
        pager.configure(setOf(key),"needle",true);yield()
        val seen=mutableSetOf<String>()
        for(start in 0 until total step 50){
            assertEquals(start,pager.windows.getValue(key).position);assertTrue(pager.rows.size<=50)
            seen+=pager.rows.map{it.target.key};assertEquals(start>0,pager.hasNewer)
            if(pager.hasMore){pager.loadMore();yield()}
        }
        assertEquals(total,seen.size);assertTrue(pager.partial);assertEquals(6,calls)
        for(start in listOf(200,150,100,50,0)){
            pager.loadNewer();yield();assertEquals(start,pager.windows.getValue(key).position)
            assertEquals("r$start",pager.rows.first().target.run);assertEquals(50,pager.rows.size)
        }
        assertFalse(pager.hasNewer);assertEquals(11,calls)
    }
}
