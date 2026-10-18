import Synchronization
import XCTest
@testable import ZerusKit

/// A value shared with escaping test closures.
private final class Shared<Value: Sendable>: Sendable {
    private let mutex: Mutex<Value>
    init(_ value: Value) { mutex = Mutex(value) }
    var value: Value { mutex.withLock { $0 } }
    func set(_ value: Value) { mutex.withLock { $0 = value } }
}

private final class Counter: Sendable {
    private let mutex = Mutex(0)
    func next() -> Int { mutex.withLock { $0 += 1; return $0 } }
}

/// How observed work ended; errors are reduced to what the assertions need.
private enum Outcome<T: Sendable>: Sendable {
    case value(T)
    case expired
    case failed(String)

    var success: T? { if case .value(let value) = self { value } else { nil } }
}

/// The automatic status-only receipt check; the probe is ported from Android OutgoingReceiptProbeTest.
final class OutgoingReceiptProbeTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let connection = Connection(id: "gateway", label: "Test", url: "https://gateway.example", token: "synthetic-token")
    private var message: OutgoingMessage {
        OutgoingMessage(requestId: "request", target: target, text: "Original message", attachments: [], createdAt: 1, status: "uncertain")
    }

    private func with(_ message: OutgoingMessage, status: String? = nil, requestId: String? = nil) -> OutgoingMessage {
        var copy = message
        if let status { copy.status = status }
        if let requestId { copy.requestId = requestId }
        return copy
    }

    /// Runs `work` without trusting it to end: the test stops waiting after `seconds` and then cancels it.
    private func observe<T: Sendable>(_ seconds: Double, _ work: @escaping @Sendable () async throws -> T) async -> (Outcome<T>?, Duration) {
        let outcome = Shared<Outcome<T>?>(nil)
        let started = ContinuousClock.now
        let task = Task {
            do {
                outcome.set(.value(try await work()))
            } catch let error as ZerusError where error == Deadline.expired {
                outcome.set(.expired)
            } catch {
                outcome.set(.failed(String(describing: type(of: error))))
            }
        }
        while outcome.value == nil && ContinuousClock.now - started < .seconds(seconds) {
            try? await Task.sleep(for: .milliseconds(5))
        }
        let elapsed = ContinuousClock.now - started
        task.cancel()
        return (outcome.value, elapsed)
    }

    private func expired<T>(_ outcome: Outcome<T>?, _ line: UInt = #line) {
        guard case .expired? = outcome else { return XCTFail("Expected the deadline to end the check, got \(String(describing: outcome)).", line: line) }
    }

    /// Only reads of this request's original receipt happen; nothing is submitted.
    private func onlyReceiptReads(_ count: Int, _ line: UInt = #line) {
        XCTAssertEqual(StubProtocol.requests.map(\.method), Array(repeating: "GET", count: count), line: line)
        XCTAssertEqual(StubProtocol.requests.map(\.url.path), Array(repeating: "/v1/requests/known", count: count), line: line)
    }

    // MARK: Probe

    func testSamePresentationOnlyChecksOnceButReopeningOrANewRequestCanCheck() {
        var probe = OutgoingReceiptProbe()
        XCTAssertTrue(probe.claim(target, navigation: 1, message: message, busy: false))
        for _ in 0..<20 { XCTAssertFalse(probe.claim(target, navigation: 1, message: message, busy: false)) }
        XCTAssertTrue(probe.claim(target, navigation: 2, message: message, busy: false))
        XCTAssertTrue(probe.claim(target, navigation: 2, message: with(message, requestId: "new request"), busy: false))
    }

    func testOtherScopesAndActiveOrSettledDeliveriesNeverClaim() {
        var probe = OutgoingReceiptProbe()
        XCTAssertFalse(probe.claim(target, navigation: 1, message: message, busy: true))
        for status in ["sending", "submitted", "recorded", "reviewed", "failed"] {
            XCTAssertFalse(probe.claim(target, navigation: 1, message: with(message, status: status), busy: false), status)
        }
        for other in [target.with(run: "other"), target.with(conversation: "other"), target.with(connectionId: "other"), target.with(agentId: "child")] {
            XCTAssertFalse(probe.claim(other, navigation: 1, message: message, busy: false))
        }
        XCTAssertTrue(probe.claim(target, navigation: 1, message: message, busy: false))
    }

    // MARK: Deadline

    func testDeadlineReturnsAFastResultAndKeepsItsOwnFailure() async {
        let (value, _) = await observe(3) { try await Deadline.within(.seconds(5)) { "receipt" } }
        XCTAssertEqual(value?.success, "receipt")
        struct Refused: Error {}
        let (failure, _) = await observe(3) { () async throws -> String in try await Deadline.within(.seconds(5)) { throw Refused() } }
        guard case .failed(let name)? = failure else { return XCTFail("The failure must propagate.") }
        XCTAssertEqual(name, "Refused")
    }

    func testDeadlineEndsCooperativeWorkAndLeavesNothingRunning() async throws {
        let finished = Shared(false)
        let (result, elapsed) = await observe(3) { () async throws -> String in
            try await Deadline.within(.milliseconds(100)) {
                try await Task.sleep(for: .seconds(10))
                finished.set(true)
                return "late"
            }
        }
        expired(result)
        XCTAssertLessThan(elapsed, .seconds(1))
        try await Task.sleep(for: .milliseconds(300))
        XCTAssertFalse(finished.value)
        print("Deadline 100 ms over a 10 s sleep ended after \(elapsed)")
    }

    // MARK: Bounded receipt check

    func testHungReceiptReadIsCancelledAtTheDeadlineWithoutAnotherRead() async throws {
        StubProtocol.reset { _ in .hang }
        let client = StubProtocol.client(), connection = self.connection
        let (result, elapsed) = await observe(3) { try await client.boundedReceipt(connection, id: "known", limit: .milliseconds(300)) }
        expired(result)
        XCTAssertLessThan(elapsed, .seconds(2))
        try await Task.sleep(for: .milliseconds(600))
        onlyReceiptReads(1)
        XCTAssertEqual(StubProtocol.stoppedHanging.withLock { $0 }, 1)
        print("Hung first receipt read cancelled after \(elapsed)")
    }

    func testHungPollIsCancelledAndPollingStops() async throws {
        let reads = Counter()
        StubProtocol.reset { _ in
            reads.next() == 1 ? StubProtocol.json(200, #"{"request_id":"known","state":"claimed"}"#) : .hang
        }
        let client = StubProtocol.client(), connection = self.connection
        let (result, elapsed) = await observe(3) {
            try await client.boundedReceipt(connection, id: "known", limit: .milliseconds(400), pollDelay: .milliseconds(50))
        }
        expired(result)
        XCTAssertLessThan(elapsed, .seconds(2))
        try await Task.sleep(for: .milliseconds(600))
        onlyReceiptReads(2)
        XCTAssertEqual(StubProtocol.stoppedHanging.withLock { $0 }, 1)
        print("Hung receipt poll cancelled after \(elapsed)")
    }

    func testDeadlineDuringThePollDelayStartsNoLaterRead() async throws {
        StubProtocol.reset { _ in StubProtocol.json(200, #"{"request_id":"known","state":"claimed"}"#) }
        let client = StubProtocol.client(), connection = self.connection
        let (result, elapsed) = await observe(3) {
            try await client.boundedReceipt(connection, id: "known", limit: .milliseconds(200), pollDelay: .seconds(1))
        }
        expired(result)
        XCTAssertLessThan(elapsed, .seconds(1))
        // The cancelled poll delay would have read again after one second.
        try await Task.sleep(for: .milliseconds(1300))
        onlyReceiptReads(1)
        print("Deadline during the poll delay ended after \(elapsed)")
    }

    func testSettledReceiptIsReturnedWithinTheDeadline() async {
        let reads = Counter()
        StubProtocol.reset { _ in
            reads.next() == 1 ? StubProtocol.json(200, #"{"request_id":"known","state":"claimed"}"#)
                : StubProtocol.json(200, #"{"request_id":"known","state":"completed","result":{"status":"submitted"}}"#)
        }
        let client = StubProtocol.client(), connection = self.connection
        let (result, _) = await observe(3) {
            try await client.boundedReceipt(connection, id: "known", limit: .seconds(5), pollDelay: .milliseconds(50))
        }
        XCTAssertEqual(result?.success?.string("state"), "completed")
        onlyReceiptReads(2)
    }
}
