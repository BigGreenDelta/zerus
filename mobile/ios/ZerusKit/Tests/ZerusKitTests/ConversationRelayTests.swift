import XCTest
@testable import ZerusKit

/// Inspect, history and shared-read requests, ported from Android RelayApi checks.
final class ConversationRelayTests: XCTestCase {
    private let connection = Connection(id: "gateway", label: "Test", url: "https://gateway.example", token: "synthetic-token")
    private let target = Target(computerId: "machine", session: "codex/example/tag", run: "run", conversation: "conversation", connectionId: "gateway")

    /// Answers each submitted request with a completed receipt carrying `result`.
    private func complete(_ result: @escaping @Sendable (JSONObject) -> JSON) {
        StubProtocol.reset { request in
            let body = request.json?.object ?? [:]
            let receipt: JSON = ["request_id": .string(body.string("request_id")), "state": "completed", "result": result(body)]
            return StubProtocol.json(200, receipt.compact)
        }
    }

    func testInspectSendsExactChildIdentityAndRejectsAnotherChild() async throws {
        var child = target
        child.agentId = "worker"
        child.parentConversation = "conversation"
        child.conversation = "conversation/worker"
        complete { _ in ["name": "codex/example/tag", "run_id": "run", "agent_id": "worker", "parent_conversation_id": "conversation"] }
        let raw = try await StubProtocol.client().inspect(connection, target: child)
        XCTAssertEqual("worker", raw.string("agent_id"))
        let payload = StubProtocol.requests[0].json?["payload"]?.object
        XCTAssertEqual("worker", payload?.string("agent_id"))
        XCTAssertEqual("run", payload?.string("expected_run_id"))
        XCTAssertEqual("conversation", payload?.string("expected_conversation_id"))
        XCTAssertEqual("inspect", StubProtocol.requests[0].json?.string("operation"))

        complete { _ in ["name": "codex/example/tag", "run_id": "run", "agent_id": "other", "parent_conversation_id": "conversation"] }
        do {
            _ = try await StubProtocol.client().inspect(connection, target: child)
            XCTFail("Another child must not be shown")
        } catch let error as ZerusError { XCTAssertEqual(error.message, "Computer returned a different child conversation.") }
    }

    func testInspectRejectsArchivedOrForeignConversationForLiveTarget() async throws {
        complete { _ in ["name": "codex/example/tag", "run_id": "run", "state": "archived"] }
        do {
            _ = try await StubProtocol.client().inspect(connection, target: target)
            XCTFail("An archived conversation is not the live target")
        } catch let error as ZerusError { XCTAssertEqual(error.message, "Computer returned a different conversation for this session.") }
        complete { _ in ["name": "codex/other/tag", "run_id": "run"] }
        do {
            _ = try await StubProtocol.client().inspect(connection, target: target)
            XCTFail("Another session must not be shown")
        } catch is ZerusError {}
        complete { _ in ["agent_id": "child"] }
        do {
            _ = try await StubProtocol.client().inspect(connection, target: target)
            XCTFail("A child conversation must not replace its parent")
        } catch let error as ZerusError { XCTAssertEqual(error.message, "Computer returned a child conversation for the parent.") }
    }

    func testArchivedInspectCarriesItsArchiveIdentity() async throws {
        var archived = target
        archived.archiveId = "11111111-1111-4111-8111-111111111111"
        complete { _ in ["name": "codex/example/tag", "archive_id": "11111111-1111-4111-8111-111111111111", "state": "archived"] }
        _ = try await StubProtocol.client().inspect(connection, target: archived)
        XCTAssertEqual(archived.archiveId, StubProtocol.requests[0].json?["payload"]?.string("archive_id"))
    }

    func testFailedInspectReportsTheComputerError() async {
        StubProtocol.reset { request in
            let id = request.json?.string("request_id") ?? ""
            return StubProtocol.json(200, #"{"request_id":"\#(id)","state":"failed","error":"Native session is not running."}"#)
        }
        do {
            _ = try await StubProtocol.client().inspect(connection, target: target)
            XCTFail("A failed receipt is not activity")
        } catch let error as ZerusError { XCTAssertEqual(error.message, "Native session is not running.") }
        catch { XCTFail("Unexpected \(error)") }
    }

    func testHistoryParsesTheExactRequestedPage() async throws {
        complete { body in
            ["request_id": .string(body.string("request_id")), "name": "codex/example/tag", "run_id": "run", "conversation_id": "conversation",
             "history_epoch": "epoch", "head": ["complete": true, "incoming_seq": 2, "total_incoming": 2],
             "events": [["id": "e1", "role": "Agent", "text": "Hello", "history_id": "h1", "history_cursor": "c1", "history_epoch": "epoch", "incoming_seq": 2]],
             "has_more_before": false, "has_more_after": false]
        }
        let page = try await StubProtocol.client().history(connection, target: target, direction: "before", cursor: "c9")
        XCTAssertEqual("epoch", page.epoch)
        XCTAssertEqual(true, page.complete)
        let payload = StubProtocol.requests[0].json?["payload"]?.object
        XCTAssertEqual("c9", payload?.string("before"))
        XCTAssertEqual("run", payload?.string("expected_run_id"))
    }

    func testHistoryRequiresAStartedConversationAndMatchingCursor() async {
        for (direction, cursor, conversation) in [("", "", ""), ("before", "", "conversation"), ("", "cursor", "conversation"), ("sideways", "c", "conversation")] {
            do {
                _ = try await StubProtocol.client().history(connection, target: target.with(conversation: conversation), direction: direction, cursor: cursor)
                XCTFail("Invalid history request \(direction) \(cursor)")
            } catch {}
        }
    }

    func testMarkReadBelongsToTheLiveParent() async throws {
        complete { _ in ["status": "ok"] }
        let receipt = try await StubProtocol.client().markRead(connection, target: target, payload: ["read_through": 3])
        XCTAssertEqual("completed", receipt.string("state"))
        XCTAssertEqual("mark_read", StubProtocol.requests[0].json?.string("operation"))
        var child = target
        child.agentId = "worker"
        do {
            _ = try await StubProtocol.client().markRead(connection, target: child, payload: [:])
            XCTFail("Children have no shared read marks")
        } catch {}
    }
}
