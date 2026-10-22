/*
 * Copyright 2025 mox
 * Licensed under the Apache License, Version 2.0 (see Zerus/Licenses/mux-pod.txt).
 * Adapted from MuxPod lib/screens/terminal/widgets/ansi_text_view.dart, commit
 * c33d0fdeb66ca7bbdf4745203578088961982c13.
 * Modified for Zerus: SwiftUI snapshot renderer ported from the Compose adaptation, frozen
 * selection, cursor following, confirmed OSC 8 links, explicit buffered input, adaptive outbound
 * relay reads, fixed keys, no SSH credentials or input replay.
 */
import SwiftUI
import UIKit
import ZerusKit

private let terminalBackground: UInt32 = 0xFF10_1517
private let terminalForeground: UInt32 = 0xFFD4_D4D4

/// A fullscreen view of the existing native pane. Reads run only while it is active and focused.
struct TerminalView: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openURL) private var openURL
    let session: Session

    @State private var frozen: JSONObject?
    @State private var selecting = false
    @State private var fontSize: CGFloat = 13
    @State private var dangerousKey: DangerousKey?
    @State private var reviewInput: SessionAction?
    @State private var parser = TerminalAnsi()
    @State private var parsed: ParsedScreen?
    @State private var link: LinkChoice?

    /// A tapped link with the text it was shown as.
    struct LinkChoice: Identifiable {
        let id = UUID()
        let target: TerminalLinkTarget
        let label: String
    }

    struct DangerousKey: Identifiable {
        let key: String
        let binding: String
        var id: String { key + binding }
    }

    struct ParsedScreen {
        let source: String
        let screen: TerminalScreen
    }

    private var model: ConversationModel { app.conversation }
    private var target: Target { session.target }

    var body: some View {
        let current = model.terminalState.flatMap { raw in
            raw.string("name") == target.session && raw.string("run_id") == target.run && raw.string("conversation_id") == target.conversation ? raw : nil
        }
        let displayed = selecting ? frozen : current
        let source = displayed?.string("screen") ?? ""
        let readActive = TerminalPolling.readAllowed(foreground: scenePhase == .active, focused: true, selecting: selecting,
                                                     reviewing: reviewInput != nil, ownedKeyConfirmation: dangerousKey != nil)
        NavigationStack {
            VStack(alignment: .leading, spacing: 4) {
                TimelineView(.periodic(from: .now, by: 0.5)) { _ in
                    let fresh = model.terminalFresh(target)
                    Text(selecting ? "Selection frozen" : fresh ? "Connected" : current != nil ? "Last known" : "Connecting…")
                        .font(.caption)
                        .foregroundStyle(fresh && !selecting ? Theme.mint : Theme.muted)
                }
                .padding(.horizontal, 16)
                if !model.terminalError.isEmpty {
                    Text(model.terminalError).font(.footnote).foregroundStyle(Theme.amber).padding(.horizontal, 16)
                }
                if parsed?.screen.truncated == true || displayed?.bool("truncated") == true {
                    Text("Captured screen is truncated.").font(.caption).foregroundStyle(Theme.muted).padding(.horizontal, 16)
                }
                Group {
                    if let parsed, parsed.source == source, displayed != nil {
                        TerminalScreenView(screen: parsed.screen, raw: displayed, fontSize: fontSize, selecting: selecting) { chosen in
                            let utf16 = parsed.screen.text.utf16
                            let start = utf16.index(utf16.startIndex, offsetBy: chosen.start), end = utf16.index(utf16.startIndex, offsetBy: chosen.end)
                            link = LinkChoice(target: TerminalLinkTarget.of(chosen.uri), label: String(String(parsed.screen.text[start..<end]).prefix(200)))
                        }
                    } else {
                        Text("Loading captured screen…").foregroundStyle(Theme.muted).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                    }
                }
                .padding(8)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                recentInput
                TerminalInputEditor(target: target, selecting: selecting) { key in
                    if ["C-c", "C-d"].contains(key) {
                        if let binding = current?.string("terminal_binding_id").nonEmpty { dangerousKey = DangerousKey(key: key, binding: binding) }
                    } else {
                        model.terminalInput(target, key: key)
                    }
                }
            }
            .background(Color(argb: terminalBackground).ignoresSafeArea())
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button { dismiss() } label: { Image(systemName: "chevron.backward") }
                        .accessibilityLabel("Back to Activity")
                }
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 0) {
                        Text("Terminal").font(.headline)
                        Text(session.title).font(.caption2).foregroundStyle(Theme.muted).lineLimit(1)
                    }
                }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button { fontSize = max(9, fontSize - 1) } label: { Image(systemName: "textformat.size.smaller") }
                        .accessibilityLabel("Zoom out")
                    Button { fontSize = min(24, fontSize + 1) } label: { Image(systemName: "textformat.size.larger") }
                        .accessibilityLabel("Zoom in")
                    Button(selecting ? "Live" : "Select") {
                        if !selecting { frozen = current }
                        selecting.toggle()
                    }
                    .disabled(current == nil)
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(Color(argb: terminalBackground), for: .navigationBar)
        }
        .preferredColorScheme(.dark)
        .onAppear { model.setTerminalVisible(target, true) }
        .onDisappear { model.setTerminalVisible(target, false) }
        .task(id: readActive) {
            guard readActive else { return }
            var policy = TerminalPolling()
            var previous = model.terminalState?.string("screen")
            while !Task.isCancelled {
                if !model.terminalLoading { model.terminalSnapshot(target) }
                while model.terminalLoading && !Task.isCancelled { try? await Task.sleep(for: .milliseconds(50)) }
                let next = model.terminalState?.string("screen")
                policy = policy.observed(changed: next != previous)
                previous = next
                try? await Task.sleep(for: .milliseconds(policy.delayMilliseconds))
            }
        }
        .task(id: source) {
            let parser = parser, text = source
            let screen = await Task.detached(priority: .userInitiated) { parser.parse(text) }.value
            parsed = ParsedScreen(source: text, screen: screen)
        }
        .alert(dangerousKey?.key == "C-c" ? "Send Ctrl+C?" : "Send Ctrl+D?", isPresented: Binding(get: { dangerousKey != nil }, set: { if !$0 { dangerousKey = nil } }),
               presenting: dangerousKey) { danger in
            let valid = model.terminalState?.string("terminal_binding_id") == danger.binding && model.terminalReason(target).isEmpty
            Button("Send \(danger.key)", role: .destructive) {
                if model.terminalState?.string("terminal_binding_id") == danger.binding && model.terminalReason(target).isEmpty {
                    model.terminalInput(target, key: danger.key)
                }
                dangerousKey = nil
            }
            .disabled(!valid)
            Button("Cancel", role: .cancel) { dangerousKey = nil }
        } message: { danger in
            Text("Send \(danger.key) to \(session.title) on \(app.machineName(target.machineKey, fallback: target.computerId))? This may interrupt or close the native program.")
        }
        .alert(Self.linkTitle(link?.target, computer: computer), isPresented: Binding(get: { link != nil }, set: { if !$0 { link = nil } }),
               presenting: link) { choice in
            switch choice.target {
            case .web(_, let url):
                Button("Open in Browser") { openURL(url) }
            case .file(_, _, let reference):
                Button("Read on \(computer)") { app.computerFiles.show(target, reference) }
            case .other:
                EmptyView()
            }
            Button("Copy Link") { UIPasteboard.general.string = choice.target.uri }
            Button("Cancel", role: .cancel) {}
        } message: { choice in
            Text("Shown as: \(choice.label)\n\n\(choice.target.uri)\n\n\(Self.linkDetail(choice.target, computer: computer))")
        }
        // Terminal covers the conversation, so its file links present the Open or Save sheet here.
        .sheet(item: Binding(get: { app.computerFiles.selection }, set: { if $0 == nil { app.computerFiles.close() } })) { ComputerFileSheet(selection: $0) }
        .alert("Review uncertain Terminal input", isPresented: Binding(get: { reviewInput != nil }, set: { if !$0 { reviewInput = nil } }),
               presenting: reviewInput) { action in
            Button("Mark reviewed") {
                model.reviewSessionAction(action)
                reviewInput = nil
            }
            Button("Cancel", role: .cancel) { reviewInput = nil }
        } message: { action in
            let input = action.arguments.has("text") ? action.arguments.string("text") : action.arguments.string("key")
            Text("Input may already have reached \(session.title). Check its original receipt and the native screen before preparing it again. Marking reviewed does not resend it.\n\n\(String(input.prefix(600)))")
        }
    }

    @ViewBuilder
    private var recentInput: some View {
        if let action = model.messageState.sessionActions.filter({ $0.target == target && $0.operation == "terminal_input" }).max(by: { $0.createdAt < $1.createdAt }) {
            VStack(alignment: .leading, spacing: 2) {
                Text(Self.inputStatus(action.status)).font(.caption).foregroundStyle(["failed", "uncertain"].contains(action.status) ? Theme.amber : Theme.muted)
                if ["failed", "uncertain", "reviewed"].contains(action.status) {
                    HStack(spacing: 16) {
                        Button("Check result") { model.checkSessionAction(action) }.disabled(model.actionFlights.contains(action.requestId))
                        if action.status == "uncertain" { Button("Review input") { reviewInput = action } }
                        if ["failed", "reviewed"].contains(action.status) && action.arguments.has("text") {
                            Button("Restore input") { model.restoreTerminalInput(action) }
                        }
                    }
                    .font(.caption.weight(.semibold))
                    .buttonStyle(.borderless)
                }
            }
            .padding(.horizontal, 12)
        }
    }

    private var computer: String { app.machineName(target.machineKey, fallback: target.computerId) }

    static func linkTitle(_ target: TerminalLinkTarget?, computer: String) -> String {
        switch target {
        case .web: "Open link?"
        case .file: "Read file from \(computer)?"
        case .other, nil: "Terminal link"
        }
    }

    /// The whole target is shown before anything opens; its visible text may say something else.
    static func linkDetail(_ target: TerminalLinkTarget, computer: String) -> String {
        switch target {
        case .web: return "Opens in your browser."
        case .file(_, let host, let reference):
            let named = host.isEmpty || host.lowercased() == "localhost" ? "" : " The link names host \(host)."
            return "Zerus reads \(reference.path) on \(computer), the computer of this session.\(named)"
        case .other: return "Zerus opens only web and file links. You can copy this one."
        }
    }

    static func inputStatus(_ status: String) -> String {
        switch status {
        case "sending": "Sending input…"
        case "submitted", "completed": "Input handed to the native terminal."
        case "uncertain": "Input result unknown. Check the original receipt; do not repeat it."
        case "failed": "Input was not sent."
        default: status.prefix(1).uppercased() + status.dropFirst()
        }
    }
}

