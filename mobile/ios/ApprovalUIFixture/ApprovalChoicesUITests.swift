import XCTest
import Darwin

/// Native SwiftUI tests of the unchanged production QuestionCard. The application is a separate
/// simulator-only memory host; callback assertions do not prove provider, relay or persistence delivery.
@MainActor final class ApprovalChoicesUITests: XCTestCase {
    private var app: XCUIApplication!
    private enum Mode: String { case normal, feedback, replacement }
    private var fixtureLaunch = ""
    private var fixtureGeneration = ""
    private var posted: Set<String> = []
    @MainActor private func launchFixture(_ mode: Mode = .normal) {
        continueAfterFailure = false
        fixtureLaunch = UUID().uuidString.lowercased(); posted = []
        app = XCUIApplication()
        app.launchArguments = ["--zerus-fixture-launch", fixtureLaunch, "--zerus-fixture-mode", mode.rawValue]
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
        XCTAssertTrue(["begin-feedback-review", "replace"].contains(action))
        XCTAssertTrue(posted.insert(action).inserted, "No control replays")
        XCTAssertEqual(app.staticTexts["fixture-generation"].label, fixtureGeneration)
        let name = "test.example.zerus.ui.\(fixtureLaunch).\(fixtureGeneration.lowercased()).\(action)"
        XCTAssertLessThanOrEqual(name.utf8.count, 128)
        XCTAssertEqual(name.withCString { notify_post($0) }, 0)
        phase("posted-\(action)") // Posting success is not delivery proof; the visible acknowledgement is required.
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
    func testRealCountdownAndOneTimeChoice() {
        defer { closeFixture() }
        launchFixture()
        choice("Approve once").tap()
        XCTAssertFalse(send.isEnabled); count(0)
        reviewed(); send.tap()
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        XCTAssertTrue(app.staticTexts["last-callback"].label.contains("\"selected_option_ids\":[\"1\"]"))
    }
    func testBroaderCancelThenSeparateConfirm() {
        defer { closeFixture() }
        launchFixture()
        broad(); app.buttons["Cancel"].tap(); count(0)
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
        post("begin-feedback-review")
        let reviewBegan = ack("begin-feedback-review", serial: 0)
        phase("early-feedback-return-begin")
        field.typeText("d\n")
        let earlyElapsed = ProcessInfo.processInfo.systemUptime - reviewBegan
        phase("early-feedback-return-end-\(earlyElapsed)")
        XCTAssertGreaterThanOrEqual(earlyElapsed, 0)
        XCTAssertLessThan(earlyElapsed, 3, "Fixture missed the real early-review window; no bypass conclusion")
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
