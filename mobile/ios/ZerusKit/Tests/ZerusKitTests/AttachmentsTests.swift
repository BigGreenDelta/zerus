import CryptoKit
import Synchronization
import XCTest
@testable import ZerusKit

private func sha256(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

/// Ported from Android ComputerFileReferenceTest and ComputerFileProtocolTest.
final class ComputerFileTests: XCTestCase {
    func testUnicodeSpreadsheetAndEncodedNamesStayOnTheConversationComputer() {
        let path = "/work/example/documents/Сводная_2026-03_2026-09.xlsx"
        XCTAssertEqual(ComputerFileReference(path: path), ComputerFileReference.parse(path))
        XCTAssertEqual("Сводная_2026-03_2026-09.xlsx", ComputerFileReference.parse(path)?.name)
        XCTAssertEqual(ComputerFileReference(path: "/work/example/report final.xlsx"), ComputerFileReference.parse("file:///work/example/report%20final.xlsx"))
        XCTAssertEqual(ComputerFileReference(path: "/work/example/a%20b.xlsx"), ComputerFileReference.parse("/work/example/a%2520b.xlsx"))
        XCTAssertEqual(ComputerFileReference(path: "/work/example/a+b.xlsx"), ComputerFileReference.parse("/work/example/a+b.xlsx"))
        XCTAssertEqual(ComputerFileReference(path: "/work/example/report final.xlsx"), ComputerFileReference.parse("/work/example/report final.xlsx"))
    }

    func testRelativeAndHomePathsAreLeftForNativeSessionResolution() {
        for path in ["docs/report.xlsx", "./report.xlsx", "../report.xlsx", "~/reports/report.xlsx"] {
            XCTAssertEqual(ComputerFileReference(path: path), ComputerFileReference.parse(path), path)
        }
        XCTAssertEqual(ComputerFileReference(path: "src/example.rs", location: ":12:3"), ComputerFileReference.parse("src/example.rs:12:3"))
        XCTAssertEqual(ComputerFileReference(path: "/work/example/a.md", location: "#L12"), ComputerFileReference.parse("/work/example/a.md#L12"))
    }

    func testWebAppAndNetworkDestinationsAreNeverComputerFiles() {
        for value in ["", "#section", "https://example.com/a.xlsx", "http://example.com/a", "//example.com/a", "file://example.com/a.xlsx",
                      "content://documents/file", "javascript:alert(1)", "mailto:user@example.com", "sandbox:/mnt/data/report.xlsx",
                      "C:\\work\\report.xlsx", "file:///C:/work/report.xlsx?query", "/work/a?query", "/work/%00.xlsx", "/work/%0a.xlsx",
                      "/work/%5c.xlsx", "/work/%zz", "/work/a\n.xlsx"] {
            XCTAssertNil(ComputerFileReference.parse(value), value)
        }
    }

    private let target = Target(computerId: "computer-a", session: "codex/project/session", run: "run", conversation: "conversation", connectionId: "gateway")
    private let reference = ComputerFileReference(path: "/work/example/Сводная.xlsx")
    private let id = UUID().uuidString.lowercased()
    private func result() -> JSONObject {
        ["request_id": .string(id), "name": .string(target.session), "run_id": .string(target.run), "conversation_id": .string(target.conversation),
         "requested_path": .string(reference.path), "path": .string(reference.path), "file_name": .string(reference.name),
         "mime": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "bytes": 3, "sha256": .string(String(repeating: "a", count: 64)),
         "offset": 0, "length": 3, "data_base64": .string(Data([0, 255, 80]).base64EncodedString())]
    }

    func testExactComputerConversationAndUnicodePathArePinned() throws {
        let payload = try ComputerFileProtocol.payload(target, reference, requestId: id, offset: 0, length: ComputerFileProtocol.chunkBytes, first: nil)
        XCTAssertEqual(reference.path, payload.string("path"))
        let file = try ComputerFileProtocol.chunk(result(), requestId: id, target: target, reference: reference, offset: 0, length: ComputerFileProtocol.chunkBytes, first: nil)
        XCTAssertEqual(Data([0, 255, 80]), file.data)
        var other = target
        other.computerId = "other"
        XCTAssertNotEqual(ComputerFileProtocol.cacheKey(target, file), ComputerFileProtocol.cacheKey(other, file))
        XCTAssertThrowsError(try ComputerFileProtocol.payload(target, reference, requestId: id, offset: 10, length: 1, first: nil))
        XCTAssertThrowsError(try ComputerFileProtocol.payload(target, reference, requestId: "request", offset: 0, length: 1, first: nil))
    }

    func testMismatchedIdentitiesVersionsAndMalformedRangesAreRejected() throws {
        let first = try ComputerFileProtocol.chunk(result(), requestId: id, target: target, reference: reference, offset: 0, length: ComputerFileProtocol.chunkBytes, first: nil)
        let changes: [(String, JSON)] = [("request_id", "other"), ("name", "other"), ("run_id", "other"), ("conversation_id", "other"),
                                         ("requested_path", "/work/other.xlsx"), ("path", "relative.xlsx"), ("file_name", "other.xlsx"), ("mime", "bad type"),
                                         ("bytes", 0), ("bytes", 3.5), ("offset", 1), ("length", 2), ("data_base64", "not base64")]
        for (field, value) in changes {
            XCTAssertThrowsError(try ComputerFileProtocol.chunk(result().putting(field, value), requestId: id, target: target, reference: reference, offset: 0,
                                                                length: ComputerFileProtocol.chunkBytes, first: nil), field) { XCTAssertTrue($0 is AttachmentOpenError) }
        }
        XCTAssertThrowsError(try ComputerFileProtocol.chunk(result().putting("sha256", .string(String(repeating: "b", count: 64))), requestId: id, target: target,
                                                            reference: reference, offset: 0, length: ComputerFileProtocol.chunkBytes, first: first))
    }
}

/// Ported from Android AttachmentViewTest and ComputerFileDownloadsTest.
final class AttachmentViewTests: XCTestCase {
    private var temporary: URL!
    private final class TestClock: Sendable { let value = Mutex<Int64>(1_000_000_000) }
    private let clock = TestClock()
    private let message = "00000000-0000-4000-8000-000000000001"
    private let target = Target(computerId: "computer", session: "codex/example/tag", run: "run", conversation: "conversation", connectionId: "connection")
    private var file: DisplayedAttachment { DisplayedAttachment(name: "Screenshot.png", mime: "image/png", bytes: 1_200_000, reference: "[Image #1]", messageId: message, index: 0) }

    override func setUp() { temporary = FileManager.default.temporaryDirectory.appendingPathComponent("zerus-attachment-view-\(UUID().uuidString)") }
    override func tearDown() { try? FileManager.default.removeItem(at: temporary) }

    private var root: URL { temporary.appendingPathComponent("attachment-view") }
    private func advance(_ delta: Int64) { clock.value.withLock { $0 += delta } }
    private func cache(maxBytes: Int64 = 100 * 1024 * 1024) -> AttachmentViewCache {
        let clock = self.clock
        return AttachmentViewCache(root: root, now: { clock.value.withLock { $0 } }, maxBytes: maxBytes)
    }
    @discardableResult
    private func stored(_ cache: AttachmentViewCache, _ key: String, _ data: Data) throws -> AttachmentViewCache.Entry {
        try cache.store(key, name: "file.bin", mime: "application/octet-stream", data: data, sha256: sha256(data))
    }
    private func listing() -> [String] { (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? [] }

    private func result(_ id: String, _ offset: Int64, _ data: Data, digest: String = String(repeating: "a", count: 64)) -> JSONObject {
        ["request_id": .string(id), "name": .string(target.session), "message_id": .string(message), "index": 0, "bytes": .int(file.bytes),
         "sha256": .string(digest), "offset": .int(offset), "length": .int(Int64(data.count)), "file_name": .string(file.name), "mime": .string(file.mime),
         "data_base64": .string(data.base64EncodedString())]
    }

    func testReceiptPositionsAreTheOnlyRemoteIdentity() throws {
        XCTAssertTrue(AttachmentIdentity.remote(file))
        var invalid: [DisplayedAttachment] = []
        for change in [{ (f: inout DisplayedAttachment) in f.messageId = "" }, { $0.messageId = self.message.uppercased().replacingOccurrences(of: "0", with: "A") },
                       { $0.messageId = "../../private/file.png" }, { $0.index = -1 }, { $0.index = 8 }, { $0.bytes = 0 },
                       { $0.bytes = AttachmentPolicy.maxFileBytes + 1 }] {
            var copy = file
            change(&copy)
            invalid.append(copy)
        }
        for value in invalid { XCTAssertFalse(AttachmentIdentity.remote(value), "\(value)") }
        let payload = try AttachmentIdentity.payload(target, file, requestId: "request", offset: 524_288, length: 524_288)
        XCTAssertEqual(Set(["request_id", "expected_run_id", "expected_conversation_id", "message_id", "index", "offset", "length"]), Set(payload.keys))
        XCTAssertEqual(["run", "conversation", message], [payload.string("expected_run_id"), payload.string("expected_conversation_id"), payload.string("message_id")])
        XCTAssertEqual([0, 524_288, 524_288], [payload.int("index"), payload.int("offset"), payload.int("length")])
        var child = target
        child.agentId = "worker-1"
        child.parentConversation = "parent"
        child.archiveId = UUID().uuidString.lowercased()
        let childPayload = try AttachmentIdentity.payload(child, file, requestId: "request", offset: 0, length: 10)
        XCTAssertEqual(["parent", "worker-1", child.archiveId], [childPayload.string("expected_conversation_id"), childPayload.string("agent_id"), childPayload.string("archive_id")])
        let ranges: [(Int64, Int)] = [(-1, 1), (0, 0), (0, 524_289), (file.bytes, 1), (file.bytes - 1, 2)]
        for (offset, length) in ranges {
            XCTAssertThrowsError(try AttachmentIdentity.payload(target, file, requestId: "request", offset: offset, length: length))
        }
        var cleared = target
        cleared.conversation = ""
        XCTAssertThrowsError(try AttachmentIdentity.payload(cleared, file, requestId: "request", offset: 0, length: 1))
        var otherRun = target
        otherRun.run = "other"
        var second = file
        second.index = 1
        XCTAssertNotEqual(AttachmentIdentity.remoteKey(target, file), AttachmentIdentity.remoteKey(otherRun, file))
        XCTAssertNotEqual(AttachmentIdentity.remoteKey(target, file), AttachmentIdentity.remoteKey(target, second))
    }

    func testCapabilityGatingExplainsUpdatesAndConnectivity() {
        let both: Set<String> = ["history", "attachment_read"]
        XCTAssertEqual(.supported, AttachmentIdentity.access(relay: both, machine: both, online: true))
        XCTAssertEqual(.unsupported, AttachmentIdentity.access(relay: ["history"], machine: both, online: true))
        XCTAssertEqual(.unsupported, AttachmentIdentity.access(relay: both, machine: ["history"], online: true))
        XCTAssertEqual(.offline, AttachmentIdentity.access(relay: both, machine: both, online: false))
        XCTAssertEqual(.unknown, AttachmentIdentity.access(relay: nil, machine: both, online: true))
        XCTAssertEqual("Update Zerus on this computer to open attachments.", AttachmentIdentity.unavailableReason(target, file, .unsupported))
        XCTAssertNil(AttachmentIdentity.unavailableReason(target, file, .supported))
        var local = file
        local.messageId = ""
        XCTAssertNotNil(AttachmentIdentity.unavailableReason(target, local, .supported))
        var demo = target
        demo.connectionId = "demo"
        XCTAssertNotNil(AttachmentIdentity.unavailableReason(demo, file, .supported))
        XCTAssertEqual(AttachmentIdentity.updateRequired, AttachmentIdentity.relayFailure(409))
        XCTAssertEqual(AttachmentIdentity.updateRequired, AttachmentIdentity.failure(["state": "failed", "error": "native attachment read ABI is unavailable"]))
        XCTAssertTrue(AttachmentIdentity.failure(["state": "queued"]).contains("did not answer"))
    }

    func testRangesMustAnswerTheExactRequestFileAndPosition() throws {
        let data = Data((0..<524_288).map { UInt8(truncatingIfNeeded: $0) })
        let first = try AttachmentIdentity.chunk(result("one", 0, data), requestId: "one", target: target, file: file, offset: 0, length: data.count, first: nil)
        XCTAssertEqual(data, first.data)
        let tail = Data(repeating: 7, count: 524_288)
        let offset = Int64(data.count)
        _ = try AttachmentIdentity.chunk(result("two", offset, tail), requestId: "two", target: target, file: file, offset: offset, length: 524_288, first: first)
        let changes: [(String, JSON)] = [("request_id", "other"), ("name", "codex/other"), ("message_id", .string(UUID().uuidString.lowercased())), ("index", 1),
                                         ("bytes", .int(file.bytes + 1)), ("sha256", .string(String(repeating: "b", count: 64))), ("sha256", "not-a-digest"),
                                         ("offset", 1), ("length", 10), ("data_base64", "not base64!"), ("archive_id", .string(UUID().uuidString.lowercased())),
                                         ("agent_id", "child")]
        for (field, value) in changes {
            XCTAssertThrowsError(try AttachmentIdentity.chunk(result("two", offset, tail).putting(field, value), requestId: "two", target: target, file: file,
                                                              offset: offset, length: 524_288, first: first), field) { XCTAssertTrue($0 is AttachmentOpenError) }
        }
        XCTAssertThrowsError(try AttachmentIdentity.chunk(result("one", 0, data.prefix(100)), requestId: "one", target: target, file: file, offset: 0,
                                                          length: data.count, first: nil))
    }

    func testOwnDeliveredFilesKeepTheirPrivateCopyByPosition() {
        let local = [Attachment(id: UUID().uuidString.lowercased(), name: "Screenshot.png", mime: "image/png", bytes: 1_200_000, sha256: String(repeating: "c", count: 64)),
                     Attachment(id: UUID().uuidString.lowercased(), name: "notes.txt", mime: "text/plain", bytes: 5, sha256: String(repeating: "d", count: 64))]
        var renamed = file
        renamed.name = "renamed.txt"
        renamed.mime = "text/plain"
        renamed.bytes = 5
        renamed.index = 1
        let merged = AttachmentIdentity.withLocal([file, renamed], local)
        XCTAssertEqual(local[0].id, merged[0].localId)
        XCTAssertEqual(message, merged[0].messageId)
        XCTAssertEqual("", merged[1].localId)
        XCTAssertEqual(local[0], AttachmentIdentity.local(merged[0]))
        var tampered = merged[0]
        tampered.sha256 = "bad"
        XCTAssertNil(AttachmentIdentity.local(tampered))
        XCTAssertNotEqual(AttachmentIdentity.rowKey(target, merged[0]), AttachmentIdentity.rowKey(target, file))
        let displayed = local[1].displayed()
        XCTAssertEqual([local[1].id, local[1].sha256, ""], [displayed.localId, displayed.sha256, displayed.messageId])
    }

    func testCacheVerifiesDigestIdentityAndNeverExposesPartialFiles() throws {
        let cache = cache()
        let data = Data("synthetic attachment".utf8)
        let pending = try cache.begin("key", name: "../../evil.png", mime: "image/png")
        try pending.write(data)
        XCTAssertNil(cache.lookup("key"))
        let entry = try pending.commit(sha256(data))
        XCTAssertEqual("evil.png", entry.file.lastPathComponent)
        XCTAssertTrue(entry.file.resolvingSymlinksInPath().path.hasPrefix(root.resolvingSymlinksInPath().path))
        XCTAssertEqual(data, try Data(contentsOf: XCTUnwrap(cache.lookup("key")).file))
        XCTAssertNil(cache.lookup("other"))
        let wrong = try cache.begin("bad", name: "bad.bin", mime: "application/octet-stream")
        try wrong.write(data)
        XCTAssertThrowsError(try wrong.commit(String(repeating: "0", count: 64))) { XCTAssertTrue($0 is AttachmentOpenError) }
        XCTAssertNil(cache.lookup("bad"))
        let oversize = try cache.begin("large", name: "large.bin", mime: "application/octet-stream")
        XCTAssertThrowsError(try oversize.write(Data(count: Int(AttachmentPolicy.maxFileBytes) + 1))) { XCTAssertTrue($0 is AttachmentOpenError) }
        oversize.abort()
        try Data("tampered!!!!!!!!!!!!".utf8).write(to: entry.file)
        XCTAssertNil(cache.lookup("key"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: entry.file.path))
        XCTAssertEqual([], listing())
    }

    func testCacheExpiresAfterADayAndTrimsLeastRecentlyOpenedWithinBudget() throws {
        let cache = cache(maxBytes: 25)
        try stored(cache, "a", Data(repeating: 1, count: 10))
        advance(1000)
        try stored(cache, "b", Data(repeating: 2, count: 10))
        advance(1000)
        XCTAssertNotNil(cache.lookup("a"))
        advance(1000)
        try stored(cache, "c", Data(repeating: 3, count: 10))
        XCTAssertNil(cache.lookup("b"))
        XCTAssertNotNil(cache.lookup("a"))
        XCTAssertNotNil(cache.lookup("c"))
        advance(24 * 60 * 60 * 1000)
        XCTAssertNil(cache.lookup("a"))
        try stored(cache, "d", Data(repeating: 4, count: 5))
        cache.trim()
        XCTAssertEqual(1, listing().count)
        let active = try cache.begin("e", name: "e.bin", mime: "application/octet-stream")
        try FileManager.default.createDirectory(at: root.appendingPathComponent(".part-stale"), withIntermediateDirectories: true)
        cache.trim()
        XCTAssertTrue(listing().contains { $0.hasPrefix(".part-") })
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(".part-stale").path))
        active.abort()
        cache.clear()
        XCTAssertEqual(0, listing().count)
    }

    private let fileTarget = Target(computerId: "computer-a", session: "codex/example/session", run: "run", conversation: "conversation", connectionId: "gateway")
    private let reference = ComputerFileReference(path: "/workspace/example/Сводная итог.xlsx")
    private static func fileResult(_ data: Data, _ payload: JSONObject, _ fileTarget: Target, _ reference: ComputerFileReference, digest: String? = nil) -> JSONObject {
        let offset = payload.int("offset"), length = payload.int("length")
        let end = min(offset + length, data.count)
        return ["state": "completed", "result": .object([
            "request_id": .string(payload.string("request_id")), "name": .string(fileTarget.session), "run_id": .string(fileTarget.run),
            "conversation_id": .string(fileTarget.conversation), "requested_path": .string(reference.path), "path": .string(reference.path),
            "file_name": .string(reference.name), "mime": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "bytes": .int(Int64(data.count)),
            "sha256": .string(digest ?? sha256(data)), "offset": .int(Int64(offset)), "length": .int(Int64(end - offset)),
            "data_base64": .string(data[offset..<end].base64EncodedString())])]
    }

    func testBinaryChunksKeepOriginalTargetAndFinishWithVerifiedPrivateBytes() async throws {
        let data = Data((0..<1_100_123).map { UInt8(truncatingIfNeeded: $0) })
        let cache = cache()
        let calls = Mutex(0), progress = Mutex<[Double]>([])
        let expected = sha256(data), path = reference.path, fileTarget = fileTarget, reference = reference
        let (_, entry) = try await ComputerFileDownloads.load(cache, target: fileTarget, reference: reference, read: { id, payload in
            XCTAssertEqual(id, payload.string("request_id"))
            XCTAssertEqual(path, payload.string("path"))
            XCTAssertEqual("run", payload.string("expected_run_id"))
            if payload.int("offset") > 0 { XCTAssertEqual(expected, payload.string("sha256")) }
            calls.withLock { $0 += 1 }
            return Self.fileResult(data, payload, fileTarget, reference)
        }, progress: { value in progress.withLock { $0.append(value) } })
        XCTAssertEqual(3, calls.withLock { $0 })
        XCTAssertEqual(1, progress.withLock { $0.last })
        XCTAssertEqual(data, try Data(contentsOf: entry.file))
        XCTAssertEqual(expected, entry.sha256)
        XCTAssertEqual(reference.name, entry.name)
    }

    func testChangedSecondChunkNeverPublishesAPartialOrMixedFile() async throws {
        let data = Data((0..<700_000).map { UInt8(truncatingIfNeeded: $0) })
        let cache = cache(), fileTarget = fileTarget, reference = reference
        do {
            _ = try await ComputerFileDownloads.load(cache, target: fileTarget, reference: reference, read: { _, payload in
                Self.fileResult(data, payload, fileTarget, reference, digest: payload.int("offset") > 0 ? String(repeating: "b", count: 64) : nil)
            })
            XCTFail("Expected rejection")
        } catch { XCTAssertTrue(error is AttachmentOpenError) }
        XCTAssertEqual([], listing())
    }

    func testAttachmentDownloadMapsReceiptsAndRelayFailuresWithoutPublishing() async throws {
        let cache = cache()
        let small = DisplayedAttachment(name: "note.txt", mime: "text/plain", bytes: 5, messageId: message, index: 0)
        let data = Data("hello".utf8), session = target.session, message = message
        let entry = try await AttachmentDownloads.load(cache, key: "remote", target: target, file: small, read: { id, payload in
            ["state": "completed", "result": .object(["request_id": .string(id), "name": .string(session), "message_id": .string(message), "index": 0,
                                                      "bytes": 5, "sha256": .string(sha256(data)), "offset": 0, "length": 5,
                                                      "data_base64": .string(data.base64EncodedString())])]
        })
        XCTAssertEqual(data, try Data(contentsOf: entry.file))
        do {
            _ = try await AttachmentDownloads.load(cache, key: "queued", target: target, file: small, read: { _, _ in ["state": "queued"] })
            XCTFail("Expected rejection")
        } catch { XCTAssertTrue((error as? AttachmentOpenError)?.message.contains("did not answer") == true) }
        do {
            _ = try await AttachmentDownloads.load(cache, key: "old", target: target, file: small, read: { _, _ in throw RelayError(status: 409, message: "") })
            XCTFail("Expected rejection")
        } catch { XCTAssertEqual(AttachmentIdentity.updateRequired, (error as? AttachmentOpenError)?.message) }
        XCTAssertEqual(1, listing().count)
    }
}

