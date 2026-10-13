package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class SpeechProviderHelpTest {
    @Test fun documentationDispatchUsesOnlyKnownProviderLinks() {
        val base = "https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"
        val opened = mutableListOf<String>()
        for (adapter in listOf(null, "gigaam", "openai_http", "https://private.example.test/?key=secret")) {
            assertEquals("", openSpeechDocumentation(adapter) { opened.add(it) })
        }
        assertEquals(listOf(base, "$base#local-gigaam", "$base#openai-compatible-server", base), opened)
        assertNotEquals(speechDocumentation("gigaam").label, speechDocumentation("openai_http").label)
    }

    @Test fun browserFailureIsReportedWithoutAnAutomaticRetry() {
        var attempts = 0
        val error = openSpeechDocumentation("openai_http") { attempts++; throw IllegalStateException("private platform detail") }
        assertEquals(1, attempts)
        assertTrue(error.isNotBlank())
        assertFalse(error.contains("private platform detail"))
    }

    @Test fun serviceReadinessCannotBorrowAnotherComputersPositiveResult() {
        val host = SpeechConfigHost(MachineKey("workspace", "relay"), "b4075bf0-9c22-4a67-b768-55af2d2936a4", "Fixture GPU")
        val config = SpeechConfiguration(host.machineId, 1, true, "", emptyList(), emptyList(), null)
        val unknown = speechServiceReadiness(host, null, false)
        assertEquals(unknown, speechServiceReadiness(host, config.copy(machineId = "another-computer"), false))
        assertNotEquals(unknown, speechServiceReadiness(host, config, false))
        assertNotEquals(speechServiceReadiness(host, config, false), speechServiceReadiness(host, config, true))
        val unavailable = config.copy(serviceAvailable = false, serviceReason = "Fixture service stopped")
        assertTrue(speechServiceReadiness(host, unavailable, false).contains(unavailable.serviceReason))
    }
}
