package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.math.BigDecimal
import java.math.BigInteger

internal object GitStatusTestData {
    const val SECOND = 1_000_000_000L
    const val ID = "11111111-1111-4111-8111-111111111111"
    fun candidate(path: String = "/fixture/alias", connection: String = "fixture-connection", computer: String = "fixture-computer") =
        GitStatusCandidate(GitStatusKey(MachineKey(connection, computer), path),
            GitCheckoutEvidence("/fixture/native", "main", false, metadataState = "ok"))
    fun request(candidate: GitStatusCandidate = candidate(), start: Long = SECOND, id: String = ID) =
        GitStatusRequest(id, candidate.key, 1, start, candidate.evidence)
    fun snapshot() = JSONObject().put("state", "ok").put("path", "/fixture/native")
        .put("remote_state", "verified").put("root", "/fixture/native").put("branch", "main")
        .put("detached", false).put("unborn", false).put("changed_files", 0).put("conflicts", 0)
        .put("ahead", 0).put("behind", 0).put("upstream", "origin/main").put("remote_age_seconds", 0)
    fun result(request: GitStatusRequest = request(), snapshot: JSONObject = snapshot()) = JSONObject()
        .put("schema", 1).put("request_id", request.requestId).put("requested_path", request.key.requestedPath).put("snapshot", snapshot)
    fun completed(request: GitStatusRequest, snapshot: JSONObject = snapshot()) = JSONObject()
        .put("request_id", request.requestId).put("state", "completed").put("result", result(request, snapshot))
    fun queued(request: GitStatusRequest) = JSONObject().put("request_id", request.requestId).put("state", "queued")
    fun parsed(snapshot: JSONObject = snapshot()) = GitStatus.parse(result(snapshot = snapshot), request())!!
    fun badge(snapshot: GitStatusSnapshot = parsed(), candidate: GitStatusCandidate = candidate(),
        start: Long? = SECOND, now: Long = SECOND, online: Boolean = true) =
        GitStatus.present(snapshot, start?.let { GitStatusOrigin(ID, it) }, candidate, online, now)!!
}

class GitStatusTest {
    @Test fun exactEnvelopeRetainsOriginalArgumentSeparatelyFromNativeCanonicalPath() {
        val request = GitStatusTestData.request()
        val parsed = GitStatus.parse(GitStatusTestData.result(request), request)!!
        assertEquals("/fixture/native", parsed.path)
        assertNotEquals(request.key.requestedPath, parsed.path)
        val payload = request.payload()
        assertEquals(setOf("request_id", "path"), payload.keys().asSequence().toSet())
        assertEquals(request.requestId, payload.getString("request_id"))
        assertEquals(request.key.requestedPath, payload.getString("path"))
        for (bad in listOf(
            GitStatusTestData.result().put("unexpected", true), GitStatusTestData.result().also { it.remove("schema") },
            GitStatusTestData.result().put("request_id", "22222222-2222-4222-8222-222222222222"),
            GitStatusTestData.result().put("requested_path", "/fixture/alias/other"),
            GitStatusTestData.result().put("snapshot", "not an object"))) assertNull(GitStatus.parse(bad, request))
    }

