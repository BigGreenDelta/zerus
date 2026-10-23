import QuickLook
import SwiftUI
import ZerusKit

enum AppTab: Hashable {
    case sessions, projects, drafts, accounts, machines
}

struct RootView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.scenePhase) private var scenePhase
    @State private var tab: AppTab = .sessions
    @State private var settings = false
    @State private var pairing: PairingRequest?
    @State private var sessionsPath = NavigationPath()
    @State private var projectsPath = NavigationPath()

    var body: some View {
        @Bindable var model = model
        TabView(selection: Binding(get: { tab }, set: { select($0) })) {
            Tab(value: AppTab.sessions) {
                SessionsScreen(settings: $settings, path: $sessionsPath, onPair: { tab = .machines }, onBackToProject: {
                    model.clearSessionScope()
                    tab = .projects
                })
            } label: { Label("Sessions", systemImage: "rectangle.stack") }
            Tab(value: AppTab.projects) {
                ProjectsScreen(settings: $settings, path: $projectsPath, onPair: { tab = .machines }, onViewSessions: { project in
                    model.viewProjectSessions(project)
                    sessionsPath = NavigationPath()
                    tab = .sessions
                })
            } label: { Label("Projects", systemImage: "folder") }
            Tab(value: AppTab.drafts) {
                DraftsScreen(settings: $settings, onOpen: openConversation)
            } label: { Label("Drafts", systemImage: "square.and.pencil") }
            .badge(model.conversation.drafts.filter { $0.status != "editing" && $0.status != "submitted" && $0.questionId != "__interrupt" }.count +
                   model.conversation.normalizedOutgoing.filter { ["uncertain", "failed"].contains($0.status) }.count)
            Tab(value: AppTab.accounts) {
                AccountsScreen(settings: $settings, onMachines: { tab = .machines })
            } label: { Label("Accounts", systemImage: "person.crop.circle") }
            Tab(value: AppTab.machines) {
                MachinesScreen(settings: $settings, pairing: $pairing)
            } label: { Label("Machines", systemImage: "desktopcomputer") }
        }
        .minimizingTabBarOnScroll()
        .sheet(isPresented: $settings) { SettingsView() }
        .quickLookPreview(Binding(get: { model.viewer.opened?.file }, set: { if $0 == nil { model.viewer.opened = nil } }))
        .fullScreenCover(isPresented: Binding(get: { model.launch.visible }, set: { if !$0 { model.launch.close() } })) {
            if model.launch.machine != nil { LaunchForm().environment(model) }
            else { NavigationStack { Text("Refresh your connected computers first.").toolbar { Button("Close") { model.launch.close() } } } }
        }
        .sheet(item: $pairing) { request in PairingSheet(request: request) }
        .onChange(of: model.conversationRequest) { _, target in
            guard let target else { return }
            model.conversationRequest = nil
            openConversation(target)
        }
        .overlay(alignment: .bottom) {
            if !model.notice.isEmpty {
                Text(model.notice)
                    .font(.footnote)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                    .background(.regularMaterial, in: Capsule())
                    .padding(.bottom, 90)
                    .transition(.opacity)
                    .task(id: model.notice) {
                        try? await Task.sleep(for: .seconds(3))
                        model.notice = ""
                    }
            }
        }
        .onChange(of: model.invitation) { _, invite in
            guard let invite else { return }
            model.invitation = nil
            tab = .machines
            pairing = PairingRequest(server: invite.server, code: invite.code, fromLink: true)
        }
        .task(id: scenePhase) {
            // While visible, refresh every five seconds, as the Android client does.
            guard scenePhase == .active else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                if Task.isCancelled { break }
                if model.conversation.selected != nil { model.conversation.refreshActivity() }
                else if tab != .drafts { model.refresh() }
            }
        }
        .task(id: scenePhase) {
            // Apply confirmed "next idle" settings intents, as Android checks every ten seconds.
            guard scenePhase == .active else { return }
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(10))
                if Task.isCancelled { break }
                if model.storageReady && !model.demo { await model.conversation.watchScheduledSettings() }
            }
        }
        .onAppear {
            #if DEBUG
            // UI checks on the simulator: `-zerus-demo` opens the read-only preview,
            // `-zerus-tab NAME` selects a tab.
            let arguments = ProcessInfo.processInfo.arguments
            if arguments.contains("-zerus-demo") { model.preview() }
            if let index = arguments.firstIndex(of: "-zerus-tab"), index + 1 < arguments.count {
                tab = ["sessions": .sessions, "projects": .projects, "drafts": .drafts, "accounts": .accounts,
                       "machines": .machines][arguments[index + 1]] ?? tab
            }
            // `-zerus-open N` opens the Nth session of the catalog.
            if let index = arguments.firstIndex(of: "-zerus-open"), index + 1 < arguments.count, let row = Int(arguments[index + 1]) {
                Task {
                    try? await Task.sleep(for: .milliseconds(600))
                    if model.sessions.indices.contains(row) { openConversation(model.sessions[row].target) }
                    // `-zerus-attachment` taps the first message attachment; `-zerus-file-link` the first file link.
                    try? await Task.sleep(for: .milliseconds(600))
                    let target = model.conversation.selected?.target
                    if arguments.contains("-zerus-attachment"), let file = model.conversation.events.flatMap(\.attachments).first {
                        model.viewer.open(target, file)
                    }
                    if arguments.contains("-zerus-file-link"), let target, let reference = ComputerFileReference.parse("docs/orders-traffic.md:12") {
                        model.computerFiles.show(target, reference)
                    }
                }
            }
            #endif
        }
        .alert("Zerus", isPresented: Binding(get: { !model.error.isEmpty }, set: { if !$0 { model.error = "" } })) {
            Button("OK", role: .cancel) { model.error = "" }
        } message: {
            Text(model.error)
        }
    }

    /// Opens an exact conversation from another tab.
    private func openConversation(_ target: Target) {
        model.clearSessionScope()
        tab = .sessions
        var path = NavigationPath()
        path.append(SessionRoute(target: target))
        sessionsPath = path
    }

    private func select(_ next: AppTab) {
        if next == tab {
            // Tapping the current tab returns to its root, like other iOS apps.
            switch next {
            case .sessions: sessionsPath = NavigationPath()
            case .projects: projectsPath = NavigationPath()
            default: break
            }
        }
        if next != .sessions { model.clearSessionScope() }
        tab = next
    }
}

