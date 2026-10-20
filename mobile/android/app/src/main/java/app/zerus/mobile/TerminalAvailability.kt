package app.zerus.mobile

import org.json.JSONObject

/** Missing or malformed discovery is unknown; an actual empty operation array is unsupported. */
object OperationCapabilities {
    fun parse(raw: JSONObject?, requireVersion: Boolean = true): Set<String>? {
        if (raw == null || (requireVersion || raw.has("protocol_version")) && raw.opt("protocol_version") != 1) return null
        val array = raw.optJSONArray("operations") ?: return null
        if (array.length() > 4096) return null
        val result = mutableSetOf<String>()
        for (index in 0 until array.length()) {
            val operation = array.opt(index) as? String ?: return null
            if (operation.isBlank() || operation.length > 128) return null
            result += operation
        }
        return result
    }

    /** A failed explicit discovery must not retain a stale grant or a stale refusal. */
    fun <K> replace(previous: Map<K, Set<String>>, key: K, operations: Set<String>?): Map<K, Set<String>> =
        if (operations == null) previous - key else previous + (key to operations)
}

object TerminalAvailability {
    const val checking = "Checking Terminal availability…"
    const val unknownGateway = "Terminal capability metadata is unavailable from the gateway. Refresh and try again."
    const val unknownComputer = "Terminal capability metadata is unavailable from this computer. Refresh and try again."

    fun nativeReason(target: Target, row: JSONObject?): String = when {
        row?.optBoolean("last_known") == true -> "Refresh this session to check Terminal availability."
        row?.opt("terminal_supported") == false ->
            row.string("terminal_reason").ifBlank { "This native adapter does not provide remote Terminal." }
        row?.has("terminal_supported") == true && row.opt("terminal_supported") !is Boolean ->
            "Terminal support metadata is unavailable for this session. Refresh and try again."
        row?.opt("terminal_supported") == true -> ""
        target.session.substringBefore('/') == "opencode" -> "OpenCode runs through its native server. Use the conversation instead of Terminal."
        else -> ""
    }

    fun reason(target: Target, row: JSONObject?, online: Boolean, gateway: Set<String>?, computer: Set<String>?): String = when {
        target.agentId.isNotBlank() || target.archiveId.isNotBlank() -> "Terminal is available only for the live parent session."
        !online -> "The machine is offline."
        nativeReason(target, row).isNotBlank() -> nativeReason(target, row)
        gateway == null -> unknownGateway
        computer == null -> unknownComputer
        "terminal_snapshot" !in gateway -> "This gateway does not support remote Terminal. Update the gateway."
        "terminal_snapshot" !in computer -> "This computer does not advertise remote Terminal. Refresh or update Zerus on this computer."
        else -> ""
    }
}
