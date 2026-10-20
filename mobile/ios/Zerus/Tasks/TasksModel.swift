import Foundation
import Observation
import ZerusKit
import ZerusReplica

/// The Tasks screen over the phone's replica of a project, and the replica's sync while the app is
/// active, ported from the Tasks section of Android `ZerusViewModel`
/// (docs/superpowers/specs/2026-10-20-task-tracker-ios-design.md). Every store call runs off the
/// main actor through `TaskReplicas`; edits sync the project at once.
@MainActor
@Observable
final class TasksModel {
    @ObservationIgnored unowned let app: AppModel
    @ObservationIgnored private let replicas: TaskReplicas?
    @ObservationIgnored private let taskSync: TaskSync?
    /// Why the phone keeps no task replicas, when its storage could not be prepared.
    let storageError: String

    private(set) var tasksKey: ProjectKey?
    private(set) var tasksChosen: ChosenSession?
    private(set) var taskListing: [TaskInfo] = []
    var taskTab: TaskTab = .todo
    private(set) var tasksOnlyThisSession = false
    /// The screen's next notice.
    var taskNotice: String?
    private(set) var taskBusy = false
    private(set) var doneRows: [DoneRow] = []
    private(set) var doneTotal = 0
    private(set) var doneRecent = 0
    private(set) var doneNext: DoneCursor?
    private(set) var doneExpanded = false
    private(set) var doneLoading = false
    /// A page of Done arrived, so its count is known.
    private(set) var doneLoaded = false
    private(set) var doneError = ""
    /// The tasks In review of each project, for the badge on the Tasks button.
    private(set) var reviewCounts: [ProjectKey: Int] = [:]
    /// TaskSync's held Send nows, as state, so a card follows their release.
    private var heldSends: Set<String> = []

    @ObservationIgnored private var syncTask: Task<Void, Never>?
    /// The exchanges that edits and opening Tasks start; they stop with the app too.
    @ObservationIgnored private var projectSyncs: [UUID: Task<Void, Never>] = [:]
    /// The latest first-page read; an older one's answer is dropped.
    @ObservationIgnored private var doneRequest = 0

    convenience init(app: AppModel) {
        let (replicas, failure) = Self.liveReplicas()
        self.init(app: app, replicas: replicas, storageError: failure)
    }

    private static func liveReplicas() -> (TaskReplicas?, String) {
        do { return (try TaskReplicas.live(), "") }
        catch { return (nil, "The phone could not prepare its task storage: \(TaskSyncError.reason(error))") }
    }

    init(app: AppModel, replicas: TaskReplicas?, storageError: String = "") {
        self.app = app
        self.replicas = replicas
        self.storageError = replicas == nil && storageError.isEmpty ? "The phone keeps no task storage." : storageError
        taskSync = replicas.map { TaskSync(client: app.client, replicas: $0) }
    }

    // MARK: Sync

    /// Exchanges every project whose computers name a digest other than the phone's; then the held Send nows.
    func syncTasks() {
        guard !app.demo, syncTask == nil, let taskSync else { return }
        let original = app.connections
        syncTask = Task { [weak self] in
            await self?.runSync(taskSync, original)
            self?.syncTask = nil
        }
    }

    private func runSync(_ taskSync: TaskSync, _ original: [Connection]) async {
        var digests = app.taskDigests, operations = app.machineOperations, known = app.machines
        if Date().timeIntervalSince(app.taskDigestsAt) > 20 {
            var fresh: [ProjectKey: [String: String]] = [:]
            var freshOperations: [MachineKey: Set<String>] = [:]
            var seen: [Machine] = []
            for connection in original {
                // That connection syncs once it answers again.
                guard let computers = try? await app.client.computers(connection) else { continue }
                if Task.isCancelled { return }
                let catalog = await Task.detached { SessionCatalogParser.parse(connection, computers.objects("computers")) }.value
                fresh.merge(catalog.projects.taskDigests) { $1 }
                freshOperations.merge(catalog.operations) { $1 }
                seen += catalog.machines
            }
            digests = fresh
            operations = freshOperations
            known = seen
        }
        let names = Dictionary(app.projects.map { ($0.key, $0.name) }, uniquingKeysWith: { first, _ in first })
        let plan = TaskSyncPlan.peers(digests, relayOperations: app.relayOperations, machineOperations: operations, machines: known)
        for (key, peers) in plan.sorted(by: { $0.key.key < $1.key.key }) {
            if Task.isCancelled || app.connections != original { return }
            guard let connection = original.first(where: { $0.id == key.connectionId }) else { continue }
            // Failures wait in TaskSync's backoff.
            _ = await taskSync.sync(connection, key, name: names[key] ?? "Project", peers: peers)
            refreshReviewCount(key)
            if tasksKey == key { refreshTasks() }
        }
        // A Send now that may have happened waits for an exchange with its computer.
        for connection in original {
            if Task.isCancelled || app.connections != original { return }
            await taskSync.settleSends(connection) { names[$0] ?? "Project" }
        }
        heldSends = await taskSync.heldTasks()
        refreshTasks()
    }

