import SwiftUI
import ZerusKit

/// The message editor. Text is saved privately as it is typed; a message is sent only
/// after its outgoing record is durable, and persistence echoes never rewrite the editor.
struct Composer: View {
    @Environment(AppModel.self) private var app
    let session: Session
    let online: Bool
    let onReview: (Draft) -> Void

    @State private var text = ""
    @State private var generation = ""
    @State private var coldDraft: Draft?
    @State private var coldClear = false
    @State private var blockedReason: String?
    @FocusState private var focused: Bool

    private var model: ConversationModel { app.conversation }
    private var target: Target { session.target }

    var body: some View {
        let draft = model.draft(target)
        let reason = sendReason(draft)
        let canSend = reason.isEmpty && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        let previous = model.normalizedOutgoing.last { $0.belongs(to: target) && ["sending", "uncertain"].contains($0.status) }
        VStack(alignment: .leading, spacing: 6) {
            ContextFooter(session: session, captureDraft: capture)
            if draft.status != "editing" {
                HStack {
                    Text(DeliveryLabel.text(draft.status) + ". The original draft is saved.").font(.caption).foregroundStyle(Theme.amber)
                    Spacer()
                    Button("Review") { onReview(draft) }
                        .font(.caption.weight(.semibold))
                        .disabled(draft.status == "submitting" || model.receiptFlights.contains(draft.key))
                }
            }
            if previous?.status == "sending" {
                Text("Sending… You can write the next message.").font(.caption).foregroundStyle(Theme.muted)
            } else if previous?.status == "uncertain" {
                Text("Delivery unknown. Check the previous message before sending again.").font(.caption).foregroundStyle(Theme.amber)
            }
            if !model.contextNotice.isEmpty {
                Text(model.contextNotice).font(.caption).foregroundStyle(Theme.amber)
            }
            HStack(alignment: .bottom, spacing: 8) {
                TextField(reason.isEmpty ? "Message" : reason, text: $text, axis: .vertical)
                    .lineLimit(1...6)
                    .focused($focused)
                    .disabled(draft.status != "editing" || app.demo)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 9)
                    .background(Theme.surface, in: RoundedRectangle(cornerRadius: 20))
                Button {
                    send(canSend: canSend, reason: reason)
                } label: {
                    Image(systemName: canSend || reason.isEmpty ? "arrow.up" : "info")
                        .font(.body.weight(.semibold))
                        .frame(width: 40, height: 40)
                }
                .buttonStyle(.plain)
                .foregroundStyle(canSend ? Theme.background : Theme.muted)
                .background(canSend ? Theme.mint : Color(hex: 0x2B3B3B), in: Circle())
                .disabled(!canSend && reason.isEmpty)
                .accessibilityLabel(canSend || reason.isEmpty ? "Send message" : "Why sending is unavailable")
            }
        }
        .padding(.horizontal, 12)
        .padding(.top, 6)
        .padding(.bottom, 8)
        .background(.bar)
        .onAppear { reconcile(draft) }
        .onChange(of: draft.generation) { reconcile(model.draft(target)) }
        .onChange(of: text) { _, value in
            if value != model.draft(target).text { model.edit(target, text: value) }
        }
        .alert("Sending unavailable", isPresented: Binding(get: { blockedReason != nil }, set: { if !$0 { blockedReason = nil } })) {
            Button("Close", role: .cancel) { blockedReason = nil }
        } message: {
            Text(blockedReason ?? "")
        }
        .alert("Continue with a cold cache?", isPresented: Binding(get: { coldDraft != nil }, set: { if !$0 { coldDraft = nil } })) {
            Button("Send with full context") {
                guard let captured = coldDraft else { return }
                coldDraft = nil
                let current = model.draft(captured.target)
                guard ContextPolicies.sameDraft(captured, current) else {
                    model.contextNotice = "Your composition changed. Review it before sending."
                    return
                }
                Task { await model.sendMessage(captured.target, allowColdCache: true) }
            }
            Button("Compact and continue") {
                guard let captured = coldDraft else { return }
                coldDraft = nil
                model.compactContext(captured.target, continueDraft: true, expectedDraft: captured)
            }
            .disabled(!(model.contextActionReason(target, "compact_context").isEmpty &&
                        ContextPresentation.from(model.activity, recorded: !model.activityVerified).compactReason.isEmpty))
            Button("Clear context", role: .destructive) {
                coldDraft = nil
                coldClear = true
            }
            .disabled(!(model.contextActionReason(target, "clear_context").isEmpty &&
                        ContextPresentation.from(model.activity, recorded: !model.activityVerified).clearReason.isEmpty))
            Button("Cancel", role: .cancel) { coldDraft = nil }
        } message: {
            Text("The next message may process the full conversation again, increasing token use or cost. Send with full context, compact it into a summary first, or clear it and keep your draft. Compaction uses tokens and may omit details.")
        }
        .clearContextConfirmation(isPresented: $coldClear, session: session, beforeClear: { _ = capture() })
    }

    /// Saves the editor text and returns the exact draft a context action captures.
    private func capture() -> Draft {
        if model.draft(target).text != text { model.edit(target, text: text) }
        return model.draft(target)
    }

    private func reconcile(_ draft: Draft) {
        guard model.draft(target).generation == draft.generation else { return }
        let update = ComposerReconciler.external(current: generation, generation: draft.generation, text: draft.text)
        generation = update.generation
        if let replacement = update.replacement { text = replacement }
    }

    private func sendReason(_ draft: Draft) -> String {
        let block: String
        if model.activityVerified {
            block = target.agentId.isEmpty ? NativeEvents.messageBlockReason(model.activity) : model.childSendReason(target)
        } else {
            block = "Refresh to verify the session before sending."
        }
        let previous = model.normalizedOutgoing.last { $0.belongs(to: target) && ["sending", "uncertain"].contains($0.status) }
        if app.demo { return "Pair to send a message." }
        if !app.storageReady { return "Private storage is not ready. Sending is unavailable." }
        if draft.status == "submitting" { return "Sending the saved draft. Wait for its result." }
        if draft.status != "editing" { return "Review the saved draft before sending again." }
        if !draft.attachments.isEmpty { return "Attachments are not available on this phone yet." }
        if !block.isEmpty { return block }
        if !online || target.run.isEmpty { return "Refresh to verify the session before sending." }
        if previous?.status == "uncertain" { return "Delivery unknown. Check the previous message before sending again." }
        if previous?.status == "sending" { return "Sending your previous message. You can write the next message." }
        if model.contextBlocked(target) { return "Check the pending session command before sending." }
        if model.sendingBlocked(target) { return "Preparing your message. You can write the next message." }
        return ""
    }

    private func send(canSend: Bool, reason: String) {
        guard canSend else {
            if !reason.isEmpty { blockedReason = reason }
            return
        }
        model.edit(target, text: text)
        let captured = model.draft(target)
        if ContextPresentation.isCold(model.activity) {
            coldDraft = captured
        } else {
            Task { await model.sendMessage(target) }
        }
    }
}

