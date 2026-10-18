import Foundation

/// The choices and texts a question card opens with.
public struct QuestionOpening: Hashable, Sendable {
    public let options: [String: Set<String>]
    public let texts: [String: String]
}

public struct QuestionQueue: Sendable {
    public let selected: Question?
    public let optional: [Question]
    public let index: Int
    public let submitted: [Question]
    public let promptCount: Int
}

extension QuestionPolicies {
    public static func submitted(_ question: Question, _ target: Target) -> NativeQuestionDelivery? {
        guard let delivery = question.answerDelivery, question.source == "codex_async", question.optional, delivery.status == "submitted",
              delivery.questionId == question.id, delivery.questionHash == question.hash, delivery.run == question.run,
              delivery.conversation == question.conversation, delivery.run == target.run, delivery.conversation == target.conversation,
              !question.id.isEmpty, !question.hash.isEmpty, !target.run.isEmpty else { return nil }
        return delivery
    }

    public static func key(_ question: Question) -> String { question.id + ":" + question.hash }

    /// A card with items, or one that cannot be answered here (hgs clears the items of an oversized form); the card
    /// shows its reason, or a fallback when none is reported.
    public static func shown(_ question: Question) -> Bool {
        !question.id.isEmpty && (!question.prompts.isEmpty || !question.canAnswer)
    }

    public static func queue(_ questions: [Question], _ target: Target, chosen: String) -> QuestionQueue {
        var seen = Set<String>()
        let distinct = questions.filter { shown($0) && seen.insert($0.id).inserted }
        let submitted = distinct.filter { self.submitted($0, target) != nil }
        let offered = distinct.filter { question in !submitted.contains(question) }
        let required = offered.filter { !$0.optional }
        let selectedRequired = required.first { $0.canAnswer } ?? required.first
        let optional = offered.filter(\.optional).enumerated()
            .sorted { $0.element.createdAt != $1.element.createdAt ? $0.element.createdAt > $1.element.createdAt : $0.offset < $1.offset }.map(\.element)
        let index = max(optional.firstIndex { key($0) == chosen } ?? 0, 0)
        return QuestionQueue(selected: selectedRequired ?? (optional.indices.contains(index) ? optional[index] : nil), optional: optional, index: index,
                             submitted: submitted, promptCount: offered.reduce(0) { $0 + $1.prompts.count })
    }

    /// Native answers in source option order, independent of tap order.
    public static func answers(_ prompts: [Prompt], options: [String: Set<String>], texts: [String: String]) -> JSON {
        .array(prompts.map { prompt in
            ["question_id": .string(prompt.id),
             "selected_option_ids": JSON(prompt.choices.filter { options[prompt.id]?.contains($0.id) == true }.map(\.id)),
             "text": .string(prompt.other ? (texts[prompt.id] ?? "") : "")]
        })
    }

    public static func answerError(_ prompts: [Prompt], options: [String: Set<String>], texts: [String: String]) -> String {
        for prompt in prompts {
            let text = texts[prompt.id] ?? ""
            let selected = options[prompt.id] ?? []
            if text.utf8.count > 4096 || text.unicodeScalars.contains(where: PairingInvite.isISOControl) {
                return "Answers must be one line and at most 4096 UTF-8 bytes."
            }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            if !prompt.literalText && (trimmed.hasPrefix("/") || trimmed.hasPrefix("!")) { return "Enter command-like answers on your computer." }
            if selected.contains(where: { id in !prompt.choices.contains { $0.id == id } }) { return "An answer option changed. Review this request again." }
            if !trimmed.isEmpty && !prompt.other { return "This request does not accept an Other answer." }
            if !prompt.multi && selected.count + (trimmed.isEmpty ? 0 : 1) > 1 { return "Choose one answer for this question." }
            // Form item bounds, worded as the computer refuses them; reported only by OpenCode forms.
            let characters = text.unicodeScalars.count, values = selected.count + (trimmed.isEmpty ? 0 : 1)
            if let max = prompt.maxLength, !text.isEmpty, characters > max { return "Answer is longer than \(max) characters" }
            if let min = prompt.minLength, !text.isEmpty, characters < min { return "Answer is shorter than \(min) characters" }
            if let max = prompt.maxItems, prompt.multi, values > max { return "Choose at most \(max) answers" }
            if let min = prompt.minItems, prompt.multi, values > 0, values < min { return "Choose at least \(min) answers" }
        }
        return ""
    }