    /// Nothing syncs in the background: leaving the foreground stops the running exchanges; merges are idempotent.
    func stopTaskSync() {
        syncTask?.cancel()
        projectSyncs.values.forEach { $0.cancel() }
        projectSyncs = [:]
    }

    private func launchProjectSync(_ key: ProjectKey, then: (@MainActor () -> Void)? = nil) {
        let id = UUID()
        projectSyncs[id] = Task { [weak self] in
            await self?.syncProject(key)
            if !Task.isCancelled { then?() }
            self?.projectSyncs[id] = nil
        }
    }

    /// One sync of the project with its computers now; failures wait in TaskSync's backoff.
    private func syncProject(_ key: ProjectKey) async {
        guard !app.demo, let taskSync, let connection = app.connections.first(where: { $0.id == key.connectionId }) else { return }
        let peers = TaskSyncPlan.peers([key: app.taskDigests[key] ?? [:]], relayOperations: app.relayOperations,
                                       machineOperations: app.machineOperations, machines: app.machines)[key] ?? []
        guard !peers.isEmpty else { return }
        _ = await taskSync.sync(connection, key, name: taskProjectName(key), peers: peers)
        if tasksKey == key { refreshTasks() }
    }

    /// Drops the connection's replicas. A merge is idempotent: the running exchanges stop here, as on Android, and the replicas go.
    func purge(_ connectionId: String) async {
        if tasksKey?.connectionId == connectionId { closeTasks() }
        reviewCounts = reviewCounts.filter { $0.key.connectionId != connectionId }
        let running = [syncTask].compactMap { $0 } + Array(projectSyncs.values)
        stopTaskSync()
        for task in running { await task.value }
        await taskSync?.purge(connectionId: connectionId)
    }

    // MARK: The project and its sessions

    func taskProjectName(_ key: ProjectKey) -> String { app.projects.first { $0.key == key }?.name ?? "Project" }
    /// Only catalogued projects have tasks.
    func taskProject(_ session: Session) -> ProjectKey? {
        guard let key = session.projectKey, app.projects.contains(where: { $0.key == key && $0.catalogued }) else { return nil }
        return key
    }
    func projectSessions(_ key: ProjectKey) -> [Session] {
        app.sessions.filter { $0.projectKey == key && $0.target.archiveId.isEmpty && $0.target.agentId.isEmpty }
    }
    func taskChips(_ key: ProjectKey) -> [SessionChip] { TasksPresentation.chips(projectSessions(key), machines: app.machines) }
    /// An operation counts when both the relay and the computer offer it.
    private func taskOperations(_ machine: Machine) -> Set<String> {
        (app.machineOperations[machine.key] ?? []).intersection(app.relayOperations[machine.connectionId] ?? [])
    }
    /// The computers that list the project; every computer of its connection before the catalog names them.
    private func projectMachines(_ key: ProjectKey) -> [Machine] {
        let listed = Set(app.taskDigests[key]?.keys.map { $0 } ?? [])
        return app.machines.filter { $0.connectionId == key.connectionId && (listed.isEmpty || listed.contains($0.id)) }
    }
    private func computerName(_ key: ProjectKey, _ computerId: String) -> String {
        app.machines.first { $0.connectionId == key.connectionId && $0.id == computerId }?.name ?? computerId
    }

    func sendRoute(_ task: TaskInfo) -> RouteState {
        guard let key = tasksKey else { return .blocked("No project") }
        if taskBusy { return .blocked("Another task action is running") }
        if heldSends.contains(task.uuid) { return .blocked("It may have been sent already; its stage arrives with the next sync") }
        return TasksPresentation.route(task, chosen: tasksChosen, machines: app.machines, connectionId: key.connectionId,
                                       operations: taskOperations, sessions: projectSessions(key))
    }

