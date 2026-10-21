package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.time.LocalDate
import java.time.ZoneOffset
import java.time.temporal.ChronoUnit
import java.util.UUID

internal data class AnalyticsRange(val from: LocalDate, val through: LocalDate) {
    init { require(ChronoUnit.DAYS.between(from, through) in 0..89) }
    fun json(): JSONObject = JSONObject().put("schema", 1).put("from", from.toString()).put("through", through.toString())
    fun contains(day: LocalDate) = day >= from && day <= through
    companion object {
        fun recent(days: Int, today: LocalDate = LocalDate.now(ZoneOffset.UTC)) = AnalyticsRange(today.minusDays(days.toLong() - 1), today)
    }
}

internal data class AnalyticsHistogram(val counts: List<Long>, val count: Long, val sum: Long) {
    val meanMs: Double? get() = if (count > 0) sum.toDouble() / count else null
}
internal data class AnalyticsBucket(val date: LocalDate, val provider: String, val producer: String,
    val counters: Map<String, Long>, val workHistogram: AnalyticsHistogram?)
internal data class AnalyticsCoverage(val start: Double?, val end: Double?, val gaps: Set<String>,
    val unsupported: Set<String>, val partial: Boolean, val providerUnsupported: Map<String, Set<String>> = emptyMap())
internal data class AnalyticsSnapshot(val machineId: String, val epoch: String, val revision: Long, val generatedAt: Double,
    val range: AnalyticsRange, val enabled: Boolean, val coverage: AnalyticsCoverage, val buckets: List<AnalyticsBucket>,
    val retiredEpochs: Set<String> = emptySet(), val retentionDays: Int = 90, val unresolvedWaits: Long? = null)
/** Source ordering outlives selected date buckets, but stays within the controller's bounded memory. */
internal data class AnalyticsSourceGuard(val machineId: String, val epoch: String, val revision: Long,
    val generatedAt: Double, val retiredEpochs: Set<String>)
internal fun AnalyticsSnapshot.sourceGuard() = AnalyticsSourceGuard(machineId, epoch, revision, generatedAt, retiredEpochs)
internal data class SpeechAnalytics(val enabled: Boolean, val counters: Map<String, Long>, val errorCodes: Map<String, Long>)

/** Only reviewed dimensions and numeric fields survive parsing. Never retain a raw reply in view state. */
internal object PrivateAnalyticsProtocol {
    const val operation = "analytics_read"
    const val maxBytes = 1024 * 1024
    const val maxBuckets = 1080
    const val maxMachines = 32
    const val maxRoutes = 3
    const val maxRetiredEpochs = 64
    const val maxNumber = 9_007_199_254_740_991L
    val providers = setOf("codex", "claude", "kimi", "dsh", "opencode", "other")
    val failureCodes = setOf("authentication", "rate_limit", "quota", "policy", "network", "service", "unknown")
    val counters = setOf("turn_started", "turn_completed", "turn_interrupted", "work_ms", "approval_wait_ms", "input_wait_ms",
        "approval_requested", "approval_resolved", "input_requested", "input_resolved",
        "delivery_submitted", "delivery_failed", "delivery_uncertain", "recovery_attempt", "recovery_succeeded", "recovery_exhausted",
        "input_tokens", "output_tokens", "cache_tokens", "reasoning_tokens") + failureCodes.map { "failure_$it" }
    val gapCodes = setOf("collection_disabled", "history_purged", "retention_boundary", "quota_exhausted", "observation_gap",
        "clock_discontinuity", "missing_identity", "unsupported_provider", "unknown")
    val measures = setOf("turns", "work_ms", "approval_wait_ms", "input_wait_ms", "delivery", "recovery", "failures", "tokens", "cost", "background_jobs", "unknown")
    val bounds = listOf(100L, 1000L, 10000L, 60000L, 600000L)
    val speechCounters = setOf("submitted", "completed", "failed", "cancelled", "uncertain", "audio_ms", "queue_ms", "processing_ms")
    private val speechErrors = setOf("service_restarted", "result_expired", "inference_failed", "http_connection", "http_timeout",
        "http_address_blocked", "http_authentication", "http_rate_limit", "http_server", "http_response_invalid", "unknown")

