package app.zerus.mobile

import org.junit.Assert.assertEquals
import org.junit.Test

/** The task store's crate notices: one entry per crate, its texts from every file below its folder. */
class ThirdPartyNoticesTest {
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
