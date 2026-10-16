import XCTest
@testable import ZerusKit

/// Ported from Android PairingInviteTest, ManagedRelayTest and RelaySafetyTest.
final class PairingTests: XCTestCase {
    private let code = "synthetic_invitation_01234567890123456789"

    private func link(server: String = "https://relay.zerus.dev", code: String? = nil) -> String {
        let encoded = server.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._*")))!
        return "zerus://pair?server=\(encoded)&code=\(code ?? self.code)"
    }

    func testKnownInvitePreservesCodeAndCustomGateway() {
        XCTAssertEqual(PairingInvite(server: "https://private.example/gateway", code: code),
                       PairingInvite.parse(link(server: "https://private.example/gateway/")))
        XCTAssertEqual(PairingInvite(server: ManagedRelay.current, code: code),
                       PairingInvite.parse(link(server: "https://zerus.dev.guthub.dev")))
    }

    func testRejectsUnrelatedMalformedOrAmbiguousLinks() {
        let base = link()
        let rejected = [
            "https://example.test", base.replacingOccurrences(of: "zerus://pair", with: "zerus://other"),
            base + "&code=\(code)", base + "&%63ode=\(code)", base + "&extra=1", base + "#fragment",
            base.replacingOccurrences(of: "pair?", with: "pair:80?"), base.replacingOccurrences(of: "pair?", with: "user@pair?"),
            base.replacingOccurrences(of: "pair?", with: "pair/path?"), "zerus://pair?code=\(code)", base + "&",
            base.replacingOccurrences(of: "server=", with: "server=%GG"),
        ]
        for value in rejected { XCTAssertNil(PairingInvite.parse(value), value) }
    }

    func testRejectsUnsafeGatewayAndCode() {
        for server in ["http://relay.zerus.dev", "https://u:p@example.test", "https://example.test?q=1", "https://example.test#fragment",
                       "https://example.test:0", "https://example.test:65536", "https://example.test/ a"] {
            XCTAssertNil(PairingInvite.parse(link(server: server)), server)
        }
        for bad in ["short", String(repeating: "a", count: 129), String(repeating: "a", count: 16) + "%0A",
                    String(repeating: "a", count: 16) + "%FF", String(repeating: "a", count: 16) + "%2B"] {
            XCTAssertNil(PairingInvite.parse(link(code: bad)), bad)
        }
        XCTAssertNil(PairingInvite.parse(link() + String(repeating: "x", count: 4096)))
    }

    func testManagedAliasRequiresExactOrigin() {
        XCTAssertEqual(PairingInvite.parse(link(server: "https://zerus.dev.guthub.dev/custom"))?.server, "https://zerus.dev.guthub.dev/custom")
        XCTAssertEqual(PairingInvite.parse(link(server: "https://zerus.dev.guthub.dev.example"))?.server, "https://zerus.dev.guthub.dev.example")
    }

    func testOnlyExactManagedHttpsRootMigrates() {
        for value in ["https://zerus.dev.guthub.dev", "https://zerus.dev.guthub.dev/", "https://ZERUS.DEV.GUTHUB.DEV:443/"] {
            XCTAssertEqual(ManagedRelay.transport(value), ManagedRelay.current, value)
        }
        for value in ["http://zerus.dev.guthub.dev", "https://zerus.dev.guthub.dev:444", "https://zerus.dev.guthub.dev/path",
                      "https://zerus.dev.guthub.dev?x=1", "https://zerus.dev.guthub.dev#x", "https://synthetic-user" + "@" + "zerus.dev.guthub.dev",
                      "https://zerus.dev.guthub.dev.example", "https://custom.example"] {
            XCTAssertEqual(ManagedRelay.transport(value), value, value)
        }
    }

    func testDisplayNameUsesCanonicalHostForLegacyOrUuidLabels() {
        let connection = Connection(id: "saved-workspace", label: "", url: "https://zerus.dev.guthub.dev", token: "synthetic")
        XCTAssertEqual(connection.displayName, "relay.zerus.dev")
        XCTAssertEqual(Connection(id: "a", label: "zerus.dev.guthub.dev", url: connection.url, token: "t").displayName, "relay.zerus.dev")
        XCTAssertEqual(Connection(id: "a", label: "My workspace", url: connection.url, token: "t").displayName, "My workspace")
        XCTAssertEqual(Connection(id: "a", label: "6f9619ff-8b86-d011-b42d-00cf4fc964ff", url: "https://gateway.example", token: "t").displayName, "gateway.example")
        XCTAssertEqual(connection.url, "https://zerus.dev.guthub.dev")
        XCTAssertEqual(connection.endpoint, ManagedRelay.current)
    }

    func testServerUrlRequiresHttpsAndRejectsCredentialAndQueryInjection() throws {
        XCTAssertEqual(try EndpointPolicy.normalize(" https://gateway.example/ "), "https://gateway.example")
        for value in ["http://gateway.example", "https://user:secret@gateway.example", "https://gateway.example?token=private",
                      "https://gateway.example#fragment", "https://", "gateway.example"] {
            XCTAssertThrowsError(try EndpointPolicy.normalize(value), value)
        }
    }

    func testDestinationIncludesGatewayMachineRunAndConversation() throws {
        let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "gateway")
        XCTAssertNotEqual(target.key, Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "other").key)
        XCTAssertNotEqual(target.key, Target(computerId: "other", session: "session", run: "run", conversation: "conversation", connectionId: "gateway").key)
        XCTAssertNotEqual(target.key, Target(computerId: "computer", session: "session", run: "next", conversation: "conversation", connectionId: "gateway").key)
        XCTAssertNotEqual(target.key, Target(computerId: "computer", session: "session", run: "run", conversation: "next", connectionId: "gateway").key)
        XCTAssertEqual(target.key, #"["gateway","computer","session","run","conversation"]"#)
        let json = try target.json()
        XCTAssertEqual(json["expected_run_id"], "run")
        XCTAssertEqual(json["expected_conversation_id"], "conversation")
        XCTAssertThrowsError(try Target(computerId: "c", session: "s", run: "r", conversation: "v", connectionId: "g", archiveId: "a").json())
    }

    func testChildTargetKeyAndPayloadNameParentConversation() throws {
        let child = Target(computerId: "c", session: "s", run: "r", conversation: "parent/agent-1", connectionId: "g", agentId: "agent-1", parentConversation: "parent")
        XCTAssertEqual(child.key, #"["g","c","s","r","parent/agent-1","agent","agent-1","parent"]"#)
        let json = try child.json()
        XCTAssertEqual(json["expected_conversation_id"], "parent")
        XCTAssertEqual(json["agent_id"], "agent-1")
    }
}