    func cancelRoute(_ task: TaskInfo) -> RouteState {
        guard let key = tasksKey else { return .blocked("No project") }
        if taskBusy { return .blocked("Another task action is running") }
        return TasksPresentation.cancelRoute(task, machines: projectMachines(key), operations: taskOperations)
    }

    // MARK: Opening

    func openTasks(_ session: Session) {
        guard let key = taskProject(session) else { return }
        let machine = app.machines.first { $0.connectionId == session.target.connectionId && $0.id == session.target.computerId }
        showTasks(key, ChosenSession(name: session.target.session, computerId: session.target.computerId, machineId: machine?.machineId ?? ""))
    }

    /// Another session for Tasks only, keeping the tab; one of another project shows that project's tasks.
    func chooseTaskSession(_ key: ProjectKey, _ chosen: ChosenSession) {
        if key != tasksKey { showTasks(key, chosen, selectDefault: false) }
        else {
            tasksChosen = chosen
            if tasksOnlyThisSession { loadDone(more: false) }
        }
    }

    /// Only this session: Done then lists and counts only the tasks sent to it.
    func setOnlyThisSession(_ on: Bool) {
        tasksOnlyThisSession = on
        loadDone(more: false)
    }

    private func showTasks(_ key: ProjectKey, _ chosen: ChosenSession, selectDefault: Bool = true) {
        tasksKey = key
        tasksChosen = chosen
        taskListing = []
        doneRows = []
        doneNext = nil
        doneTotal = 0
        doneRecent = 0
        doneExpanded = false
        doneError = ""
        doneLoaded = false
        taskNotice = nil
        refreshTasks(selectDefault: selectDefault)
        loadDone(more: false)
        launchProjectSync(key)
    }

    func closeTasks() {
        tasksKey = nil
        taskNotice = nil
    }

    /// Done lists from a computer each time it is opened, so tasks accepted since show.
    func selectTaskTab(_ tab: TaskTab) {
        taskTab = tab
        if tab == .done && !doneLoading { loadDone(more: false) }
    }

    // MARK: Listing

    func refreshTasks(selectDefault: Bool = false) {
        guard let key = tasksKey, let replicas else { return }
        let name = taskProjectName(key)
        Task {
            // An import may hold the project for a moment; the listing waits once, then keeps what it shows.
            var failure = ""
            var read: TaskListing?
            for attempt in 0..<2 {
                do {
                    read = try await replicas.run { try replicas.store(key, name: name).list() }
                    break
                } catch {
                    failure = TaskSyncError.reason(error)
                    if attempt == 0 { try? await Task.sleep(for: .milliseconds(500)) }
                }
            }
            guard let listing = read else {
                if tasksKey == key && (selectDefault || taskListing.isEmpty) {
                    taskNotice = failure.isEmpty ? "The tasks could not be read." : failure
                }
                return
            }
            guard tasksKey == key else { return }
            taskListing = listing.tasks
            reviewCounts[key] = listing.tasks.count { $0.stage == .review }
            if selectDefault {
                taskTab = TasksPresentation.defaultTab(TasksPresentation.counts(listing.tasks, chosen: tasksChosen, onlyThisSession: tasksOnlyThisSession))
            }
        }
    }

    func refreshReviewCount(_ key: ProjectKey) {
        guard let replicas else { return }
        let name = taskProjectName(key)
        Task {
            guard let count = try? await replicas.run({ try replicas.store(key, name: name).list().tasks.count { $0.stage == .review } })
            else { return }
            reviewCounts[key] = count
        }
    }

    // MARK: Edits

    /// A local edit, then the project's exchange with its computers, so the edit reaches them.
    private func editTasks(_ change: @escaping @Sendable (TaskStore) throws -> Void) {
        guard let key = tasksKey, let replicas else { return }
        let name = taskProjectName(key)
        Task {
            do { try await replicas.run { try change(replicas.store(key, name: name)) } }
            catch {
                taskNotice = Self.reason(error, "The task could not be changed.")
                return
            }
            refreshTasks()
            launchProjectSync(key) { [weak self] in
                guard let self, self.tasksKey == key, self.taskTab == .done else { return }
                self.loadDone(more: false, keepExpanded: true)
            }
        }
    }

