package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.File

class SpeechLocalSetupTest {
    @Test fun canonicalGuidePreservesExactCopyableShellBlocks() {
        val source = File("../../../docs/speech-local-setup.json").readText()
        val raw = JSONObject(source)
        val guide = SpeechSetupGuide.parse(source)
        assertEquals(raw.getString("title"), guide.title)
        val commands = raw.getJSONArray("steps").objects().flatMap { step ->
            val values = step.optJSONArray("commands") ?: JSONArray()
            (0 until values.length()).map { values.getString(it) }
        }
        assertTrue(commands.isNotEmpty())
        assertEquals(commands, guide.steps.flatMap { it.commands })
        assertTrue(commands.any { '\n' in it })
        assertThrows(IllegalArgumentException::class.java) {
            SpeechSetupGuide.parse(JSONObject(source).put("schema", 2).toString())
        }
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
