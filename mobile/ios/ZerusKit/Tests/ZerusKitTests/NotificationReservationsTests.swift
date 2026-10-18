import Foundation
import Synchronization
import XCTest
@testable import ZerusKit

/// A scripted store whose calls are atomic; `failing` models a locked keychain, and `commitThenThrow` a native write
/// that stores its data and then reports an error, so its caller cannot know the outcome.
private final class ScriptedStore: RevisionedItemStore {
    private let item = Mutex<RevisionedItem?>(nil)
    let failing = Mutex(false)
    let commitThenThrow = Mutex(false)
    let writes = Mutex(0)

    private func unknownOutcome() throws {
        if commitThenThrow.withLock({ flag in defer { flag = false }; return flag }) { throw ZerusError.invalid("errSecIO") }
    }

    init(_ initial: RevisionedItem? = nil) { item.withLock { $0 = initial } }

    var current: RevisionedItem? { item.withLock { $0 } }

    func read(_ account: String) throws -> RevisionedItem? {
        if failing.withLock({ $0 }) { throw ZerusError.invalid("errSecInteractionNotAllowed") }
        return item.withLock { $0 }
    }

    func create(_ account: String, _ data: Data, revision: String) throws -> Bool {
        if failing.withLock({ $0 }) { throw ZerusError.invalid("errSecInteractionNotAllowed") }
        let created = item.withLock { item in
            guard item == nil else { return false }
            item = RevisionedItem(data: data, revision: revision)
            writes.withLock { $0 += 1 }
            return true
        }
        if created { try unknownOutcome() }
        return created
    }

    func replace(_ account: String, _ data: Data, expected: String, revision: String) throws -> Bool {
        if failing.withLock({ $0 }) { throw ZerusError.invalid("errSecInteractionNotAllowed") }
        let replaced = item.withLock { item in
            guard item?.revision == expected else { return false }
            item = RevisionedItem(data: data, revision: revision)
            writes.withLock { $0 += 1 }
            return true
        }
        if replaced { try unknownOutcome() }
        return replaced
    }
}

private final class Calls: Sendable {
    private let value = Mutex(0)
    var count: Int { value.withLock { $0 } }
    func add() { value.withLock { $0 += 1 } }
}

final class NotificationReservationsTests: XCTestCase {
    /// A toy ledger: the reserved keys, one per line. A key is reserved at most once.
    private static func keys(_ data: Data?) throws -> [String] {
        guard let data else { return [] }
        guard let text = String(data: data, encoding: .utf8), text.hasPrefix("v1\n") else { throw ZerusError.invalid("unknown ledger") }
        return text.dropFirst(3).split(separator: "\n").map(String.init)
    }

    /// Always "v1\n" followed by the keys, so an empty ledger is "v1\n" and decodes to no keys.
    private static func encode(_ keys: [String]) -> Data { Data(("v1\n" + keys.joined(separator: "\n")).utf8) }

    /// Reserves `key` unless the snapshot already holds it; the result says whether this writer reserved it. The n-th
    /// decision first runs `during[n]`: another process acting after this writer's snapshot and before its write.
    private static func reserve(_ store: ScriptedStore, _ key: String, calls: Calls? = nil, during: [() -> Void] = []) -> ReservationOutcome<Bool> {
        var pending = during
        return NotificationReservations.reserve(store) { data in
            calls?.add()
            if !pending.isEmpty { pending.removeFirst()() }
            let existing = try keys(data)
            if existing.contains(key) { return (nil, false) }
            return (encode(existing + [key]), true)
        }
    }

    private static func reserved(_ outcome: ReservationOutcome<Bool>) -> Bool? {
        switch outcome {
        case .committed(let value): value
        case .unchanged(let value): value
        case .failedClosed: nil
        }
    }

    private static func failure(_ outcome: ReservationOutcome<Bool>) -> ReservationFailure? {
        if case .failedClosed(let reason) = outcome { reason } else { nil }
    }

    func testAPausedWriterCannotCommitOverANewerReservation() throws {
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        let calls = Calls()
        // A has read r0 and pauses for any length of time; meanwhile B reserves the same need and posts it.
        let a = Self.reserve(store, "need", calls: calls, during: [{ XCTAssertEqual(true, Self.reserved(Self.reserve(store, "need"))) }])
        // A's write over r0 is refused; deciding again from B's ledger, it finds the need already reserved.
        guard case .unchanged(false) = a else { return XCTFail("The paused writer must not reserve again: \(a)") }
        XCTAssertEqual(calls.count, 2)
        XCTAssertEqual(try Self.keys(store.current?.data), ["need"])
        XCTAssertEqual(store.writes.withLock { $0 }, 1)
    }

    func testDifferentNeedsFromBothWritersAreBothKept() throws {
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        guard case .committed(true) = Self.reserve(store, "from-app", during: [{ _ = Self.reserve(store, "from-extension") }]) else { return XCTFail("The app's need must be reserved") }
        XCTAssertEqual(try Self.keys(store.current?.data), ["from-extension", "from-app"])
    }

