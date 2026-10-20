import SwiftUI
import ZerusKit

/// Chooses the computer for a new session.
struct NewSessionMachineSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                if app.machines.isEmpty { Text("Refresh your connected machines first.").foregroundStyle(Theme.muted) }
                ForEach(app.machines, id: \.key) { machine in
                    let reason = app.launch.reason(machine.key)
                    Button {
                        dismiss()
                        app.launch.prepare(machine.key)
                    } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            MachineLabel(name: machine.name, colorHex: app.machineColor(machine.key))
                            if app.connections.count > 1 {
                                Text(app.connections.first { $0.id == machine.connectionId }?.displayName ?? "").font(.caption2).foregroundStyle(Theme.muted)
                            }
                            if !reason.isEmpty { Text(reason).font(.footnote).foregroundStyle(Theme.muted) }
                        }
                    }
                    .disabled(!reason.isEmpty)
                }
            }
            .navigationTitle("New session on which machine?")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .zerusScreen()
        }
        .presentationDetents([.medium, .large])
    }
}

/// The full-screen New session form. Edits ask before they are discarded, and nothing can close it
/// while a folder, worktree or session is being created.
struct LaunchForm: View {
    @Environment(AppModel.self) private var app
    let machine: MachineKey

    @State private var provider = ""
    @State private var account = ""
    @State private var selectedModel = ""
    @State private var selectedEffort = ""
    @State private var projectId = ""
    @State private var folder = ""
    @State private var browsedFolder = ""
    @State private var tag = ""
    @State private var autoPreferred = true
    @State private var edited = false
    @State private var appliedWorktree = ""
    @State private var allowedCatalog = ""
    @State private var choosingProject = false
    @State private var choosingWorktree = false
    @State private var creatingWorktree = false
    @State private var browsing = false
    @State private var discarding = false

    private var launch: LaunchModel { app.launch }

