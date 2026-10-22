import Foundation
import Observation
import ZerusKit

/// The New session form's native reads and requests, ported from Android `ZerusViewModel`.
/// Folder creation, worktree creation and launch are each saved before transport and settled
/// only from their original receipts; an unknown result is never retried.
@MainActor
@Observable
final class LaunchModel {
    @ObservationIgnored unowned let app: AppModel

    var machine: MachineKey?
    var visible = false
    var catalog: JSONObject?
    var directory: JSONObject?
    var worktrees: JSONObject?
    var worktreeLoading = false
    var worktreeSubmitting = false
    var directoryCreateSubmitting = false
    var createdDirectory = ""
    var catalogLoading = false
    var directoryLoading = false
    var error = ""
    var submitting = false
    /// Identifies one opened form; its view state is keyed by it.
    var formId = 0
    /// Generated once per form: the default session name and Auto's new branch.
    var generatedName = ""
    /// Launch, worktree and folder receipt statuses when the form opened; later changes belong to this form.
    var baseline: [String: String] = [:]
    /// Auto worktree progress: creating, checking or launching.
    var stage = ""
    var createdWorktree = ""
    var createdEvidence: JSONObject?
    var automaticFailed = false
    private(set) var catalogEvidence = ""

    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var catalogTask: Task<Void, Never>?
    @ObservationIgnored private var directoryTask: Task<Void, Never>?
    @ObservationIgnored private var worktreeTask: Task<Void, Never>?

    init(app: AppModel) { self.app = app }

    var busy: Bool { submitting || worktreeSubmitting || directoryCreateSubmitting || !stage.isEmpty }
    private var conversation: ConversationModel { app.conversation }
    private func computerTarget(_ machine: MachineKey) -> Target {
        Target(computerId: machine.computerId, session: "", run: "", conversation: "", connectionId: machine.connectionId)
    }
    private func supports(_ machine: MachineKey, _ operation: String) -> Bool {
        app.relayOperations[machine.connectionId]?.contains(operation) == true && app.machineOperations[machine]?.contains(operation) == true
    }

    func reason(_ machine: MachineKey) -> String {
        if !app.storageReady || app.demo || !app.connections.contains(where: { $0.id == machine.connectionId }) {
            return "Connect to this workspace before creating a session."
        }
        if !app.machines.contains(where: { $0.key == machine && $0.online }) { return "Refresh the computer list; this computer is offline or last known." }
        if ["catalog", "dirs", "launch"].contains(where: { !supports(machine, $0) }) { return "This computer does not support safe mobile session creation." }
        if conversation.sessionActions.contains(where: { $0.operation == "launch" && $0.target.machineKey == machine && $0.blocksSending }) {
            return "Check the previous launch receipt before creating another session."
        }
        return ""
    }

    func prepare(_ machine: MachineKey) {
        guard !busy else { return }
        let problem = reason(machine)
        guard problem.isEmpty else {
            app.error = problem
            return
        }
        generation += 1
        self.machine = machine
        visible = true
        catalog = nil
        directory = nil
        worktrees = nil
        error = ""
        catalogEvidence = ""
        formId = generation
        generatedName = LaunchPresentation.generatedName()
        stage = ""
        createdWorktree = ""
        createdEvidence = nil
        automaticFailed = false
        createdDirectory = ""
        baseline = Dictionary(conversation.sessionActions.filter { ["launch", "worktree_create", "directory_create"].contains($0.operation) }.map { ($0.requestId, $0.status) }) { a, _ in a }
        catalogTask?.cancel()
        directoryTask?.cancel()
        worktreeTask?.cancel()
        worktreeLoading = false
        directoryLoading = false
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let form = generation
        catalogLoading = true
        catalogTask = Task {
            defer { if generation == form { catalogLoading = false } }
            do {
                let id = UUID().uuidString.lowercased()
                let receipt = try await app.client.waitForReceipt(connection, initial: try await app.client.submit(connection, target: computerTarget(machine), operation: "catalog",
                                                                                                                  payload: ["request_id": .string(id)], id: id))
                guard receipt.string("state") == "completed", let raw = receipt.object("result") else {
                    throw ZerusError(.unavailable, receipt.string("error").nonEmpty ?? "The native account catalog is unavailable.")
                }
                let sanitized = await Task.detached { LaunchPresentation.sanitizeCatalog(raw) }.value
                if generation == form && self.machine == machine {
                    catalog = sanitized
                    catalogEvidence = JSON.object(sanitized).compact
                }
            } catch is CancellationError {
            } catch {
                if generation == form { self.error = AppModel.message(error, fallback: "The native account catalog is unavailable.") }
            }
        }
        browse(machine, "~")
    }

