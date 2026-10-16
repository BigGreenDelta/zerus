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
                        SubagentRow(child: child, last: index == children.count - 1)
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

    var body: some View {
        let colors = DesktopSessionPalette.colors(child.kind, dark: true)
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
struct SessionCopyMenu: View {
    @Environment(AppModel.self) private var model
    let session: Session

    var body: some View {
        let reference = SessionReference.resolve(session.target, model.machines)
        Button { UIPasteboard.general.string = session.target.session } label: { Label("Copy session name", systemImage: "doc.on.doc") }
        if let uri = reference.uri {
            Button { UIPasteboard.general.string = uri } label: { Label("Copy session reference", systemImage: "link") }
        } else {
            Button {} label: { Label("Copy session reference", systemImage: "link") }.disabled(true)
            Text(reference.reason)
        }
        let folder = session.raw.string("cwd")
        if !folder.isEmpty {
            Button { UIPasteboard.general.string = folder } label: { Label("Copy folder path", systemImage: "folder") }
        }
    }
}
