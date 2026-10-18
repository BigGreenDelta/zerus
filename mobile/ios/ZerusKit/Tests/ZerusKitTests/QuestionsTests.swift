import XCTest
@testable import ZerusKit

/// Ported from Android QuestionPoliciesTest, QuestionTrackingTest, QuestionSendConfirmationTest,
/// QuestionReplyPresentationTest, QuestionSubmitGateTest, QuestionDisplayStateTest and ApprovalReviewTest.
final class QuestionPoliciesTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let prompt = Prompt(id: "q", text: "Question", choices: [Choice(id: "first", label: "First", description: ""), Choice(id: "second", label: "Second", description: "")],
                                multi: false, required: true, other: false, header: "", body: "", otherLabel: "", otherDescription: "")
    private func question(_ id: String = "request", optional: Bool = false, canAnswer: Bool = true, at: Double = 1) -> Question {
        Question(id: id, hash: "hash-\(id)", prompts: [prompt], canAnswer: canAnswer, canSkip: true, optional: optional, delivery: "",
                 createdAt: at, run: target.run, conversation: target.conversation)
    }
    private func submitted() -> Question {
        var q = question(optional: true)
        q.source = "codex_async"
        q.delivery = "submitted"
        q.answerDelivery = NativeQuestionDelivery(requestId: "uuid", questionId: q.id, questionHash: q.hash, run: target.run, conversation: target.conversation,
                                                  status: "submitted", answers: [NativeQuestionAnswer(questionId: "q", optionIds: ["second"], text: "", skip: false)])
        return q
    }

    func testParserPreservesFullDisclosureAndDefaultsOtherFalse() throws {
        let body = String(repeating: "dangerous command\n", count: 300)
        let raw: JSON = ["pending_questions": [["question_id": "id", "question_hash": "hash", "approval": true, "trust_request": true,
            "created_at": 123.0, "source": "claude_tool_approval", "answer_unavailable_reason": "Exact native reason", "run_id": "r",
            "conversation_id": "c", "tool_call_id": "tool",
            "questions": [["id": "q", "question": "Approve?", "header": "Header", "body": .string(body), "other_label": "Custom",
                           "other_description": "Native custom disclosure"]]]]]
        let parsed = try XCTUnwrap(NativeEvents.questions(raw.object!).first)
        XCTAssertEqual(body, parsed.prompts.first?.body)
        XCTAssertEqual("Header", parsed.prompts.first?.header)
        XCTAssertEqual("Native custom disclosure", parsed.prompts.first?.otherDescription)
        XCTAssertEqual(false, parsed.prompts.first?.other)
        XCTAssertTrue(parsed.approval)
        XCTAssertTrue(parsed.trustRequest)
        XCTAssertEqual("Exact native reason", parsed.unavailableReason)
        XCTAssertEqual(123, parsed.createdAt)
        XCTAssertEqual("tool", parsed.toolCallId)
    }

    func testParserPreservesTypedSubmittedAnswerInNativeOrder() throws {
        let q = submitted()
        let delivery: JSON = ["request_id": "uuid", "question_id": .string(q.id), "question_hash": .string(q.hash), "run_id": .string(target.run),
                              "conversation_id": .string(target.conversation), "status": "submitted",
                              "answers": [["question_id": "q", "selected_option_ids": ["second", "first"], "text": "literal"]]]
        let parsed = try XCTUnwrap(NativeEvents.questions(JSON.object(["pending_questions": [["answer_delivery": delivery]]]).object!).first)
        XCTAssertEqual(["second", "first"], parsed.answerDelivery?.answers.first?.optionIds)
        XCTAssertEqual("literal", parsed.answerDelivery?.answers.first?.text)
    }

    func testRequiredActionableRequestTakesPriorityOverOptionalAndUnavailable() {
        let unavailable = question("unavailable", canAnswer: false)
        let required = question("required")
        let optional = question("optional", optional: true)
        XCTAssertEqual(required, QuestionPolicies.queue([optional, unavailable, required], target, chosen: QuestionPolicies.key(optional)).selected)
    }

    func testOptionalNewestFirstSelectionSurvivesPollInsertion() {
        let old = question("old", optional: true, at: 1)
        let chosen = question("chosen", optional: true, at: 2)
        let fresh = question("new", optional: true, at: 3)
        let queue = QuestionPolicies.queue([old, fresh, chosen], target, chosen: QuestionPolicies.key(chosen))
        XCTAssertEqual(chosen, queue.selected)
        XCTAssertEqual([fresh, chosen, old], queue.optional)
        XCTAssertEqual(1, queue.index)
    }

    func testSubmittedMatchingRequestIsHiddenFromOfferedCountAndRetainsAnswer() {
        let sent = submitted()
        let next = question("next", optional: true)
        let queue = QuestionPolicies.queue([sent, next], target, chosen: "")
        XCTAssertEqual(next, queue.selected)
        XCTAssertEqual(1, queue.promptCount)
        XCTAssertEqual([sent], queue.submitted)
        XCTAssertEqual(["second"], QuestionPolicies.submitted(sent, target)?.answers.first?.optionIds)
    }

    func testNativeSubmissionMustMatchEveryIdentityField() {
        let sent = submitted()
        let delivery = sent.answerDelivery!
        let changes: [(inout NativeQuestionDelivery) -> Void] = [{ $0.questionId = "other" }, { $0.questionHash = "other" }, { $0.run = "other" },
                                                                 { $0.conversation = "other" }, { $0.status = "uncertain" }]
        for change in changes {
            var changed = delivery
            change(&changed)
            var q = sent
            q.answerDelivery = changed
            XCTAssertNil(QuestionPolicies.submitted(q, target))
        }
        XCTAssertNil(QuestionPolicies.submitted(sent, target.with(run: "reused")))
        XCTAssertNil(QuestionPolicies.submitted(sent, target.with(conversation: "new")))
        var hook = sent
        hook.source = "hook"
        XCTAssertNil(QuestionPolicies.submitted(hook, target))
        var required = sent
        required.optional = false
        XCTAssertNil(QuestionPolicies.submitted(required, target))
    }

    func testQuestionHashReuseDoesNotSelectReplacedOptionalRequest() {
        let old = question("same", optional: true)
        var replacement = old
        replacement.hash = "replacement"
        let newest = question("newest", optional: true, at: 5)
        XCTAssertEqual(newest, QuestionPolicies.queue([replacement, newest], target, chosen: QuestionPolicies.key(old)).selected)
    }

    func testDuplicateIdsAndEmptyQuestionsAreNotOfferedTwice() {
        let q = question()
        var blank = q
        blank.id = ""
        var empty = q
        empty.id = "empty"
        empty.prompts = []
        XCTAssertEqual(1, QuestionPolicies.queue([q, q, blank, empty], target, chosen: "").promptCount)
    }

    func testMissingOptionsNeverCreateUnsupportedFreeformAnswer() {
        var unsupported = prompt
        unsupported.choices = []
        XCTAssertFalse(QuestionPolicies.complete([unsupported], options: [:], texts: ["q": "invented"]))
        XCTAssertFalse(QuestionPolicies.answerError([unsupported], options: [:], texts: ["q": "invented"]).isEmpty)
        XCTAssertEqual("", QuestionPolicies.answers([unsupported], options: [:], texts: ["q": "invented"]).array?.first?.string("text"))
    }

    func testMultiAnswersUseSourceOptionOrder() {
        var multi = prompt
        multi.multi = true
        let result = QuestionPolicies.answers([multi], options: ["q": ["second", "first"]], texts: [:])
        XCTAssertEqual(JSON(["first", "second"]), result.array?.first?.value("selected_option_ids"))
    }

    func testCustomAnswerByteAndCommandRulesMatchNative() {
        var p = prompt
        p.other = true
        XCTAssertFalse(QuestionPolicies.answerError([p], options: [:], texts: ["q": String(repeating: "é", count: 2049)]).isEmpty)
        XCTAssertFalse(QuestionPolicies.answerError([p], options: [:], texts: ["q": "/command"]).isEmpty)
        XCTAssertFalse(QuestionPolicies.answerError([p], options: [:], texts: ["q": "a\nb"]).isEmpty)
        XCTAssertTrue(QuestionPolicies.complete([p], options: [:], texts: ["q": "literal <tag>"]))
    }

    func testApprovalRequiresExactNativeIds() {
        var approval = question()
        approval.approval = true
        XCTAssertFalse(QuestionPolicies.approvalSupported(approval))
        var choices = prompt
        choices.choices = [Choice(id: "allow", label: "Yes", description: ""), Choice(id: "deny", label: "No", description: "")]
        approval.prompts = [choices]
        XCTAssertTrue(QuestionPolicies.approvalSupported(approval))
        // Approve and Deny keep sending exactly today's row.
        XCTAssertEqual(QuestionPolicies.approvalAnswers(approval, "allow")?.compact,
                       JSON.array([["question_id": .string("q"), "selected_option_ids": [.string("allow")], "text": ""]]).compact)
        XCTAssertEqual(QuestionPolicies.approvalAnswers(approval, "deny"), [["question_id": "q", "selected_option_ids": ["deny"], "text": ""]])
        approval.prompts = []
        XCTAssertNil(QuestionPolicies.approvalAnswers(approval, "allow"))
    }

    /// One native card with one item, as the computer reports it.
    private func card(_ source: String = "opencode_native", approval: Bool = false, canAnswer: Bool = true, items: [JSONObject]) throws -> Question {
        try XCTUnwrap(NativeEvents.questions(["pending_questions": [["question_id": "card", "question_hash": "hash", "can_answer": .bool(canAnswer),
                                                                     "approval": .bool(approval), "source": .string(source), "run_id": "run",
                                                                     "conversation_id": "conversation", "questions": .array(items.map(JSON.object))]]]).first)
    }

    func testCommandLikeAnswersStayOnTheComputerOutsideOpenCodeForms() throws {
        let item: JSONObject = ["id": "q", "question": "Name", "options": [["id": "a", "label": "A"]], "allow_other": true]
        let command = "Enter command-like answers on your computer."
        for source in ["codex_async", "codex_tui", "claude_tool_approval", "claude_tui", "kimi_wire", "kimi_cache_hint", "dsh_native", "startup_terminal_choice", ""] {
            for approval in [false, true] {
                let question = try card(source, approval: approval, items: [item])
                for text in ["/command", "!command", "  /padded"] {
                    XCTAssertEqual(QuestionPolicies.answerError(question.prompts, options: [:], texts: ["q": text]), command, "\(source) \(text)")
                    XCTAssertFalse(QuestionPolicies.complete(question.prompts, options: [:], texts: ["q": text]), "\(source) \(text)")
                }
            }
        }
        // An OpenCode permission is an approval, not a form: it keeps the rule.
        let permission = try card(approval: true, items: [item])
        XCTAssertEqual(QuestionPolicies.answerError(permission.prompts, options: [:], texts: ["q": "/command"]), command)
        // An OpenCode form sends its text through the native form reply: the text item and Other text on a choice item.
        let choice = try card(items: [item.putting("input_kind", "choice")])
        let text = try card(items: [["id": "q", "header": "Path", "prompt": "Path", "options": [], "allow_other": true, "input_kind": "text"]])
        for question in [choice, text] {
            for value in ["/tmp/report.txt", "!important", " /padded"] {
                XCTAssertEqual(QuestionPolicies.answerError(question.prompts, options: [:], texts: ["q": value]), "", value)
                XCTAssertTrue(QuestionPolicies.complete(question.prompts, options: [:], texts: ["q": value]), value)
                XCTAssertEqual(QuestionPolicies.answers(question.prompts, options: [:], texts: ["q": value]).array?.first?.string("text"), value)
            }
            // Every other text rule still applies.
            XCTAssertEqual(QuestionPolicies.answerError(question.prompts, options: [:], texts: ["q": "/a\nb"]), "Answers must be one line and at most 4096 UTF-8 bytes.")
            XCTAssertEqual(QuestionPolicies.answerError(question.prompts, options: [:], texts: ["q": "/" + String(repeating: "é", count: 2048)]),
                           "Answers must be one line and at most 4096 UTF-8 bytes.")
        }
        let closed = try card(items: [item.putting("allow_other", false).putting("input_kind", "choice")])
        XCTAssertEqual(QuestionPolicies.answerError(closed.prompts, options: [:], texts: ["q": "/purple"]), "This request does not accept an Other answer.")
    }

    private let permissionItem: JSONObject = ["id": "permission", "header": "Permission", "prompt": "shell\nprintf native-tool-output", "multi_select": false,
                                              "allow_other": false, "required": true,
                                              "options": [["id": "once", "label": "Allow once", "scope": "once"], ["id": "always", "label": "Always allow", "scope": "broader"],
                                                          ["id": "reject", "label": "Reject", "description": "Rejects every pending request in this session", "scope": "once"]]]

    private func options(_ ids: [String]) -> JSON { .array(ids.map { ["id": .string($0), "label": .string($0)] }) }

    func testOnlyAnswerableOpenCodePermissionsUseTheirNativeChoices() throws {
        let permission = try card(approval: true, items: [permissionItem])
        XCTAssertTrue(QuestionPolicies.openCodePermission(permission))
        XCTAssertFalse(QuestionPolicies.approvalSupported(permission), "allow/deny approvals keep their own rule")
        XCTAssertTrue(QuestionPolicies.openCodePermission(try card(approval: true, items: [permissionItem.putting("options", options(["once", "reject"]))])),
                      "Always allow is optional")
        XCTAssertNil(PermissionConfirmation(try card(approval: true, items: [permissionItem.putting("options", options(["reject", "once"]))])),
                     "no confirmation without Always allow")

        // A Claude card with native approval choices stays unanswerable on phones, exactly as today.
        let claude = try card("claude_tool_approval", approval: true, items: [["id": "approval", "question": "Run?", "header": "Bash", "allow_other": false,
                                                                              "options": [["id": "1", "label": "Yes", "scope": "once"],
                                                                                          ["id": "2", "label": "Yes, and don't ask again", "scope": "broader"],
                                                                                          ["id": "3", "label": "No", "scope": "once"]]]])
        XCTAssertFalse(QuestionPolicies.openCodePermission(claude))
        XCTAssertFalse(QuestionPolicies.approvalSupported(claude))
        XCTAssertNil(PermissionConfirmation(claude))
        // An allow/deny card behaves exactly as today, whatever its source.
        let allowDeny = try card(approval: true, items: [permissionItem.putting("options", options(["allow", "deny"]))])
        XCTAssertFalse(QuestionPolicies.openCodePermission(allowDeny))
        XCTAssertTrue(QuestionPolicies.approvalSupported(allowDeny))

        let refused: [(String, Question)] = [
            ("another agent", try card("claude_tool_approval", approval: true, items: [permissionItem])),
            ("a form", try card(items: [permissionItem])),
            ("not answerable", try card(approval: true, canAnswer: false, items: [permissionItem])),
            ("another item id", try card(approval: true, items: [permissionItem.putting("id", "approval")])),
            ("a multi prompt", try card(approval: true, items: [permissionItem.putting("multi_select", true)])),
            ("a second prompt", try card(approval: true, items: [permissionItem, permissionItem.putting("id", "second")])),
            ("an extra option", try card(approval: true, items: [permissionItem.putting("options", options(["once", "always", "reject", "allow"]))])),
            ("an unknown option", try card(approval: true, items: [permissionItem.putting("options", options(["once", "reject", "later"]))])),
            ("no once", try card(approval: true, items: [permissionItem.putting("options", options(["always", "reject"]))])),
            ("no reject", try card(approval: true, items: [permissionItem.putting("options", options(["once", "always"]))])),
            ("no prompt", try card(approval: true, items: [])),
        ]
        for (reason, question) in refused {
            XCTAssertFalse(QuestionPolicies.openCodePermission(question), reason)
            XCTAssertNil(PermissionConfirmation(question), reason)
        }
    }

    func testPermissionButtonsMapToTheirNativeOptions() throws {
        let permission = try card(approval: true, items: [permissionItem])
        XCTAssertEqual(PermissionAction.actions(permission), [.reject, .allowOnce, .alwaysAllow])
        XCTAssertEqual(PermissionAction.actions(permission).map(\.option), ["reject", "once", "always"])
        XCTAssertEqual(PermissionAction.actions(try card(approval: true, items: [permissionItem.putting("options", options(["once", "reject"]))])),
                       [.reject, .allowOnce], "Always allow… only when offered")
        XCTAssertEqual(PermissionAction.actions(try card(approval: true, items: [permissionItem.putting("options", options(["allow", "deny"]))])), [])
        XCTAssertEqual(PermissionAction.actions(try card(approval: true, canAnswer: false, items: [permissionItem])), [])
        XCTAssertEqual(PermissionAction.reject.label(remaining: 2), "Reject")
        XCTAssertEqual(PermissionAction.allowOnce.label(remaining: 2), "Allow once (2)")
        XCTAssertEqual(PermissionAction.allowOnce.label(remaining: 0), "Allow once")
        XCTAssertEqual(PermissionAction.alwaysAllow.label(remaining: 2), "Always allow…")
        // Reject is enabled like Deny; Allow once and Always allow… also wait for this card's review.
        XCTAssertTrue(PermissionAction.reject.enabled(answerable: true, reviewed: false))
        XCTAssertFalse(PermissionAction.reject.enabled(answerable: false, reviewed: true))
        for action in [PermissionAction.allowOnce, .alwaysAllow] {
            XCTAssertFalse(action.enabled(answerable: true, reviewed: false), "\(action)")
            XCTAssertFalse(action.enabled(answerable: false, reviewed: true), "\(action)")
            XCTAssertTrue(action.enabled(answerable: true, reviewed: true), "\(action)")
        }
        // Only Always allow asks for a confirmation, and only it is broader than this request.
        XCTAssertEqual(PermissionAction.allCases.filter(\.confirmation), [.alwaysAllow])
        XCTAssertEqual(PermissionConfirmation(permission)?.answers(permission, editing: true, available: true),
                       QuestionPolicies.approvalAnswers(permission, PermissionAction.alwaysAllow.option))
    }

    func testAlwaysAllowConfirmationNamesThePatternsItSavesVerbatim() throws {
        let general = "OpenCode will allow matching requests without asking again."
        func described(_ text: String) throws -> Question {
            try card(approval: true, items: [permissionItem.putting("options", [
                ["id": "once", "label": "Allow once", "scope": "once"],
                ["id": "always", "label": "Always allow", "description": .string(text), "scope": "broader"],
                ["id": "reject", "label": "Reject", "description": "Rejects every pending request in this session", "scope": "once"]])])
        }
        let saved = "Allows without asking again: printf *"
        XCTAssertEqual(try XCTUnwrap(PermissionConfirmation(try described(saved))).message, general + "\n\n" + saved)
        let several = "Allows without asking again: git push *, git fetch *"
        XCTAssertEqual(try XCTUnwrap(PermissionConfirmation(try described(several))).message, general + "\n\n" + several)
        // A card from an older computer has no description: the general sentence alone.
        XCTAssertEqual(try XCTUnwrap(PermissionConfirmation(try card(approval: true, items: [permissionItem]))).message, general)
        XCTAssertEqual(try XCTUnwrap(PermissionConfirmation(try described("  "))).message, general)
    }

    /// Confirm revalidates what the Always allow… button needed: this card, answerable and editable, in the foreground,
    /// visible, with its own review complete. A dialog left open across background and resume cannot skip a new review.
    func testAlwaysAllowConfirmsOnlyForAReviewedVisibleForegroundCard() throws {
        let permission = try card(approval: true, items: [permissionItem])
        let confirmation = try XCTUnwrap(PermissionConfirmation(permission))
        let key = ApprovalReview.key(target, permission)
        let always = QuestionPolicies.approvalAnswers(permission, PermissionAction.alwaysAllow.option)
        func confirm(_ review: ApprovalReview, now: Double, foreground: Bool = true, visible: Bool = true, editing: Bool = true, available: Bool = true,
                     on target: Target? = nil, card: Question? = nil) -> JSON? {
            confirmation.confirm(card ?? permission, target: target ?? self.target, editing: editing, available: available, foreground: foreground,
                                 visible: visible, review: review, now: now)
        }
        var review = ApprovalReview().observe(eligible: true, now: 1, card: key)
        XCTAssertNil(confirm(review, now: 2), "the review is still counting")
        // Control: this card, reviewed, visible and in the foreground.
        XCTAssertEqual(confirm(review, now: 4), always)
        XCTAssertNotNil(always)
        XCTAssertNil(confirm(review, now: 4, foreground: false), "not in the foreground")
        XCTAssertNil(confirm(review, now: 4, visible: false), "not visible")
        XCTAssertNil(confirm(review, now: 4, editing: false))
        XCTAssertNil(confirm(review, now: 4, available: false))
        var replaced = permission
        replaced.hash = "replacement"
        XCTAssertNil(confirm(review, now: 4, card: replaced), "another card")

        // Background, then resume with the dialog still open: nothing is confirmed until a new review completes.
        review = review.observe(eligible: false, now: 5, card: key)
        XCTAssertNil(confirm(review, now: 5, foreground: false))
        XCTAssertNil(confirm(review, now: 5), "back in the foreground before the card is observed again")
        review = review.observe(eligible: true, now: 6, card: key)
        XCTAssertNil(confirm(review, now: 6))
        XCTAssertNil(confirm(review, now: 8.9))
        XCTAssertEqual(confirm(review, now: 9), always, "a new review completed")
        // Scrolled out of view and back: the same.
        review = review.observe(eligible: false, now: 10, card: key).observe(eligible: true, now: 10.5, card: key)
        XCTAssertNil(confirm(review, now: 11))
        // A review of another card or on another target never confirms this one.
        var other = permission
        other.id = "other"
        XCTAssertNil(confirm(ApprovalReview().observe(eligible: true, now: 1, card: ApprovalReview.key(target, other)), now: 20))
        XCTAssertNil(confirm(review, now: 20, on: target.with(run: "other")))
        XCTAssertNil(confirm(ApprovalReview(), now: 20), "never reviewed")
    }

    /// Leaving eligibility (background, out of view) resets the review, and a late tick from the stopping clock cannot
    /// re-arm it: after three or more seconds away, Approve once, Allow once and Always allow wait for three new seconds.
    func testReturningAfterTimeAwayNeedsAFullNewReview() throws {
        let allowDeny = try card("claude_tool_approval", approval: true,
                                 items: [permissionItem.putting("id", "approval").putting("options", options(["allow", "deny"]))])
        let permission = try card(approval: true, items: [permissionItem])
        XCTAssertTrue(QuestionPolicies.approvalSupported(allowDeny))
        XCTAssertTrue(QuestionPolicies.openCodePermission(permission))
        let confirmation = try XCTUnwrap(PermissionConfirmation(permission))
        for (label, question) in [("Claude allow/deny", allowDeny), ("OpenCode permission", permission)] {
            let key = ApprovalReview.key(target, question)
            func reviewed(_ review: ApprovalReview, _ now: Double) -> Bool { review.secondsRemaining(now: now, card: key) == 0 }
            var review = ApprovalReview().observe(eligible: true, now: 1, card: key)
            for now in [1.2, 2.5, 4.0] { review = review.tick(now: now, card: key) }
            XCTAssertTrue(reviewed(review, 4), label)
            // The card leaves; the clock that stops may still deliver one more tick.
            review = review.observe(eligible: false, now: 5, card: key).tick(now: 5.2, card: key)
            XCTAssertFalse(reviewed(review, 9), "\(label): time away is not review time")
            // Back after four seconds away: three new eligible seconds before any approving button.
            review = review.observe(eligible: true, now: 9, card: key)
            for now in [9.0, 9.2, 10.4, 11.8] {
                review = review.tick(now: now, card: key)
                XCTAssertFalse(reviewed(review, now), "\(label) at \(now)")
                XCTAssertFalse(PermissionAction.allowOnce.enabled(answerable: true, reviewed: reviewed(review, now)), "\(label) at \(now)")
            }
            review = review.tick(now: 12, card: key)
            XCTAssertTrue(reviewed(review, 12), label)
            if question == permission {
                XCTAssertNil(confirmation.confirm(permission, target: target, editing: true, available: true, foreground: true, visible: true,
                                                  review: review, now: 11.9))
                XCTAssertNotNil(confirmation.confirm(permission, target: target, editing: true, available: true, foreground: true, visible: true,
                                                     review: review, now: 12))
            }
        }
    }

    func testClockTicksNeverStartOrRearmAReview() {
        let a = "a", b = "b"
        XCTAssertEqual(3, ApprovalReview(card: a).tick(now: 10, card: a).secondsRemaining(now: 20, card: a), "a tick never starts a review")
        let started = ApprovalReview().observe(eligible: true, now: 1, card: a)
        XCTAssertEqual(started.tick(now: 2, card: a), started)
        XCTAssertEqual(started.tick(now: 2, card: b), started, "a late tick for another card leaves this review alone")
        XCTAssertEqual(3, started.tick(now: 0.5, card: a).secondsRemaining(now: 10, card: a), "a clock that went back resets it")
        // The defect: observing as eligible after leaving re-armed the review at the moment the card left.
        let left = started.observe(eligible: false, now: 5, card: a)
        XCTAssertEqual(0, left.observe(eligible: true, now: 5.2, card: a).secondsRemaining(now: 9, card: a))
        XCTAssertEqual(3, left.tick(now: 5.2, card: a).secondsRemaining(now: 9, card: a))
    }

    func testAlwaysAllowIsSentOnlyForTheExactCardThatStillOffersIt() throws {
        let permission = try card(approval: true, items: [permissionItem])
        let confirmation = try XCTUnwrap(PermissionConfirmation(permission))
        XCTAssertEqual(confirmation.answers(permission, editing: true, available: true)?.compact,
                       #"[{"question_id":"permission","selected_option_ids":["always"],"text":""}]"#)
        XCTAssertNil(confirmation.answers(permission, editing: false, available: true), "an answer is already being sent or was sent")
        XCTAssertNil(confirmation.answers(permission, editing: true, available: false), "the request must be verified again")
        let changes: [(String, (inout Question) -> Void)] = [
            ("another hash", { $0.hash = "replacement" }),
            ("another request", { $0.id = "other" }),
            ("no longer answerable", { $0.canAnswer = false }),
            ("Always allow withdrawn", { question in question.prompts[0].choices.removeAll { $0.id == "always" } }),
            ("an extra option", { $0.prompts[0].choices.append(Choice(id: "allow", label: "Allow", description: "")) }),
            ("another agent", { $0.source = "claude_tool_approval" }),
            ("no longer an approval", { $0.approval = false }),
        ]
        for (reason, change) in changes {
            var current = permission
            change(&current)
            XCTAssertNil(confirmation.answers(current, editing: true, available: true), reason)
        }
        // Benign polling metadata keeps the same request.
        var polled = permission
        polled.createdAt = 500
        polled.delivery = "refreshed"
        XCTAssertNotNil(confirmation.answers(polled, editing: true, available: true))
    }
}

