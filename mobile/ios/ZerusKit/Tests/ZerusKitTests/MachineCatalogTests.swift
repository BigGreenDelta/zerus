import XCTest
@testable import ZerusKit

/// Ported from Android MachineCatalogTest, MachineColorsTest and MachineNamesTest.
final class MachineCatalogTests: XCTestCase {
    private let gatewayId = "11111111-1111-4111-8111-111111111111"
    private let canonicalId = "22222222-2222-4222-8222-222222222222"
    private let peerId = "33333333-3333-4333-8333-333333333333"
    private let otherId = "44444444-4444-4444-8444-444444444444"

    private func raw(_ id: String? = nil, _ name: String = "Computer") -> JSONObject {
        ["id": .string(id ?? peerId), "name": .string(name), "online": true]
    }
    private func via(_ id: JSON? = nil, name: JSON = "Reported gateway") -> JSON {
        ["gateway_id": id ?? .string(gatewayId), "gateway_name": name]
    }
    private func parse(_ raw: JSONObject, _ connection: String = "workspace") -> Machine { try! MachineCatalog.parse(connection, raw) }
    private func direct(_ id: String? = nil, _ name: String = "Gateway") -> Machine { parse(raw(id ?? gatewayId, name).putting("via", .null)) }
    private func peer() -> Machine { parse(raw().putting("via", via())) }

    func testNativeReferenceIdentityUsesReportedHostRatherThanDisplayLabel() {
        let machine = parse(raw(nil, "Office label").putting("machine_id", .string(canonicalId)).putting("snapshot", ["host": "native-host"]))
        XCTAssertEqual(machine.machineId, canonicalId)
        XCTAssertEqual(machine.nativeHost, "native-host")
        let renamed = MachineNames.apply(machine, [MachineAlias(key: MachineKey(connectionId: "workspace", computerId: peerId), name: "Phone label")])
        XCTAssertEqual(renamed.nativeHost, "native-host")
        XCTAssertEqual(renamed.nativeName, "Office label")
        for invalid: JSON in [.null, 7, .string(String(repeating: "x", count: 81))] {
            XCTAssertEqual(parse(raw().putting("snapshot", ["host": invalid])).nativeHost, "")
        }
        XCTAssertEqual(parse(raw(nil, "display-is-not-host")).nativeHost, "")
    }

    func testCliVersionsRemainScopedAndLegacyReportsStayUnknown() {
        XCTAssertEqual(parse(raw(gatewayId, "Desktop").putting("snapshot", ["hgs_version": "1.2.3"])).hgsVersion, "1.2.3")
        XCTAssertEqual(parse(raw(peerId, "Laptop").putting("snapshot", ["hgs_version": "1.2.4-preview.1+fixture"])).hgsVersion, "1.2.4-preview.1+fixture")
        XCTAssertEqual(parse(raw()).hgsVersion, "")
        for invalid: JSON in [.null, 7, [:], "hgs 1.2.3", "null", "1.2.3\nforeign output", .string("1.2.3-" + String(repeating: "x", count: 129))] {
            XCTAssertEqual(parse(raw().putting("snapshot", ["hgs_version": invalid])).hgsVersion, "", "\(invalid)")
        }
    }

    func testOnlyExplicitNullIsDirect() {
        XCTAssertEqual(parse(raw()).route, .unknown)
        XCTAssertEqual(direct().route, .direct)
        for value: JSON in ["null", 0, false, [], [:], via("not-a-uuid"), via("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA"), via(.null)] {
            XCTAssertEqual(parse(raw().putting("via", value)).route, .unknown, "\(value)")
        }
    }

    func testValidRouteAndAliasesPreserveNativeIdentity() {
        let machine = parse(raw().putting("aliases", JSON([peerId, otherId, peerId])).putting("via", via()))
        XCTAssertEqual(machine.route, .via(gatewayId: gatewayId, gatewayName: "Reported gateway"))
        XCTAssertEqual(machine.serverAliases, [peerId, otherId])
        XCTAssertEqual(machine.nativeName, "Computer")
    }

    func testMalformedOrOversizedAliasesAreNotUsed() {
        for value: JSON in ["alias", [:], JSON([gatewayId, "invalid"]), [7], JSON(Array(repeating: gatewayId, count: 4097))] {
            XCTAssertTrue(parse(raw().putting("aliases", value)).serverAliases.isEmpty)
        }
    }

