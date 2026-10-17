package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class DefaultVoiceRoutesTest {
    private val native = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
    private val connections = listOf(Connection("workspace", "Fixture", "https://relay.example.test", "fixture-token"))
    private val machine = Machine("workspace", "relay-computer", "Fixture computer", true, machineId = native)

    @Test fun globalDefaultsResolveNativeOrLegacyIdsThroughTheExactWorkspace() {
        val expected = SpeechConfigHost(MachineKey("workspace", machine.id), native, machine.name)
        assertEquals(expected, DefaultVoiceRoutes.resolve(MachineKey("workspace", native), listOf(machine), connections))
        assertEquals(expected, DefaultVoiceRoutes.resolve(MachineKey("workspace", machine.id), listOf(machine), connections))
        assertNull(DefaultVoiceRoutes.resolve(MachineKey("another-workspace", native), listOf(machine), connections))
    }
    @Test fun offlineStaleAmbiguousAndUnverifiedMachinesCannotSupplyProfiles() {
        val selected = MachineKey("workspace", native)
        for (rows in listOf(listOf(machine.copy(online = false)), listOf(machine.copy(lastKnown = true)),
                listOf(machine.copy(machineId = "")), listOf(machine, machine.copy(id = "another-relay-id")))) {
            assertNull(DefaultVoiceRoutes.resolve(selected, rows, connections))
        }
        assertNull(DefaultVoiceRoutes.resolve(selected, listOf(machine), emptyList()))
        assertNull(DefaultVoiceRoutes.resolve(null, listOf(machine), connections))
    }
    @Test fun globalComputerPickerIsBoundedAndCanFindAComputerBeyondItsFirstPage() {
        val rows = (0..150).map { index -> machine.copy(id = "route-$index", name = "Fixture $index",
            machineId = "00000000-0000-4000-8000-%012d".format(index + 1)) }
        assertEquals(100, DefaultVoiceRoutes.hosts(rows, connections).size)
        assertEquals("Fixture 150", DefaultVoiceRoutes.hosts(rows, connections, "150").single().name)
        assertEquals("Fixture 150", DefaultVoiceRoutes.resolve(MachineKey("workspace", rows.last().machineId), rows, connections)?.name)
        assertTrue(DefaultVoiceRoutes.hosts(rows.map { it.copy(online = false) }, connections).isEmpty())
    }
}