    /// New tasks go to To do, or to Backlog while Backlog is open, pinned to the chosen session.
    func addTask(_ text: String) {
        let reason = TasksPresentation.addReason(tasksChosen)
        guard reason.isEmpty, let chosen = tasksChosen else {
            taskNotice = reason
            return
        }
        let stage: TaskStage = taskTab == .backlog ? .backlog : .todo
        let target = TaskTarget.session(session: SessionRef(machine: chosen.machineId, session: chosen.name))
        editTasks { _ = try $0.add(text: text, stage: stage, target: target, position: .end) }
    }

    /// The task settings' Save: the prompt and the session, whichever changed.
    func saveTask(_ uuid: String, text: String?, target: TaskTarget?) {
        editTasks { store in
            if let text { _ = try store.editText(reference: uuid, text: text) }
            if let target { _ = try store.setTarget(reference: uuid, target: target) }
        }
    }

    func commentTask(_ uuid: String, _ text: String) { editTasks { _ = try $0.comment(reference: uuid, text: text) } }

    /// Only the fields changed here, onto the replica's current settings, so a change that arrived meanwhile stays.
    func saveTaskSettings(original: TaskSettings, edited: TaskSettings) {
        editTasks { store in
            let current = try store.settings()
            _ = try store.setSettings(settings: TaskSettings(
                taskTemplate: edited.taskTemplate != original.taskTemplate ? edited.taskTemplate : current.taskTemplate,
                cancelTemplate: edited.cancelTemplate != original.cancelTemplate ? edited.cancelTemplate : current.cancelTemplate,
                agentsOther: edited.agentsOther != original.agentsOther ? edited.agentsOther : current.agentsOther,
                agentLimit: edited.agentLimit != original.agentLimit ? edited.agentLimit : current.agentLimit))
        }
    }

    func taskSettings() async -> TaskSettings? {
        guard let key = tasksKey, let replicas else { return nil }
        let name = taskProjectName(key)
        do { return try await replicas.run { try replicas.store(key, name: name).settings() } }
        catch {
            taskNotice = Self.reason(error, "The task settings could not be read.")
            return nil
        }
    }

    // MARK: Actions

    /// The task settings' action: their unsaved prompt and session first, then the action.
    func saveThen(_ task: TaskInfo, text: String?, target: TaskTarget?, action: TaskAction) {
        guard text != nil || target != nil else { return taskAction(task, action) }
        guard let key = tasksKey, let replicas else { return }
        let name = taskProjectName(key), uuid = task.uuid
        Task {
            let saved: TaskInfo
            do {
                saved = try await replicas.run {
                    let store = replicas.store(key, name: name)
                    if let text { _ = try store.editText(reference: uuid, text: text) }
                    if let target { _ = try store.setTarget(reference: uuid, target: target) }
                    return try store.get(reference: uuid)
                }
            } catch {
                taskNotice = Self.reason(error, "The task could not be changed.")
                return
            }
            refreshTasks()
            taskAction(saved, action)
        }
    }

    /// A stage action of a card or the task settings; Cancel goes through its dialog.
    func taskAction(_ task: TaskInfo, _ action: TaskAction) {
        let uuid = task.uuid
        switch action {
        case .toBacklog: editTasks { _ = try $0.setStage(reference: uuid, stage: .backlog) }
        case .toTodo, .backToTodo: editTasks { _ = try $0.setStage(reference: uuid, stage: .todo) }
        case .accept: editTasks { _ = try $0.setStage(reference: uuid, stage: .done) }
        case .toTop: editTasks { _ = try $0.moveTask(reference: uuid, position: .top) }
        case .send: sendTask(task)
        case .edit, .cancel: break
        }
    }

    private func sendTask(_ task: TaskInfo) {
        guard let key = tasksKey, let taskSync else { return }
        let route: SendRoute
        switch sendRoute(task) {
        case .blocked(let reason):
            taskNotice = reason
            return
        case .ready(let ready): route = ready
        }
        guard let connection = app.connections.first(where: { $0.id == key.connectionId }) else { return }
        taskBusy = true
        let name = taskProjectName(key), computer = computerName(key, route.computerId)
        Task {
            let outcome = await taskSync.send(connection, key, name: name, computerId: route.computerId, task: task.uuid,
                                              session: route.session, computerName: computer)
            taskNotice = switch outcome {
            case .done: "Sent to \(route.session)"
            case .refused(let reason): reason
            case .uncertain(let reason): "It may have been sent: \(reason)"
            }
            // Held before it stops being busy, so Send now never reads as ready for a task that may have been sent.
            heldSends = await taskSync.heldTasks()
            taskBusy = false
            refreshTasks()
        }
    }