    /// Every required answer is present; an optional form item (OpenCode `required: false`) may stay empty.
    public static func complete(_ prompts: [Prompt], options: [String: Set<String>], texts: [String: String]) -> Bool {
        !prompts.isEmpty && answerError(prompts, options: options, texts: texts).isEmpty && prompts.allSatisfy { prompt in
            !(options[prompt.id] ?? []).isEmpty || prompt.other && !(texts[prompt.id] ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
                !prompt.inputKind.isEmpty && !prompt.required
        }
    }

    /// The answers a card opens with. A saved draft of this exact question (or its native submission) wins for every item
    /// it records, including an intentionally empty text; otherwise form defaults seed the choices and an OpenCode text
    /// item's `default_text`. Seeded answers are only shown: nothing is saved, confirmed or sent until the person acts.
    public static func opening(_ prompts: [Prompt], draftAnswers: String, native: NativeQuestionDelivery?) -> QuestionOpening {
        var saved: [String: NativeQuestionAnswer] = [:]
        if let native {
            for answer in native.answers { saved[answer.questionId] = answer }
        } else if let rows = (try? JSON.parse(draftAnswers))?.array {
            for row in rows.compactMap(\.object) {
                let id = row.string("question_id")
                saved[id] = NativeQuestionAnswer(questionId: id, optionIds: row.strings("selected_option_ids"), text: row.string("text"), skip: row.bool("skip"))
            }
        }
        let choices = native == nil ? defaults(prompts) : [:]
        let options = prompts.map { prompt in (prompt.id, saved[prompt.id].map { Set($0.optionIds) } ?? choices[prompt.id] ?? []) }
        let texts = prompts.map { prompt in (prompt.id, saved[prompt.id]?.text ?? (native == nil ? prompt.defaultText : "")) }
        return QuestionOpening(options: Dictionary(options, uniquingKeysWith: { first, _ in first }),
                               texts: Dictionary(texts, uniquingKeysWith: { first, _ in first }))
    }

    /// Preselected form answers (`default_option_ids`) among the offered options; one for a single-answer item.
    public static func defaults(_ prompts: [Prompt]) -> [String: Set<String>] {
        var result: [String: Set<String>] = [:]
        for prompt in prompts {
            let offered = prompt.defaultOptionIds.filter { id in prompt.choices.contains { $0.id == id } }
            if let first = offered.first { result[prompt.id] = prompt.multi ? Set(offered) : [first] }
        }
        return result
    }

    /// The reported length bounds of a text answer, or "".
    public static func textHint(_ prompt: Prompt) -> String {
        switch (prompt.minLength, prompt.maxLength) {
        case let (min?, max?): min == max ? "\(max) characters" : "\(min)–\(max) characters"
        case let (nil, max?): "Up to \(max) characters"
        case let (min?, nil): "At least \(min) characters"
        default: ""
        }
    }

    /// The reported answer-count bounds of a multiple-choice item, or "".
    public static func choiceHint(_ prompt: Prompt) -> String {
        guard prompt.multi else { return "" }
        switch (prompt.minItems, prompt.maxItems) {
        case let (min?, max?): return min == max ? "Choose \(max)" : "Choose \(min)–\(max)"
        case let (nil, max?): return "Choose up to \(max)"
        case let (min?, nil): return "Choose at least \(min)"
        default: return ""
        }
    }

    /// Submit appears only on a card that can be answered here; approvals use their own buttons.
    public static func showsSubmit(_ question: Question) -> Bool { question.canAnswer && !question.approval }

    public static func approvalSupported(_ question: Question) -> Bool {
        guard question.approval, question.prompts.count == 1, let prompt = question.prompts.first, !prompt.multi else { return false }
        return prompt.choices.contains { $0.id == "allow" } && prompt.choices.contains { $0.id == "deny" }
    }

    /// An answerable OpenCode permission: Allow once, Reject and optionally Always allow, answered with those native ids.
    /// Other approvals with native choices (Claude) stay on the computer; allow/deny approvals keep `approvalSupported`.
    public static func openCodePermission(_ question: Question) -> Bool {
        guard question.approval, question.source == "opencode_native", question.canAnswer, question.prompts.count == 1,
              let prompt = question.prompts.first, prompt.id == "permission", !prompt.multi else { return false }
        let ids = prompt.choices.map(\.id)
        return ids.contains("once") && ids.contains("reject") && ids.allSatisfy { ["once", "always", "reject"].contains($0) }
    }

    /// The single answer row of an approval decision: one native option of its one prompt.
    public static func approvalAnswers(_ question: Question, _ option: String) -> JSON? {
        guard let prompt = question.prompts.first else { return nil }
        return .array([["question_id": .string(prompt.id), "selected_option_ids": [.string(option)], "text": ""]])
    }
}

/// "Always allow" on an OpenCode permission reaches beyond this one request, so it needs its own confirmation. That
/// confirmation belongs to the exact card that offered it and sends only while that card still offers it and can be
/// answered and edited here; otherwise it closes without sending.
public struct PermissionConfirmation: Hashable, Sendable {
    public static let option = "always"
    public let questionId: String
    public let questionHash: String

