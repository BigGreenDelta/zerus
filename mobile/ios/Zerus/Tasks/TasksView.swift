import SwiftUI
import ZerusKit
import ZerusReplica

/// The app's own popup surfaces (docs/superpowers/specs/2026-10-20-task-tracker-ios-design.md, "Behaviour on iOS").
enum TasksColors {
    static let sheet = Color(hex: 0x1A2225)
    static let notice = Color(hex: 0x2A3538)
    static let blocked = Color(hex: 0x2B3B3B)
    static let danger = Color(hex: 0xE06C75)
}

extension TaskTab {
    /// The icon of the action that moves a task there, as on the desktop tiles.
    var symbol: String {
        switch self {
        case .review: "tray"
        case .active: "play.fill"
        case .todo: "text.badge.plus"
        case .backlog: "clock"
        case .done: "checkmark"
        }
    }
}

extension TaskAction {
    var symbol: String {
        switch self {
        case .edit: "pencil"
        case .toBacklog: "clock"
        case .toTop: "chevron.up.2"
        case .send: "play.fill"
        case .toTodo, .backToTodo: "text.badge.plus"
        case .cancel: "xmark"
        case .accept: "checkmark"
        }
    }
}

private struct OpenTask: Identifiable { let id: String }

/// The project's tasks full-screen, as Terminal opens; Back returns to the conversation.
struct TasksView: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    @State private var openTask: String?
    @State private var cancelling: TaskInfo?
    /// Cancel chosen in the task settings: its alert waits until the sheet has gone.
    @State private var cancelAfterSheet: TaskInfo?
    @State private var cancelReason = ""
    @State private var projectSettings = false
    @State private var allSessions = false
    @State private var shown: String?
    @State private var shownId = UUID()

    var body: some View {
        let tasks = app.tasks!
        NavigationStack {
            VStack(spacing: 0) {
                if !tasks.storageError.isEmpty && tasks.tasksKey != nil {
                    Text(tasks.storageError).foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading).padding(20)
                } else if let key = tasks.tasksKey {
                    chips(tasks, key)
                    tabStrip(tasks)
                    ProgressView().progressViewStyle(.linear).tint(Theme.mint).frame(height: 3).opacity(tasks.taskBusy ? 1 : 0)
                    if tasks.taskTab == .done { DoneList(onOpen: { openTask = $0.uuid }) } else { cards(tasks) }
                }
            }
            .safeAreaInset(edge: .bottom) { TaskInput().padding(.horizontal, 12).padding(.vertical, 8).background(Theme.background) }
            .overlay(alignment: .bottom) { notice }
            .background(Theme.background.ignoresSafeArea())
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button { dismiss() } label: { Image(systemName: "chevron.left") }.accessibilityLabel("Back")
                }
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 1) {
                        Text("Tasks").font(.headline)
                        Text(tasks.tasksKey.map(tasks.taskProjectName) ?? "").font(.caption).foregroundStyle(Theme.muted).lineLimit(1)
                    }
                }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    let only = tasks.tasksOnlyThisSession
                    Button { tasks.setOnlyThisSession(!only) } label: {
                        Image(systemName: "line.3.horizontal.decrease.circle").foregroundStyle(only ? Theme.mint : Theme.onSurface)
                    }
                    .accessibilityLabel("Only this session")
                    .accessibilityValue(only ? "On" : "Off")
                    Button { projectSettings = true } label: { Image(systemName: "gearshape") }.accessibilityLabel("Project task settings")
                }
            }
        }
        .onChange(of: tasks.tasksKey) { _, key in if key == nil { dismiss() } }
        .onChange(of: tasks.taskNotice) { _, message in
            guard let message else { return }
            tasks.taskNotice = nil
            shown = message
            shownId = UUID()
        }
        .task(id: shownId) {
            try? await Task.sleep(for: .seconds(3))
            if !Task.isCancelled { shown = nil }
        }
        .sheet(item: Binding(get: { openTask.map(OpenTask.init) }, set: { openTask = $0?.id }), onDismiss: {
            guard let task = cancelAfterSheet else { return }
            cancelAfterSheet = nil
            cancelReason = ""
            cancelling = task
        }) { open in
            TaskSettingsSheet(uuid: open.id) { task in cancelAfterSheet = task }
        }
        .sheet(isPresented: $projectSettings) { ProjectTaskSettingsSheet() }
        .sheet(isPresented: $allSessions) { AllSessionsSheet() }
        .alert(cancelling.map { "Cancel \($0.id)?" } ?? "", isPresented: Binding(get: { cancelling != nil }, set: { if !$0 { cancelling = nil } }),
               presenting: cancelling) { task in
            TextField("Reason (optional)", text: $cancelReason)
            Button("Keep running", role: .cancel) {}
            Button("Cancel task", role: .destructive) {
                tasks.cancelTask(task, reason: cancelReason.trimmingCharacters(in: .whitespacesAndNewlines))
            }
        } message: { task in
            Text(cancelMessage(tasks, task))
        }
    }

    private func cancelMessage(_ tasks: TasksModel, _ task: TaskInfo) -> String {
        let session = task.session?.session ?? "the session"
        var direct = true
        if case .ready(let route) = tasks.cancelRoute(task) { direct = route.direct }
        let what = direct ? "\(task.id) moves to In review, and \(session) gets the cancel message."
            : "\(task.id) moves to In review. The computer of \(session) is out of reach, so \(session) gets no cancel message."
        return "\(task.text.prefix(300))\n\n\(what)\n\nThe reason is saved as a comment on the task, not sent to the agent."
    }

    @ViewBuilder private var notice: some View {
        if let shown {
            Text(shown)
                .font(.footnote)
                .foregroundStyle(Theme.onSurface)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(TasksColors.notice, in: RoundedRectangle(cornerRadius: 10))
                .padding(.horizontal, 16)
                .padding(.bottom, 76)
                .transition(.opacity)
                .accessibilityAddTraits(.updatesFrequently)
        }
    }

    private func chips(_ tasks: TasksModel, _ key: ProjectKey) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(tasks.taskChips(key), id: \.session) { chip in
                    let selected = chip.session == tasks.tasksChosen
                    Button { tasks.chooseTaskSession(key, chip.session) } label: {
                        HStack(spacing: 6) {
                            StatusDot(dot: chip.dot)
                            Text(chip.session.name).font(.caption.weight(.medium)).lineLimit(1)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 6)
                        .foregroundStyle(selected ? Theme.background : Theme.onSurface)
                        .background(selected ? Theme.mint : Theme.surface, in: Capsule())
                        .overlay { if !selected { Capsule().stroke(Theme.outline) } }
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(selected ? .isSelected : [])
                }
                Button { allSessions = true } label: {
                    Image(systemName: "ellipsis").frame(width: 32, height: 28).background(Theme.surface, in: Capsule())
                        .overlay { Capsule().stroke(Theme.outline) }
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Sessions of every project")
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
    }

    private func tabStrip(_ tasks: TasksModel) -> some View {
        let counts = TasksPresentation.counts(tasks.taskListing, chosen: tasks.tasksChosen, onlyThisSession: tasks.tasksOnlyThisSession)
        return ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 18) {
                    ForEach(TaskTab.allCases, id: \.self) { tab in
                        let count = tab == .done ? (tasks.doneLoaded ? tasks.doneRecent : nil) : counts[tab]
                        let selected = tasks.taskTab == tab
                        Button { tasks.selectTaskTab(tab) } label: {
                            VStack(spacing: 6) {
                                Text(count.map { $0 > 0 ? "\(tab.label) \($0)" : tab.label } ?? tab.label)
                                    .font(.subheadline.weight(selected ? .semibold : .regular))
                                    .foregroundStyle(selected ? Theme.mint : Theme.onSurface)
                                    .lineLimit(1)
                                Rectangle().fill(selected ? Theme.mint : .clear).frame(height: 2)
                            }
                            .fixedSize()
                        }
                        .buttonStyle(.plain)
                        .id(tab)
                        .accessibilityAddTraits(selected ? .isSelected : [])
                    }
                }
                .padding(.horizontal, 16)
            }
            .onChange(of: tasks.taskTab) { _, tab in withAnimation { proxy.scrollTo(tab, anchor: .center) } }
            .onAppear { proxy.scrollTo(tasks.taskTab, anchor: .center) }
        }
        .padding(.top, 4)
    }

    private func cards(_ tasks: TasksModel) -> some View {
        let cards = TasksPresentation.cards(tasks.taskListing, tab: tasks.taskTab, chosen: tasks.tasksChosen,
                                            onlyThisSession: tasks.tasksOnlyThisSession, send: tasks.sendRoute, cancel: tasks.cancelRoute)
        let key = tasks.tasksKey
        return ScrollView {
            LazyVStack(spacing: 10) {
                if cards.isEmpty {
                    Text("Nothing \(tasks.taskTab.label.prefix(1).lowercased() + tasks.taskTab.label.dropFirst()).")
                        .foregroundStyle(Theme.muted)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                }
                ForEach(cards, id: \.uuid) { card in
                    if let task = tasks.taskListing.first(where: { $0.uuid == card.uuid }) {
                        TaskCardView(card: card, badge: card.tab == .active ? activeSession(tasks, key, task.session) : nil,
                                     onOpen: { openTask = card.uuid }, onAction: { act(tasks, task, $0) })
                    }
                }
            }
            .padding(12)
        }
    }

    private func activeSession(_ tasks: TasksModel, _ key: ProjectKey?, _ ref: SessionRef?) -> Session? {
        guard let key, let ref else { return nil }
        return tasks.projectSessions(key).first { session in
            session.target.session == ref.session && app.machines.contains { $0.id == session.target.computerId && $0.machineId == ref.machine }
        }
    }

    private func act(_ tasks: TasksModel, _ task: TaskInfo, _ state: ActionState) {
        if state.action == .edit { openTask = task.uuid }
        else if !state.enabled { tasks.taskNotice = state.reason }
        else if state.action == .cancel {
            cancelReason = ""
            cancelling = task
        } else { tasks.taskAction(task, state.action) }
    }
}

