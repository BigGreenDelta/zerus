import Foundation

/// Presentation snapshots built from Foundation's CommonMark/GFM parser. They
/// never load external resources; unsafe destinations stay visible and inert.
public struct MarkdownDocument: Hashable, Sendable {
    public let source: String
    public let blocks: [MarkdownBlock]
    public let literalFallback: Bool
    public let estimatedBytes: Int

    public static func literal(_ source: String) -> MarkdownDocument {
        MarkdownDocument(source: source, blocks: [], literalFallback: true, estimatedBytes: source.utf8.count * 4)
    }
}

public indirect enum MarkdownBlock: Hashable, Sendable {
    case paragraph([MarkdownInline])
    case heading(level: Int, content: [MarkdownInline])
    case literal(text: String, code: Bool, language: String)
    case quote([MarkdownBlock])
    case items(start: Int?, items: [MarkdownListItem])
    case rule
    case table(rows: [[MarkdownTableCell]])
}

public struct MarkdownListItem: Hashable, Sendable {
    public let blocks: [MarkdownBlock]
    public let checked: Bool?
}

public enum MarkdownAlignment: Hashable, Sendable { case left, center, right }

public struct MarkdownTableCell: Hashable, Sendable {
    public let content: [MarkdownInline]
    public let header: Bool
    public let alignment: MarkdownAlignment?
}

public enum MarkdownStyle: Hashable, Sendable { case strong, emphasis, strike }

public indirect enum MarkdownInline: Hashable, Sendable {
    case text(String)
    case styled(MarkdownStyle, [MarkdownInline])
    case code(String)
    case link(content: [MarkdownInline], destination: String, safeURL: URL?)
    case image(alt: String)
    case lineBreak
}

public enum MarkdownText {
    /// Absolute http(s) URLs only, checked when parsed and again on activation.
    public static func safeURL(_ destination: String) -> URL? {
        guard !destination.isEmpty, !destination.unicodeScalars.contains(where: { $0.properties.isWhitespace || $0.value < 32 || $0.value == 127 || $0 == "\\" }),
              destination.range(of: "%(?:0[0-9a-f]|1[0-9a-f]|7f)", options: [.regularExpression, .caseInsensitive]) == nil,
              let components = URLComponents(string: destination), let scheme = components.scheme?.lowercased(), ["http", "https"].contains(scheme),
              let host = components.host, !host.isEmpty, components.user == nil, components.password == nil,
              destination.lowercased().hasPrefix(scheme + "://"), let url = components.url else { return nil }
        if let port = components.port, !(0...65535).contains(port) { return nil }
        return url
    }

    /// Text that people see and select, including inert destinations and image descriptions.
    public static func plain(_ content: [MarkdownInline]) -> String {
        var result = ""
        func append(_ inlines: [MarkdownInline]) {
            for inline in inlines {
                switch inline {
                case .text(let text), .code(let text): result += text
                case .styled(_, let children): append(children)
                case .link(let children, let destination, let safe):
                    append(children)
                    if safe == nil && plain(children) != destination { result += " (\(destination))" }
                case .image(let alt): result += alt.trimmingCharacters(in: .whitespaces).isEmpty ? "[Image]" : "[Image: \(alt)]"
                case .lineBreak: result += "\n"
                }
            }
        }
        append(content)
        return result
    }
}

public struct MarkdownParser: Sendable {
    public var maxSourceBytes = 256 * 1024
    public var maxDepth = 32
    public var maxNodes = 8192
    public var maxTableCells = 4096
    public var maxCodeLineColumns = 4096

    public init() {}

    struct Budget: Error {}

    /// A block-structure node keyed by Foundation's presentation identity.
    final class Node {
        let identity: Int
        let kind: PresentationIntent.Kind?
        var children: [Node] = []
        var runs: [Run] = []
        init(identity: Int, kind: PresentationIntent.Kind?) {
            self.identity = identity
            self.kind = kind
        }
    }

    struct Run {
        let text: String
        let inline: InlinePresentationIntent
        let link: URL?
        let image: Bool
    }