    func cancelTask(_ task: TaskInfo, reason: String) {
        guard let key = tasksKey, let taskSync else { return }
        let route: SendRoute
        switch cancelRoute(task) {
        case .blocked(let blocked):
            taskNotice = blocked
            return
        case .ready(let ready): route = ready
        }
        guard let connection = app.connections.first(where: { $0.id == key.connectionId }) else { return }
        taskBusy = true
        let name = taskProjectName(key), computer = computerName(key, route.computerId)
        Task {
            let outcome = await taskSync.cancel(connection, key, name: name, computerId: route.computerId, task: task.uuid,
                                                reason: reason, computerName: computer)
            taskNotice = switch outcome {
            case .done: "Cancelled; it is In review"
            case .refused(let refusal): refusal
            case .uncertain(let doubt): "It may have been cancelled: \(doubt)"
            }
            taskBusy = false
            refreshTasks()
        }
    }

    // MARK: Done

    /// The computer that lists Done: the chosen session's, or any of the project's that offers it.
    private func doneComputer(_ key: ProjectKey) -> Machine? {
        let able = projectMachines(key).filter { $0.online && !$0.lastKnown && taskOperations($0).contains("task_done") }
        return able.first { $0.id == tasksChosen?.computerId } ?? able.first
    }

    /// The first page, or with `more` the next one; "Show older" also shows the older rows already loaded.
    /// A reload after an edit keeps the older rows shown.
    func loadDone(more: Bool, keepExpanded: Bool = false) {
        guard let key = tasksKey, let taskSync else { return }
        if more && doneLoading { return }
        // The first page may already hold older tasks; showing them needs no read.
        if more && ((!doneExpanded && doneRows.count > doneRecent) || doneNext == nil) {
            doneExpanded = true
            return
        }
        doneExpanded = more || (keepExpanded && doneExpanded)
        var session: SessionRef?
        if tasksOnlyThisSession {
            let reason = TasksPresentation.addReason(tasksChosen)
            guard reason.isEmpty, let chosen = tasksChosen else {
                doneError = reason
                return
            }
            session = SessionRef(machine: chosen.machineId, session: chosen.name)
        }
        guard let machine = doneComputer(key) else {
            doneError = "No computer of this project lists Done tasks now"
            return
        }
        guard let connection = app.connections.first(where: { $0.id == key.connectionId }) else { return }
        doneRequest += 1
        let request = doneRequest, before = more ? doneNext : nil, filter = session
        doneLoading = true
        Task {
            do {
                let page = try await taskSync.donePage(connection, key, computerId: machine.id, before: before, session: filter)
                guard tasksKey == key, request == doneRequest else { return }
                doneRows = more ? doneRows + page.rows : page.rows
                doneNext = page.next
                doneTotal = page.total
                doneRecent = page.recent
                doneError = ""
                doneLoaded = true
            } catch {
                if request == doneRequest { doneError = Self.reason(error, "The Done list could not be read.") }
            }
            if request == doneRequest { doneLoading = false }
        }
    }

    /// Fetches a Done task from that computer into the replica, then hands it over.
    func openDoneTask(_ row: DoneRow, opened: @escaping @MainActor (TaskInfo) -> Void) {
        guard let key = tasksKey, let taskSync, let replicas else { return }
        guard let machine = doneComputer(key) else {
            taskNotice = "No computer of this project is online"
            return
        }
        guard let connection = app.connections.first(where: { $0.id == key.connectionId }) else { return }
        taskBusy = true
        let name = taskProjectName(key), uuid = row.uuid
        Task {
            do {
                try await taskSync.exchangeNow(connection, key, name: name, computerId: machine.id, fetch: [uuid])
                let task = try await replicas.run { try replicas.store(key, name: name).get(reference: uuid) }
                refreshTasks()
                opened(task)
            } catch {
                taskNotice = Self.reason(error, "The task could not be opened.")
            }
            taskBusy = false
        }
    }

    private static func reason(_ error: Error, _ fallback: String) -> String {
        let reason = TaskSyncError.reason(error)
        return reason.isEmpty ? fallback : reason
    }
}