/// Native context and cache state with its pending command; tapping opens details and context actions.
struct ContextFooter: View {
    @Environment(AppModel.self) private var app
    let session: Session
    /// Saves the editor text before a context action, so the captured draft is current.
    var captureDraft: () -> Draft = { Draft(target: Target(computerId: "", session: "", run: "", conversation: ""), text: "") }
    @State private var details = false

    var body: some View {
        let model = app.conversation!
        let target = session.target
        let recorded = !model.activityVerified || !target.archiveId.isEmpty || app.demo
        let operation = model.contextOperation(target)
        let status = operation.map { ActionText.context($0.operation, $0.status) } ?? ""
        TimelineView(.periodic(from: .now, by: ContextPresentation.countdownMinute(model.activity, now: Date().timeIntervalSince1970) == nil ? 60 : 1)) { timeline in
            let view = ContextPresentation.from(model.activity, recorded: recorded, now: timeline.date.timeIntervalSince1970)
            Button { details = true } label: {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 10) {
                        Text(view.summary).foregroundStyle(Color(argb: ContextPresentation.color(view.tone)))
                        Spacer(minLength: 4)
                        Text(view.cacheSummary).foregroundStyle(Color(argb: ContextPresentation.cacheColor(view.cacheTone)))
                        Image(systemName: "info.circle").foregroundStyle(Theme.muted)
                    }
                    let second = [view.recorded ? "Last known" : "", status.isEmpty ? view.compactionSummary : status].filter { !$0.isEmpty }.joined(separator: " / ")
                    if !second.isEmpty { Text(second).foregroundStyle(Theme.muted) }
                }
                .font(.caption2)
                .lineLimit(1)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Context and cache details: \(view.summary), \(view.cacheSummary)")
            .sheet(isPresented: $details) { ContextSheet(session: session, view: view, captureDraft: captureDraft) }
        }
    }
}

