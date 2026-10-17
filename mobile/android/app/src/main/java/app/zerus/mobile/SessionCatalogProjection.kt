package app.zerus.mobile

/** Captured on main; the full catalog merge runs on Default and publishes once.
 * Archive rows are bounded by ArchivePaging. Never compare or copy the full
 * live catalog on the UI thread, including in snapshotFlow's equality check. */
internal class SessionCatalogProjection(
    private val catalog: List<Session>, private val archives: List<Session>,
    private val native: Map<MachineKey, Set<String>>, private val relay: Map<String, Set<String>>,
) {
    fun matches(rows: List<Session>, pages: List<Session>,
        operations: Map<MachineKey, Set<String>>, gateways: Map<String, Set<String>>) =
        catalog === rows && native === operations && relay === gateways && archives == pages

    fun prepare(): List<Session> {
        val seen = HashSet<String>()
        val result = ArrayList<Session>(catalog.size + archives.size)
        for (row in catalog) {
            if (SessionFilters.archived(row) && "archive_list" in native[SessionFilters.machineKey(row.target)].orEmpty() &&
                "archive_list" in relay[row.target.connectionId].orEmpty()) continue
            if (seen.add(row.target.key)) result.add(row)
        }
        for (row in archives) if (seen.add(row.target.key)) result.add(row)
        return result
    }

    override fun equals(other: Any?) = other is SessionCatalogProjection &&
        matches(other.catalog,other.archives,other.native,other.relay)
    override fun hashCode() = System.identityHashCode(catalog)
}
