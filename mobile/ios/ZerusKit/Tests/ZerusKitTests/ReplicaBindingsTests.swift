import Foundation
import XCTest
import ZerusReplica

/// The Rust task replica through its Swift bindings: a store files a task and
/// another replica of the project catches up through one exchange.
final class ReplicaBindingsTests: XCTestCase {
    private func folder() -> String { FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).path }

    func testAStoreFilesATaskAndAnswersAnExchange() throws {
        let computer = TaskStore(root: folder(), project: "project-1", projectName: "orchard")
        let phone = TaskStore(root: folder(), project: "project-1", projectName: "orchard")
        let task = try computer.add(text: "Add rate limiting", stage: .todo, target: .anySession, position: .end)
        XCTAssertEqual(try computer.list().tasks.count, 1)
        XCTAssertNotNil(task.id.range(of: "^T-[a-z0-9]{6}$", options: .regularExpression), task.id)
        let digest = try computer.liveDigest()
        XCTAssertNotNil(digest.range(of: "^[0-9a-f]{64}$", options: .regularExpression), digest)
        let exchange = phone.exchange()
        while let request = try exchange.nextRequest() {
            _ = try exchange.accept(answer: try computer.answerExchange(request: request))
        }
        XCTAssertEqual(try phone.liveDigest(), digest)
        XCTAssertEqual(try phone.get(reference: task.uuid).text, "Add rate limiting")
    }
}
