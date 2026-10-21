import XCTest
import Darwin
import Dispatch

/// Owned reverse-ACK registration in the UITest runner, never a command channel or sender authentication.
/// The main-queue callback and teardown share one actor and attempt each token cancellation once.
@MainActor private final class ReviewAcknowledgement {
    let arrived = XCTestExpectation(description: "Owned review acknowledgement")
    private var token: Int32?
    private(set) var receivedAt: Double?
    private var failure = ""
    private var consumed = false
    func register(_ name: String) {
        precondition(token == nil && !consumed && name.utf8.count <= 128)
        var selected: Int32 = -1
        let code = name.withCString { pointer in
            notify_register_dispatch(pointer, &selected, DispatchQueue.main) { [weak self] received in
                MainActor.assumeIsolated { self?.receive(received) }
            }
        }
        if selected >= 0 { token = selected } // Own it before checking a fallible result.
        XCTAssertTrue(code == 0 && token != nil, "Reverse acknowledgement registration failed")
    }
    private func receive(_ received: Int32) {
        guard !consumed, token == received else { failure = "stale-or-repeated-reverse-ack"; return }
        consumed = true
        token = nil // A failed cancellation is unknown; never try the token again.
        guard notify_cancel(received) == 0 else { failure = "reverse-ack-cancel"; arrived.fulfill(); return }
        receivedAt = ProcessInfo.processInfo.systemUptime
        arrived.fulfill()
    }
    func wait() {
        XCTAssertEqual(XCTWaiter.wait(for: [arrived], timeout: 2), .completed)
        XCTAssertTrue(consumed && token == nil && failure.isEmpty && receivedAt != nil, "Missing checked reverse acknowledgement")
    }
    func close() {
        if let owned = token {
            token = nil
            if notify_cancel(owned) != 0 { failure = "reverse-ack-teardown-cancel" }
        }
        XCTAssertTrue(failure.isEmpty && token == nil, "Owned reverse acknowledgement cleanup failed: \(failure)")
    }
}

