import SwiftUI
import ZerusKit

/// Private unsent compositions and deliveries to review, kept per exact conversation.
struct DraftsScreen: View {
    @Environment(AppModel.self) private var app
    @Binding var settings: Bool
    let onOpen: (Target) -> Void
    @State private var review: Draft?
    @State private var reviewOutgoing: OutgoingMessage?
    @State private var discard: Draft?
    @State private var reviewContext: ContextOperation?
    @State private var reviewAction: SessionAction?

    private var model: ConversationModel { app.conversation }

    var body: some View {
        let unsent = model.drafts
            .filter { (!$0.text.isEmpty || !$0.attachments.isEmpty || !$0.answers.isEmpty) && !["submitted", "recorded"].contains($0.status) && $0.questionId != "__interrupt" }
            .sorted { $0.updatedAt > $1.updatedAt }
        let unresolved = model.normalizedOutgoing.filter { ["sending", "uncertain", "failed"].contains($0.status) }.sorted { $0.createdAt > $1.createdAt }
        let contexts = model.contextOperations.filter { ["sending", "submitted", "uncertain", "failed"].contains($0.status) }.sorted { $0.createdAt > $1.createdAt }
        let actions = model.sessionActions.filter { ["sending", "uncertain", "failed"].contains($0.status) || $0.needsProjectReview }.sorted { $0.createdAt > $1.createdAt }
        NavigationStack {
            List {
                Section {
                    Text("Private to this phone. Kept for the exact machine, session, run, and conversation.")
                        .font(.footnote).foregroundStyle(Theme.muted)
                        .listRowBackground(Color.clear)
                }
                if !model.actionResultNotice.isEmpty {
                    Text(model.actionResultNotice).font(.footnote).foregroundStyle(Theme.amber)
                }
                ForEach(unsent, id: \.key) { draft in draftCard(draft) }
                if !unresolved.isEmpty {
                    Section("Outgoing messages") {
                        ForEach(unresolved, id: \.requestId) { message in outgoingCard(message) }
                    }
                }
                if !contexts.isEmpty {
                    Section("Context commands") {
                        ForEach(contexts, id: \.requestId) { operation in contextCard(operation) }
                    }
                }
                if !actions.isEmpty {
                    Section("Native actions") {
                        ForEach(actions, id: \.requestId) { action in actionCard(action) }
                    }
                }
                if app.voice.draft != nil || app.voice.recording || app.voice.savingRecording {
                    Section("Voice recording") { VoiceComposerStatus() }
                }
                if !app.voice.outgoing.isEmpty {
                    Section("Voice and video messages") { VoiceSavedMessages() }
                }
                if unsent.isEmpty && unresolved.isEmpty && contexts.isEmpty && actions.isEmpty && app.voice.outgoing.isEmpty && app.voice.draft == nil {
                    ContentUnavailableView("No saved drafts", systemImage: "square.and.pencil",
                                           description: Text("Your unsent messages will appear here."))
                        .listRowBackground(Color.clear)
                }
            }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Saved drafts")
            .zerusScreen()
            .reviewDialogs(review: $review, reviewOutgoing: $reviewOutgoing, discard: $discard)
            .alert("Review context command", isPresented: Binding(get: { reviewContext != nil }, set: { if !$0 { reviewContext = nil } }), presenting: reviewContext) { operation in
                Button("Mark reviewed") { model.reviewContextOperation(operation) }
                Button("Cancel", role: .cancel) {}
            } message: { operation in
                Text("\(ActionText.context(operation.operation, operation.status))\n\nThe command may already have changed \(operation.target.session). Check its original receipt and the native conversation before allowing another context command. Reviewing does not repeat the command or send a message.")
            }
            .alert("Review native action?", isPresented: Binding(get: { reviewAction != nil }, set: { if !$0 { reviewAction = nil } }), presenting: reviewAction) { action in
                Button("Mark reviewed") { model.reviewSessionAction(action) }
                Button("Cancel", role: .cancel) {}
            } message: { action in
                Text(action.needsProjectReview
                     ? "\(action.resultTarget?.session ?? "")\nThe session was created. Review its project placement before dismissing this notice. Marking reviewed never creates another session."
                     : "\(ActionText.title(action))\nThis action may already have changed the machine. Check its original receipt and native state first. Marking it reviewed does not undo or repeat it.")
            }
        }
    }

    private func session(for target: Target) -> Session? {
        app.sessions.first { $0.target == target } ?? app.sessions.first { ContextPolicies.sameSessionRun($0.target, target) }
    }

