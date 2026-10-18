import SwiftUI
import ZerusKit

/// The selected native question, any submitted answers awaiting the agent, and optional-question paging.
struct QuestionsPane: View {
    @Environment(AppModel.self) private var app
    let target: Target
    let questions: [Question]
    let available: Bool
    @Binding var display: QuestionDisplayState
    let onReview: (Draft) -> Void
    @State private var shownSubmitted: Set<String> = []

    var body: some View {
        let queue = QuestionPolicies.queue(questions, target, chosen: display.chosen)
        VStack(alignment: .leading, spacing: 8) {
            ForEach(queue.submitted) { question in
                let key = QuestionPolicies.key(question)
                Text("Answer submitted. Waiting for the agent to record it.").font(.footnote).foregroundStyle(Theme.muted)
                Button(shownSubmitted.contains(key) ? "Hide submitted answer" : "Show submitted answer") {
                    if shownSubmitted.contains(key) { shownSubmitted.remove(key) } else { shownSubmitted.insert(key) }
                }
                .font(.footnote.weight(.semibold))
                if shownSubmitted.contains(key) && !display.matches(question) {
                    QuestionCard(target: target, question: question, available: false, display: $display, expanded: false, onReview: onReview)
                }
            }
            if let question = queue.selected {
                if question.optional && queue.optional.count > 1 {
                    HStack {
                        Button("Previous") { display.chosen = QuestionPolicies.key(queue.optional[queue.index - 1]) }.disabled(queue.index == 0)
                        Spacer()
                        Text("Question \(queue.index + 1)/\(queue.optional.count)").font(.caption.weight(.semibold))
                        Spacer()
                        Button("Next") { display.chosen = QuestionPolicies.key(queue.optional[queue.index + 1]) }
                            .disabled(queue.index + 1 >= queue.optional.count)
                    }
                    .font(.footnote.weight(.semibold))
                }
                if !display.matches(question) {
                    QuestionCard(target: target, question: question, available: available, display: $display, expanded: false, onReview: onReview)
                }
            }
        }
    }
}

/// One native request with exact answers. Approvals require three continuous visible seconds.
struct QuestionCard: View {
    @Environment(AppModel.self) private var app
    @Environment(\.scenePhase) private var scenePhase
    let target: Target
    let question: Question
    let available: Bool
    @Binding var display: QuestionDisplayState
    let expanded: Bool
    let onReview: (Draft) -> Void

    @State private var options: [String: Set<String>] = [:]
    @State private var texts: [String: String] = [:]
    @State private var loadedIdentity = ""
    @State private var gate = QuestionSubmitGate()
    @State private var submitting = false
    @State private var review = ApprovalReview()
    @State private var now = ProcessInfo.processInfo.systemUptime
    @State private var visible = false
    @State private var confirmation: PermissionConfirmation?

    private var model: ConversationModel { app.conversation }

