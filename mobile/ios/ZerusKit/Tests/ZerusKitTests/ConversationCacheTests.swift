import CryptoKit
import Synchronization
import XCTest
@testable import ZerusKit

/// Ported from Android ConversationCacheTest, ConversationPageCacheTest, ConversationDisplayCacheTest and HistoryReadPathsTest.
final class ConversationCacheTests: XCTestCase {
    private var temporary: URL!
    private var root: URL { temporary.appendingPathComponent("cache") }
    private let key = StaticKeyProvider(SymmetricKey(size: .bits256))
    private final class TestClock: Sendable {
        let value = Mutex<Int64>(1_000_000)
    }
    private let clock = TestClock()
    private let target = Target(computerId: "computer", session: "session/name", run: "run", conversation: "conversation", connectionId: "workspace")

    override func setUp() {
        temporary = FileManager.default.temporaryDirectory.appendingPathComponent("zerus-history-cache-test-\(UUID().uuidString)")
    }
    override func tearDown() { try? FileManager.default.removeItem(at: temporary) }

    private func now() -> Int64 { clock.value.withLock { $0 } }
    private func advance(_ delta: Int64) { clock.value.withLock { $0 += delta } }
    private func engine(entries: Int = 20, bytes: Int64 = 20 * 1024 * 1024, payload: Int = 1024 * 1024, ttl: Int64 = 7 * 86400 * 1000,
                        keys: KeyProvider? = nil) -> ConversationFiles {
        let clock = self.clock
        return ConversationFiles(root: root, keys: keys ?? key, now: { clock.value.withLock { $0 } }, maxEntries: entries, maxBytes: bytes,
                                 maxPayloadBytes: payload, ttl: ttl)
    }
    private func raw(_ destination: Target? = nil) -> JSONObject {
        let destination = destination ?? target
        var raw: JSONObject = ["name": .string(destination.session), "run_id": .string(destination.run), "conversation_id": .string(destination.conversation)]
        if !destination.archiveId.isEmpty {
            raw["archive_id"] = .string(destination.archiveId)
            raw["state"] = "archived"
        }
        return raw
    }
    private func write(_ cache: ConversationFiles, _ destination: Target? = nil, _ text: String = "Synthetic private history",
                       identity: JSONObject? = nil, observedAt: Int64? = nil, generation: ConversationFiles.Generation? = nil) {
        let destination = destination ?? target
        cache.write(destination, identity: identity ?? raw(destination), payload: Data(text.utf8), observedAt: observedAt, generation: generation)
    }
    private func text(_ data: Data?) -> String? { data.map { String(decoding: $0, as: UTF8.self) } }
    private func files() -> [URL] { ((try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []).map { root.appendingPathComponent($0) } }

    func testSealedAtomicHistoryRestoresAfterRestart() throws {
        let cache = engine()
        write(cache)
        XCTAssertEqual("Synthetic private history", text(cache.memoryPayload(target)))
        let blob = try XCTUnwrap(files().first)
        XCTAssertEqual(1, files().count)
        XCTAssertFalse(String(decoding: try Data(contentsOf: blob), as: UTF8.self).contains("Synthetic private history"))
        let permissions = try FileManager.default.attributesOfItem(atPath: blob.path)[.posixPermissions] as? NSNumber
        XCTAssertEqual(0o600, permissions?.intValue)
        let reopened = engine()
        XCTAssertNil(reopened.memoryPayload(target))
        XCTAssertEqual("Synthetic private history", text(reopened.read(target)))
        XCTAssertEqual("Synthetic private history", text(reopened.memoryPayload(target)))
    }

    func testWorkspaceComputerNameRunConversationAndArchiveNeverAlias() {
        let cache = engine()
        write(cache)
        let archive = target.with(archiveId: "55555555-5555-4555-8555-555555555555")
        for other in [target.with(connectionId: "other"), target.with(computerId: "other"), target.with(session: "other"),
                      target.with(run: "other"), target.with(conversation: "other"), archive] {
            XCTAssertNil(cache.memoryPayload(other))
            XCTAssertNil(cache.read(other))
        }
        write(cache, archive, "Archive")
        XCTAssertNotEqual(text(cache.read(target)), text(cache.read(archive)))
    }

    func testMismatchedReturnedIdentityIsNeverAdmitted() {
        let cache = engine()
        var unnamed = raw()
        unnamed["name"] = nil
        for change in [("name", JSON.string("other")), ("run_id", "other"), ("conversation_id", "other"), ("archive_id", "other"), ("state", "archived")] {
            var identity = raw()
            identity[change.0] = change.1
            write(cache, identity: identity)
            XCTAssertNil(cache.memoryPayload(target))
        }
        write(cache, identity: unnamed)
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.path))
    }

    func testExpiredAndFutureDatedEntriesAreIgnored() {
        let cache = engine(ttl: 100)
        write(cache)
        advance(101)
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertNil(engine(ttl: 100).read(target))
        XCTAssertEqual(0, files().count)
        write(cache)
        advance(-1)
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertNil(engine(ttl: 100).read(target))
    }

    func testCorruptTruncatedWrongKeyAndFileSwapNeverReturnHistory() throws {
        let cache = engine()
        write(cache)
        let file = try XCTUnwrap(files().first)
        let sealed = try Data(contentsOf: file)
        try sealed.prefix(20).write(to: file)
        XCTAssertNil(engine().read(target))
        write(cache)
        var flipped = try Data(contentsOf: file)
        flipped[flipped.count - 1] ^= 1
        try flipped.write(to: file)
        XCTAssertNil(engine().read(target))
        write(cache)
        XCTAssertNil(engine(keys: StaticKeyProvider(SymmetricKey(size: .bits256))).read(target))
        write(cache)
        let second = target.with(run: "second")
        write(cache, second)
        let other = try XCTUnwrap(files().first { $0 != file })
        try Data(contentsOf: other).write(to: file)
        XCTAssertNil(engine().read(target))
    }

    func testEntryAndByteBudgetsEvictOldestConfirmedFilesAndBoundMemory() {
        var cache = engine(entries: 2)
        let second = target.with(run: "second"), third = target.with(run: "third")
        write(cache)
        advance(1)
        write(cache, second)
        advance(1)
        _ = engine(entries: 2).read(target)
        advance(1)
        write(cache, third)
        XCTAssertEqual(2, files().count)
        XCTAssertNil(engine(entries: 2).read(target))
        XCTAssertNil(cache.memoryPayload(target))
        cache.invalidate(nil)
        cache.clear()
        cache = engine(bytes: 1400)
        for index in 0..<8 {
            advance(1)
            write(cache, target.with(run: "run-\(index)"))
        }
        let sizes = files().compactMap { (try? FileManager.default.attributesOfItem(atPath: $0.path)[.size] as? NSNumber)?.int64Value }
        XCTAssertLessThanOrEqual(sizes.reduce(0, +), 1400)
        XCTAssertLessThan(files().count, 8)
    }

    func testPayloadLimitAndOverBudgetObjectCannotPublish() {
        var cache = engine(payload: 1024)
        write(cache, target, String(repeating: "λ", count: 600))
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.path))
        cache = engine(bytes: 100)
        write(cache)
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertEqual(0, files().count)
    }

    func testOutOfDateWritesCannotReplaceNewerConfirmedHistoryEvenAfterRestart() {
        let cache = engine()
        write(cache, target, "Newer", observedAt: now())
        advance(10)
        write(engine(), target, "Older", observedAt: now() - 11)
        XCTAssertEqual("Newer", text(engine().read(target)))
        write(cache, target, "Newer still", observedAt: now())
        write(cache, target, "Older", observedAt: now() - 1)
        XCTAssertEqual("Newer still", text(cache.memoryPayload(target)))
    }

    func testDisconnectPurgesOnlyOwnWorkspaceAndInvalidatesPendingWrites() {
        let cache = engine()
        let other = target.with(connectionId: "another")
        write(cache)
        write(cache, other)
        let pending = cache.generation(target.connectionId)
        cache.invalidate(target.connectionId)
        cache.purgeWorkspace(target.connectionId)
        write(cache, target, generation: pending)
        XCTAssertNil(cache.memoryPayload(target))
        XCTAssertNil(engine().read(target))
        XCTAssertNotNil(engine().read(other))
        let old = cache.generation(other.connectionId)
        cache.invalidate(nil)
        cache.clear()
        write(cache, other, generation: old)
        XCTAssertNil(cache.memoryPayload(other))
        XCTAssertEqual(0, files().count)
    }

    func testReadAfterInvalidateBeforeQueuedPurgeCannotRestoreHistory() {
        let cache = engine()
        write(cache)
        cache.invalidate(target.connectionId)
        XCTAssertNil(cache.read(target))
        XCTAssertNil(cache.memoryPayload(target))
        write(cache, target, "Late network callback")
        XCTAssertNil(cache.memoryPayload(target))
        cache.purgeWorkspace(target.connectionId)
        XCTAssertNil(engine().read(target))
        let global = engine()
        write(global, target.with(connectionId: "fresh"))
        global.invalidate(nil)
        XCTAssertNil(global.read(target.with(connectionId: "fresh")))
        global.clear()
        XCTAssertNil(engine().read(target.with(connectionId: "fresh")))
    }

    func testInvalidationAfterAtomicRenameCannotResurrectMemory() {
        for globally in [false, true] {
            let raced = engine()
            let connection = target.connectionId
            raced.afterRename.withLock { hook in
                hook = { [unowned raced] in
                    raced.afterRename.withLock { $0 = nil }
                    raced.invalidate(globally ? nil : connection)
                }
            }
            write(raced)
            XCTAssertNil(raced.memoryPayload(target))
            if globally { raced.clear() } else { raced.purgeWorkspace(connection) }
            XCTAssertNil(engine().read(target))
        }
    }

    func testSymlinkObjectIsRejectedAndCrashPartIsRemoved() throws {
        let cache = engine()
        write(cache)
        let file = try XCTUnwrap(files().first)
        let outside = temporary.appendingPathComponent("outside")
        try Data(contentsOf: file).write(to: outside)
        try FileManager.default.removeItem(at: file)
        try FileManager.default.createSymbolicLink(at: file, withDestinationURL: outside)
        XCTAssertNil(engine().read(target))
        XCTAssertTrue(FileManager.default.fileExists(atPath: outside.path))
        try Data("unfinished".utf8).write(to: root.appendingPathComponent(".synthetic.part"))
        _ = engine().read(target)
        XCTAssertEqual(0, files().count)
    }

    // MARK: Display snapshot

    private func displayRaw() -> JSONObject {
        ["name": "session/name", "run_id": "run", "conversation_id": "conversation", "events": [], "pending_questions": [["question_id": "kept"]]]
    }

    func testRetainedHistorySurvivesColdCacheThenTailRefresh() throws {
        let original = Event(id: "journal:1", role: "You", text: "Earlier message", time: "", at: 1)
        let encoded = try ConversationDisplayCache.encode(target, raw: displayRaw(), events: [original], truncated: false)
        let restored = try XCTUnwrap(ConversationDisplayCache.decode(encoded))
        let refreshed = RollingConversationHistory.merge(target: target, previousTarget: target, previous: restored.events,
                                                         incoming: [Event(id: "journal:180", role: "Agent", text: "New reply", time: "", at: 180)])
        XCTAssertEqual(["journal:1", "journal:180"], refreshed.events.map(\.id))
        XCTAssertEqual("kept", restored.raw.objects("pending_questions").first?.string("question_id"))
        XCTAssertFalse(restored.raw.has("events"))
    }

    func testDisplayBudgetDropsWholeOldestRowsAndReportsTruncation() throws {
        let rows = (1...100).map { Event(id: "event:\($0)", role: "You", text: String(repeating: "x", count: 20_000), time: "", at: Double($0)) }
        let encoded = try ConversationDisplayCache.encode(target, raw: displayRaw(), events: rows, truncated: false)
        XCTAssertLessThanOrEqual(encoded.count, 1_048_576)
        let restored = try XCTUnwrap(ConversationDisplayCache.decode(encoded))
        XCTAssertTrue(restored.truncated)
        XCTAssertEqual("event:100", restored.events.last?.id)
    }

    func testCorruptDisplaySnapshotIsAMiss() {
        XCTAssertNil(ConversationDisplayCache.decode(Data("{\"raw\":\"{}\",\"events\":[\"invalid\"],\"truncated\":false}".utf8)))
    }

    func testDisplayCacheRoundTripsThroughSealedFiles() async throws {
        let cache = ConversationCache(files: engine())
        await cache.write(target, raw: displayRaw(), events: [Event(id: "e", role: "Agent", text: "Hello", time: "")], truncated: false, observedAt: now())
        XCTAssertEqual(["e"], cache.memory(target)?.events.map(\.id))
        let reopened = ConversationCache(files: engine())
        let restored = await reopened.read(target)
        XCTAssertEqual("Hello", restored?.events.first?.text)
    }

    // MARK: Pages

    private let canonical = Event(id: "history:1", role: "AgentMessage", text: "reply", time: "", at: 1, historyId: "1", historyCursor: "cursor:1",
                                  historyEpoch: "epoch", incomingSeq: 1)
    private func savedPage(_ events: [Event]? = nil, epoch: String = "epoch") -> CachedHistoryPage {
        CachedHistoryPage(target: target, epoch: epoch, events: events ?? [canonical], before: "cursor:1", after: "cursor:1", hasBefore: false, hasAfter: false,
                          latestIncoming: nil, totalIncoming: nil, complete: false, indexing: false, truncated: true)
    }

    func testSealedPageRestartPreservesScopeAndKeepsTextPrivate() async throws {
        let pages = ConversationPageCache(files: engine(entries: 128, bytes: 64 * 1024 * 1024))
        var row = canonical
        row.text = "private page text"
        await pages.write(target, mode: "before", cursor: "opaque-cursor", page: savedPage([row]))
        let file = try XCTUnwrap(files().first)
        XCTAssertFalse(String(decoding: try Data(contentsOf: file), as: UTF8.self).contains("private page text"))
        let reopened = ConversationPageCache(files: engine(entries: 128, bytes: 64 * 1024 * 1024))
        let restored = try await reopened.read(target, mode: "before", cursor: "opaque-cursor")
        XCTAssertEqual("private page text", restored?.events.first?.text)
        let afterPage = try await reopened.read(target, mode: "after", cursor: "opaque-cursor")
        XCTAssertNil(afterPage)
        let otherWorkspace = try await reopened.read(target.with(connectionId: "other"), mode: "before", cursor: "opaque-cursor")
        XCTAssertNil(otherWorkspace)
        let archived = try await reopened.read(target.with(archiveId: "archive"), mode: "before", cursor: "opaque-cursor")
        XCTAssertNil(archived)
    }

    func testPreviouslyWrittenMixedPageIsAMissWithoutWeakeningStrictDecoder() async throws {
        var journal = canonical
        journal.id = "journal:fixture:AgentMessage:native:2"
        journal.historyId = ""
        journal.historyCursor = ""
        journal.historyEpoch = ""
        journal.incomingSeq = nil
        let mixed = savedPage([canonical, journal])
        XCTAssertThrowsError(try mixed.page(for: target))
        let missing = try await HistoryReadPaths.saved { try mixed.page(for: self.target) }
        XCTAssertNil(missing)
        let fresh = try await HistoryReadPaths.saved { try self.savedPage().page(for: self.target) }
        XCTAssertEqual([canonical], fresh?.events)
        XCTAssertEqual(false, fresh?.complete)
        XCTAssertNil(fresh?.latestIncoming)
    }

    func testForeignScopeAndCacheFailureAreSafeMisses() async throws {
        for other in [target.with(connectionId: "other"), target.with(run: "other")] {
            let page = try await HistoryReadPaths.saved { try self.savedPage().page(for: other) }
            XCTAssertNil(page)
        }
        let foreign = try await HistoryReadPaths.saved { try self.savedPage([self.canonical], epoch: "foreign epoch").page(for: self.target) }
        XCTAssertNil(foreign)
        let failed = try await HistoryReadPaths.saved { throw URLError(.cannotOpenFile) }
        XCTAssertNil(failed)
    }

    func testOfflineInvalidCachePreservesOriginalNetworkFailureAndDoesNotRetry() async {
        var attempts = 0
        do {
            _ = try await HistoryReadPaths.read(network: {
                attempts += 1
                throw URLError(.notConnectedToInternet)
            }, cached: { nil })
            XCTFail("An empty cache cannot satisfy this read")
        } catch let error as URLError { XCTAssertEqual(.notConnectedToInternet, error.code) }
        catch { XCTFail("Unexpected \(error)") }
        XCTAssertEqual(1, attempts)
    }

    func testResetIndexReopensVerifiedHeadInsteadOfReplayingOldEpoch() async throws {
        let head = HistoryPage(target: target, epoch: "new epoch", events: [], before: nil, after: nil, hasBefore: false, hasAfter: false,
                               latestIncoming: nil, totalIncoming: nil, complete: true, indexing: false, truncated: false)
        let stale = try savedPage().page(for: target)
        var probes = 0
        let failed = try await HistoryReadPaths.reopening(epoch: "epoch", read: { throw URLError(.badServerResponse) }) {
            probes += 1
            return head
        }
        XCTAssertTrue(failed.head)
        XCTAssertTrue(failed.result.verified)
        XCTAssertEqual("new epoch", failed.result.page.epoch)
        let cached = try await HistoryReadPaths.reopening(epoch: "epoch", read: { .init(page: stale, verified: false) }) {
            probes += 1
            return head
        }
        XCTAssertTrue(cached.head)
        XCTAssertEqual(2, probes)
    }

    func testSameEpochOfflineOrVerifiedReadsKeepTheirOriginalOutcome() async throws {
        let stale = try savedPage().page(for: target)
        var probes = 0
        let verified = try await HistoryReadPaths.reopening(epoch: "epoch", read: { .init(page: stale, verified: true) }) {
            probes += 1
            return stale
        }
        XCTAssertFalse(verified.head)
        XCTAssertEqual(0, probes)
        let cached = try await HistoryReadPaths.reopening(epoch: "epoch", read: { .init(page: stale, verified: false) }) { stale }
        XCTAssertFalse(cached.head)
        XCTAssertFalse(cached.result.verified)
        do {
            _ = try await HistoryReadPaths.reopening(epoch: nil, read: { throw URLError(.timedOut) }) {
                probes += 1
                return stale
            }
            XCTFail("A head read has no cursor epoch")
        } catch let error as URLError { XCTAssertEqual(.timedOut, error.code) }
        XCTAssertEqual(0, probes)
    }

    func testCancellationPropagatesInsteadOfBeingTreatedAsAMiss() async {
        do {
            _ = try await HistoryReadPaths.saved { throw CancellationError() }
            XCTFail("Cancellation must propagate")
        } catch is CancellationError {} catch { XCTFail("Unexpected \(error)") }
    }

    func testRetainedSliceUsesOnlyCanonicalRowsAroundTheAnchor() throws {
        let rows = (0..<250).map { index in
            Event(id: "history:\(index)", role: "Agent", text: "row", time: "", at: Double(index), historyId: "\(index)", historyCursor: "cursor:\(index)",
                  historyEpoch: "epoch", incomingSeq: Int64(index))
        }
        var journal = Event(id: "journal:x", role: "Agent", text: "live", time: "", at: 300)
        journal.historyEpoch = ""
        let window = HistoryWindow(target: target, epoch: "epoch", events: rows + [journal], before: "cursor:0", after: "cursor:249",
                                   hasBefore: false, hasAfter: false, atHead: true)
        let retained = try XCTUnwrap(CachedHistoryPage.retained(window, anchor: "cursor:120"))
        XCTAssertEqual(100, retained.events.count)
        XCTAssertEqual("history:71", retained.events.first?.id)
        XCTAssertTrue(retained.hasBefore)
        XCTAssertTrue(retained.hasAfter)
        let neighbors = CachedHistoryPage.neighbors(window, anchor: "cursor:120")
        XCTAssertEqual(["before", "after"], neighbors.map(\.direction))
        XCTAssertEqual("cursor:71", neighbors[0].cursor)
        XCTAssertEqual("cursor:170", neighbors[1].cursor)
        XCTAssertNil(CachedHistoryPage.retained(window, anchor: "missing"))
        XCTAssertNoThrow(try retained.page(for: target))
    }
}