/// Ported from Android AttachmentStoreTest; synthetic data and an injected software key.
final class AttachmentStoreTests: XCTestCase {
    private var temporary: URL!
    private var root: URL { temporary.appendingPathComponent("attachments") }
    private let key = StaticKeyProvider(SymmetricKey(size: .bits256))
    private var files: AttachmentStore!

    override func setUp() {
        temporary = FileManager.default.temporaryDirectory.appendingPathComponent("zerus-attachment-test-\(UUID().uuidString)")
        files = AttachmentStore(root: root, keys: key)
    }
    override func tearDown() { try? FileManager.default.removeItem(at: temporary) }

    private func saved(_ name: String = "fixture.txt", _ data: Data = Data("private fixture body".utf8)) throws -> Attachment {
        try files.importData(data, name: name, mime: "text/plain", current: [])
    }
    private func metadata(_ bytes: Int64 = 1) -> Attachment {
        Attachment(id: UUID().uuidString.lowercased(), name: "fixture.bin", mime: "application/octet-stream", bytes: bytes, sha256: String(repeating: "0", count: 64))
    }
    private func failure(_ block: () throws -> Void) -> String {
        do { try block() } catch { return (error as? ZerusError)?.message ?? "\(error)" }
        XCTFail("Expected attachment rejection")
        return ""
    }
    private func listing() -> [String] { (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? [] }
    private func blob(_ attachment: Attachment) -> URL { root.appendingPathComponent("\(attachment.id).bin") }
    private func wire(_ attachment: Attachment) throws -> JSONObject { try XCTUnwrap(files.payload([attachment]).array?.first?.object) }

    func testImmutableEncryptedCopyHasNoWirePathOrReference() throws {
        let original = Data("synthetic secret fixture".utf8)
        let attachment = try saved("fixture.txt", original)
        let stored = try Data(contentsOf: blob(attachment))
        XCTAssertNil(stored.range(of: original))
        let wire = try wire(attachment)
        XCTAssertEqual(original, Data(base64Encoded: wire.string("data_base64")))
        XCTAssertEqual(Set(["name", "mime", "data_base64"]), Set(wire.keys))
        XCTAssertEqual(sha256(original), attachment.sha256)
        let permissions = try FileManager.default.attributesOfItem(atPath: blob(attachment).path)[.posixPermissions] as? NSNumber
        XCTAssertEqual(0o600, permissions?.intValue)
    }

    func testEmptyAndOverLimitInputsLeaveNoPartOrPublishedFile() {
        XCTAssertTrue(failure { _ = try saved("empty.txt", Data()) }.contains("Empty"))
        XCTAssertEqual([], listing())
        XCTAssertTrue(failure { _ = try saved("large.bin", Data(count: Int(AttachmentPolicy.maxFileBytes) + 1)) }.contains("10 MiB"))
        XCTAssertEqual([], listing())
    }

    func testExactPerFileBoundaryImportsAndRevalidates() throws {
        let data = Data((0..<Int(AttachmentPolicy.maxFileBytes)).map { UInt8($0 % 251) })
        let attachment = try saved("large.bin", data)
        XCTAssertEqual(AttachmentPolicy.maxFileBytes, attachment.bytes)
        XCTAssertEqual(data, try files.decrypt(attachment))
    }

    func testAggregateAndCountBoundsUseActualBytes() {
        let current = [metadata(AttachmentPolicy.maxFileBytes), metadata(AttachmentPolicy.maxFileBytes)]
        XCTAssertTrue(failure { _ = try files.importData(Data([1]), name: "small.bin", mime: nil, current: current) }.contains("20 MiB"))
        XCTAssertEqual([], listing())
        XCTAssertTrue(failure { _ = try files.importData(Data([1]), name: "small.bin", mime: nil, current: (0..<8).map { _ in metadata() }) }.contains("8 files"))
    }

    func testFileImportChecksSizeBeforeReadingAndCopiesExactBytes() throws {
        try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
        let source = temporary.appendingPathComponent("picked.pdf")
        try Data("pdf body".utf8).write(to: source)
        let attachment = try files.importFile(source, name: nil, mime: "application/pdf", current: [])
        XCTAssertEqual("picked.pdf", attachment.name)
        XCTAssertEqual(Data("pdf body".utf8), try files.decrypt(attachment))
        let large = temporary.appendingPathComponent("large.bin")
        FileManager.default.createFile(atPath: large.path, contents: nil)
        let handle = try FileHandle(forWritingTo: large)
        try handle.truncate(atOffset: UInt64(AttachmentPolicy.maxFileBytes) + 1)
        try handle.close()
        XCTAssertTrue(failure { _ = try files.importFile(large, name: nil, mime: nil, current: []) }.contains("10 MiB"))
        XCTAssertEqual(1, listing().count)
    }

    func testChangedDigestSizeNameMimeAndMissingFileAreRejectedBeforeWire() throws {
        let attachment = try saved()
        for change in [{ (a: inout Attachment) in a.sha256 = String(repeating: "f", count: 64) }, { $0.bytes += 1 }, { $0.name = "changed-name.txt" },
                       { $0.mime = "application/json" }] {
            var copy = attachment
            change(&copy)
            _ = failure { _ = try files.payload([copy]) }
        }
        try FileManager.default.removeItem(at: blob(attachment))
        _ = failure { _ = try files.payload([attachment]) }
    }

    func testCiphertextTagMutationTruncationAndCrossReferenceSwapCannotBeSent() throws {
        let first = try saved()
        var stored = try Data(contentsOf: blob(first))
        stored[stored.count - 1] ^= 1
        try stored.write(to: blob(first))
        _ = failure { _ = try files.payload([first]) }
        try stored.prefix(20).write(to: blob(first))
        _ = failure { _ = try files.payload([first]) }
        let second = try saved("fixture.txt", Data([1, 2, 3]))
        try Data(contentsOf: blob(second)).write(to: blob(first))
        var swapped = first
        swapped.bytes = second.bytes
        swapped.sha256 = second.sha256
        _ = failure { _ = try files.payload([swapped]) }
    }

    func testRemovingProtectedObjectsNeverDeletesDraftOrInFlightCopies() throws {
        let attachment = try saved()
        XCTAssertFalse(try files.remove(attachment.id, protected: [attachment.id]))
        XCTAssertTrue(FileManager.default.fileExists(atPath: blob(attachment).path))
        XCTAssertTrue(try files.remove(attachment.id, protected: []))
        XCTAssertFalse(try files.remove(attachment.id, protected: []))
        _ = failure { _ = try files.remove("../fixture", protected: []) }
    }

    func testSymlinkObjectAndSymlinkDirectoryAreRejected() throws {
        let attachment = try saved()
        let outside = temporary.appendingPathComponent("outside")
        try Data(contentsOf: blob(attachment)).write(to: outside)
        try FileManager.default.removeItem(at: blob(attachment))
        try FileManager.default.createSymbolicLink(at: blob(attachment), withDestinationURL: outside)
        _ = failure { _ = try files.payload([attachment]) }
        let linked = temporary.appendingPathComponent("linked")
        try FileManager.default.createSymbolicLink(at: linked, withDestinationURL: root)
        _ = failure { _ = try AttachmentStore(root: linked, keys: key).importData(Data([1]), name: "fixture.bin", mime: nil, current: []) }
    }

    func testDiskReopenWithSameKeyWorksAndWrongKeyNeverReturnsWire() throws {
        let attachment = try files.importData(Data([1, 2]), name: "fixture.bin", mime: nil, current: [])
        XCTAssertEqual(Data([1, 2]), try AttachmentStore(root: root, keys: key).decrypt(attachment))
        _ = failure { _ = try AttachmentStore(root: root, keys: StaticKeyProvider(SymmetricKey(size: .bits256))).payload([attachment]) }
    }

    func testNamesAreSafeUtf8BoundedAndMimeMatchesNativeHundredByteLimit() {
        XCTAssertEqual("fixture_.txt", AttachmentPolicy.filename("C:\\private\\fixture\n.txt"))
        XCTAssertEqual("attachment", AttachmentPolicy.filename("../.."))
        let long = AttachmentPolicy.filename(String(repeating: "😀", count: 200))
        XCTAssertLessThanOrEqual(long.utf8.count, 255)
        XCTAssertFalse(long.contains("\u{FFFD}"))
        XCTAssertEqual("application/octet-stream", AttachmentPolicy.mime("x/" + String(repeating: "a", count: 101)))
        XCTAssertEqual("application/octet-stream", AttachmentPolicy.mime("text/plain\r\ninjected"))
        XCTAssertEqual("image/png", AttachmentPolicy.mime("IMAGE/PNG"))
        var evil = metadata()
        evil.id = "../evil"
        _ = failure { try AttachmentPolicy.selection([evil]) }
        let same = metadata()
        _ = failure { try AttachmentPolicy.selection([same, same]) }
    }

    func testSavedDraftFilesDecryptIntoTheViewCacheWithTheirExactBytes() throws {
        let data = Data((0..<200_000).map { UInt8($0 % 251) })
        let saved = try files.importData(data, name: "photo.jpg", mime: "image/jpeg", current: [])
        let cache = AttachmentViewCache(root: temporary.appendingPathComponent("view"))
        let entry = try files.open(saved, cache: cache)
        XCTAssertEqual(data, try Data(contentsOf: entry.file))
        var changed = saved
        changed.sha256 = String(repeating: "e", count: 64)
        _ = failure { _ = try files.decrypt(changed) }
        XCTAssertEqual(data.base64EncodedString(), try wire(saved).string("data_base64"))
    }
}

final class AttachmentViewingTests: XCTestCase {
    func testViewNamesGainAnExtensionFromTheirTypeOnlyWhenMissing() {
        XCTAssertEqual("Screenshot.png", AttachmentViewing.name("Screenshot.png", mime: "image/jpeg"))
        XCTAssertEqual("notes.pdf", AttachmentViewing.name("notes", mime: "application/pdf"))
        XCTAssertEqual("blob", AttachmentViewing.name("blob", mime: "application/octet-stream"))
        XCTAssertEqual("evil.txt", AttachmentViewing.name("../evil", mime: "text/plain"))
    }
}
