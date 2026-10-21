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

/// A message the person did not write, framed like desktop's notice cards (ActivityNotices).
struct NoticeCard: View {
    let event: Event
    @State private var expanded = false

    private var label: String { ActivityNotices.label(origin: event.origin ?? "", sender: event.sender ?? "") }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label).font(.caption.weight(.semibold)).foregroundStyle(Theme.notice)
            MarkdownContent(source: event.text)
            if let report = event.report, !report.isEmpty {
                Button(expanded ? "Hide report" : "Show report") { expanded.toggle() }
                    .font(.caption)
                if expanded { MarkdownContent(source: report) }
            }
            if event.at > 0 {
                HStack {
                    Spacer(minLength: 0)
                    Text(MessageTime.text(event.at))
                }
                .font(.caption2)
                .foregroundStyle(Theme.muted)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 17))
        .overlay(RoundedRectangle(cornerRadius: 17).stroke(Theme.notice.opacity(0.45), lineWidth: 1))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(label)
    }
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
        if event.role == ActivityNotices.role {
            VStack(alignment: .leading, spacing: 4) {
                NoticeCard(event: event)
                if !event.attachments.isEmpty { attachments }
                if event.detailTruncated { Text("Message text shortened").font(.caption2).foregroundStyle(Theme.amber) }
            }
        } else {
            bubble
        }
    }

    @ViewBuilder private var bubble: some View {
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
                if !event.attachments.isEmpty && event.voice == nil { attachments }
                if event.detailTruncated {
                    Text("Message text shortened").font(.caption2).foregroundStyle(Theme.amber)
                }
                footer
                if let outgoing, outgoing.recoverable { recovery(outgoing) }
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
            VoiceMessageContent(target: target, voice: voice)
                .frame(minWidth: 240)
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

    /// A message the agent has not admitted shows the native "Queued" or "Not delivered"; phones offer no removal.
    @ViewBuilder
    private var footer: some View {
        let queued = QueuedDelivery.label(event.deliveryState)
        if event.at > 0 || !event.delivery.isEmpty || !queued.isEmpty {
            HStack(spacing: 8) {
                Spacer(minLength: 0)
                if event.at > 0 { Text(MessageTime.text(event.at)) }
                if !queued.isEmpty {
                    Text(queued).foregroundStyle(event.deliveryState == "not_delivered" ? Theme.amber : Theme.muted)
                } else if !event.delivery.isEmpty {
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
