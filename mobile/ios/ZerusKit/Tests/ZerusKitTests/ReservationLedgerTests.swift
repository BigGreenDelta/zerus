import Foundation
import Synchronization
import XCTest
@testable import ZerusKit

/// An atomic in-memory store that counts every call.
private final class MemoryLedgerStore: RevisionedItemStore {
    private let item = Mutex<RevisionedItem?>(nil)
    let reads = Mutex(0)
    let writes = Mutex(0)

    init(_ data: Data? = nil) { item.withLock { $0 = data.map { RevisionedItem(data: $0, revision: "r0") } } }

    var current: RevisionedItem? { item.withLock { $0 } }

    func read(_ account: String) throws -> RevisionedItem? {
        reads.withLock { $0 += 1 }
        return item.withLock { $0 }
    }

    func create(_ account: String, _ data: Data, revision: String) throws -> Bool {
        item.withLock { item in
            guard item == nil else { return false }
            item = RevisionedItem(data: data, revision: revision)
            writes.withLock { $0 += 1 }
            return true
        }
    }

    func replace(_ account: String, _ data: Data, expected: String, revision: String) throws -> Bool {
        item.withLock { item in
            guard item?.revision == expected else { return false }
            item = RevisionedItem(data: data, revision: revision)
            writes.withLock { $0 += 1 }
            return true
        }
    }
}

final class ReservationLedgerTests: XCTestCase {
    private typealias Announcement = ReservationLedger.Announcement

    private static func fingerprint(_ seed: String) -> String { NotificationPolicy.fingerprint(seed) }

    private static func seen(_ seed: String, _ event: Int64, slot: String = "slot", workspace: String = "workspace",
                             kind: ReservationLedger.Kind = .input, by: ReservationLedger.Writer = .extension) -> ReservationLedger.Observation {
        ReservationLedger.Observation(workspace: workspace, slot: slot, fingerprint: fingerprint(seed), kind: kind, eventId: event, by: by)
    }

    private static func next(_ result: Announcement) -> ReservationLedger? {
        switch result {
        case .fresh(let ledger), .observed(let ledger): ledger
        default: nil
        }
    }

    private static func isFresh(_ result: Announcement) -> Bool { if case .fresh = result { true } else { false } }
    private static func isObserved(_ result: Announcement) -> Bool { if case .observed = result { true } else { false } }

    private static func results(_ outcome: ReservationOutcome<[Announcement]>) -> [Announcement]? {
        switch outcome {
        case .committed(let results), .unchanged(let results): results
        case .failedClosed: nil
        }
    }

    private static func ledger(_ store: MemoryLedgerStore) throws -> ReservationLedger { try ReservationLedger.decode(store.current?.data) }

