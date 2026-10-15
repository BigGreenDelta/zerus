package app.zerus.mobile

/** Immutable list projection. Build on Default; scrolling only composes visible cards. */
data class SessionListPresentation(
    val groups: List<SessionGroup> = emptyList(),
    val counts: Map<SessionFilter, Int> = SessionFilter.entries.associateWith { 0 },
    val machineCounts: Map<MachineKey, Int> = emptyMap(),
    val readAllTargets: List<Target> = emptyList(),
    val visibleCount: Int = 0,
) {
    companion object {
        fun prepare(sessions: List<Session>, machines: List<Machine>, projects: List<ProjectSummary>,
            selectedMachines: Set<MachineKey>, query: String, project: ProjectKey?, filter: SessionFilter,
            drafts: List<Draft>): SessionListPresentation {
            val scoped = SessionFilters.scoped(sessions, machines, selectedMachines, query, project)
            val drafted = SessionFilters.draftedSessions(sessions, drafts)
            val online = machines.filter { it.online }.mapTo(hashSetOf()) { MachineKey(it.connectionId, it.id) }
            fun online(row: Session) = SessionFilters.machineKey(row.target) in online
            val visible = scoped.filter { SessionFilters.matches(it, filter, online(it), it.target.key in drafted) }
            return SessionListPresentation(SessionFilters.grouped(visible, projects),
                SessionFilters.counts(scoped, machines, drafted),
                SessionFilters.machineCounts(sessions, machines, filter, drafted, project),
                scoped.filter { ReadAllPolicies.listCandidate(it, online(it)) }.map { it.target }, visible.size)
        }
    }
}
