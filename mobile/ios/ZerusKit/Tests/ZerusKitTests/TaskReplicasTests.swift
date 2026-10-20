import Foundation
import XCTest
@testable import ZerusKit

final class TaskReplicasTests: XCTestCase {
    private func replicas() throws -> TaskReplicas { try TaskReplicas(root: URL(fileURLWithPath: FakeComputer.folder())) }

    /// A store call may wait seconds for the project's file lock: never on the main actor.
    @MainActor
    func testRunLeavesTheCallersThread() async throws {
        let replicas = try replicas()
        XCTAssertTrue(Thread.isMainThread)
        let onMain = try await replicas.run { Thread.isMainThread }
        XCTAssertFalse(onMain)
    }

    #if !os(Linux)
    func testReplicaFoldersAreExcludedFromBackup() throws {
        let replicas = try replicas()
        XCTAssertEqual(try replicas.root.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup, true)
    }
    #endif
}
