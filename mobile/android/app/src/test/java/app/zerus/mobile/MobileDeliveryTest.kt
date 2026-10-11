package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class MobileDeliveryTest {
    private val gatewayId = "11111111-1111-4111-8111-111111111111"
    private val peerId = "33333333-3333-4333-8333-333333333333"
    private fun machine(id: String, route: MachineRoute = MachineRoute.Direct, online: Boolean = true, value: String = "away") =
        Machine("workspace", id, "Desktop", online, route = route, mobileDelivery = value)
    private fun action(value: String, status: String = "sending", createdAt: Long = 1_000) =
        SessionAction("request", Target(gatewayId, "", "", "", "workspace"), MobileDelivery.OPERATION,
            JSONObject().put("value", value).toString(), status = status, createdAt = createdAt)
    private fun receipt(result: JSONObject?, state: String = "completed") =
        JSONObject().put("request_id", "request").put("state", state).put("result", result ?: JSONObject.NULL)

    @Test fun snapshotValuesAreExactAndUnknownValuesStayUnreported() {
        MobileDelivery.values.forEach { assertEquals(it, MobileDelivery.parse(JSONObject().put("preferences", JSONObject().put("mobile_delivery", it)))) }
        listOf(JSONObject(), JSONObject().put("preferences", "away"), JSONObject().put("preferences", JSONObject().put("mobile_delivery", "later")),
            JSONObject().put("preferences", JSONObject().put("mobile_delivery", 5))).forEach { assertEquals("", MobileDelivery.parse(it)) }
        assertEquals("", MobileDelivery.parse(null))
        val raw = JSONObject().put("id", gatewayId).put("name", "Desktop").put("online", true)
            .put("snapshot", JSONObject().put("preferences", JSONObject().put("mobile_delivery", "away_10")))
        assertEquals("away_10", MachineCatalog.parse("workspace", raw).mobileDelivery)
        assertEquals("As soon as I'm away", MobileDelivery.label("away"))
        assertEquals("Immediately, even if I'm at a computer", MobileDelivery.label("immediate"))
    }
    @Test fun argumentsContainOnlyAKnownValue() {
        assertEquals("{\"value\":\"away_5\"}", MobileDelivery.arguments("away_5").toString())
        assertThrows(IllegalArgumentException::class.java) { MobileDelivery.arguments("later") }
    }
    @Test fun onlyTheAdvertisingGatewayCarriesTheChoice() {
        val gateway = machine(gatewayId, value = "away_15")
        val peer = machine(peerId, MachineRoute.Via(gatewayId), value = "immediate")
        assertEquals(gatewayId, MobileDelivery.gateway(listOf(peer, gateway)) { it.id == gatewayId }?.id)
        assertEquals(gatewayId, MobileDelivery.gateway(listOf(machine(peerId, online = false), gateway)) { true }?.id)
        assertEquals(gatewayId, MobileDelivery.gateway(listOf(peer, gateway)) { false }?.id) // read-only fallback
        assertNull(MobileDelivery.gateway(listOf(peer)) { false })
    }
    @Test fun olderGatewaysOrRelaysExplainWhyTheChoiceIsReadOnly() {
        val gateway = machine(gatewayId)
        assertEquals("No gateway computer reports this setting yet.", MobileDelivery.reason(null, false, true, false))
        assertEquals("Update Zerus on Desktop to change this from the phone.", MobileDelivery.reason(gateway, false, true, false))
        assertEquals("Update the relay to change this from the phone.", MobileDelivery.reason(gateway, true, false, false))
        assertEquals("Desktop is offline. Try again when it is connected.", MobileDelivery.reason(machine(gatewayId, online = false), true, true, false))
        assertEquals("Check the previous change before choosing again.", MobileDelivery.reason(gateway, true, true, true))
        assertEquals("", MobileDelivery.reason(gateway, true, true, false))
    }
    @Test fun onlyAnExactNativeAcknowledgementConfirmsTheChange() {
        val pending = action("away_10")
        assertEquals("completed", SessionActionPolicies.result(pending, receipt(JSONObject().put("ok", true).put("mobile_delivery", "away_10"))).status)
        listOf(JSONObject().put("ok", true).put("mobile_delivery", "away"), JSONObject().put("mobile_delivery", "away_10"),
            JSONObject().put("ok", "true").put("mobile_delivery", "away_10"), null).forEach {
            assertEquals("uncertain", SessionActionPolicies.result(pending, receipt(it)).status)
        }
        assertEquals("uncertain", SessionActionPolicies.result(pending, receipt(null, "uncertain")).status)
        assertEquals("failed", SessionActionPolicies.result(pending, receipt(null, "failed").put("error", "gateway offline")).status)
        assertEquals("uncertain", SessionActionPolicies.result(pending, receipt(null).put("request_id", "other")).status)
    }
    @Test fun pendingAndJustConfirmedChoicesAreShownUntilTheSnapshotCatchesUp() {
        assertEquals("away_10", MobileDelivery.shown("away", action("away_10"), 2_000))
        assertEquals("away_10", MobileDelivery.shown("away", action("away_10", "uncertain"), 2_000))
        assertEquals("away_10", MobileDelivery.shown("away", action("away_10", "completed"), 30_000))
        assertEquals("away", MobileDelivery.shown("away", action("away_10", "completed"), 120_000))
        assertEquals("away", MobileDelivery.shown("away", action("away_10", "failed"), 2_000))
        assertEquals("immediate", MobileDelivery.shown("immediate", null, 2_000))
    }
    @Test fun statusExplainsSubmittedConfirmedAndBlockedStates() {
        assertEquals("Saving on Desktop…", MobileDelivery.status("Desktop", "away", action("away_10"), "", 2_000))
        assertEquals(MobileDelivery.UNCONFIRMED, MobileDelivery.status("Desktop", "away", action("away_10", "uncertain"), "", 2_000))
        assertEquals("Shared by your computers through Desktop.", MobileDelivery.status("Desktop", "away_10", action("away_10", "completed"), "", 2_000))
        assertEquals("Saved on Desktop. The computers report it with their next update.", MobileDelivery.status("Desktop", "away", action("away_10", "completed"), "", 2_000))
        assertEquals("Desktop is offline. Try again when it is connected.", MobileDelivery.status("Desktop", "away", null, "Desktop is offline. Try again when it is connected.", 2_000))
        assertEquals("Not reported by Desktop yet.", MobileDelivery.status("Desktop", "", null, "", 2_000))
    }
}