final class QuestionTrackingTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let delivery = NativeQuestionDelivery(requestId: "receipt", questionId: "question", questionHash: "hash", run: "run", conversation: "conversation",
                                                  status: "submitted", answers: [NativeQuestionAnswer(questionId: "prompt", optionIds: ["allow"], text: "captured", skip: false)])
    private var card: Question {
        Question(id: "question", hash: "hash", prompts: [], canAnswer: true, canSkip: true, optional: true, delivery: "", source: "codex_async",
                 run: "run", conversation: "conversation", answerDelivery: delivery)
    }

    func testUnseenSubmittedAnswerSurvivesRestartAndStaleNativePoll() throws {
        let state = QuestionTracking.remember(MessageState(), target, [card])
        let restored = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(state))
        var stale = card
        stale.answerDelivery = nil
        stale.delivery = ""
        XCTAssertEqual(delivery, QuestionTracking.restore(restored, target, [stale]).first?.answerDelivery)
        XCTAssertEqual("submitted", restored.drafts.first?.status)
    }

    func testOtherConversationOrHashCannotAcquireLock() {
        let state = QuestionTracking.remember(MessageState(), target, [card])
        var changed = card
        changed.hash = "new"
        changed.answerDelivery = nil
        XCTAssertNil(QuestionTracking.restore(state, target, [changed]).first?.answerDelivery)
        XCTAssertTrue(QuestionTracking.remember(MessageState(), target.with(conversation: "new"), [card]).drafts.isEmpty)
    }

    func testUnrelatedUncertainSubmissionIsPreserved() {
        let draft = Draft(target: target, text: "private", status: "uncertain", requestId: "different", questionId: "question", questionHash: "hash", answers: "original")
        XCTAssertEqual([draft], QuestionTracking.remember(MessageState(drafts: [draft]), target, [card]).drafts)
    }

    func testReceiptSettlesOnlyTheExactAttempt() {
        let draft = Draft(target: target, text: "", status: "submitting", requestId: "request", questionId: "question", questionHash: "hash")
        let completed = object(#"{"request_id":"request","state":"completed","result":{"status":"answered","question_id":"question"}}"#)
        XCTAssertEqual("submitted", QuestionTracking.settle(MessageState(drafts: [draft]), draft, completed).drafts.first?.status)
        let foreign = object(#"{"request_id":"request","state":"completed","result":{"status":"answered","question_id":"other"}}"#)
        XCTAssertEqual("uncertain", QuestionTracking.settle(MessageState(drafts: [draft]), draft, foreign).drafts.first?.status)
        let failed = object(#"{"request_id":"request","state":"failed"}"#)
        XCTAssertEqual("failed", QuestionTracking.settle(MessageState(drafts: [draft]), draft, failed).drafts.first?.status)
        var interrupt = draft
        interrupt.questionId = "__interrupt"
        let stopped = object(#"{"request_id":"request","state":"completed","result":{"status":"completed"}}"#)
        XCTAssertTrue(QuestionTracking.settle(MessageState(drafts: [interrupt]), interrupt, stopped).drafts.isEmpty)
    }
}

final class QuestionSendConfirmationTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
    private let prompt = Prompt(id: "prompt", text: "Choose", choices: [Choice(id: "allow", label: "Allow", description: ""), Choice(id: "deny", label: "Deny", description: "")],
                                multi: false, required: true, other: true, header: "", body: "", otherLabel: "", otherDescription: "")
    private var question: Question {
        Question(id: "question", hash: "hash", prompts: [prompt], canAnswer: true, canSkip: true, optional: false, delivery: "pending",
                 source: "claude", run: target.run, conversation: target.conversation, toolCallId: "tool")
    }
    private var answers: String { QuestionPolicies.answers(question.prompts, options: [:], texts: ["prompt": "Exact  Other answer"]).compact }
    private lazy var draft = Draft(target: target, text: "Exact  Other answer", questionId: question.id, questionHash: question.hash,
                                   answers: answers, revision: "revision", generation: "generation")
    private func intent(_ q: Question? = nil, _ payload: String? = nil) -> QuestionSendIntent {
        QuestionSendPolicies.capture(target, navigation: 1, question: q ?? question, draft: draft, answers: payload ?? answers)
    }
    private func prepared(_ answers: String? = nil) -> Draft {
        var copy = draft
        copy.status = "submitting"
        copy.requestId = "request"
        copy.answers = answers ?? self.answers
        return copy
    }

    func testNativeKimiCacheChoicesKeepExactAnswersWithoutSecondConsent() {
        var cachePrompt = prompt
        cachePrompt.id = "cache"
        cachePrompt.choices = (0...3).map { Choice(id: "cache_\($0)", label: "Native choice \($0)", description: "") }
        var cache = question
        cache.source = "kimi_cache_hint"
        cache.toolCallId = "kimi_cache_hint"
        cache.prompts = [cachePrompt]
        for choice in 0...3 {
            let payload = QuestionPolicies.answers(cache.prompts, options: ["cache": ["cache_\(choice)"]], texts: [:]).compact
            let pending = intent(cache, payload)
            let gate = QuestionSendGate()
            XCTAssertEqual(pending, gate.request(pending, cold: true))
            XCTAssertNil(gate.pending)
            let ready = prepared(payload)
            XCTAssertTrue(pending.permitsPrepared(target: target, navigation: 1, current: cache, prepared: ready, requestId: "request", cold: true, consent: false))
            XCTAssertTrue(pending.answers.contains("cache_\(choice)"))
            var changed = cache
            changed.hash = "changed"
            XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 1, current: changed, prepared: ready, requestId: "request", cold: true, consent: false))
            XCTAssertFalse(pending.permitsPrepared(target: target.with(run: "changed"), navigation: 1, current: cache, prepared: ready, requestId: "request", cold: true, consent: false))
            changed = cache
            changed.source = "claude"
            XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 1, current: changed, prepared: ready, requestId: "request", cold: true, consent: false))
        }
        var other = cache
        other.toolCallId = "other"
        XCTAssertTrue(QuestionSendPolicies.requiresCacheConsent(other, cold: true))
        XCTAssertTrue(QuestionSendPolicies.requiresCacheConsent(question, cold: true))
    }

    func testDefaultColdRequestCannotReleaseTransportOrChangeSavedForm() {
        let gate = QuestionSendGate()
        let original = draft
        let pending = intent()
        XCTAssertNil(gate.request(pending, cold: true))
        XCTAssertEqual(pending, gate.pending)
        XCTAssertEqual(original, pending.draft)
        XCTAssertEqual("editing", pending.draft.status)
        XCTAssertEqual("", pending.draft.requestId)
        // A second submit event must not replace the first captured form.
        XCTAssertNil(gate.request(intent(nil, answers.replacingOccurrences(of: "Exact", with: "Changed")), cold: true))
        XCTAssertEqual(pending, gate.pending)
        gate.cancel(pending)
        XCTAssertNil(gate.pending)
        XCTAssertNil(gate.confirm(pending, valid: true))
    }

    func testConfirmationPostsExactCapturedBytesAtMostOnce() {
        let gate = QuestionSendGate()
        let pending = intent()
        var posted: [String] = []
        _ = gate.request(pending, cold: true)
        for _ in 0..<2 {
            if let approved = gate.confirm(pending, valid: pending.matches(target: target, navigation: 1, current: question, draft: draft)) { posted.append(approved.answers) }
        }
        XCTAssertEqual([answers], posted)
        XCTAssertTrue(posted[0].contains("Exact  Other"))
        XCTAssertNil(gate.pending)
    }

    func testEqualRecaptureIsNotTheActiveConfirmation() {
        let gate = QuestionSendGate()
        let pending = intent()
        _ = gate.request(pending, cold: true)
        let duplicate = intent()
        XCTAssertFalse(gate.active(duplicate))
        XCTAssertNil(gate.confirm(duplicate, valid: true))
        XCTAssertTrue(gate.active(pending))
    }

    func testInactiveDuplicateCannotReportRefusalWhileAcceptedAnswerIsSending() {
        let gate = QuestionSendGate()
        var accepted = 0
        var refused = 0
        func callback(_ intent: QuestionSendIntent, _ valid: Bool) {
            guard gate.active(intent) else { return }
            if gate.confirm(intent, valid: valid) == nil { refused += 1 } else { accepted += 1 }
        }
        let pending = intent()
        _ = gate.request(pending, cold: true)
        callback(pending, true)
        callback(pending, false)
        XCTAssertEqual(1, accepted)
        XCTAssertEqual(0, refused)
        let next = intent()
        _ = gate.request(next, cold: true)
        callback(next, false)
        callback(next, false)
        XCTAssertEqual(1, accepted)
        XCTAssertEqual(1, refused)
    }

    func testNewProcessCannotRestoreConsent() throws {
        let retained = QuestionSendGate()
        let pending = intent()
        _ = retained.request(pending, cold: true)
        XCTAssertEqual(pending, retained.pending)
        let restarted = QuestionSendGate()
        XCTAssertNil(restarted.confirm(pending, valid: true))
        let recovered = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(MessageState(drafts: [draft]))).drafts[0]
        XCTAssertTrue(ContextPolicies.sameDraft(draft, recovered))
        XCTAssertNil(restarted.request(QuestionSendPolicies.capture(target, navigation: 1, question: question, draft: recovered, answers: answers), cold: true))
    }

    func testNavigationTargetQuestionSchemaAndRawDraftChangesInvalidateConsent() {
        let pending = intent()
        XCTAssertTrue(pending.matches(target: target, navigation: 1, current: question, draft: draft))
        XCTAssertFalse(pending.matches(target: target, navigation: 2, current: question, draft: draft))
        var changedTargets = [target.with(computerId: "other"), target.with(run: "other"), target.with(conversation: "other"), target.with(session: "other")]
        var connection = target
        connection.connectionId = "other"
        changedTargets.append(connection)
        for changed in changedTargets { XCTAssertFalse(pending.matches(target: changed, navigation: 1, current: question, draft: draft)) }
        let questionChanges: [(inout Question) -> Void] = [
            { $0.id = "other" }, { $0.hash = "other" }, { $0.toolCallId = "other" }, { $0.source = "other" }, { $0.run = "other" },
            { $0.conversation = "other" }, { $0.prompts[0].text = "Changed"; $0.prompts[0].choices.reverse() }, { $0.canAnswer = false },
            { $0.delivery = "pending"; $0.canAnswer = false; $0.canSkip = true }]
        for change in questionChanges {
            var changed = question
            change(&changed)
            XCTAssertFalse(pending.matches(target: target, navigation: 1, current: changed, draft: draft))
        }
        let draftChanges: [(inout Draft) -> Void] = [{ $0.generation = "other" }, { $0.revision = "other" }, { $0.text = "Changed" },
                                                     { [answers] in $0.answers = answers.replacingOccurrences(of: "  ", with: " ") }, { $0.status = "uncertain" }]
        for change in draftChanges {
            var changed = draft
            change(&changed)
            XCTAssertFalse(pending.matches(target: target, navigation: 1, current: question, draft: changed))
        }
        let gate = QuestionSendGate()
        _ = gate.request(pending, cold: true)
        var revised = draft
        revised.revision = "changed"
        XCTAssertNil(gate.confirm(pending, valid: pending.matches(target: target, navigation: 1, current: question, draft: revised)))
        XCTAssertNil(gate.confirm(pending, valid: true))
    }

    func testApprovalAndSkipUseTheSameColdConsentWithTheirOwnCapability() {
        var approval = question
        approval.approval = true
        approval.canSkip = false
        let allow = QuestionPolicies.answers(approval.prompts, options: ["prompt": ["allow"]], texts: [:]).compact
        let skip = JSON.array([["question_id": "prompt", "skip": true, "selected_option_ids": [], "text": ""]]).compact
        var unanswerable = question
        unanswerable.canAnswer = false
        for (q, payload) in [(approval, allow), (unanswerable, skip)] {
            let gate = QuestionSendGate()
            let pending = intent(q, payload)
            XCTAssertTrue(pending.matches(target: target, navigation: 1, current: q, draft: draft))
            XCTAssertNil(gate.request(pending, cold: true))
            XCTAssertEqual(payload, gate.confirm(pending, valid: true)?.answers)
        }
        var unskippable = question
        unskippable.canSkip = false
        XCTAssertFalse(QuestionSendPolicies.available(unskippable, target, skip))
        XCTAssertFalse(QuestionSendPolicies.available(unanswerable, target, allow))
    }

    func testExpiryOrStateChangeDuringPersistenceCannotReleasePreparedAnswer() {
        let pending = intent()
        let ready = prepared()
        XCTAssertTrue(pending.permitsPrepared(target: target, navigation: 1, current: question, prepared: ready, requestId: "request", cold: false, consent: false))
        XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 1, current: question, prepared: ready, requestId: "request", cold: true, consent: false))
        XCTAssertTrue(pending.permitsPrepared(target: target, navigation: 1, current: question, prepared: ready, requestId: "request", cold: true, consent: true))
        let changes: [(inout Draft) -> Void] = [{ $0.requestId = "other" }, { $0.generation = "other" }, { $0.revision = "other" },
                                                { [answers] in $0.answers = answers.replacingOccurrences(of: "  ", with: " ") }, { $0.status = "uncertain" }]
        for change in changes {
            var changed = ready
            change(&changed)
            XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 1, current: question, prepared: changed, requestId: "request", cold: true, consent: true))
        }
        XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 2, current: question, prepared: ready, requestId: "request", cold: true, consent: true))
        var tool = question
        tool.toolCallId = "other"
        XCTAssertFalse(pending.permitsPrepared(target: target, navigation: 1, current: tool, prepared: ready, requestId: "request", cold: true, consent: true))
    }

    func testRefusalCannotLeakToAnotherComputerRunOrQuestion() {
        let refusal = QuestionSendRefusal(target: target, questionId: question.id, questionHash: question.hash, text: "Cache expired; nothing was sent.")
        XCTAssertEqual(refusal.text, refusal.text(for: target, question))
        XCTAssertEqual("", refusal.text(for: target.with(computerId: "other"), question))
        XCTAssertEqual("", refusal.text(for: target.with(run: "other"), question))
        var changed = question
        changed.hash = "other"
        XCTAssertEqual("", refusal.text(for: target, changed))
        changed = question
        changed.id = "other"
        XCTAssertEqual("", refusal.text(for: target, changed))
    }

    func testBenignPollingMetadataDoesNotInvalidateThePinnedQuestion() {
        var polled = question
        polled.createdAt = 500
        polled.unavailableReason = "Previous observation"
        polled.delivery = "refreshed"
        XCTAssertTrue(intent().matches(target: target, navigation: 1, current: polled, draft: draft))
    }

    func testExpiryRestoresExactOriginalApprovalFormOnlyWhilePreparedRequestIsOwned() {
        let allow = QuestionPolicies.answers(question.prompts, options: ["prompt": ["allow"]], texts: [:]).compact
        var approval = question
        approval.approval = true
        let pending = intent(approval, allow)
        let ready = prepared(allow)
        XCTAssertEqual([draft], pending.restoreAfterExpiry(MessageState(drafts: [ready]), requestId: "request").drafts)
        let changes: [(inout Draft) -> Void] = [{ $0.requestId = "other" }, { $0.revision = "other" }, { $0.generation = "other" }, { $0.text = "changed" },
                                                { [answers] in $0.answers = answers }, { $0.status = "uncertain" }, { [target] in $0.target = target.with(run: "other") }]
        for change in changes {
            var changed = ready
            change(&changed)
            XCTAssertEqual([changed], pending.restoreAfterExpiry(MessageState(drafts: [changed]), requestId: "request").drafts)
        }
    }

    func testForeignPayloadDoesNotMatchQuestion() {
        let foreign = QuestionSendPolicies.capture(target, navigation: 1, question: question, draft: draft, answers: answers.replacingOccurrences(of: "prompt", with: "foreign"))
        XCTAssertFalse(foreign.matches(target: target, navigation: 1, current: question, draft: draft))
    }
}

