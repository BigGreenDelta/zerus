import XCTest
@testable import ZerusKit

/// Ported from Android MarkdownPresentationTest and MarkdownDocumentCacheTest.
final class MarkdownTests: XCTestCase {
    private let parser = MarkdownParser()

    private func paragraph(_ source: String) -> [MarkdownInline] {
        let result = parser.parse(source)
        XCTAssertFalse(result.literalFallback)
        guard case .paragraph(let content)? = result.blocks.first, result.blocks.count == 1 else {
            XCTFail("Expected one paragraph, got \(result.blocks)")
            return []
        }
        return content
    }
    private func links(_ content: [MarkdownInline]) -> [(String, URL?)] {
        content.compactMap { if case .link(_, let destination, let safe) = $0 { (destination, safe) } else { nil } }
    }

    func testComputerDocumentLinksKeepTheirExactUnicodePath() {
        let path = "/workspace/example/Пример_документов/Сводная итог.xlsx"
        let found = links(paragraph("[Download spreadsheet](<\(path)>)"))
        XCTAssertEqual([path], found.map(\.0))
        XCTAssertNil(found.first?.1)
    }

    func testHeadingsParagraphsAndInlineStylesFollowCommonMark() {
        let source = (1...6).map { String(repeating: "#", count: $0) + " Heading \($0)" }.joined(separator: "\n\n") +
            "\n\nA **strong and *nested***, ~~removed~~, `x < y` &amp; value.\nSoft  \nHard"
        let document = parser.parse(source)
        XCTAssertFalse(document.literalFallback)
        XCTAssertEqual(Array(1...6), document.blocks.compactMap { if case .heading(let level, _) = $0 { level } else { nil } })
        guard case .paragraph(let content)? = document.blocks.last else { return XCTFail() }
        XCTAssertEqual("A strong and nested, removed, x < y & value. Soft\nHard", MarkdownText.plain(content))
        guard case .styled(.strong, let strong)? = content.first(where: { if case .styled = $0 { true } else { false } }) else { return XCTFail() }
        XCTAssertTrue(strong.contains { if case .styled(.emphasis, _) = $0 { true } else { false } })
        XCTAssertTrue(content.contains { if case .styled(.strike, _) = $0 { true } else { false } })
        XCTAssertTrue(content.contains(.code("x < y")))
        XCTAssertTrue(content.contains(.lineBreak))
    }

    func testFencedAndIndentedCodeRemainLiteralWithWhitespace() {
        let document = parser.parse("```kotlin\n  **literal** <script>\n\tlast\n```\n\n    indented *literal*\n    second\n")
        XCTAssertEqual(document.blocks, [.literal(text: "  **literal** <script>\n\tlast\n", code: true, language: "kotlin"),
                                         .literal(text: "indented *literal*\nsecond\n", code: true, language: "")])
    }

    func testOrderedTaskAndNestedListsKeepStructureAndStartNumber() {
        let document = parser.parse("3. Third\n4. Fourth\n\n- [x] Finished\n- [ ] Waiting\n  - Nested\n")
        XCTAssertFalse(document.literalFallback)
        guard case .items(let start, let ordered)? = document.blocks.first, case .items(let none, let tasks)? = document.blocks.last else { return XCTFail() }
        XCTAssertEqual(3, start)
        XCTAssertEqual(2, ordered.count)
        XCTAssertNil(none)
        XCTAssertEqual([true, false], tasks.map(\.checked))
        guard case .paragraph(let waiting)? = tasks[1].blocks.first, case .items(_, let nested)? = tasks[1].blocks.last,
              case .paragraph(let child)? = nested.first?.blocks.first else { return XCTFail() }
        XCTAssertEqual("Waiting", MarkdownText.plain(waiting))
        XCTAssertEqual("Nested", MarkdownText.plain(child))
    }

    func testNestedQuotesRulesAndSetextHeadingsAreBlocks() {
        let document = parser.parse("> Outer\n>\n> > Inner\n\n---\n\nSetext\n======\n")
        guard case .quote(let outer)? = document.blocks.first, case .quote(let inner)? = outer.last, case .paragraph(let text)? = inner.first else { return XCTFail() }
        XCTAssertEqual("Inner", MarkdownText.plain(text))
        XCTAssertEqual(.rule, document.blocks[1])
        guard case .heading(1, _) = document.blocks[2] else { return XCTFail() }
    }

    func testTablesKeepHeaderInlineContentAndColumnAlignment() {
        guard case .table(let rows)? = parser.parse("| Left | Center | Right |\n| :--- | :---: | ---: |\n| **bold** | `x` | [web](https://example.com) |").blocks.first else {
            return XCTFail()
        }
        XCTAssertEqual(2, rows.count)
        XCTAssertTrue(rows[0].allSatisfy(\.header))
        XCTAssertFalse(rows[1].contains(where: \.header))
        XCTAssertEqual([.left, .center, .right], rows[0].map(\.alignment))
        XCTAssertEqual(["bold", "x", "web"], rows[1].map { MarkdownText.plain($0.content) })
        guard case .styled? = rows[1][0].content.first else { return XCTFail() }
    }

    func testRawHtmlIsLiteralAndImagesCarryAltOnlyWithoutLinks() {
        let html = "<script src=\"https://example.com/load.js\">alert(1)</script>"
        XCTAssertEqual([.literal(text: html, code: false, language: "")], parser.parse(html).blocks)
        let content = paragraph("<b>literal</b> ![**diagram** [label](https://example.com)](https://example.com/image.png)")
        XCTAssertEqual("<b>literal</b> [Image: diagram label]", MarkdownText.plain(content))
        XCTAssertTrue(links(content).isEmpty)
        XCTAssertEqual("[Image]", MarkdownText.plain(paragraph("![](file:///example.png)")))
    }

