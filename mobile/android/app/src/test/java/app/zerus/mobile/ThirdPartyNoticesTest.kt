package app.zerus.mobile

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ThirdPartyNoticesTest {
    private val app = generateSequence(File("").absoluteFile) { it.parentFile }
        .map { File(it, "mobile/android/app") }.first { File(it, "build.gradle.kts").isFile }
    private val licenses = File(app, "src/main/assets/licenses")
    private val rows = File(licenses, "libraries.tsv").readLines().filter { it.isNotBlank() && !it.startsWith("#") }
        .map { it.split('\t') }

    @Test fun declaredLibrariesAndPlatformsHaveGeneratedNotices() {
        val listed = rows.associate { fields -> fields[0].substringBeforeLast(':') to fields[0].substringAfterLast(':') }
        // Every library the build declares, Firebase ones included, with the exact version it names.
        val declared = Regex("""(?:implementation|platform)\("([\w.-]+):([\w.-]+)(?::([\w.-]+))?"\)""")
            .findAll(File(app, "build.gradle.kts").readText()).toList()
        assertTrue(declared.size > 20)
        declared.forEach { match ->
            val (group, artifact, version) = match.destructured
            val name = "$group:$artifact"
            assertTrue("$name is missing from libraries.tsv; run scripts/android-notices.py", name in listed)
            if (version.isNotEmpty()) assertEquals("Regenerate the notices for $name", version, listed[name])
        }
    }

    @Test fun everyLicenseHasAReadableNameAndEveryTextShips() {
        val notices = libraryNotices(File(licenses, "libraries.tsv").readText(), emptyList(), emptyList())
        assertEquals(setOf("Apache-2.0", "Apache-2.0 AND BSD-3-Clause", "BSD-2-Clause", "BSD-3-Clause", "LicenseRef-Android-SDK", "MIT"),
            notices.byLicense.map { it.first }.toSet())
        notices.byLicense.forEach { (license, names) ->
            assertFalse(license, licenseName(license).contains("Ref"))
            assertTrue(license, names.isNotEmpty())
        }
        assertTrue(File(licenses, "Apache-2.0.txt").readText().contains("Version 2.0, January 2004"))
        // Licenses other than Apache 2.0 and Google's SDK terms carry their copyright holders' text.
        rows.filter { it[1] != "platform" && it[1] != "Apache-2.0" && it[1] != "LicenseRef-Android-SDK" }.forEach {
            assertEquals(it[0], 3, it.size)
            assertTrue(it[2], File(licenses, it[2]).readText().contains("Copyright"))
        }
        assertEquals(listOf("checker-qual.txt", "commonmark-java.txt", "libyuv.txt", "autolink-java.txt", "protobuf.txt").sorted(),
            notices.texts.keys.sorted())
    }

    @Test fun noticeSectionsKeepEveryNameAndText() {
        val google = File(licenses, "google-notices.txt").readText()
        val sections = noticeSections(google)
        assertEquals(Regex("(?m)^### ").findAll(google).count(), sections.size)
        assertTrue(sections.all { (names, text) -> names.isNotBlank() && text.isNotBlank() && !text.startsWith("###") })
        assertTrue(sections.any { (names, _) -> "Kotlin" in names })
        val library = noticeSections(File(licenses, "library-notices.txt").readText())
        assertTrue(library.any { (names, text) -> "okhttp" in names && "Mozilla Public License" in text })
        assertEquals(listOf("a" to "first\nline", "b, c" to "second"), noticeSections("### a\n\nfirst\nline\n\n### b, c\n\nsecond\n\n"))
    }

    @Test
    fun eachCrateGetsItsTitleAndAllOfItsTexts() {
        val inventory = """{"targets": ["aarch64-linux-android"], "packages": [
            {"name": "loro-internal", "version": "1.16.2", "license": "MIT"},
            {"name": "spin", "version": "0.9.9", "license": "MIT"}]}"""
        val files = mapOf(
            "rust-licenses/loro-internal-1.16.2" to listOf("rust-licenses/loro-internal-1.16.2/LICENSE",
                "rust-licenses/loro-internal-1.16.2/src/diff/LICENSE"),
            "rust-licenses/spin-0.9.9" to listOf("rust-licenses/spin-0.9.9/LICENSE"))
        val notices = crateNotices(inventory, { files.getValue(it) }, { "text of $it" })
        assertEquals(listOf("loro-internal 1.16.2 (MIT)", "spin 0.9.9 (MIT)"), notices.map { it.title })
        assertEquals("text of rust-licenses/loro-internal-1.16.2/LICENSE\n\ntext of rust-licenses/loro-internal-1.16.2/src/diff/LICENSE",
            notices[0].text)
    }
}
