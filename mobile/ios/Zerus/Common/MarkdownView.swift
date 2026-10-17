import SwiftUI
import ZerusKit

/// Small documents parse on the main actor through a bounded memo; larger ones use the shared actor cache.
@MainActor
enum MarkdownMemo {
    private static var entries: [String: MarkdownDocument] = [:]
    private static var order: [String] = []
    static let syncLimit = 4096

    static func document(_ source: String) -> MarkdownDocument? {
        guard source.utf8.count <= syncLimit else { return nil }
        if let cached = entries[source] { return cached }
        let parsed = MarkdownParser().parse(source)
        entries[source] = parsed
        order.append(source)
        if order.count > 256 { entries[order.removeFirst()] = nil }
        return parsed
    }
}

/// Opens a Markdown link to a file on the conversation's computer; unset outside conversations.
struct ComputerFileLink {
    let open: @MainActor (ComputerFileReference) -> Void
    init(_ open: @escaping @MainActor (ComputerFileReference) -> Void) { self.open = open }

    static let scheme = "zerus-computer-file"

    /// An inert in-app URL that carries the original destination; the system never opens it.
    static func url(_ destination: String) -> URL? {
        var components = URLComponents()
        components.scheme = scheme
        components.host = "open"
        components.queryItems = [URLQueryItem(name: "d", value: destination)]
        return components.url
    }

    static func reference(_ url: URL) -> ComputerFileReference? {
        guard url.scheme == scheme, let destination = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "d" })?.value else { return nil }
        return ComputerFileReference.parse(destination)
    }
}

extension EnvironmentValues {
    @Entry var computerFileLink: ComputerFileLink?
}

/// Renders agent Markdown. Safe http(s) links open in the browser; links to files on the
/// conversation's computer open its file sheet; everything else stays visible as text.
struct MarkdownContent: View {
    let source: String
    @State private var loaded: MarkdownDocument?
    @Environment(\.computerFileLink) private var fileLink

    var body: some View {
        let document = MarkdownMemo.document(source) ?? (loaded?.source == source ? loaded : nil)
        Group {
            if let document, !document.literalFallback {
                MarkdownBlocks(blocks: document.blocks)
            } else {
                Text(verbatim: source)
            }
        }
        .textSelection(.enabled)
        .environment(\.openURL, OpenURLAction { url in
            if let reference = ComputerFileLink.reference(url) {
                guard let fileLink else { return .discarded }
                fileLink.open(reference)
                return .handled
            }
            return ZerusKit.MarkdownText.safeURL(url.absoluteString) != nil ? .systemAction : .discarded
        })
        .task(id: source) {
            guard source.utf8.count > MarkdownMemo.syncLimit else { return }
            loaded = await MarkdownDocumentCache.shared.document(source)
        }
    }

}

struct MarkdownBlocks: View {
    let blocks: [MarkdownBlock]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
                MarkdownBlockView(block: block)
            }
        }
    }
}

struct MarkdownBlockView: View {
    let block: MarkdownBlock

    var body: some View {
        switch block {
        case .paragraph(let content):
            Text(MarkdownInlines.attributed(content))
                .fixedSize(horizontal: false, vertical: true)
        case .heading(let level, let content):
            Text(MarkdownInlines.attributed(content))
                .font(MarkdownBlockView.headingFont(level))
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, level <= 2 ? 4 : 0)
        case .literal(let text, let code, let language):
            if code {
                CodeBlock(text: text, language: language)
            } else {
                Text(verbatim: text)
                    .font(.callout.monospaced())
                    .foregroundStyle(Theme.muted)
                    .fixedSize(horizontal: false, vertical: true)
            }
        case .quote(let blocks):
            HStack(alignment: .top, spacing: 10) {
                RoundedRectangle(cornerRadius: 1.5).fill(Theme.outline).frame(width: 3)
                MarkdownBlocks(blocks: blocks).foregroundStyle(Theme.muted)
            }
        case .items(let start, let items):
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(items.enumerated()), id: \.offset) { index, item in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        marker(start: start, index: index, item: item)
                        MarkdownBlocks(blocks: item.blocks)
                    }
                }
            }
        case .rule:
            Rectangle().fill(Theme.outline).frame(height: 1).padding(.vertical, 4)
        case .table(let rows):
            MarkdownTable(rows: rows)
        }
    }

    @ViewBuilder
    private func marker(start: Int?, index: Int, item: MarkdownListItem) -> some View {
        if let checked = item.checked {
            Image(systemName: checked ? "checkmark.square.fill" : "square")
                .foregroundStyle(checked ? Theme.mint : Theme.muted)
                .accessibilityLabel(checked ? "Done" : "Not done")
        } else if let start {
            Text("\(start + index).").monospacedDigit().foregroundStyle(Theme.muted)
        } else {
            Text("•").foregroundStyle(Theme.muted)
        }
    }

    static func headingFont(_ level: Int) -> Font {
        switch level {
        case 1: .title2.weight(.semibold)
        case 2: .title3.weight(.semibold)
        case 3: .headline
        default: .subheadline.weight(.semibold)
        }
    }
}