struct StatusDot: View {
    let dot: ChipDot
    var body: some View {
        Circle()
            .fill(dot == .working ? Theme.mint : dot == .attention ? Theme.amber : Theme.muted)
            .frame(width: 8, height: 8)
            .accessibilityLabel(dot == .working ? "Working" : dot == .attention ? "Needs input" : "Ready")
    }
}

/// The session card style; the header keeps its height, so its icons never move the text.
struct TaskCardView: View {
    @Environment(AppModel.self) private var app
    let card: TaskCard
    let badge: Session?
    let onOpen: () -> Void
    let onAction: (ActionState) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Image(systemName: card.tab.symbol).font(.caption).foregroundStyle(Theme.muted)
                Text(card.id).font(.caption.weight(.medium)).foregroundStyle(Theme.muted).lineLimit(1)
                Text(card.sessionLabel).font(.caption.weight(.medium)).lineLimit(1)
                    .foregroundStyle(card.highlighted ? Theme.mint : Theme.muted)
                Spacer(minLength: 0)
                ForEach(card.actions, id: \.action) { state in
                    // A disabled action still answers a tap with its reason.
                    Button { onAction(state) } label: {
                        Image(systemName: state.action.symbol).font(.system(size: 15, weight: .medium)).frame(width: 36, height: 36)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(state.enabled ? Theme.onSurface : Theme.muted.opacity(0.45))
                    .accessibilityLabel(state.action.label)
                    .accessibilityHint(state.enabled ? "" : "Unavailable: \(state.reason)")
                }
            }
            .frame(height: 44)
            if let badge { SessionStatusBadge(session: badge, online: SessionFilters.online(badge, app.machines)).padding(.bottom, 4) }
            Text(card.text).font(.subheadline).foregroundStyle(Theme.onSurface).lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.trailing, 10)
            if !card.note.isEmpty {
                Text(card.note).font(.footnote).foregroundStyle(Theme.muted).lineLimit(3).padding(.top, 6).padding(.trailing, 10)
            }
        }
        .padding(.leading, 14)
        .padding(.trailing, 4)
        .padding(.bottom, 12)
        .background(Theme.surface, in: RoundedRectangle(cornerRadius: 14))
        .overlay { if card.highlighted { RoundedRectangle(cornerRadius: 14).stroke(Theme.mint, lineWidth: 1.5) } }
        .contentShape(RoundedRectangle(cornerRadius: 14))
        .onTapGesture(perform: onOpen)
        .accessibilityElement(children: .contain)
    }
}

