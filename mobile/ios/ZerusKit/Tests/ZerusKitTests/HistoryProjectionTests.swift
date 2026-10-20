import XCTest
@testable import ZerusKit

final class HistoryProjectionTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "claude/project", run: "run", conversation: "conversation", connectionId: "workspace")
    private let a = String(repeating: "a", count: 64), b = String(repeating: "b", count: 64), c = String(repeating: "c", count: 64)
    private func row(_ id: String, text: String = "old", at: Double = 1) -> JSONObject {
        ["history_id": .string(id), "history_cursor": .string("cursor:\(id)"), "history_stream": "provider", "type": "UserMessage",
         "source": "claude_native", "detail": .string(text), "at": .double(at), "original_ids": []]
    }
    private func event(_ id: String, text: String = "old", at: Double = 1) -> Event {
        Event(id: "history:\(id)", role: "You", text: text, time: "", at: at, source: "claude_native", nativeType: "UserMessage",
              historyId: id, historyCursor: "cursor:\(id)", historyEpoch: "epoch")
    }
    private func entry(_ generation: Int64, _ id: String, change: String = "retired", successor: JSONObject? = nil, resolved: Bool = true) -> JSONObject {
        ["generation": .int(generation), "history_id": .string(id), "change": .string(change), "resolution": .string(resolved ? "resolved" : "unresolved"),
         "successor": successor.map(JSON.object) ?? .null]
    }
    private func response(_ entries: [JSONObject], until: Int64? = nil, complete: Bool = true, next: Int64? = nil) -> JSONObject {
        let until = until ?? entries.last?.int64("generation") ?? 0
        return ["request_id": "request", "name": .string(target.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation),
                "history_epoch": "epoch", "projection_generation": .int(until), "projection_changes": .array(entries.map(JSON.object)),
                "complete": .bool(complete), "next_projection_after": .int(next ?? until)]
    }
    private func parse(_ raw: JSONObject, after: Int64 = 0, until: Int64? = nil) throws -> HistoryProjection.Page {
        try HistoryProjection.parse(target: target, id: "request", epoch: "epoch", after: after, until: until ?? raw.int64("projection_generation"), raw: raw)
    }
    private func window(_ events: [Event], marker: Int64 = 0) -> HistoryWindow {
        HistoryWindow(target: target, epoch: "epoch", events: events, before: events.first?.historyCursor, after: events.last?.historyCursor,
                      hasBefore: false, hasAfter: false, atHead: true, projectionApplied: marker)
    }
    private func apply(_ window: HistoryWindow, _ page: HistoryProjection.Page, start: Int64? = nil) -> HistoryWindow {
        HistoryProjection.apply(window, target: target, epoch: "epoch", start: start ?? window.projectionApplied,
                                entries: page.prefix, applied: page.applied(after: start ?? window.projectionApplied))!
    }

    func testThirtyThreeRefreshesApplyToLatestWindowAndKeepLaterArrival() throws {
        let ids = (1...33).map { String(repeating: "0", count: 62) + String(format: "%02x", $0) }
        let page = try parse(response(ids.enumerated().map { entry(Int64($0.offset + 1), $0.element, change: "refreshed", successor: row($0.element, text: "fixed \($0.offset)")) }))
        let latest = window(ids.map { event($0) } + [event(c, text: "arrived", at: 100)])
        let updated = apply(latest, page)
        XCTAssertEqual(updated.projectionApplied, 33)
        XCTAssertEqual(updated.events.map(\.text), (0...32).map { "fixed \($0)" } + ["arrived"])
    }
    func testUnresolvedEitherChangeStopsOnlyTheResolvedPrefix() throws {
        for change in ["retired", "refreshed"] {
            let page = try parse(response([entry(1, a), entry(2, b, change: change, resolved: false), entry(3, c)]))
            let fixed = apply(window([event(a), event(b), event(c)]), page)
            XCTAssertEqual(fixed.events.map(\.historyId), [b, c])
            XCTAssertEqual(fixed.projectionApplied, 1)
            XCTAssertEqual(page.prefix.count, 1)
        }
    }
    func testOlderAnswersNeverReplaceCorrectionsCoveredByANewerPage() throws {
        for change in ["retired", "refreshed"] {
            let stale = entry(1, a, change: change, successor: change == "refreshed" ? row(a, text: "stale") : nil)
            let page = try parse(response([stale, entry(3, b, change: "refreshed", successor: row(b, text: "new b"))]))
            let fixed = apply(window([event(a, text: "newer a"), event(b)], marker: 2), page, start: 0)
            XCTAssertEqual(fixed.events.map(\.text), ["newer a", "new b"])
            XCTAssertEqual(fixed.projectionApplied, 3)
        }
    }
    func testMalformedEntryAnywhereRejectsTheWholeEnvelope() {
        var reason = entry(2, b); reason["reason"] = "oversize"
        var float = entry(2, b); float["generation"] = .double(2)
        var extra = entry(2, b); extra["extra"] = true
        var cursor = row(b); cursor["history_cursor"] = ""
        let invalid = [entry(2, b, change: "refreshed"), entry(2, b, successor: row(b)), entry(2, b, change: "refreshed", successor: row(b), resolved: false),
                       reason, entry(1, b), float, extra, entry(2, "not-a-hash"), entry(2, b, change: "refreshed", successor: cursor)]
        for value in invalid { XCTAssertThrowsError(try parse(response([entry(1, a), value], until: 3))) }
    }
    func testIdentityCountIntegerAndEmptyIncompleteChecks() throws {
        let original = response([entry(1, a)], until: 2, complete: false, next: 1)
        let invalid: [(String, JSON)] = [("name", "other"), ("run_id", "other"), ("conversation_id", "other"), ("history_epoch", "other"),
                                             ("archive_id", .null), ("complete", "false"), ("next_projection_after", 0), ("projection_generation", 1)]
        for (field, value) in invalid {
            var bad = original; bad[field] = value
            XCTAssertThrowsError(try parse(bad, until: 2))
        }
        XCTAssertThrowsError(try parse(response([], until: 2, complete: false, next: 0)))
        XCTAssertThrowsError(try parse(response([entry(2, a)], until: 2, complete: false, next: 2)))
        XCTAssertThrowsError(try parse(response((1...257).map { entry(Int64($0), a) })))
        XCTAssertEqual(try parse(original).next, 1)
        XCTAssertEqual(try parse(response([], until: 5), after: 5).applied(after: 5), 5)
    }
    func testASCIIByteLimitCountsUnicodeAndEnvelopeOverhead() throws {
        let fixture = try XCTUnwrap(SharedFixture.json("history-projection-wire.json").object)
        for test in fixture.objects("cases") {
            XCTAssertEqual(try HistoryProjection.asciiBytes(test["value"]!), Int(test.int64("bytes")))
        }
        let unicode = response([entry(1, a, change: "refreshed", successor: row(a, text: String(repeating: "я", count: 151_000)))])
        XCTAssertLessThan(JSON.object(unicode).compact.utf8.count, HistoryProjection.maxBytes)
        XCTAssertThrowsError(try parse(unicode))
        let large = response([entry(1, a, change: "refreshed", successor: row(a, text: String(repeating: "x", count: HistoryProjection.maxBytes)))])
        XCTAssertThrowsError(try parse(large))
        var unresolved = entry(1, a, change: "refreshed", resolved: false); unresolved["reason"] = "oversize"
        XCTAssertEqual(try parse(response([unresolved])).applied(after: 0), 0)
        var exact = response([entry(1, a)]); exact["padding"] = ""
        let base = try HistoryProjection.asciiBytes(.object(exact))
        exact["padding"] = .string(String(repeating: "x", count: HistoryProjection.maxBytes - base))
        XCTAssertEqual(try HistoryProjection.asciiBytes(.object(exact)), HistoryProjection.maxBytes)
        XCTAssertTrue(try parse(exact).complete)
        exact["padding"] = .string(exact.string("padding") + "x")
        XCTAssertThrowsError(try parse(exact))
    }
    func testAliasesPreserveReceiptBodyAndNeverImportUnseenRanges() throws {
        var receipt = event(c, text: "receipt body"); receipt.requestId = "receipt"; receipt.source = "hgs_delivery"
        receipt.attachments = [DisplayedAttachment(name: "photo", mime: "image/png", bytes: 7)]
        var successor = row(b, text: "receipt body", at: 2)
        successor["source"] = "hgs_delivery"; successor["message_id"] = "receipt"; successor["submitted_text"] = "send"
        successor["attachments"] = [["name": "photo", "mime": "image/png", "bytes": 7]]
        successor["original_ids"] = [.string("history:\(a)"), .string("history:\(c)")]
        let fixed = apply(window([event(a), event(b, text: "older successor"), receipt]), try parse(response([entry(1, a, change: "refreshed", successor: successor)])))
        XCTAssertEqual(fixed.events.map(\.historyId), [b])
        XCTAssertEqual(fixed.events.first?.requestId, "receipt")
        XCTAssertEqual(fixed.events.first?.attachments.first?.name, "photo")
        let absent = apply(window([event(c)]), try parse(response([entry(1, a, change: "refreshed", successor: row(b))])))
        XCTAssertEqual(absent.events.map(\.historyId), [c])
    }
    func testProtectedRowsAndTargetEpochNeverChange() throws {
        let page = try parse(response([entry(1, a)]))
        var receipt = event(a); receipt.requestId = "receipt"
        var outbox = event(a); outbox.outgoingId = "outbox"
        var voice = event(a); voice.source = "hgs_voice"
        var reply = event(a); reply.role = "AgentMessage"
        var foreign = event(a); foreign.historyEpoch = "other"
        for value in [receipt, outbox, voice, reply, foreign] { XCTAssertEqual(apply(window([value]), page).events, [value]) }
        XCTAssertNil(HistoryProjection.apply(window([event(a)]), target: target, epoch: "other", start: 0, entries: page.prefix, applied: 1))
        var other = target; other.run = "other"
        XCTAssertNil(HistoryProjection.apply(window([event(a)]), target: other, epoch: "epoch", start: 0, entries: page.prefix, applied: 1))
    }
    func testOldSliceLowersCoverageAndRetirementIsIdempotent() throws {
        let page = try parse(response([entry(1, a)])), fixed = apply(window([event(a)]), try parse(response([entry(1, a)])))
        let old = HistoryPage(target: target, epoch: "epoch", events: [event(a)], before: "old", after: "old", hasBefore: false, hasAfter: false,
                              latestIncoming: nil, totalIncoming: nil, complete: false, indexing: false, truncated: true)
        let merged = fixed.merge(old, direction: "before")
        XCTAssertEqual(merged.projectionApplied, 0)
        XCTAssertTrue(apply(merged, page).events.isEmpty)
        let later = try parse(response([entry(4, a)]), after: 3)
        XCTAssertEqual(apply(window([event(a)]), later, start: 3).projectionApplied, 0)
    }
    func testCacheMarkerIsAtomicAndOldPagesDecodeToZero() throws {
        let retained = CachedHistoryPage.retained(window([event(a)], marker: 8), anchor: "cursor:\(a)")!
        XCTAssertEqual(try retained.page(for: target).projectionApplied, 8)
        let encoded = try JSON.parse(JSONEncoder().encode(retained)).object!
        var old = encoded; old["projectionApplied"] = nil
        let legacy = try JSONDecoder().decode(CachedHistoryPage.self, from: JSON.object(old).data())
        XCTAssertEqual(try legacy.page(for: target).projectionApplied, 0)
        var bad = retained; bad.projectionApplied = -1
        XCTAssertThrowsError(try bad.page(for: target))
    }
    func testFourPageBudgetSnapshotsUntilAndResumesWithoutRepeatingPrefix() throws {
        var run = HistoryProjection.Run(start: 0, until: 6), requested: [Int64] = []
        while run.canRead {
            requested.append(run.after)
            run.accepted(try parse(response([entry(run.after + 1, a)], until: 6, complete: false, next: run.after + 1), after: run.after))
        }
        XCTAssertEqual(requested, [0, 1, 2, 3]); XCTAssertEqual(run.after, 4); XCTAssertEqual(run.until, 6)
        var resumed = HistoryProjection.Run(start: run.after, until: 6)
        var tail = response([entry(5, a), entry(6, b)], until: 6); tail["projection_generation"] = 8
        resumed.accepted(try parse(tail, after: 4, until: 6))
        XCTAssertFalse(resumed.canRead); XCTAssertEqual(resumed.after, 6)
        XCTAssertFalse(HistoryProjection.supported(relay: ["history"], native: ["history_projection"]))
        XCTAssertFalse(HistoryProjection.supported(relay: ["history_projection"], native: nil))
        XCTAssertTrue(HistoryProjection.supported(relay: ["history_projection"], native: ["history_projection"]))
    }

    func testRelayPinsArchiveChildAndMakesOneRequest() async throws {
        var child = target
        child.agentId = "worker"; child.parentConversation = "conversation"; child.conversation = "conversation/worker"
        child.archiveId = "10000000-0000-4000-8000-000000000001"
        let captured = child
        StubProtocol.reset { request in
            let id = request.json?.string("request_id") ?? ""
            let result: JSONObject = ["request_id": .string(id), "name": .string(captured.session), "run_id": .string(captured.run),
                                      "conversation_id": .string(captured.conversation), "agent_id": .string(captured.agentId),
                                      "parent_conversation_id": .string(captured.parentConversation), "archive_id": .string(captured.archiveId),
                                      "history_epoch": "epoch", "projection_generation": 1, "projection_changes": [], "complete": true, "next_projection_after": 1]
            return StubProtocol.json(200, JSON.object(["request_id": .string(id), "state": "completed", "result": .object(result)]).compact)
        }
        let connection = Connection(id: target.connectionId, label: "Test", url: "https://gateway.example", token: "synthetic-token")
        let page = try await StubProtocol.client().historyProjection(connection, target: child, epoch: "epoch", after: 0, until: 1)
        XCTAssertEqual(page.applied(after: 0), 1); XCTAssertEqual(StubProtocol.requests.count, 1)
        let sent = try XCTUnwrap(StubProtocol.requests.first?.json?.object), payload = try XCTUnwrap(sent.object("payload"))
        XCTAssertEqual(sent.string("operation"), "history_projection"); XCTAssertEqual(sent.string("computer_id"), target.computerId)
        XCTAssertEqual(payload.string("expected_conversation_id"), "conversation"); XCTAssertEqual(payload.string("agent_id"), "worker")
        XCTAssertEqual(payload.string("archive_id"), child.archiveId); XCTAssertNil(payload["limit"])
    }
    func testRefreshGrowthKeepsTheByteBoundAndOlderNavigation() throws {
        let rows = (1...20).map { event(String(repeating: "0", count: 62) + String(format: "%02x", $0), text: String(repeating: "x", count: 100_000), at: Double($0)) }
        let first = rows[0].historyId
        let fixed = apply(window(rows), try parse(response([entry(1, first, change: "refreshed", successor: row(first, text: String(repeating: "x", count: 500_000)))])))
        XCTAssertTrue(fixed.hasBefore); XCTAssertEqual(fixed.before, fixed.events.first?.historyCursor)
        XCTAssertLessThanOrEqual(fixed.events.reduce(2) { $0 + $1.approximateBytes + 1 }, 2 * 1024 * 1024)
    }
}