    /// Closing never interrupts worktree creation or a launch; their receipts must settle first.
    func close() {
        guard !busy else { return }
        finish()
    }

    private func finish() {
        worktreeTask?.cancel()
        worktreeLoading = false
        visible = false
        generation += 1
        catalogTask?.cancel()
        directoryTask?.cancel()
        catalogLoading = false
        directoryLoading = false
    }

    private func readDirectory(_ connection: Connection, _ machine: MachineKey, _ path: String) async throws -> JSONObject {
        let id = UUID().uuidString.lowercased()
        let receipt = try await app.client.waitForReceipt(connection, initial: try await app.client.submit(connection, target: computerTarget(machine), operation: "dirs",
                                                                                                          payload: ["request_id": .string(id), "path": .string(path)], id: id))
        guard receipt.string("state") == "completed", var raw = receipt.object("result") else {
            throw ZerusError(.unavailable, receipt.string("error").nonEmpty ?? "This folder is unavailable.")
        }
        guard raw.string("path").hasPrefix("/") else { throw ZerusError.mismatch("Computer returned an invalid folder.") }
        raw["requested_path"] = .string(path)
        return raw
    }

    private func readWorktrees(_ connection: Connection, _ machine: MachineKey, _ path: String, requested: String) async throws -> JSONObject {
        let id = UUID().uuidString.lowercased()
        let receipt = try await app.client.waitForReceipt(connection, initial: try await app.client.submit(connection, target: computerTarget(machine), operation: "worktrees",
                                                                                                          payload: ["request_id": .string(id), "path": .string(path)], id: id))
        guard receipt.string("state") == "completed", var raw = receipt.object("result") else {
            throw ZerusError(.unavailable, receipt.string("error").nonEmpty ?? "Worktree catalog unavailable.")
        }
        raw["requested_path"] = .string(requested)
        return raw
    }

    func browse(_ machine: MachineKey, _ path: String) {
        guard self.machine == machine, !directoryLoading, visible, stage.isEmpty, !directoryCreateSubmitting else { return }
        if path != "~" && !LaunchPresentation.absoluteFolder(path) {
            error = "Choose an absolute native folder."
            return
        }
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        worktreeTask?.cancel()
        worktrees = nil
        worktreeLoading = false
        directory = nil
        let form = generation
        directoryLoading = true
        error = ""
        directoryTask = Task {
            defer { if generation == form { directoryLoading = false } }
            do {
                let raw = try await readDirectory(connection, machine, path)
                if generation == form && self.machine == machine {
                    directory = raw
                    loadWorktrees(machine, raw.string("path"), requested: path)
                }
            } catch is CancellationError {
            } catch {
                if generation == form { self.error = AppModel.message(error, fallback: "This folder is unavailable.") }
            }
        }
    }

    private func loadWorktrees(_ machine: MachineKey, _ path: String, requested: String) {
        worktreeTask?.cancel()
        worktrees = nil
        worktreeLoading = false
        guard supports(machine, "worktrees"), let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let form = generation
        worktreeLoading = true
        worktreeTask = Task {
            defer { if generation == form && self.machine == machine { worktreeLoading = false } }
            if let raw = try? await readWorktrees(connection, machine, path, requested: requested), generation == form, self.machine == machine { worktrees = raw }
        }
    }