    var body: some View {
        let draft = model.draft(target, question)
        let native = QuestionPolicies.submitted(question, target)
        let identity = "\(target.key)|\(QuestionPolicies.key(question))|\(native?.requestId ?? "")|\(draft.generation)"
        let editing = isEditing(draft, native)
        // A card that cannot be answered here is read-only, so no draft is written that could never be sent.
        let inputs = editing && question.canAnswer
        // Only the final step sends; Next moves on once this part's answer is valid.
        let step = QuestionSteps.current(question.prompts, page: display.page(question), options: options, texts: texts)
        let page = step.page
        let prompts = question.prompts.indices.contains(page) ? [question.prompts[page]] : []
        let otherPrompts = prompts.filter { $0.other && !question.approval }
        let error = QuestionPolicies.answerError(question.prompts, options: options, texts: texts)
        let complete = QuestionPolicies.complete(question.prompts, options: options, texts: texts)
        let sendEnabled = question.canAnswer && editing && complete && available && !submitting
        let actionEnabled = step.nextPage != nil ? editing && !submitting && step.canAdvance : sendEnabled
        // A card that cannot be answered here shows its reason and no Submit.
        let showsSubmit = QuestionPolicies.showsSubmit(question)
        let approvalSupported = QuestionPolicies.approvalSupported(question)
        let permission = QuestionPolicies.openCodePermission(question)
        let eligible = visible && scenePhase == .active && available && editing && question.canAnswer && (approvalSupported || permission)
        // The review belongs to this card: another card shown here starts its own three seconds.
        let reviewCard = ApprovalReview.key(target, question)
        let remaining = review.secondsRemaining(now: now, card: reviewCard)
        let reviewed = eligible && remaining == 0
        let answerable = question.canAnswer && editing && available
        // The Always allow dialog is shown only while confirming would send; one that cannot send (including in the
        // background or out of view) is dropped.
        let alwaysSendable = confirmation?.confirm(question, target: target, editing: editing, available: available, foreground: scenePhase == .active,
                                                   visible: visible, review: review, now: now) != nil
        let staleConfirmation = confirmation != nil && !alwaysSendable

        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(question.approval || question.trustRequest ? "Needs approval" : (question.optional ? "Optional question" : "Needs input"))
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(Theme.amber)
                Spacer()
                if !expanded && question.createdAt > 0 {
                    Text(Date(timeIntervalSince1970: question.createdAt), format: .dateTime.day().month().hour().minute())
                        .font(.caption2).foregroundStyle(Theme.muted)
                }
                Button(expanded ? "Collapse" : "Expand") { display = expanded ? display.close() : display.open(question) }
                    .font(.footnote.weight(.semibold))
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(prompts, id: \.id) { prompt in
                        promptView(prompt, editing: inputs)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: expanded ? .infinity : 360)
            .fixedSize(horizontal: false, vertical: !expanded)
            .onScrollVisibilityChange(threshold: 0.3) { visible = $0 }
            ForEach(Array(otherPrompts.enumerated()), id: \.element.id) { index, prompt in
                HStack(spacing: 8) {
                    TextField(otherLabel(prompt), text: Binding(get: { texts[prompt.id] ?? "" }, set: { value in
                        texts[prompt.id] = value
                        if !prompt.multi && !value.trimmingCharacters(in: .whitespaces).isEmpty { options[prompt.id] = [] }
                        persist()
                    }))
                    .textFieldStyle(.roundedBorder)
                    .disabled(!inputs)
                    .submitLabel(step.nextPage != nil ? .next : .send)
                    .onSubmit { primaryAction(step, editing: editing) }
                    if index == otherPrompts.count - 1 && showsSubmit { actionButton(step, enabled: actionEnabled, editing: editing) }
                }
                if !QuestionPolicies.textHint(prompt).isEmpty {
                    Text(QuestionPolicies.textHint(prompt)).font(.footnote).foregroundStyle(Theme.muted)
                }
            }
            status(draft: draft, native: native)
            if !error.isEmpty { Text(error).font(.footnote).foregroundStyle(Theme.amber) }
            let notice = model.questionSendNotice(target, question)
            if !notice.isEmpty { Text(notice).font(.footnote).foregroundStyle(Theme.amber) }
            if question.approval && permission {
                let reject = PermissionAction.reject, once = PermissionAction.allowOnce, always = PermissionAction.alwaysAllow
                HStack(spacing: 8) {
                    Button(reject.label(remaining: remaining)) { approve(reject.option) }
                        .buttonStyle(.bordered)
                        .disabled(!reject.enabled(answerable: answerable, reviewed: reviewed))
                    Button(once.label(remaining: remaining)) { approve(once.option) }
                        .buttonStyle(.borderedProminent)
                        .disabled(!once.enabled(answerable: answerable, reviewed: reviewed))
                }
                // Always allow reaches beyond this request, so it opens its own confirmation.
                if PermissionAction.actions(question).contains(always) {
                    Button(always.label(remaining: remaining)) { confirmation = PermissionConfirmation(question) }
                        .buttonStyle(.bordered)
                        .disabled(!always.enabled(answerable: answerable, reviewed: reviewed))
                }
                Text("Reject rejects every pending request in this session.").font(.footnote).foregroundStyle(Theme.muted)
            } else if question.approval {
                HStack(spacing: 8) {
                    Button("Deny") { approve("deny") }
                        .buttonStyle(.bordered)
                        .disabled(!(approvalSupported && question.canAnswer && editing && available))
                    Button(remaining > 0 ? "Approve once (\(remaining))" : "Approve once") { approve("allow") }
                        .buttonStyle(.borderedProminent)
                        .disabled(!reviewed)
                }
                Text("Approval applies only to this command. Review it before continuing.").font(.footnote).foregroundStyle(Theme.muted)
            } else if question.prompts.count > 1 || question.canSkip || otherPrompts.isEmpty {
                HStack(spacing: 4) {
                    if question.prompts.count > 1 {
                        Button { display = display.withPage(question, page - 1) } label: { Image(systemName: "chevron.left") }
                            .disabled(page == 0).accessibilityLabel("Previous part")
                        Text("Question \(page + 1)/\(question.prompts.count)").font(.caption.weight(.semibold))
                        Button { display = display.withPage(question, page + 1) } label: { Image(systemName: "chevron.right") }
                            .disabled(page + 1 >= question.prompts.count).accessibilityLabel("Next part")
                    }
                    Spacer()
                    if question.canSkip {
                        Button("Skip") { skip() }.disabled(!(editing && available))
                    }
                    if otherPrompts.isEmpty && showsSubmit { actionButton(step, enabled: actionEnabled, editing: editing) }
                }
            }
            if !available {
                Text("Refresh to verify this request. Your answer is kept.").font(.footnote).foregroundStyle(Theme.muted)
            } else if !question.canAnswer {
                Text(question.unavailableReason.isEmpty ? "Complete this request on your machine." : question.unavailableReason)
                    .font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled)
            }
            if question.approval && !approvalSupported && !permission {
                Text("This approval cannot be answered here. Continue on your machine.").font(.footnote).foregroundStyle(Theme.muted)
            }
        }
        .padding(12)
        .background(Color(hex: 0x2B2923), in: RoundedRectangle(cornerRadius: 14))
        .confirmationDialog("Always allow?", isPresented: Binding(get: { alwaysSendable }, set: { if !$0 { confirmation = nil } }),
                            titleVisibility: .visible, presenting: confirmation) { pending in
            Button("Always allow", role: .destructive) { confirmAlways(pending) }
            Button("Cancel", role: .cancel) { confirmation = nil }
        } message: { pending in
            Text(verbatim: pending.message)
        }
        // A confirmation that cannot send (its card changed, can no longer be answered or edited here, left the foreground or
        // view, or needs a new review) closes without sending.
        .onChange(of: staleConfirmation) { _, stale in if stale { confirmation = nil } }
        .onAppear { load(identity: identity, draft: draft, native: native) }
        .onChange(of: identity) { load(identity: identity, draft: draft, native: native) }
        .onChange(of: eligible, initial: true) { observeReview(eligible: eligible, card: reviewCard) }
        .onChange(of: reviewCard) { observeReview(eligible: eligible, card: reviewCard) }
        // The countdown clock only advances a review that eligibility started; once cancelled it stops without another tick.
        .task(id: "\(eligible && question.approval)|\(reviewCard)") {
            guard eligible && question.approval else { return }
            while true {
                do { try await Task.sleep(for: .milliseconds(200)) } catch { return }
                guard !Task.isCancelled else { return }
                now = ProcessInfo.processInfo.systemUptime
                review = review.tick(now: now, card: reviewCard)
            }
        }
    }

    @ViewBuilder
    private func promptView(_ prompt: Prompt, editing: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if !prompt.header.isEmpty { Text(prompt.header).font(.footnote.weight(.semibold)).foregroundStyle(Theme.muted) }
            if question.approval {
                Text(verbatim: prompt.text).fontWeight(.semibold).textSelection(.enabled)
                if !prompt.body.isEmpty { Text(verbatim: prompt.body).font(.footnote.monospaced()).textSelection(.enabled) }
            } else {
                MarkdownContent(source: prompt.text).fontWeight(.semibold)
                if !prompt.body.isEmpty { MarkdownContent(source: prompt.body) }
            }
            if !prompt.otherDescription.isEmpty {
                if question.approval { Text(verbatim: prompt.otherDescription).font(.footnote).textSelection(.enabled) }
                else { MarkdownContent(source: prompt.otherDescription).font(.footnote) }
            }
            ForEach(prompt.choices, id: \.id) { choice in
                if question.approval {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: choice.label).font(.footnote)
                        if !choice.description.isEmpty { Text(verbatim: choice.description).font(.footnote).foregroundStyle(Theme.muted) }
                    }
                    .textSelection(.enabled)
                } else {
                    QuestionOption(choice: choice, multi: prompt.multi, selected: options[prompt.id]?.contains(choice.id) == true, enabled: editing) {
                        var selected = options[prompt.id] ?? []
                        if prompt.multi {
                            if selected.contains(choice.id) { selected.remove(choice.id) } else { selected.insert(choice.id) }
                        } else {
                            selected = [choice.id]
                            texts[prompt.id] = ""
                        }
                        options[prompt.id] = selected
                        persist()
                    }
                }
            }
            if !QuestionPolicies.choiceHint(prompt).isEmpty {
                Text(QuestionPolicies.choiceHint(prompt)).font(.footnote).foregroundStyle(Theme.muted)
            }
        }
    }

    @ViewBuilder
    private func status(draft: Draft, native: NativeQuestionDelivery?) -> some View {
        if let native {
            Text("Answer submitted. Waiting for the agent to record it.").font(.footnote)
            if native.answers.contains(where: \.skip) { Text("Skipped").font(.footnote) }
        } else {
            let state = model.answerStatus(draft)
            if state != "editing" {
                HStack(spacing: 8) {
                    if state == "submitting" { ProgressView().controlSize(.small) }
                    Text(Self.statusText(state)).font(.footnote).foregroundStyle(Theme.amber)
                }
                if ["uncertain", "failed"].contains(state) {
                    HStack {
                        if !draft.requestId.isEmpty {
                            Button("Check delivery") { Task { await model.checkReceipt(draft) } }.disabled(model.receiptFlights.contains(draft.key))
                        }
                        Button("Review") { onReview(draft) }.disabled(model.receiptFlights.contains(draft.key))
                    }
                    .font(.footnote.weight(.semibold))
                }
            }
        }
    }

    static func statusText(_ state: String) -> String {
        switch state {
        case "submitting": "Sending answer…"
        case "submitted": "Answer submitted. Waiting for the agent."
        case "failed": "Not sent. Your answer is kept."
        default: "Delivery unknown. Your answer may already have reached the agent."
        }
    }

    /// Next, Review answers, Send all or Send, as on Android: one action per step.
    private func actionButton(_ step: QuestionStep, enabled: Bool, editing: Bool) -> some View {
        let label = step.reviewing ? "Review answers" : step.nextPage != nil ? "Next" : step.count > 1 ? "Send all" : "Send"
        let description = step.reviewing ? "Review unanswered questions" : step.nextPage != nil ? "Next question" : step.count > 1 ? "Send all" : "Send answer"
        return Button { primaryAction(step, editing: editing) } label: {
            HStack(spacing: 6) {
                Text(label)
                Image(systemName: step.nextPage != nil ? "chevron.right" : "arrow.up")
            }
            .font(.footnote.weight(.semibold))
            .padding(.horizontal, 12)
            .frame(minHeight: 44)
        }
        .buttonStyle(.plain)
        .foregroundStyle(enabled ? Theme.background : Theme.muted)
        .background(enabled ? Theme.mint : Color(hex: 0x2B3B3B), in: Capsule())
        .disabled(!enabled)
        .accessibilityLabel(description)
    }

    /// Moves to the step's next page after saving the answer, or sends from the final step through the unchanged send.
    private func primaryAction(_ step: QuestionStep, editing: Bool) {
        guard let next = step.nextPage else { return submit(editing: editing) }
        guard editing && !submitting && step.canAdvance else { return }
        persist()
        display = display.withPage(question, next)
    }

    private func otherLabel(_ prompt: Prompt) -> String {
        if !prompt.otherLabel.isEmpty { return prompt.otherLabel }
        // A text form item has no options, so its field is the answer itself.
        let text = prompt.inputKind == "text"
        if question.prompts.count > 1 { return "\(text ? "Answer" : "Other"): \(prompt.header.isEmpty ? prompt.text : prompt.header)" }
        return text ? "Answer" : "Other answer"
    }

    private func load(identity: String, draft: Draft, native: NativeQuestionDelivery?) {
        guard loadedIdentity != identity else { return }
        loadedIdentity = identity
        gate = QuestionSubmitGate()
        submitting = false
        confirmation = nil
        // This question's saved draft wins; form defaults and default_text only seed an unsaved one and are never sent by themselves.
        let opening = QuestionPolicies.opening(question.prompts, draftAnswers: draft.answers, native: native)
        options = opening.options
        texts = opening.texts
    }

    private func answers() -> JSON { QuestionPolicies.answers(question.prompts, options: options, texts: texts) }

    private func persist() {
        let text = question.prompts.compactMap { texts[$0.id] }.filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }.joined(separator: "\n")
        model.edit(target, text: text, question: question, answers: answers().compact)
    }

    private func submit(editing: Bool) {
        let allowed = question.canAnswer && editing && available && !submitting && model.draft(target, question).status == "editing" &&
            QuestionPolicies.complete(question.prompts, options: options, texts: texts)
        guard gate.claim(allowed) else { return }
        submitting = true
        persist()
        let payload = answers().compact
        Task {
            await model.sendQuestion(target, question, answers: payload)
            gate.finish()
            submitting = false
        }
    }

    private func approve(_ option: String) {
        guard let payload = QuestionPolicies.approvalAnswers(question, option)?.compact else { return }
        Task { await model.sendQuestion(target, question, answers: payload) }
    }

    /// Sends Always allow only for the card that offered it, while it is answerable, editable, in the foreground, visible
    /// and still reviewed; all of it is checked again now.
    private func confirmAlways(_ pending: PermissionConfirmation) {
        confirmation = nil
        let editing = isEditing(model.draft(target, question), QuestionPolicies.submitted(question, target))
        guard let payload = pending.confirm(question, target: target, editing: editing, available: available, foreground: scenePhase == .active,
                                            visible: visible, review: review, now: ProcessInfo.processInfo.systemUptime)?.compact else { return }
        Task { await model.sendQuestion(target, question, answers: payload) }
    }

    /// The answer can still be edited and sent: no attempt, native submission or pending send confirmation holds it.
    private func isEditing(_ draft: Draft, _ native: NativeQuestionDelivery?) -> Bool {
        draft.status == "editing" && native == nil && model.pendingQuestionSend == nil
    }

    private func observeReview(eligible: Bool, card: String) {
        now = ProcessInfo.processInfo.systemUptime
        review = review.observe(eligible: eligible, now: now, card: card)
    }

    private func skip() {
        let payload = JSON.array(question.prompts.map { ["question_id": .string($0.id), "skip": true, "selected_option_ids": [], "text": ""] }).compact
        Task { await model.sendQuestion(target, question, answers: payload) }
    }
}

