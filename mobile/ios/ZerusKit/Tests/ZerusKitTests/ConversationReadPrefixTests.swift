import XCTest
@testable import ZerusKit

/// The read watermark acknowledges only a proved prefix of incoming rows (docs/mobile-architecture.md,
/// "Read positions"): an exact native-provided alias proves that one row was seen, never the rows before it.
final class ConversationReadPrefixTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")

    private func native(_ sequence: Int64) -> Event {
        Event(id: "provider:\(sequence)", role: "AgentMessage", text: "Reply \(sequence)", time: "", at: Double(sequence), source: "codex_transcript",
              nativeType: "AgentMessage")
    }

    /// The canonical row for incoming sequence `sequence`; `aliased` links it to the native row of the same reply.
    private func canonical(_ sequence: Int64, aliased: Bool = false) -> Event {
        var event = native(sequence)
        event.id = "history:\(sequence)"
        event.historyId = "\(sequence)"
        event.historyCursor = "cursor:\(sequence)"
        event.historyEpoch = "epoch"
        event.incomingSeq = sequence
        if aliased {
            event.originalId = "provider:\(sequence)"
            event.originalIds = ["provider:\(sequence)"]
        }
        return event
    }

    private func page(_ rows: [Event], head: Int64) -> HistoryPage {
        HistoryPage(target: target, epoch: "epoch", events: rows, before: nil, after: nil, hasBefore: false, hasAfter: false,
                    latestIncoming: head, totalIncoming: head, complete: true, indexing: false, truncated: false)
    }

    /// Canonical rows through `readThrough` were read on screen; then only the native rows in `natives` were seen.
    private func state(readThrough: Int64, natives: [Int64]) -> ConversationReadState {
        ConversationReadState().head(target, epoch: "epoch", latest: readThrough, total: readThrough, complete: true)
            .visible(target, [canonical(readThrough)])
            .visible(target, natives.map(native))
    }

    private func unseenStayUnread(_ reconciled: ConversationReadState, _ rows: ClosedRange<Int64>, _ line: UInt = #line) {
        let record = reconciled.record(target)
        for sequence in rows { XCTAssertTrue(ConversationReadPolicies.unread(record, canonical(sequence)), "row \(sequence)", line: line) }
    }

    // MARK: Head observation

    func testAnAliasBeyondAGapKeepsTheUnseenRowsInsideItUnread() {
        let read = state(readThrough: 63, natives: [74])
        XCTAssertEqual(read.record(target)?.readThrough, 63)
        let rows = (64...73).map { canonical($0) } + [canonical(74, aliased: true)]
        let reconciled = ConversationReadHead.observe(read, page(rows, head: 74))
        XCTAssertEqual(reconciled.record(target)?.readThrough, 63)
        unseenStayUnread(reconciled, 64...73)
        // The exact alias is still known; it waits for the prefix before it.
        XCTAssertTrue(reconciled.record(target)?.seen.contains("history:74") == true)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(reconciled.record(target), rows), 11)
    }

    func testASingleRowHeadPageNeverProvesTheRangeBeforeIt() {
        let read = state(readThrough: 63, natives: [74])
        let reconciled = ConversationReadHead.observe(read, page([canonical(74, aliased: true)], head: 74))
        XCTAssertEqual(reconciled.record(target)?.readThrough, 63)
        XCTAssertTrue(reconciled.record(target)?.seen.contains("history:74") == true)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(reconciled.record(target), nil), 11)
    }

    func testAMissingSequenceStopsTheProvedPrefix() {
        // A merged-away row leaves a hole; nothing proves what it was.
        let read = state(readThrough: 63, natives: [64, 66])
        let reconciled = ConversationReadHead.observe(read, page([canonical(64, aliased: true), canonical(66, aliased: true)], head: 66))
        XCTAssertEqual(reconciled.record(target)?.readThrough, 64)
    }

    func testTheNextSequenceItselfIsAProvedPrefix() {
        // Sequences are unique and increase in timeline order within one epoch, so no row lies between 73 and 74.
        let read = state(readThrough: 73, natives: [74])
        XCTAssertEqual(ConversationReadHead.observe(read, page([canonical(74, aliased: true)], head: 74)).record(target)?.readThrough, 74)
        let run = state(readThrough: 63, natives: [64, 65])
        let rows = [canonical(64, aliased: true), canonical(65, aliased: true), canonical(66)]
        let reconciled = ConversationReadHead.observe(run, page(rows, head: 66))
        XCTAssertEqual(reconciled.record(target)?.readThrough, 65)
        unseenStayUnread(reconciled, 66...66)
    }

    // MARK: Watermark bounds

    func testAMaximumWatermarkWithAnEmptyPageNeverOverflows() {
        let read = ConversationReadState().head(target, epoch: "epoch", latest: .max, total: .max, complete: true).visible(target, [canonical(.max)])
        XCTAssertEqual(read.record(target)?.readThrough, .max)
        XCTAssertEqual(ConversationReadHead.observe(read, page([], head: .max)).record(target)?.readThrough, .max)
    }

    func testAdvancingToTheMaximumSequenceStopsThere() {
        let read = state(readThrough: .max - 1, natives: [.max])
        XCTAssertEqual(read.record(target)?.readThrough, .max - 1)
        XCTAssertEqual(ConversationReadHead.observe(read, page([canonical(.max, aliased: true)], head: .max)).record(target)?.readThrough, .max)
    }

    // MARK: The existing selected load path

    /// The selected conversation's history load as the app composes it today: exact aliases over the whole page
    /// supersede first, then the head observation. This pins the same gap on that pre-existing path.
    func testTheSelectedLoadPathKeepsTheUnseenRowsInsideAnAliasGapUnread() {
        let read = state(readThrough: 63, natives: [74])
        let rows = (64...73).map { canonical($0) } + [canonical(74, aliased: true)]
        let loaded = page(rows, head: 74)
        let known = Set(read.record(target)?.seen ?? [])
        let superseded = read.supersede(target, HistoryAliases.exact(loaded.events, known: known))
        let reconciled = ConversationReadHead.observe(superseded, loaded)
        XCTAssertEqual(reconciled.record(target)?.readThrough, 63)
        unseenStayUnread(reconciled, 64...73)
    }
}
