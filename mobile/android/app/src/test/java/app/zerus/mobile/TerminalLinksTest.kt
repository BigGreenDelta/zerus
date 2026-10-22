package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class TerminalLinksTest {
    @Test fun onlyWellFormedWebLinksOpenInTheBrowser() {
        assertEquals(TerminalLinkTarget.Web("https://example.com/docs?q=1"),TerminalLinkTarget.of("https://example.com/docs?q=1"))
        assertEquals(TerminalLinkTarget.Web("http://example.com:8080/"),TerminalLinkTarget.of("http://example.com:8080/"))
        for (other in listOf("https://user@example.com/","https:example.com","javascript:alert(1)","mailto:a@example.com",
                "intent://scan/#Intent;scheme=zxing;end","ssh://example.com","https://example.com/%0d")) {
            assertEquals(other,TerminalLinkTarget.Other(other),TerminalLinkTarget.of(other))
        }
    }
    @Test fun fileLinksKeepTheirHostAndReadThePathOnTheSessionComputer() {
        assertEquals(TerminalLinkTarget.File("file://archlinux/srv/a%20b.txt","archlinux",ComputerFileReference("/srv/a b.txt")),
            TerminalLinkTarget.of("file://archlinux/srv/a%20b.txt"))
        assertEquals(TerminalLinkTarget.File("file:///tmp/x.kt#L12","",ComputerFileReference("/tmp/x.kt","#L12")),
            TerminalLinkTarget.of("file:///tmp/x.kt#L12"))
        assertEquals(ComputerFileReference("/src/main.rs",":42"),(TerminalLinkTarget.of("file://localhost/src/main.rs:42") as TerminalLinkTarget.File).reference)
        for (other in listOf("file://host/a?b","file://user@host/a","file://host:22/a","file:relative","file://host//double","file://host/a%5Cb")) {
            assertEquals(other,TerminalLinkTarget.Other(other),TerminalLinkTarget.of(other))
        }
    }
}