/// Native SwiftUI tests of the unchanged production QuestionCard. The application is a separate
/// simulator-only memory host; callback assertions do not prove provider, relay or persistence delivery.
@MainActor final class ApprovalChoicesUITests: XCTestCase {
    private var app: XCUIApplication!
    private enum Mode: String { case normal, countdown, feedback, replacement }
    private var fixtureLaunch = ""
    private var fixtureGeneration = ""
    private var acknowledgementNonce = ""
    private var posted: Set<String> = []
    @MainActor private func launchFixture(_ mode: Mode = .normal) {
        continueAfterFailure = false
        fixtureLaunch = UUID().uuidString.lowercased(); posted = []
        acknowledgementNonce = UUID().uuidString.lowercased()
        XCTAssertNotEqual(acknowledgementNonce, fixtureLaunch)
        app = XCUIApplication()
        app.launchArguments = ["--zerus-fixture-launch", fixtureLaunch, "--zerus-fixture-mode", mode.rawValue,
                               "--zerus-fixture-ack", acknowledgementNonce]
        app.launch()
        XCTAssertTrue(app.staticTexts["Synthetic UI only — no native delivery"].waitForExistence(timeout: 5))
        count(0)
        fixtureGeneration = app.staticTexts["fixture-generation"].label
        XCTAssertNotNil(UUID(uuidString: fixtureGeneration))
        let ready = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == %@",
            "ready|\(mode.rawValue)|\(fixtureLaunch)|\(fixtureGeneration)"), object: app.staticTexts["fixture-control-status"])
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 2), .completed)
        phase("launched-\(mode.rawValue)")
    }
    private func phase(_ name: String) {
        print("FIXTURE-UI \(name) uptime=\(ProcessInfo.processInfo.systemUptime) generation=\(fixtureGeneration)")
    }
    private func post(_ action: String) {
        XCTAssertTrue(["begin-feedback-review", "replace", "begin-countdown-review"].contains(action))
        XCTAssertTrue(posted.insert(action).inserted, "No control replays")
        XCTAssertEqual(app.staticTexts["fixture-generation"].label, fixtureGeneration)
        let name = "test.example.zerus.ui.\(fixtureLaunch).\(fixtureGeneration.lowercased()).\(action)"
        XCTAssertLessThanOrEqual(name.utf8.count, 128)
        XCTAssertEqual(name.withCString { notify_post($0) }, 0)
        phase("posted-\(action)") // Posting success is not delivery proof; the visible acknowledgement is required.
    }
    private func beginReview(_ action: String) -> (began: Double, received: Double) {
        let code: String
        switch action { case "begin-countdown-review": code = "c"; case "begin-feedback-review": code = "f"
        default: preconditionFailure("No reverse acknowledgement for this action") }
        XCTAssertEqual(app.staticTexts["fixture-generation"].label, fixtureGeneration)
        XCTAssertTrue(posted.insert(action).inserted, "No control replays")
        let receiver = ReviewAcknowledgement()
        // XCTest runs this even when continueAfterFailure=false aborts the original test method.
        addTeardownBlock { () async in await receiver.close() }
        let reverse = "zui.ack.\(fixtureLaunch).\(fixtureGeneration.lowercased()).\(acknowledgementNonce).\(code)"
        receiver.register(reverse)
        let name = "test.example.zerus.ui.\(fixtureLaunch).\(fixtureGeneration.lowercased()).\(action)"
        XCTAssertLessThanOrEqual(name.utf8.count, 128)
        let began = ProcessInfo.processInfo.systemUptime // Before post: a conservative early-window upper bound.
        XCTAssertEqual(name.withCString { notify_post($0) }, 0)
        receiver.wait() // No AX polling before the early input/assertion.
        let received = receiver.receivedAt ?? -1
        XCTAssertGreaterThanOrEqual(received, began)
        phase("reverse-ack-\(action)")
        return (began, received)
    }
    private func ack(_ action: String, serial: Int) -> Double {
        let status = app.staticTexts["fixture-control-status"]
        let seen = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label BEGINSWITH %@", "ack|\(action)|\(serial)|"), object: status)
        XCTAssertEqual(XCTWaiter.wait(for: [seen], timeout: 2), .completed)
        let fields = status.label.split(separator: "|")
        guard fields.count == 4, let stamp = Double(fields[3]), stamp.isFinite, stamp > 0 else {
            XCTFail("Missing exact control acknowledgement"); return 0
        }
        XCTAssertEqual(app.staticTexts["fixture-generation"].label, fixtureGeneration)
        phase("ack-\(action)")
        return stamp
    }
    private func closeFixture() {
        defer { app?.terminate() }
        guard let app, app.state != .notRunning else { XCTFail("Fixture exited before checked control cleanup"); return }
        if app.state != .runningForeground { app.activate() }
        if app.buttons["Cancel"].exists { app.buttons["Cancel"].tap() }
        XCTAssertTrue(app.buttons["Close controls"].exists)
        app.buttons["Close controls"].tap()
        let closed = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'closed'"), object: app.staticTexts["fixture-control-status"])
        XCTAssertEqual(XCTWaiter.wait(for: [closed], timeout: 2), .completed, "Owned registration cleanup must be acknowledged")
        phase("closed")
    }
    private var send: XCUIElement { app.buttons["Send answer"] }
    private func choice(_ label: String) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", label)).firstMatch
    }
    private func count(_ value: Int, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(app.staticTexts["callback-count"].label, "Callbacks: \(value)", file: file, line: line)
    }
    private func reviewed(file: StaticString = #filePath, line: UInt = #line) {
        let ready = XCTNSPredicateExpectation(predicate: NSPredicate(format: "enabled == true"), object: send)
        XCTAssertEqual(XCTWaiter.wait(for: [ready], timeout: 5), .completed, file: file, line: line)
    }
    private func broad() {
        choice("Approve for this session").tap(); reviewed(); send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 2))
        XCTAssertTrue(app.staticTexts["“Approve for this session” changes the agent's permissions beyond this request.\n\nExact command for this session only"].exists)
        count(0)
    }
    private func cancelBroaderConfirmation() {
        let confirm = app.buttons["Confirm"]
        XCTAssertTrue(confirm.exists)
        let disclosure = "“Approve for this session” changes the agent's permissions beyond this request.\n\nExact command for this session only"
        XCTAssertTrue(app.staticTexts[disclosure].exists)
        if app.buttons["Cancel"].exists {
            app.buttons["Cancel"].tap()
        } else {
            // Adaptive native popovers can omit the cancel-role action. Only this positively identified
            // presentation may be dismissed through the fixture's passive header, never a guessed point.
            XCTAssertEqual(app.popovers.count, 1, "Unqualified native confirmation presentation")
            let popover = app.popovers.element(boundBy: 0)
            XCTAssertTrue(popover.buttons["Confirm"].exists && popover.staticTexts[disclosure].exists)
            let anchor = app.staticTexts["fixture-dismiss-anchor"]
            XCTAssertTrue(anchor.exists && anchor.isHittable)
            XCTAssertTrue(["regular", "compact"].contains(anchor.value as? String ?? ""), "Missing actual fixture size class")
            let popup = popover.frame, frame = anchor.frame
            let windows = app.windows.allElementsBoundByIndex.filter { $0.exists && $0.frame.contains(popup) && $0.frame.contains(frame) }
            XCTAssertEqual(windows.count, 1, "Native dialog and passive anchor must share one owned app window")
            XCTAssertTrue([popup.minX, popup.minY, popup.width, popup.height, frame.minX, frame.minY, frame.width, frame.height].allSatisfy { $0.isFinite })
            XCTAssertTrue(popup.width > 0 && popup.height > 0 && frame.width > 0 && frame.height > 0 && !popup.intersects(frame))
            phase("cancel-popover-\(popup)-anchor-\(frame)-trait-\(anchor.value as? String ?? "unknown")")
            XCTAssertTrue(confirm.exists && anchor.isHittable)
            anchor.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        }
        let retired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: confirm)
        XCTAssertEqual(XCTWaiter.wait(for: [retired], timeout: 2), .completed)
    }
    func testRealCountdownAndOneTimeChoice() {
        defer { closeFixture() }
        launchFixture(.countdown)
        choice("Approve once").tap()
        let review = beginReview("begin-countdown-review")
        XCTAssertFalse(send.isEnabled); count(0)
        let earlyEnded = ProcessInfo.processInfo.systemUptime
        let earlyElapsed = earlyEnded - review.began
        phase("early-countdown-end-\(earlyElapsed)")
        XCTAssertGreaterThanOrEqual(earlyElapsed, 0)
        XCTAssertLessThan(earlyElapsed, 3, "Fixture missed the real early-review window; no countdown conclusion")
        let appBegan = ack("begin-countdown-review", serial: 0)
        XCTAssertGreaterThanOrEqual(appBegan, review.began); XCTAssertLessThanOrEqual(appBegan, review.received)
        reviewed(); send.tap()
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        XCTAssertTrue(app.staticTexts["last-callback"].label.contains("\"selected_option_ids\":[\"1\"]"))
    }
    func testBroaderCancelThenSeparateConfirm() {
        defer { closeFixture() }
        launchFixture()
        broad(); cancelBroaderConfirmation(); count(0)
        XCTAssertTrue(send.exists); send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 2)); count(0)
        app.buttons["Confirm"].tap()
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        XCTAssertTrue(app.staticTexts["last-callback"].label.contains("\"selected_option_ids\":[\"2\"]"))
    }
    func testReplacementRetiresAnOpenConfirmation() {
        defer { closeFixture() }
        launchFixture(.replacement)
        choice("Approve for this session").tap(); reviewed()
        send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 1)); count(0)
        post("replace")
        let retired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["Confirm"])
        XCTAssertEqual(XCTWaiter.wait(for: [retired], timeout: 4), .completed)
        _ = ack("replace", serial: 1)
        count(0); XCTAssertFalse(send.isEnabled)
    }
    func testUnavailableRetiresAnOpenConfirmation() {
        defer { closeFixture() }
        launchFixture()
        choice("Approve for this session").tap(); reviewed()
        app.buttons["Unavailable in 2 seconds"].tap(); send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 1)); count(0)
        let retired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["Confirm"])
        XCTAssertEqual(XCTWaiter.wait(for: [retired], timeout: 4), .completed)
        count(0); XCTAssertFalse(send.isEnabled)
    }
    func testFeedbackKeyboardCannotBypassReview() {
        defer { closeFixture() }
        launchFixture(.feedback)
        let field = app.textFields["Reject with feedback"]
        XCTAssertTrue(field.exists)
        XCTAssertEqual(field.value as? String, "Use a safer comman")
        field.tap()
        let review = beginReview("begin-feedback-review")
        phase("early-feedback-return-begin")
        field.typeText("d\n")
        let earlyElapsed = ProcessInfo.processInfo.systemUptime - review.began
        phase("early-feedback-return-end-\(earlyElapsed)")
        XCTAssertGreaterThanOrEqual(earlyElapsed, 0)
        XCTAssertLessThan(earlyElapsed, 3, "Fixture missed the real early-review window; no bypass conclusion")
        let appBegan = ack("begin-feedback-review", serial: 0)
        XCTAssertGreaterThanOrEqual(appBegan, review.began); XCTAssertLessThanOrEqual(appBegan, review.received)
        count(0)
        reviewed(); field.typeText("\n")
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        let answer = app.staticTexts["last-callback"].label
        XCTAssertTrue(answer.contains("\"selected_option_ids\":[]")); XCTAssertTrue(answer.contains("Use a safer command"))
        XCTAssertEqual(answer, "fixture-hash-0|[{\"question_id\":\"approval\",\"selected_option_ids\":[],\"text\":\"Use a safer command\"}]")
    }
    func testBackgroundClosesConfirmationWithoutSending() {
        defer { closeFixture() }
        launchFixture()
        broad(); let generation = app.staticTexts["fixture-generation"].label
        XCUIDevice.shared.press(.home)
        app.activate()
        XCTAssertTrue(app.staticTexts["Synthetic UI only — no native delivery"].waitForExistence(timeout: 3))
        XCTAssertEqual(app.staticTexts["fixture-generation"].label, generation, "Fixture was relaunched instead of resumed")
        XCTAssertFalse(app.buttons["Confirm"].exists); count(0)
        XCTAssertFalse(send.isEnabled)
    }
    func testHiddenAndReadOnlyCardsNeverSubmit() {
        defer { closeFixture() }
        launchFixture()
        choice("Approve once").tap(); reviewed()
        app.buttons["Hide card"].tap(); count(0); XCTAssertFalse(send.exists)
        app.buttons["Show card"].tap(); XCTAssertFalse(send.isEnabled); count(0)
        app.buttons["Read-only card"].tap()
        XCTAssertTrue(app.staticTexts["Synthetic request is no longer current"].exists)
        XCTAssertFalse(choice("Approve once").isEnabled); XCTAssertFalse(send.exists); count(0)
    }
}
