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

    private var model: ConversationModel { app.conversation }

    var body: some View {
        let unsent = model.drafts
            .filter { (!$0.text.isEmpty || !$0.attachments.isEmpty || !$0.answers.isEmpty) && !["submitted", "recorded"].contains($0.status) && $0.questionId != "__interrupt" }
            .sorted { $0.updatedAt > $1.updatedAt }
        let unresolved = model.normalizedOutgoing.filter { ["sending", "uncertain", "failed"].contains($0.status) }.sorted { $0.createdAt > $1.createdAt }
        NavigationStack {
            List {
                Section {
                    Text("Private to this phone. Kept for the exact machine, session, run, and conversation.")
                        .font(.footnote).foregroundStyle(Theme.muted)
                        .listRowBackground(Color.clear)
                }
                ForEach(unsent, id: \.key) { draft in draftCard(draft) }
                if !unresolved.isEmpty {
                    Section("Outgoing messages") {
                        ForEach(unresolved, id: \.requestId) { message in outgoingCard(message) }
                    }
                }
                if unsent.isEmpty && unresolved.isEmpty {
                    ContentUnavailableView("No saved drafts", systemImage: "square.and.pencil",
                                           description: Text("Your unsent messages will appear here."))
                        .listRowBackground(Color.clear)
                }
            }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Saved drafts")
            .zerusScreen()
            .reviewDialogs(review: $review, reviewOutgoing: $reviewOutgoing, discard: $discard)
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
                Label(file.name, systemImage: "paperclip").font(.footnote).foregroundStyle(Theme.muted)
            }
            Text("\(app.machineName(draft.target.machineKey)) · Run \(String(draft.target.run.prefix(12)))").font(.caption2).foregroundStyle(Theme.muted)
            HStack(spacing: 16) {
                if let session = session(for: draft.target) {
                    Button(session.target == draft.target ? "Open conversation" : "Open current conversation") { onOpen(session.target) }
                }
                if !draft.detachedId.isEmpty { Button("Restore") { model.restoreDetached(draft) } }
                if draft.status != "editing" { Button("Review") { review = draft }.disabled(flight) }
                Button("Discard", role: .destructive) { discard = draft }.disabled(flight)
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
