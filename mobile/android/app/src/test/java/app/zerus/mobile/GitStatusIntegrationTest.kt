package app.zerus.mobile

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** The list/controller boundary uses the actual catalog admission, not a made-up epoch. */
class GitStatusIntegrationTest {
    private class ListHarness : AutoCloseable {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
        val key = GitStatusTestData.candidate().key.machine
        val connection = Connection(key.connectionId, "Fixture", "https://relay.example.test", "fixture-token")
        val machine = Machine(key.connectionId, key.computerId, "Fixture computer", true,
            machineId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", route = MachineRoute.Direct)
        var catalog = GitStatusCatalog(listOf(connection), listOf(machine),
            mapOf(key.connectionId to setOf(GitStatus.OPERATION)), mapOf(key to setOf(GitStatus.OPERATION)), false, true)
        var now = GitStatusTestData.SECOND
        val requests = mutableListOf<GitStatusRequest>()
        val replies = mutableListOf<CompletableDeferred<JSONObject>>()
        val controller = GitStatusController(scope, { now }, { catalog.access(it) }, start = { request ->
            requests += request
            val response = CompletableDeferred<JSONObject>().also(replies::add)
            withContext(NonCancellable) { response.await() }
        }, receipt = { error("A completed fixture must not poll") }, pause = { CompletableDeferred<Unit>().await() })
        fun complete(index: Int) { replies[index].complete(GitStatusTestData.completed(requests[index])) }
        override fun close() {
            controller.setForeground(false)
            replies.forEach { it.complete(JSONObject().put("state", "failed")) }
            scope.cancel()
        }
    }

    @Test fun anUnknownRouteCannotReadOrKeepCurrentProofWhenAnOnlineCardReturns() = ListHarness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.catalog = h.catalog.copy(machines = listOf(h.machine.copy(route = MachineRoute.Unknown)))
        h.controller.setForeground(true)
        h.controller.watch(candidate, true)
        assertEquals(0, h.requests.size)
        h.catalog = h.catalog.copy(machines = listOf(h.machine))
        h.controller.refresh()
        assertEquals(1, h.requests.size)
        h.complete(0)
        assertTrue(h.controller.badge(candidate)!!.current)
        h.catalog = h.catalog.copy(machines = listOf(h.machine.copy(route = MachineRoute.Unknown)))
        h.controller.refresh()
        assertFalse(h.controller.badge(candidate)!!.current)
        h.now += GitStatusController.CADENCE_NANOS
        h.controller.refresh()
        assertEquals(1, h.requests.size)
    }

    @Test fun storageAndPreviewCannotGrantListReadsAndCapabilitiesMustAgreeOnBothSides() = ListHarness().use { h ->
        h.controller.setForeground(true)
        val candidate = GitStatusTestData.candidate()
        for (blocked in listOf(h.catalog.copy(storageReady = false), h.catalog.copy(demo = true),
            h.catalog.copy(relayOperations = emptyMap()), h.catalog.copy(machineOperations = emptyMap()))) {
            h.catalog = blocked
            val lease = h.controller.watch(candidate, true)!!
            h.controller.refresh()
            assertEquals(0, h.requests.size)
            lease.close()
        }
        h.catalog = h.catalog.copy(machineOperations = mapOf(h.key to setOf(GitStatus.OPERATION)))
        h.controller.watch(candidate, true)
        assertEquals(1, h.requests.size)
        h.complete(0)
        assertTrue(h.controller.badge(candidate)!!.synced)
    }

    @Test fun aLateResultAfterTokenOrNativeRouteChangeCannotPublishIntoTheReplacedCatalog() {
        for (change in listOf("token", "native", "route", "offline")) ListHarness().use { h ->
            val candidate = GitStatusTestData.candidate()
            h.controller.setForeground(true)
            h.controller.watch(candidate, true)
            h.catalog = when (change) {
                "token" -> h.catalog.copy(connections = listOf(h.connection.copy(token = "replacement-fixture-token")))
                "native" -> h.catalog.copy(machines = listOf(h.machine.copy(machineId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")))
                "route" -> h.catalog.copy(machines = listOf(h.machine.copy(route = MachineRoute.Via("cccccccc-cccc-4ccc-8ccc-cccccccccccc"))))
                else -> h.catalog.copy(machines = listOf(h.machine.copy(online = false)))
            }
            h.controller.refresh()
            h.complete(0)
            assertNull(change, h.controller.badge(candidate))
            assertEquals(change, 0, h.controller.counts.inFlight)
        }
    }

    @Test fun viewportReplacementDropsTheOldGenerationEvenWhenTheSameCardReturns() = ListHarness().use { h ->
        val candidate = GitStatusTestData.candidate()
        h.controller.setForeground(true)
        val original = h.controller.watch(candidate, true)!!
        original.close()
        val replacement = h.controller.watch(candidate, true)!!
        h.complete(0)
        assertNull(h.controller.badge(candidate))
        assertEquals("Returning to a card cannot bypass the host/path cadence", 1, h.requests.size)
        h.now += GitStatusController.CADENCE_NANOS
        h.controller.refresh()
        assertEquals(2, h.requests.size)
        h.complete(1)
        assertTrue(h.controller.badge(candidate)!!.current)
        replacement.close()
        assertFalse(h.controller.badge(candidate)!!.current)
    }

    @Test fun losingForegroundKeepsUncooperativeCallsBoundedButNeverAcceptsTheirLateReply() = ListHarness().use { h ->
        val first = GitStatusTestData.candidate()
        val second = GitStatusTestData.candidate("/fixture/second")
        h.controller.setForeground(true)
        h.controller.watch(first, true)
        h.controller.watch(second, true)
        h.controller.setForeground(false)
        h.now += GitStatusController.CADENCE_NANOS
        h.controller.setForeground(true)
        assertEquals(2, h.requests.size)
        assertEquals(2, h.controller.counts.inFlight)
        h.complete(0)
        assertNull(h.controller.badge(first))
        assertEquals(2, h.controller.counts.inFlight)
        assertEquals(3, h.requests.size)
    }
}
