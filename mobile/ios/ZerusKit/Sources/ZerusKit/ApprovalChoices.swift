import Foundation

extension QuestionPolicies {
    static let nativeScopes: Set<String> = ["once", "broader"]
    static let broaderNotice = "Applies beyond this request. Asks for confirmation."

    /// An approval answered with the agent's own options, as the desktop and Android answer it: Claude's terminal panel
    /// and Kimi's tool approval mark it with `approval_choices`. One single-choice item whose options each have a scope:
    /// `once` grants nothing beyond this request, `broader` grants more and asks for a `BroaderChoiceConfirmation`. An
    /// Other answer, when offered, is the agent's own text choice, such as Kimi's Reject with feedback. OpenCode
    /// permission cards keep `openCodePermission`; other approvals keep `approvalSupported`.
    public static func nativeChoices(_ question: Question) -> Bool {
        guard question.approval, question.approvalChoices, question.source != "opencode_native", question.prompts.count == 1,
              let prompt = question.prompts.first, !prompt.multi, !prompt.choices.isEmpty else { return false }
        return prompt.choices.allSatisfy { !$0.id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && nativeScopes.contains($0.scope) } &&
            Set(prompt.choices.map(\.id)).count == prompt.choices.count
    }

    /// The broader option chosen in `options` on a `nativeChoices` card, or nil.
    public static func broaderChoice(_ question: Question, options: [String: Set<String>]) -> Choice? {
        guard nativeChoices(question), let prompt = question.prompts.first else { return nil }
        return prompt.choices.first { $0.scope == "broader" && options[prompt.id]?.contains($0.id) == true }
    }

    /// Whether a card shows `prompt`'s text field. Approvals have none, except a `nativeChoices` card that can be
    /// answered here and offers the agent's own text answer.
    public static func otherField(_ question: Question, _ prompt: Prompt) -> Bool {
        prompt.other && (!question.approval || nativeChoices(question) && question.canAnswer)
    }

    /// A native option as its card lists it: a broader option also says that it reaches beyond this request.
    public static func listedChoice(_ choice: Choice) -> Choice {
        guard choice.scope == "broader" else { return choice }
        var listed = choice
        listed.description = [choice.description, broaderNotice].filter { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            .joined(separator: "\n")
        return listed
    }

    /// Every native choice, a rejection included, waits for the card's review, as on the desktop.
    public static func nativeChoicesHint(remaining: Int) -> String {
        remaining > 0 ? "Review the request, then choose one of the agent's options (\(remaining))." : "Choose one of the agent's options."
    }
}

/// The separate confirmation of one broader option on a `nativeChoices` card, for that exact card (ID and hash) and
/// option. Like `PermissionConfirmation`, it sends only while that option is still the chosen one and the card can be
/// answered and edited here, in the foreground and visible, with its own review complete; otherwise it closes without
/// sending. Opening it needs a finished review and never starts, extends or re-arms one.
public struct BroaderChoiceConfirmation: Hashable, Sendable {
    public let questionId: String
    public let questionHash: String
    public let optionId: String

    /// The confirmation an enabled Send opens when `options` choose a broader option, or nil: no broader option is
    /// chosen, or the card's review has not finished (`reviewed`).
    public init?(_ question: Question, options: [String: Set<String>], reviewed: Bool) {
        guard reviewed, let choice = QuestionPolicies.broaderChoice(question, options: options) else { return nil }
        questionId = question.id
        questionHash = question.hash
        optionId = choice.id
    }

    /// The dialog text: what the option does, then the agent's own description of it, verbatim.
    public static func message(_ choice: Choice) -> String {
        let general = "\u{201C}\(choice.label)\u{201D} changes the agent's permissions beyond this request."
        return choice.description.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? general : general + "\n\n" + choice.description
    }

    /// The option to confirm, or nil to close without sending; the dialog is shown only while this is non-nil. It
    /// rechecks everything Send needed: the same card, still answerable and editable, in the foreground and visible,
    /// with that card's own review complete, and the same broader option chosen. Background or a hidden card restarts
    /// the review, so a dialog left open across them cannot confirm until a new review completes.
    public func showing(_ current: Question, options: [String: Set<String>], target: Target, editing: Bool, available: Bool,
                        foreground: Bool, visible: Bool, review: ApprovalReview, now: Double) -> Choice? {
        guard current.id == questionId, current.hash == questionHash, current.canAnswer, editing, available, foreground, visible,
              review.secondsRemaining(now: now, card: ApprovalReview.key(target, current)) == 0,
              let choice = QuestionPolicies.broaderChoice(current, options: options), choice.id == optionId else { return nil }
        return choice
    }

    /// Calls `send` once when the person confirms and `showing` still holds; otherwise sends nothing.
    public func confirm(_ current: Question, options: [String: Set<String>], target: Target, editing: Bool, available: Bool,
                        foreground: Bool, visible: Bool, review: ApprovalReview, now: Double, send: () -> Void) {
        if showing(current, options: options, target: target, editing: editing, available: available, foreground: foreground,
                   visible: visible, review: review, now: now) != nil { send() }
    }
}
