import XCTest
@testable import ZerusKit

/// Ported from Android QuestionPoliciesTest: approvals answered with the agent's own options (`approval_choices`), as the
/// desktop answers a Claude terminal panel or a Kimi tool approval.
final class ApprovalChoicesTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "kimi/project/work", run: "run", conversation: "conversation", connectionId: "workspace")

    /// The Kimi Bash approval card hgs builds (src/state/kimi_approval.rs), in parts that a test may edit before parsing.
    private struct Card {
        var card: JSONObject
        var item: JSONObject
        var options: [JSONObject]
        var secondItem = false
    }

    private func kimi(_ edit: (inout Card) -> Void = { _ in }) throws -> Question {
        var parts = Card(
            card: ["question_id": "approval_1", "question_hash": "kimi-hash", "tool_call_id": "tool_1", "agent_id": "main",
                   "run_id": .string(target.run), "conversation_id": .string(target.conversation), "created_at": 1.5,
                   "source": "kimi_tool_approval", "approval": true, "approval_choices": true, "can_answer": true,
                   "answer_transport": "kimi_tui", "answer_unavailable_reason": ""],
            item: ["id": "approval", "question": "Run this command?", "header": "Bash",
                   "body": "cwd: /synthetic/work\nDangerous: recursive delete\n$ rm -rf .cache", "multi_select": false,
                   "allow_other": true, "other_label": "Reject with feedback",
                   "other_description": "Reject this request with your message; Kimi continues without it"],
            options: [["id": "1", "label": "Approve once", "description": "", "scope": "once"],
                      ["id": "2", "label": "Approve for this session",
                       "description": "Kimi also stops asking about this exact command for the rest of this session.", "scope": "broader"],
                      ["id": "3", "label": "Reject", "description": "", "scope": "once"]])
        edit(&parts)
        let item = parts.item.putting("options", .array(parts.options.map(JSON.object)))
        let items = parts.secondItem ? [item, item.putting("id", "other")] : [item]
        return try XCTUnwrap(NativeEvents.questions(["pending_questions": [.object(parts.card.putting("questions", .array(items.map(JSON.object))))]]).first)
    }

    /// A Claude terminal panel: its options verbatim, without a text answer.
    private func claude() throws -> Question {
        try kimi { parts in
            parts.card["source"] = "claude_tool_approval"
            parts.card["answer_transport"] = "claude_tui"
            parts.item["allow_other"] = false
            parts.item["other_label"] = nil
            parts.options = [["id": "1", "label": "Yes", "scope": "once"],
                             ["id": "2", "label": "Yes, and don't ask again for git commands in /synthetic/work", "scope": "broader"],
                             ["id": "3", "label": "No", "scope": "once"]]
        }
    }

    func testNativeApprovalChoicesQualifyInTheirExactShape() throws {
        let kimi = try self.kimi()
        XCTAssertTrue(kimi.approvalChoices)
        let prompt = try XCTUnwrap(kimi.prompts.first)
        // The agent's options, verbatim and in its order.
        XCTAssertEqual(prompt.choices.map(\.id), ["1", "2", "3"])
        XCTAssertEqual(prompt.choices.map(\.label), ["Approve once", "Approve for this session", "Reject"])
        XCTAssertEqual(prompt.choices.map(\.scope), ["once", "broader", "once"])
        XCTAssertTrue(QuestionPolicies.nativeChoices(kimi))
        XCTAssertTrue(QuestionPolicies.showsSubmit(kimi))
        XCTAssertFalse(QuestionPolicies.approvalSupported(kimi))
        XCTAssertFalse(QuestionPolicies.openCodePermission(kimi))
        XCTAssertEqual(PermissionAction.actions(kimi), [])
        XCTAssertNil(PermissionConfirmation(kimi))
        // Kimi's Reject with feedback is the card's own text answer.
        XCTAssertTrue(QuestionPolicies.otherField(kimi, prompt))
        XCTAssertEqual(prompt.otherLabel, "Reject with feedback")

        let claude = try self.claude()
        XCTAssertTrue(QuestionPolicies.nativeChoices(claude))
        XCTAssertTrue(QuestionPolicies.showsSubmit(claude))
        XCTAssertFalse(QuestionPolicies.otherField(claude, try XCTUnwrap(claude.prompts.first)))
        XCTAssertEqual(claude.prompts.first?.choices.map(\.label), ["Yes", "Yes, and don't ask again for git commands in /synthetic/work", "No"])

        // A card that cannot be answered here keeps its native options read-only, with its reason.
        let readOnly = try self.kimi { $0.card["can_answer"] = false; $0.card["answer_unavailable_reason"] = "Open Terminal to answer this approval." }
        XCTAssertTrue(QuestionPolicies.nativeChoices(readOnly))
        XCTAssertFalse(QuestionPolicies.showsSubmit(readOnly))
        XCTAssertFalse(QuestionPolicies.otherField(readOnly, try XCTUnwrap(readOnly.prompts.first)))
        XCTAssertEqual(readOnly.unavailableReason, "Open Terminal to answer this approval.")

        let refused: [(String, (inout Card) -> Void)] = [
            ("no approval_choices", { $0.card["approval_choices"] = false }),
            ("older host without the flag", { $0.card["approval_choices"] = nil }),
            ("not an approval", { $0.card["approval"] = false }),
            ("multi", { $0.item["multi_select"] = true }),
            ("no options", { $0.options = [] }),
            ("missing scope", { $0.options[0]["scope"] = nil }),
            ("unknown scope", { $0.options[1]["scope"] = "always" }),
            ("repeated id", { $0.options[2]["id"] = "1" }),
            ("blank id", { $0.options[2]["id"] = "" }),
            ("whitespace id", { $0.options[2]["id"] = " " }),
            ("second item", { $0.secondItem = true }),
            ("OpenCode", { $0.card["source"] = "opencode_native" }),
        ]
        for (name, edit) in refused {
            let question = try self.kimi(edit)
            XCTAssertFalse(QuestionPolicies.nativeChoices(question), name)
            XCTAssertNil(QuestionPolicies.broaderChoice(question, options: ["approval": ["2"]]), name)
            XCTAssertNil(BroaderChoiceConfirmation(question, options: ["approval": ["2"]], reviewed: true), name)
            if question.approval {
                // Every other approval keeps today's rule: no Send and no text field here.
                XCTAssertFalse(QuestionPolicies.showsSubmit(question), name)
                XCTAssertFalse(QuestionPolicies.otherField(question, try XCTUnwrap(question.prompts.first)), name)
            }
        }
        // Ordinary questions keep their text field, answerable or not.
        let form = try self.kimi { $0.card["approval"] = false }
        XCTAssertTrue(QuestionPolicies.otherField(form, try XCTUnwrap(form.prompts.first)))
        var unanswerable = form
        unanswerable.canAnswer = false
        XCTAssertTrue(QuestionPolicies.otherField(unanswerable, try XCTUnwrap(unanswerable.prompts.first)))
    }

    func testNativeChoiceAnswersSendTheChosenOptionOrTheFeedbackText() throws {
        let kimi = try self.kimi()
        let prompts = kimi.prompts
        let once = QuestionPolicies.answers(prompts, options: ["approval": ["1"]], texts: [:])
        XCTAssertEqual(once.compact, #"[{"question_id":"approval","selected_option_ids":["1"],"text":""}]"#)
        // A rejection is an ordinary option.
        XCTAssertTrue(QuestionPolicies.complete(prompts, options: ["approval": ["3"]], texts: [:]))
        let feedback = ["approval": "Use rg instead"]
        XCTAssertEqual(QuestionPolicies.answers(prompts, options: [:], texts: feedback).compact,
                       #"[{"question_id":"approval","selected_option_ids":[],"text":"Use rg instead"}]"#)
        XCTAssertTrue(QuestionPolicies.complete(prompts, options: [:], texts: feedback))
        XCTAssertEqual(QuestionPolicies.answerError(prompts, options: ["approval": ["3"]], texts: feedback), "Choose one answer for this question.")
        XCTAssertEqual(QuestionPolicies.answerError(prompts, options: [:], texts: ["approval": "/rm"]), "Enter command-like answers on your computer.")
        XCTAssertEqual(QuestionPolicies.answerError(prompts, options: [:], texts: ["approval": "line\nbreak"]),
                       "Answers must be one line and at most 4096 UTF-8 bytes.")
        XCTAssertFalse(QuestionPolicies.complete(prompts, options: [:], texts: ["approval": "line\nbreak"]))
        XCTAssertFalse(QuestionPolicies.complete(prompts, options: [:], texts: [:]))
        XCTAssertEqual(QuestionPolicies.answerError(prompts, options: ["approval": ["4"]], texts: [:]), "An answer option changed. Review this request again.")
        XCTAssertTrue(QuestionSendPolicies.available(kimi, target, once.compact))
        // A Claude panel accepts only its options.
        let claude = try self.claude()
        XCTAssertTrue(QuestionPolicies.complete(claude.prompts, options: ["approval": ["3"]], texts: [:]))
        XCTAssertEqual(QuestionPolicies.answerError(claude.prompts, options: [:], texts: ["approval": "Use rg instead"]),
                       "This request does not accept an Other answer.")
    }

    func testNativeOptionsListWhatABroaderOptionDoes() throws {
        let choices = try XCTUnwrap(try kimi().prompts.first?.choices)
        XCTAssertEqual(QuestionPolicies.listedChoice(choices[0]), choices[0])
        XCTAssertEqual(QuestionPolicies.listedChoice(choices[1]).description,
                       "Kimi also stops asking about this exact command for the rest of this session.\nApplies beyond this request. Asks for confirmation.")
        XCTAssertEqual(QuestionPolicies.listedChoice(choices[1]).label, "Approve for this session")
        XCTAssertEqual(QuestionPolicies.listedChoice(try XCTUnwrap(try claude().prompts.first?.choices[1])).description,
                       "Applies beyond this request. Asks for confirmation.")
        XCTAssertEqual(QuestionPolicies.nativeChoicesHint(remaining: 2), "Review the request, then choose one of the agent's options (2).")
        XCTAssertEqual(QuestionPolicies.nativeChoicesHint(remaining: 0), "Choose one of the agent's options.")
        XCTAssertEqual(BroaderChoiceConfirmation.message(choices[1]),
                       "\u{201C}Approve for this session\u{201D} changes the agent's permissions beyond this request.\n\n" +
                       "Kimi also stops asking about this exact command for the rest of this session.")
        XCTAssertEqual(BroaderChoiceConfirmation.message(Choice(id: "2", label: "Yes, always", description: "  ", scope: "broader")),
                       "\u{201C}Yes, always\u{201D} changes the agent's permissions beyond this request.")
    }

    /// The confirmation belongs to the exact card and option, and rechecks everything Send needed, including this card's
    /// own review: its own presentation is never review time, so a dialog left open across a reset cannot confirm.
    func testBroaderChoiceAsksForItsOwnConfirmationOfTheExactCardAndOption() throws {
        let kimi = try self.kimi()
        let session: [String: Set<String>] = ["approval": ["2"]], once: [String: Set<String>] = ["approval": ["1"]]
        XCTAssertNil(QuestionPolicies.broaderChoice(kimi, options: once))
        XCTAssertNil(QuestionPolicies.broaderChoice(kimi, options: [:]))
        XCTAssertEqual(QuestionPolicies.broaderChoice(kimi, options: session)?.label, "Approve for this session")
        XCTAssertNil(BroaderChoiceConfirmation(kimi, options: session, reviewed: false), "the review has not finished")
        XCTAssertNil(BroaderChoiceConfirmation(kimi, options: once, reviewed: true), "Approve once sends without a second confirmation")
        let open = try XCTUnwrap(BroaderChoiceConfirmation(kimi, options: session, reviewed: true))
        XCTAssertEqual([open.questionId, open.questionHash, open.optionId], ["approval_1", "kimi-hash", "2"])

        let key = ApprovalReview.key(target, kimi)
        let reviewed = ApprovalReview().observe(eligible: true, now: 1, card: key)
        func showing(_ current: Question? = nil, _ options: [String: Set<String>]? = nil, review: ApprovalReview? = nil, now: Double = 4,
                     foreground: Bool = true, visible: Bool = true, editing: Bool = true, available: Bool = true, on target: Target? = nil) -> Choice? {
            open.showing(current ?? kimi, options: options ?? session, target: target ?? self.target, editing: editing, available: available,
                         foreground: foreground, visible: visible, review: review ?? reviewed, now: now)
        }
        // Control: the exact card and option, reviewed, visible and in the foreground.
        XCTAssertEqual(showing()?.id, "2")
        XCTAssertNil(showing(now: 2), "the review is still counting")
        XCTAssertNil(showing(foreground: false), "not in the foreground")
        XCTAssertNil(showing(visible: false), "not visible")
        XCTAssertNil(showing(editing: false), "an answer is already being sent or was sent")
        XCTAssertNil(showing(available: false), "the request must be verified again")
        XCTAssertNil(showing(on: target.with(run: "other")), "another target")
        // The dialog closes without sending when anything it confirmed changed.
        XCTAssertNil(showing(nil, once), "another option is chosen")
        XCTAssertNil(showing(nil, [:]), "no option is chosen")
        var changed = kimi
        changed.hash = "changed"
        XCTAssertNil(showing(changed), "another hash")
        changed = kimi
        changed.id = "approval_2"
        XCTAssertNil(showing(changed), "another request")
        changed = kimi
        changed.canAnswer = false
        XCTAssertNil(showing(changed), "no longer answerable")
        XCTAssertNil(showing(try self.kimi { $0.options[1]["scope"] = "once" }), "the option no longer reaches beyond this request")
        XCTAssertNil(showing(try self.kimi { $0.card["approval_choices"] = nil }), "no longer a native choice card")

        // Background or out of view with the dialog still open: nothing is confirmed until a new review completes, and a
        // late tick from the stopping clock never re-arms the reset review.
        var review = reviewed.observe(eligible: false, now: 5, card: key).tick(now: 5.2, card: key)
        XCTAssertNil(showing(review: review, now: 5))
        XCTAssertNil(showing(review: review, now: 9), "time away is not review time")
        review = review.observe(eligible: true, now: 9, card: key)
        for now in [9.0, 9.2, 10.4, 11.9] {
            review = review.tick(now: now, card: key)
            XCTAssertNil(showing(review: review, now: now), "at \(now)")
        }
        XCTAssertEqual(showing(review: review.tick(now: 12, card: key), now: 12)?.id, "2", "a new review completed")
        XCTAssertNil(showing(review: ApprovalReview(), now: 20), "never reviewed")
        var other = kimi
        other.id = "other"
        XCTAssertNil(showing(review: ApprovalReview().observe(eligible: true, now: 1, card: ApprovalReview.key(target, other)), now: 20),
                     "a review of another card")

        var sent = 0
        open.confirm(kimi, options: once, target: target, editing: true, available: true, foreground: true, visible: true, review: reviewed, now: 4) { sent += 1 }
        XCTAssertEqual(sent, 0)
        open.confirm(kimi, options: session, target: target, editing: true, available: true, foreground: true, visible: true, review: reviewed, now: 2) { sent += 1 }
        XCTAssertEqual(sent, 0, "the review is still counting")
        open.confirm(kimi, options: session, target: target, editing: true, available: true, foreground: true, visible: true, review: reviewed, now: 4) { sent += 1 }
        XCTAssertEqual(sent, 1)
    }
}
