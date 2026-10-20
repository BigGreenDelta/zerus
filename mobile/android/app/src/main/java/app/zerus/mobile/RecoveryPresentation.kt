package app.zerus.mobile

import org.json.JSONObject
import kotlin.math.ceil

data class RecoveryAttempt(val at: Double?,val status: String,val attempt: Int?,val reason: String)
data class NativeRecovery(val id: String,val state: String,val reason: String,val action: String,val acknowledged: Boolean,
    val attempt: Int?,val classAttempt: Int?,val delays: List<Int>,val dueAt: Double?,val notBefore: Double?,
    val history: List<RecoveryAttempt>,val recorded: Boolean,val resumed: Boolean = false) {
    fun status(now: Double): String = RecoveryPresentation.status(state,acknowledged,dueAt,now,resumed)
    val attempts: String get() = if(attempt == null) "Attempts not reported" else if(delays.lastOrNull() == 0)
        "$attempt attempts (repeats until stopped)" else if(delays.isEmpty()) "$attempt attempts" else
        "${classAttempt ?: attempt}/${delays.size - if(delays.last() <= 0) 1 else 0} attempts"
    fun retryTimeAllowed(now: Double) = notBefore == null || now >= notBefore
}

/** Literal native recovery job. No provider retry schedule or success is inferred. */
object RecoveryPresentation {
    fun from(raw: JSONObject?,target: Target,verified: Boolean): NativeRecovery? {
        val job = raw?.optJSONObject("recovery") ?: return null
        val state = job.opt("state") as? String ?: return null
        if(state.isBlank()) return null
        val identity = job.optJSONArray("identity")
        val exact = raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation &&
            job.string("name") == target.session && identity?.opt(0) == target.run && identity?.opt(1) == target.conversation
        val delays = job.optJSONArray("delays")?.let { values -> (0 until values.length().coerceAtMost(16)).mapNotNull { integer(values.opt(it),-1,86400) } }.orEmpty()
        val history = job.optJSONArray("history")?.objects().orEmpty().takeLast(32).map { entry ->
            RecoveryAttempt(time(entry.opt("at")),status(entry.string("state"),entry.optBoolean("acknowledged"),null,0.0),
                integer(entry.opt("attempt"),0,Int.MAX_VALUE),entry.string("reason"))
        }
        return NativeRecovery(job.string("id"),state,job.string("reason"),job.string("action"),job.optBoolean("acknowledged"),
            integer(job.opt("attempt"),0,Int.MAX_VALUE),integer(job.opt("class_attempt"),0,Int.MAX_VALUE),delays,
            time(job.opt("due_at")),time(job.opt("not_before")),history,!verified || !exact || target.archiveId.isNotBlank() || target.agentId.isNotBlank(),
            resumed(job))
    }
    /** The agent accepted the continuation and the provider served it again. The episode stays
     *  open until the turn ends only to keep its attempt budget; the turn is ordinary work. */
    fun resumed(job: JSONObject?): Boolean = job != null && job.string("state") == "retrying" && job.optBoolean("acknowledged") &&
        (time(job.opt("resumed_at")) ?: 0.0) > 0.0
    private fun integer(value: Any?,min: Int,max: Int): Int? = (value as? Number)?.let {
        val number = it.toDouble()
        if(number.isFinite() && number in min.toDouble()..max.toDouble() && number % 1 == 0.0) number.toInt() else null
    }
    private fun time(value: Any?): Double? = (value as? Number)?.toDouble()?.takeIf { it.isFinite() && it in 0.0..253_402_300_799.0 }
    fun status(state: String,acknowledged: Boolean,due: Double?,now: Double,resumed: Boolean = false): String = when(state) {
        "waiting" -> due?.let { ceil((it - now).coerceAtLeast(0.0)).toLong() }?.takeIf { it > 0 }?.let { "Retry in $it s" } ?: "Waiting to retry"
        "dispatching" -> "Sending recovery request…"
        "retrying" -> if(!acknowledged) "Waiting for delivery confirmation…" else if(resumed) "Agent resumed after recovery" else "Recovery in progress"
        "exhausted" -> "Automatic attempts exhausted"
        "uncertain" -> "Check the agent before retrying"
        "blocked" -> "Automatic recovery needs attention"
        "cancelled" -> "Automatic recovery cancelled"
        "succeeded" -> "Recovered"
        else -> state.replace('_',' ').replaceFirstChar { it.uppercase() }
    }
}
