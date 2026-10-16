import SwiftUI
import ZerusKit

enum DeliveryLabel {
    static func text(_ status: String) -> String {
        switch status {
        case "sending", "submitting": "Sending…"
        case "uncertain": "Delivery unknown"
        case "failed": "Not sent"
        case "submitted", "recorded": "Sent"
        default: status.prefix(1).uppercased() + status.dropFirst()
        }
    }
}

enum MessageTime {
    static let formatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm"
        return formatter
    }()

    static func text(_ at: Double) -> String { formatter.string(from: Date(timeIntervalSince1970: at)) }
}

/// One conversation row: your messages on the right, agent replies as full-width Markdown.
struct MessageBubble: View {
    @Environment(AppModel.self) private var app
    let event: Event
    let target: Target?
    let agent: String
    let outgoing: OutgoingMessage?
    let onReviewOutgoing: (OutgoingMessage) -> Void

    private var own: Bool { QuestionReplyPresentation.isOwnRole(event.role) }

    var body: some View {
        let replies = own ? QuestionReplyPresentation.parse(event.text) : nil
        HStack {
            if own { Spacer(minLength: 28) }
            VStack(alignment: .leading, spacing: 6) {
                if !own {
                    Text(agent.isEmpty ? event.role : agent)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(Theme.muted)
                }
                content(replies)
                if !event.attachments.isEmpty { attachments }
                if event.detailTruncated {
                    Text("Message text shortened").font(.caption2).foregroundStyle(Theme.amber)
                }
                footer
                if let outgoing, ["failed", "uncertain"].contains(outgoing.status) { recovery(outgoing) }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .frame(maxWidth: own ? nil : .infinity, alignment: .leading)
            .background(own ? Color(hex: 0x203733) : Theme.surface, in: RoundedRectangle(cornerRadius: 17))
            .accessibilityElement(children: .contain)
            .accessibilityLabel(own ? "You" : (agent.isEmpty ? "Agent" : agent))
        }
    }

    @ViewBuilder
    private func content(_ replies: [PresentedQuestionReply]?) -> some View {
        if let voice = event.voice {
            VStack(alignment: .leading, spacing: 4) {
                Label(voice.video != nil ? "Video message" : "Voice message", systemImage: voice.video != nil ? "video" : "waveform")
                    .font(.subheadline.weight(.semibold))
                if !voice.transcript.isEmpty { Text(voice.transcript).textSelection(.enabled) }
                Text(VoiceProtocol.label(voice.state)).font(.caption).foregroundStyle(Theme.muted)
            }
        } else if let replies {
            VStack(alignment: .leading, spacing: 8) {
                Text("You (answer)").font(.caption.weight(.semibold)).foregroundStyle(Theme.muted)
                ForEach(Array(replies.enumerated()), id: \.offset) { _, reply in
                    Text("Question").font(.caption2).foregroundStyle(Theme.muted)
                    MarkdownContent(source: reply.question)
                    Text("Your answer").font(.caption2).foregroundStyle(Theme.mint)
                    Text(verbatim: reply.answer.isEmpty ? "(empty answer)" : reply.answer).textSelection(.enabled)
                }
            }
        } else if !event.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            if event.role == "You (answer)" {
                Text("You (answer)").font(.caption.weight(.semibold)).foregroundStyle(Theme.muted)
            }
            if own {
                Text(verbatim: event.text).textSelection(.enabled)
            } else {
                MarkdownContent(source: event.text)
            }
        }
    }

    private var attachments: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(Array(event.attachments.enumerated()), id: \.offset) { _, file in
                AttachmentRow(target: target, file: file)
            }
        }
    }

    @ViewBuilder
    private var footer: some View {
        if event.at > 0 || !event.delivery.isEmpty {
            HStack(spacing: 8) {
                Spacer(minLength: 0)
                if event.at > 0 { Text(MessageTime.text(event.at)) }
                if !event.delivery.isEmpty {
                    Text(DeliveryLabel.text(event.delivery))
                        .foregroundStyle(["failed", "uncertain"].contains(event.delivery) ? Theme.amber : Theme.muted)
                }
            }
            .font(.caption2)
            .foregroundStyle(Theme.muted)
        }
    }

    private func recovery(_ message: OutgoingMessage) -> some View {
        let flight = app.conversation.receiptFlights.contains("outgoing:\(message.requestId)")
        return VStack(alignment: .leading, spacing: 4) {
            if !message.error.isEmpty { Text(message.error).font(.footnote).foregroundStyle(Theme.amber) }
            HStack {
                if message.status == "uncertain" {
                    Button("Check delivery") { Task { await app.conversation.checkOutgoing(message) } }.disabled(flight)
                }
                Button("Review") { onReviewOutgoing(message) }.disabled(flight)
            }
            .buttonStyle(.borderless)
            .font(.footnote.weight(.semibold))
        }
    }
}
