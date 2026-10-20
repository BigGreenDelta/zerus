import SwiftUI
import ZerusKit

extension Color {
    init(rgb: UInt32) { self.init(hex: rgb) }

    /// A `#rrggbb` project or machine color, or nil.
    init?(projectHex: String) {
        guard projectHex.count == 7, projectHex.hasPrefix("#"), let value = UInt32(projectHex.dropFirst(), radix: 16) else { return nil }
        self.init(hex: value)
    }
}

/// The same display label in session cards and machine selection. Identity stays in the caller.
struct MachineLabel: View {
    let name: String
    var colorHex: String?

    var body: some View {
        let checked = colorHex.flatMap { try? MachineColors.checked($0) }
        let foreground = checked.flatMap { try? MachineColors.foreground($0, dark: true) }.map(Color.init(rgb:)) ?? Theme.muted
        let background = checked.flatMap { try? MachineColors.background($0, dark: true) }.map(Color.init(rgb:)) ?? Theme.surfaceVariant
        HStack(spacing: 5) {
            GlyphView(glyph: .machines, color: foreground).frame(width: 14, height: 14)
            Text(name).lineLimit(1).truncationMode(.tail)
        }
        .font(.caption.weight(.medium))
        .foregroundStyle(foreground)
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(background, in: RoundedRectangle(cornerRadius: 7))
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Computer \(name)")
    }
}

/// The desktop provider identity motif, tinted with the provider color.
struct ProviderBadge: View {
    let agent: String
    var size: CGFloat = 18

    var body: some View {
        GlyphView(glyph: .providerIcon(agent), color: DesktopGlyph.providerColor(agent))
            .frame(width: size, height: size)
            .accessibilityLabel(DesktopGlyph.providerName(agent))
            .accessibilityHidden(false)
    }
}

/// Provider pill geometry and colours from the desktop IdentityBadges renderer.
struct ProviderLabel: View {
    let agent: String

    var body: some View {
        let foreground = DesktopGlyph.providerColor(agent)
        HStack(spacing: 4) {
            GlyphView(glyph: .providerIcon(agent), color: foreground).frame(width: 14, height: 14)
            Text(DesktopGlyph.providerName(agent)).lineLimit(1)
        }
        .font(.caption2.weight(.medium))
        .foregroundStyle(foreground)
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(DesktopGlyph.providerBackground, in: RoundedRectangle(cornerRadius: 4))
    }
}

/// The desktop status badge with its semantic color, plus a separate new-reply mark.
struct SessionStatusBadge: View {
    let session: Session
    let online: Bool

    var body: some View {
        let badge = DesktopSessionPalette.badge(session, online)
        let colors = DesktopSessionPalette.colors(badge.kind, dark: true)
        HStack(spacing: 6) {
            HStack(spacing: 5) {
                switch badge.kind {
                case .working, .background:
                    Circle().fill(Color(rgb: colors.foreground)).frame(width: 5, height: 5)
                case .attention, .error:
                    Text("!").font(.caption2.weight(.bold))
                case .paused:
                    GlyphView(glyph: .paused, color: Color(rgb: colors.foreground)).frame(width: 12, height: 12)
                case .unread:
                    Image(systemName: "envelope.badge").font(.system(size: 10))
                case .scheduled, .waiting:
                    Image(systemName: "clock").font(.system(size: 10))
                case .neutral:
                    EmptyView()
                }
                Text(badge.caption).lineLimit(1)
            }
            .font(.caption2.weight(.semibold))
            .foregroundStyle(Color(rgb: colors.foreground))
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(Color(rgb: colors.background), in: RoundedRectangle(cornerRadius: 5))
            if badge.additionalUnread {
                let unread = DesktopSessionPalette.colors(SessionNotificationPreferences.muted(session) ? .neutral : .unread, dark: true)
                Image(systemName: "envelope.badge")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(Color(rgb: unread.foreground))
                    .padding(4)
                    .background(Color(rgb: unread.background), in: RoundedRectangle(cornerRadius: 5))
                    .accessibilityLabel("New reply")
            }
            OptionalQuestionChipView(session: session, online: online)
        }
        .accessibilityElement(children: .combine)
    }
}

/// Pending optional questions after the status badge, in the attention amber. Information only.
struct OptionalQuestionChipView: View {
    let session: Session
    let online: Bool

    var body: some View {
        if let chip = SessionCardPresentation.optionalQuestionChip(session, online) {
            let colors = DesktopSessionPalette.colors(SessionNotificationPreferences.muted(session) ? .neutral : .attention, dark: true)
            HStack(spacing: 3) {
                GlyphView(glyph: .question, color: Color(rgb: colors.foreground)).frame(width: 15, height: 15)
                Text(chip.label).font(.caption2.weight(.semibold)).lineLimit(1)
            }
            .foregroundStyle(Color(rgb: colors.foreground))
            .padding(.leading, 4)
            .padding(.trailing, 7)
            .padding(.vertical, 2)
            .background(Color(rgb: colors.background), in: RoundedRectangle(cornerRadius: 5))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(chip.description)
        }
    }
}