    var body: some View {
        let catalog = launch.catalog
        let signature = launch.catalogEvidence
        let providers = LaunchPresentation.agents(catalog)
        let accounts = LaunchPresentation.accounts(catalog, provider)
        let modelOptions = LaunchPresentation.models(catalog, provider: provider, accountId: account)
        let effortOptions = LaunchPresentation.efforts(catalog, provider: provider, accountId: account, model: selectedModel,
                                                       supported: launch.effortSupported(machine))
        let choices = LaunchPresentation.accountChoices(catalog, machine: machine, catalogs: app.accountCatalogs, now: Clock.seconds(),
                                                        selectedProvider: provider, selectedAccount: account)
        let projects = LaunchPresentation.projects(catalog)
        let projectSupported = catalog?["project_launch_supported"] == .bool(true)
        let project = projects.first { $0.id == projectId }
        let computer = app.machineName(machine)
        let busy = launch.busy
        let supported = launch.worktreeOperationsSupported(machine)
        let evidence = WorktreePresentation.forFolder(launch.worktrees, folder)
        let createdEvidence = !folder.isEmpty && folder == launch.createdWorktree ? launch.createdEvidence : nil
        let placement = WorktreePresentation.placement(fresh: evidence, created: createdEvidence, project: project, directory: folder)
        let inProject = project?.folders.contains { $0.path == folder } == true
        let related = project != nil && !inProject && WorktreePresentation.anchor(placement, project!, folder) != nil
        let autoPossible = supported && WorktreePresentation.automaticAvailable(evidence, folder)
        let automatic = autoPreferred && autoPossible
        let branch = launch.generatedName
        let plannedPath = automatic ? WorktreePresentation.automaticPath(evidence, branch: branch) : ""
        let plannedAdded = automatic && project != nil && evidence != nil &&
            WorktreePresentation.anchor(WorktreePresentation.withCreated(evidence!, path: plannedPath), project!, plannedPath) == nil
        let folderValid = LaunchPresentation.selectedFolder(launch.directory, folder)
        let changed = !allowedCatalog.isEmpty && signature != allowedCatalog
        let actions = app.conversation.sessionActions.filter { $0.target.machineKey == machine }
        let inForm: (SessionAction) -> Bool = { launch.baseline[$0.requestId] != $0.status }
        let pendingWorktree = launch.worktreeSubmitting ? nil : actions.last { $0.operation == "worktree_create" && $0.blocksSending }
        let failedWorktree = actions.last { $0.operation == "worktree_create" && $0.status == "failed" && inForm($0) }
        let createdWorktree = actions.last { $0.operation == "worktree_create" && $0.status == "completed" && !$0.resultPath.isEmpty && inForm($0) }
        let pendingLaunch = launch.submitting ? nil : actions.last { $0.operation == "launch" && $0.blocksSending }
        let confirmed = actions.last { $0.operation == "launch" && $0.status == "completed" && $0.resultTarget != nil && inForm($0) }
        let nameError = LaunchPresentation.nameError(tag)
        let readiness = Self.readiness(launch: launch, machine: machine, catalog: catalog, allowedCatalog: allowedCatalog, changed: changed,
                                       projectSupported: projectSupported, project: project, folder: folder, folderValid: folderValid, providers: providers,
                                       provider: provider, accounts: accounts, account: account, nameError: nameError, automatic: automatic,
                                       confirmed: confirmed != nil)
        let reason = LaunchPresentation.disabledReason(readiness)
        let progress = launch.stage == "checking" ? "Checking folder…" : (launch.stage == "launching" || launch.submitting) ? "Creating session…"
            : (launch.stage == "creating" || launch.worktreeSubmitting) ? "Creating worktree…" : ""
        let dirty = confirmed == nil && (edited || tag != launch.generatedName)
        let canChoose = !busy && !launch.worktreeLoading && !WorktreePresentation.entries(evidence).isEmpty
        let canCreate = !busy && !launch.worktreeLoading && autoPossible && launch.worktreeReason(machine).isEmpty

        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    if launch.catalogLoading { ProgressView().progressViewStyle(.linear).tint(Theme.mint) }
                    if let action = pendingWorktree {
                        notice(Theme.amber) {
                            Text("Worktree creation is unconfirmed. Check the original result before creating another.")
                            planned(action)
                            Button("Check original worktree result") { app.conversation.checkSessionAction(action) }.disabled(busy)
                        }
                    }
                    if let action = failedWorktree, pendingWorktree == nil {
                        notice(Theme.amber) {
                            Text("Worktree was not created. \(action.error)".trimmingCharacters(in: .whitespaces))
                            planned(action)
                            Text("Nothing was retried. Inspect this folder before using it.").font(.footnote).foregroundStyle(Theme.muted)
                            let destination = action.arguments.string("destination")
                            if LaunchPresentation.absoluteFolder(destination) && destination != folder {
                                Button("Check this folder") {
                                    autoPreferred = false
                                    select(destination)
                                }
                                .disabled(busy || launch.directoryLoading)
                            }
                        }
                    }
                    if let action = createdWorktree {
                        notice(nil) {
                            Text("Worktree created on \(computer). It stays there if you close this form.")
                            Text(action.resultPath).font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled)
                            if action.resultPath != folder {
                                Button("Use created worktree") {
                                    autoPreferred = false
                                    select(action.resultPath)
                                }
                                .disabled(busy || launch.directoryLoading)
                            }
                        }
                    }
                    if let action = pendingLaunch {
                        notice(Theme.amber) {
                            Text("Launch result unknown. A native session may already exist. This request has not been retried.")
                            Button("Check original launch result") { app.conversation.checkSessionAction(action) }.disabled(busy)
                        }
                    }
                    if let action = confirmed {
                        notice(Theme.mint) {
                            Text("\(computer) confirmed creation of \(action.resultTarget?.session ?? "").")
                            if !action.error.isEmpty && action.error != launch.error { Text(action.error).font(.footnote).foregroundStyle(Theme.amber) }
                            Button("Open confirmed session") {
                                launch.close()
                                app.conversation.openSessionActionResult(action)
                                if let target = action.resultTarget { app.conversationRequest = target }
                            }
                            .disabled(busy)
                        }
                    }
                    if changed {
                        notice(Theme.amber) {
                            Text("Creation choices changed. Review them before creating a session.")
                            Button("Review updated choices") {
                                allowedCatalog = signature
                                provider = ""
                                account = ""
                                projectId = ""
                                folder = ""
                            }
                            .disabled(busy)
                        }
                    }
                    section("Project") {
                        field(project?.name ?? (launch.catalogLoading ? "Loading projects…" : projects.isEmpty ? "No projects available" : "Choose project"),
                              supporting: project.map(LaunchPresentation.folderSummary), enabled: !busy && projectSupported && !projects.isEmpty,
                              icon: Image(systemName: "folder.fill"), tint: project.flatMap { Color(projectHex: $0.color) } ?? Theme.muted) { choosingProject = true }
                        if !projectSupported && !launch.catalogLoading && catalog != nil {
                            hint("Project assignment is unavailable on this computer. Update its connector and native CLI to choose a project.")
                        }
                    }
                    section("Folder") {
                        let projectFolders = project?.folders ?? []
                        if let project, projectFolders.isEmpty, folder.isEmpty {
                            notice(Theme.amber) { Text("\(project.name) has no folder on \(computer). Browse to choose one; it will be added to this project.") }
                        }
                        if projectFolders.isEmpty && folder.isEmpty {
                            Button { browsing = true } label: {
                                Label("Browse folders", systemImage: "folder").frame(maxWidth: .infinity, minHeight: 44)
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(busy)
                        } else {
                            HStack(spacing: 8) {
                                Menu {
                                    ForEach(projectFolders, id: \.id) { candidate in
                                        Button {
                                            select(candidate.path)
                                        } label: {
                                            Text(candidate.name)
                                            Text(candidate.path)
                                        }
                                    }
                                    Button("Browse…") { browsing = true }
                                } label: {
                                    fieldLabel(projectFolders.first { $0.path == folder }?.name ?? (LaunchPresentation.leaf(folder).nonEmpty ?? folder.nonEmpty ?? "Choose folder"),
                                               supporting: folder.isEmpty ? nil : inProject ? "Project folder" : related ? "Worktree of this project" : "Other folder",
                                               icon: Image(systemName: "folder"), tint: Theme.muted)
                                }
                                .disabled(busy)
                                Button("Browse") { browsing = true }.buttonStyle(.bordered).disabled(busy)
                            }
                        }
                        if !folder.isEmpty { Text(folder).font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled) }
                        if launch.directoryLoading && launch.stage.isEmpty {
                            ProgressView().progressViewStyle(.linear).tint(Theme.mint)
                        } else if !folder.isEmpty && !folderValid && !busy {
                            HStack {
                                hint("This folder has not been checked on \(computer).")
                                Spacer()
                                Button("Check again") { launch.browse(machine, folder) }.font(.footnote.weight(.semibold))
                            }
                        }
                        if !automatic, let project, !folder.isEmpty {
                            if related { hint("Verified worktree of \(project.name). No project folder will be added.") }
                            else if !inProject { hint("This folder is outside \(project.name). Starting here adds it to the project.", color: Theme.amber) }
                        }
                    }
                    section("Worktree") {
                        HStack(spacing: 6) {
                            Picker("Worktree", selection: Binding(get: { automatic }, set: { value in
                                autoPreferred = value
                                edited = true
                            })) {
                                Label("Auto", systemImage: "arrow.triangle.branch").tag(true)
                                Label("None", systemImage: "folder").tag(false)
                            }
                            .pickerStyle(.segmented)
                            .disabled(busy || !autoPossible)
                            if supported {
                                Menu {
                                    Button("Choose existing…", systemImage: "arrow.triangle.branch") { choosingWorktree = true }.disabled(!canChoose)
                                    Button("Create manually…", systemImage: "plus") { creatingWorktree = true }.disabled(!canCreate)
                                } label: { Image(systemName: "ellipsis.circle").frame(width: 36, height: 36) }
                                    .disabled(!(canChoose || canCreate))
                                    .accessibilityLabel("Worktree options")
                            }
                        }
                        if automatic {
                            VStack(alignment: .leading, spacing: 2) {
                                Text("New worktree").font(.caption.weight(.semibold)).foregroundStyle(Theme.muted)
                                Text(plannedPath).font(.footnote).textSelection(.enabled)
                                hint("New branch \(branch) from HEAD, created when the session starts.")
                                if plannedAdded, let project { hint("The new worktree will be added to \(project.name).", color: Theme.amber) }
                            }
                        } else if autoPossible {
                            hint("The session starts directly in the selected folder.")
                        } else if let note = WorktreePresentation.automaticNote(launch.worktrees, folder: folder, supported: supported, loading: launch.worktreeLoading).nonEmpty {
                            hint(note)
                        }
                    }
                    section("Account and agent") {
                        LaunchAccountList(choices: choices, provider: provider, account: account, enabled: !busy) { chosen in
                            provider = chosen.provider
                            account = chosen.id
                            edited = true
                        }
                        if providers.isEmpty && !launch.catalogLoading && catalog != nil { hint("No installed native providers were reported.") }
                    }
                    if provider == "opencode" {
                        section("Model") {
                            NavigationLink {
                                OpenCodeModelList(models: modelOptions.map {
                                    OpenCodeModel(value: $0.value, label: $0.label, provider: $0.provider, providerLabel: $0.providerLabel,
                                        free: $0.free, favorite: $0.favorite, isDefault: $0.isDefault, efforts: $0.efforts)
                                }, selected: selectedModel, nativeDefault: "Native default model") { selectedModel = $0 }
                            } label: {
                                HStack {
                                    Text("Model").foregroundStyle(Theme.onSurface)
                                    Spacer()
                                    Text(modelOptions.first { $0.value == selectedModel }?.label ?? "Native default model").foregroundStyle(Theme.mint)
                                }
                            }
                            .disabled(busy)
                            let notice = LaunchPresentation.catalogNotice(catalog, provider: provider, accountId: account)
                            if !notice.isEmpty { hint(notice) }
                            hint("Models configured on \(computer). Native default uses OpenCode's configuration.")
                            if !effortOptions.isEmpty {
                                HStack {
                                    Text("Effort").foregroundStyle(Theme.onSurface)
                                    Spacer()
                                    Picker("Effort", selection: $selectedEffort) {
                                        Text("Model default").tag("")
                                        ForEach(effortOptions, id: \.value) { Text($0.label).tag($0.value) }
                                    }
                                    .pickerStyle(.menu)
                                    .disabled(busy)
                                }
                            }
                        }
                    }
                    section("Session name") {
                        TextField("Session name", text: $tag)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .padding(12)
                            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 12))
                            .overlay(RoundedRectangle(cornerRadius: 12).stroke(!tag.isEmpty && !nameError.isEmpty ? Theme.amber : Theme.outline))
                            .disabled(busy)
                        if !tag.isEmpty && !nameError.isEmpty { Text(nameError).font(.footnote).foregroundStyle(Theme.amber) }
                    }
                }
                .padding(16)
            }
            .scrollDismissesKeyboard(.interactively)
            .safeAreaInset(edge: .bottom) {
                VStack(spacing: 8) {
                    if !busy && !launch.error.isEmpty { Text(launch.error).font(.footnote).foregroundStyle(Theme.amber).frame(maxWidth: .infinity, alignment: .leading) }
                    if !busy && !reason.isEmpty && reason != launch.error {
                        Text(reason).font(.footnote).foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading)
                    }
                    Button {
                        create(automatic: automatic, reason: reason, placement: placement)
                    } label: {
                        HStack(spacing: 10) {
                            if busy { ProgressView().tint(Theme.background) }
                            Text(busy ? (progress.nonEmpty ?? "Working…") : "Create session").fontWeight(.semibold)
                        }
                        .frame(maxWidth: .infinity, minHeight: 48)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(busy || !reason.isEmpty)
                }
                .padding(16)
                .background(.bar)
            }
            .navigationTitle("New session")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button { if dirty { discarding = true } else { launch.close() } } label: { Image(systemName: "xmark") }
                        .disabled(busy)
                        .accessibilityLabel("Close")
                }
                ToolbarItem(placement: .principal) { MachineLabel(name: computer, colorHex: app.machineColor(machine)) }
            }
            .zerusScreen()
        }
        .interactiveDismissDisabled(busy || dirty)
        .onAppear { tag = launch.generatedName }
        .onChange(of: signature, initial: true) { if allowedCatalog.isEmpty && !signature.isEmpty { allowedCatalog = signature } }
        .onChange(of: providers, initial: true) { if !providers.contains(provider) { provider = providers.first ?? "" } }
        .onChange(of: accounts.map(\.id), initial: true) { account = LaunchPresentation.defaultAccount(accounts, selected: account) }
        .onChange(of: modelOptions.map(\.value)) { if !selectedModel.isEmpty && !modelOptions.contains(where: { $0.value == selectedModel }) { selectedModel = "" } }
        .onChange(of: effortOptions.map(\.value)) { if !selectedEffort.isEmpty && !effortOptions.contains(where: { $0.value == selectedEffort }) { selectedEffort = "" } }
        .onChange(of: projects.map(\.id), initial: true) {
            guard !projects.contains(where: { $0.id == projectId }) else { return }
            let scoped = app.sessionProjectScope?.key
            let preferred = scoped.flatMap { $0.connectionId == machine.connectionId && $0.swarmId == catalog?.string("swarm_id") ? $0.id : nil } ?? catalog?.string("default_project")
            projectId = projects.first { $0.id == preferred }?.id ?? projects.first?.id ?? ""
            folder = browsedFolder
        }
        .onChange(of: FolderKey(project: projectId, loading: launch.directoryLoading)) {
            if folder.isEmpty && !launch.directoryLoading, let first = project?.folders.first {
                folder = first.path
                launch.browse(machine, first.path)
            }
        }
        .onChange(of: launch.createdWorktree) {
            // A confirmed checkout from Auto or Create manually is selected directly.
            let created = launch.createdWorktree
            if !created.isEmpty && created != appliedWorktree {
                appliedWorktree = created
                folder = created
                browsedFolder = ""
                autoPreferred = false
                creatingWorktree = false
                edited = true
            }
        }
        .sheet(isPresented: $choosingProject) {
            ProjectPicker(projects: projects, selected: projectId) { chosen in
                choosingProject = false
                if chosen != projectId {
                    projectId = chosen
                    folder = browsedFolder
                    edited = true
                }
            }
        }
        .sheet(isPresented: $choosingWorktree) {
            WorktreeChooser(entries: WorktreePresentation.entries(evidence), selected: folder) { path in
                choosingWorktree = false
                autoPreferred = false
                select(path)
            }
        }
        .sheet(isPresented: $creatingWorktree) {
            if let evidence {
                CreateWorktreeSheet(machine: machine, evidence: evidence,
                                    suggestedBranch: tag.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: " ", with: "-").nonEmpty ?? branch)
            }
        }
        .sheet(isPresented: $browsing, onDismiss: {
            // Navigation replaced the verified folder; check the kept selection again so it stays usable.
            if !folder.isEmpty && !LaunchPresentation.selectedFolder(launch.directory, folder) { launch.browse(machine, folder) }
        }) {
            FolderBrowser(machine: machine) { selected in
                folder = selected
                browsedFolder = selected
                browsing = false
                edited = true
            }
        }
        .alert("Discard new session?", isPresented: $discarding) {
            Button("Discard", role: .destructive) { launch.close() }
            Button("Keep editing", role: .cancel) {}
        } message: {
            Text("Your choices for this session will be lost." + (createdWorktree.map { "\n\nThe created worktree and its branch stay on \(computer): \($0.resultPath)" } ?? ""))
        }
    }

    private static func readiness(launch: LaunchModel, machine: MachineKey, catalog: JSONObject?, allowedCatalog: String, changed: Bool,
                                  projectSupported: Bool, project: LaunchProject?, folder: String, folderValid: Bool, providers: [String], provider: String,
                                  accounts: [LaunchAccount], account: String, nameError: String, automatic: Bool, confirmed: Bool) -> LaunchReadiness {
        var readiness = LaunchReadiness()
        readiness.machineReason = launch.reason(machine)
        readiness.catalogLoading = launch.catalogLoading
        readiness.catalogAvailable = catalog != nil && !allowedCatalog.isEmpty
        readiness.catalogChanged = changed
        readiness.projectRequired = projectSupported
        readiness.projectChosen = project != nil
        readiness.projectHasFolder = project.map { !$0.folders.isEmpty } ?? true
        readiness.folderChosen = !folder.isEmpty
        readiness.folderLoading = launch.directoryLoading
        readiness.folderVerified = folderValid
        readiness.repositoryLoading = launch.worktreeLoading
        readiness.providers = providers.count
        readiness.providerChosen = providers.contains(provider)
        readiness.accounts = accounts.count
        readiness.accountChosen = accounts.contains { $0.id == account }
        readiness.nameError = nameError
        readiness.automatic = automatic
        readiness.automaticFailed = launch.automaticFailed
        readiness.automaticUsed = !launch.createdWorktree.isEmpty
        readiness.worktreeReason = launch.worktreeReason(machine)
        readiness.confirmed = confirmed
        return readiness
    }

    private struct FolderKey: Equatable {
        let project: String
        let loading: Bool
    }

    private func select(_ path: String) {
        folder = path
        browsedFolder = ""
        edited = true
        launch.browse(machine, path)
    }

    private func create(automatic: Bool, reason: String, placement: JSONObject?) {
        guard !launch.busy, reason.isEmpty else { return }
        if automatic {
            launch.launchWithWorktree(machine, agent: provider, folder: folder, tag: tag, accountId: account, expectedCatalog: allowedCatalog, projectId: projectId,
                                      model: selectedModel, effort: selectedEffort)
        } else {
            launch.launch(machine, agent: provider, directory: folder, tag: tag, accountId: account, expectedCatalog: allowedCatalog, projectId: projectId,
                          worktrees: placement, model: selectedModel, effort: selectedEffort)
        }
    }

    private func section(_ title: String, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.muted).accessibilityAddTraits(.isHeader)
            content()
        }
    }

    private func hint(_ text: String, color: Color = Theme.muted) -> some View { Text(text).font(.footnote).foregroundStyle(color) }

    private func notice(_ accent: Color?, @ViewBuilder content: () -> some View) -> some View {
        VStack(alignment: .leading, spacing: 6) { content() }
            .font(.callout)
            .buttonStyle(.borderless)
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke((accent ?? Theme.outline).opacity(0.55)))
    }

    @ViewBuilder
    private func planned(_ action: SessionAction) -> some View {
        let destination = action.arguments.string("destination")
        if !destination.isEmpty { Text(destination).font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled) }
    }

    private func field(_ value: String, supporting: String?, enabled: Bool, icon: Image, tint: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) { fieldLabel(value, supporting: supporting, icon: icon, tint: tint) }
            .buttonStyle(.plain)
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.5)
    }

    private func fieldLabel(_ value: String, supporting: String?, icon: Image, tint: Color) -> some View {
        HStack(spacing: 12) {
            icon.foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 2) {
                Text(value).lineLimit(1).foregroundStyle(Theme.onSurface)
                if let supporting, !supporting.isEmpty { Text(supporting).font(.footnote).lineLimit(1).foregroundStyle(Theme.muted) }
            }
            Spacer(minLength: 0)
            Image(systemName: "chevron.up.chevron.down").foregroundStyle(Theme.muted)
        }
        .padding(.horizontal, 14)
        .frame(minHeight: 52)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline))
    }
}