    fun validMachine(value: String) = runCatching { UUID.fromString(value).toString() == value && value != "00000000-0000-0000-0000-000000000000" }.getOrDefault(false)
    private fun integer(value: Any): Long {
        require(value is Int || value is Long)
        return (value as Number).toLong().also { require(it in 0..maxNumber) }
    }
    private fun time(value: Any): Double {
        require(value is Number)
        return value.toDouble().also { require(it.isFinite() && it in 0.0..253402300799.0) }
    }
    private fun codes(array: JSONArray, allowed: Set<String>): Set<String> {
        require(array.length() <= 64)
        return (0 until array.length()).map { index ->
            val value = array.get(index); require(value is String)
            if (value in allowed) value else "unknown"
        }.toSet()
    }
    private fun numbers(raw: JSONObject, allowed: Set<String>): Map<String, Long> {
        require(raw.length() <= 64)
        return allowed.filter { raw.has(it) && !raw.isNull(it) }.associateWith { integer(raw.get(it)) }
    }
    fun parse(raw: JSONObject, machineId: String, range: AnalyticsRange): AnalyticsSnapshot {
        require(raw.toString().toByteArray(Charsets.UTF_8).size <= maxBytes)
        require(integer(raw.get("schema")) == 1L && raw.getString("source") == "agent")
        require(validMachine(machineId) && raw.getString("machine_id") == machineId)
        val returnedRange = raw.getJSONObject("range")
        require(returnedRange.getString("from") == range.from.toString() && returnedRange.getString("through") == range.through.toString())
        val epoch = raw.getString("source_epoch"); require(UUID.fromString(epoch).toString() == epoch)
        val collection = raw.getJSONObject("collection")
        val retention = integer(collection.get("retention_days"))
        require(integer(collection.get("event_retention_days")) == 14L && retention in 1..90)
        val enabled = collection.get("enabled"); require(enabled is Boolean)
        val coverage = raw.getJSONObject("coverage")
        val partial = coverage.get("partial"); require(partial is Boolean)
        val start = coverage.opt("start")?.takeUnless { it == JSONObject.NULL }?.let(::time)
        val end = coverage.opt("end")?.takeUnless { it == JSONObject.NULL }?.let(::time)
        require(start == null || end == null || start <= end)
        val perProvider = coverage.optJSONObject("providers")?.let { entries ->
            require(entries.length() <= providers.size)
            providers.filter { entries.has(it) }.associateWith { codes(entries.getJSONObject(it).getJSONArray("unsupported"), measures) }
        }.orEmpty()
        val unresolved = raw.optJSONObject("unresolved_waits")?.let { integer(it.get("observed")) }
        val rows = raw.getJSONArray("buckets"); require(rows.length() <= maxBuckets)
        val seen = mutableSetOf<Triple<LocalDate, String, String>>()
        val buckets = (0 until rows.length()).map { index ->
            val row = rows.getJSONObject(index)
            val date = LocalDate.parse(row.getString("date")); require(range.contains(date))
            val provider = row.getString("provider"); require(provider in providers)
            val producer = row.getString("producer"); require(producer in setOf("main", "child"))
            require(seen.add(Triple(date, provider, producer)))
            val histogram = row.optJSONObject("histograms")?.optJSONObject("work_ms")?.let { h ->
                require(integer(h.get("version")) == 1L)
                val rawBounds = h.getJSONArray("bounds"); require(rawBounds.length() == bounds.size)
                require((0 until rawBounds.length()).map { integer(rawBounds.get(it)) } == bounds)
                val rawCounts = h.getJSONArray("counts"); require(rawCounts.length() == bounds.size + 1)
                val counts = (0 until rawCounts.length()).map { integer(rawCounts.get(it)) }
                val count = integer(h.get("count")); require(counts.fold(0L, Math::addExact) == count)
                val sum = integer(h.get("sum")); require(count != 0L || sum == 0L)
                AnalyticsHistogram(counts, count, sum)
            }
            AnalyticsBucket(date, provider, producer, numbers(row.getJSONObject("counters"), counters), histogram)
        }
        return AnalyticsSnapshot(machineId, epoch, integer(raw.get("revision")), time(raw.get("generated_at")), range, enabled,
            AnalyticsCoverage(start, end, codes(coverage.getJSONArray("gaps"), gapCodes), codes(coverage.getJSONArray("unsupported"), measures), partial, perProvider),
            buckets, retentionDays = retention.toInt(), unresolvedWaits = unresolved)
    }

