import XCTest
@testable import ZerusKit

/// Ported from Android QuestionStepsTest.
final class QuestionStepsTests: XCTestCase {
    private let prompts = ["first", "second"].map {
        Prompt(id: $0, text: "Question \($0)", choices: [Choice(id: "yes", label: "Yes", description: "")], multi: false, required: true, other: true,
               header: "", body: "", otherLabel: "", otherDescription: "")
    }

    private func step(_ page: Int, _ options: [String: Set<String>] = [:], _ texts: [String: String] = [:]) -> QuestionStep {
        QuestionSteps.current(prompts, page: page, options: options, texts: texts)
    }

    func testAnsweringTheFirstQuestionEnablesNavigationBeforeTheWholeRequestIsComplete() {
        XCTAssertFalse(step(0).canAdvance)
        let next = step(0, ["first": ["yes"]])
        XCTAssertEqual(next.nextPage, 1)
        XCTAssertTrue(next.canAdvance)
        XCTAssertFalse(next.complete)
        XCTAssertFalse(next.reviewing)
    }

    func testFinalSubmissionIsOfferedOnlyAtTheLastCompleteQuestion() {
        let answers = Dictionary(uniqueKeysWithValues: prompts.map { ($0.id, Set(["yes"])) })
        XCTAssertEqual(step(0, answers).nextPage, 1)
        let final = step(1, answers)
        XCTAssertNil(final.nextPage)
        XCTAssertTrue(final.complete)
    }

    func testBrowsingToTheLastQuestionReturnsToAnEarlierMissingOrInvalidAnswer() {
        let review = step(1, ["second": ["yes"]])
        XCTAssertEqual(review.nextPage, 0)
        XCTAssertTrue(review.canAdvance)
        XCTAssertTrue(review.reviewing)
        XCTAssertFalse(review.complete)
        XCTAssertEqual(step(1, Dictionary(uniqueKeysWithValues: prompts.map { ($0.id, Set(["yes"])) }), ["first": "/invalid"]).nextPage, 0)
    }

    func testTheCurrentAnswerMustBeValidButAnUnseenInvalidAnswerDoesNotBlockNavigation() {
        XCTAssertFalse(step(0, ["first": ["changed"]]).canAdvance)
        XCTAssertFalse(step(0, [:], ["first": "/command"]).canAdvance)
        XCTAssertTrue(step(0, [:], ["first": "Custom answer", "second": "/invalid"]).canAdvance)
        XCTAssertFalse(step(1, [:], ["first": "Custom answer", "second": "/invalid"]).complete)
    }

    func testARestoredPageIsBoundedAndAnEmptyRequestIsNeverComplete() {
        XCTAssertEqual(step(-10).page, 0)
        XCTAssertEqual(step(20).page, 1)
        let empty = QuestionSteps.current([], page: 20, options: [:], texts: [:])
        XCTAssertEqual(empty.page, 0)
        XCTAssertNil(empty.nextPage)
        XCTAssertFalse(empty.complete)
        XCTAssertFalse(empty.canAdvance)
    }

    func testALiteralFormTextIsAValidAnswerForItsOwnStep() {
        var literal = prompts
        literal[0].literalText = true
        XCTAssertTrue(QuestionSteps.current(literal, page: 0, options: [:], texts: ["first": "/deploy"]).canAdvance)
    }
}