struct QuestionOption: View {
    let choice: Choice
    let multi: Bool
    let selected: Bool
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: multi ? (selected ? "checkmark.square.fill" : "square") : (selected ? "largecircle.fill.circle" : "circle"))
                    .foregroundStyle(selected ? Theme.mint : Theme.muted)
                VStack(alignment: .leading, spacing: 2) {
                    Text(verbatim: choice.label).foregroundStyle(Theme.onSurface)
                    if !choice.description.isEmpty { MarkdownContent(source: choice.description).font(.footnote).foregroundStyle(Theme.muted) }
                }
                Spacer(minLength: 0)
            }
            .padding(10)
            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 8))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// The expanded question lives outside the history list, so scrolling never dismisses it.
struct ExpandedQuestionSheet: View {
    @Environment(AppModel.self) private var app
    let target: Target
    let questions: [Question]
    let available: Bool
    @Binding var display: QuestionDisplayState
    let onReview: (Draft) -> Void

    var body: some View {
        NavigationStack {
            Group {
                if let question = display.resolve(questions) {
                    QuestionCard(target: target, question: question, available: available, display: $display, expanded: true, onReview: onReview)
                        .padding(8)
                } else {
                    ContentUnavailableView("Question unavailable", systemImage: "questionmark.bubble",
                                           description: Text("This exact request is not in the loaded activity. Refresh the conversation to verify it. Your saved answer is kept."))
                }
            }
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Close") { display = display.close() } } }
            .zerusScreen()
        }
    }
}
