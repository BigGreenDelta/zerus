import XCTest
@testable import ZerusKit

final class SharedReadRetriesTests: XCTestCase {
    func testTheDelaySaturatesAtSixteenMinutes() {
        var retries = SharedReadRetries()
        var now = 0.0, delays: [Double] = []
        for _ in 0..<6 {
            let due = retries.unapplied("mark", connectionId: "workspace", now: now)!
            delays.append(due - now)
            XCTAssertEqual(retries.release(now: due), ["mark"])
            now = due
        }
        XCTAssertEqual(delays, [120, 240, 480, 960, 960, 960])
        XCTAssertEqual((0...5).map(SharedReadRetries.delay), [120, 240, 480, 960, 960, 960])
        XCTAssertEqual(SharedReadRetries.delay(-1), 120)
    }

    func testNothingIsReleasedEarlyAndAnAppliedMarkIsNeverReleased() {
        var retries = SharedReadRetries()
        let due = retries.unapplied("mark", connectionId: "workspace", now: 10)!
        XCTAssertEqual(retries.nextDue, due)
        XCTAssertEqual(retries.release(now: due - 0.001), [])
        retries.applied("mark")
        XCTAssertNil(retries.nextDue)
        // A timer that was already sleeping finds nothing to release.
        XCTAssertEqual(retries.release(now: due + 1), [])
        XCTAssertEqual(retries.count, 0)
    }

    func testAStaleWakeupCannotReleaseARescheduledMark() {
        var retries = SharedReadRetries()
        let first = retries.unapplied("mark", connectionId: "workspace", now: 0)!
        XCTAssertEqual(retries.release(now: first), ["mark"])
        let second = retries.unapplied("mark", connectionId: "workspace", now: first)!
        // The wakeup for the first attempt arrives late; the second attempt is not due yet.
        XCTAssertEqual(retries.release(now: first + 1), [])
        XCTAssertEqual(retries.release(now: second), ["mark"])
    }

    func testCapacityFailsClosedAndRetiredMarksMakeRoom() {
        var retries = SharedReadRetries()
        for index in 0..<SharedReadRetries.capacity {
            XCTAssertNotNil(retries.unapplied("mark-\(index)", connectionId: "workspace", now: 0))
        }
        // At capacity a new mark is not scheduled: it stays sent, and no timer is added for it.
        XCTAssertNil(retries.unapplied("overflow", connectionId: "workspace", now: 1))
        XCTAssertEqual(retries.count, SharedReadRetries.capacity)
        XCTAssertEqual(retries.release(now: 120).count, SharedReadRetries.capacity)
        // Released marks are remembered for one maximum delay, then retired.
        XCTAssertNil(retries.unapplied("overflow", connectionId: "workspace", now: 120 + 960 - 1))
        XCTAssertNotNil(retries.unapplied("overflow", connectionId: "workspace", now: 120 + 960 + 1))
        XCTAssertEqual(retries.count, 1)
    }

    func testARetiredMarkStartsOverAtTwoMinutes() {
        var retries = SharedReadRetries()
        let first = retries.unapplied("mark", connectionId: "workspace", now: 0)!
        _ = retries.release(now: first)
        let second = retries.unapplied("mark", connectionId: "workspace", now: first + 10)!
        XCTAssertEqual(second - (first + 10), 240)
        _ = retries.release(now: second)
        let restarted = retries.unapplied("mark", connectionId: "workspace", now: second + 960 + 1)!
        XCTAssertEqual(restarted - (second + 960 + 1), 120)
    }

    func testForgettingAWorkspaceOrResettingReleasesNothingLater() {
        var retries = SharedReadRetries()
        let due = retries.unapplied("a", connectionId: "first", now: 0)!
        _ = retries.unapplied("b", connectionId: "second", now: 0)
        retries.forget(connection: "first")
        XCTAssertEqual(retries.release(now: due), ["b"])
        _ = retries.unapplied("c", connectionId: "second", now: due)
        retries.reset()
        XCTAssertNil(retries.nextDue)
        XCTAssertEqual(retries.release(now: .greatestFiniteMagnitude), [])
    }
}