    /// The confirmation an "Always allow…" tap opens, or nil when this card does not offer Always allow.
    public init?(_ question: Question) {
        guard Self.offered(question) else { return nil }
        questionId = question.id
        questionHash = question.hash
    }

    static func offered(_ question: Question) -> Bool {
        QuestionPolicies.openCodePermission(question) && question.prompts[0].choices.contains { $0.id == option }
    }

    /// The answer to send when the person confirms, or nil to send nothing.
    public func answers(_ current: Question, editing: Bool, available: Bool) -> JSON? {
        guard current.id == questionId, current.hash == questionHash, Self.offered(current), editing, available else { return nil }
        return QuestionPolicies.approvalAnswers(current, Self.option)
    }
}

/// Exact native submitted answers are retained as locks, never replayed.
public enum QuestionTracking {
    public static func locked(_ draft: Draft) -> Bool { draft.status == "submitted" && !draft.questionId.isEmpty && draft.questionId != "__interrupt" }

    public static func settle(_ state: MessageState, _ draft: Draft, _ receipt: JSONObject) -> MessageState {
        let result = receipt.object("result")
        let checks = [("request_id", draft.requestId), ("name", draft.target.session), ("run_id", draft.target.run),
                      ("conversation_id", draft.target.conversation), ("question_id", draft.questionId), ("question_hash", draft.questionHash)]
        let exact = receipt.string("request_id") == draft.requestId && result != nil &&
            checks.allSatisfy { key, value in !(result!.has(key)) || result!.string(key) == value }
        let success = exact && receipt.string("state") == "completed" &&
            ["submitted", "answered", "skipped", "queued", "completed"].contains(result?.string("status") ?? "")
        var copy = state
        copy.drafts = state.drafts.compactMap { existing in
            guard existing.key == draft.key, existing.requestId == draft.requestId, existing.generation == draft.generation else { return existing }
            if success && draft.questionId == "__interrupt" { return nil }
            var updated = existing
            if success { updated.status = "submitted"; return updated }
            if locked(existing) { return existing }
            updated.status = receipt.string("request_id") == draft.requestId && receipt.string("state") == "failed" ? "failed" : "uncertain"
            return updated
        }
        return copy
    }

