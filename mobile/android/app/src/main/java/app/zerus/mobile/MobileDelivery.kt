package app.zerus.mobile

import org.json.JSONObject

/** The swarm-wide phone delivery choice. The gateway computer's hgs stores it; the phone requests changes. */
object MobileDelivery {
    const val OPERATION = "set_mobile_delivery"
    const val UNCONFIRMED = "The change was not confirmed. Check its original receipt in Drafts; it has not been repeated."
    val values = listOf("immediate", "away", "away_5", "away_10", "away_15", "away_30")
    private const val CONFIRMATION_MS = 60_000L

    fun label(value: String) = when (value) {
        "immediate" -> "Immediately, even if I'm at a computer"
        "away" -> "As soon as I'm away"
        "away_5" -> "After 5 minutes away"
        "away_10" -> "After 10 minutes away"
        "away_15" -> "After 15 minutes away"
        "away_30" -> "After 30 minutes away"
        else -> ""
    }
    /** Unknown or missing values stay unreported instead of pretending to be the default. */
    fun parse(snapshot: JSONObject?): String =
        (snapshot?.optJSONObject("preferences")?.opt("mobile_delivery") as? String)?.takeIf { it in values }.orEmpty()
    fun arguments(value: String): JSONObject {
        require(value in values) { "Choose a supported notification timing." }
        return JSONObject().put("value", value)
    }
    fun requested(action: SessionAction) = runCatching { JSONObject(action.arguments).string("value") }.getOrDefault("")
    /** Only gateway connectors advertise the operation; an older one still shows its value read-only. */
    fun gateway(machines: List<Machine>, supported: (Machine) -> Boolean): Machine? =
        machines.filter(supported).sortedWith(compareBy<Machine>({ !it.online || it.lastKnown }, { it.route != MachineRoute.Direct }, { it.id })).firstOrNull()
            ?: machines.filter { it.route == MachineRoute.Direct && it.mobileDelivery.isNotBlank() }.minByOrNull { it.id }
    fun reason(gateway: Machine?, gatewaySupports: Boolean, relaySupports: Boolean, pending: Boolean): String = when {
        gateway == null -> "No gateway computer reports this setting yet."
        !gatewaySupports -> "Update Zerus on ${gateway.name} to change this from the phone."
        !relaySupports -> "Update the relay to change this from the phone."
        !gateway.online || gateway.lastKnown -> "${gateway.name} is offline. Try again when it is connected."
        pending -> "Check the previous change before choosing again."
        else -> ""
    }
    /** Called after the generic identity and failure checks of SessionActionPolicies.result. */
    fun result(action: SessionAction, receipt: JSONObject): SessionAction {
        val native = receipt.optJSONObject("result")
        if (receipt.string("state") != "completed" || native == null || native.opt("ok") != true ||
            native.opt("mobile_delivery") != requested(action)) return action.copy(status = "uncertain", error = UNCONFIRMED)
        return action.copy(status = "completed", error = "")
    }
    fun shown(reported: String, action: SessionAction?, now: Long): String {
        val requested = action?.let(::requested).orEmpty()
        return when {
            action == null || requested.isBlank() -> reported
            action.blocksSending -> requested
            action.status == "completed" && now - action.createdAt in 0 until CONFIRMATION_MS -> requested
            else -> reported
        }
    }
    fun status(gateway: String, reported: String, action: SessionAction?, reason: String, now: Long): String {
        val recent = action != null && now - action.createdAt in 0 until CONFIRMATION_MS
        return when {
            action?.status == "sending" -> "Saving on $gateway…"
            action?.status == "uncertain" -> UNCONFIRMED
            action?.status == "failed" && recent -> action.error.ifBlank { "Not changed. Try again." }
            reason.isNotBlank() -> reason
            reported.isBlank() -> "Not reported by $gateway yet."
            action?.status == "completed" && recent && requested(action) != reported -> "Saved on $gateway. The computers report it with their next update."
            else -> "Shared by your computers through $gateway."
        }
    }
}