/// Done from a computer: the last 7 days, then "Show older (N)". A tap fetches the task, then opens it.
private struct DoneList: View {
    @Environment(AppModel.self) private var app
    let onOpen: (TaskInfo) -> Void

    var body: some View {
        let tasks = app.tasks!
        let rows = tasks.doneExpanded ? tasks.doneRows : Array(tasks.doneRows.prefix(tasks.doneRecent))
        let older = TasksPresentation.olderLabel(total: tasks.doneTotal, recent: tasks.doneRecent, shown: rows.count,
                                                 expanded: tasks.doneExpanded, more: tasks.doneNext != nil)
        ScrollView {
            LazyVStack(spacing: 10) {
                if !tasks.doneError.isEmpty {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(tasks.doneError).foregroundStyle(Theme.muted)
                        Button("Try again") { tasks.loadDone(more: false) }.disabled(tasks.doneLoading)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
                }
                if rows.isEmpty && tasks.doneLoaded && tasks.doneError.isEmpty {
                    Text(tasks.doneTotal > 0 ? "Nothing done in the last 7 days." : "Nothing done.")
                        .foregroundStyle(Theme.muted).frame(maxWidth: .infinity, alignment: .leading).padding(8)
                }
                ForEach(rows, id: \.uuid) { row in
                    let card = TaskCard(uuid: row.uuid, id: row.id, text: row.text, tab: .done, sessionLabel: row.session?.session ?? "any session",
                                        highlighted: false, note: "",
                                        actions: [ActionState(.backToTodo, enabled: !tasks.taskBusy, reason: "Another task action is running")])
                    TaskCardView(card: card, badge: nil, onOpen: { tasks.openDoneTask(row, opened: onOpen) }) { state in
                        if !state.enabled { tasks.taskNotice = state.reason }
                        else { tasks.openDoneTask(row) { tasks.taskAction($0, .backToTodo) } }
                    }
                }
                if tasks.doneLoading { ProgressView().padding(8) }
                else if let older { Button(older) { tasks.loadDone(more: true) }.padding(8) }
            }
            .padding(12)
        }
    }
}