    func testFirstUseHasExactlyOneCreator() throws {
        let store = ScriptedStore()
        let second = Self.reserve(store, "need", during: [{ XCTAssertEqual(true, Self.reserved(Self.reserve(store, "need"))) }])
        guard case .unchanged(false) = second else { return XCTFail("The second creator must find the need reserved: \(second)") }
        XCTAssertEqual(store.writes.withLock { $0 }, 1)
        XCTAssertEqual(try Self.keys(store.current?.data), ["need"])
    }

    func testContentionIsRetriedAtMostThreeTimesThenFailsClosed() {
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        let calls = Calls()
        // Another writer wins before every one of A's writes.
        let others: [() -> Void] = (0..<NotificationReservations.attempts).map { index in { _ = Self.reserve(store, "other-\(index)") } }
        XCTAssertEqual(Self.failure(Self.reserve(store, "need", calls: calls, during: others)), .contended)
        XCTAssertEqual(calls.count, NotificationReservations.attempts)
        XCTAssertFalse((try? Self.keys(store.current?.data))?.contains("need") ?? true)
    }

    func testAnUnreadableOrOversizedLedgerFailsClosedAndIsNeverReset() {
        for stored in [Data("not a ledger".utf8), Data("v2\nneed".utf8)] {
            let store = ScriptedStore(RevisionedItem(data: stored, revision: "r0"))
            XCTAssertEqual(Self.failure(Self.reserve(store, "need")), .rejected)
            XCTAssertEqual(store.current, RevisionedItem(data: stored, revision: "r0"))
        }
        let large = ScriptedStore(RevisionedItem(data: Data(repeating: 0x41, count: NotificationReservations.maxBytes + 1), revision: "r0"))
        let calls = Calls()
        XCTAssertEqual(Self.failure(Self.reserve(large, "need", calls: calls)), .oversized)
        XCTAssertEqual(calls.count, 0)
        XCTAssertEqual(large.current?.revision, "r0")
        // A decision beyond the bound is never written either.
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        let grown = NotificationReservations.reserve(store) { _ in (Data(repeating: 0x41, count: NotificationReservations.maxBytes + 1), true) }
        XCTAssertEqual(Self.failure(grown), .oversized)
        XCTAssertEqual(store.current?.revision, "r0")
    }

    func testALockedStoreFailsClosedWithoutPartialState() {
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        store.failing.withLock { $0 = true }
        XCTAssertEqual(Self.failure(Self.reserve(store, "need")), .unavailable)
        store.failing.withLock { $0 = false }
        // The store locks between the snapshot and the write.
        XCTAssertEqual(Self.failure(Self.reserve(store, "need", during: [{ store.failing.withLock { $0 = true } }])), .unavailable)
        XCTAssertEqual(store.current?.revision, "r0")
    }

    func testAWriteWithAnUnknownOutcomeIsNeverReplayedOrAnnounced() throws {
        for initial in [RevisionedItem(data: Self.encode([]), revision: "r0"), nil] {
            let store = ScriptedStore(initial)
            let calls = Calls()
            store.commitThenThrow.withLock { $0 = true }
            // The write stored the reservation and then failed: this call reserves nothing it may announce.
            XCTAssertEqual(Self.failure(Self.reserve(store, "need", calls: calls)), .unavailable)
            XCTAssertEqual(calls.count, 1)
            XCTAssertEqual(store.writes.withLock { $0 }, 1)
            // The stored reservation is kept; the next independent call re-reads it and does not reserve again.
            XCTAssertEqual(try Self.keys(store.current?.data), ["need"])
            guard case .unchanged(false) = Self.reserve(store, "need") else { return XCTFail("The stored reservation must deduplicate") }
            XCTAssertEqual(store.writes.withLock { $0 }, 1)
        }
    }

    func testNothingToReserveWritesNothing() {
        let store = ScriptedStore(RevisionedItem(data: Self.encode(["need"]), revision: "r0"))
        guard case .unchanged(false) = Self.reserve(store, "need") else { return XCTFail("An already reserved need is unchanged") }
        XCTAssertEqual(store.writes.withLock { $0 }, 0)
        XCTAssertEqual(store.current?.revision, "r0")
    }

    func testEveryCommitInstallsAFreshRevision() {
        let store = ScriptedStore(RevisionedItem(data: Self.encode([]), revision: "r0"))
        _ = Self.reserve(store, "first")
        let first = store.current?.revision
        _ = Self.reserve(store, "second")
        let second = store.current?.revision
        XCTAssertNotEqual(first, "r0")
        XCTAssertNotEqual(first, second)
        // A revision source that repeats the stored revision would allow an undetectable overwrite; it fails closed.
        let repeated = NotificationReservations.reserve(store, revision: { second ?? "" }) { _ in (Self.encode(["third"]), true) }
        XCTAssertEqual(Self.failure(repeated), .rejected)
        XCTAssertEqual(store.current?.revision, second)
    }
}