final class QuestionPresentationTests: XCTestCase {
    private func item(_ answer: String = "literal <tag>\n```code```") -> JSON {
        ["questionItemId": "native-1", "question": "Keep both paths?", "answer": .string(answer)]
    }
    private func envelope(_ body: String) -> String { "<send_user_message_question_reply>\n\(body)\n</send_user_message_question_reply>" }

    func testCompleteObjectPreservesLiteralText() throws {
        let result = try XCTUnwrap(QuestionReplyPresentation.parse(envelope(item().compact))?.first)
        XCTAssertEqual("Keep both paths?", result.question)
        XCTAssertEqual("literal <tag>\n```code```", result.answer)
    }

    func testCompleteArrayPreservesAllAnswersWithoutPreviewLimit() throws {
        let long = String(repeating: "answer ", count: 10000)
        let result = try XCTUnwrap(QuestionReplyPresentation.parse(envelope(JSON.array([item(long), item("")]).compact)))
        XCTAssertEqual(2, result.count)
        XCTAssertEqual(long, result[0].answer)
        XCTAssertEqual("", result[1].answer)
    }

    func testNativeIdePrefixUsesFinalRequestMarkerOnly() {
        XCTAssertNotNil(QuestionReplyPresentation.parse("# Context from my IDE setup:\ncontext\n## My request for Codex:\n" + envelope(item().compact)))
        XCTAssertNil(QuestionReplyPresentation.parse("# Context from my IDE setup:\n" + envelope(item().compact)))
        XCTAssertNil(QuestionReplyPresentation.parse("unrelated preface\n" + envelope(item().compact)))
    }