    private func draftCard(_ draft: Draft) -> some View {
        let flight = draft.status == "submitting" || model.receiptFlights.contains(draft.key)
        let label = draft.questionId.isEmpty ? (draft.status == "editing" ? "Draft" : DeliveryLabel.text(draft.status))
            : "Answer " + (draft.status == "editing" ? "draft" : DeliveryLabel.text(draft.status).lowercased())
        return VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(draft.target.session).font(.subheadline.weight(.semibold)).lineLimit(1)
                Spacer()
                Tag(text: label, color: draft.status == "editing" ? Theme.mint : Theme.amber)
            }
            if !draft.text.isEmpty || !draft.answers.isEmpty {
                Text(verbatim: draft.text.isEmpty ? "Saved question answer" : draft.text).lineLimit(8).textSelection(.enabled)
            }
            ForEach(draft.attachments, id: \.id) { file in
                AttachmentRow(target: draft.target, file: file.displayed())
            }
            Text("\(app.machineName(draft.target.machineKey)) · Run \(String(draft.target.run.prefix(12)))").font(.caption2).foregroundStyle(Theme.muted)
            HStack(spacing: 16) {
                if let session = session(for: draft.target) {
                    Button(session.target == draft.target ? "Open conversation" : "Open current conversation") { onOpen(session.target) }
                }
                if !draft.detachedId.isEmpty { Button("Restore") { model.restoreDetached(draft) } }
                if let action = model.confirmedDraftAction(draft) {
                    Button("Restore to confirmed session") { model.restoreDraftToConfirmedResult(draft, action) }
                }
                if draft.status != "editing" { Button("Review") { review = draft }.disabled(flight) }
                Button("Discard", role: .destructive) { discard = draft }.disabled(flight)
            }
            .buttonStyle(.borderless)
            .font(.footnote.weight(.semibold))
        }
        .padding(.vertical, 4)
    }

    private func contextCard(_ operation: ContextOperation) -> some View {
        let flight = model.receiptFlights.contains("context:\(operation.requestId)")
        return VStack(alignment: .leading, spacing: 8) {
            Text(operation.target.session).font(.subheadline.weight(.semibold)).lineLimit(1)
            Text(ActionText.context(operation.operation, operation.status)).font(.caption).foregroundStyle(Theme.amber)
            if !operation.error.isEmpty { Text(operation.error).font(.footnote).foregroundStyle(Theme.amber) }
            Text("The original command identity is saved. It is never repeated automatically.").font(.caption).foregroundStyle(Theme.muted)
            HStack(spacing: 16) {
                if let session = app.sessions.first(where: { ContextPolicies.sameSessionRun($0.target, operation.target) }) {
                    Button("Open") { onOpen(session.target) }
                }
                if ["submitted", "uncertain"].contains(operation.status) {
                    Button("Check result") { model.checkContextOperation(operation) }
                        .disabled(flight || !app.connections.contains { $0.id == operation.target.connectionId })
                }
                if ["failed", "uncertain"].contains(operation.status) { Button("Review") { reviewContext = operation }.disabled(flight) }
            }
            .buttonStyle(.borderless)
            .font(.footnote.weight(.semibold))
        }
        .padding(.vertical, 4)
    }

    private func actionCard(_ action: SessionAction) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(ActionText.title(action)).font(.subheadline.weight(.semibold)).lineLimit(1)
            Text(action.needsProjectReview ? "Session created / Project assignment needs review" : "\(ActionText.label(action.operation)) / \(ActionText.status(action))")
                .font(.caption).foregroundStyle(Theme.amber)
            if !action.error.isEmpty { Text(action.error).font(.footnote).foregroundStyle(Theme.muted) }
            Text("The original request is saved. It is never repeated automatically.").font(.caption).foregroundStyle(Theme.muted)
            HStack(spacing: 16) {
                if action.needsProjectReview {
                    Button("Open created session") {
                        if let result = action.resultTarget { onOpen(result) }
                    }
                    .disabled(!app.connections.contains { $0.id == action.target.connectionId })
                    Button("Mark reviewed") { reviewAction = action }
                }
                if let session = app.sessions.first(where: { $0.target == action.target }) { Button("Open") { onOpen(session.target) } }
                if action.status == "uncertain" {
                    Button("Check original receipt") { model.checkSessionAction(action) }
                        .disabled(!app.connections.contains { $0.id == action.target.connectionId })
                    Button("Review") { reviewAction = action }
                }
            }
            .buttonStyle(.borderless)
            .font(.footnote.weight(.semibold))
        }
        .padding(.vertical, 4)
    }

    private func outgoingCard(_ message: OutgoingMessage) -> some View {
        let flight = model.receiptFlights.contains("outgoing:\(message.requestId)")
        return VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(message.target.session).font(.subheadline.weight(.semibold)).lineLimit(1)
                Spacer()
                Tag(text: DeliveryLabel.text(message.status), color: message.blocksSending || message.status == "failed" ? Theme.amber : Theme.mint)
            }
            if !message.text.isEmpty { Text(verbatim: message.text).lineLimit(8).textSelection(.enabled) }
            if !message.error.isEmpty { Text(message.error).font(.footnote).foregroundStyle(Theme.amber) }
            HStack(spacing: 16) {
                if let session = app.sessions.first(where: { message.belongs(to: $0.target) }) {
                    Button("Open") { onOpen(session.target) }
                }
                if message.status == "uncertain" {
                    Button("Check delivery") { Task { await model.checkOutgoing(message) } }.disabled(flight)
                }
                if ["failed", "uncertain"].contains(message.status) { Button("Review") { reviewOutgoing = message }.disabled(flight) }
            }
            .buttonStyle(.borderless)
            .font(.footnote.weight(.semibold))
        }
        .padding(.vertical, 4)
    }
}
