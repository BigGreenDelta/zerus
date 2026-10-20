import XCTest

/// Native SwiftUI tests of the unchanged production QuestionCard. The application is a separate
/// simulator-only memory host; callback assertions do not prove provider, relay or persistence delivery.
@MainActor final class ApprovalChoicesUITests: XCTestCase {
    private var app: XCUIApplication!
    @MainActor private func launchFixture() {
        continueAfterFailure = false
        app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.staticTexts["Synthetic UI only — no native delivery"].waitForExistence(timeout: 5))
        count(0)
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
        XCTAssertTrue(app.staticTexts["Exact command for this session only"].exists)
        count(0)
    }
    func testRealCountdownAndOneTimeChoice() {
        defer { app?.terminate() }
        launchFixture()
        choice("Approve once").tap()
        XCTAssertFalse(send.isEnabled); count(0)
        reviewed(); send.tap()
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        XCTAssertTrue(app.staticTexts["last-callback"].label.contains("\"selected_option_ids\":[\"1\"]"))
    }
    func testBroaderCancelThenSeparateConfirm() {
        defer { app?.terminate() }
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
        defer { app?.terminate() }
        launchFixture()
        choice("Approve for this session").tap(); reviewed()
        app.buttons["Replace in 2 seconds"].tap(); send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 1)); count(0)
        let retired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["Confirm"])
        XCTAssertEqual(XCTWaiter.wait(for: [retired], timeout: 4), .completed)
        count(0); XCTAssertFalse(send.isEnabled)
    }
    func testUnavailableRetiresAnOpenConfirmation() {
        defer { app?.terminate() }
        launchFixture()
        choice("Approve for this session").tap(); reviewed()
        app.buttons["Unavailable in 2 seconds"].tap(); send.tap()
        XCTAssertTrue(app.buttons["Confirm"].waitForExistence(timeout: 1)); count(0)
        let retired = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["Confirm"])
        XCTAssertEqual(XCTWaiter.wait(for: [retired], timeout: 4), .completed)
        count(0); XCTAssertFalse(send.isEnabled)
    }
    func testFeedbackKeyboardCannotBypassReview() {
        defer { app?.terminate() }
        launchFixture()
        let field = app.textFields["Reject with feedback"]
        XCTAssertTrue(field.exists); field.tap(); field.typeText("Use a safer command\n")
        count(0)
        reviewed(); field.typeText("\n")
        let recorded = XCTNSPredicateExpectation(predicate: NSPredicate(format: "label == 'Callbacks: 1'"), object: app.staticTexts["callback-count"])
        XCTAssertEqual(XCTWaiter.wait(for: [recorded], timeout: 3), .completed)
        let answer = app.staticTexts["last-callback"].label
        XCTAssertTrue(answer.contains("\"selected_option_ids\":[]")); XCTAssertTrue(answer.contains("Use a safer command"))
    }
    func testBackgroundClosesConfirmationWithoutSending() {
        defer { app?.terminate() }
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
        defer { app?.terminate() }
        launchFixture()
        choice("Approve once").tap(); reviewed()
        app.buttons["Hide card"].tap(); count(0); XCTAssertFalse(send.exists)
        app.buttons["Show card"].tap(); XCTAssertFalse(send.isEnabled); count(0)
        app.buttons["Read-only card"].tap()
        XCTAssertTrue(app.staticTexts["Synthetic request is no longer current"].exists)
        XCTAssertFalse(choice("Approve once").isEnabled); XCTAssertFalse(send.exists); count(0)
    }
}