    public func parse(_ source: String) -> MarkdownDocument {
        guard source.utf8.count <= maxSourceBytes else { return .literal(source) }
        do {
            let parsed = try AttributedString(markdown: source, options: .init(allowsExtendedAttributes: true, interpretedSyntax: .full,
                                                                                failurePolicy: .returnPartiallyParsedIfPossible))
            let root = Node(identity: 0, kind: nil)
            var nodes = 0, cells = 0, weight = 0, html = -1
            for run in parsed.runs {
                let text = String(parsed[run.range].characters)
                nodes += 1
                weight += 128 + text.utf8.count * 2
                let components = run.presentationIntent?.components ?? []
                guard components.count <= maxDepth else { throw Budget() }
                var current = root
                for component in components.reversed() {
                    if let last = current.children.last, last.identity == component.identity {
                        current = last
                    } else {
                        let node = Node(identity: component.identity, kind: component.kind)
                        current.children.append(node)
                        current = node
                        nodes += 1
                        if case .tableCell = component.kind { cells += 1 }
                    }
                }
                if components.isEmpty {
                    // Raw HTML blocks carry no presentation intent; keep them as one literal block.
                    if let last = root.children.last, last.identity == html { current = last } else {
                        html -= 1
                        let node = Node(identity: html, kind: nil)
                        root.children.append(node)
                        current = node
                    }
                }
                current.runs.append(Run(text: text, inline: run.inlinePresentationIntent ?? [], link: run.link, image: run.imageURL != nil))
                guard nodes <= maxNodes, cells <= maxTableCells else { throw Budget() }
            }
            let blocks = try blocks(root.children)
            return MarkdownDocument(source: source, blocks: blocks, literalFallback: false, estimatedBytes: source.utf8.count * 4 + weight)
        } catch {
            return .literal(source)
        }
    }

    func blocks(_ nodes: [Node]) throws -> [MarkdownBlock] {
        try nodes.compactMap { node -> MarkdownBlock? in
            guard let kind = node.kind else {
                var text = node.runs.map(\.text).joined()
                if text.hasSuffix("\n") { text.removeLast() }
                return .literal(text: text, code: false, language: "")
            }
            switch kind {
            case .paragraph: return .paragraph(inlines(node.runs))
            case .header(let level): return .heading(level: level, content: inlines(node.runs))
            case .codeBlock(let language):
                let text = node.runs.map(\.text).joined()
                guard codeLinesFit(text) else { throw Budget() }
                return .literal(text: text, code: true, language: language ?? "")
            case .blockQuote: return .quote(try blocks(node.children))
            case .orderedList, .unorderedList:
                var start: Int?
                let items = try node.children.map { child -> MarkdownListItem in
                    if case .listItem(let ordinal) = child.kind, case .orderedList = kind, start == nil { start = ordinal }
                    return task(try blocks(child.children))
                }
                if case .orderedList = kind { return .items(start: start ?? 1, items: items) }
                return .items(start: nil, items: items)
            case .thematicBreak: return .rule
            case .table(let columns):
                let rows = node.children.map { row -> [MarkdownTableCell] in
                    var header = false
                    if case .tableHeaderRow = row.kind { header = true }
                    return row.children.map { cell in
                        var alignment: MarkdownAlignment?
                        if case .tableCell(let column) = cell.kind, columns.indices.contains(column) {
                            switch columns[column].alignment {
                            case .left: alignment = .left
                            case .center: alignment = .center
                            case .right: alignment = .right
                            @unknown default: alignment = nil
                            }
                        }
                        return MarkdownTableCell(content: inlines(cell.runs), header: header, alignment: alignment)
                    }
                }
                return .table(rows: rows)
            default:
                // Unknown containers keep their readable content.
                return node.children.isEmpty ? .paragraph(inlines(node.runs)) : .quote(try blocks(node.children))
            }
        }
    }

    /// GFM task markers that Foundation leaves as literal text.
    func task(_ blocks: [MarkdownBlock]) -> MarkdownListItem {
        guard case .paragraph(var content)? = blocks.first, case .text(let text)? = content.first else { return MarkdownListItem(blocks: blocks, checked: nil) }
        let markers: [(String, Bool)] = [("[ ] ", false), ("[x] ", true), ("[X] ", true)]
        guard let marker = markers.first(where: { text.hasPrefix($0.0) }) else { return MarkdownListItem(blocks: blocks, checked: nil) }
        let rest = String(text.dropFirst(marker.0.count))
        if rest.isEmpty { content.removeFirst() } else { content[0] = .text(rest) }
        return MarkdownListItem(blocks: [.paragraph(content)] + blocks.dropFirst(), checked: marker.1)
    }

    func codeLinesFit(_ text: String) -> Bool {
        var columns = 0
        for scalar in text.unicodeScalars {
            switch scalar {
            case "\n", "\r": columns = 0
            case "\t": columns += 4 - columns % 4
            default: columns += 1
            }
            if columns > maxCodeLineColumns { return false }
        }
        return true
    }

    static let styles: [(InlinePresentationIntent, MarkdownStyle)] = [(.stronglyEmphasized, .strong), (.emphasized, .emphasis), (.strikethrough, .strike)]

