import SwiftUI

/// Desktop geometry from tray/src/WorkspaceIcons.h and IdentityBadge.h, as ported to
/// Android `DesktopIcons`. These are Zerus glyphs, including provider motifs, not vendor logos.
struct DesktopGlyph: Sendable {
    let viewport: CGFloat
    let lineWidth: CGFloat
    let stroke: Path
    let fill: Path

    init(viewport: CGFloat = 24, lineWidth: CGFloat = 1.6, stroke: Path, fill: Path = Path()) {
        self.viewport = viewport
        self.lineWidth = lineWidth
        self.stroke = stroke
        self.fill = fill
    }

    static func build(viewport: CGFloat = 24, lineWidth: CGFloat = 1.6, _ draw: (inout Path) -> Void, fill: ((inout Path) -> Void)? = nil) -> DesktopGlyph {
        var stroke = Path()
        draw(&stroke)
        var filled = Path()
        fill?(&filled)
        return DesktopGlyph(viewport: viewport, lineWidth: lineWidth, stroke: stroke, fill: filled)
    }
}

extension Path {
    mutating func line(_ points: CGPoint...) {
        guard let first = points.first else { return }
        move(to: first)
        for point in points.dropFirst() { addLine(to: point) }
    }

    mutating func rect(_ x: CGFloat, _ y: CGFloat, _ width: CGFloat, _ height: CGFloat) {
        addRect(CGRect(x: x, y: y, width: width, height: height))
    }

    mutating func rounded(_ x: CGFloat, _ y: CGFloat, _ width: CGFloat, _ height: CGFloat, _ r: CGFloat = 2) {
        addRoundedRect(in: CGRect(x: x, y: y, width: width, height: height), cornerSize: CGSize(width: r, height: r), style: .circular)
    }

    mutating func circle(_ x: CGFloat, _ y: CGFloat, _ r: CGFloat) {
        addEllipse(in: CGRect(x: x - r, y: y - r, width: 2 * r, height: 2 * r))
    }

    /// Half of an axis-aligned ellipse from the right point to the left point, bulging up.
    mutating func upperHalfEllipse(center: CGPoint, rx: CGFloat, ry: CGFloat) {
        let k: CGFloat = 0.5522847498
        move(to: CGPoint(x: center.x + rx, y: center.y))
        addCurve(to: CGPoint(x: center.x, y: center.y - ry), control1: CGPoint(x: center.x + rx, y: center.y - ry * k),
                 control2: CGPoint(x: center.x + rx * k, y: center.y - ry))
        addCurve(to: CGPoint(x: center.x - rx, y: center.y), control1: CGPoint(x: center.x - rx * k, y: center.y - ry),
                 control2: CGPoint(x: center.x - rx, y: center.y - ry * k))
    }
}

private func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x, y: y) }

extension DesktopGlyph {
    static let all = build({ path in
        path.rounded(4, 4, 16, 7)
        path.rounded(4, 14, 16, 6)
        path.line(p(12, 7.5), p(16, 7.5))
    }, fill: { $0.circle(8, 7.5, 0.8) })

    static let attention = build { path in
        path.move(to: p(6, 10))
        path.addCurve(to: p(18, 10), control1: p(6, 3), control2: p(18, 3))
        path.addLine(to: p(18, 15)); path.addLine(to: p(20, 18)); path.addLine(to: p(4, 18)); path.addLine(to: p(6, 15))
        path.closeSubpath()
        path.move(to: p(10, 20))
        path.addCurve(to: p(12, 22), control1: p(10, 21.1046), control2: p(10.8954, 22))
        path.addCurve(to: p(14, 20), control1: p(13.1046, 22), control2: p(14, 21.1046))
    }

    static let working = build { path in
        path.line(p(13, 3), p(5, 13), p(11, 13), p(10, 21), p(19, 10), p(13, 10))
        path.closeSubpath()
    }

    static let paused = build { path in
        path.line(p(8, 5), p(8, 19))
        path.line(p(16, 5), p(16, 19))
    }

    static let archived = build { path in
        path.rect(3, 4, 18, 4)
        path.rect(5, 8, 14, 12)
        path.line(p(10, 12), p(14, 12))
    }