    /** Speech profile identities never enter analytics state or export. Daily rows are counted once per host. */
    fun speech(raw: JSONObject, range: AnalyticsRange): SpeechAnalytics {
        require(raw.toString().toByteArray(Charsets.UTF_8).size <= maxBytes)
        require(integer(raw.get("schema")) == 1L && integer(raw.get("retention_days")) == 90L)
        val enabled = raw.get("enabled"); require(enabled is Boolean)
        val rows = raw.getJSONArray("days"); require(rows.length() <= 2880)
        val counters = mutableMapOf<String, Long>(); val errors = mutableMapOf<String, Long>()
        val seen = mutableSetOf<Pair<LocalDate, String>>()
        for (index in 0 until rows.length()) {
            val row = rows.getJSONObject(index)
            val date = LocalDate.parse(row.getString("day"))
            val profile = row.getString("profile_id"); require(profile.matches(Regex("[A-Za-z0-9_-]{1,64}")))
            require(seen.add(date to profile))
            if (!range.contains(date)) continue
            // The existing speech service records durations as finite floating-point milliseconds.
            speechCounters.forEach { key ->
                if (row.has(key) && !row.isNull(key)) {
                    val value = row.get(key); require(value is Number)
                    val number = value.toDouble(); require(number.isFinite() && number in 0.0..maxNumber.toDouble())
                    counters[key] = Math.addExact(counters[key] ?: 0L, number.toLong())
                }
            }
            val codes = row.optJSONObject("error_codes") ?: JSONObject(); require(codes.length() <= 64)
            codes.keys().forEach { rawCode ->
                val code = rawCode.takeIf { it in speechErrors } ?: "unknown"
                errors[code] = Math.addExact(errors[code] ?: 0L, integer(codes.get(rawCode)))
            }
        }
        return SpeechAnalytics(enabled, counters, errors)
    }
}

/** Replace whole source revisions; never add route replies. Epoch resets can reduce all totals to zero. */
internal object AnalyticsFleet {
    /** Null means the response was rejected, not a fresh observation of retained data. */
    fun acceptSource(current: AnalyticsSourceGuard?, incoming: AnalyticsSnapshot): AnalyticsSourceGuard? {
        if (current == null || current.machineId != incoming.machineId) return incoming.sourceGuard()
        if (incoming.epoch in current.retiredEpochs) return null
        if (incoming.generatedAt < current.generatedAt) return null
        if (incoming.epoch == current.epoch && incoming.revision < current.revision) return null
        // Equal-time different epochs are ambiguous; keep the accepted source until a newer read.
        if (incoming.generatedAt == current.generatedAt && incoming.epoch != current.epoch) return null
        val retired = if (incoming.epoch != current.epoch) {
            // Fail closed at the bound rather than forgetting a purge and reviving a retired source.
            require(current.retiredEpochs.size < PrivateAnalyticsProtocol.maxRetiredEpochs)
            current.retiredEpochs + current.epoch
        } else current.retiredEpochs
        return incoming.sourceGuard().copy(retiredEpochs = retired)
    }
    fun replace(current: AnalyticsSnapshot?, incoming: AnalyticsSnapshot): AnalyticsSnapshot {
        val accepted = acceptSource(current?.sourceGuard(), incoming) ?: return requireNotNull(current)
        return incoming.copy(retiredEpochs = accepted.retiredEpochs)
    }
    fun unique(snapshots: List<AnalyticsSnapshot>): List<AnalyticsSnapshot> {
        val unique = linkedMapOf<String, AnalyticsSnapshot>()
        snapshots.forEach { unique[it.machineId] = replace(unique[it.machineId], it) }
        require(unique.size <= PrivateAnalyticsProtocol.maxMachines)
        return unique.values.toList()
    }
    fun buckets(snapshots: List<AnalyticsSnapshot>, provider: String? = null, producer: String? = null): List<AnalyticsBucket> =
        unique(snapshots).flatMap { it.buckets }.filter { (provider == null || it.provider == provider) && (producer == null || it.producer == producer) }
    private fun add(left: Long, right: Long): Long = Math.addExact(left, right).also {
        if (it > PrivateAnalyticsProtocol.maxNumber) throw ArithmeticException("Analytics total exceeds safe integer bound")
    }
    fun counters(rows: List<AnalyticsBucket>): Map<String, Long> {
        val result = mutableMapOf<String, Long>()
        rows.forEach { row -> row.counters.forEach { (key, value) -> result[key] = add(result[key] ?: 0L, value) } }
        return result
    }
    fun totals(snapshots: List<AnalyticsSnapshot>, provider: String? = null, producer: String? = null): Map<String, Long> =
        counters(buckets(snapshots, provider, producer))
    /** Parser accepts only version 1 with fixed bounds. Pool samples; never average machine means. */
    fun workHistogram(rows: List<AnalyticsBucket>): AnalyticsHistogram? {
        val histograms = rows.mapNotNull { it.workHistogram }
        if (histograms.isEmpty()) return null
        val counts = MutableList(PrivateAnalyticsProtocol.bounds.size + 1) { 0L }
        var count = 0L
        var sum = 0L
        histograms.forEach { histogram ->
            counts.indices.forEach { bin -> counts[bin] = add(counts[bin], histogram.counts[bin]) }
            count = add(count, histogram.count)
            sum = add(sum, histogram.sum)
        }
        return AnalyticsHistogram(counts, count, sum)
    }
    fun stale(snapshot: AnalyticsSnapshot, now: Double) = now - snapshot.generatedAt > 300 || snapshot.generatedAt > now + 60
}