/// Accounts grouped as on the Accounts tab, with the desktop quota and pace rails.
struct LaunchAccountList: View {
    let choices: [LaunchAccountChoice]
    let provider: String
    let account: String
    let enabled: Bool
    let onChoose: (LaunchAccount) -> Void

    var body: some View {
        TimelineView(.periodic(from: .now, by: 30)) { timeline in
            let now = timeline.date.timeIntervalSince1970
            VStack(spacing: 0) {
                ForEach(choices) { choice in
                    let selected = choice.account.provider == provider && choice.account.id == account
                    let limits = choice.usage.map { LaunchPresentation.accountLimits($0, now: now) } ?? choice.limits
                    Button { onChoose(choice.account) } label: {
                        HStack(spacing: 12) {
                            ProviderBadge(agent: choice.account.provider, size: 28)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(choice.title).fontWeight(.semibold).lineLimit(1).foregroundStyle(Theme.onSurface)
                                LimitRails(usage: choice.usage, now: now, limits: limits)
                            }
                            Spacer(minLength: 0)
                            if selected { Image(systemName: "checkmark").foregroundStyle(Theme.mint) }
                        }
                        .padding(.horizontal, 14)
                        .padding(.vertical, 10)
                        .frame(minHeight: 60)
                        .background(selected ? Theme.mint.opacity(0.14) : Color.clear)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(DesktopGlyph.providerName(choice.account.provider)) \(choice.title), \(limits)")
                    .accessibilityAddTraits(selected ? .isSelected : [])
                    if choice.id != choices.last?.id { Divider() }
                }
            }
            .disabled(!enabled)
            .opacity(enabled ? 1 : 0.5)
            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline))
        }
    }
}

