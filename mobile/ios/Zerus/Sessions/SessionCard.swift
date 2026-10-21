import SwiftUI
import UIKit
import ZerusKit

/// A desktop-style session card. Tapping opens the exact session; long-press offers copy actions.
struct SessionCard: View {
    @Environment(AppModel.self) private var model
    let session: Session

    var body: some View {
        let online = model.online(session)
        let drafted = SessionCardPresentation.draftedChildren(session.target, model.messageState.drafts)
        let children = SessionCardPresentation.children(session, online, drafted: drafted)
        let counter = SessionCardPresentation.counter(session, online, children)
        let expanded = counter?.expandable == true && model.expandedSubagents.contains(session.target.key)
        VStack(alignment: .leading, spacing: 9) {
            NavigationLink(value: SessionRoute(target: session.target)) {
                VStack(alignment: .leading, spacing: 9) {
                    HStack(spacing: 10) {
                        ProviderBadge(agent: session.agent, size: 22)
                        Text(SessionFilters.label(session))
                            .font(.system(size: 17, weight: .semibold))
                            .lineLimit(1)
                            .foregroundStyle(Theme.onSurface)
                        Spacer(minLength: 4)
                        if SessionNotificationPreferences.muted(session) {
                            Image(systemName:"bell.slash").foregroundStyle(Theme.muted).accessibilityLabel("Notifications muted on all devices")
                        }
                    }
                    HStack(spacing: 8) {
                        SessionStatusBadge(session: session, online: online)
                            .layoutPriority(1)
                        MachineLabel(name: model.machineName(session.target.machineKey, fallback: session.target.computerId),
                                     colorHex: model.machineColor(session.target.machineKey))
                        Spacer(minLength: 0)
                    }
                    CheckoutLine(raw: session.raw)
                    let archived = SessionFilters.archiveDate(session)
                    if !archived.isEmpty { Text("Archived \(archived)").font(.caption2).foregroundStyle(Theme.muted) }
                    Text(online && SessionFilters.waitingIdle(session) ? SessionFilters.waitingLabel(session)
                         : (session.preview.isEmpty ? "Open to view activity" : session.preview))
                        .font(.subheadline)
                        .foregroundStyle(Theme.muted)
                        .lineLimit(2)
                        .multilineTextAlignment(.leading)
                    if model.connections.count > 1 {
                        Text(model.connections.first { $0.id == session.target.connectionId }?.displayName ?? "")
                            .font(.caption2).foregroundStyle(Theme.muted)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if let counter {
                SubagentCounterButton(counter: counter, expanded: expanded) { model.toggleSubagents(session.target) }
            }
            if expanded {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(children.enumerated()), id: \.element.id) { index, child in
                        SubagentRow(child: child, last: index == children.count - 1,notificationsMuted:SessionNotificationPreferences.muted(session))
                    }
                }
            }
        }
        .padding(14)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 14))
        .contextMenu { SessionCopyMenu(session: session) }
    }
}

/// The desktop card's checkout line: folder / branch with a Git branch mark, or an ordinary folder's name.
struct CheckoutLine: View {
    let raw: JSONObject

    var body: some View {
        let line = SessionCardPresentation.line(raw)
        if !line.isEmpty {
            HStack(spacing: 5) {
                GlyphView(glyph: SessionCardPresentation.branch(raw).isEmpty ? .folder : .gitBranch).frame(width: 14, height: 14)
                Text(line).font(.footnote).foregroundStyle(Theme.muted).lineLimit(1).truncationMode(.middle)
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(SessionCardPresentation.checkoutDescription(raw))
        }
    }
}

private struct SubagentCounterButton: View {
    let counter: SessionChildCounter
    let expanded: Bool
    let toggle: () -> Void

    var body: some View {
        let content = HStack(spacing: 5) {
            GlyphView(glyph: .subagents).frame(width: 16, height: 16)
            Text(counter.label).font(.caption.weight(.medium)).foregroundStyle(Theme.muted)
            if counter.expandable {
                Image(systemName: expanded ? "chevron.up" : "chevron.down").font(.caption2).foregroundStyle(Theme.muted)
            }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        if counter.expandable {
            Button(action: toggle) { content.background(Color(hex: 0x28323C), in: RoundedRectangle(cornerRadius: 8)) }
                .buttonStyle(.plain)
                .accessibilityLabel(counter.description)
                .accessibilityValue(expanded ? "Expanded" : "Collapsed")
                .accessibilityHint(expanded ? "Hide subagents" : "Show subagents")
        } else {
            content.accessibilityElement(children: .ignore).accessibilityLabel(counter.description)
        }
    }
}

private struct SubagentRow: View {
    let child: SessionCardChild
    let last: Bool
    let notificationsMuted: Bool

