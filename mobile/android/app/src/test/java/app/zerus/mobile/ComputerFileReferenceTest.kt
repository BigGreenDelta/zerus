package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class ComputerFileReferenceTest {
    @Test fun unicodeSpreadsheetAndEncodedNamesStayOnTheConversationComputer() {
        val path = "/work/example/documents/Сводная_2026-03_2026-09.xlsx"
        assertEquals(ComputerFileReference(path), ComputerFileReference.parse(path))
        assertEquals("Сводная_2026-03_2026-09.xlsx", ComputerFileReference.parse(path)?.name)
        assertEquals(ComputerFileReference("/work/example/report final.xlsx"),
            ComputerFileReference.parse("file:///work/example/report%20final.xlsx"))
        assertEquals(ComputerFileReference("/work/example/a%20b.xlsx"),
            ComputerFileReference.parse("/work/example/a%2520b.xlsx"))
        assertEquals(ComputerFileReference("/work/example/a+b.xlsx"),
            ComputerFileReference.parse("/work/example/a+b.xlsx"))
    }

    @Test fun relativeAndHomePathsAreLeftForNativeSessionResolution() {
        for (path in listOf("docs/report.xlsx", "./report.xlsx", "../report.xlsx", "~/reports/report.xlsx"))
            assertEquals(ComputerFileReference(path), ComputerFileReference.parse(path))
        assertEquals(ComputerFileReference("src/example.rs", ":12:3"), ComputerFileReference.parse("src/example.rs:12:3"))
        assertEquals(ComputerFileReference("/work/example/a.md", "#L12"), ComputerFileReference.parse("/work/example/a.md#L12"))
    }

    @Test fun webAppAndNetworkDestinationsAreNeverComputerFiles() {
        for (value in listOf("", "#section", "https://example.com/a.xlsx", "http://example.com/a", "//example.com/a",
            "file://example.com/a.xlsx", "content://documents/file", "javascript:alert(1)", "mailto:user@example.com",
            "sandbox:/mnt/data/report.xlsx", "C:\\work\\report.xlsx", "file:///C:/work/report.xlsx?query", "/work/a?query",
            "/work/%00.xlsx", "/work/%0a.xlsx", "/work/%5c.xlsx", "/work/%zz", "/work/a\n.xlsx"))
            assertNull(value, ComputerFileReference.parse(value))
    }
}