    func testEmbeddedExampleOrTruncatedEnvelopeRemainsLiteral() {
        XCTAssertNil(QuestionReplyPresentation.parse("Example: " + envelope(item().compact)))
        XCTAssertNil(QuestionReplyPresentation.parse(envelope(item().compact) + " trailing"))
        XCTAssertNil(QuestionReplyPresentation.parse(String(envelope(item().compact).dropLast(10)) + "…"))
    }

    func testOneMalformedArrayMemberRejectsEntirePresentation() {
        XCTAssertNil(QuestionReplyPresentation.parse(envelope(JSON.array([item(), ["answer": "x"]]).compact)))
        XCTAssertNil(QuestionReplyPresentation.parse(envelope("[]")))
        XCTAssertNil(QuestionReplyPresentation.parse(envelope("null")))
    }

    func testRequiredFieldsHaveExactStringTypes() {
        let changes: [(String, JSON)] = [("answer", .bool(true)), ("question", .int(1)), ("questionItemId", .string("")), ("question", .string("  "))]
        for (key, value) in changes {
            var changed = item().object!
            changed[key] = value
            XCTAssertNil(QuestionReplyPresentation.parse(envelope(JSON.object(changed).compact)))
        }
    }

    func testMalformedJavascriptSyntaxIsNotJson() {
        for body in [#"{'questionItemId':'i','question':'q','answer':'a'}"#, #"{questionItemId:"i",question:"q",answer:"a"}"#,
                     #"{"questionItemId":"i","question":"q","answer":"a",}"#] {
            XCTAssertNil(QuestionReplyPresentation.parse(envelope(body)))
        }
    }