    private static func text(_ rows: [String]) -> Data { Data(#"{"schema":1,"proofs":[\#(rows.joined(separator: ","))]}"#.utf8) }

    private static func row(workspace: String = "\"workspace\"", slot: String = "\"slot\"", announced: String? = nil, kind: String = "\"input\"",
                            announcedEvent: String = "1", observedEvent: String = "1", by: String = "\"app\"", extra: String = "") -> String {
        let fingerprint = announced ?? "\"\(fingerprint("question-1"))\""
        return #"{"workspace":\#(workspace),"slot":\#(slot),"announced":\#(fingerprint),"kind":\#(kind),"announced_event_id":\#(announcedEvent),"observed_event_id":\#(observedEvent),"by":\#(by)\#(extra)}"#
    }

    // MARK: Codec

    func testAMissingLedgerIsEmptyAndRoundTripsCanonically() throws {
        XCTAssertEqual(try ReservationLedger.decode(nil), ReservationLedger())
        var ledger = ReservationLedger()
        for (index, kind) in ReservationLedger.Kind.allCases.enumerated() {
            ledger = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q\(index)", Int64(index + 1), slot: "slot-\(index)", kind: kind))))
        }
        ledger = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q0", 9, slot: "slot-0"))))
        let data = try ledger.encoded()
        XCTAssertEqual(try ReservationLedger.decode(data), ledger)
        XCTAssertEqual(try ReservationLedger.decode(data).encoded(), data)
        XCTAssertEqual(ledger.proofs.first?.announcedEventId, 1)
        XCTAssertEqual(ledger.proofs.first?.observedEventId, 9)
        XCTAssertEqual(try ReservationLedger.decode(Self.text([Self.row(observedEvent: "5")])).proofs.first?.by, .app)
    }

    func testUnknownMissingExtraOrMistypedFieldsFailClosed() {
        let invalid: [Data] = [
            Data("not json".utf8),
            Data(#"{"schema":2,"proofs":[]}"#.utf8),
            Data(#"{"schema":1}"#.utf8),
            Data(#"{"schema":1,"proofs":[],"covered_through":{}}"#.utf8),
            Data(#"{"schema":"1","proofs":[]}"#.utf8),
            Data(#"{"schema":1.0,"proofs":[]}"#.utf8),
            Data(#"{"schema":1,"proofs":{}}"#.utf8),
            Self.text(["1"]),
            Self.text([Self.row(extra: #","note":"x""#)]),
            Self.text([#"{"workspace":"workspace","slot":"slot","announced":"\#(Self.fingerprint("q"))","kind":"input","event_id":1,"by":"app"}"#]),
            Self.text([Self.row(announcedEvent: "\"1\"")]),
            Self.text([Self.row(observedEvent: "1.5")]),
            Self.text([Self.row(announcedEvent: "0", observedEvent: "0")]),
            Self.text([Self.row(announcedEvent: "-1")]),
            Self.text([Self.row(announcedEvent: "5", observedEvent: "4")]),
            Self.text([Self.row(kind: "\"finished\"")]),
            Self.text([Self.row(kind: "\"optional\"")]),
            Self.text([Self.row(by: "\"relay\"")]),
            Self.text([Self.row(announced: "\"\(Self.fingerprint("q").uppercased())\"")]),
            Self.text([Self.row(announced: "\"abc\"")]),
            Self.text([Self.row(workspace: "\"\"")]),
            Self.text([Self.row(slot: "\"line\\nbreak\"")]),
            Self.text([Self.row(), Self.row(observedEvent: "2")]),
        ]
        for data in invalid { XCTAssertThrowsError(try ReservationLedger.decode(data), String(decoding: data, as: UTF8.self)) }
    }

    func testTheGlobalBoundsHoldOnDecodeAndEncode() throws {
        let rows = (0...ReservationLedger.maxProofs).map { Self.row(slot: "\"slot-\($0)\"") }
        XCTAssertThrowsError(try ReservationLedger.decode(Self.text(rows)))
        XCTAssertNoThrow(try ReservationLedger.decode(Self.text(Array(rows.prefix(ReservationLedger.maxProofs)))))
        XCTAssertThrowsError(try ReservationLedger.decode(Data(repeating: 0x20, count: NotificationReservations.maxBytes + 1)))
        // Thirty-two maximal slots exceed 16 KiB: such a ledger is never written.
        var ledger = ReservationLedger()
        for index in 0..<ReservationLedger.maxProofs {
            ledger = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q\(index)", 1, slot: String(repeating: "s", count: 4000) + "\(index)"))))
        }
        XCTAssertThrowsError(try ledger.encoded())
    }

    // MARK: Fence

    func testALaterSameFingerprintRaisesTheFenceSoAnOlderChangedIdentityIsStale() throws {
        let first = try XCTUnwrap(Self.next(ReservationLedger().observing(Self.seen("F", 2))))
        // F again at event 100 announces nothing but fences everything up to 100.
        let fenced = first.observing(Self.seen("F", 100))
        XCTAssertTrue(Self.isObserved(fenced))
        let ledger = try XCTUnwrap(Self.next(fenced))
        XCTAssertEqual(ledger.proofs.first?.announced, Self.fingerprint("F"))
        XCTAssertEqual(ledger.proofs.first?.announcedEventId, 2)
        XCTAssertEqual(ledger.proofs.first?.observedEventId, 100)
        XCTAssertEqual(ledger.observing(Self.seen("G", 50)), .stale)
        XCTAssertEqual(ledger.observing(Self.seen("G", 100)), .stale)
        // Equal or older observations of F change nothing and never lower the fence.
        XCTAssertEqual(ledger.observing(Self.seen("F", 100)), .alreadyAnnounced)
        XCTAssertEqual(ledger.observing(Self.seen("F", 3)), .alreadyAnnounced)
        // A changed identity from a newer event is fresh and moves both events.
        let changed = try XCTUnwrap(Self.next(ledger.observing(Self.seen("G", 101))))
        XCTAssertTrue(Self.isFresh(ledger.observing(Self.seen("G", 101))))
        XCTAssertEqual(changed.proofs.first?.announced, Self.fingerprint("G"))
        XCTAssertEqual(changed.proofs.first?.announcedEventId, 101)
        XCTAssertEqual(changed.proofs.first?.observedEventId, 101)
        // G seen again later raises the fence but is never announced twice.
        XCTAssertTrue(Self.isObserved(changed.observing(Self.seen("G", 102))))
        // The same slot in another workspace is a different scope.
        XCTAssertTrue(Self.isFresh(changed.observing(Self.seen("G", 1, workspace: "other"))))
    }

    func testAFullLedgerNeverEvictsButAnExistingSlotCanStillChange() throws {
        var ledger = ReservationLedger()
        for index in 0..<ReservationLedger.maxProofs {
            ledger = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q\(index)", 1, slot: "slot-\(index)"))))
        }
        XCTAssertEqual(ledger.observing(Self.seen("q", 9, slot: "slot-new")), .full)
        let changed = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q-new", 2, slot: "slot-0"))))
        XCTAssertEqual(changed.proofs.count, ReservationLedger.maxProofs)
        XCTAssertEqual(changed.proofs.map(\.slot), ledger.proofs.map(\.slot))
    }

    func testAMalformedObservationIsNeverRecorded() {
        var short = Self.seen("q", 1)
        short.fingerprint = "short"
        XCTAssertEqual(ReservationLedger().observing(short), .invalid)
        XCTAssertEqual(ReservationLedger().observing(Self.seen("q", 0)), .invalid)
        XCTAssertEqual(ReservationLedger().observing(Self.seen("q", 1, slot: "")), .invalid)
    }

    // MARK: Coordinator

    func testTheFenceHoldsWithinOneBatch() async throws {
        let store = MemoryLedgerStore()
        let coordinator = NotificationReservationCoordinator(store: store)
        _ = await coordinator.observe([Self.seen("F", 2)])
        let outcome = await coordinator.observe([Self.seen("F", 100), Self.seen("G", 50)])
        guard case .committed(let results) = outcome else { return XCTFail("The raised fence must be stored: \(outcome)") }
        XCTAssertTrue(Self.isObserved(results[0]))
        XCTAssertEqual(results[1], .stale)
        XCTAssertEqual(try Self.ledger(store).proofs.first?.observedEventId, 100)
        XCTAssertEqual(try Self.ledger(store).proofs.first?.announced, Self.fingerprint("F"))
    }

    func testTheFenceHoldsAcrossCalls() async throws {
        let store = MemoryLedgerStore()
        let coordinator = NotificationReservationCoordinator(store: store)
        guard case .committed(let first) = await coordinator.observe([Self.seen("F", 2)]), Self.isFresh(first[0]) else { return XCTFail("F is fresh") }
        guard case .committed(let second) = await coordinator.observe([Self.seen("F", 100)]), Self.isObserved(second[0]) else {
            return XCTFail("The fence must be committed")
        }
        guard case .unchanged([.stale]) = await coordinator.observe([Self.seen("G", 50)]) else { return XCTFail("G at 50 must be stale") }
        guard case .unchanged([.alreadyAnnounced]) = await coordinator.observe([Self.seen("F", 60)]) else { return XCTFail("F at 60 changes nothing") }
        XCTAssertEqual(store.writes.withLock { $0 }, 2)
        guard case .committed(let changed) = await coordinator.observe([Self.seen("G", 101)]), Self.isFresh(changed[0]) else {
            return XCTFail("G at 101 is fresh")
        }
    }

    func testAnotherProcessRaisingTheFenceMakesAStaleWriterDecideAgain() async throws {
        let store = MemoryLedgerStore()
        let other = NotificationReservationCoordinator(store: store)
        _ = await other.observe([Self.seen("F", 2)])
        var decisions = 0
        // This process decides G at event 50 from the snapshot after F2; meanwhile another process commits F at 100.
        let outcome = NotificationReservations.reserve(store) { data in
            decisions += 1
            if decisions == 1 {
                let raised = NotificationReservations.reserve(store) { try NotificationReservationCoordinator.decide($0, [Self.seen("F", 100)]) }
                XCTAssertEqual(Self.results(raised).map { $0.map(Self.isObserved) }, [true])
            }
            return try NotificationReservationCoordinator.decide(data, [Self.seen("G", 50)])
        }
        guard case .unchanged([.stale]) = outcome else { return XCTFail("The stale writer must find G fenced: \(outcome)") }
        XCTAssertEqual(decisions, 2)
        XCTAssertEqual(try Self.ledger(store).proofs.first?.announced, Self.fingerprint("F"))
        XCTAssertEqual(try Self.ledger(store).proofs.first?.observedEventId, 100)
    }

    func testConcurrentCallersInOneProcessReserveTheSameNeedOnce() async throws {
        let store = MemoryLedgerStore()
        let coordinator = NotificationReservationCoordinator(store: store)
        let observation = Self.seen("F", 7)
        let outcomes = await withTaskGroup(of: ReservationOutcome<[Announcement]>.self) { group in
            for _ in 0..<10 { group.addTask { await coordinator.observe([observation]) } }
            var collected: [ReservationOutcome<[Announcement]>] = []
            for await outcome in group { collected.append(outcome) }
            return collected
        }
        let fresh = outcomes.filter { if case .committed(let results) = $0, Self.isFresh(results[0]) { true } else { false } }
        let repeated = outcomes.filter { if case .unchanged([.alreadyAnnounced]) = $0 { true } else { false } }
        XCTAssertEqual(fresh.count, 1)
        XCTAssertEqual(repeated.count, 9)
        XCTAssertEqual(store.writes.withLock { $0 }, 1)
    }

    func testAnOverlongBatchFailsClosedWithoutTouchingTheStore() async {
        let store = MemoryLedgerStore()
        let coordinator = NotificationReservationCoordinator(store: store)
        let identical = Array(repeating: Self.seen("F", 1), count: NotificationReservationCoordinator.maxBatch + 1)
        guard case .failedClosed(.rejected) = await coordinator.observe(identical) else { return XCTFail("33 observations must be refused") }
        XCTAssertEqual(store.reads.withLock { $0 }, 0)
        XCTAssertEqual(store.writes.withLock { $0 }, 0)
        let bounded = Self.results(await coordinator.observe(Array(identical.prefix(NotificationReservationCoordinator.maxBatch))))
        XCTAssertEqual(bounded?.count, NotificationReservationCoordinator.maxBatch)
    }

    func testAnUnknownLedgerFailsClosedAndAFullOneWritesNothing() async throws {
        let unknown = Data(#"{"schema":2,"proofs":[]}"#.utf8)
        let store = MemoryLedgerStore(unknown)
        guard case .failedClosed(.rejected) = await NotificationReservationCoordinator(store: store).observe([Self.seen("F", 1)]) else {
            return XCTFail("An unknown ledger must fail closed")
        }
        XCTAssertEqual(store.current, RevisionedItem(data: unknown, revision: "r0"))
        var ledger = ReservationLedger()
        for index in 0..<ReservationLedger.maxProofs {
            ledger = try XCTUnwrap(Self.next(ledger.observing(Self.seen("q\(index)", 1, slot: "slot-\(index)"))))
        }
        let encoded = try ledger.encoded()
        let full = MemoryLedgerStore(encoded)
        guard case .unchanged([.full]) = await NotificationReservationCoordinator(store: full).observe([Self.seen("q", 1, slot: "slot-new")]) else {
            return XCTFail("A full ledger must announce nothing")
        }
        XCTAssertEqual(full.writes.withLock { $0 }, 0)
        XCTAssertEqual(full.current, RevisionedItem(data: encoded, revision: "r0"))
    }
}