    @Test fun schemaAndCountsRequireActualIntegerTypesNeverBooleanFloatingOrString() {
        val request = GitStatusTestData.request()
        for (bad in listOf(true, false, "1", 1.0, BigDecimal.ONE, 0, -1, JSONObject.NULL)) {
            assertNull("schema=$bad", GitStatus.parse(GitStatusTestData.result().put("schema", bad), request))
        }
        for (field in listOf("changed_files", "conflicts", "ahead", "behind")) {
            for (bad in listOf(true, false, "0", 0.0, BigDecimal.ZERO, -1)) {
                assertNull("$field=$bad", GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put(field, bad)), request))
            }
            val huge = BigInteger("18446744073709551615")
            assertNotNull(GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put(field, huge)), request))
        }
        assertNotNull(GitStatus.parse(GitStatusTestData.result().put("schema", BigInteger.ONE), request))
    }

    @Test fun optionalFieldsRemainAbsentAndNullableCountsNeverBecomeZero() {
        val minimal = JSONObject().put("state", "unavailable").put("path", "/fixture/native").put("remote_state", "unknown")
        val parsed = GitStatusTestData.parsed(minimal)
        assertEquals(setOf("state", "path", "remote_state"), parsed.present)
        assertNull(parsed.changedFiles); assertNull(parsed.ahead); assertNull(parsed.detached); assertNull(parsed.remoteAgeSeconds)
        val explicitNull = GitStatusTestData.parsed(GitStatusTestData.snapshot().put("ahead", JSONObject.NULL).put("behind", JSONObject.NULL))
        assertTrue("ahead" in explicitNull.present); assertNull(explicitNull.ahead); assertNull(explicitNull.behind)
        for (field in listOf("changed_files", "conflicts", "detached", "unborn", "root", "sampled_at")) {
            assertNull(GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put(field, JSONObject.NULL)), GitStatusTestData.request()))
        }
    }

    @Test fun strictWhitelistEnumsStringsFlagsAndFiniteNonnegativeTimes() {
        val request = GitStatusTestData.request()
        for (snapshot in listOf(GitStatusTestData.snapshot().put("extra", 0), GitStatusTestData.snapshot().put("state", "future"),
            GitStatusTestData.snapshot().put("remote_state", "future"), GitStatusTestData.snapshot().put("path", true),
            GitStatusTestData.snapshot().put("detached", 0), GitStatusTestData.snapshot().put("unborn", "false"))) {
            assertNull(GitStatus.parse(GitStatusTestData.result(snapshot = snapshot), request))
        }
        for (field in listOf("state", "path", "remote_state")) assertNull(GitStatus.parse(
            GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().also { it.remove(field) }), request))
        for (field in listOf("sampled_at", "remote_checked_at", "remote_age_seconds")) {
            for (bad in listOf(true, "0", -0.1, BigDecimal("1E+400"))) assertNull("$field=$bad", GitStatus.parse(
                GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put(field, bad)), request))
            assertNotNull(GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put(field, 0.25)), request))
        }
    }

    @Test fun allAcceptedBackendStatesRemainReadableWithoutInventingSuccessfulStatus() {
        for (state in listOf("ok", "unavailable", "folder_unavailable", "timeout", "git_unavailable", "metadata_error", "too_large", "not_repo", "changed_during_check")) {
            assertNotNull(GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put("state", state)), GitStatusTestData.request()))
            if (state != "ok" && state != "not_repo") assertFalse(GitStatusTestData.badge(GitStatusTestData.parsed(
                GitStatusTestData.snapshot().put("state", state))).current)
        }
        for (state in listOf("unknown", "detached", "no_upstream", "local_upstream", "unavailable", "verified", "changed", "missing")) {
            val parsed = GitStatusTestData.parsed(GitStatusTestData.snapshot().put("remote_state", state))
            assertEquals(state == "verified", GitStatusTestData.badge(parsed).verified)
        }
    }

    @Test fun argumentIsStrictPosixControlFreeUnicodeWithUtf8ByteBound() {
        for (bad in listOf("relative", "C:\\fixture", "/fixture\n", "/fixture\u0000", "/\u007f", "/\ud800", "/\udc00", "/\ud800x")) {
            assertFalse("path=$bad", GitStatus.validKey(GitStatusTestData.candidate(bad).key))
        }
        assertTrue(GitStatus.validKey(GitStatusTestData.candidate("/" + "x".repeat(4095)).key))
        assertFalse(GitStatus.validKey(GitStatusTestData.candidate("/" + "x".repeat(4096)).key))
        assertTrue(GitStatus.validKey(GitStatusTestData.candidate("/" + "é".repeat(2047)).key))
        assertFalse(GitStatus.validKey(GitStatusTestData.candidate("/" + "é".repeat(2048)).key))
        assertTrue(GitStatus.validKey(GitStatusTestData.candidate("/\ud83d\ude00").key))
        for (text in listOf("\ud800", "\udc00", "x\ud800y")) assertNull(GitStatus.parse(
            GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put("head", text)), GitStatusTestData.request()))
        assertNotNull(GitStatus.parse(GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put("head", "\ud83d\ude00")), GitStatusTestData.request()))
    }

    @Test fun canonicalUuidAndNonemptyExactMachineIdentityAreRequired() {
        for (bad in listOf("1-1-1-1-1", GitStatusTestData.ID.uppercase(), "not-a-uuid", "")) {
            if (bad != GitStatusTestData.ID) assertNull(GitStatus.parse(GitStatusTestData.result(), GitStatusTestData.request(id = bad)))
        }
        for (candidate in listOf(GitStatusTestData.candidate(connection = ""), GitStatusTestData.candidate(computer = " "),
            GitStatusTestData.candidate(computer = "x".repeat(37)))) assertFalse(GitStatus.validKey(candidate.key))
    }

    @Test fun asciiEnvelopeIncludesExact32768BoundaryAndRefuses32769() {
        val result = GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().also { it.remove("remote_age_seconds") }.put("head", ""))
        val raw = result.getJSONObject("snapshot")
        val count = GitStatus.MAX_BYTES - result.toString().length
        raw.put("head", "x".repeat(count))
        assertEquals(32768, result.toString().length)
        assertNotNull(GitStatus.parse(result, GitStatusTestData.request()))
        raw.put("head", "x".repeat(count + 1))
        assertEquals(32769, result.toString().length)
        assertNull(GitStatus.parse(result, GitStatusTestData.request()))
    }

    @Test fun unicodeSizeUsesAsciiEscapedUpperBoundRatherThanUtf8AndFloatBoundaryIsConservative() {
        val raw = GitStatusTestData.snapshot().put("head", "é".repeat(5500))
        val result = GitStatusTestData.result(snapshot = raw)
        assertTrue(result.toString().toByteArray(Charsets.UTF_8).size < GitStatus.MAX_BYTES)
        assertNull(GitStatus.parse(result, GitStatusTestData.request()))
        raw.put("head", "\ud83d\ude00".repeat(2000))
        assertNotNull(GitStatus.parse(result, GitStatusTestData.request()))
        raw.put("head", "\ud83d\ude00".repeat(3000))
        assertNull(GitStatus.parse(result, GitStatusTestData.request()))
        val near = GitStatusTestData.result(snapshot = GitStatusTestData.snapshot().put("sampled_at", 1.0).put("head", ""))
        near.getJSONObject("snapshot").put("head", "x".repeat(GitStatus.MAX_BYTES - near.toString().length - 1))
        assertTrue(near.toString().length < GitStatus.MAX_BYTES)
        assertNull("Conservative 32-byte floating reserve is intentional", GitStatus.parse(near, GitStatusTestData.request()))
    }

    @Test fun currentHasStrictThirtySecondOriginalMonotonicBoundaryAndIgnoresHostClock() {
        val parsed = GitStatusTestData.parsed(GitStatusTestData.snapshot().put("sampled_at", 9.0E15).put("remote_checked_at", 9.0E15))
        val start = GitStatusTestData.SECOND
        assertTrue(GitStatusTestData.badge(parsed, start = start, now = start + 30 * start - 1).current)
        assertFalse(GitStatusTestData.badge(parsed, start = start, now = start + 30 * start).current)
        for (origin in listOf(null, 0L, -1L, start + 1)) {
            val badge = GitStatusTestData.badge(parsed, start = origin, now = start)
            assertFalse(badge.current); assertFalse(badge.synced); assertTrue(badge.description.contains("Last known"))
        }
        assertFalse(GitStatusTestData.badge(parsed, online = false).current)
        assertFalse(GitStatusTestData.badge(parsed, online = false).synced)
    }

    @Test fun verifiedUsesRemoteAgePlusOriginalLocalElapsedWithStrictSixtySecondBoundary() {
        val parsed = GitStatusTestData.parsed(GitStatusTestData.snapshot().put("remote_age_seconds", 40))
        val start = GitStatusTestData.SECOND
        assertTrue(GitStatusTestData.badge(parsed, now = start + 20 * start - 1).verified)
        val expired = GitStatusTestData.badge(parsed, now = start + 20 * start)
        assertTrue(expired.current); assertFalse(expired.verified); assertFalse(expired.synced)
        assertTrue(expired.marks.any { it.kind == GitStatusKind.Unknown })
    }

    @Test fun rootBranchDetachedAndOptionalKnownHeadMustMatchWithoutPrefixGuessing() {
        val candidate = GitStatusTestData.candidate()
        val parsed = GitStatusTestData.parsed(GitStatusTestData.snapshot().put("head", "fixture-head"))
        assertTrue(GitStatus.scopeMatches(parsed, candidate.evidence))
        assertTrue(GitStatus.scopeMatches(parsed, candidate.evidence.copy(head = "fixture-head")))
        for (evidence in listOf(candidate.evidence.copy(root = null), candidate.evidence.copy(root = "/fixture"),
            candidate.evidence.copy(branch = null), candidate.evidence.copy(branch = "other"), candidate.evidence.copy(detached = null),
            candidate.evidence.copy(detached = true), candidate.evidence.copy(metadataState = null),
            candidate.evidence.copy(metadataState = "unavailable"), candidate.evidence.copy(head = "other"))) {
            assertFalse(evidence.toString(), GitStatusTestData.badge(parsed, candidate.copy(evidence = evidence)).current)
        }
        for (missing in listOf("root", "branch", "detached")) assertFalse(GitStatusTestData.badge(GitStatusTestData.parsed(
            GitStatusTestData.snapshot().also { it.remove(missing) })).current)
    }

    @Test fun syncedRequiresEveryPositiveCleanUpstreamProofAndNeverMissingDefaults() {
        assertTrue(GitStatusTestData.badge().synced)
        for (field in listOf("changed_files", "conflicts", "ahead", "behind", "unborn", "upstream", "remote_age_seconds")) {
            assertFalse(field, GitStatusTestData.badge(GitStatusTestData.parsed(GitStatusTestData.snapshot().also { it.remove(field) })).synced)
        }
        for (raw in listOf(GitStatusTestData.snapshot().put("unborn", true), GitStatusTestData.snapshot().put("upstream", ""),
            GitStatusTestData.snapshot().put("conflicts", 1), GitStatusTestData.snapshot().put("remote_state", "no_upstream"),
            GitStatusTestData.snapshot().put("ahead", JSONObject.NULL))) assertFalse(GitStatusTestData.badge(GitStatusTestData.parsed(raw)).synced)
        val detached = GitStatusTestData.candidate().let { it.copy(evidence = it.evidence.copy(detached = true, branch = "")) }
        assertFalse(GitStatusTestData.badge(GitStatusTestData.parsed(GitStatusTestData.snapshot().put("detached", true).put("branch", "")), detached).synced)
    }

    @Test fun countsAreCappedVisuallyButFullForAccessibilityAndOfflineDataIsMuted() {
        val raw = GitStatusTestData.snapshot().put("changed_files", 101).put("conflicts", 3).put("ahead", 200).put("behind", 4)
        val current = GitStatusTestData.badge(GitStatusTestData.parsed(raw))
        assertEquals(listOf("99+", "99+", "4"), current.marks.map { it.count })
        assertEquals(GitStatusTone.Conflict, current.marks.first().tone)
        assertTrue(current.description.contains("101 changed")); assertTrue(current.description.contains("200 outgoing"))
        val old = GitStatusTestData.badge(GitStatusTestData.parsed(raw), online = false)
        assertTrue(old.marks.all { it.tone == GitStatusTone.Muted })
        assertTrue(old.description.contains("Computer offline"))
    }

    @Test fun knownNonRepositoryNeverProducesABadge() {
        val candidate = GitStatusTestData.candidate()
        assertNull(GitStatus.present(GitStatusTestData.parsed(GitStatusTestData.snapshot().put("state", "not_repo")), null, candidate, true, 1))
        assertNull(GitStatus.present(GitStatusTestData.parsed(), null, candidate.copy(evidence = candidate.evidence.copy(metadataState = "not_repo")), true, 1))
    }
}
