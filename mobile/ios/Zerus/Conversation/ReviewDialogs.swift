import SwiftUI
import ZerusKit

/// Review choices for unconfirmed drafts and messages, discard confirmation and cold-cache answer consent.
/// Nothing here sends again: preparing restores a draft, and checking reads the original receipt.
struct ReviewDialogs: ViewModifier {
    @Environment(AppModel.self) private var app
    @Binding var review: Draft?
    @Binding var reviewOutgoing: OutgoingMessage?
    @Binding var discard: Draft?

    func body(content: Content) -> some View {
        let model = app.conversation!
        content
            .alert("Review unconfirmed delivery", isPresented: present($review), presenting: review) { draft in
                Button("Prepare again") { model.review(draft) }
                Button("Check receipt") { Task { await model.checkReceipt(draft) } }
                Button("Cancel", role: .cancel) {}
            } message: { draft in
                Text("Your message may already have reached \(draft.target.session). Check its conversation and receipt before preparing another send.\n\n\(draft.text.isEmpty ? draft.answers : draft.text)\n\nPreparing again keeps the draft. Sending remains a separate action.")
            }
            .alert("Review delivery", isPresented: present($reviewOutgoing), presenting: reviewOutgoing) { message in
                let flight = model.receiptFlights.contains("outgoing:\(message.requestId)")
                Button("Prepare again") { model.prepareOutgoing(message) }.disabled(flight)
                Button("Check receipt") { Task { await model.checkOutgoing(message) } }.disabled(flight)
                Button("Cancel", role: .cancel) {}
            } message: { message in
                Text((message.status == "uncertain"
                      ? "This message may already have reached \(message.target.session). Check its receipt and conversation before preparing another send."
                      : "This message was not accepted. Review it before trying again.") +
                     "\n\n\(message.text)\n\nYour current composition is kept in Saved drafts. Preparing again does not send the message.")
            }
            .alert("Discard saved draft?", isPresented: present($discard), presenting: discard) { draft in
                Button("Discard", role: .destructive) { model.discard(draft) }
                Button("Cancel", role: .cancel) {}
            } message: { draft in
                Text("This removes the private draft for \(draft.target.session) from this phone.")
            }
            .alert("Submit with full context?", isPresented: Binding(get: { model.pendingQuestionSend != nil },
                                                                     set: { if !$0, let intent = model.pendingQuestionSend { model.cancelQuestionSend(intent) } }),
                   presenting: model.pendingQuestionSend) { intent in
                Button("Submit full context") { model.confirmQuestionSend(intent) }.disabled(!model.questionSendValid(intent))
                Button("Cancel", role: .cancel) { model.cancelQuestionSend(intent) }
            } message: { intent in
                Text("The prompt cache for \(intent.target.session) is cold or expired. Submitting this answer may process the full conversation again, increasing token use or cost. Clearing or compacting context would invalidate this pending answer. Cancel to manage context separately; your answer stays saved.")
            }
    }

    private func present<T>(_ binding: Binding<T?>) -> Binding<Bool> {
        Binding(get: { binding.wrappedValue != nil }, set: { if !$0 { binding.wrappedValue = nil } })
    }
}

extension View {
    func reviewDialogs(review: Binding<Draft?>, reviewOutgoing: Binding<OutgoingMessage?>, discard: Binding<Draft?>) -> some View {
        modifier(ReviewDialogs(review: review, reviewOutgoing: reviewOutgoing, discard: discard))
    }
}