    func testHugeExponentCannotBeCoercedIntoRequiredText() {
        let keys = ["answer", "question", "questionItemId"]
        for key in keys {
            let json = "{" + keys.map { "\"\($0)\":" + ($0 == key ? "1e999999" : "\"literal\"") }.joined(separator: ",") + "}"
            XCTAssertNil(QuestionReplyPresentation.parse(envelope(json)))
        }
    }

    func testOversizedHistoryRemainsLiteralRatherThanPreviewTruncated() {
        XCTAssertNil(QuestionReplyPresentation.parse(envelope(item(String(repeating: "x", count: 256 * 1024)).compact)))
    }

    func testOnlyExplicitUserRolesUseUserBubble() {
        XCTAssertTrue(QuestionReplyPresentation.isOwnRole("You"))
        XCTAssertTrue(QuestionReplyPresentation.isOwnRole("You (answer)"))
        XCTAssertFalse(QuestionReplyPresentation.isOwnRole("You (malformed)"))
        XCTAssertFalse(QuestionReplyPresentation.isOwnRole("Agent"))
    }

    func testSubmitGateAllowsOneAttemptUntilFinished() {
        let gate = QuestionSubmitGate()
        XCTAssertTrue(gate.claim(true))
        XCTAssertFalse(gate.claim(true))
        gate.finish()
        XCTAssertFalse(gate.claim(false))
        XCTAssertTrue(gate.claim(true))
    }