struct ContextSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let session: Session
    let view: ContextView
    let captureDraft: () -> Draft
    @State private var clear = false
    @State private var review: ContextOperation?
    @State private var restore: Draft?

    var body: some View {
        let model = app.conversation!
        let target = session.target
        let operation = model.contextOperation(target)
        let available = !app.demo && app.storageReady && model.activityVerified && !model.contextBlocked(target) && !model.sendingBlocked(target)
        let compactReason = model.contextActionReason(target, "compact_context").nonEmpty ?? view.compactReason
        let clearReason = model.contextActionReason(target, "clear_context").nonEmpty ?? view.clearReason
        let current = model.draft(target)
        NavigationStack {
            List {
                Section {
                    ForEach(view.details, id: \.self) { LabeledContent($0.label, value: $0.value).font(.footnote) }
                } header: {
                    Text(SessionFilters.label(session) + (view.recorded ? " / Last known values / Read-only" : ""))
                }
                Section { ForEach(view.notes, id: \.self) { Text($0).font(.footnote).foregroundStyle(Theme.muted) } }
                if let operation {
                    Section {
                        Text(ActionText.context(operation.operation, operation.status)).font(.subheadline.weight(.semibold))
                        if !operation.error.isEmpty { Text(operation.error).font(.footnote).foregroundStyle(Theme.amber) }
                        let flight = model.receiptFlights.contains("context:\(operation.requestId)")
                        let enabled = model.activityVerified && !app.demo && target.archiveId.isEmpty && !flight
                        if ["submitted", "uncertain"].contains(operation.status) { Button("Check result") { model.checkContextOperation(operation) }.disabled(!enabled) }
                        if ["failed", "uncertain"].contains(operation.status) { Button("Review") { review = operation }.disabled(!enabled) }
                    }
                }
                if !model.contextNotice.isEmpty { Text(model.contextNotice).font(.footnote).foregroundStyle(Theme.muted) }
                let saved = model.contextDrafts(target)
                if !saved.isEmpty {
                    Section("Drafts from earlier context") {
                        ForEach(saved, id: \.key) { previous in
                            VStack(alignment: .leading, spacing: 4) {
                                Text(previous.text.isEmpty ? "Saved files" : previous.text).font(.footnote).lineLimit(3)
                                Button("Review and restore") { restore = previous }
                                    .disabled(app.demo || !model.canRestoreContextDraft(previous, target))
                                    .buttonStyle(.borderless)
                            }
                        }
                    }
                }
                if model.compactContinuationActive {
                    Section {
                        Text("The draft stays saved while compaction is pending. Changes cancel the pending send.").font(.footnote).foregroundStyle(Theme.muted)
                        Button("Cancel pending send") { model.cancelCompactContinuation() }
                    }
                }
                if target.archiveId.isEmpty {
                    Section {
                        Button("Compact") {
                            _ = captureDraft()
                            model.compactContext(target)
                        }
                        .disabled(!(available && compactReason.isEmpty))
                        Button("Compact and continue") {
                            let captured = captureDraft()
                            model.compactContext(target, continueDraft: true, expectedDraft: captured)
                            dismiss()
                        }
                        .disabled(!(available && compactReason.isEmpty && (!current.text.isEmpty || !current.attachments.isEmpty) && !model.sendingBlocked(target)))
                        Button("Clear context", role: .destructive) {
                            _ = captureDraft()
                            clear = true
                        }
                        .disabled(!(available && clearReason.isEmpty))
                    } footer: {
                        VStack(alignment: .leading, spacing: 4) {
                            if !compactReason.isEmpty { Text("Compact: \(compactReason)") }
                            if !clearReason.isEmpty { Text("Clear: \(clearReason)") }
                            if model.contextBlocked(target) || model.sendingBlocked(target) {
                                Text("Resolve the pending message or context operation before another context change.")
                            }
                            if available && compactReason.isEmpty { Text("Compaction uses tokens and may omit details. A submitted command is not yet a completed compaction.") }
                        }
                    }
                }
            }
            .textSelection(.enabled)
            .navigationTitle("Context and cache")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .zerusScreen()
        }
        .presentationDetents([.medium, .large])
        .clearContextConfirmation(isPresented: $clear, session: session)
        .alert("Review context command", isPresented: Binding(get: { review != nil }, set: { if !$0 { review = nil } }), presenting: review) { operation in
            Button("Mark reviewed") { model.reviewContextOperation(operation) }
                .disabled(app.demo || !["failed", "uncertain"].contains(model.contextOperations.first { $0.requestId == operation.requestId }?.status ?? ""))
            Button("Cancel", role: .cancel) {}
        } message: { operation in
            Text("\(ActionText.context(operation.operation, operation.status))\n\nThe command may already have changed \(operation.target.session). Check its original receipt and the native conversation before allowing another context command. Reviewing does not repeat the command or send a message.")
        }
        .alert("Restore earlier draft?", isPresented: Binding(get: { restore != nil }, set: { if !$0 { restore = nil } }), presenting: restore) { draft in
            Button("Restore draft") {
                _ = captureDraft()
                model.restoreContextDraft(draft, target)
            }
            .disabled(app.demo || !model.canRestoreContextDraft(draft, target))
            Button("Cancel", role: .cancel) {}
        } message: { draft in
            Text("Restore this saved draft into the current conversation in \(target.session)? Your current composition is kept in Saved drafts. This prepares the editor without sending.\n\n\(draft.text)")
        }
    }
}