/// The common top bar: brand and the Settings button.
struct MainToolbar: ToolbarContent {
    @Environment(AppModel.self) private var model
    @Binding var settings: Bool

    var body: some ToolbarContent {
        ToolbarItem(placement: .topBarLeading) {
            BrandMark().fill(Theme.mint).frame(width: 26, height: 26)
                .accessibilityLabel(model.demo ? "Zerus preview" : "Zerus")
        }
        .withoutGlassBackground()
        if model.demo {
            ToolbarItem(placement: .topBarLeading) { Tag(text: "PREVIEW", color: Theme.amber) }
                .withoutGlassBackground()
        }
        ToolbarItem(placement: .topBarTrailing) {
            Button { model.launch.open() } label: { Image(systemName: "plus") }
                .disabled(model.demo || model.connections.isEmpty)
                .accessibilityLabel("New session")
        }
        ToolbarItem(placement: .topBarTrailing) {
            Button { settings = true } label: { Image(systemName: "gearshape") }
                .accessibilityLabel("Settings")
        }
    }
}

extension View {
    /// iOS 26 shrinks the floating tab bar while reading, leaving room for the composer.
    @ViewBuilder
    func minimizingTabBarOnScroll() -> some View {
        if #available(iOS 26, *) {
            tabBarMinimizeBehavior(.onScrollDown)
        } else {
            self
        }
    }
}
