package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class MarkdownPresentationTest {
    private val parser = MarkdownParser()
    private fun paragraph(source: String): List<MarkdownInline> {
        val result = parser.parse(source)
        assertFalse(result.literalFallback)
        return (result.blocks.single() as MarkdownBlock.Paragraph).content
    }

    @Test fun computerDocumentLinksKeepTheirExactUnicodePath() {
        val path="/workspace/example/Пример_документов/Сводная итог.xlsx"
        val link=paragraph("[Download spreadsheet](<$path>)").filterIsInstance<MarkdownInline.Link>().single()
        assertNull(link.safeUrl)
        assertEquals(ComputerFileReference(path),link.fileReference)
    }

    @Test fun headingsParagraphsAndInlineStylesFollowCommonMark() {
        val source = (1..6).joinToString("\n\n") { "${"#".repeat(it)} Heading $it" } +
            "\n\nA **strong and *nested***, ~~removed~~, `x < y` &amp; value.\nSoft  \nHard"
        val document = parser.parse(source)
        assertFalse(document.literalFallback)
        assertEquals((1..6).toList(), document.blocks.filterIsInstance<MarkdownBlock.Heading>().map { it.level })
        val content = (document.blocks.last() as MarkdownBlock.Paragraph).content
        assertEquals("A strong and nested, removed, x < y & value. Soft\nHard", markdownInlineText(content))
        val strong = content.filterIsInstance<MarkdownInline.Styled>().first()
        assertEquals(MarkdownStyle.Strong, strong.style)
        assertEquals(MarkdownStyle.Emphasis, strong.content.filterIsInstance<MarkdownInline.Styled>().single().style)
        assertTrue(content.any { it is MarkdownInline.Styled && it.style == MarkdownStyle.Strike })
        assertTrue(content.any { it is MarkdownInline.Code && it.text == "x < y" })
        assertTrue(content.contains(MarkdownInline.Break))
    }

    @Test fun fencedAndIndentedCodeRemainLiteralWithWhitespace() {
        val document = parser.parse("```kotlin\n  **literal** <script>\n\tlast\n```\n\n    indented *literal*\n    second\n")
        assertFalse(document.literalFallback)
        val code = document.blocks.filterIsInstance<MarkdownBlock.Literal>()
        assertEquals(2, code.size)
        assertEquals("  **literal** <script>\n\tlast\n", code[0].text)
        assertEquals("kotlin", code[0].language)
        assertEquals("indented *literal*\nsecond\n", code[1].text)
        assertTrue(code.all { it.code })
    }

    @Test fun orderedTaskAndNestedListsKeepStructureAndStartNumber() {
        val document = parser.parse("3. Third\n4. Fourth\n\n- [x] Finished\n- [ ] Waiting\n  - Nested\n")
        assertFalse(document.literalFallback)
        val ordered = document.blocks[0] as MarkdownBlock.Items
        assertEquals(3, ordered.start)
        assertEquals(2, ordered.items.size)
        val tasks = document.blocks[1] as MarkdownBlock.Items
        assertNull(tasks.start)
        assertEquals(listOf(true, false), tasks.items.map { it.checked })
        val waiting = tasks.items[1]
        assertEquals("Waiting", markdownInlineText((waiting.blocks[0] as MarkdownBlock.Paragraph).content))
        val nested = waiting.blocks[1] as MarkdownBlock.Items
        assertEquals("Nested", markdownInlineText((nested.items.single().blocks.single() as MarkdownBlock.Paragraph).content))
    }

    @Test fun nestedQuotesRulesAndSetextHeadingsAreBlocks() {
        val document = parser.parse("> Outer\n>\n> > Inner\n\n---\n\nSetext\n======\n")
        assertFalse(document.literalFallback)
        val outer = document.blocks[0] as MarkdownBlock.Quote
        val inner = outer.blocks[1] as MarkdownBlock.Quote
        assertEquals("Inner", markdownInlineText((inner.blocks.single() as MarkdownBlock.Paragraph).content))
        assertEquals(MarkdownBlock.Rule, document.blocks[1])
        assertEquals(1, (document.blocks[2] as MarkdownBlock.Heading).level)
    }

    @Test fun tablesKeepHeaderInlineContentAndColumnAlignment() {
        val table = parser.parse("| Left | Center | Right |\n| :--- | :---: | ---: |\n| **bold** | `x` | [web](https://example.com) |")
            .blocks.single() as MarkdownBlock.Table
        assertEquals(2, table.rows.size)
        assertTrue(table.rows.first().all { it.header })
        assertTrue(table.rows.last().none { it.header })
        assertEquals(listOf(MarkdownAlignment.Left, MarkdownAlignment.Center, MarkdownAlignment.Right), table.rows.first().map { it.alignment })
        assertEquals(listOf("bold", "x", "web"), table.rows.last().map { markdownInlineText(it.content) })
        assertTrue(table.rows[1][0].content.single() is MarkdownInline.Styled)
    }

    @Test fun rawHtmlIsLiteralAndImagesCarryAltOnlyWithoutLinks() {
        val html = "<script src=\"https://example.com/load.js\">alert(1)</script>"
        val block = parser.parse(html).blocks.single() as MarkdownBlock.Literal
        assertFalse(block.code)
        assertEquals(html, block.text)
        val content = paragraph("<b>literal</b> ![**diagram** [label](https://example.com)](https://example.com/image.png)")
        assertEquals("<b>literal</b> [Image: diagram label]", markdownInlineText(content))
        assertEquals("diagram label", content.filterIsInstance<MarkdownInline.Image>().single().alt)
        assertTrue(content.none { it is MarkdownInline.Link })
        assertEquals("[Image]", markdownInlineText(paragraph("![](file:///example.png)")))
    }

    @Test fun unsupportedDestinationsRemainVisibleAndNonInteractive() {
        val content = paragraph("[script](javascript:alert%281%29) [file](file:///example.txt) [relative](../example.md) [fragment](#part) [mail](mailto:reader@example.com)")
        val links = content.filterIsInstance<MarkdownInline.Link>()
        assertEquals(5, links.size)
        assertTrue(links.all { it.safeUrl == null })
        assertEquals("script (javascript:alert%281%29) file (file:///example.txt) relative (../example.md) fragment (#part) mail (mailto:reader@example.com)",
            markdownInlineText(content))
        assertEquals("../example.md", markdownInlineText(paragraph("[../example.md](../example.md)")))
    }

    @Test fun onlyExplicitAbsoluteHttpLinksAndBareAutolinksAreActive() {
        val content = paragraph("[one](https://example.com/path?q=a&x=1) https://example.org/test.")
        val links = content.filterIsInstance<MarkdownInline.Link>()
        assertEquals(listOf("https://example.com/path?q=a&x=1", "https://example.org/test"), links.map { it.safeUrl })
        assertEquals("one https://example.org/test.", markdownInlineText(content))
        assertEquals("http://example.com:8080/path", safeMarkdownUrl("http://example.com:8080/path"))
        assertEquals("https://[2001:db8::1]/", safeMarkdownUrl("https://[2001:db8::1]/"))
        listOf("//example.com", "https:/example.com", "https://", "https://user@example.com",
            "https://user:password@example.com", "https://example.com:99999", "https://example.com/\nfile",
            "https://example.com/%0afile", "https://example.com/%1f", "https://example.com/%7F",
            "https://example.com\\evil", "javascript:alert(1)", "data:text/html,test", "content://example",
            "file:///example", "intent://example", "mailto:reader@example.com").forEach {
            assertNull("Unsafe destination: $it", safeMarkdownUrl(it))
        }
    }

    @Test fun byteNodeDepthAndTableBudgetsFallBackToTheEntireOriginalSource() {
        val examples = listOf(
            MarkdownParser(maxSourceBytes = 10) to "é".repeat(6),
            MarkdownParser(maxNodes = 4) to "one **two** and *three* tail",
            MarkdownParser(maxDepth = 3) to "> > > > deep\n\nOriginal tail",
            MarkdownParser(maxTableCells = 3) to "| A | B |\n| --- | --- |\n| C | D |\n\nOriginal tail",
            parser to ("> ".repeat(1000) + "deep\n\nOriginal tail"),
            parser to ("*".repeat(200) + "nested" + "*".repeat(200) + "\n\nOriginal tail"),
            parser to ("paragraph\n\n".repeat(8200) + "Original tail"),
            parser to ("x".repeat(256 * 1024) + "Original tail"),
        )
        examples.forEach { (parser, source) ->
            val result = parser.parse(source)
            assertTrue("Expected fallback for ${source.take(40)}", result.literalFallback)
            assertEquals(source, result.source)
            assertTrue(result.blocks.isEmpty())
        }
    }

    @Test fun oversizedCodeLinesUseFullWrappingLiteralFallbackInsteadOfUnboundedMeasurement() {
        val boundary = "```\n" + "x".repeat(4096) + "\n```"
        assertFalse(parser.parse(boundary).literalFallback)
        val examples = listOf(
            "```\n" + "x".repeat(4097) + "\n```\n\nOriginal tail",
            "    " + "x".repeat(4097) + "\n\nOriginal tail",
            "```\n" + "\t".repeat(1025) + "\n```\n\nOriginal tail",
        )
        examples.forEach { source ->
            val result = parser.parse(source)
            assertTrue(result.literalFallback)
            assertEquals(source, result.source)
            assertTrue(result.blocks.isEmpty())
        }
    }

    @Test fun parsedSnapshotsDoNotExposeMutableLists() {
        val result = parser.parse("**one**\n\n- second")
        assertThrows(UnsupportedOperationException::class.java) { (result.blocks as MutableList).clear() }
        val content = (result.blocks[0] as MarkdownBlock.Paragraph).content
        assertThrows(UnsupportedOperationException::class.java) { (content as MutableList).clear() }
        val items = (result.blocks[1] as MarkdownBlock.Items).items
        assertThrows(UnsupportedOperationException::class.java) { (items as MutableList).clear() }
    }

    @Test fun sourceIdentityAndLiteralSelectionsDoNotChangeTheSuppliedText() {
        val source = "# Title\n\n`**literal**` [path](./example.kt#L4) ![chart](https://example.com/chart.png)"
        val result = parser.parse(source)
        assertEquals(source, result.source)
        val selected = markdownInlineText((result.blocks[1] as MarkdownBlock.Paragraph).content)
        assertEquals("**literal** path (./example.kt#L4) [Image: chart]", selected)
        assertFalse(selected.contains("chart.png"))
    }
}