    func testParentResolvesByExactIdOrServerAliasAndUsesCurrentPrivateName() {
        let peer = peer()
        XCTAssertEqual(MachineCatalog.gateway(peer, [peer, direct()]), direct())
        let canonical = MachineNames.apply(parse(raw(canonicalId, "Reported gateway").putting("via", .null).putting("aliases", JSON([gatewayId]))),
                                           [MachineAlias(key: MachineKey(connectionId: "workspace", computerId: canonicalId), name: "My desktop")])
        XCTAssertEqual(MachineCatalog.directGateway(peer, [peer, canonical]), canonical)
        XCTAssertEqual(MachineCatalog.gatewayName(peer, [peer, canonical]), "My desktop")
    }

    func testWorkspaceAndDuplicateNamesCannotBindAnotherGateway() {
        let peer = peer()
        let foreign = parse(raw(gatewayId, "Reported gateway").putting("via", .null), "other-workspace")
        let sameName = direct(otherId, "Reported gateway")
        XCTAssertNil(MachineCatalog.gateway(peer, [peer, foreign, sameName]))
        XCTAssertEqual(MachineCatalog.gatewayName(peer, [peer, foreign, sameName]), "Reported gateway")
    }

    func testAmbiguousAliasesAndSelfReferencesStayUnresolved() {
        let peer = peer()
        let alias = parse(raw(canonicalId).putting("via", .null).putting("aliases", JSON([gatewayId])))
        XCTAssertNil(MachineCatalog.gateway(peer, [peer, direct(), alias]))
        let selfRef = parse(raw(gatewayId).putting("via", via()))
        XCTAssertNil(MachineCatalog.gateway(selfRef, [selfRef]))
    }

    func testOrphanDoesNotBecomeDirectAndResolutionNeverFollowsAnotherHop() {
        let peer = peer()
        XCTAssertNil(MachineCatalog.directGateway(peer, [peer]))
        let intermediary = parse(raw(gatewayId, "Intermediate").putting("via", via(.string(otherId))))
        let third = direct(otherId, "Third computer")
        XCTAssertEqual(MachineCatalog.gateway(peer, [peer, intermediary, third]), intermediary)
        XCTAssertNil(MachineCatalog.directGateway(peer, [peer, intermediary, third]))
        XCTAssertNil(MachineCatalog.gateway(third, [peer, intermediary, third]))
    }

    func testUnsafeGatewayCaptionsDoNotInvalidateRoute() {
        for name: JSON in ["bad\nname", "bad\u{202e}name", .string(String(repeating: "x", count: 129)), 7, .null] {
            XCTAssertEqual(parse(raw().putting("via", via(name: name))).route, .via(gatewayId: gatewayId, gatewayName: ""))
        }
        let emoji = String(repeating: "😀", count: 128)
        XCTAssertEqual(parse(raw().putting("via", via(name: .string(emoji)))).route, .via(gatewayId: gatewayId, gatewayName: emoji))
    }

    func testTreeKeepsOneHopChildrenVisibleAndEveryMachineOnce() {
        let gateway = direct()
        var peer = peer(); peer.online = false; peer.lastKnown = true
        let other = direct(otherId, "Gateway")
        let unknown = Machine(connectionId: "workspace", id: canonicalId, name: "Gateway", online: false, lastKnown: true)
        var foreign = gateway; foreign.connectionId = "other"
        let rows = MachineCatalog.tree("workspace", [peer, foreign, unknown, gateway, other, peer])
        XCTAssertEqual(rows.map(\.machine.id), [gatewayId, peerId, otherId, canonicalId])
        XCTAssertEqual(rows[1].parent, gateway)
        XCTAssertTrue(rows.enumerated().filter { $0.offset != 1 }.allSatisfy { $0.element.parent == nil })
    }

    func testUnreachablePeerStaysUnderGatewayWithOptionalLastSeen() {
        let gateway = direct()
        let offline = parse(raw().putting("online", false).putting("via", via()).putting("last_seen_at", .null).putting("snapshot", .null))
        let group = MachineCatalog.groups("workspace", [gateway, offline]).first!
        XCTAssertEqual(group.root.machine, gateway)
        XCTAssertEqual(group.children.map(\.machine), [offline])
        XCTAssertEqual(group.children.first?.gatewayName, "Gateway")
        XCTAssertNil(offline.lastSeenAt)
        XCTAssertEqual(MachineCatalog.status(offline), "Offline")
        let seen = parse(raw().putting("online", false).putting("via", via()).putting("last_seen_at", 1_700_000_000.5))
        XCTAssertEqual(seen.lastSeenAt, 1_700_000_000.5)
        XCTAssertTrue(MachineCatalog.status(seen).hasPrefix("Offline · last seen "))
        var online = seen; online.online = true
        XCTAssertEqual(MachineCatalog.status(online), "Online")
        var known = seen; known.lastKnown = true
        XCTAssertEqual(MachineCatalog.status(known), "Last known")
        for invalid: JSON in ["1700000000", -1, true, [:]] {
            XCTAssertNil(parse(raw().putting("online", false).putting("last_seen_at", invalid)).lastSeenAt)
        }
    }

