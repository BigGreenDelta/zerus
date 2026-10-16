import SwiftUI
import ZerusKit

enum AppTab: Hashable {
    case sessions, projects, drafts, accounts, machines
}

struct RootView: View {
    @Environment(AppModel.self) private var model
    @State private var tab: AppTab = .sessions
    @State private var settings = false
    @State private var pairing: PairingRequest?

    var body: some View {
        @Bindable var model = model
        TabView(selection: $tab) {
            Tab("Sessions", systemImage: "rectangle.stack", value: AppTab.sessions) {
                PlaceholderScreen(title: "Sessions", settings: $settings, onPair: { tab = .machines })
            }
            Tab("Projects", systemImage: "folder", value: AppTab.projects) {
                PlaceholderScreen(title: "Projects", settings: $settings, onPair: { tab = .machines })
            }
            Tab("Drafts", systemImage: "square.and.pencil", value: AppTab.drafts) {
                PlaceholderScreen(title: "Drafts", settings: $settings, onPair: { tab = .machines })
            }
            Tab("Accounts", systemImage: "person.crop.circle", value: AppTab.accounts) {
                PlaceholderScreen(title: "Accounts", settings: $settings, onPair: { tab = .machines })
            }
            Tab("Machines", systemImage: "desktopcomputer", value: AppTab.machines) {
                MachinesScreen(settings: $settings, pairing: $pairing)
            }
        }
        .sheet(isPresented: $settings) { SettingsView() }
        .sheet(item: $pairing) { request in PairingSheet(request: request) }
        .onChange(of: model.invitation) { _, invite in
            guard let invite else { return }
            model.invitation = nil
            tab = .machines
            pairing = PairingRequest(server: invite.server, code: invite.code, fromLink: true)
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
}

/// The common top bar: brand, version and the Settings button.
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
            Button { model.refresh(explicit: true) } label: { Image(systemName: "arrow.clockwise") }
                .accessibilityLabel("Refresh")
                .disabled(model.demo)
        }
        ToolbarItem(placement: .topBarTrailing) {
            Button { settings = true } label: { Image(systemName: "gearshape") }
                .accessibilityLabel("Settings")
        }
    }
}

/// Screens that later sub-projects fill in; they already route to pairing.
private struct PlaceholderScreen: View {
    @Environment(AppModel.self) private var model
    let title: String
    @Binding var settings: Bool
    let onPair: () -> Void

    var body: some View {
        NavigationStack {
            ContentUnavailableView {
                Label(title, systemImage: "hammer")
            } description: {
                Text(model.connections.isEmpty && !model.demo
                     ? "Pair a workspace in Machines to browse its sessions."
                     : "\(title) arrive in the next iOS development build.")
            } actions: {
                if model.connections.isEmpty && !model.demo { Button("Open Machines", action: onPair) }
            }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle(title)
            .zerusScreen()
        }
    }
}
