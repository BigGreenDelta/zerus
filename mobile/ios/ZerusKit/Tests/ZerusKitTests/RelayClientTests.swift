import XCTest
@testable import ZerusKit

/// Ported from Android RelaySafetyTest and ManagedRelayTest, plus iOS transport limits.
final class RelayClientTests: XCTestCase {
    private let connection = Connection(id: "gateway", label: "Test", url: "https://gateway.example", token: "synthetic-token")
    private let target = Target(computerId: "machine", session: "native-session", run: "exact-run", conversation: "exact-conversation", connectionId: "gateway")

    func testLostTransportMakesOnlyOneNativeMutationAttempt() async throws {
        StubProtocol.reset { _ in .failure(.networkConnectionLost) }
        let payload = try target.json().putting("request_id", "request-one").putting("text", "synthetic message")
        do {
            _ = try await StubProtocol.client().submit(connection, target: target, operation: "send", payload: payload, id: "request-one")
            XCTFail("A lost connection must surface as an error")
        } catch {}
        let requests = StubProtocol.requests
        XCTAssertEqual(requests.count, 1)
        XCTAssertEqual(requests[0].method, "POST")
        XCTAssertEqual(requests[0].url.path, "/v1/requests")
        XCTAssertEqual(requests[0].headers["Authorization"], "Bearer synthetic-token")
        XCTAssertEqual(requests[0].json?.string("computer_id"), "machine")
        XCTAssertEqual(requests[0].json?["payload"]?.string("expected_run_id"), "exact-run")
        XCTAssertEqual(requests[0].json?.string("operation"), "send")
        XCTAssertEqual(requests[0].json?.string("session"), "native-session")
    }