extension View {
    /// The native clear command keeps the unsent draft; a new conversation is accepted only from its receipt.
    func clearContextConfirmation(isPresented: Binding<Bool>, session: Session, beforeClear: @escaping () -> Void = {}) -> some View {
        modifier(ClearContextConfirmation(isPresented: isPresented, session: session, beforeClear: beforeClear))
    }
}

struct ClearContextConfirmation: ViewModifier {
    @Environment(AppModel.self) private var app
    @Binding var isPresented: Bool
    let session: Session
    let beforeClear: () -> Void

    func body(content: Content) -> some View {
        let model = app.conversation!
        let target = session.target
        let view = ContextPresentation.from(model.activity, recorded: !model.activityVerified || !target.archiveId.isEmpty)
        content.alert("Clear context?", isPresented: $isPresented) {
            Button("Clear context", role: .destructive) {
                beforeClear()
                model.clearContext(target)
            }
            .disabled(!(!app.demo && app.storageReady && model.selected?.target == target && model.activityVerified && !model.contextBlocked(target) &&
                        !model.sendingBlocked(target) && model.contextActionReason(target, "clear_context").isEmpty && view.clearReason.isEmpty))
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Clear the conversation context in \(target.session) on \(app.machineName(target.machineKey))?\n\nThis runs the agent's native clear command. Future messages start without the current conversation context. Your unsent draft is kept.")
        }
    }
}

/// One message queued behind a busy agent; Send now asks the native agent to take it immediately.
struct NativeInputQueueCard: View {
    @Environment(AppModel.self) private var app
    let target: Target
    @State private var expanded = false

    var body: some View {
        let model = app.conversation!
        if let queue = model.activity?.object("input_queue"), !queue.string("id").isEmpty, target.agentId.isEmpty, target.archiveId.isEmpty {
            let id = queue.string("id")
            let reason = model.actionReason(target, "send_now").nonEmpty ??
                SessionActionPolicies.inspectReason(target, "send_now", ["queue_id": .string(id)], model.activity ?? [:])
            VStack(alignment: .leading, spacing: 8) {
                Button { expanded.toggle() } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "tray.full").foregroundStyle(Theme.muted)
                        VStack(alignment: .leading) {
                            Text("Waiting for agent").font(.subheadline.weight(.semibold))
                            Text("1 queued message").font(.caption).foregroundStyle(Theme.muted)
                        }
                        Spacer()
                        Image(systemName: expanded ? "chevron.up" : "chevron.down").foregroundStyle(Theme.muted)
                    }
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Waiting for agent, 1 queued message, \(expanded ? "collapse" : "expand") queued message")
                if expanded {
                    ScrollView { Text(queue.string("text")).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }.frame(maxHeight: 180)
                    if !queue.string("hint").isEmpty { Text(queue.string("hint")).font(.footnote).foregroundStyle(Theme.muted) }
                    if !reason.isEmpty { Text(reason).font(.footnote).foregroundStyle(Theme.muted) }
                    Button("Send now") { model.sendNow(target, queueId: id) }.disabled(!reason.isEmpty)
                } else {
                    Text(queue.string("text")).lineLimit(2).font(.callout)
                }
            }
            .padding(12)
            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 14))
        }
    }
}

extension Color {
    /// An Android ARGB color value.
    init(argb: UInt32) { self.init(hex: argb & 0xFFFFFF, opacity: Double((argb >> 24) & 0xFF) / 255) }
}
