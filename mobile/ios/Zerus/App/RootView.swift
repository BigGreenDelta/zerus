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
                DraftsPlaceholder(settings: $settings)
            } label: { Label("Drafts", systemImage: "square.and.pencil") }
            Tab(value: AppTab.accounts) {
                AccountsScreen(settings: $settings, onMachines: { tab = .machines })
            } label: { Label("Accounts", systemImage: "person.crop.circle") }
            Tab(value: AppTab.machines) {
                MachinesScreen(settings: $settings, pairing: $pairing)
            } label: { Label("Machines", systemImage: "desktopcomputer") }
        }
        .sheet(isPresented: $settings) { SettingsView() }
        .sheet(item: $pairing) { request in PairingSheet(request: request) }
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
                if tab != .drafts { model.refresh() }
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
            #endif
        }
        .alert("Zerus", isPresented: Binding(get: { !model.error.isEmpty }, set: { if !$0 { model.error = "" } })) {
            Button("OK", role: .cancel) { model.error = "" }
        } message: {
            Text(model.error)
        }
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
            Button { settings = true } label: { Image(systemName: "gearshape") }
                .accessibilityLabel("Settings")
        }
    }
}

/// Drafts arrive with the conversation sub-project.
private struct DraftsPlaceholder: View {
    @Binding var settings: Bool

    var body: some View {
        NavigationStack {
            ContentUnavailableView("No saved drafts", systemImage: "square.and.pencil",
                                   description: Text("Unsent messages and deliveries to review appear here."))
                .toolbar { MainToolbar(settings: $settings) }
                .navigationTitle("Drafts")
                .zerusScreen()
        }
    }
}
