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
            ContextSummary(raw: model.activity, recorded: !model.activityVerified)
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
            Button("Cancel", role: .cancel) { coldDraft = nil }
        } message: {
            Text("The next message may process the full conversation again, increasing token use or cost. Send it with full context, or cancel and keep your draft.")
        }
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

/// Native context and cache state; tapping opens every reported detail.
struct ContextSummary: View {
    let raw: JSONObject?
    let recorded: Bool
    @State private var details = false

    var body: some View {
        TimelineView(.periodic(from: .now, by: 30)) { timeline in
            let view = ContextPresentation.from(raw, recorded: recorded, now: timeline.date.timeIntervalSince1970)
            Button { details = true } label: {
                HStack(spacing: 10) {
                    Text(view.summary).foregroundStyle(Color(argb: ContextPresentation.color(view.tone)))
                    Text(view.cacheSummary).foregroundStyle(Color(argb: ContextPresentation.cacheColor(view.cacheTone)))
                    if !view.compactionSummary.isEmpty { Text(view.compactionSummary).foregroundStyle(Theme.amber) }
                    Spacer(minLength: 0)
                    Image(systemName: "info.circle").foregroundStyle(Theme.muted)
                }
                .font(.caption2)
                .lineLimit(1)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Context details: \(view.summary), \(view.cacheSummary)")
            .sheet(isPresented: $details) { ContextDetailsSheet(view: view) }
        }
    }
}

struct ContextDetailsSheet: View {
    @Environment(\.dismiss) private var dismiss
    let view: ContextView

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(view.details, id: \.self) { detail in
                        LabeledContent(detail.label, value: detail.value).textSelection(.enabled)
                    }
                }
                Section("Notes") {
                    ForEach(view.notes, id: \.self) { Text($0).font(.footnote).foregroundStyle(Theme.muted) }
                }
            }
            .navigationTitle("Context")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .zerusScreen()
        }
        .presentationDetents([.medium, .large])
    }
}

extension Color {
    /// An Android ARGB color value.
    init(argb: UInt32) { self.init(hex: argb & 0xFFFFFF, opacity: Double((argb >> 24) & 0xFF) / 255) }
}
