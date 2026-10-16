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

    private var model: ConversationModel { app.conversation }

    var body: some View {
        let draft = model.draft(target, question)
        let native = QuestionPolicies.submitted(question, target)
        let identity = "\(target.key)|\(QuestionPolicies.key(question))|\(native?.requestId ?? "")|\(draft.generation)"
        let editing = draft.status == "editing" && native == nil && model.pendingQuestionSend == nil
        let page = min(max(display.page(question), 0), max(question.prompts.count - 1, 0))
        let prompts = question.prompts.indices.contains(page) ? [question.prompts[page]] : []
        let otherPrompts = prompts.filter { $0.other && !question.approval }
        let error = QuestionPolicies.answerError(question.prompts, options: options, texts: texts)
        let complete = QuestionPolicies.complete(question.prompts, options: options, texts: texts)
        let sendEnabled = question.canAnswer && editing && complete && available && !submitting
        let approvalSupported = QuestionPolicies.approvalSupported(question)
        let eligible = visible && scenePhase == .active && available && editing && question.canAnswer && approvalSupported
        let remaining = review.secondsRemaining(now: now)

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
                        promptView(prompt, editing: editing)
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
                    .disabled(!editing)
                    .submitLabel(.send)
                    .onSubmit { submit(editing: editing) }
                    if index == otherPrompts.count - 1 { sendButton(enabled: sendEnabled, multiple: question.prompts.count > 1, editing: editing) }
                }
            }
            status(draft: draft, native: native)
            if !error.isEmpty { Text(error).font(.footnote).foregroundStyle(Theme.amber) }
            let notice = model.questionSendNotice(target, question)
            if !notice.isEmpty { Text(notice).font(.footnote).foregroundStyle(Theme.amber) }
            if question.approval {
                HStack(spacing: 8) {
                    Button("Deny") { approve("deny") }
                        .buttonStyle(.bordered)
                        .disabled(!(approvalSupported && question.canAnswer && editing && available))
                    Button(remaining > 0 ? "Approve once (\(remaining))" : "Approve once") { approve("allow") }
                        .buttonStyle(.borderedProminent)
                        .disabled(!(eligible && remaining == 0))
                }
                Text("Approval applies only to this command. Review it before continuing.").font(.footnote).foregroundStyle(Theme.muted)
            } else if question.prompts.count > 1 || question.canSkip || otherPrompts.isEmpty {
                HStack(spacing: 4) {
                    if question.prompts.count > 1 {
                        Button { display = display.withPage(question, page - 1) } label: { Image(systemName: "chevron.left") }
                            .disabled(page == 0).accessibilityLabel("Previous part")
                        Text("\(page + 1)/\(question.prompts.count)").font(.caption.weight(.semibold))
                        Button { display = display.withPage(question, page + 1) } label: { Image(systemName: "chevron.right") }
                            .disabled(page + 1 >= question.prompts.count).accessibilityLabel("Next part")
                    }
                    Spacer()
                    if question.canSkip {
                        Button("Skip") { skip() }.disabled(!(editing && available))
                    }
                    if otherPrompts.isEmpty { sendButton(enabled: sendEnabled, multiple: question.prompts.count > 1, editing: editing) }
                }
            }
            if !available {
                Text("Refresh to verify this request. Your answer is kept.").font(.footnote).foregroundStyle(Theme.muted)
            } else if !question.canAnswer {
                Text(question.unavailableReason.isEmpty ? "Complete this request on your machine." : question.unavailableReason)
                    .font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled)
            }
            if question.approval && !approvalSupported {
                Text("This approval cannot be answered here. Continue on your machine.").font(.footnote).foregroundStyle(Theme.muted)
            }
        }
        .padding(12)
        .background(Color(hex: 0x2B2923), in: RoundedRectangle(cornerRadius: 14))
        .onAppear { load(identity: identity, draft: draft, native: native) }
        .onChange(of: identity) { load(identity: identity, draft: draft, native: native) }
        .onChange(of: eligible, initial: true) {
            now = ProcessInfo.processInfo.systemUptime
            review = review.observe(eligible: eligible, now: now)
        }
        .task(id: eligible && question.approval) {
            guard eligible && question.approval else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(200))
                now = ProcessInfo.processInfo.systemUptime
                review = review.observe(eligible: true, now: now)
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

    private func sendButton(enabled: Bool, multiple: Bool, editing: Bool) -> some View {
        Button { submit(editing: editing) } label: {
            Image(systemName: "arrow.up").font(.body.weight(.semibold)).frame(width: 36, height: 36)
        }
        .buttonStyle(.plain)
        .foregroundStyle(enabled ? Theme.background : Theme.muted)
        .background(enabled ? Theme.mint : Color(hex: 0x2B3B3B), in: Circle())
        .disabled(!enabled)
        .accessibilityLabel(multiple ? "Send all" : "Send answer")
    }

    private func otherLabel(_ prompt: Prompt) -> String {
        if !prompt.otherLabel.isEmpty { return prompt.otherLabel }
        return question.prompts.count > 1 ? "Other: \(prompt.header.isEmpty ? prompt.text : prompt.header)" : "Other answer"
    }

    private func load(identity: String, draft: Draft, native: NativeQuestionDelivery?) {
        guard loadedIdentity != identity else { return }
        loadedIdentity = identity
        gate = QuestionSubmitGate()
        submitting = false
        var saved: [String: NativeQuestionAnswer] = [:]
        if let native {
            for answer in native.answers { saved[answer.questionId] = answer }
        } else if let rows = (try? JSON.parse(draft.answers))?.array {
            for row in rows.compactMap(\.object) {
                let id = row.string("question_id")
                saved[id] = NativeQuestionAnswer(questionId: id, optionIds: row.strings("selected_option_ids"), text: row.string("text"), skip: row.bool("skip"))
            }
        }
        options = Dictionary(uniqueKeysWithValues: question.prompts.map { ($0.id, Set(saved[$0.id]?.optionIds ?? [])) })
        texts = Dictionary(uniqueKeysWithValues: question.prompts.map { ($0.id, saved[$0.id]?.text ?? "") })
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
        guard let prompt = question.prompts.first else { return }
        let payload = JSON.array([["question_id": .string(prompt.id), "selected_option_ids": [.string(option)], "text": ""]]).compact
        Task { await model.sendQuestion(target, question, answers: payload) }
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
