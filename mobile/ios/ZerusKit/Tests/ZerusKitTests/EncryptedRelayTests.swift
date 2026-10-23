import CryptoKit
import Foundation
import Synchronization
import XCTest
@testable import ZerusKit

final class EncryptedRelayTests: XCTestCase {
    private nonisolated static func waitForHeldPage(_ semaphore: DispatchSemaphore) -> Bool {
        semaphore.wait(timeout: .now() + 5) == .success
    }

    private struct CatalogFixture {
        let gateway: WorkspaceIdentity, phone: WorkspaceIdentity, candidate: WorkspaceCandidate
        let bootstrap: WorkspaceBootstrap, approval: WorkspaceApproval, roster: WorkspaceMembership
        init() throws {
            let f = try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object)
            gateway = try WorkspaceIdentity.parse(XCTUnwrap(f["gateway_identity"]?.object)); phone = try WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object))
            candidate = try WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object)); bootstrap = try WorkspaceBootstrap.parse(XCTUnwrap(f["bootstrap_snapshot"]?.object))
            approval = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object)); roster = try WorkspaceMembership.parse(XCTUnwrap(f["final_state"]?.object))
        }
        var history: JSONObject { ["changes": [], "membership_revision": .int(roster.revision), "membership_head": .string(roster.head), "next_after": .null] }
        func journal(_ connection: Connection, directory: URL) throws -> SQLiteWorkspaceJournal {
            let journal = SQLiteWorkspaceJournal(directory: directory, testKeys: StaticKeyProvider(SymmetricKey(data: Data(repeating: 9, count: 32))))
            let pending = WorkspaceState(identity: phone, candidate: candidate, issuer: gateway.device, pinnedRevision: 0, pinnedHead: bootstrap.state.head)
            try journal.create(connection, state: pending)
            var first = pending; first.membership = bootstrap.state; first.enrollment = approval
            try journal.update(connection, expected: pending, next: first)
            var active = first; active.membership = roster
            active.routes = ["old": WorkspaceRoute(computerId: "old", gatewayId: gateway.device.deviceId, routeId: "route-old", machineId: "old")]
            try journal.update(connection, expected: first, next: active, changes: [approval.membership])
            return journal
        }
        func snapshot(_ computer: String, expires: Int64, name: String = "Fresh computer", privateId: String? = nil, clearOverride: String? = nil, routeId: String? = nil) throws -> JSONObject {
            let native = privateId ?? computer, route = routeId ?? "route-\(computer)"
            let clear = EncryptedRelayTests.catalogContent(native, name: name).putting("encrypted_transport", .object([
                "gateway_id": .string(gateway.device.deviceId), "route_id": .string(route), "machine_id": .string(native)]))
            let header = try WorkspaceHeader(workspaceId: roster.workspaceId, senderDeviceId: gateway.device.deviceId, recipientDeviceId: phone.device.deviceId,
                senderKeyGeneration: 1, recipientKeyGeneration: 1, gatewayId: gateway.device.deviceId, routeId: route, requestId: "snapshot-\(computer)",
                kind: "snapshot", operationClass: "read", expiresAt: expires, membershipRevision: roster.revision, membershipHead: roster.head)
            let plaintext = try clearOverride.map { Data($0.utf8) } ?? JSON.object(clear).data()
            let envelope = try WorkspaceEnvelope.seal(identity: gateway, recipient: phone.device, header: header, plaintext: plaintext)
            return ["computer_id": .string(computer), "gateway_id": .string(gateway.device.deviceId), "route_id": .string(route), "online": true, "envelope": .object(envelope.json)]
        }
    }

    private static func catalogContent(_ computer: String, name: String = "Cached computer", folder: String = "/workspace/cached") -> JSONObject {
        let session: JSONObject = ["name": .string("codex/fixture/\(computer)"), "run_id": .string("run-\(computer)"), "conversation_id": .string("conversation-\(computer)"),
            "state": "running", "activity": "idle", "mobile_project_id": "shared", "cwd": .string(folder)]
        let account: JSONObject = ["id": "fixture", "provider": "codex", "usage": .object(["status": "ok", "identity": .object(["name": .string(name)])])]
        let project: JSONObject = ["id": "shared", "name": .string(name), "folders": .array([.object(["id": .string("folder-\(computer)"), "path": .string(folder)])])]
        let snapshot: JSONObject = ["sessions": .array([.object(session)]),
            "mobile_capabilities": .object(["protocol_version": 1, "operations": ["history"], "features": ["accounts_snapshot"]]),
            "mobile_accounts": .object(["schema": 1, "available": true, "checked_at": 1000, "accounts": .array([.object(account)])]),
            "mobile_projects": .object(["schema": 1, "available": true, "swarm_id": "swarm", "projects": .array([.object(project)])])]
        return ["id": .string(computer), "machine_id": .string(computer), "name": .string(name), "online": true, "via": .null, "snapshot": .object(snapshot)]
    }

    private func serveCatalog(_ fixture: CatalogFixture, pages: [[JSONObject]], onPage: @escaping @Sendable (Int) -> Void = { _ in }, onPost: @escaping @Sendable (WorkspaceEnvelope) -> Void = { _ in }) {
        let history = fixture.history
        StubProtocol.reset { request in
            do {
                if request.url.path == "/v2/membership" { return .response(status: 200, body: try JSON.object(history).data()) }
                if request.url.path == "/v2/computers" {
                    let after = URLComponents(url: request.url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "after" })?.value
                    let index = after.flatMap { Int($0.dropFirst(5)) } ?? 0
                    guard pages.indices.contains(index) else { return StubProtocol.json(400, "{}") }
                    onPage(index)
                    return .response(status: 200, body: try JSON.object(["computers": .array(pages[index].map(JSON.object)),
                        "next_after": index + 1 < pages.count ? .string("page-\(index + 1)") : .null]).data())
                }
                if request.method == "POST", request.url.path == "/v2/requests" {
                    let envelope = try WorkspaceEnvelope.parse(request.json?["envelope"]?.object ?? JSONObject()); onPost(envelope)
                    return .response(status: 200, body: try JSON.object(["request_id": .string(envelope.header.requestId), "state": "queued", "envelope": .null]).data())
                }
                return StubProtocol.json(404, "{}")
            } catch { return StubProtocol.json(500, "{}") }
        }
    }

    func testExpiredComputerKeepsFreshComputerUsableAcrossRowsPagesAndRepeatedRefresh() async throws {
        let fixture = try CatalogFixture(), now: Int64 = 2_000_000_000
        let expired = try fixture.snapshot("old", expires: now - 60, name: "Expired private content"), current = try fixture.snapshot("fresh", expires: now + 300)
        for pages in [[[expired, current]], [[current, expired]], [[expired], [current]], [[current], [expired]]] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            let connection = Connection(id: "mixed-catalog", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
            let journal = try fixture.journal(connection, directory: directory), posted = Mutex<[String]>([])
            serveCatalog(fixture, pages: pages, onPost: { envelope in posted.withLock { $0.append(envelope.header.routeId) } })
            let client = RelayClient(session: StubProtocol.client().session, workspaceJournal: journal, catalogNowSeconds: { now })
            for _ in 0..<2 {
                let result: JSONObject
                do { result = try await client.computers(connection) }
                catch { XCTFail("Expired A must not block fresh B: \(error)"); return }
                XCTAssertEqual(result.objects("computers").map { $0.string("id") }, ["fresh"])
                XCTAssertEqual(result.strings("unavailable_computers"), ["old"])
                XCTAssertFalse(try JSON.object(result).text().contains("Expired private content"))
                XCTAssertEqual(Set(try journal.load(connection).routes.keys), ["fresh"])
            }
            do { _ = try await client.post(connection, computerId: "old", operation: "inspect", payload: [:], id: "expired-action"); XCTFail("Expired route authorized a new request") } catch {}
            _ = try await client.post(connection, computerId: "fresh", operation: "inspect", payload: [:], id: "fresh-action")
            XCTAssertEqual(posted.withLock { $0 }, ["route-fresh"])
        }
    }

    func testCatalogAdmissionRechecksExpiryAfterHeldPageAcrossRowAndPageOrders() async throws {
        let fixture = try CatalogFixture(), now: Int64 = 2_000_000_000
        let expiring = try fixture.snapshot("old", expires: now + 1, name: "Expiring private content")
        let fresh = try fixture.snapshot("fresh", expires: now + 300)
        for pages in [[[expiring, fresh], []], [[fresh, expiring], []], [[expiring], [fresh]], [[fresh], [expiring], []]] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            let connection = Connection(id: "held-page", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
            let journal = try fixture.journal(connection, directory: directory), clock = Mutex(now)
            let entered = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
            serveCatalog(fixture, pages: pages, onPage: { index in
                if index == pages.count - 1 { entered.signal(); XCTAssertEqual(release.wait(timeout: .now() + 5), .success) }
            })
            let client = RelayClient(session: StubProtocol.client().session, workspaceJournal: journal, catalogNowSeconds: { clock.withLock { $0 } })
            let pending = Task { try await client.computers(connection) }
            defer { release.signal(); pending.cancel() }
            let reached = await Task.detached { EncryptedRelayTests.waitForHeldPage(entered) }.value
            XCTAssertTrue(reached, "A later page must be held before advancing the clock")
            clock.withLock { $0 = now + 2 }; release.signal()
            let result = try await pending.value
            XCTAssertEqual(result.objects("computers").map { $0.string("id") }, ["fresh"])
            XCTAssertEqual(result.strings("unavailable_computers"), ["old"])
            XCTAssertFalse(try JSON.object(result).text().contains("Expiring private content"))
            XCTAssertEqual(Set(try journal.load(connection).routes.keys), ["fresh"])
            do { _ = try await client.post(connection, computerId: "old", operation: "inspect", payload: [:], id: "expired-after-page"); XCTFail("Expired candidate authorized a new request") } catch {}
        }
    }

    func testFinalAdmissionKeepsFreshAlternativeWhenPreferredRouteExpiresDuringHeldPage() async throws {
        let fixture = try CatalogFixture(), now: Int64 = 2_000_000_000
        let preferred = try fixture.snapshot("same", expires: now + 1, name: "Expiring preferred", routeId: "preferred-route")
        let alternative = try fixture.snapshot("same", expires: now + 300, name: "Usable alternative", routeId: "alternative-route")
        for pages in [[[preferred, alternative], []], [[alternative, preferred], []], [[preferred], [alternative]], [[alternative], [preferred], []]] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            let connection = Connection(id: "held-alternative", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
            let journal = try fixture.journal(connection, directory: directory), clock = Mutex(now), posted = Mutex<[String]>([])
            let original = try journal.load(connection)
            var preferredState = original
            preferredState.routes = ["same": WorkspaceRoute(computerId: "same", gatewayId: fixture.gateway.device.deviceId, routeId: "preferred-route", machineId: "same")]
            try journal.update(connection, expected: original, next: preferredState)
            let entered = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
            serveCatalog(fixture, pages: pages, onPage: { index in
                if index == pages.count - 1 { entered.signal(); XCTAssertEqual(release.wait(timeout: .now() + 5), .success) }
            }, onPost: { envelope in posted.withLock { $0.append(envelope.header.routeId) } })
            let client = RelayClient(session: StubProtocol.client().session, workspaceJournal: journal, catalogNowSeconds: { clock.withLock { $0 } })
            let pending = Task { try await client.computers(connection) }
            defer { release.signal(); pending.cancel() }
            let reached = await Task.detached { EncryptedRelayTests.waitForHeldPage(entered) }.value
            XCTAssertTrue(reached, "A later page must be held before advancing the clock")
            clock.withLock { $0 = now + 2 }; release.signal()
            let result = try await pending.value
            XCTAssertEqual(result.objects("computers").first?.string("name"), "Usable alternative")
            XCTAssertTrue(result.strings("unavailable_computers").isEmpty)
            XCTAssertEqual(try journal.load(connection).routes["same"]?.routeId, "alternative-route")
            _ = try await client.post(connection, computerId: "same", operation: "inspect", payload: [:], id: "admitted-alternative")
            XCTAssertEqual(posted.withLock { $0 }, ["alternative-route"])
        }
    }

    func testFreshSameComputerWinsOverExpiredRouteInBothPageOrders() async throws {
        let fixture = try CatalogFixture(), now = RelayClient.nowSeconds
        let expired = try fixture.snapshot("same", expires: now - 60, name: "Expired private content"), current = try fixture.snapshot("same", expires: now + 300, name: "Current private content")
        for pages in [[[expired, current]], [[current, expired]], [[expired], [current]], [[current], [expired]]] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            let connection = Connection(id: "same-computer", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
            let journal = try fixture.journal(connection, directory: directory)
            serveCatalog(fixture, pages: pages)
            let result = try await RelayClient(session: StubProtocol.client().session, workspaceJournal: journal).computers(connection)
            XCTAssertEqual(result.objects("computers").first?.string("name"), "Current private content")
            XCTAssertTrue(result.strings("unavailable_computers").isEmpty)
            XCTAssertEqual(Set(try journal.load(connection).routes.keys), ["same"])
        }
    }

    func testExpiredMalformedOrUnauthenticatedSnapshotStillRefusesWholeCatalog() async throws {
        let fixture = try CatalogFixture(), now = RelayClient.nowSeconds
        let original = try fixture.snapshot("old", expires: now - 60)
        let envelope = try XCTUnwrap(original["envelope"]?.object), header = try XCTUnwrap(envelope["header"]?.object)
        let invalidSignature = original.putting("envelope", .object(envelope.putting("signature", .string(WorkspaceProtocol.encode(Data(repeating: 0, count: 64))))))
        let invalidHead = original.putting("envelope", .object(envelope.putting("header", .object(header.putting("membership_head", .string(WorkspaceProtocol.encode(Data(repeating: 0, count: 32))))))))
        let invalidBinding = try fixture.snapshot("old", expires: now - 60, privateId: "substituted-native")
        let invalidJSON = try fixture.snapshot("old", expires: now - 60, clearOverride: "not JSON")
        for bad in [invalidSignature, invalidHead, invalidBinding, invalidJSON] {
            let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
            defer { try? FileManager.default.removeItem(at: directory) }
            let connection = Connection(id: "bad-catalog", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
            let journal = try fixture.journal(connection, directory: directory), previous = try journal.load(connection).routes
            serveCatalog(fixture, pages: [[try fixture.snapshot("fresh", expires: now + 300), bad]])
            do { _ = try await RelayClient(session: StubProtocol.client().session, workspaceJournal: journal).computers(connection); XCTFail("Invalid expired snapshot admitted") } catch {}
            XCTAssertEqual(try journal.load(connection).routes, previous)
        }
    }

    func testUnavailableCachePreservesOnlyPriorScopedDisplayDataWithoutCapabilities() throws {
        let connection = Connection(id: "cache", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
        let previous = SessionCatalogParser.parse(connection, [Self.catalogContent("old", name: "Cached A"), Self.catalogContent("fresh", name: "Cached B"), Self.catalogContent("removed", name: "Removed C")])
        let incoming = SessionCatalogParser.parse(connection, [Self.catalogContent("fresh", name: "Current B", folder: "/workspace/current")])
        let response: JSONObject = ["unavailable_computers": ["old", "fresh", "unknown"]]
        let first = WorkspaceCatalogCache.preserve(connection, response: response, fresh: incoming, sessions: previous.projects.sessions, machines: previous.machines, projects: previous.projects.projects, accounts: previous.accounts)
        let repeated = WorkspaceCatalogCache.preserve(connection, response: response, fresh: incoming, sessions: first.projects.sessions, machines: first.machines, projects: first.projects.projects, accounts: first.accounts)
        for result in [first, repeated] {
            XCTAssertEqual(Set(result.machines.map(\.id)), ["old", "fresh"])
            let stale = try XCTUnwrap(result.machines.first { $0.id == "old" }); XCTAssertFalse(stale.online); XCTAssertTrue(stale.lastKnown)
            XCTAssertEqual(result.machines.first { $0.id == "fresh" }?.name, "Current B")
            XCTAssertTrue(try XCTUnwrap(result.projects.sessions.first { $0.target.computerId == "old" }).raw.bool("last_known"))
            let account = try XCTUnwrap(result.accounts.first { $0.key.computerId == "old" })
            XCTAssertTrue(account.stale); XCTAssertFalse(account.online); XCTAssertFalse(account.available); XCTAssertEqual(account.accounts.first?.identity.name, "Cached A")
            let project = try XCTUnwrap(result.projects.projects.first)
            XCTAssertTrue(project.stale); XCTAssertEqual(Set(project.computers.keys), ["old", "fresh"])
            XCTAssertEqual(project.folders.first { $0.computerId == "fresh" }?.path, "/workspace/current")
            XCTAssertEqual(Set(result.operations.keys), [MachineKey(connectionId: connection.id, computerId: "fresh")])
            XCTAssertEqual(Set(result.features.keys), [MachineKey(connectionId: connection.id, computerId: "fresh")])
        }
        let foreign = Connection(id: "foreign", label: "Fixture", url: connection.url, token: connection.token, protocolVersion: 2)
        let foreignPrior = SessionCatalogParser.parse(foreign, [Self.catalogContent("old")])
        XCTAssertEqual(WorkspaceCatalogCache.preserve(connection, response: response, fresh: incoming, sessions: foreignPrior.projects.sessions, machines: foreignPrior.machines, projects: foreignPrior.projects.projects, accounts: foreignPrior.accounts).machines, incoming.machines)
    }

    func testOnlineCatalogRouteWinsForNewRequestsWhilePendingCiphertextKeepsOriginalRoute() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let f = try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object)
        let gateway = try WorkspaceIdentity.parse(XCTUnwrap(f["gateway_identity"]?.object)), phone = try WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object))
        let candidate = try WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object)), bootstrap = try WorkspaceBootstrap.parse(XCTUnwrap(f["bootstrap_snapshot"]?.object))
        let approval = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object)), roster = try WorkspaceMembership.parse(XCTUnwrap(f["final_state"]?.object))
        let computer = "22222222-2222-4222-8222-222222222222", pendingId = "11111111-1111-4111-8111-111111111111", newId = "33333333-3333-4333-8333-333333333333"
        let journal = SQLiteWorkspaceJournal(directory: directory, testKeys: StaticKeyProvider(SymmetricKey(data: Data(repeating: 8, count: 32))))
        let connection = Connection(id: "route-choice", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
        let pending = WorkspaceState(identity: phone, candidate: candidate, issuer: gateway.device, pinnedRevision: 0, pinnedHead: bootstrap.state.head)
        try journal.create(connection, state: pending)
        var first = pending; first.membership = bootstrap.state; first.enrollment = approval
        try journal.update(connection, expected: pending, next: first)
        var active = first; active.membership = roster
        let preferredRoute = WorkspaceRoute(computerId: computer, gatewayId: gateway.device.deviceId, routeId: "preferred-route", machineId: computer)
        active.routes = [computer: preferredRoute]
        try journal.update(connection, expected: first, next: active, changes: [approval.membership])
        func snapshot(_ route: String) throws -> JSONObject {
            let clear: JSONObject = ["id": .string(computer), "machine_id": .string(computer), "name": "Private computer", "via": .null,
                "snapshot": .object(["sessions": []]), "encrypted_transport": .object(["gateway_id": .string(gateway.device.deviceId), "route_id": .string(route), "machine_id": .string(computer)])]
            let header = try WorkspaceHeader(workspaceId: roster.workspaceId, senderDeviceId: gateway.device.deviceId, recipientDeviceId: phone.device.deviceId,
                senderKeyGeneration: 1, recipientKeyGeneration: 1, gatewayId: gateway.device.deviceId, routeId: route, requestId: "snapshot-\(route)",
                kind: "snapshot", operationClass: "read", expiresAt: RelayClient.nowSeconds + 300, membershipRevision: roster.revision, membershipHead: roster.head)
            let envelope = try WorkspaceEnvelope.seal(identity: gateway, recipient: phone.device, header: header, plaintext: JSON.object(clear).data())
            return ["computer_id": .string(computer), "gateway_id": .string(gateway.device.deviceId), "route_id": .string(route), "envelope": .object(envelope.json)]
        }
        let preferred = try snapshot("preferred-route"), alternative = try snapshot("online-route"), mode = Mutex(0)
        let history: JSONObject = ["changes": [], "membership_revision": .int(roster.revision), "membership_head": .string(roster.head), "next_after": .null]
        StubProtocol.reset { request in
            do {
                if request.url.path == "/v2/membership" { return .response(status: 200, body: try JSON.object(history).data()) }
                if request.url.path == "/v2/computers" {
                    let rows: [JSONObject]
                    switch mode.withLock({ $0 }) {
                    case 0: rows = [alternative.putting("online", true), preferred.putting("online", false)]
                    case 1: rows = [alternative.putting("online", true), preferred.putting("online", true)]
                    case 2: rows = [preferred.putting("online", false), alternative.putting("online", false)]
                    default: rows = [preferred.putting("online", false), alternative.putting("online", true)]
                    }
                    return .response(status: 200, body: try JSON.object(["computers": .array(rows.map(JSON.object)), "next_after": .null]).data())
                }
                XCTAssertEqual(request.method, "POST"); XCTAssertEqual(request.url.path, "/v2/requests")
                let envelope = try WorkspaceEnvelope.parse(request.json?["envelope"]?.object ?? JSONObject())
                let clear = try JSON.parse(envelope.open(identity: gateway, sender: phone.device, expected: envelope.header))
                XCTAssertEqual(clear["encrypted_transport"]?.string("route_id"), envelope.header.routeId)
                return .response(status: 200, body: try JSON.object(["request_id": .string(envelope.header.requestId), "state": "queued", "envelope": .null]).data())
            } catch { return StubProtocol.json(500, #"{"error":"Synthetic endpoint rejected the envelope"}"#) }
        }
        let client = RelayClient(session: StubProtocol.client().session, workspaceJournal: journal)
        let target = Target(computerId: computer, session: "native-session", run: "run", conversation: "conversation", connectionId: connection.id)
        let payload = try target.json().putting("text", "Private message")
        _ = try await client.submit(connection, target: target, operation: "send", payload: payload, id: pendingId)
        let computers = try await client.computers(connection).objects("computers")
        XCTAssertEqual(computers.count, 1); XCTAssertEqual(computers.first?.bool("online"), true)
        XCTAssertEqual(try journal.load(connection).routes[computer]?.routeId, "online-route")
        _ = try await client.submit(connection, target: target, operation: "send", payload: payload, id: newId)
        _ = try await client.submit(connection, target: target, operation: "send", payload: payload, id: pendingId)
        let posts = StubProtocol.requests.filter { $0.method == "POST" }
        XCTAssertEqual(posts.count, 3)
        XCTAssertEqual(try WorkspaceEnvelope.parse(XCTUnwrap(posts[0].json?["envelope"]?.object)).header.routeId, "preferred-route")
        XCTAssertEqual(try WorkspaceEnvelope.parse(XCTUnwrap(posts[1].json?["envelope"]?.object)).header.routeId, "online-route")
        XCTAssertEqual(posts[0].body, posts[2].body)
        for choice in 1...3 {
            let current = try journal.load(connection)
            var next = current; next.routes = [computer: preferredRoute]
            try journal.update(connection, expected: current, next: next)
            mode.withLock { $0 = choice }; _ = try await client.computers(connection)
            XCTAssertEqual(try journal.load(connection).routes[computer]?.routeId, choice == 3 ? "online-route" : "preferred-route")
        }
    }

    func testEncryptedHttpRetryAndReceiptRecoveryPreserveExactCiphertext() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let f = try XCTUnwrap(SharedFixture.json("encrypted-workspace-v1.json").object)
        let gateway = try WorkspaceIdentity.parse(XCTUnwrap(f["gateway_identity"]?.object)), phone = try WorkspaceIdentity.parse(XCTUnwrap(f["phone_identity"]?.object))
        let candidate = try WorkspaceCandidate.parse(XCTUnwrap(f["candidate"]?.object)), bootstrap = try WorkspaceBootstrap.parse(XCTUnwrap(f["bootstrap_snapshot"]?.object))
        let approval = try WorkspaceApproval.parse(XCTUnwrap(f["approval"]?.object)), roster = try WorkspaceMembership.parse(XCTUnwrap(f["final_state"]?.object))
        let journal = SQLiteWorkspaceJournal(directory: directory, testKeys: StaticKeyProvider(SymmetricKey(data: Data(repeating: 6, count: 32))))
        let connection = Connection(id: "connection-fixture", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
        let pending = WorkspaceState(identity: phone, candidate: candidate, issuer: gateway.device, pinnedRevision: 0, pinnedHead: bootstrap.state.head)
        try journal.create(connection, state: pending)
        var first = pending; first.membership = bootstrap.state; first.enrollment = approval
        try journal.update(connection, expected: pending, next: first)
        var active = first; active.membership = roster
        active.routes = ["machine-fixture": WorkspaceRoute(computerId: "machine-fixture", gatewayId: gateway.device.deviceId, routeId: "opaque-route", machineId: "machine-fixture")]
        try journal.update(connection, expected: first, next: active, changes: [approval.membership])
        let header = Mutex<WorkspaceHeader?>(nil), mode = Mutex(0)
        let history: JSONObject = ["changes": [], "membership_revision": .int(roster.revision), "membership_head": .string(roster.head), "next_after": .null]
        StubProtocol.reset { request in
            do {
                if request.url.path == "/v2/membership" { return .response(status: 200, body: try JSON.object(history).data()) }
                if request.method == "POST" {
                    XCTAssertEqual(request.url.path, "/v2/requests")
                    let wire = String(decoding: request.body, as: UTF8.self)
                    XCTAssertFalse(wire.contains("private message")); XCTAssertFalse(wire.contains("native-session"))
                    let envelope = try WorkspaceEnvelope.parse(request.json?["envelope"]?.object ?? JSONObject())
                    header.withLock { $0 = envelope.header }
                    let clear = try JSON.parse(envelope.open(identity: gateway, sender: phone.device, expected: envelope.header))
                    XCTAssertEqual(clear.string("session"), "native-session")
                    XCTAssertEqual(clear["payload"]?.string("text"), "private message")
                    XCTAssertEqual(clear["encrypted_transport"]?.string("machine_id"), "machine-fixture")
                    return StubProtocol.json(200, #"{"request_id":"request-one","state":"queued","envelope":null}"#)
                }
                if mode.withLock({ $0 }) == 0 { return StubProtocol.json(200, #"{"request_id":"request-one","state":"completed","envelope":null}"#) }
                let original = header.withLock { $0 }!
                let resultHeader = try WorkspaceHeader(workspaceId: roster.workspaceId, senderDeviceId: gateway.device.deviceId, recipientDeviceId: phone.device.deviceId,
                    senderKeyGeneration: 1, recipientKeyGeneration: 1, gatewayId: original.gatewayId,
                    routeId: mode.withLock({ $0 }) == 1 ? "substituted-route" : original.routeId, requestId: original.requestId, kind: "result", operationClass: original.operationClass,
                    expiresAt: original.expiresAt, membershipRevision: roster.revision, membershipHead: roster.head)
                let result = try WorkspaceEnvelope.seal(identity: gateway, recipient: phone.device, header: resultHeader,
                    plaintext: Data(#"{"request_id":"request-one","state":"completed","result":{"status":"submitted"},"error":null}"#.utf8))
                return .response(status: 200, body: try JSON.object(["request_id": "request-one", "state": "completed", "envelope": .object(result.json)]).data())
            } catch { return StubProtocol.json(500, #"{"error":"Synthetic endpoint rejected the envelope"}"#) }
        }
        let client = RelayClient(session: StubProtocol.client().session, workspaceJournal: journal)
        let target = Target(computerId: "machine-fixture", session: "native-session", run: "exact-run", conversation: "exact-conversation", connectionId: connection.id)
        let payload = try target.json().putting("text", "private message")
        _ = try await client.submit(connection, target: target, operation: "send", payload: payload, id: "request-one")
        let beforeRouteChange = try journal.load(connection)
        var afterRouteChange = beforeRouteChange
        afterRouteChange.routes = ["machine-fixture": WorkspaceRoute(computerId: "machine-fixture", gatewayId: gateway.device.deviceId, routeId: "changed-catalog-route", machineId: "machine-fixture")]
        try journal.update(connection, expected: beforeRouteChange, next: afterRouteChange)
        _ = try await client.submit(connection, target: target, operation: "send", payload: payload, id: "request-one")
        let posts = StubProtocol.requests.filter { $0.method == "POST" }
        XCTAssertEqual(posts.count, 2); XCTAssertEqual(posts[0].body, posts[1].body)
        let outerOnly = try await client.receipt(connection, id: "request-one")
        XCTAssertEqual(outerOnly.string("state"), "uncertain")
        mode.withLock { $0 = 1 }
        do { _ = try await client.receipt(connection, id: "request-one"); XCTFail("A different result route must fail") } catch {}
        mode.withLock { $0 = 2 }
        let completed = try await client.receipt(connection, id: "request-one")
        XCTAssertEqual(completed.string("state"), "completed")
        XCTAssertEqual(StubProtocol.requests.filter { $0.method == "POST" }.count, 2)
    }

    func testMissingProtectedStateRefusesBeforeNetwork() async throws {
        StubProtocol.reset { _ in StubProtocol.json(500, "{}") }
        let connection = Connection(id: "missing", label: "Fixture", url: "https://relay.example", token: "fixture-token", protocolVersion: 2)
        do {
            _ = try await StubProtocol.client().submit(connection, target: Target(computerId: "machine", session: "private-session", run: "run", conversation: "conversation"), operation: "send", payload: [:], id: "request-one")
            XCTFail("Protected state must never fall back to plaintext")
        } catch {}
        XCTAssertTrue(StubProtocol.requests.isEmpty)
    }
}