    func testCyclesOrAmbiguousParentsStayVisible() {
        let first = parse(raw(gatewayId).putting("via", via(.string(peerId))))
        let second = peer()
        let cycle = MachineCatalog.tree("workspace", [first, second])
        XCTAssertEqual(cycle.map(\.machine), [first, second])
        XCTAssertTrue(cycle.allSatisfy { $0.parent == nil })
    }

    func testProjectionKeepsWorkspacesSeparate() {
        var gateway = direct(); gateway.serverAliases = [gatewayId, canonicalId]
        let firstPeer = peer()
        let aliasPeer = parse(raw(otherId).putting("via", via(.string(canonicalId))))
        var foreign = gateway; foreign.connectionId = "other"; foreign.name = "Other workspace"
        var foreignPeer = firstPeer; foreignPeer.connectionId = "other"
        let projection = MachineCatalog.groupsByConnection([firstPeer, gateway, aliasPeer, foreign, foreignPeer])
        let group = projection["workspace"]!.first!
        XCTAssertEqual(group.root.machine, gateway)
        XCTAssertEqual(group.children.map(\.machine), [firstPeer, aliasPeer])
        XCTAssertEqual(projection["other"]?.first?.children.first?.gatewayName, "Other workspace")
    }

    func testPaletteAndAutomaticHashMatchDesktop() {
        XCTAssertEqual(MachineColors.palette, ["#5fbfa5", "#8ea9f4", "#c098e6", "#e7ae66", "#e38f9e", "#75bfcf", "#acc577"])
        XCTAssertEqual(MachineColors.automatic(MachineKey(connectionId: "workspace", computerId: "node")), "#c098e6")
        XCTAssertEqual(MachineColors.automatic(MachineKey(connectionId: "workspace", computerId: "550e8400-e29b-41d4-a716-446655440000")), "#e38f9e")
        XCTAssertEqual(MachineColors.automatic(MachineKey(connectionId: "workspace", computerId: "🖥️")), "#5fbfa5")
    }

    func testDesktopTonalBlends() throws {
        XCTAssertEqual(try MachineColors.foreground("#5fbfa5", dark: true), 0x9cd7c7)
        XCTAssertEqual(try MachineColors.background("#5fbfa5", dark: true), 0x274141)
        XCTAssertEqual(try MachineColors.foreground("#5fbfa5", dark: false), 0x3a6f65)
        XCTAssertEqual(try MachineColors.background("#5fbfa5", dark: false), 0xe4f4f0)
        for hex in ["red", "#123", "#12345678", "#12gg34", " #5fbfa5", "#٥fbfa5"] {
            XCTAssertThrowsError(try MachineColors.checked(hex), hex)
        }
        XCTAssertEqual(try MachineColors.checked("#8EA9F4"), "#8ea9f4")
    }

    func testNamesHaveBoundedLiteralUnicodeAndRejectControls() throws {
        XCTAssertEqual(try MachineNames.checked(String(repeating: "😀", count: 80)), String(repeating: "😀", count: 80))
        for bad in [" ", String(repeating: "x", count: 81), "bad\nname", "bad\u{202e}name", "bad\u{0}name"] {
            XCTAssertThrowsError(try MachineNames.checked(bad), bad)
        }
        XCTAssertEqual(try MachineNames.checked(" Studio / desk "), "Studio / desk")
    }

    func testAliasesAreWorkspaceAndNodeScoped() {
        let aliases = [MachineAlias(key: MachineKey(connectionId: "workspace", computerId: "node"), name: "Studio phone label")]
        XCTAssertEqual(MachineNames.apply(Machine(connectionId: "workspace", id: "node", name: "Fresh native name", online: true), aliases).name, "Studio phone label")
        XCTAssertEqual(MachineNames.apply(Machine(connectionId: "other", id: "node", name: "Native", online: true), aliases).name, "Native")
        XCTAssertEqual(MachineNames.apply(Machine(connectionId: "workspace", id: "other", name: "Native", online: true), aliases).name, "Native")
    }
}
