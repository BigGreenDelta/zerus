import XCTest
@testable import ZerusKit

/// Ported from Android ConversationReadHeadTest.
final class ConversationReadHeadTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let native = Event(id: "provider:reply", role: "AgentMessage", text: "Latest reply", time: "", at: 1, source: "codex_transcript",
                               nativeType: "AgentMessage")

    private func canonical(_ sequence: Int64 = 74) -> Event {
        var event = native
        event.id = "history:reply"
        event.historyId = "reply"
        event.historyCursor = "cursor:reply"
        event.historyEpoch = "epoch"
        event.incomingSeq = sequence
        event.originalId = native.id
        event.originalIds = [native.id]
        return event
    }

    private func page(_ rows: [Event], total: Int64 = 74, target: Target? = nil) -> HistoryPage {
        HistoryPage(target: target ?? self.target, epoch: "epoch", events: rows, before: nil, after: nil, hasBefore: false, hasAfter: false,
                    latestIncoming: total, totalIncoming: total, complete: true, indexing: false, truncated: false)
    }

    func testCanonicalHeadReconcilesTheExactNativeReplyAlreadyReadOnScreen() throws {
        let read = ConversationReadState().head(target, epoch: "epoch", latest: 63, total: 63, complete: true).visible(target, [native])
        XCTAssertEqual(ConversationReadPolicies.lowerBound(read.record(target), [native]), 63)
        let reconciled = ConversationReadHead.observe(read, page([canonical()]))
        XCTAssertEqual(reconciled.record(target)?.readThrough, 74)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(reconciled.record(target), [canonical()]), 0)
        let stored = try JSONDecoder().decode(ConversationReadState.self, from: JSONEncoder().encode(reconciled))
        XCTAssertEqual(stored, reconciled)
    }

    func testLaterRepliesAndUnseenHeadsRemainUnread() {
        let read = ConversationReadState().visible(target, [native])
        let reconciled = ConversationReadHead.observe(read, page([canonical()], total: 75))
        XCTAssertEqual(ConversationReadPolicies.lowerBound(reconciled.record(target), []), 1)
        XCTAssertEqual(ConversationReadPolicies.lowerBound(ConversationReadHead.observe(ConversationReadState(), page([canonical()])).record(target), []), 74)
    }

    func testIdenticalTextOrAnAmbiguousAliasNeverProvesReading() {
        let read = ConversationReadState().visible(target, [native])
        var unlinked = canonical()
        unlinked.originalId = ""
        unlinked.originalIds = []
        var other = canonical()
        other.id = "history:other"
        other.historyId = "other"
        other.historyCursor = "cursor:other"
        for rows in [[unlinked], [canonical(), other]] {
            XCTAssertEqual(ConversationReadHead.observe(read, page(rows)).record(target)?.readThrough, 0)
        }
    }

    func testForeignScopeOrNoncanonicalMetadataCannotTransferAReadPosition() {
        let read = ConversationReadState().visible(target, [native])
        let foreign = [target.with(connectionId: "other"), target.with(conversation: "other"),
                       target.with(agentId: "child", parentConversation: target.conversation)]
        for scope in foreign {
            XCTAssertEqual(ConversationReadHead.observe(read, page([canonical()], target: scope)).record(scope)?.readThrough, 0)
        }
        var otherEpoch = canonical()
        otherEpoch.historyEpoch = "other"
        var noCursor = canonical()
        noCursor.historyCursor = ""
        for row in [otherEpoch, noCursor] {
            XCTAssertEqual(ConversationReadHead.observe(read, page([row])).record(target)?.readThrough, 0)
        }
    }
}
