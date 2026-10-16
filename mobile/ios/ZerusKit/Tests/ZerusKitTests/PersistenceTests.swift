import CryptoKit
import Foundation
import Synchronization
import XCTest
@testable import ZerusKit

final class SealedFileStoreTests: XCTestCase {
    private struct Sample: Codable, Equatable { var text: String; var count: Int }
    private var directory: URL!

    override func setUpWithError() throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("zerus-sealed-\(UUID().uuidString)")
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(at: directory) }

    private func store(_ key: SymmetricKey = SymmetricKey(size: .bits256)) -> SealedFileStore {
        SealedFileStore(directory: directory, keys: StaticKeyProvider(key))
    }

    func testRoundTripAndMissingFile() throws {
        let store = store()
        XCTAssertNil(try store.read("state", as: Sample.self))
        try store.write("state", Sample(text: "draft ✓", count: 3))
        XCTAssertEqual(try store.read("state", as: Sample.self), Sample(text: "draft ✓", count: 3))
        try store.remove("state")
        XCTAssertNil(try store.read("state", as: Sample.self))
    }

    func testCiphertextDoesNotContainPlaintext() throws {
        let store = store()
        try store.write("state", Sample(text: "very private draft", count: 1))
        let raw = try Data(contentsOf: directory.appendingPathComponent("state.sealed"))
        XCTAssertNil(raw.range(of: Data("very private draft".utf8)))
    }

    func testTamperedCiphertextAndWrongKeyAreRejected() throws {
        let key = SymmetricKey(size: .bits256)
        try store(key).write("state", Sample(text: "x", count: 1))
        let file = directory.appendingPathComponent("state.sealed")
        var raw = try Data(contentsOf: file)
        raw[raw.count - 1] ^= 0x01
        try raw.write(to: file)
        XCTAssertThrowsError(try store(key).read("state", as: Sample.self))
        try store(key).write("state", Sample(text: "x", count: 1))
        XCTAssertThrowsError(try store().read("state", as: Sample.self))
    }

    func testSealedFileCannotBeReplayedUnderAnotherName() throws {
        let key = SymmetricKey(size: .bits256)
        try store(key).write("drafts", Sample(text: "a", count: 1))
        try FileManager.default.copyItem(at: directory.appendingPathComponent("drafts.sealed"), to: directory.appendingPathComponent("outbox.sealed"))
        XCTAssertThrowsError(try store(key).read("outbox", as: Sample.self))
    }

    func testDirectoryIsExcludedFromBackup() throws {
        try store().write("state", Sample(text: "x", count: 1))
        XCTAssertEqual(try directory.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup, true)
    }

    func testInvalidNamesAreRejected() {
        for name in ["", "../escape", "a/b", ".hidden"] {
            XCTAssertThrowsError(try store().write(name, Sample(text: "x", count: 1)), name)
        }
    }
}

@MainActor
final class OrderedStatePersistenceTests: XCTestCase {
    private final class Sink: Sendable {
        let values = Mutex<[Int]>([])
        let failing = Mutex(false)
        func write(_ value: Int) throws {
            if failing.withLock({ $0 }) { throw ZerusError.invalid("disk full") }
            values.withLock { $0.append(value) }
        }
        var written: [Int] { values.withLock { $0 } }
    }

    private func persistence(_ sink: Sink, initial: Int = 0) -> OrderedStatePersistence<Int> {
        OrderedStatePersistence(initial: initial, debounce: .milliseconds(100), maxDelay: .milliseconds(400)) { try sink.write($0) }
    }

    func testEditsPublishImmediatelyAndCoalesceIntoOneWrite() async throws {
        let sink = Sink()
        let store = persistence(sink)
        store.edit { $0 + 1 }
        store.edit { $0 + 1 }
        XCTAssertEqual(store.edit { $0 + 1 }, 3)
        XCTAssertEqual(store.state, 3)
        XCTAssertEqual(sink.written, [])
        try await Task.sleep(for: .milliseconds(350))
        XCTAssertEqual(sink.written, [3])
        store.close()
    }

    func testContinuousEditingIsWrittenByTheMaximumDelay() async throws {
        let sink = Sink()
        let store = persistence(sink)
        for _ in 0..<10 {
            store.edit { $0 + 1 }
            try await Task.sleep(for: .milliseconds(60))
        }
        XCTAssertFalse(sink.written.isEmpty, "A write must happen within the maximum delay while edits continue")
        store.close()
    }

    func testDurableWriteCompletesBeforeReturningAndAcknowledges() async throws {
        let sink = Sink()
        let store = persistence(sink, initial: 5)
        store.edit { $0 + 1 }
        let written = try await store.durable({ $0 * 10 }, acknowledge: { current, written in written + 1 })
        XCTAssertEqual(written, 60)
        XCTAssertEqual(sink.written.last, 60)
        XCTAssertEqual(store.state, 61)
        store.close()
    }

    func testFailedDurableWriteThrowsAndKeepsState() async throws {
        let sink = Sink()
        sink.failing.withLock { $0 = true }
        let store = persistence(sink, initial: 1)
        var reported: Error?
        store.onResult = { reported = $0 }
        do {
            _ = try await store.durable { $0 + 1 }
            XCTFail("A failed durable write must not release transport")
        } catch {}
        XCTAssertEqual(store.state, 1)
        XCTAssertNotNil(reported)
        store.close()
    }

    func testThrowingDurableReducerChangesNothing() async throws {
        let sink = Sink()
        let store = persistence(sink, initial: 1)
        do {
            _ = try await store.durable { _ in throw ZerusError.invalid("precondition") }
            XCTFail("Expected the reducer error")
        } catch let error as ZerusError { XCTAssertEqual(error.message, "precondition") }
        XCTAssertEqual(store.state, 1)
        XCTAssertEqual(sink.written, [])
        store.close()
    }

    func testFlushWritesTheLatestEdit() async throws {
        let sink = Sink()
        let store = persistence(sink)
        store.edit { $0 + 7 }
        let flushed = try await store.flush()
        XCTAssertEqual(flushed, 7)
        XCTAssertEqual(sink.written, [7])
        store.close()
    }
}

final class ConnectionStoreTests: XCTestCase {
    func testInMemoryStoreRoundTripsConnections() throws {
        let store = InMemoryConnectionStore()
        XCTAssertEqual(try store.load(), [])
        let connection = Connection(id: "id", label: "Lab", url: "https://gateway.example", token: "secret")
        try store.save([connection])
        XCTAssertEqual(try store.load(), [connection])
    }
}
