package app.zerus.mobile

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

object HistoryReadPaths {
    data class Result(val page:HistoryPage,val verified:Boolean)
    /** A corrupt or incompatible display cache is a miss, never evidence for a live action. */
    suspend fun saved(target:Target,cached:suspend ()->JSONObject?):HistoryPage? {
        try {
            val raw=cached() ?: return null
            return withContext(Dispatchers.Default) { HistoryPage.cached(target,raw) }
        } catch(error:CancellationException) { throw error }
        catch(error:Exception) { return null }
    }
    data class Reopened(val result:Result,val head:Boolean)
    /** A reset index rejects every cursor of its previous epoch, the connector reports that only as a failed
     *  command, and the page cache replays the old epoch. Reopen the verified head once it reports another epoch. */
    suspend fun reopening(epoch:String?,read:suspend ()->Result,head:suspend ()->HistoryPage):Reopened {
        var failure:Exception?=null
        val first=try { read() } catch(error:CancellationException) { throw error } catch(error:Exception) { failure=error;null }
        if(!epoch.isNullOrBlank() && first?.verified!=true) {
            val latest=try { head() } catch(error:CancellationException) { throw error } catch(_:Exception) { null }
            if(latest!=null && latest.epoch.isNotBlank() && latest.epoch!=epoch) return Reopened(Result(latest,true),true)
        }
        return Reopened(first ?: throw failure!!,false)
    }
    suspend fun read(target:Target,network:suspend ()->HistoryPage,cached:suspend ()->JSONObject?):Result {
        try { return Result(network(),true) } catch(error:CancellationException) { throw error }
        catch(error:Exception) {
            val page=saved(target,cached) ?: throw error
            return Result(page,false)
        }
    }
}