    public static func remember(_ state: MessageState, _ target: Target, _ questions: [Question]) -> MessageState {
        var drafts = state.drafts
        for question in questions {
            guard let delivery = QuestionPolicies.submitted(question, target), !delivery.requestId.isEmpty else { continue }
            let existing = drafts.first { $0.target == target && $0.questionId == question.id && $0.questionHash == question.hash && $0.detachedId.isEmpty }
            if let existing, ["submitting", "uncertain", "submitted"].contains(existing.status), existing.requestId != delivery.requestId { continue }
            let answers: JSON = .array(delivery.answers.map { answer in
                ["question_id": .string(answer.questionId), "selected_option_ids": JSON(answer.optionIds), "text": .string(answer.text), "skip": .bool(answer.skip)]
            })
            var locked = existing ?? Draft(target: target, text: "", questionId: question.id, questionHash: question.hash)
            locked.status = "submitted"
            locked.requestId = delivery.requestId
            locked.answers = answers.compact
            if locked != existing { drafts = drafts.filter { $0.key != locked.key } + [locked] }
        }
        guard drafts != state.drafts else { return state }
        var copy = state
        copy.drafts = drafts
        return copy
    }

    public static func restore(_ state: MessageState, _ target: Target, _ questions: [Question]) -> [Question] {
        questions.map { question in
            guard QuestionPolicies.submitted(question, target) == nil, question.source == "codex_async", question.optional,
                  question.run == target.run, question.conversation == target.conversation,
                  let draft = state.drafts.first(where: { $0.target == target && $0.questionId == question.id && $0.questionHash == question.hash &&
                      $0.detachedId.isEmpty && $0.status == "submitted" && !$0.requestId.isEmpty }),
                  let rows = (try? JSON.parse(draft.answers))?.array else { return question }
            var answers: [NativeQuestionAnswer] = []
            for row in rows {
                guard let value = row.object, case .string(let id)? = value["question_id"], let selected = value.array("selected_option_ids"),
                      case .string(let text)? = value["text"] else { return question }
                answers.append(NativeQuestionAnswer(questionId: id, optionIds: selected.compactMap { if case .string(let s) = $0 { s } else { nil } },
                                                    text: text, skip: value.bool("skip")))
            }
            var copy = question
            copy.answerDelivery = NativeQuestionDelivery(requestId: draft.requestId, questionId: question.id, questionHash: question.hash,
                                                         run: target.run, conversation: target.conversation, status: "submitted", answers: answers)
            copy.delivery = "submitted"
            return copy
        }
    }
}

/// Explicit answer intent retained by the app model, never persisted as consent.
public struct QuestionSendIntent: Hashable, Sendable {
    public let target: Target
    public let navigation: Int
    public let question: Question
    public let draft: Draft
    public let answers: String
    /// Kotlin compares the pending intent by reference; a fresh token gives each capture its own identity.
    let token = UUID()

    public func matches(target: Target?, navigation: Int, current: Question?, draft: Draft) -> Bool {
        guard self.target == target, self.navigation == navigation, let current else { return false }
        return QuestionSendPolicies.schema(question) == QuestionSendPolicies.schema(current) &&
            QuestionSendPolicies.available(current, self.target, answers) && ContextPolicies.sameDraft(self.draft, draft)
    }

    func ownsPrepared(_ prepared: Draft, _ requestId: String) -> Bool {
        guard !requestId.isEmpty, prepared.requestId == requestId, prepared.status == "submitting" else { return false }
        var expected = draft
        expected.answers = answers
        var candidate = prepared
        candidate.status = "editing"
        return ContextPolicies.sameDraft(expected, candidate)
    }

    public func restoreAfterExpiry(_ state: MessageState, requestId: String) -> MessageState {
        var copy = state
        copy.drafts = state.drafts.map { ownsPrepared($0, requestId) ? draft : $0 }
        return copy
    }

    public func permitsPrepared(target: Target?, navigation: Int, current: Question?, prepared: Draft, requestId: String, cold: Bool, consent: Bool) -> Bool {
        guard !requestId.isEmpty, prepared.requestId == requestId, prepared.status == "submitting",
              !QuestionSendPolicies.requiresCacheConsent(question, cold: cold) || consent else { return false }
        var editing = prepared
        editing.status = "editing"
        editing.answers = draft.answers
        return matches(target: target, navigation: navigation, current: current, draft: editing) && prepared.answers == answers
    }
}

public enum QuestionSendPolicies {
    /// Kimi's reviewed native chooser already asks how to handle its cold cache.
    public static func requiresCacheConsent(_ question: Question, cold: Bool) -> Bool {
        cold && !(question.source == "kimi_cache_hint" && question.toolCallId == "kimi_cache_hint")
    }