    // MARK: Folders

    func directoryCreateReason(_ machine: MachineKey) -> String {
        if let problem = reason(machine).nonEmpty { return problem }
        if !supports(machine, "directory_create") { return "Update the gateway and Zerus on this computer to create folders." }
        if DirectoryCreation.pending(conversation.sessionActions, machine) != nil { return "Check the previous folder creation receipt before creating another." }
        return ""
    }

    private func publishCreatedDirectory(_ machine: MachineKey, _ form: Int, _ action: SessionAction) {
        guard generation == form, self.machine == machine, visible, DirectoryCreation.computerTarget(action, machine), action.status == "completed",
              !action.resultPath.isEmpty else { return }
        createdDirectory = action.resultPath
        // The result proves creation, but browsing still verifies the selectable native folder.
        browse(machine, action.resultPath)
    }

    func createDirectory(_ machine: MachineKey, _ path: String) {
        guard !busy, !directoryLoading, visible, self.machine == machine else { return }
        if let problem = directoryCreateReason(machine).nonEmpty ?? DirectoryCreation.pathError(path).nonEmpty {
            error = problem
            return
        }
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }),
              let arguments = try? SessionActionPolicies.arguments(DirectoryCreation.operation, ["path": .string(path)]) else { return }
        let form = generation
        let action = SessionAction(requestId: UUID().uuidString.lowercased(), target: computerTarget(machine), operation: DirectoryCreation.operation, arguments: arguments)
        directoryCreateSubmitting = true
        error = ""
        conversation.actionFlights.insert(action.requestId)
        Task {
            var attempted = false, rejected = false
            defer {
                directoryCreateSubmitting = false
                conversation.actionFlights.remove(action.requestId)
                if let settled = app.messageState.sessionActions.first(where: { $0.requestId == action.requestId }) { publishCreatedDirectory(machine, form, settled) }
            }
            do {
                _ = try await app.durableMessages({ state in
                    guard DirectoryCreation.pending(state.sessionActions, machine) == nil else { throw ZerusError.invalid("A folder creation is already pending.") }
                    return state.action(action)
                }, acknowledge: { current, written in written.sessionActions.first { $0.requestId == action.requestId }.map(current.action) ?? current })
                guard app.storageReady, !app.demo, visible, generation == form, self.machine == machine, app.connections.contains(connection),
                      app.machines.contains(where: { $0.key == machine && $0.online && !$0.lastKnown }), supports(machine, "directory_create") else {
                    throw ZerusError.invalid("The selected computer or form changed. Folder creation was not sent.")
                }
                var payload = arguments
                payload["request_id"] = .string(action.requestId)
                attempted = true
                let initial: JSONObject
                do { initial = try await app.client.submit(connection, target: action.target, operation: action.operation, payload: payload, id: action.requestId) }
                catch let failure as RelayError {
                    rejected = ConversationModel.rejectedStatuses.contains(failure.status)
                    throw failure
                }
                await conversation.settleSessionAction(action, try await app.client.waitForReceipt(connection, initial: initial))
                if generation == form { error = app.messageState.sessionActions.first { $0.requestId == action.requestId }?.error ?? "" }
            } catch {
                let failed = !attempted || rejected
                let message = failed ? AppModel.message(error, fallback: "The folder was not created.") : DirectoryCreation.uncertain
                await record(action.requestId, status: failed ? "failed" : "uncertain", message: message)
                if generation == form { self.error = message }
            }
        }
    }

    /// Only reads the original durable receipt. An unknown result never submits again.
    func checkDirectoryCreation(_ machine: MachineKey, _ action: SessionAction) {
        guard let stored = conversation.sessionActions.first(where: { $0.requestId == action.requestId && DirectoryCreation.computerTarget($0, machine) }),
              !busy, visible, self.machine == machine, !conversation.actionFlights.contains(stored.requestId),
              let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let form = generation
        directoryCreateSubmitting = true
        conversation.actionFlights.insert(stored.requestId)
        Task {
            defer {
                directoryCreateSubmitting = false
                conversation.actionFlights.remove(stored.requestId)
                if let settled = app.messageState.sessionActions.first(where: { $0.requestId == stored.requestId }) { publishCreatedDirectory(machine, form, settled) }
            }
            do {
                await conversation.settleSessionAction(stored, try await app.client.waitForReceipt(connection, initial: try await app.client.receipt(connection, id: stored.requestId)))
            } catch {
                if generation == form { self.error = "The original folder receipt could not be checked. Nothing has been retried." }
            }
        }
    }

    private func record(_ requestId: String, status: String, message: String) async {
        do {
            _ = try await app.durableMessages { state in
                guard var current = state.sessionActions.first(where: { $0.requestId == requestId }), current.blocksSending else { return state }
                current.status = status
                current.error = message
                return state.action(current)
            }
        } catch {
            self.error = "Could not save the outcome. The original request remains recoverable."
        }
    }

    // MARK: Worktrees

    func worktreeOperationsSupported(_ machine: MachineKey) -> Bool { supports(machine, "worktrees") && supports(machine, "worktree_create") }

    func worktreeReason(_ machine: MachineKey) -> String {
        if let problem = reason(machine).nonEmpty { return problem }
        if !worktreeOperationsSupported(machine) { return "Update Zerus on this computer to use worktree actions." }
        if conversation.sessionActions.contains(where: { $0.operation == "worktree_create" && $0.target.machineKey == machine && $0.blocksSending }) {
            return "Check the previous worktree creation receipt before creating another."
        }
        return ""
    }

    private func worktreeAction(_ machine: MachineKey, _ evidence: JSONObject, branch: String, base: String, destination: String) throws -> SessionAction {
        let arguments = try SessionActionPolicies.arguments("worktree_create", ["path": .string(evidence.string("path")), "common_dir": .string(evidence.string("common_dir")),
                                                                                  "branch": .string(branch), "base": .string(base), "destination": .string(destination)])
        return SessionAction(requestId: UUID().uuidString.lowercased(), target: computerTarget(machine), operation: "worktree_create", arguments: arguments)
    }

    /// Sends one durable creation request and returns its confirmed path, or "" after a recorded failure or unknown result.
    private func submitWorktree(_ machine: MachineKey, _ connection: Connection, _ action: SessionAction, _ evidence: JSONObject, _ form: Int) async -> String {
        var attempted = false, rejected = false
        do {
            _ = try await app.durableMessages({ state in
                guard !state.sessionActions.contains(where: { $0.operation == "worktree_create" && $0.target == action.target && $0.blocksSending }) else {
                    throw ZerusError.invalid("A worktree creation is already pending.")
                }
                return state.action(action)
            }, acknowledge: { current, written in written.sessionActions.first { $0.requestId == action.requestId }.map(current.action) ?? current })
            guard visible, generation == form, self.machine == machine, worktrees?.string("common_dir") == evidence.string("common_dir") else {
                throw ZerusError.invalid("The selected repository changed. Worktree creation was not sent.")
            }
            var payload = action.arguments
            payload["request_id"] = .string(action.requestId)
            attempted = true
            let initial: JSONObject
            do { initial = try await app.client.submit(connection, target: action.target, operation: "worktree_create", payload: payload, id: action.requestId) }
            catch let failure as RelayError {
                rejected = ConversationModel.rejectedStatuses.contains(failure.status)
                throw failure
            }
            await conversation.settleSessionAction(action, try await app.client.waitForReceipt(connection, initial: initial))
            let completed = app.messageState.sessionActions.first { $0.requestId == action.requestId }
            if completed?.status == "completed", let path = completed?.resultPath, !path.isEmpty { return path }
            error = completed?.error.nonEmpty ?? "Worktree creation is unconfirmed. Check its original receipt; nothing has been retried."
            return ""
        } catch {
            let failed = !attempted || rejected
            await record(action.requestId, status: failed ? "failed" : "uncertain",
                         message: failed ? AppModel.message(error, fallback: "The worktree was not created.")
                            : "Worktree creation is unconfirmed. Check the original receipt and folder; nothing has been retried.")
            self.error = failed ? AppModel.message(error, fallback: "The worktree was not created.")
                : "Worktree creation is unconfirmed. Check its original receipt; nothing has been retried."
            return ""
        }
    }

    /// Keeps a confirmed checkout in this form, selected for a direct-folder launch.
    private func retainCreatedWorktree(_ path: String, _ evidence: JSONObject) {
        createdEvidence = WorktreePresentation.withCreated(evidence, path: path)
        createdWorktree = path
    }

    func createWorktree(_ machine: MachineKey, evidence: JSONObject, branch: String, base: String, destination: String) {
        let problem = worktreeReason(machine)
        guard problem.isEmpty, !busy, visible, self.machine == machine, WorktreePresentation.healthy(evidence),
              worktrees?.string("common_dir") == evidence.string("common_dir") else {
            error = problem.nonEmpty ?? "Refresh the current worktrees before creation."
            return
        }
        let action: SessionAction
        do { action = try worktreeAction(machine, evidence, branch: branch, base: base, destination: destination) } catch {
            self.error = AppModel.message(error, fallback: "Invalid worktree.")
            return
        }
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let form = generation
        worktreeSubmitting = true
        conversation.actionFlights.insert(action.requestId)
        error = ""
        Task {
            defer {
                conversation.actionFlights.remove(action.requestId)
                worktreeSubmitting = false
            }
            let created = await submitWorktree(machine, connection, action, evidence, form)
            if !created.isEmpty && visible && generation == form && self.machine == machine {
                retainCreatedWorktree(created, evidence)
                browse(machine, created)
            }
        }
    }

    // MARK: Launch

    /// An OpenCode launch carries an effort only when both the relay and this computer's connector accept it.
    func effortSupported(_ machine: MachineKey) -> Bool {
        LaunchPresentation.effortSupported(relay: app.relayFeatures[machine.connectionId], computer: app.machineFeatures[machine])
    }

    private func launchArguments(_ machine: MachineKey, _ catalog: JSONObject, agent: String, directory: String, tag: String, accountId: String?,
                                 projectId: String, worktrees: JSONObject?, model: String = "", effort: String = "", implicitInbox: Bool = false) throws -> JSONObject {
        try LaunchPresentation.arguments(catalog, agent: agent, directory: directory, tag: tag, accountId: accountId, projectId: projectId,
                                         worktrees: worktrees, model: model, effort: effort, effortSupported: effortSupported(machine), implicitInbox: implicitInbox)
    }

    func launch(_ machine: MachineKey, agent: String, directory: String, tag: String, accountId: String?, expectedCatalog: String, projectId: String,
                worktrees: JSONObject?, model: String = "", effort: String = "", implicitInbox: Bool = false) {
        if let problem = reason(machine).nonEmpty {
            error = problem
            return
        }
        guard !busy, visible, self.machine == machine, !catalogLoading, !directoryLoading, !worktreeLoading, expectedCatalog == catalogEvidence else {
            error = "The creation details changed. Review the current choices."
            return
        }
        guard let catalog else { return }
        guard LaunchPresentation.selectedFolder(self.directory, directory) else {
            error = "Select a folder from the current native browser."
            return
        }
        let args: JSONObject
        do {
            args = try launchArguments(machine, catalog, agent: agent, directory: directory, tag: tag, accountId: accountId, projectId: projectId,
                                       worktrees: worktrees, model: model, effort: effort, implicitInbox: implicitInbox)
        }
        catch {
            self.error = AppModel.message(error, fallback: "Invalid launch choices.")
            return
        }
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let action = SessionAction(requestId: UUID().uuidString.lowercased(), target: computerTarget(machine), operation: "launch", arguments: args)
        let form = generation
        submitting = true
        conversation.actionFlights.insert(action.requestId)
        error = ""
        Task { await submitLaunch(machine, connection, action, agent: agent, directory: directory, expectedCatalog: expectedCatalog, form: form) }
    }

    /// The caller marks the launch in flight; this records, sends and settles it exactly once.
    private func submitLaunch(_ machine: MachineKey, _ connection: Connection, _ action: SessionAction, agent: String, directory: String,
                              expectedCatalog: String, form: Int) async {
        let target = action.target
        var attempted = false, rejected = false
        defer {
            conversation.actionFlights.remove(action.requestId)
            submitting = false
        }
        do {
            _ = try await app.durableMessages({ state in
                guard !state.sessionActions.contains(where: { $0.operation == "launch" && $0.target.machineKey == target.machineKey && $0.blocksSending }) else {
                    throw ZerusError.invalid("A launch is already pending on this computer.")
                }
                return state.action(action)
            }, acknowledge: { current, written in written.sessionActions.first { $0.requestId == action.requestId }.map(current.action) ?? current })
            guard visible, generation == form, self.machine == machine, catalogEvidence == expectedCatalog, app.connections.contains(where: { $0.id == machine.connectionId }) else {
                throw ZerusError.invalid("The launch form changed before creating the session.")
            }
            var payload = action.arguments
            payload["request_id"] = .string(action.requestId)
            attempted = true
            let initial: JSONObject
            do { initial = try await app.client.submit(connection, target: target, operation: "launch", payload: payload, id: action.requestId) }
            catch let failure as RelayError {
                rejected = ConversationModel.rejectedStatuses.contains(failure.status)
                throw failure
            }
            await conversation.settleSessionAction(action, try await app.client.waitForReceipt(connection, initial: initial))
            let completed = app.messageState.sessionActions.first { $0.requestId == action.requestId }
            if let completed, completed.status == "completed", let created = completed.resultTarget, visible, generation == form, self.machine == machine {
                if completed.error.isEmpty {
                    finish()
                    let session = Session(target: created, title: created.session, agent: agent, status: "starting", project: directory, preview: "", pendingCount: 0,
                                          raw: ["name": .string(created.session), "run_id": .string(created.run), "conversation_id": .string(created.conversation)])
                    conversation.open(session)
                    app.conversationRequest = created
                    app.refresh()
                } else {
                    error = completed.error
                    app.refresh()
                }
            } else if completed?.status != "completed" {
                error = completed?.error ?? ""
            }
        } catch {
            let failed = !attempted || rejected
            await record(action.requestId, status: failed ? "failed" : "uncertain",
                         message: failed ? AppModel.message(error, fallback: "The session was not created.")
                            : "Launch is unconfirmed. Check its original receipt; it has not been retried.")
            self.error = failed ? AppModel.message(error, fallback: "The session was not created.")
                : "Launch is unconfirmed. Check its original receipt before creating another session."
        }
    }

    /// Desktop Auto: create the generated sibling worktree, verify its folder and catalog, then launch there.
    /// Every step is pinned to this form; a failed or unknown creation is never replayed and never launches.
    func launchWithWorktree(_ machine: MachineKey, agent: String, folder: String, tag: String, accountId: String?, expectedCatalog: String,
                            projectId: String, model: String = "", effort: String = "", implicitInbox: Bool = false) {
        if let problem = worktreeReason(machine).nonEmpty {
            error = problem
            return
        }
        guard !busy, visible, self.machine == machine, !catalogLoading, !directoryLoading, !worktreeLoading, expectedCatalog == catalogEvidence else {
            error = "The creation details changed. Review the current choices."
            return
        }
        // One generated branch per form: after any creation or failed attempt, only direct launches remain.
        guard !automaticFailed, createdWorktree.isEmpty else {
            error = "This form already attempted worktree creation. Choose None or another worktree."
            return
        }
        guard let catalog else { return }
        guard LaunchPresentation.selectedFolder(directory, folder) else {
            error = "Select a folder from the current native browser."
            return
        }
        guard let evidence = WorktreePresentation.forFolder(worktrees, folder), WorktreePresentation.automaticAvailable(evidence, folder) else {
            error = "The repository could not be confirmed. Choose a working folder again."
            return
        }
        let branch = generatedName
        let destination = WorktreePresentation.automaticPath(evidence, branch: branch)
        let creation: SessionAction
        do {
            // Everything the session needs is checked before anything is created.
            _ = try launchArguments(machine, catalog, agent: agent, directory: destination, tag: tag, accountId: accountId, projectId: projectId,
                                    worktrees: WorktreePresentation.withCreated(evidence, path: destination), model: model, effort: effort, implicitInbox: implicitInbox)
            creation = try worktreeAction(machine, evidence, branch: branch, base: "HEAD", destination: destination)
        } catch {
            self.error = AppModel.message(error, fallback: "Invalid launch choices.")
            return
        }
        guard let connection = app.connections.first(where: { $0.id == machine.connectionId }) else { return }
        let form = generation
        stage = "creating"
        worktreeSubmitting = true
        conversation.actionFlights.insert(creation.requestId)
        error = ""
        func current() -> Bool { visible && generation == form && self.machine == machine }
        Task {
            defer { stage = "" }
            let created = await submitWorktree(machine, connection, creation, evidence, form)
            conversation.actionFlights.remove(creation.requestId)
            worktreeSubmitting = false
            guard !created.isEmpty else {
                automaticFailed = true
                return
            }
            guard current() else { return }
            directoryTask?.cancel()
            worktreeTask?.cancel()
            stage = "checking"
            directory = nil
            worktrees = nil
            retainCreatedWorktree(created, evidence)
            directoryLoading = true
            let checked: JSONObject
            do {
                checked = try await readDirectory(connection, machine, created)
                if generation == form { directoryLoading = false }
            } catch {
                if generation == form { directoryLoading = false }
                if current() {
                    let detail = AppModel.message(error, fallback: "no response").trimmingCharacters(in: CharacterSet(charactersIn: "."))
                    self.error = "Worktree created at \(created). Checking its folder failed: \(detail). Check it again before starting there."
                }
                return
            }
            guard current() else { return }
            directory = checked
            worktreeLoading = true
            let fresh = supports(machine, "worktrees") ? try? await readWorktrees(connection, machine, checked.string("path"), requested: created) : nil
            if generation == form { worktreeLoading = false }
            guard current() else { return }
            worktrees = fresh
            let blocked = reason(machine)
            if !blocked.isEmpty || expectedCatalog != catalogEvidence {
                error = "Worktree created at \(created). \(blocked.nonEmpty ?? "The creation details changed.") Start the session there when ready."
                return
            }
            let project = LaunchPresentation.projects(catalog).first { $0.id == projectId }
            let placement = WorktreePresentation.placement(fresh: fresh, created: createdEvidence, project: project, directory: created)
            let args: JSONObject
            do {
                args = try launchArguments(machine, catalog, agent: agent, directory: created, tag: tag, accountId: accountId, projectId: projectId,
                                           worktrees: placement, model: model, effort: effort, implicitInbox: implicitInbox)
            }
            catch {
                self.error = "Worktree created at \(created). \(AppModel.message(error, fallback: ""))"
                return
            }
            let launch = SessionAction(requestId: UUID().uuidString.lowercased(), target: computerTarget(machine), operation: "launch", arguments: args)
            stage = "launching"
            submitting = true
            conversation.actionFlights.insert(launch.requestId)
            await submitLaunch(machine, connection, launch, agent: agent, directory: created, expectedCatalog: expectedCatalog, form: form)
        }
    }
}