/// The captured screen at its native size, scrollable both ways, with the native cursor cell.
/// Following starts on open, stops when the user scrolls away and never moves a selection.
struct TerminalScreenView: View {
    let screen: TerminalScreen
    let raw: JSONObject?
    let fontSize: CGFloat
    let selecting: Bool
    var onLink: (TerminalLink) -> Void = { _ in }
    @State private var position = ScrollPosition()
    @State private var following = true
    @State private var dragged = false
    @State private var viewport: Viewport?

    /// Scroll sizes without the offset, so scrolling itself does not render the screen again.
    struct Viewport: Equatable {
        let content: CGSize
        let container: CGSize
        let insets: EdgeInsets

        var width: Double { container.width - insets.leading - insets.trailing }
        var height: Double { container.height - insets.top - insets.bottom }
        var maxX: Double { content.width - width }
        var maxY: Double { content.height - height }
    }

    var body: some View {
        let font = UIFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
        let cellWidth = ("M" as NSString).size(withAttributes: [.font: font]).width
        let lineHeight = font.lineHeight
        let cols = min(max(raw?.int("cols") ?? 1, 1), 1000), rows = min(max(raw?.int("rows") ?? 1, 1), 1000)
        let cursor = raw?.object("cursor")
        let x = cursor.map { $0.has("x") ? $0.int("x") : -1 } ?? -1, y = cursor.map { $0.has("y") ? $0.int("y") : -1 } ?? -1
        let followed = viewport.map { viewport in
            CGPoint(x: TerminalFollow.horizontal(cursorX: x, cols: cols, cell: cellWidth, viewport: viewport.width, max: viewport.maxX),
                    y: TerminalFollow.vertical(row: TerminalFollow.row(cursorY: y, rows: rows), line: lineHeight,
                                               viewport: viewport.height, max: viewport.maxY))
        }
        let target = following && !selecting ? followed : nil
        GeometryReader { proxy in
        ZStack(alignment: .bottomTrailing) {
        ScrollView([.horizontal, .vertical]) {
            Text(Self.attributed(screen, size: fontSize, links: !selecting))
                .font(Font(font))
                .foregroundStyle(Color(argb: terminalForeground))
                .lineSpacing(0)
                .fixedSize()
                .frame(minWidth: CGFloat(cols) * cellWidth, minHeight: CGFloat(rows) * lineHeight, alignment: .topLeading)
                .overlay(alignment: .topLeading) {
                    // Native coordinates also cover a cursor on blank end-of-line, where no glyph exists.
                    if !selecting && (0..<cols).contains(x) && (0..<rows).contains(y) {
                        Rectangle()
                            .stroke(Color.white, lineWidth: 1)
                            .frame(width: cellWidth, height: lineHeight)
                            .offset(x: CGFloat(x) * cellWidth, y: CGFloat(y) * lineHeight)
                            .allowsHitTesting(false)
                    }
                }
                .modifier(Selectable(enabled: selecting))
                // Links carry only their index; nothing opens before the confirmation.
                .environment(\.openURL, OpenURLAction { url in
                    if url.scheme == Self.linkScheme, let index = Int(url.host() ?? ""), screen.links.indices.contains(index) {
                        onLink(screen.links[index])
                    }
                    return .handled
                })
                .frame(minWidth: proxy.size.width, minHeight: proxy.size.height, alignment: .topLeading)
        }
        .defaultScrollAnchor(.topLeading)
        .scrollPosition($position)
        .onScrollGeometryChange(for: Viewport.self) { geometry in
            Viewport(content: geometry.contentSize, container: geometry.containerSize, insets: geometry.contentInsets)
        } action: { _, value in
            viewport = value
        }
        .onScrollPhaseChange { _, phase, context in
            if phase == .interacting {
                dragged = true
                following = false
            } else if phase == .idle && dragged, let followed {
                dragged = false
                let offset = context.geometry.contentOffset, insets = context.geometry.contentInsets
                following = TerminalFollow.atFollowed(y: offset.y + insets.top, x: offset.x + insets.leading, followedY: followed.y,
                                                      followedX: followed.x, line: lineHeight, cell: cellWidth)
            }
        }
        if !following && !selecting {
            Button { following = true } label: { Label("Follow cursor", systemImage: "arrow.down.to.line") }
                .buttonStyle(.borderedProminent)
                .font(.footnote)
                .padding(8)
        }
        }
        }
        .onChange(of: target, initial: true) { _, target in
            guard let target, let viewport else { return }
            position.scrollTo(point: CGPoint(x: target.x - viewport.insets.leading, y: target.y - viewport.insets.top))
        }
    }