    public static func schema(_ question: Question) -> Question {
        var copy = question
        copy.canAnswer = false
        copy.canSkip = false
        copy.delivery = ""
        copy.createdAt = 0
        copy.unavailableReason = ""
        copy.answerDelivery = nil
        return copy
    }

    public static func capture(_ target: Target, navigation: Int, question: Question, draft: Draft, answers: String) -> QuestionSendIntent {
        QuestionSendIntent(target: target, navigation: navigation, question: schema(question), draft: draft, answers: answers)
    }

    public static func available(_ question: Question, _ target: Target, _ answers: String) -> Bool {
        if QuestionPolicies.submitted(question, target) != nil { return false }
        guard let rows = (try? JSON.parse(answers))?.array?.compactMap(\.object), rows.count == question.prompts.count, !rows.isEmpty,
              rows.map({ $0.string("question_id") }) == question.prompts.map(\.id) else { return false }
        let skip = rows.allSatisfy { $0.bool("skip") }
        return skip ? question.canSkip : (!rows.contains { $0.bool("skip") } && question.canAnswer)
    }
}

/// Refusals follow only the original target and question, including expanded cards.
public struct QuestionSendRefusal: Hashable, Sendable {
    public let target: Target
    public let questionId: String
    public let questionHash: String
    public let text: String
    public init(target: Target, questionId: String, questionHash: String, text: String) {
        self.target = target
        self.questionId = questionId
        self.questionHash = questionHash
        self.text = text
    }
    public func text(for target: Target, _ question: Question) -> String {
        target == self.target && question.id == questionId && question.hash == questionHash ? text : ""
    }
}

/// A pending confirmation cannot be replaced, replayed, or restored after process death.
public final class QuestionSendGate {
    public private(set) var pending: QuestionSendIntent?
    public init() {}

    public func request(_ intent: QuestionSendIntent, cold: Bool) -> QuestionSendIntent? {
        if pending != nil { return nil }
        if !QuestionSendPolicies.requiresCacheConsent(intent.question, cold: cold) { return intent }
        pending = intent
        return nil
    }
    public func active(_ expected: QuestionSendIntent) -> Bool { pending == expected }
    public func confirm(_ expected: QuestionSendIntent, valid: Bool) -> QuestionSendIntent? {
        guard active(expected) else { return nil }
        pending = nil
        return valid ? expected : nil
    }
    public func cancel(_ expected: QuestionSendIntent) { if pending == expected { pending = nil } }
}

/// One form attempt shared by the button and the keyboard submit. The model owns durability.
public final class QuestionSubmitGate {
    private var pending = false
    public init() {}
    public func claim(_ enabled: Bool) -> Bool {
        if !enabled || pending { return false }
        pending = true
        return true
    }
    public func finish() { pending = false }
}

/// Presentation ownership only; answers and delivery attempts remain in MessageState.
public struct QuestionDisplayState: Hashable, Sendable {
    public let owner: String
    public var chosen = ""
    public var expandedId = ""
    public var expandedHash = ""
    public private(set) var pages: [(String, Int)] = []
    public private(set) var offsets: [(String, Int)] = []

    public init(owner: String) { self.owner = owner }

