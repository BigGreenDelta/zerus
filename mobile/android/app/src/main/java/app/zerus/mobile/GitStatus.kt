package app.zerus.mobile

import org.json.JSONObject
import java.math.BigInteger
import java.util.UUID

/** A computer-scoped read. Its original argument is independent of native canonical paths. */
internal data class GitStatusKey(val machine: MachineKey, val requestedPath: String)
internal data class GitCheckoutEvidence(val root: String?, val branch: String?, val detached: Boolean?,
    val head: String? = null, val metadataState: String? = null)
internal data class GitStatusCandidate(val key: GitStatusKey, val evidence: GitCheckoutEvidence)
internal data class GitStatusRequest(val requestId: String, val key: GitStatusKey, val generation: Long,
    val originalStartNanos: Long, val evidence: GitCheckoutEvidence) {
    fun payload(): JSONObject {
        require(GitStatus.validKey(key) && GitStatus.canonicalId(requestId))
        return JSONObject().put("request_id", requestId).put("path", key.requestedPath)
    }
}

/** Optional native fields stay optional; a missing or null count never becomes zero. */
internal data class GitStatusSnapshot(val state: String, val path: String, val remoteState: String,
    val head: String?, val branch: String?, val upstream: String?, val root: String?, val commonDir: String?,
    val remote: String?, val remoteRef: String?, val unborn: Boolean?, val detached: Boolean?,
    val changedFiles: BigInteger?, val conflicts: BigInteger?, val ahead: BigInteger?, val behind: BigInteger?,
    val sampledAt: Double?, val remoteCheckedAt: Double?, val remoteAgeSeconds: Double?, val present: Set<String>)

internal data class GitStatusOrigin(val requestId: String, val originalStartNanos: Long)
internal enum class GitStatusKind { Changed, Ahead, Behind, Unknown, Synced }
internal enum class GitStatusTone { Muted, Attention, Incoming, Synced, Conflict }
internal data class GitStatusMark(val kind: GitStatusKind, val count: String = "", val tone: GitStatusTone)
internal data class GitStatusBadge(val marks: List<GitStatusMark>, val description: String,
    val current: Boolean, val verified: Boolean, val synced: Boolean)

internal object GitStatus {
    const val OPERATION = "git_status"
    const val MAX_BYTES = 32 * 1024
    private val zero = BigInteger.ZERO
    private val strings = setOf("state", "path", "remote_state", "head", "branch", "upstream", "root", "common_dir", "remote", "remote_ref")
    private val flags = setOf("unborn", "detached")
    private val integers = setOf("changed_files", "conflicts")
    private val nullableIntegers = setOf("ahead", "behind")
    private val times = setOf("sampled_at", "remote_checked_at", "remote_age_seconds")
    private val fields = strings + flags + integers + nullableIntegers + times
    private val states = setOf("ok", "unavailable", "folder_unavailable", "timeout", "git_unavailable", "metadata_error", "too_large", "not_repo", "changed_during_check")
    private val remoteStates = setOf("unknown", "detached", "no_upstream", "local_upstream", "unavailable", "verified", "changed", "missing")