    func inlines(_ runs: [Run]) -> [MarkdownInline] {
        var result: [MarkdownInline] = []
        var index = 0
        while index < runs.count {
            let run = runs[index]
            if let link = run.link {
                var end = index
                while end + 1 < runs.count, runs[end + 1].link == link { end += 1 }
                let destination = Self.destination(link)
                result.append(.link(content: styled(Array(runs[index...end]).map { Run(text: $0.text, inline: $0.inline, link: nil, image: $0.image) }),
                                    destination: destination, safeURL: MarkdownText.safeURL(destination)))
                index = end + 1
            } else {
                var end = index
                while end + 1 < runs.count, runs[end + 1].link == nil { end += 1 }
                result += styled(Array(runs[index...end]))
                index = end + 1
            }
        }
        return Self.merged(result)
    }

    /// Groups consecutive runs that share a style, outermost first.
    func styled(_ runs: [Run]) -> [MarkdownInline] {
        var result: [MarkdownInline] = []
        var index = 0
        while index < runs.count {
            let run = runs[index]
            if let (flag, style) = Self.styles.first(where: { run.inline.contains($0.0) }) {
                var end = index
                while end + 1 < runs.count, runs[end + 1].inline.contains(flag) { end += 1 }
                let inner = runs[index...end].map { Run(text: $0.text, inline: $0.inline.subtracting(flag), link: $0.link, image: $0.image) }
                result.append(.styled(style, Self.merged(styled(inner))))
                index = end + 1
                continue
            }
            if run.image { result.append(.image(alt: run.text.replacingOccurrences(of: "\u{FFFC}", with: ""))) }
            else if run.inline.contains(.code) { result.append(.code(run.text)) }
            else if run.inline.contains(.lineBreak) { result.append(.lineBreak) }
            else if run.inline.contains(.softBreak) { result.append(.text(" ")) }
            else { result.append(.text(run.text)) }
            index += 1
        }
        return result
    }

    static func merged(_ inlines: [MarkdownInline]) -> [MarkdownInline] {
        var result: [MarkdownInline] = []
        for inline in inlines {
            if case .text(let next) = inline, case .text(let previous)? = result.last {
                result[result.count - 1] = .text(previous + next)
            } else {
                result.append(inline)
            }
        }
        return result
    }

    /// Foundation percent-encodes relative destinations; computer paths keep their exact text.
    static func destination(_ url: URL) -> String {
        if url.scheme == nil { return url.absoluteString.removingPercentEncoding ?? url.absoluteString }
        return url.absoluteString
    }
}

/// Exact-content in-memory LRU with at most two parses at a time; same-content requests share one parse.
public actor MarkdownDocumentCache {
    let maxEntries: Int
    let maxBytes: Int
    let parse: @Sendable (String) -> MarkdownDocument
    var entries: [String: MarkdownDocument] = [:]
    var order: [String] = []
    var bytes = 0
    var flights: [String: Task<MarkdownDocument, Never>] = [:]
    var running = 0
    var waiting: [CheckedContinuation<Void, Never>] = []

    public static let shared = MarkdownDocumentCache()

    public init(maxEntries: Int = 128, maxBytes: Int = 4 * 1024 * 1024, parse: @escaping @Sendable (String) -> MarkdownDocument = { MarkdownParser().parse($0) }) {
        self.maxEntries = maxEntries
        self.maxBytes = maxBytes
        self.parse = parse
    }

    public func cached(_ source: String) -> MarkdownDocument? { entries[source] }

    public func document(_ source: String) async -> MarkdownDocument {
        if let document = entries[source] {
            order.removeAll { $0 == source }
            order.append(source)
            return document
        }
        if let flight = flights[source] { return await flight.value }
        let parse = self.parse
        let flight = Task<MarkdownDocument, Never> {
            await self.acquire()
            let document = await Task.detached(priority: .userInitiated) { parse(source) }.value
            await self.finish(source, document)
            return document
        }
        flights[source] = flight
        return await flight.value
    }

    func acquire() async {
        if running < 2 {
            running += 1
            return
        }
        await withCheckedContinuation { waiting.append($0) }
    }

    func finish(_ source: String, _ document: MarkdownDocument) {
        flights[source] = nil
        if maxEntries > 0, document.estimatedBytes <= maxBytes {
            if let old = entries.updateValue(document, forKey: source) { bytes -= old.estimatedBytes }
            bytes += document.estimatedBytes
            order.removeAll { $0 == source }
            order.append(source)
            while entries.count > maxEntries || bytes > maxBytes, !order.isEmpty {
                let oldest = order.removeFirst()
                if let removed = entries.removeValue(forKey: oldest) { bytes -= removed.estimatedBytes }
            }
        }
        if waiting.isEmpty { running -= 1 } else { waiting.removeFirst().resume() }
    }

    public func snapshot() -> (entries: Int, bytes: Int) { (entries.count, bytes) }
}
