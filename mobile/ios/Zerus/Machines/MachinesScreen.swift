import SwiftUI
import ZerusKit

struct PairingRequest: Identifiable, Hashable {
    let id = UUID()
    var server: String
    var code: String
    var fromLink: Bool
}

struct MachinesScreen: View {
    @Environment(AppModel.self) private var model
    @Binding var settings: Bool
    @Binding var pairing: PairingRequest?
    @State private var disconnecting: Connection?

    var body: some View {
        NavigationStack {
            List {
                if model.connections.isEmpty && !model.demo {
                    Section {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Connect your computers").font(.headline)
                            Text("Pair a gateway with a short-lived invitation created by its administrator. Credentials and drafts stay encrypted on this phone.")
                                .font(.subheadline).foregroundStyle(Theme.muted)
                        }
                        .padding(.vertical, 6)
                        Button { pairing = PairingRequest(server: ManagedRelay.current, code: "", fromLink: false) } label: {
                            Label("Pair a workspace", systemImage: "link.badge.plus")
                        }
                        Button { model.preview() } label: { Label("Try demo", systemImage: "play.rectangle") }
                    }
                }
                if model.demo {
                    Section {
                        workspace(name: "Design workstation", connectionId: "demo")
                        Button { model.stopPreview() } label: { Label("Leave preview", systemImage: "xmark.circle") }
                    } header: { Text("Read-only preview") }
                }
                ForEach(model.connections) { connection in
                    Section {
                        workspace(name: connection.displayName, connectionId: connection.id)
                        Button(role: .destructive) { disconnecting = connection } label: {
                            Label("Disconnect", systemImage: "link.badge.minus")
                        }
                        .disabled(model.busy)
                    } header: {
                        Text(connection.displayName)
                    } footer: {
                        Text(connection.endpoint).textSelection(.enabled)
                    }
                }
                if !model.connections.isEmpty {
                    Section {
                        Button { pairing = PairingRequest(server: ManagedRelay.current, code: "", fromLink: false) } label: {
                            Label("Pair another workspace", systemImage: "link.badge.plus")
                        }
                    }
                }
            }
            .refreshable { model.refresh(explicit: true) }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Machines")
            .zerusScreen()
            .overlay(alignment: .top) {
                if model.catalogBusy || model.busy { ProgressView().progressViewStyle(.linear).tint(Theme.mint) }
            }
            .confirmationDialog(disconnecting.map { "Disconnect \($0.displayName)?" } ?? "",
                                isPresented: Binding(get: { disconnecting != nil }, set: { if !$0 { disconnecting = nil } }),
                                titleVisibility: .visible, presenting: disconnecting) { connection in
                Button("Disconnect", role: .destructive) { Task { await model.disconnect(connection) } }
            } message: { _ in
                Text("This removes the gateway credential from this phone. Saved drafts remain available.")
            }
        }
    }

    @ViewBuilder
    private func workspace(name: String, connectionId: String) -> some View {
        let groups = MachineCatalog.groups(connectionId, model.machines)
        if groups.isEmpty {
            Text(model.catalogBusy ? "Loading computers…" : "No computers reported yet.").foregroundStyle(Theme.muted)
        }
        ForEach(groups, id: \.root.machine.id) { group in
            MachineRow(row: group.root, indented: false)
            ForEach(group.children, id: \.machine.id) { child in MachineRow(row: child, indented: true) }
        }
    }
}

private struct MachineRow: View {
    let row: MachineTreeRow
    let indented: Bool

    var body: some View {
        let machine = row.machine
        HStack(spacing: 12) {
            if indented { Image(systemName: "arrow.turn.down.right").foregroundStyle(Theme.muted).font(.caption) }
            Image(systemName: "desktopcomputer")
                .foregroundStyle(MachineTint.color(machine.key))
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 3) {
                Text(machine.name).lineLimit(2)
                Text(caption).font(.caption).foregroundStyle(Theme.muted).lineLimit(2)
            }
            Spacer(minLength: 0)
            Circle().fill(machine.online && !machine.lastKnown ? Theme.mint : Theme.muted.opacity(0.5))
                .frame(width: 8, height: 8)
                .accessibilityHidden(true)
        }
        .accessibilityElement(children: .combine)
    }

    private var caption: String {
        var parts = [MachineCatalog.status(row.machine)]
        if !row.gatewayName.isEmpty { parts.append("Through \(row.gatewayName)") }
        parts.append(row.machine.hgsVersion.isEmpty ? "version unavailable" : "hgs \(row.machine.hgsVersion)")
        return parts.joined(separator: " · ")
    }
}

/// Machine label colors use the desktop palette and tonal blends.
enum MachineTint {
    static func color(_ key: MachineKey, overrides: [MachineColorOverride] = []) -> Color {
        let hex = MachineColors.color(overrides, key)
        return Color(hex: (try? MachineColors.foreground(hex, dark: true)) ?? 0x9AACB2)
    }
    static func background(_ key: MachineKey, overrides: [MachineColorOverride] = []) -> Color {
        let hex = MachineColors.color(overrides, key)
        return Color(hex: (try? MachineColors.background(hex, dark: true)) ?? 0x1A2225)
    }
}