/// The app's message input without the attach button and the microphone; nothing is sent to the agent.
private struct TaskInput: View {
    @Environment(AppModel.self) private var app
    @State private var text = ""

    var body: some View {
        let tasks = app.tasks!
        let stage = tasks.taskTab == .backlog ? "Backlog" : "To do"
        let reason = TasksPresentation.addReason(tasks.tasksChosen)
        let blank = text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        let canAdd = !blank && reason.isEmpty
        HStack(alignment: .bottom, spacing: 8) {
            TextField("Add to \(stage)", text: $text, axis: .vertical)
                .lineLimit(1...6)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(Theme.surface, in: RoundedRectangle(cornerRadius: 20))
                .overlay { RoundedRectangle(cornerRadius: 20).stroke(Theme.outline) }
            // A session that cannot take tasks answers the tap with why.
            Button {
                if canAdd {
                    tasks.addTask(text.trimmingCharacters(in: .whitespacesAndNewlines))
                    text = ""
                } else { tasks.taskNotice = reason }
            } label: {
                Image(systemName: !blank && !canAdd ? "info" : "text.badge.plus")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(canAdd ? Theme.background : Theme.muted)
                    .frame(width: 40, height: 40)
                    .background(canAdd ? Theme.mint : TasksColors.blocked, in: Circle())
            }
            .buttonStyle(.plain)
            .disabled(blank)
            .accessibilityLabel(!blank && !canAdd ? "Why the task cannot be added" : "Add to \(stage)")
        }
    }
}
