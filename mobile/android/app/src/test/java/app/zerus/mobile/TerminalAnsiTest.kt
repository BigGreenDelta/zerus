package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class TerminalAnsiTest {
    private val esc = "\u001b"
    @Test fun basicBrightAndResetColorsMatchMuxPod() {
        val screen = TerminalAnsi().parse("${esc}[31mred${esc}[92mgreen${esc}[0mplain")
        assertEquals("redgreenplain",screen.text)
        assertEquals(0xFFCD3131.toInt(),screen.spans[0].style.foreground)
        assertEquals(0xFF23D18B.toInt(),screen.spans[1].style.foreground)
        assertEquals(TerminalStyle(),screen.spans[2].style)
    }
    @Test fun extendedColorsAndIndependentAttributes() {
        val screen = TerminalAnsi().parse("${esc}[1;2;3;4;7;9;38;5;196;48;2;1;2;3ma${esc}[22;23;24;27;29;39;49mb")
        assertEquals(TerminalStyle(0xFFFF0000.toInt(),0xFF010203.toInt(),true,true,true,true,true,true),screen.spans[0].style)
        assertEquals(TerminalStyle(),screen.spans[1].style)
        assertEquals(0xFF080808.toInt(),TerminalAnsi.palette(232))
        assertEquals(0xFFEEEEEE.toInt(),TerminalAnsi.palette(255))
    }
    @Test fun styleContinuesAcrossLinesAndCachedReusePreservesIt() {
        val parser = TerminalAnsi()
        val first = parser.parse("${esc}[32mfirst\nsecond")
        val next = parser.parse("${esc}[31mfirst\nsecond")
        assertEquals(0xFF0DBC79.toInt(),first.spans.last().style.foreground)
        assertEquals(0xFFCD3131.toInt(),next.spans.last().style.foreground)
        assertEquals(next,parser.parse("${esc}[31mfirst\nsecond"))
    }
    @Test fun oscClipboardTitleAndLinksNeverReachVisibleText() {
        val screen = TerminalAnsi().parse("A${esc}]52;c;secret\u0007B${esc}]0;title${esc}\\C${esc}]8;;https://example.invalid\u0007label${esc}]8;;${esc}\\D")
        assertEquals("ABClabelD",screen.text)
        assertFalse(screen.text.contains("secret"))
        // Only the OSC 8 target is kept, as a link range; it never becomes visible text.
        assertEquals(listOf(TerminalLink(3,8,"https://example.invalid")),screen.links)
    }
    @Test fun osc8LinksBecomeRangesWithEitherTerminator() {
        // tmux 3.8 capture-pane -e output, plus a BEL-terminated link.
        val screen = TerminalAnsi().parse("see ${esc}]8;id=a;https://example.com/docs?q=1;x${esc}\\docs link${esc}]8;;${esc}\\ and " +
            "${esc}]8;;file:///tmp/a%20b.txt\u0007a file${esc}]8;;\u0007 end")
        assertEquals("see docs link and a file end",screen.text)
        assertEquals(listOf(TerminalLink(4,13,"https://example.com/docs?q=1;x"),TerminalLink(18,24,"file:///tmp/a%20b.txt")),screen.links)
    }
    @Test fun osc8LinkContinuesAcrossLinesAndCachedLinesKeepIt() {
        val parser = TerminalAnsi()
        val input = "${esc}]8;;https://example.com/${esc}\\ab\ncd${esc}]8;;${esc}\\e\nab"
        val screen = parser.parse(input)
        assertEquals("ab\ncde\nab",screen.text)
        assertEquals(listOf(TerminalLink(0,2,"https://example.com/"),TerminalLink(3,5,"https://example.com/")),screen.links)
        // The last "ab" line has the same text but no open link, so the cache must not reuse the first one.
        assertEquals(screen,parser.parse(input))
    }
    @Test fun invalidOrUnterminatedOsc8TargetsEndLinksInsteadOfOpeningThem() {
        val long = "https://example.com/" + "a".repeat(TerminalAnsi.MAX_LINK_LENGTH)
        for (bad in listOf("https://example.com/a b","https://example.com/\u00e9",long)) {
            val screen = TerminalAnsi().parse("${esc}]8;;https://example.com/${esc}\\x${esc}]8;;$bad${esc}\\y")
            assertEquals("xy",screen.text)
            assertEquals(listOf(TerminalLink(0,1,"https://example.com/")),screen.links)
        }
        assertEquals(emptyList<TerminalLink>(),TerminalAnsi().parse("a${esc}]8;;https://example.com/b").links)
        assertEquals(emptyList<TerminalLink>(),TerminalAnsi().parse("${esc}]8;https://example.com/${esc}\\x").links)
    }
    @Test fun linkCountIsBoundedWithoutHidingText() {
        // Four links per row: 260 rows stay under the row bound and exceed the link bound.
        val input = (0 until 260).joinToString("\n") { row -> (0 until 4).joinToString(" ") { "${esc}]8;;https://example.com/$row/$it${esc}\\$row.$it${esc}]8;;${esc}\\" } }
        val screen = TerminalAnsi().parse(input)
        assertEquals(TerminalAnsi.MAX_LINKS,screen.links.size)
        assertEquals("https://example.com/255/3",screen.links.last().uri)
        assertFalse(screen.truncated)
        assertTrue(screen.text.endsWith("\n259.0 259.1 259.2 259.3"))
    }
    @Test fun c1ControlsAndOtherEscapePayloadsAreDiscarded() {
        assertEquals("abcd",TerminalAnsi().parse("a\u009d52;c;private\u009cb${esc}Pprivate${esc}\\c${esc}[2Jd").text)
        assertEquals("safe",TerminalAnsi().parse("safe${esc}]52;unterminated").text)
    }
    @Test fun unicodeTabsUseCellsWithoutSplittingSurrogates() {
        assertEquals("界      x",TerminalAnsi().parse("界\tx").text)
        assertEquals("e\u0301       x",TerminalAnsi().parse("e\u0301\tx").text)
        assertEquals("😀      x",TerminalAnsi().parse("😀\tx").text)
        assertEquals(0..1,TerminalAnsi.cursorRange("😀x",1,0))
        assertEquals(2..2,TerminalAnsi.cursorRange("😀x",2,0))
        // Blank end-of-line is rendered using the native cell coordinates, not a character range.
        assertNull(TerminalAnsi.cursorRange("x",1,0))
        assertNull(TerminalAnsi.cursorRange("",0,0))
    }
    @Test fun emptyAndMalformedExtendedSequencesDoNotInventColors() {
        assertEquals(TerminalStyle(),TerminalAnsi().parse("${esc}[38;2;999;0;0mx").spans.single().style)
        assertEquals("text",TerminalAnsi().parse("text${esc}[").text)
        assertTrue(TerminalAnsi().parse("x".repeat(140000)).truncated)
        assertTrue(TerminalAnsi().parse("\n".repeat(1200)).truncated)
    }
}