    func testPollingKnownReceiptDoesNotCreateReplacementRequest() async throws {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"request_id":"known","state":"completed","result":{"status":"submitted"}}"#) }
        let receipt = try await StubProtocol.client().waitForReceipt(connection, initial: ["request_id": "known", "state": "claimed"],
                                                                      maxPolls: 1, pollDelay: .milliseconds(50))
        XCTAssertEqual(receipt.string("state"), "completed")
        XCTAssertEqual(StubProtocol.requests.map(\.method), ["GET"])
        XCTAssertEqual(StubProtocol.requests.map(\.url.path), ["/v1/requests/known"])
    }

    func testTerminalInitialReceiptNeedsNoPolling() async throws {
        StubProtocol.reset { _ in StubProtocol.json(500, "{}") }
        let receipt = try await StubProtocol.client().waitForReceipt(connection, initial: ["request_id": "known", "state": "uncertain"])
        XCTAssertEqual(receipt.string("state"), "uncertain")
        XCTAssertTrue(StubProtocol.requests.isEmpty)
    }

    func testOversizedResponseIsRejected() async {
        StubProtocol.reset { _ in .response(status: 200, body: Data(repeating: UInt8(ascii: "x"), count: 1024 * 1024 + 1)) }
        do {
            _ = try await StubProtocol.client().call(connection.url, token: "", path: "/v1/computers")
            XCTFail("Expected the 1 MiB response limit")
        } catch let error as ZerusError {
            XCTAssertEqual(error.kind, .limit)
        } catch { XCTFail("Unexpected \(error)") }
    }

    func testMismatchedReceiptRemainsUnconfirmed() async throws {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"request_id":"other","state":"queued"}"#) }
        do {
            _ = try await StubProtocol.client().submit(connection, target: target, operation: "send", payload: [:], id: "mine")
            XCTFail("A different receipt must not be accepted")
        } catch let error as ZerusError {
            XCTAssertEqual(error.kind, .mismatch)
            XCTAssertEqual(error.message, "Gateway returned a mismatched receipt. Delivery remains unconfirmed.")
        }
        do {
            _ = try await StubProtocol.client().receipt(connection, id: "mine")
            XCTFail("A different receipt must not be accepted")
        } catch let error as ZerusError { XCTAssertEqual(error.kind, .mismatch) }
    }

    func testKnownReceiptAndPushUseCanonicalTransportWithoutReplacingRequest() async throws {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"request_id":"original","state":"completed"}"#) }
        let legacy = Connection(id: "original-identity", label: "Workspace", url: "https://zerus.dev.guthub.dev", token: "synthetic")
        let client = StubProtocol.client()
        _ = try await client.receipt(legacy, id: "original")
        _ = try await client.call(legacy.url, token: legacy.token, path: "/v1/push", body: ["provider": "apns", "token": "synthetic-token"])
        let requests = StubProtocol.requests
        XCTAssertEqual(requests.map { $0.url.host }, ["relay.zerus.dev", "relay.zerus.dev"])
        XCTAssertEqual(requests[0].method, "GET")
        XCTAssertEqual(requests[0].url.path, "/v1/requests/original")
        XCTAssertEqual(requests[0].headers["Authorization"], "Bearer synthetic")
        XCTAssertEqual(requests[1].method, "POST")
    }

    func testErrorStatusCarriesGatewayMessage() async {
        StubProtocol.reset { _ in StubProtocol.json(409, #"{"error":"Request body changed"}"#) }
        do {
            _ = try await StubProtocol.client().call(connection.url, token: "t", path: "/v1/computers")
            XCTFail("Expected a gateway error")
        } catch let error as RelayError {
            XCTAssertEqual(error.status, 409)
            XCTAssertEqual(error.message, "Request body changed")
        } catch { XCTFail("Unexpected \(error)") }
        StubProtocol.reset { _ in StubProtocol.json(503, "not json") }
        do {
            _ = try await StubProtocol.client().call(connection.url, token: "t", path: "/v1/computers")
        } catch let error as RelayError {
            XCTAssertEqual(error.message, "Server returned 503.")
        } catch { XCTFail("Unexpected \(error)") }
    }

    func testRedirectStatusIsAnErrorNotAFollowedLocation() async {
        StubProtocol.reset { _ in .response(status: 302, body: Data(), headers: ["Location": "https://other.example/v1/computers"]) }
        do {
            _ = try await StubProtocol.client().call(connection.url, token: "t", path: "/v1/computers")
            XCTFail("A redirect must not succeed")
        } catch let error as RelayError {
            XCTAssertEqual(error.status, 302)
        } catch { XCTFail("Unexpected \(error)") }
        XCTAssertEqual(StubProtocol.requests.count, 1)
    }

    func testPairNormalizesGatewayAndSendsDeviceName() async throws {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"device_id":"d","device_token":"secret","workspace_id":"w","workspace_name":"Home lab"}"#) }
        let paired = try await StubProtocol.client().pair(url: " https://gateway.example/ ", code: "  invite-code  ")
        XCTAssertEqual(paired.url, "https://gateway.example")
        XCTAssertEqual(paired.label, "Home lab")
        XCTAssertEqual(paired.token, "secret")
        XCTAssertNotNil(UUID(uuidString: paired.id))
        let request = StubProtocol.requests.first
        XCTAssertEqual(request?.url.path, "/v1/pair")
        XCTAssertNil(request?.headers["Authorization"])
        XCTAssertEqual(request?.json?.string("code"), "invite-code")
        XCTAssertEqual(request?.json?.string("device_name"), "Zerus iOS")
    }

    func testPairRejectsHttpAndBlankCodeWithoutNetwork() async {
        StubProtocol.reset { _ in StubProtocol.json(200, "{}") }
        for (url, code) in [("http://gateway.example", "code"), ("https://gateway.example", "  ")] {
            do {
                _ = try await StubProtocol.client().pair(url: url, code: code)
                XCTFail("Expected rejection")
            } catch {}
        }
        XCTAssertTrue(StubProtocol.requests.isEmpty)
    }

    func testPairWithoutTokenFails() async {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"workspace_name":"x"}"#) }
        do {
            _ = try await StubProtocol.client().pair(url: "https://gateway.example", code: "code")
            XCTFail("Expected rejection")
        } catch {}
    }

    func testBodyLimitsAreCheckedBeforeSending() async {
        StubProtocol.reset { _ in StubProtocol.json(200, "{}") }
        let large = String(repeating: "a", count: 1024 * 1024)
        do {
            _ = try await StubProtocol.client().call(connection.url, token: "t", path: "/v1/push", body: ["token": .string(large)])
            XCTFail("Expected the 1 MiB request limit")
        } catch let error as ZerusError { XCTAssertEqual(error.kind, .limit) } catch { XCTFail("Unexpected \(error)") }
        XCTAssertTrue(StubProtocol.requests.isEmpty)
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"request_id":"r","state":"queued"}"#) }
        _ = try? await StubProtocol.client().call(connection.url, token: "t", path: "/v1/requests", body: ["request_id": "r", "payload": .string(large)])
        XCTAssertEqual(StubProtocol.requests.count, 1, "Requests may carry attachments up to the 29 MiB envelope")
    }

    func testArchivedAndChildTargetsRejectMutationsWithoutNetwork() async {
        StubProtocol.reset { _ in StubProtocol.json(200, "{}") }
        let archived = Target(computerId: "c", session: "s", run: "r", conversation: "v", connectionId: "gateway", archiveId: "a")
        let child = Target(computerId: "c", session: "s", run: "r", conversation: "v/x", connectionId: "gateway", agentId: "x", parentConversation: "v")
        let attempts: [(Target, String, JSONObject)] = [(archived, "send", [:]), (archived, "inspect", [:]), (child, "interrupt", [:])]
        for (target, operation, payload) in attempts {
            do {
                _ = try await StubProtocol.client().submit(connection, target: target, operation: operation, payload: payload, id: "x")
                XCTFail("\(operation) must be rejected")
            } catch let error as ZerusError { XCTAssertEqual(error.kind, .invalid) } catch { XCTFail("Unexpected \(error)") }
        }
        XCTAssertTrue(StubProtocol.requests.isEmpty)
        _ = try? await StubProtocol.client().submit(connection, target: archived, operation: "inspect", payload: ["archive_id": "a"], id: "x")
        XCTAssertEqual(StubProtocol.requests.count, 1, "Archive inspection with its exact identity is allowed")
    }

    func testRevokeUsesDeleteDevice() async throws {
        StubProtocol.reset { _ in .response(status: 204, body: Data()) }
        try await StubProtocol.client().revoke(connection)
        XCTAssertEqual(StubProtocol.requests.map(\.method), ["DELETE"])
        XCTAssertEqual(StubProtocol.requests.map(\.url.path), ["/v1/device"])
    }
}