    public func forTarget(_ key: String) -> QuestionDisplayState { owner == key ? self : QuestionDisplayState(owner: key) }
    public var expanded: Bool { !expandedId.trimmingCharacters(in: .whitespaces).isEmpty }
    public func matches(_ question: Question) -> Bool { expanded && question.id == expandedId && question.hash == expandedHash }
    public func resolve(_ questions: [Question]) -> Question? {
        let matching = questions.filter(matches)
        return matching.count == 1 ? matching[0] : nil
    }
    public func open(_ question: Question) -> QuestionDisplayState {
        var copy = self
        copy.chosen = QuestionPolicies.key(question)
        copy.expandedId = question.id
        copy.expandedHash = question.hash
        return copy
    }
    public func close() -> QuestionDisplayState {
        var copy = self
        copy.expandedId = ""
        copy.expandedHash = ""
        return copy
    }
    public func page(_ question: Question) -> Int { pages.last { $0.0 == Self.identity(question) }?.1 ?? 0 }
    public func offset(_ question: Question, page: Int) -> Int { offsets.last { $0.0 == Self.identity(question, page) }?.1 ?? 0 }
    public func withPage(_ question: Question, _ page: Int) -> QuestionDisplayState {
        var copy = self
        copy.pages = Self.bounded(pages, Self.identity(question), max(page, 0))
        return copy
    }
    public func withOffset(_ question: Question, page: Int, _ offset: Int) -> QuestionDisplayState {
        var copy = self
        copy.offsets = Self.bounded(offsets, Self.identity(question, page), max(offset, 0))
        return copy
    }

    static func identity(_ question: Question, _ page: Int? = nil) -> String {
        JSON.array([.string(question.id), .string(question.hash)] + (page.map { [.int(Int64($0))] } ?? [])).compact
    }
    static func bounded(_ values: [(String, Int)], _ key: String, _ value: Int) -> [(String, Int)] {
        Array((values.filter { $0.0 != key } + [(key, value)]).suffix(64))
    }

    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.owner == rhs.owner && lhs.chosen == rhs.chosen && lhs.expandedId == rhs.expandedId && lhs.expandedHash == rhs.expandedHash &&
            lhs.pages.elementsEqual(rhs.pages, by: ==) && lhs.offsets.elementsEqual(rhs.offsets, by: ==)
    }
    public func hash(into hasher: inout Hasher) {
        hasher.combine(owner)
        hasher.combine(expandedId)
        hasher.combine(expandedHash)
    }
}

/// Three continuous foreground, visible seconds; background time never approves an action.
public struct ApprovalReview: Hashable, Sendable {
    public var startedAt: Double?
    public init(startedAt: Double? = nil) { self.startedAt = startedAt }

    public func observe(eligible: Bool, now: Double) -> ApprovalReview {
        if !eligible || (startedAt.map { now < $0 } ?? false) { return ApprovalReview() }
        return startedAt == nil ? ApprovalReview(startedAt: now) : self
    }

    public func secondsRemaining(now: Double) -> Int {
        guard let startedAt else { return 3 }
        return min(max(3 - Int(max(now - startedAt, 0)), 0), 3)
    }
}

public struct PresentedQuestionReply: Hashable, Sendable {
    public let id: String
    public let question: String
    public let answer: String
}

/// Display-only parsing. Callers retain the original Event for receipts and delivery matching.
public enum QuestionReplyPresentation {
    static let open = "<send_user_message_question_reply>"
    static let close = "</send_user_message_question_reply>"
    static let ide = "# Context from my IDE setup:\n"
    static let request = "\n## My request for Codex:\n"

    public static func isOwnRole(_ role: String) -> Bool { role == "You" || role == "You (answer)" }

    public static func parse(_ original: String) -> [PresentedQuestionReply]? {
        if original.utf16.count > 256 * 1024 { return nil }
        var text = original.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.hasPrefix(ide) {
            guard let marker = text.range(of: request, options: .backwards) else { return nil }
            text = String(text[marker.upperBound...]).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard text.hasPrefix(open), text.hasSuffix(close), text.count >= open.count + close.count else { return nil }
        let body = String(text.dropFirst(open.count).dropLast(close.count))
        var grammar = SessionPreview.Cursor(body)
        guard grammar.complete(), let parsed = try? JSON.parse(body) else { return nil }
        let values: [JSON]
        switch parsed {
        case .object: values = [parsed]
        case .array(let items): values = items
        default: return nil
        }
        if values.isEmpty { return nil }
        var replies: [PresentedQuestionReply] = []
        for value in values {
            guard let reply = value.object, case .string(let id)? = reply["questionItemId"], case .string(let question)? = reply["question"],
                  case .string(let answer)? = reply["answer"], !id.isEmpty, !question.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
            replies.append(PresentedQuestionReply(id: id, question: question, answer: answer))
        }
        return replies
    }
}