struct LimitRails: View {
    let usage: ReportedAccount?
    let now: Double
    let limits: String

    var body: some View {
        let windows = Array(LaunchPresentation.limitWindows(usage, now: now).prefix(2))
        if windows.isEmpty {
            Text(limits).font(.footnote).foregroundStyle(Theme.muted).lineLimit(1)
        } else {
            HStack(spacing: 10) {
                ForEach(Array(windows.enumerated()), id: \.offset) { _, window in
                    let pace = LaunchPresentation.limitPace(window, now: now)
                    let color = pace.tone == .critical ? Color(hex: 0xF07878) : pace.tone == .warning ? Color(hex: 0xF0A35B) : Color(hex: 0x72CDB2)
                    HStack(spacing: 6) {
                        Canvas { context, size in
                            let track = Color(hex: 0x36434E)
                            let top: CGFloat = pace.elapsed == nil ? 3 : 1, height: CGFloat = pace.elapsed == nil ? 4 : 3
                            context.fill(Path(roundedRect: CGRect(x: 0, y: top, width: size.width, height: height), cornerRadius: height / 2), with: .color(track))
                            context.fill(Path(roundedRect: CGRect(x: 0, y: top, width: size.width * pace.used / 100, height: height), cornerRadius: height / 2), with: .color(color))
                            if let elapsed = pace.elapsed {
                                context.fill(Path(roundedRect: CGRect(x: 0, y: 6, width: size.width, height: 2), cornerRadius: 1), with: .color(track))
                                context.fill(Path(roundedRect: CGRect(x: 0, y: 6, width: size.width * elapsed, height: 2), cornerRadius: 1), with: .color(Color(hex: 0x8395A5)))
                            }
                        }
                        .frame(width: 20, height: 10)
                        Text("\(AccountPresentation.shortPeriod(window)) \(Int(pace.used + 0.5))%").font(.footnote).foregroundStyle(color).lineLimit(1)
                    }
                }
            }
        }
    }
}

