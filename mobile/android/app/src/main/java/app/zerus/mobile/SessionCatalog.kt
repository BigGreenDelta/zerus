package app.zerus.mobile

import org.json.JSONObject

/** Prepared on a worker before publishing one computer inventory to the UI. */
data class SessionCatalog(val machines: List<Machine>, val accounts: List<AccountCatalog>,
    val projects: ProjectCatalog, val operations: Map<MachineKey, Set<String>>, val features: Map<MachineKey, Set<String>>)

object SessionCatalogParser {
    fun parse(connection: Connection, computers: List<JSONObject>, retained: List<Session> = emptyList(), actions: List<SessionAction> = emptyList()): SessionCatalog {
        val operations = mutableMapOf<MachineKey, Set<String>>()
        val features = mutableMapOf<MachineKey, Set<String>>()
        val machines = computers.map { raw ->
            val key = MachineKey(connection.id, raw.getString("id"))
            val capabilities = raw.optJSONObject("snapshot")?.optJSONObject("mobile_capabilities")
            fun values(name: String) = capabilities?.optJSONArray(name)?.let { array ->
                (0 until array.length()).map { array.optString(it) }.toSet()
            }.orEmpty()
            OperationCapabilities.parse(capabilities)?.let { operations[key] = it }
            features[key] = values("features")
            MachineCatalog.parse(connection.id, raw)
        }
        val known=SessionNotificationPreferences.known(retained,actions)
        val sessions = computers.flatMap { NativeParser.sessions(connection, it) }.map { it.copy(raw=SessionNotificationPreferences.merge(it.raw,it.target,known)) }
        return SessionCatalog(machines, computers.map { AccountSnapshots.parse(connection.id, it) },
            ProjectParser.parse(connection, computers, sessions), operations, features)
    }
}