    static let calendar = build { path in
        path.rounded(4, 5, 16, 16)
        path.line(p(4, 10), p(20, 10))
        path.line(p(8, 3), p(8, 7)); path.line(p(16, 3), p(16, 7))
        path.line(p(8, 14), p(8.01, 14)); path.line(p(12, 14), p(12.01, 14)); path.line(p(16, 14), p(16.01, 14))
    }

    static let machines = build { path in
        path.rounded(3, 4, 18, 12)
        path.line(p(12, 16), p(12, 20)); path.line(p(8, 20), p(16, 20))
    }

    static let projects = build { path in
        path.line(p(3, 8), p(3, 5), p(10, 5), p(12, 8), p(21, 8), p(21, 19), p(3, 19))
        path.closeSubpath()
    }

    static let terminal = build { path in
        path.rounded(3, 4, 18, 16)
        path.line(p(7, 9), p(10, 12), p(7, 15))
        path.line(p(13, 15), p(17, 15))
    }

    static let gitBranch = build { path in
        path.line(p(6, 7.5), p(6, 16.5))
        path.move(to: p(18, 7.5)); path.addLine(to: p(18, 10))
        path.addCurve(to: p(6, 16.5), control1: p(18, 15), control2: p(6, 11))
        path.circle(6, 5, 2.5); path.circle(6, 19, 2.5); path.circle(18, 5, 2.5)
    }

    static let subagents = build(viewport: 16, lineWidth: 1.2) { path in
        path.circle(5.5, 5.75, 2); path.circle(11.5, 6.75, 2)
        path.upperHalfEllipse(center: p(5.5, 11.75), rx: 4, ry: 3)
        path.upperHalfEllipse(center: p(11, 12.25), rx: 3.5, ry: 2.5)
    }

    static let question = build({ path in
        path.circle(12, 12, 9)
        path.move(to: p(10, 10))
        path.addCurve(to: p(15, 10), control1: p(10, 7), control2: p(15, 7))
        path.addCurve(to: p(12, 14), control1: p(15, 12), control2: p(12, 11))
    }, fill: { $0.circle(12, 17, 0.8) })

    static let folder = projects
    static let sessions = all

    static func named(_ name: String) -> DesktopGlyph {
        switch name.lowercased() {
        case "all", "sessions": all
        case "attention": attention
        case "working", "active": working
        case "paused", "pause", "saved": paused
        case "archive", "archived": archived
        case "machine", "machines": machines
        case "folder", "project", "projects", "group": projects
        case "git-branch": gitBranch
        case "subagents": subagents
        case "question": question
        default: terminal
        }
    }

    // MARK: Providers

    static func providerKey(_ agent: String) -> String {
        switch agent.trimmingCharacters(in: .whitespaces).lowercased() {
        case "claude", "claude code", "claude-code": "claude"
        case "codex": "codex"
        case "opencode", "open code", "open-code": "opencode"
        case "kimi", "kimi code", "kimi-code": "kimi"
        case "dsh", "deepseek", "deepseek harness", "deepseek-harness": "dsh"
        case "sh", "shell", "terminal", "bash", "zsh": "sh"
        case let other: other
        }
    }

    static func providerName(_ agent: String) -> String {
        switch providerKey(agent) {
        case "claude": "Claude"
        case "codex": "Codex"
        case "opencode": "OpenCode"
        case "kimi": "Kimi"
        case "dsh": "DeepSeek"
        case "sh": "Shell"
        default: agent.trimmingCharacters(in: .whitespaces).isEmpty ? "Terminal" : agent
        }
    }