    /// Text selection works on the frozen snapshot only.
    struct Selectable: ViewModifier {
        let enabled: Bool
        func body(content: Content) -> some View {
            if enabled { content.textSelection(.enabled) } else { content }
        }
    }

    static let linkScheme = "zerus-terminal-link"

    static func attributed(_ screen: TerminalScreen, size: CGFloat, links: Bool) -> AttributedString {
        let utf16 = screen.text.utf16
        var result = AttributedString()
        var position = 0
        var index = utf16.startIndex
        var link = 0
        // Appends the text up to `end`, split where OSC 8 links start or end. Links stay plain
        // underlined text while selecting; otherwise a tap asks before anything opens.
        func append(_ end: Int, _ styled: (inout AttributedString) -> Void) {
            while position < end {
                while link < screen.links.count, screen.links[link].end <= position { link += 1 }
                let current = link < screen.links.count ? screen.links[link] : nil
                let inside = current.map { $0.start <= position } ?? false
                let stop = min(end, current.map { inside ? $0.end : $0.start } ?? end)
                let next = utf16.index(index, offsetBy: stop - position)
                var piece = AttributedString(screen.text[index..<next])
                styled(&piece)
                if inside {
                    piece.underlineStyle = .single
                    if links { piece.link = URL(string: "\(linkScheme)://\(link)") }
                }
                result += piece
                index = next
                position = stop
            }
        }
        for span in screen.spans where span.start >= position && span.end > span.start {
            if span.start > position { append(span.start) { _ in } }
            append(span.end) { piece in
                let style = span.style
                let foreground = style.inverse ? style.background ?? terminalBackground : style.foreground ?? terminalForeground
                let background = style.inverse ? style.foreground ?? terminalForeground : style.background
                if foreground != terminalForeground || style.dim { piece.foregroundColor = Color(argb: foreground).opacity(style.dim ? 0.5 : 1) }
                if let background { piece.backgroundColor = Color(argb: background) }
                if style.bold || style.italic {
                    var font = Font.system(size: size, weight: style.bold ? .bold : .regular, design: .monospaced)
                    if style.italic { font = font.italic() }
                    piece.font = font
                }
                if style.underline { piece.underlineStyle = .single }
                if style.strike { piece.strikethroughStyle = .single }
            }
        }
        if position < utf16.count { append(utf16.count) { _ in } }
        return result
    }
}