    func testDisplayStateNeverRebindsToAnotherRequest() {
        let original = Question(id: "question", hash: "hash", prompts: [], canAnswer: true, canSkip: true, optional: true, delivery: "")
        let state = QuestionDisplayState(owner: "owner").open(original)
        var changed = original
        changed.hash = "changed"
        var other = original
        other.id = "other"
        XCTAssertEqual(original, state.resolve([original]))
        XCTAssertNil(state.resolve([]))
        XCTAssertNil(state.resolve([changed]))
        XCTAssertNil(state.resolve([other]))
        XCTAssertNil(state.resolve([original, original]))
        XCTAssertFalse(state.close().expanded)
        XCTAssertFalse(state.forTarget("other run").expanded)
    }

    func testDisplayStateKeepsEachPartWithoutDelimiterCollisionsAndStaysBounded() {
        func question(_ id: String, _ hash: String = "hash") -> Question {
            Question(id: id, hash: hash, prompts: [], canAnswer: true, canSkip: true, optional: true, delivery: "")
        }
        let a = question("a:b", "c")
        let b = question("a", "b:c")
        let state = QuestionDisplayState(owner: "owner").open(a).withPage(a, 1).withOffset(a, page: 1, 99)
            .close().open(b).withPage(b, 3).withOffset(b, page: 3, 155).close().open(a)
        XCTAssertEqual(1, state.page(a))
        XCTAssertEqual(99, state.offset(a, page: 1))
        XCTAssertEqual(0, state.offset(a, page: 2))
        XCTAssertEqual(3, state.page(b))
        XCTAssertEqual(155, state.offset(b, page: 3))
        var bounded = QuestionDisplayState(owner: "owner")
        for i in 0..<100 { bounded = bounded.withPage(question("\(i)"), i).withOffset(question("\(i)"), page: i, i) }
        XCTAssertEqual(64, bounded.pages.count)
        XCTAssertEqual(64, bounded.offsets.count)
        XCTAssertEqual(99, bounded.page(question("99")))
        XCTAssertEqual(0, bounded.page(question("0")))
    }