    var body: some View {
        let colors = DesktopSessionPalette.colors(notificationsMuted && [.attention,.unread].contains(child.kind) ? .neutral : child.kind, dark: true)
        HStack(alignment: .center, spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 7) {
                    Text(child.status)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(Color(rgb: colors.foreground))
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(Color(rgb: colors.background), in: RoundedRectangle(cornerRadius: 5))
                    Text(child.name).font(.subheadline.weight(.medium)).lineLimit(1)
                }
                if !child.detail.isEmpty {
                    Text(child.detail).font(.footnote).foregroundStyle(Theme.muted).lineLimit(1)
                }
            }
            Spacer(minLength: 4)
            ProviderBadge(agent: child.provider, size: 16)
        }
        .padding(.leading, 20)
        .padding(.vertical, 6)
        .frame(minHeight: 44)
        .background(alignment: .leading) {
            // The desktop tree line joins each child to its parent card.
            GeometryReader { proxy in
                Path { path in
                    path.move(to: CGPoint(x: 7, y: 0))
                    path.addLine(to: CGPoint(x: 7, y: last ? proxy.size.height / 2 : proxy.size.height))
                    path.move(to: CGPoint(x: 7, y: proxy.size.height / 2))
                    path.addLine(to: CGPoint(x: 14, y: proxy.size.height / 2))
                }
                .stroke(Theme.outline, lineWidth: 1)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(child.description + (child.detail.isEmpty ? "" : ". \(child.detail)"))
    }
}

/// Copy actions that need no native verification.
/// The Sessions long-press menu, mirroring the desktop context menu. Native items open the exact
/// session, whose header confirms them against a fresh inspection.
struct SessionCopyMenu: View {
    @Environment(AppModel.self) private var model
    let session: Session

    var body: some View {
        let conversation = model.conversation!
        let target = session.target
        ForEach(Array(conversation.sessionMenu(session).enumerated()), id: \.offset) { _, section in
            Section {
                ForEach(section) { item in
                    Button {
                        run(item, conversation: conversation)
                    } label: {
                        Label {
                            Text(item.label)
                            if !(item.reason.isEmpty ? item.hint : item.reason).isEmpty { Text(item.reason.isEmpty ? item.hint : item.reason) }
                        } icon: {
                            Image(systemName: Self.icon(item.id))
                        }
                    }
                    .disabled(!item.enabled)
                }
            }
        }
        .onAppear { conversation.prepareReadEvidence(target) }
    }

    private func run(_ item: SessionMenuItem, conversation: ConversationModel) {
        let target = session.target
        switch item.id {
        case "mark_read":
            let result = conversation.markAllRead([target])
            model.notice = ConversationLoadPresentation.readAllOutcome(marked: result.marked, skipped: result.skipped, partial: result.partial)
        case "clear_reminder": conversation.clearReviewLater(target)
        case "review_later": conversation.reviewLater(target, conversation.captureReadAttention(target))
        case "copy_name": UIPasteboard.general.string = target.session
        case "copy_reference": if let uri = SessionReference.resolve(target, model.machines).uri { UIPasteboard.general.string = uri }
        case "copy_folder": if !session.raw.string("cwd").isEmpty { UIPasteboard.general.string = session.raw.string("cwd") }
        default:
            conversation.openWithSessionAction(session, item.id)
            model.conversationRequest = target
        }
    }

    static func icon(_ id: String) -> String {
        switch id {
        case SessionNotificationPreferences.operation: "bell.slash"
        case "terminal": "apple.terminal"
        case "mark_read": "checkmark.circle"
        case "clear_reminder": "bell.slash"
        case "review_later": "bookmark"
        case "pause": "pause.circle"
        case "resume": "play.circle"
        case "restore": "arrow.uturn.backward.circle"
        case "rename": "pencil"
        case "fork": "arrow.triangle.branch"
        case "archive": "archivebox"
        case "copy_name": "doc.on.doc"
        case "copy_reference": "link"
        case "copy_folder": "folder"
        case "terminate": "stop.circle"
        default: "trash"
        }
    }
}