/// Terminal input is a separate private composition; Insert sends without a newline.
struct TerminalInputEditor: View {
    @Environment(AppModel.self) private var app
    let target: Target
    let selecting: Bool
    let onKey: (String) -> Void
    @State private var text = ""
    @State private var generation = ""

    static let keys: [(String, String)] = [("Escape", "Esc"), ("Tab", "Tab"), ("BTab", "Shift+Tab"), ("BSpace", "⌫"), ("Up", "↑"), ("Down", "↓"),
                                           ("Left", "←"), ("Right", "→"), ("Home", "Home"), ("End", "End"), ("PPage", "PgUp"), ("NPage", "PgDn"),
                                           ("DC", "Delete"), ("C-c", "Ctrl+C"), ("C-d", "Ctrl+D"), ("C-l", "Ctrl+L"), ("C-a", "Ctrl+A"), ("C-e", "Ctrl+E"),
                                           ("C-u", "Ctrl+U"), ("C-w", "Ctrl+W")]

    private var model: ConversationModel { app.conversation }

    var body: some View {
        let buffer = model.terminalBuffer(target)
        let reason = model.terminalReason(target)
        let tooLarge = text.utf8.count > 65_536
        let enabled = reason.isEmpty && !selecting && buffer != nil
        VStack(alignment: .leading, spacing: 6) {
            if tooLarge { Text("Terminal input exceeds 64 KiB. Shorten it before sending or saving.").font(.caption).foregroundStyle(Theme.amber) }
            if !reason.isEmpty { Text(reason).font(.caption).foregroundStyle(Theme.muted) }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(Self.keys, id: \.0) { key, label in
                        Button(label) { onKey(key) }
                            .font(.footnote.monospaced())
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                }
            }
            .disabled(!enabled)
            HStack(alignment: .bottom, spacing: 8) {
                TextField("Terminal input (kept separately)", text: $text, axis: .vertical)
                    .font(.body.monospaced())
                    .lineLimit(1...4)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .disabled(buffer == nil || selecting)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
                    .background(Theme.surface, in: RoundedRectangle(cornerRadius: 12))
                VStack(spacing: 6) {
                    Button("Insert text") {
                        model.editTerminal(target, text)
                        model.terminalInput(target, enter: false)
                    }
                    .buttonStyle(.bordered)
                    .disabled(!enabled || tooLarge || text.isEmpty)
                    Button("Enter / Send") {
                        model.editTerminal(target, text)
                        if text.isEmpty { onKey("Enter") } else { model.terminalInput(target, enter: true) }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!enabled || tooLarge)
                }
                .font(.footnote)
            }
            Text("Insert text sends without a newline. Enter sends buffered text and submits it. Special keys go directly to the native program.")
                .font(.caption2)
                .foregroundStyle(Theme.muted)
        }
        .padding(8)
        .background(.bar)
        .onAppear { reconcile(buffer) }
        .onChange(of: buffer?.generation) { reconcile(model.terminalBuffer(target)) }
        .onChange(of: buffer?.binding) { reconcile(model.terminalBuffer(target)) }
        .onChange(of: text) { _, value in
            if let current = model.terminalBuffer(target), current.binding == buffer?.binding, value != current.text { model.editTerminal(target, value) }
        }
    }

    private func reconcile(_ buffer: TerminalBuffer?) {
        guard let buffer else { return }
        let update = ComposerReconciler.external(current: generation, generation: buffer.generation, text: buffer.text)
        generation = update.generation
        if let replacement = update.replacement { text = replacement }
    }
}