    func testApprovalReviewRestartsWhenAnotherCardTakesItsPlace() {
        let target = Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace")
        let first = Question(id: "per_1", hash: "h1", prompts: [], canAnswer: true, canSkip: false, optional: false, delivery: "")
        var second = first
        second.id = "per_2"
        var rehashed = first
        rehashed.hash = "h2"
        let a = ApprovalReview.key(target, first), b = ApprovalReview.key(target, second)
        XCTAssertEqual(Set([a, b, ApprovalReview.key(target, rehashed), ApprovalReview.key(target.with(run: "other"), first)]).count, 4)
        var review = ApprovalReview().observe(eligible: true, now: 1, card: a)
        XCTAssertEqual(0, review.secondsRemaining(now: 4, card: a))
        // Another card shown in the same place has not been reviewed, even before the next observation.
        XCTAssertEqual(3, review.secondsRemaining(now: 4, card: b))
        review = review.observe(eligible: true, now: 4, card: b)
        XCTAssertEqual(3, review.secondsRemaining(now: 4, card: b))
        XCTAssertEqual(1, review.secondsRemaining(now: 6, card: b))
        XCTAssertEqual(0, review.secondsRemaining(now: 7, card: b))
        review = review.observe(eligible: true, now: 7, card: b)
        XCTAssertEqual(0, review.secondsRemaining(now: 7, card: b), "the same card keeps its review")
        XCTAssertEqual(3, review.secondsRemaining(now: 7, card: a), "the earlier card is reviewed again")
        XCTAssertEqual(3, review.observe(eligible: true, now: 8, card: a).secondsRemaining(now: 8, card: a))
        XCTAssertEqual(3, review.observe(eligible: false, now: 8, card: b).secondsRemaining(now: 20, card: b))
    }

    func testApprovalReviewNeedsThreeContinuousVisibleSeconds() {
        var review = ApprovalReview().observe(eligible: true, now: 1)
        XCTAssertEqual(3, review.secondsRemaining(now: 1))
        XCTAssertEqual(1, review.secondsRemaining(now: 3.999))
        XCTAssertEqual(0, review.secondsRemaining(now: 4))
        review = review.observe(eligible: false, now: 4.1)
        XCTAssertEqual(3, review.secondsRemaining(now: 10))
        review = review.observe(eligible: true, now: 10)
        XCTAssertEqual(0, review.secondsRemaining(now: 13))
        XCTAssertEqual(3, ApprovalReview().observe(eligible: true, now: 5).observe(eligible: true, now: 4).secondsRemaining(now: 10))
    }
}
