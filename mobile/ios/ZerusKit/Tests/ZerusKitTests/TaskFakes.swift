import Foundation
import Synchronization
import ZerusReplica
@testable import ZerusKit

/// A computer behind the stub relay, as Android's tests run one behind MockWebServer: it answers
/// `task_exchange` from its own replica and the other task operations from scripted receipts.
final class FakeComputer: Sendable {
    typealias Receipt = @Sendable (JSONObject) -> (state: String, result: JSON, error: JSON)
    struct State: Sendable {
        var envelopes: [JSONObject] = []
        var paths: [String] = []
        /// A failed exchange receipt with this error.
        var refusal: String?
        /// Operations whose receipts stay queued, as a computer that has not claimed them yet.
        var queued: Set<String> = []
        var delay: Duration = .zero
        var receipts: [String: Receipt] = [:]
        /// Whole replies for an operation instead of its receipt: a lost answer, a relay refusal.
        var raw: [String: @Sendable () -> StubProtocol.Reply] = [:]
    }

    let store: TaskStore
    let state = Mutex(State())

    init(folder: String = FakeComputer.folder()) {
        store = TaskStore(root: folder, project: "project-1", projectName: "orchard")
        StubProtocol.reset { [self] recorded in self.reply(recorded) }
    }

    static func folder() -> String { FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).path }

    var envelopes: [JSONObject] { state.withLock { $0.envelopes } }
    var operations: [String] { envelopes.map { $0.string("operation") } }
    func update(_ change: (inout State) -> Void) { state.withLock { change(&$0) } }

    private func reply(_ recorded: StubProtocol.Recorded) -> StubProtocol.Reply {
        guard let envelope = recorded.json?.object else { return StubProtocol.json(400, #"{"error":"no envelope"}"#) }
        let operation = envelope.string("operation")
        let (delay, refusal, receipt, raw, queued) = state.withLock { state in
            state.envelopes.append(envelope)
            state.paths.append(recorded.url.path)
            return (state.delay, state.refusal, state.receipts[operation], state.raw[operation], state.queued.contains(operation))
        }
        if delay > .zero { Thread.sleep(forTimeInterval: Double(delay.components.seconds) + Double(delay.components.attoseconds) / 1e18) }
        if let raw { return raw() }
        let payload = envelope.object("payload") ?? JSONObject()
        let outcome: (state: String, result: JSON, error: JSON)
        if queued {
            outcome = ("queued", .null, .null)
        } else if operation == "task_exchange" {
            if let refusal {
                outcome = ("failed", .null, .string(refusal))
            } else {
                do {
                    let answer = try store.answerExchange(request: JSON.object(payload).compact)
                    outcome = ("completed", try JSON.parse(answer), .null)
                } catch {
                    outcome = ("failed", .null, .string("\(error)"))
                }
            }
        } else if let receipt {
            outcome = receipt(payload)
        } else {
            outcome = ("failed", .null, .string("no receipt for \(operation)"))
        }
        let body: JSON = ["request_id": .string(envelope.string("request_id")), "state": .string(outcome.state),
                          "result": outcome.result, "error": outcome.error]
        return StubProtocol.json(200, body.compact)
    }
}

extension TaskStore {
    @discardableResult
    func filed(_ text: String, stage: TaskStage = .todo, target: TaskTarget = .anySession) throws -> TaskInfo {
        try add(text: text, stage: stage, target: target, position: .end)
    }
}