/// Code keeps its exact whitespace and scrolls sideways instead of wrapping.
struct CodeBlock: View {
    let text: String
    let language: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if !language.isEmpty {
                Text(language).font(.caption2.weight(.semibold)).foregroundStyle(Theme.muted)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                Text(verbatim: text.hasSuffix("\n") ? String(text.dropLast()) : text)
                    .font(.system(.footnote, design: .monospaced))
                    .fixedSize(horizontal: true, vertical: true)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Theme.background.opacity(0.6), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(Theme.outline, lineWidth: 0.5))
        .contextMenu {
            Button("Copy code", systemImage: "doc.on.doc") { UIPasteboard.general.string = text }
        }
    }
}

struct MarkdownTable: View {
    let rows: [[MarkdownTableCell]]

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            Grid(alignment: .leading, horizontalSpacing: 0, verticalSpacing: 0) {
                ForEach(Array(rows.enumerated()), id: \.offset) { index, row in
                    GridRow {
                        ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                            Text(MarkdownInlines.attributed(cell.content))
                                .font(cell.header ? .footnote.weight(.semibold) : .footnote)
                                .frame(minWidth: 48, maxWidth: 280, alignment: cell.alignment.map(Self.alignment) ?? .leading)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 6)
                                .background(cell.header ? Theme.surfaceVariant : (index % 2 == 0 ? Color.clear : Theme.background.opacity(0.35)))
                        }
                    }
                }
            }
            .overlay(RoundedRectangle(cornerRadius: 6).stroke(Theme.outline, lineWidth: 0.5))
            .clipShape(RoundedRectangle(cornerRadius: 6))
        }
    }

    static func alignment(_ value: MarkdownAlignment) -> Alignment {
        switch value {
        case .left: .leading
        case .center: .center
        case .right: .trailing
        }
    }
}

enum MarkdownInlines {
    static func attributed(_ content: [MarkdownInline]) -> AttributedString {
        var result = AttributedString()
        append(content, into: &result, bold: false, italic: false, strike: false)
        return result
    }

    private static func append(_ content: [MarkdownInline], into result: inout AttributedString, bold: Bool, italic: Bool, strike: Bool) {
        for inline in content {
            switch inline {
            case .text(let text):
                result += styled(text, bold: bold, italic: italic, strike: strike)
            case .styled(let style, let children):
                append(children, into: &result, bold: bold || style == .strong, italic: italic || style == .emphasis, strike: strike || style == .strike)
            case .code(let text):
                var piece = AttributedString(text)
                piece.font = .system(.callout, design: .monospaced)
                piece.backgroundColor = Theme.surfaceVariant
                if strike { piece.strikethroughStyle = .single }
                result += piece
            case .link(let children, let destination, let safe):
                var inner = AttributedString()
                append(children, into: &inner, bold: bold, italic: italic, strike: strike)
                if let link = safe ?? (ComputerFileReference.parse(destination) != nil ? ComputerFileLink.url(destination) : nil) {
                    inner.link = link
                    inner.foregroundColor = Theme.mint
                    inner.underlineStyle = .single
                    result += inner
                } else {
                    result += inner
                    if ZerusKit.MarkdownText.plain(children) != destination {
                        var suffix = AttributedString(" (\(destination))")
                        suffix.foregroundColor = Theme.muted
                        result += suffix
                    }
                }
            case .image(let alt):
                var piece = AttributedString(alt.trimmingCharacters(in: .whitespaces).isEmpty ? "[Image]" : "[Image: \(alt)]")
                piece.foregroundColor = Theme.muted
                result += piece
            case .lineBreak:
                result += AttributedString("\n")
            }
        }
    }

    private static func styled(_ text: String, bold: Bool, italic: Bool, strike: Bool) -> AttributedString {
        var piece = AttributedString(text)
        if bold || italic {
            var intent: InlinePresentationIntent = []
            if bold { intent.insert(.stronglyEmphasized) }
            if italic { intent.insert(.emphasized) }
            piece.inlinePresentationIntent = intent
        }
        if strike { piece.strikethroughStyle = .single }
        return piece
    }
}