/// A searchable project list in catalog order.
struct ProjectPicker: View {
    @Environment(\.dismiss) private var dismiss
    let projects: [LaunchProject]
    let selected: String
    let onChoose: (String) -> Void
    @State private var query = ""

    var body: some View {
        let visible = LaunchPresentation.filterProjects(projects, query)
        NavigationStack {
            List {
                if visible.isEmpty { Text(projects.isEmpty ? "No projects are available on this computer." : "No projects match").foregroundStyle(Theme.muted) }
                ForEach(visible) { project in
                    Button { onChoose(project.id) } label: {
                        HStack(spacing: 14) {
                            Image(systemName: "folder.fill").foregroundStyle(Color(projectHex: project.color) ?? Theme.mint)
                            VStack(alignment: .leading) {
                                Text(project.name).fontWeight(project.id == selected ? .semibold : .regular).foregroundStyle(Theme.onSurface)
                                Text(LaunchPresentation.folderSummary(project)).font(.footnote).foregroundStyle(Theme.muted).lineLimit(1)
                            }
                            Spacer()
                            if project.id == selected { Image(systemName: "checkmark").foregroundStyle(Theme.mint) }
                        }
                    }
                }
            }
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Search projects and folders")
            .navigationTitle("Choose project")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .zerusScreen()
        }
    }
}