    fun canonicalId(value: String): Boolean = runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)
    fun validKey(key: GitStatusKey): Boolean = listOf(key.machine.connectionId, key.machine.computerId).all {
        it.isNotBlank() && it.length <= 256 && it.none(Char::isISOControl)
    } && key.machine.computerId.length <= 36 && key.requestedPath.startsWith('/') &&
        key.requestedPath.none(Char::isISOControl) && validUnicode(key.requestedPath) &&
        key.requestedPath.toByteArray(Charsets.UTF_8).size <= 4096

    private fun validUnicode(value: String): Boolean {
        var i = 0
        while (i < value.length) {
            val char = value[i++]
            if (char.isHighSurrogate()) {
                if (i == value.length || !value[i++].isLowSurrogate()) return false
            } else if (char.isLowSurrogate()) return false
        }
        return true
    }
    private fun integer(value: Any?): BigInteger? = when (value) {
        is BigInteger -> value
        is Byte, is Short, is Int, is Long -> BigInteger.valueOf((value as Number).toLong())
        else -> null
    }?.takeIf { it.signum() >= 0 }
    private fun time(value: Any?): Double? = (value as? Number)?.toDouble()?.takeIf { it.isFinite() && it >= 0.0 }

    /**
     * JSONObject has already lost raw numeric spelling. Strings have exact ASCII-escaped cost;
     * floating values reserve at least 32 bytes, a safe upper bound for a finite double's native
     * canonical spelling. A legitimate result near 32 KiB can therefore be conservatively refused.
     * No raw-wire size equivalence or permissive numeric coercion is inferred.
     */
    private fun sizeBound(result: JSONObject, snapshot: JSONObject): Boolean {
        fun quoted(value: String): Long = 2L + value.sumOf { char -> when (char) {
            '"', '\\', '\b', '\u000c', '\n', '\r', '\t' -> 2L
            else -> if (char.code < 32 || char.code > 126) 6L else 1L
        } }
        fun scalar(value: Any?): Long = when (value) {
            is String -> quoted(value)
            is Boolean -> if (value) 4L else 5L
            null, JSONObject.NULL -> 4L
            is Byte, is Short, is Int, is Long, is BigInteger -> value.toString().length.toLong()
            is Number -> maxOf(32L, value.toString().length.toLong())
            else -> MAX_BYTES + 1L
        }
        var size = 2L + 3L // Four outer fields, three commas.
        for (key in listOf("schema", "request_id", "requested_path")) size += quoted(key) + 1 + scalar(result.opt(key))
        size += quoted("snapshot") + 1 + 2
        val keys = snapshot.keys().asSequence().toList()
        size += (keys.size - 1).coerceAtLeast(0)
        for (key in keys) {
            size += quoted(key) + 1 + scalar(snapshot.opt(key))
            if (size > MAX_BYTES) return false
        }
        return size <= MAX_BYTES
    }

    fun parse(result: JSONObject, request: GitStatusRequest): GitStatusSnapshot? {
        if (!validKey(request.key) || !canonicalId(request.requestId) || result.length() != 4 ||
            result.keys().asSequence().any { it !in setOf("schema", "request_id", "requested_path", "snapshot") } ||
            integer(result.opt("schema")) != BigInteger.ONE ||
            result.opt("request_id") as? String != request.requestId ||
            result.opt("requested_path") as? String != request.key.requestedPath) return null
        val raw = result.opt("snapshot") as? JSONObject ?: return null
        if (raw.length() > fields.size) return null
        val present = raw.keys().asSequence().toSet()
        if (present.any { it !in fields }) return null
        for (key in present) {
            val value = raw.opt(key)
            val valid = when (key) {
                in strings -> value is String && validUnicode(value)
                in flags -> value is Boolean
                in integers -> integer(value) != null
                in nullableIntegers -> value == JSONObject.NULL || integer(value) != null
                in times -> time(value) != null
                else -> false
            }
            if (!valid) return null
        }
        val state = raw.opt("state") as? String ?: return null
        val path = raw.opt("path") as? String ?: return null
        val remoteState = raw.opt("remote_state") as? String ?: return null
        if (state !in states || remoteState !in remoteStates || !sizeBound(result, raw)) return null
        fun text(key: String) = raw.opt(key) as? String
        fun flag(key: String) = raw.opt(key) as? Boolean
        return GitStatusSnapshot(state, path, remoteState, text("head"), text("branch"), text("upstream"),
            text("root"), text("common_dir"), text("remote"), text("remote_ref"), flag("unborn"), flag("detached"),
            integer(raw.opt("changed_files")), integer(raw.opt("conflicts")), integer(raw.opt("ahead")), integer(raw.opt("behind")),
            time(raw.opt("sampled_at")), time(raw.opt("remote_checked_at")), time(raw.opt("remote_age_seconds")), present)
    }

    fun scopeMatches(snapshot: GitStatusSnapshot, evidence: GitCheckoutEvidence): Boolean =
        evidence.metadataState == "ok" && !evidence.root.isNullOrBlank() && evidence.branch != null && evidence.detached != null &&
            (evidence.detached || evidence.branch.isNotBlank()) && snapshot.root == evidence.root &&
            snapshot.branch == evidence.branch && snapshot.detached == evidence.detached &&
            (evidence.head == null || evidence.head.isNotBlank() && snapshot.head == evidence.head)

    fun count(value: BigInteger): String = if (value > BigInteger.valueOf(99)) "99+" else value.toString()

    fun present(snapshot: GitStatusSnapshot, origin: GitStatusOrigin?, candidate: GitStatusCandidate,
        online: Boolean, nowNanos: Long): GitStatusBadge? {
        if (snapshot.state == "not_repo" || candidate.evidence.metadataState == "not_repo") return null
        val elapsed = origin?.originalStartNanos?.takeIf { it > 0 && nowNanos >= it }?.let { (nowNanos - it) / 1_000_000_000.0 }
        val scoped = scopeMatches(snapshot, candidate.evidence)
        val current = online && scoped && snapshot.state == "ok" && snapshot.changedFiles != null && elapsed != null && elapsed < 30.0
        val verified = current && snapshot.remoteState == "verified" && snapshot.remoteAgeSeconds?.let { it + elapsed!! < 60.0 } == true
        val synced = verified && snapshot.changedFiles == zero && snapshot.conflicts == zero && snapshot.ahead == zero &&
            snapshot.behind == zero && snapshot.detached == false && snapshot.unborn == false && !snapshot.upstream.isNullOrBlank()
        val marks = buildList {
            snapshot.changedFiles?.takeIf { it > zero }?.let { add(GitStatusMark(GitStatusKind.Changed, count(it),
                if (!current) GitStatusTone.Muted else if (snapshot.conflicts?.signum() == 1) GitStatusTone.Conflict else GitStatusTone.Attention)) }
            snapshot.ahead?.takeIf { it > zero }?.let { add(GitStatusMark(GitStatusKind.Ahead, count(it), if (verified) GitStatusTone.Attention else GitStatusTone.Muted)) }
            snapshot.behind?.takeIf { it > zero }?.let { add(GitStatusMark(GitStatusKind.Behind, count(it), if (verified) GitStatusTone.Incoming else GitStatusTone.Muted)) }
            if (!verified) add(GitStatusMark(GitStatusKind.Unknown, tone = GitStatusTone.Muted))
            else if (synced) add(GitStatusMark(GitStatusKind.Synced, tone = GitStatusTone.Synced))
        }
        val description = buildList {
            add("Git status for current checkout")
            if (!online) add("Computer offline; last known values")
            else if (!current) add("Last known values; waiting for a fresh matching check")
            snapshot.changedFiles?.let { add("$it changed or new files") }
            snapshot.conflicts?.takeIf { it > zero }?.let { add("$it files with merge conflicts") }
            snapshot.ahead?.let { add("$it outgoing commits") }
            snapshot.behind?.let { add("$it incoming commits") }
            add(when {
                synced -> "Clean checkout; branch matches the verified remote"
                snapshot.remoteState == "no_upstream" -> "No upstream configured; publication is unverified"
                snapshot.remoteState == "local_upstream" -> "Local upstream; remote publication is unverified"
                snapshot.remoteState == "detached" -> "Detached HEAD; branch publication is unverified"
                snapshot.remoteState == "changed" -> "Remote branch changed"
                snapshot.remoteState == "missing" -> "Remote branch does not exist"
                !verified -> "Remote publication is unverified"
                else -> "Upstream verified"
            })
        }.joinToString(". ")
        return GitStatusBadge(marks, description, current, verified, synced)
    }
}