    static let claudeBadge = build(viewport: 18, lineWidth: 1.4) { path in
        for index in 0..<8 {
            let radians = Double(index) * 45 * .pi / 180
            let x = CGFloat(-sin(radians)), y = CGFloat(cos(radians))
            path.line(p(9 + x * 2, 9 + y * 2), p(9 + x * 5, 9 + y * 5))
        }
    }
    static let kimiBadge = build(viewport: 18, lineWidth: 1.4) { path in
        path.line(p(6, 5), p(6, 13)); path.line(p(6, 9), p(12, 5)); path.line(p(6, 9), p(12, 13))
    }
    static let openCodeBadge = build(viewport: 18, lineWidth: 1.4) { path in
        path.line(p(7, 6), p(4, 9), p(7, 12)); path.line(p(11, 6), p(14, 9), p(11, 12))
    }
    static let terminalBadge = build(viewport: 18, lineWidth: 1.4) { path in
        path.line(p(5, 6), p(8, 9), p(5, 12)); path.line(p(10, 12), p(13, 12))
    }
    static let codexLogo = build { path in
        for index in 0..<6 {
            let radians = Double(index) * 60 * .pi / 180
            let c = CGFloat(cos(radians)), s = CGFloat(sin(radians))
            func r(_ x: CGFloat, _ y: CGFloat) -> CGPoint { p(12 + (x - 12) * c - (y - 12) * s, 12 + (x - 12) * s + (y - 12) * c) }
            path.move(to: r(12, 5))
            path.addCurve(to: r(16, 15), control1: r(20, 2), control2: r(23, 12))
            path.addLine(to: r(10, 12)); path.addLine(to: r(10, 7))
        }
    }
    static let claudeLogo = build { path in
        for index in 0..<8 {
            let radians = Double(index) * 45 * .pi / 180
            let x = CGFloat(-sin(radians)), y = CGFloat(cos(radians))
            path.line(p(12 + x * 3, 12 + y * 3), p(12 + x * 9, 12 + y * 9))
        }
    }
    static let kimiLogo = build { path in
        path.line(p(6, 4), p(6, 20)); path.line(p(6, 12), p(18, 4)); path.line(p(6, 12), p(18, 20))
    }
    static let deepSeekLogo = build { path in
        path.move(to: p(3, 10))
        path.addCurve(to: p(18, 10), control1: p(5, 5), control2: p(15, 4))
        path.addLine(to: p(23, 7)); path.addLine(to: p(22, 13)); path.addLine(to: p(18, 13))
        path.addCurve(to: p(3, 10), control1: p(15, 22), control2: p(3, 19))
        path.line(p(8, 10), p(8.01, 10))
    }

    static func providerLogo(_ agent: String) -> DesktopGlyph {
        switch providerKey(agent) {
        case "codex": codexLogo
        case "claude": claudeLogo
        case "kimi": kimiLogo
        case "dsh": deepSeekLogo
        default: providerIcon(agent)
        }
    }

    static func providerIcon(_ agent: String) -> DesktopGlyph {
        switch providerKey(agent) {
        case "codex": codexLogo
        case "dsh": deepSeekLogo
        case "claude": claudeBadge
        case "kimi": kimiBadge
        case "opencode": openCodeBadge
        default: terminalBadge
        }
    }

    static func providerColor(_ agent: String) -> Color {
        switch providerKey(agent) {
        case "claude": Color(hex: 0xE8B29B)
        case "kimi": Color(hex: 0xBAC1FF)
        case "dsh": Color(hex: 0x8CB9FF)
        default: Color(hex: 0xA6D8C5)
        }
    }

    static let providerBackground = Color(hex: 0x303942)
}

/// Renders a desktop glyph at any size with its round-capped stroke.
struct GlyphView: View {
    let glyph: DesktopGlyph
    var color: Color = Theme.muted

    var body: some View {
        Canvas { context, size in
            let scale = min(size.width, size.height) / glyph.viewport
            let offset = CGSize(width: (size.width - glyph.viewport * scale) / 2, height: (size.height - glyph.viewport * scale) / 2)
            let transform = CGAffineTransform(scaleX: scale, y: scale).concatenating(CGAffineTransform(translationX: offset.width, y: offset.height))
            context.stroke(glyph.stroke.applying(transform), with: .color(color),
                           style: StrokeStyle(lineWidth: glyph.lineWidth * scale, lineCap: .round, lineJoin: .round))
            if !glyph.fill.isEmpty { context.fill(glyph.fill.applying(transform), with: .color(color)) }
        }
        .accessibilityHidden(true)
    }
}