struct WorktreeChooser: View {
    @Environment(\.dismiss) private var dismiss
    let entries: [LaunchWorktree]
    let selected: String
    let onChoose: (String) -> Void

    var body: some View {
        NavigationStack {
            List(entries, id: \.path) { entry in
                Button { onChoose(entry.path) } label: {
                    HStack(spacing: 12) {
                        Image(systemName: "arrow.triangle.branch").foregroundStyle(Theme.muted)
                        VStack(alignment: .leading) {
                            Text(entry.branch.nonEmpty ?? "detached HEAD").fontWeight(entry.path == selected ? .semibold : .regular).foregroundStyle(Theme.onSurface)
                            Text(entry.kind == "main" ? "Main checkout · \(entry.path)" : entry.path).font(.footnote).foregroundStyle(Theme.muted)
                        }
                        Spacer()
                        if entry.path == selected { Image(systemName: "checkmark").foregroundStyle(Theme.mint) }
                    }
                }
            }
            .navigationTitle("Choose an existing worktree")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } } }
            .zerusScreen()
        }
        .presentationDetents([.medium, .large])
    }
}

/// Native folder browser with explicit, receipt-confirmed folder creation.
struct FolderBrowser: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let machine: MachineKey
    let onChoose: (String) -> Void
    @State private var typedPath = ""
    @State private var lastFolder = ""
    @State private var creatingPath: String?
    @State private var observedCreation = ""

    var body: some View {
        let launch = app.launch!
        let raw = launch.directory
        let path = raw?.string("path") ?? ""
        let parent = raw?.string("parent") ?? ""
        let home = raw?.string("home") ?? ""
        let folders = LaunchPresentation.folders(raw)
        let busy = launch.busy || launch.directoryLoading
        let pending = DirectoryCreation.pending(app.conversation.sessionActions, machine)
        let createReason = launch.directoryCreateReason(machine)
        NavigationStack {
            List {
                Section {
                    if launch.directoryLoading || launch.directoryCreateSubmitting { ProgressView().progressViewStyle(.linear).tint(Theme.mint) }
                    Text(path.nonEmpty ?? lastFolder.nonEmpty ?? "Loading native folders…").font(.footnote).textSelection(.enabled)
                    HStack(spacing: 18) {
                        Button("Up") { launch.browse(machine, parent) }.disabled(parent.isEmpty || busy)
                        Button("Home") { launch.browse(machine, home.nonEmpty ?? "~") }.disabled(busy)
                        Button("New folder", systemImage: "plus") { creatingPath = DirectoryCreation.child(lastFolder, "New folder") }
                            .disabled(!LaunchPresentation.absoluteFolder(lastFolder) || busy || !createReason.isEmpty)
                    }
                    .buttonStyle(.borderless)
                    TextField("Absolute folder path", text: $typedPath)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .disabled(busy)
                    HStack(spacing: 18) {
                        Button("Browse") { launch.browse(machine, typedPath) }.disabled(!LaunchPresentation.absoluteFolder(typedPath) || busy)
                        if !typedPath.isEmpty && typedPath != path && !folders.contains(where: { $0.path == typedPath }) {
                            Button("Create this path") { creatingPath = typedPath }
                                .disabled(!DirectoryCreation.pathError(typedPath).isEmpty || busy || !createReason.isEmpty)
                        }
                    }
                    .buttonStyle(.borderless)
                    if let pending {
                        Text(DirectoryCreation.uncertain).font(.footnote)
                        Text(pending.arguments.string("path")).font(.footnote).foregroundStyle(Theme.muted)
                        Button("Check original folder result") { launch.checkDirectoryCreation(machine, pending) }.disabled(busy)
                    } else if !createReason.isEmpty {
                        Text(createReason).font(.footnote).foregroundStyle(Theme.muted)
                    }
                }
                Section {
                    ForEach(folders, id: \.path) { folder in
                        Button { launch.browse(machine, folder.path) } label: { Label(folder.name, systemImage: "folder") }.disabled(busy)
                    }
                    if raw?.bool("truncated") == true {
                        Text("Folder listing is incomplete. Browse an exact path to reach another folder.").font(.footnote).foregroundStyle(Theme.muted)
                    }
                    if !launch.error.isEmpty { Text(launch.error).font(.footnote).foregroundStyle(Theme.amber) }
                }
            }
            .navigationTitle("Choose folder on \(app.machineName(machine))")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(launch.busy) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Use this folder") { onChoose(path) }.disabled(!LaunchPresentation.absoluteFolder(path) || busy)
                }
            }
            .zerusScreen()
        }
        .interactiveDismissDisabled(launch.busy)
        .onChange(of: path, initial: true) {
            if !path.isEmpty {
                if typedPath.isEmpty { typedPath = path }
                lastFolder = path
            }
        }
        .onChange(of: launch.createdDirectory) {
            let created = launch.createdDirectory
            if !created.isEmpty && created != observedCreation {
                observedCreation = created
                typedPath = created
                creatingPath = nil
            }
        }
        .sheet(item: Binding(get: { creatingPath.map(PathItem.init) }, set: { creatingPath = $0?.path })) { item in
            CreateFolderSheet(machine: machine, initial: item.path)
        }
    }

    private struct PathItem: Identifiable {
        let path: String
        var id: String { path }
    }
}

