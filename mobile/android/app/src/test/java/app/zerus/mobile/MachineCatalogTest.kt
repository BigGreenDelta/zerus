package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class MachineCatalogTest {
    private val gatewayId = "11111111-1111-4111-8111-111111111111"
    private val canonicalId = "22222222-2222-4222-8222-222222222222"
    private val peerId = "33333333-3333-4333-8333-333333333333"
    private val otherId = "44444444-4444-4444-8444-444444444444"

    private fun raw(id: String = peerId, name: String = "Computer") =
        JSONObject().put("id", id).put("name", name).put("online", true)
    private fun via(id: Any = gatewayId, name: Any = "Reported gateway") =
        JSONObject().put("gateway_id", id).put("gateway_name", name)
    private fun parse(raw: JSONObject, connection: String = "workspace") = MachineCatalog.parse(connection, raw)
    private fun direct(id: String = gatewayId, name: String = "Gateway") = parse(raw(id, name).put("via", JSONObject.NULL))
    private fun peer() = parse(raw().put("via", via()))

    @Test fun cliVersionsRemainScopedToTheirComputerAndLegacyReportsStayUnknown() {
        val desktop = parse(raw(gatewayId, "Desktop").put("snapshot", JSONObject().put("hgs_version", "1.2.3")))
        val laptop = parse(raw(peerId, "Laptop").put("snapshot", JSONObject().put("hgs_version", "1.2.4-preview.1+fixture")))
        assertEquals("1.2.3", desktop.hgsVersion)
        assertEquals("1.2.4-preview.1+fixture", laptop.hgsVersion)
        assertEquals("", parse(raw()).hgsVersion)
        assertEquals("1.2.3", desktop.copy(online = false, lastKnown = true).hgsVersion)
        assertEquals("1.2.3", MachineNames.apply(desktop, listOf(MachineAlias(MachineKey("workspace", gatewayId), "My computer"))).hgsVersion)
        listOf(JSONObject.NULL, 7, JSONObject(), "hgs 1.2.3", "null", "1.2.3\nforeign output", "1.2.3-" + "x".repeat(129)).forEach {
            assertEquals("", parse(raw().put("snapshot", JSONObject().put("hgs_version", it))).hgsVersion)
        }
    }

    @Test fun onlyExplicitNullIsDirectAndLegacyConstructorsRemainUnknown() {
        assertEquals(MachineRoute.Unknown, parse(raw()).route)
        assertEquals(MachineRoute.Unknown, Machine("workspace", peerId, "Cached", false).route)
        assertEquals(MachineRoute.Direct, direct().route)
        listOf("null", 0, false, JSONArray(), JSONObject(), via("not-a-uuid"), via("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"), via(JSONObject.NULL)).forEach {
            assertEquals(MachineRoute.Unknown, parse(raw().put("via", it)).route)
        }
    }

    @Test fun validRouteAndAliasesPreserveNativeIdentity() {
        val machine = parse(raw().put("aliases", JSONArray(listOf(peerId, otherId, peerId))).put("via", via()))
        assertEquals(MachineRoute.Via(gatewayId, "Reported gateway"), machine.route)
        assertEquals(setOf(peerId, otherId), machine.serverAliases)
        assertEquals(peerId, machine.id)
        assertEquals("Computer", machine.nativeName)
    }

    @Test fun malformedOrOversizedAliasesAreNotUsedForBinding() {
        listOf("alias", JSONObject(), JSONArray(listOf(gatewayId, "invalid")), JSONArray(listOf(7)),
            JSONArray(List(4097) { gatewayId })).forEach {
            assertTrue(parse(raw().put("aliases", it)).serverAliases.isEmpty())
        }
        assertTrue(parse(raw()).serverAliases.isEmpty())
    }

    @Test fun parentResolvesByExactIdOrServerAliasAndUsesCurrentPrivateName() {
        val peer = peer()
        assertEquals(direct(), MachineCatalog.gateway(peer, listOf(peer, direct())))
        val canonical = MachineNames.apply(parse(raw(canonicalId, "Reported gateway").put("via", JSONObject.NULL)
            .put("aliases", JSONArray(listOf(gatewayId)))), listOf(MachineAlias(MachineKey("workspace", canonicalId), "My desktop")))
        assertEquals(canonical, MachineCatalog.directGateway(peer, listOf(peer, canonical)))
        assertEquals("My desktop", MachineCatalog.gatewayName(peer, listOf(peer, canonical)))
        assertEquals(MachineRoute.Via(gatewayId, "Reported gateway"), peer.route)
    }

    @Test fun workspaceAndDuplicateNamesCannotBindAnotherGateway() {
        val peer = peer()
        val foreign = parse(raw(gatewayId, "Reported gateway").put("via", JSONObject.NULL), "other-workspace")
        val sameName = direct(otherId, "Reported gateway")
        assertNull(MachineCatalog.gateway(peer, listOf(peer, foreign, sameName)))
        assertEquals("Reported gateway", MachineCatalog.gatewayName(peer, listOf(peer, foreign, sameName)))
    }

    @Test fun ambiguousAliasesAndSelfReferencesStayUnresolved() {
        val peer = peer()
        val alias = parse(raw(canonicalId).put("via", JSONObject.NULL).put("aliases", JSONArray(listOf(gatewayId))))
        assertNull(MachineCatalog.gateway(peer, listOf(peer, direct(), alias)))
        val self = parse(raw(gatewayId).put("via", via()))
        assertNull(MachineCatalog.gateway(self, listOf(self)))
    }

    @Test fun orphanDoesNotBecomeDirectAndResolutionNeverFollowsAnotherHop() {
        val peer = peer()
        assertNull(MachineCatalog.directGateway(peer, listOf(peer)))
        assertTrue(peer.route is MachineRoute.Via)
        val intermediary = parse(raw(gatewayId, "Intermediate").put("via", via(otherId)))
        val third = direct(otherId, "Third computer")
        assertEquals(intermediary, MachineCatalog.gateway(peer, listOf(peer, intermediary, third)))
        assertNull(MachineCatalog.directGateway(peer, listOf(peer, intermediary, third)))
        val unknown = parse(raw(gatewayId, "Legacy gateway"))
        assertEquals(unknown, MachineCatalog.gateway(peer, listOf(peer, unknown)))
        assertNull(MachineCatalog.directGateway(peer, listOf(peer, unknown)))
        assertNull(MachineCatalog.gateway(third, listOf(peer, intermediary, third)))
    }

    @Test fun unsafeGatewayCaptionsDoNotInvalidateVerifiedRouteIdentity() {
        listOf("bad\nname", "bad\u202ename", "x".repeat(129), 7, JSONObject.NULL).forEach {
            assertEquals(MachineRoute.Via(gatewayId), parse(raw().put("via", via(name = it))).route)
        }
        assertEquals(MachineRoute.Via(gatewayId, "😀".repeat(128)), parse(raw().put("via", via(name = "😀".repeat(128)))).route)
    }

    @Test fun offlineCopyAndPrivateRenameKeepRouteAliasesAndTargetKeys() {
        val machine = parse(raw().put("via", via()).put("aliases", JSONArray(listOf(peerId))))
        val offline = MachineNames.apply(machine.copy(online = false, lastKnown = true), listOf(MachineAlias(MachineKey("workspace", peerId), "Phone label")))
        assertFalse(offline.online)
        assertTrue(offline.lastKnown)
        assertEquals("Phone label", offline.name)
        assertEquals(machine.nativeName, offline.nativeName)
        assertEquals(machine.route, offline.route)
        assertEquals(machine.serverAliases, offline.serverAliases)
        assertEquals(MachineKey(machine.connectionId, machine.id), MachineKey(offline.connectionId, offline.id))
        assertEquals(MachineColors.automatic(MachineKey(machine.connectionId, machine.id)), MachineColors.automatic(MachineKey(offline.connectionId, offline.id)))
    }

    @Test fun treeKeepsOneHopChildrenVisibleAndEveryMachineKeyOnce() {
        val gateway = direct()
        val peer = peer().copy(online = false, lastKnown = true)
        val other = direct(otherId, "Gateway")
        val unknown = Machine("workspace", canonicalId, "Gateway", false, lastKnown = true)
        val foreign = gateway.copy(connectionId = "other")
        val rows = MachineCatalog.tree("workspace", listOf(peer, foreign, unknown, gateway, other, peer))
        assertEquals(listOf(gatewayId, peerId, otherId, canonicalId), rows.map { it.machine.id })
        assertEquals(gateway, rows[1].parent)
        assertTrue(rows[1].machine.lastKnown)
        assertFalse(rows[1].machine.online)
        assertTrue(rows.filterIndexed { index, _ -> index != 1 }.all { it.parent == null })
    }

    @Test fun unreachablePeerStaysUnderItsGatewayAsOfflineWithOptionalLastSeen() {
        val gateway = direct()
        val offline = parse(raw().put("online", false).put("via", via()).put("last_seen_at", JSONObject.NULL).put("snapshot", JSONObject.NULL))
        val group = MachineCatalog.groups("workspace", listOf(gateway, offline)).single()
        assertEquals(gateway, group.root.machine)
        assertEquals(listOf(offline), group.children.map { it.machine })
        assertEquals("Gateway", group.children.single().gatewayName)
        assertFalse(offline.online)
        assertNull(offline.lastSeenAt)
        assertEquals("Offline", MachineCatalog.status(offline))
        val seen = parse(raw().put("online", false).put("via", via()).put("last_seen_at", 1_700_000_000.5))
        assertEquals(1_700_000_000.5, seen.lastSeenAt!!, 0.0)
        assertTrue(MachineCatalog.status(seen).startsWith("Offline · last seen "))
        assertEquals("Online", MachineCatalog.status(seen.copy(online = true)))
        assertEquals("Last known", MachineCatalog.status(seen.copy(lastKnown = true)))
        assertEquals(seen.lastSeenAt, MachineNames.apply(seen, listOf(MachineAlias(MachineKey("workspace", peerId), "Mac"))).lastSeenAt)
        listOf("1700000000", -1, true, JSONObject()).forEach {
            val invalid = parse(raw().put("online", false).put("last_seen_at", it))
            assertNull(invalid.lastSeenAt)
            assertEquals("Offline", MachineCatalog.status(invalid))
        }
    }

    @Test fun cyclesOrAmbiguousParentsStayVisibleWithoutInventedGateways() {
        val first = parse(raw(gatewayId).put("via", via(peerId)))
        val second = peer()
        val cycle = MachineCatalog.tree("workspace", listOf(first, second))
        assertEquals(listOf(first, second), cycle.map { it.machine })
        assertTrue(cycle.all { it.parent == null })
        val alias = parse(raw(canonicalId).put("via", JSONObject.NULL).put("aliases", JSONArray(listOf(gatewayId))))
        val ambiguous = MachineCatalog.tree("workspace", listOf(second, direct(), alias))
        assertEquals(3, ambiguous.size)
        assertTrue(ambiguous.all { it.parent == null })
        assertEquals(second, ambiguous.last().machine)
    }

    @Test fun projectionIndexesAliasesOncePerIdentityAndKeepsWorkspacesSeparate() {
        val gateway = direct().copy(serverAliases = setOf(gatewayId, canonicalId))
        val firstPeer = peer()
        val aliasPeer = parse(raw(otherId).put("via", via(canonicalId)))
        val foreign = gateway.copy(connectionId = "other", name = "Other workspace")
        val foreignPeer = firstPeer.copy(connectionId = "other")
        val projection = MachineCatalog.groupsByConnection(listOf(firstPeer, gateway, aliasPeer, foreign, foreignPeer))
        val group = projection.getValue("workspace").single()
        assertEquals(gateway, group.root.machine)
        assertEquals(listOf(firstPeer, aliasPeer), group.children.map { it.machine })
        assertTrue(group.children.all { it.gatewayName == "Gateway" })
        assertEquals(foreign, projection.getValue("other").single().root.machine)
        assertEquals("Other workspace", projection.getValue("other").single().children.single().gatewayName)
    }

    @Test fun fullCatalogProjectionRetainsEveryRowWithoutTransitiveGroups() {
        val gateway = direct()
        val peers = (1..2047).map { index ->
            Machine("workspace", java.util.UUID(0, index.toLong()).toString(), "Duplicate label", true,
                route = MachineRoute.Via(gatewayId))
        }
        val projection = MachineCatalog.groupsByConnection(peers + gateway).getValue("workspace").single()
        assertEquals(gateway, projection.root.machine)
        assertEquals(peers, projection.children.map { it.machine })
        assertEquals(2048, (listOf(projection.root) + projection.children).map { it.machine.id }.distinct().size)
    }
}
