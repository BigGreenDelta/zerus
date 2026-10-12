package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

sealed interface MachineRoute {
    data object Unknown : MachineRoute
    data object Direct : MachineRoute
    data class Via(val gatewayId: String, val gatewayName: String = "") : MachineRoute
}

data class MachineTreeRow(val machine: Machine, val parent: Machine? = null, val gatewayName: String = "")
data class MachineTreeGroup(val root: MachineTreeRow, val children: List<MachineTreeRow> = emptyList())

/** Selected relay routes describe one hop, not every enrollment of a computer. */
object MachineCatalog {
    private const val maxAliases = 4096
    private val versionFormat = Regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?")

    fun parse(connectionId: String, raw: JSONObject) = Machine(
        connectionId, raw.getString("id"), raw.string("name", "id"), raw.optBoolean("online"),
        serverAliases = aliases(raw.opt("aliases")), route = route(raw),
        hgsVersion = (raw.optJSONObject("snapshot")?.opt("hgs_version") as? String)
            ?.takeIf { it.length <= 128 && versionFormat.matches(it) }.orEmpty(),
        mobileDelivery = MobileDelivery.parse(raw.optJSONObject("snapshot")),
        lastSeenAt = (raw.opt("last_seen_at") as? Number)?.toDouble()?.takeIf { it.isFinite() && it >= 0 },
        machineId = canonicalUuid(raw.opt("machine_id")).orEmpty())

    /** Relay availability only; an offline row is never a usable target. */
    fun status(machine: Machine): String = when {
        machine.lastKnown -> "Last known"
        machine.online -> "Online"
        else -> AccountPresentation.date(machine.lastSeenAt)?.let { "Offline · last seen $it" } ?: "Offline"
    }

    fun gateway(machine: Machine, catalog: List<Machine>): Machine? {
        return GatewayIndex(catalog.filter { it.connectionId == machine.connectionId }).resolve(machine)
    }

    /** Only explicit direct routes can be parents of a one-level presentation group. */
    fun directGateway(machine: Machine, catalog: List<Machine>): Machine? =
        gateway(machine, catalog)?.takeIf { it.route == MachineRoute.Direct }

    fun gatewayName(machine: Machine, catalog: List<Machine>): String =
        gateway(machine, catalog)?.name ?: (machine.route as? MachineRoute.Via)?.gatewayName.orEmpty()

    fun groupsByConnection(catalog: List<Machine>): Map<String, List<MachineTreeGroup>> =
        catalog.groupBy { it.connectionId }.mapValues { (connectionId, machines) -> groups(connectionId, machines) }

    fun tree(connectionId: String, catalog: List<Machine>): List<MachineTreeRow> =
        groups(connectionId, catalog).flatMap { listOf(it.root) + it.children }

    fun groups(connectionId: String, catalog: List<Machine>): List<MachineTreeGroup> {
        val machines = catalog.filter { it.connectionId == connectionId }.distinctBy { it.id }
        val index = GatewayIndex(machines)
        val rows = machines.map { machine ->
            val gateway = index.resolve(machine)
            MachineTreeRow(machine, gateway?.takeIf { it.route == MachineRoute.Direct },
                gateway?.name ?: (machine.route as? MachineRoute.Via)?.gatewayName.orEmpty())
        }
        val children = rows.filter { it.parent != null }.groupBy { it.parent!!.id }
        val roots = rows.filter { it.machine.route == MachineRoute.Direct } +
            rows.filter { it.machine.route != MachineRoute.Direct && it.parent == null }
        return roots.map { MachineTreeGroup(it, children[it.machine.id].orEmpty()) }
    }

    private class GatewayIndex(machines: List<Machine>) {
        private val identities = mutableMapOf<String, Machine?>()
        init {
            for (machine in machines) for (identity in machine.serverAliases + machine.id) {
                if (!identities.containsKey(identity)) identities[identity] = machine
                else if (identities[identity]?.id != machine.id) identities[identity] = null
            }
        }
        fun resolve(machine: Machine): Machine? {
            val via = machine.route as? MachineRoute.Via ?: return null
            return identities[via.gatewayId]?.takeUnless { it.id == machine.id }
        }
    }

    private fun route(raw: JSONObject): MachineRoute {
        if (!raw.has("via")) return MachineRoute.Unknown
        val value = raw.opt("via")
        if (value === JSONObject.NULL) return MachineRoute.Direct
        val via = value as? JSONObject ?: return MachineRoute.Unknown
        val id = canonicalUuid(via.opt("gateway_id")) ?: return MachineRoute.Unknown
        val name = (via.opt("gateway_name") as? String)?.takeIf { caption ->
            caption.codePointCount(0, caption.length) <= 128 && caption.none {
                Character.isISOControl(it) || Character.getType(it) == Character.FORMAT.toInt()
            }
        }.orEmpty()
        return MachineRoute.Via(id, name)
    }

    private fun aliases(value: Any?): Set<String> {
        val values = value as? JSONArray ?: return emptySet()
        if (values.length() > maxAliases) return emptySet()
        val result = mutableSetOf<String>()
        for (index in 0 until values.length()) {
            result += canonicalUuid(values.opt(index)) ?: return emptySet()
        }
        return result
    }

    private fun canonicalUuid(value: Any?): String? {
        val text = value as? String ?: return null
        if (text.length != 36) return null
        return text.takeIf { runCatching { UUID.fromString(it).toString() == it }.getOrDefault(false) }
    }
}