struct CreateFolderSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let machine: MachineKey
    let initial: String
    @State private var path = ""

    var body: some View {
        let launch = app.launch!
        let busy = launch.directoryCreateSubmitting
        let problem = DirectoryCreation.pathError(path)
        NavigationStack {
            Form {
                Section {
                    Text("Create a folder on \(app.machineName(machine)). Its parent must already exist.")
                    TextField("New folder path", text: $path).textInputAutocapitalization(.never).autocorrectionDisabled().disabled(busy)
                } footer: {
                    VStack(alignment: .leading) {
                        if !problem.isEmpty { Text(problem) }
                        if !launch.error.isEmpty { Text(launch.error).foregroundStyle(Theme.amber) }
                    }
                }
                if busy { ProgressView("Creating folder…") }
            }
            .navigationTitle("Create folder")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(busy) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create folder") { launch.createDirectory(machine, path) }
                        .disabled(busy || !problem.isEmpty || !launch.directoryCreateReason(machine).isEmpty)
                }
            }
            .zerusScreen()
        }
        .interactiveDismissDisabled(busy)
        .presentationDetents([.medium])
        .onAppear { path = initial }
    }
}

/// Desktop Create manually…: a new branch and folder; the branch suggestion follows the session name.
struct CreateWorktreeSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let machine: MachineKey
    let evidence: JSONObject
    let suggestedBranch: String
    @State private var branch = ""
    @State private var base = "HEAD"
    @State private var destination = ""
    @State private var customPath = false

    var body: some View {
        let launch = app.launch!
        let submitting = launch.worktreeSubmitting
        let problem = WorktreePresentation.createError(branch: branch, base: base, destination: destination)
        NavigationStack {
            Form {
                Section {
                    Text("Creates a new branch and working folder on this computer. It remains there if you cancel or cannot start the session.").font(.footnote)
                    Text(evidence.string("path")).font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled)
                    TextField("New branch", text: Binding(get: { branch }, set: { value in
                        branch = value
                        if !customPath { destination = WorktreePresentation.destination(evidence, branch: value) }
                    }))
                    TextField("Start from", text: $base)
                    TextField("New folder", text: Binding(get: { destination }, set: { value in
                        destination = value
                        customPath = true
                    }))
                } footer: {
                    VStack(alignment: .leading) {
                        if !problem.isEmpty { Text(problem) }
                        if !launch.error.isEmpty { Text(launch.error).foregroundStyle(Theme.amber) }
                    }
                }
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .disabled(submitting)
                if submitting { ProgressView("Creating…") }
            }
            .navigationTitle("Create worktree")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(submitting) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create worktree") { launch.createWorktree(machine, evidence: evidence, branch: branch, base: base, destination: destination) }
                        .disabled(submitting || !problem.isEmpty || !launch.worktreeReason(machine).isEmpty)
                }
            }
            .zerusScreen()
        }
        .interactiveDismissDisabled(submitting)
        .onAppear {
            branch = suggestedBranch
            destination = WorktreePresentation.destination(evidence, branch: suggestedBranch)
        }
    }
}
