package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class GitStatusAccessTest {
    private val nativeId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    private val otherId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    private val gatewayId = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    private val key = MachineKey("fixture-workspace", "fixture-computer")
    private val connection = Connection(key.connectionId, "Fixture workspace", "https://relay.example.test", "fixture-token")
    private val machine = Machine(key.connectionId, key.computerId, "Fixture computer", true,
        route = MachineRoute.Direct, machineId = nativeId)
    private val operations = setOf(GitStatus.OPERATION)

    private fun access(key: MachineKey = this.key, connections: List<Connection> = listOf(connection),
        machines: List<Machine> = listOf(machine), relay: Set<String> = operations, native: Set<String> = operations) =
        GitStatusAccessPolicies.access(key, connections, machines, relay, native)
    private fun raw() = JSONObject().put("cwd", "/fixture/alias").put("state", "running")
        .put("git_root", "/fixture/native").put("git_branch", "main").put("git_detached", false).put("git_metadata_state", "ok")
    private fun session(raw: JSONObject = raw(), target: Target = Target(key.computerId, "fixture/session", "run", "conversation", key.connectionId)) =
        Session(target, "Fixture", "codex", "idle", "Fixture", "", 0, raw)
    private fun candidate(raw: JSONObject = raw(), target: Target? = null) =
        GitStatusAccessPolicies.candidate(if (target == null) session(raw) else session(raw, target))
    private fun current(candidate: GitStatusCandidate): Boolean {
        val request = GitStatusTestData.request(candidate)
        val snapshot = GitStatus.parse(GitStatusTestData.result(request), request)!!
        return GitStatus.present(snapshot, GitStatusOrigin(request.requestId, request.originalStartNanos),
            candidate, true, request.originalStartNanos)!!.current
    }

    @Test fun exactTransportAndNativeIdentityAreImmutableEpochInputs() {
        val access = access()!!
        assertTrue(access.available)
        assertEquals(GitStatusEpoch(connection, key, nativeId, GitStatusRouteIdentity.Direct), access.epoch)
        assertEquals(connection, (access.epoch as GitStatusEpoch).connection)
    }

    @Test fun missingAndAmbiguousExactCatalogEntriesNeverBindByFirstMatch() {
        assertNull(access(connections = emptyList()))
        assertNull(access(machines = emptyList()))
        assertNull(access(connections = listOf(connection, connection)))
        assertNull(access(connections = listOf(connection, connection.copy(token = "different-fixture-token"))))
        assertNull(access(machines = listOf(machine, machine)))
        assertNull(access(machines = listOf(machine, machine.copy(machineId = otherId))))
        assertNotNull(access(connections = listOf(connection, connection.copy(id = "unrelated-workspace")),
            machines = listOf(machine, machine.copy(connectionId = "unrelated-workspace"))))
    }

    @Test fun exactComputerKeyNeverFallsBackToNativeIdServerAliasOrDisplayName() {
        assertNull(access(machines = listOf(machine.copy(id = "different-id", serverAliases = setOf(key.computerId), name = key.computerId))))
        assertNull(access(key = key.copy(computerId = nativeId)))
        assertNull(access(key = key.copy(connectionId = "different-workspace")))
        assertNull(access(machines = listOf(machine.copy(connectionId = "different-workspace"))))
    }

    @Test fun nativeMachineUuidAndComputerKeyMustBeValidWithoutNormalization() {
        for (invalid in listOf("", " ", "1-1-1-1-1", nativeId.uppercase(), "$nativeId\n"))
            assertNull(invalid, access(machines = listOf(machine.copy(machineId = invalid))))
        for (invalid in listOf(key.copy(connectionId = ""), key.copy(connectionId = "bad\nworkspace"),
            key.copy(computerId = ""), key.copy(computerId = "x".repeat(37)))) assertNull(access(key = invalid))
    }

    @Test fun urlTokenConnectionAndNativeIdentityChangesCannotReuseAnEpoch() {
        val original = access()!!.epoch
        for (changed in listOf(connection.copy(url = "https://changed.example.test"), connection.copy(token = "changed-fixture-token"),
            connection.copy(label = "Changed full connection snapshot"))) assertNotEquals(original, access(connections = listOf(changed))!!.epoch)
        val changedKey = key.copy(connectionId = "other-workspace")
        assertNotEquals(original, access(key = changedKey, connections = listOf(connection.copy(id = changedKey.connectionId)),
            machines = listOf(machine.copy(connectionId = changedKey.connectionId)))!!.epoch)
        val changedComputer = key.copy(computerId = "other-computer")
        assertNotEquals(original, access(key = changedComputer, machines = listOf(machine.copy(id = changedComputer.computerId)))!!.epoch)
        assertNotEquals(original, access(machines = listOf(machine.copy(machineId = otherId)))!!.epoch)
    }

    @Test fun machineUiAliasesAndHeartbeatMetadataDoNotChangeTransportEpoch() {
        val changed = machine.copy(name = "Private UI alias", nativeName = "Different reported caption", serverAliases = setOf(otherId),
            hgsVersion = "9.9.9", mobileDelivery = "background", lastSeenAt = 9E15, nativeHost = "changed-caption")
        assertEquals(access()!!.epoch, access(machines = listOf(changed))!!.epoch)
    }

    @Test fun directUnknownAndViaAreDistinctWithoutGatewayNameOrParentResolution() {
        val direct = access()!!
        val unknown = access(machines = listOf(machine.copy(route = MachineRoute.Unknown)))!!
        val via = access(machines = listOf(machine.copy(route = MachineRoute.Via(gatewayId, "Reported gateway"))))!!
        assertTrue(direct.available); assertFalse(unknown.available); assertTrue(via.available)
        assertNotEquals(direct.epoch, unknown.epoch); assertNotEquals(direct.epoch, via.epoch); assertNotEquals(unknown.epoch, via.epoch)
        assertEquals(GitStatusRouteIdentity.Via(gatewayId), (via.epoch as GitStatusEpoch).routeIdentity)
        assertEquals(via.epoch, access(machines = listOf(machine.copy(route = MachineRoute.Via(gatewayId, "Private gateway alias"))))!!.epoch)
        assertNotEquals(via.epoch, access(machines = listOf(machine.copy(route = MachineRoute.Via(otherId))))!!.epoch)
        for (invalid in listOf("", "not-a-uuid", gatewayId.uppercase()))
            assertNull(access(machines = listOf(machine.copy(route = MachineRoute.Via(invalid)))))
    }

    @Test fun offlineAndLastKnownRowsRemainUnavailableWithoutInventedOnlineProof() {
        val original = access()!!
        for (changed in listOf(machine.copy(online = false), machine.copy(lastKnown = true), machine.copy(online = false, lastKnown = true))) {
            val unavailable = access(machines = listOf(changed))!!
            assertFalse(unavailable.online); assertFalse(unavailable.available)
            assertEquals(original.epoch, unavailable.epoch)
        }
    }

    @Test fun bothOperationListsAreRequiredAndWithdrawalDisablesAccessWithoutAnyFeatureGate() {
        assertTrue(access()!!.available)
        for (absent in listOf(emptySet(), setOf("inspect"), setOf("git_status-v1"))) {
            val noRelay = access(relay = absent)!!; assertFalse(noRelay.relaySupported); assertFalse(noRelay.available)
            val noNative = access(native = absent)!!; assertFalse(noNative.computerSupported); assertFalse(noNative.available)
            assertEquals(access()!!.epoch, noRelay.epoch); assertEquals(access()!!.epoch, noNative.epoch)
        }
    }

    @Test fun cwdMustBeTheActualOriginalStringAndNeverAProjectOrCanonicalFallback() {
        for (invalid in listOf(JSONObject.NULL, true, 7, JSONObject(), "", "relative", "C:\\fixture", "/bad\n", "/\ud800",
            "/" + "é".repeat(2048))) assertNull("cwd=$invalid", candidate(raw().put("cwd", invalid).put("cwd_canonical", "/fixture/native").put("project", "/fixture/native")))
        assertNull(candidate(raw().also { it.remove("cwd") }.put("cwd_canonical", "/fixture/native")))
        val exact = "/fixture/../alias//"
        assertEquals(exact, candidate(raw().put("cwd", exact))!!.key.requestedPath)
        assertNotNull(candidate(raw().put("cwd", "/" + "x".repeat(4095))))
    }

    @Test fun archivedAndChildTargetsOrRawMarkersNeverProduceCandidates() {
        val target = session().target
        for (changed in listOf(target.copy(archiveId = nativeId), target.copy(agentId = "child"), target.copy(parentConversation = "parent")))
            assertNull(candidate(target = changed))
        for (field in listOf("archive_id", "agent_id", "parent_conversation_id")) {
            assertNull(field, candidate(raw().put(field, "foreign")))
            for (malformed in listOf(true, 1, JSONObject())) assertNull("$field=$malformed", candidate(raw().put(field, malformed)))
        }
        assertNull(candidate(raw().put("state", "archived")))
    }

    @Test fun omittedNullOrEmptyOptionalMarkersPreserveOrdinaryNativeRows() {
        for (field in listOf("archive_id", "agent_id", "parent_conversation_id")) {
            for (absent in listOf(JSONObject.NULL, "")) assertNotNull("$field=$absent", candidate(raw().put(field, absent)))
        }
    }

    @Test fun nonemptyWhitespaceMarkersCannotBecomeAnOrdinaryParentCandidate() {
        for (field in listOf("archive_id", "agent_id", "parent_conversation_id"))
            for (marker in listOf(" ", "\t", "\n")) assertNull("$field=$marker", candidate(raw().put(field, marker)))
        val target = session().target
        for (changed in listOf(target.copy(archiveId = " "), target.copy(agentId = " "), target.copy(parentConversation = " ")))
            assertNull(candidate(target = changed))
    }

    @Test fun checkoutProofUsesOnlyActualFieldsWithoutGuessingRootBranchOrHead() {
        val candidate = candidate(raw().put("git_head", "not-an-exported-proof"))!!
        assertEquals(key, candidate.key.machine)
        assertEquals(GitCheckoutEvidence("/fixture/native", "main", false, metadataState = "ok"), candidate.evidence)
        assertNull(candidate.evidence.head)
        assertTrue(current(candidate))
        assertFalse(current(candidate(raw().put("git_root", "/fixture"))!!))
        assertFalse(current(candidate(raw().put("git_branch", "main-prefix"))!!))
    }

    @Test fun missingOrWrongCheckoutMetadataStaysUnknownWhileStrictCwdMayStillBeRead() {
        for (field in listOf("git_root", "git_branch", "git_metadata_state", "git_detached")) {
            val values = if (field == "git_detached") listOf(JSONObject.NULL, "false", 0) else listOf(JSONObject.NULL, true, 0)
            for (malformed in values) {
                val candidate = candidate(raw().put(field, malformed))!!
                assertFalse("$field=$malformed", current(candidate))
                when (field) {
                    "git_root" -> assertNull(candidate.evidence.root)
                    "git_branch" -> assertNull(candidate.evidence.branch)
                    "git_metadata_state" -> assertNull(candidate.evidence.metadataState)
                    "git_detached" -> assertNull(candidate.evidence.detached)
                }
            }
            assertFalse(field, current(candidate(raw().also { it.remove(field) })!!))
        }
        val unknown = candidate(JSONObject().put("cwd", "/fixture/alias"))!!
        assertEquals(GitCheckoutEvidence(null, null, null), unknown.evidence)
        assertFalse(current(unknown))
        assertFalse(current(candidate(raw().put("git_metadata_state", "unavailable"))!!))
        assertNull(candidate(raw().put("git_metadata_state", "not_repo")))
    }

    @Test fun detachedProofRemainsActualBooleanAndEmptyBranchIsNotInventedFromCaption() {
        val detached = candidate(raw().put("git_detached", true).put("git_branch", ""))!!
        assertEquals(true, detached.evidence.detached); assertEquals("", detached.evidence.branch)
        val request = GitStatusTestData.request(detached)
        val parsed = GitStatus.parse(GitStatusTestData.result(request, GitStatusTestData.snapshot().put("detached", true).put("branch", "")), request)!!
        assertTrue(GitStatus.scopeMatches(parsed, detached.evidence))
        assertFalse(GitStatus.scopeMatches(parsed, candidate(raw().put("git_detached", "true").put("git_branch", ""))!!.evidence))
        assertNull(candidate(raw().also { it.remove("cwd") }.put("git_root", "/fixture/native")))
    }
}