    func testUnsupportedDestinationsRemainVisibleAndNonInteractive() {
        let content = paragraph("[script](javascript:alert%281%29) [file](file:///example.txt) [relative](../example.md) [fragment](#part) [mail](mailto:reader@example.com)")
        XCTAssertEqual(5, links(content).count)
        XCTAssertTrue(links(content).allSatisfy { $0.1 == nil })
        XCTAssertEqual("script (javascript:alert%281%29) file (file:///example.txt) relative (../example.md) fragment (#part) mail (mailto:reader@example.com)",
                       MarkdownText.plain(content))
        XCTAssertEqual("../example.md", MarkdownText.plain(paragraph("[../example.md](../example.md)")))
    }

    func testOnlyExplicitAbsoluteHttpLinksAndBareAutolinksAreActive() {
        let content = paragraph("[one](https://example.com/path?q=a&x=1) https://example.org/test.")
        XCTAssertEqual(["https://example.com/path?q=a&x=1", "https://example.org/test"], links(content).compactMap { $0.1?.absoluteString })
        XCTAssertEqual("one https://example.org/test.", MarkdownText.plain(content))
        XCTAssertNotNil(MarkdownText.safeURL("http://example.com:8080/path"))
        XCTAssertNotNil(MarkdownText.safeURL("https://[2001:db8::1]/"))
        for unsafe in ["//example.com", "https:/example.com", "https://", "https://user@example.com", "https://user:password@example.com",
                       "https://example.com:99999", "https://example.com/\nfile", "https://example.com/%0afile", "https://example.com/%1f",
                       "https://example.com/%7F", "https://example.com\\evil", "javascript:alert(1)", "data:text/html,test", "content://example",
                       "file:///example", "intent://example", "mailto:reader@example.com"] {
            XCTAssertNil(MarkdownText.safeURL(unsafe), unsafe)
        }
    }

    func testByteNodeDepthAndTableBudgetsFallBackToTheEntireOriginalSource() {
        var tiny = MarkdownParser()
        tiny.maxSourceBytes = 10
        var nodes = MarkdownParser()
        nodes.maxNodes = 4
        var depth = MarkdownParser()
        depth.maxDepth = 3
        var table = MarkdownParser()
        table.maxTableCells = 3
        let examples: [(MarkdownParser, String)] = [
            (tiny, String(repeating: "é", count: 6)),
            (nodes, "one **two** and *three* tail"),
            (depth, "> > > > deep\n\nOriginal tail"),
            (table, "| A | B |\n| --- | --- |\n| C | D |\n\nOriginal tail"),
            (parser, String(repeating: "> ", count: 1000) + "deep\n\nOriginal tail"),
            (parser, String(repeating: "paragraph\n\n", count: 8200) + "Original tail"),
            (parser, String(repeating: "x", count: 256 * 1024) + "Original tail"),
        ]
        for (parser, source) in examples {
            let result = parser.parse(source)
            XCTAssertTrue(result.literalFallback, String(source.prefix(40)))
            XCTAssertEqual(source, result.source)
            XCTAssertTrue(result.blocks.isEmpty)
        }
    }

    func testOversizedCodeLinesUseLiteralFallback() {
        XCTAssertFalse(parser.parse("```\n" + String(repeating: "x", count: 4096) + "\n```").literalFallback)
        for source in ["```\n" + String(repeating: "x", count: 4097) + "\n```\n\nOriginal tail",
                       "    " + String(repeating: "x", count: 4097) + "\n\nOriginal tail",
                       "```\n" + String(repeating: "\t", count: 1025) + "\n```\n\nOriginal tail"] {
            XCTAssertTrue(parser.parse(source).literalFallback)
        }
    }

    func testSourceIdentityAndLiteralSelectionsDoNotChangeTheSuppliedText() {
        let source = "# Title\n\n`**literal**` [path](./example.kt#L4) ![chart](https://example.com/chart.png)"
        let result = parser.parse(source)
        XCTAssertEqual(source, result.source)
        guard case .paragraph(let content) = result.blocks[1] else { return XCTFail() }
        let selected = MarkdownText.plain(content)
        XCTAssertEqual("**literal** path (./example.kt#L4) [Image: chart]", selected)
        XCTAssertFalse(selected.contains("chart.png"))
    }

    func testCacheSharesOneParseAndStaysBounded() async {
        final class Counter: @unchecked Sendable {
            let lock = NSLock()
            var value = 0
            func increment() { lock.lock(); value += 1; lock.unlock() }
        }
        let counter = Counter()
        let cache = MarkdownDocumentCache(maxEntries: 2, maxBytes: 1024 * 1024) { source in
            counter.increment()
            return MarkdownParser().parse(source)
        }
        async let first = cache.document("**same**")
        async let second = cache.document("**same**")
        _ = await (first, second)
        _ = await cache.document("**same**")
        XCTAssertEqual(1, counter.value)
        _ = await cache.document("two")
        _ = await cache.document("three")
        let snapshot = await cache.snapshot()
        XCTAssertEqual(2, snapshot.entries)
        let evicted = await cache.cached("**same**")
        XCTAssertNil(evicted)
    }
}
