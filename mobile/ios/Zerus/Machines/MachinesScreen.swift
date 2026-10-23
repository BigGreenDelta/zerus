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
    @State private var naming: Machine?
    @State private var coloring: Machine?

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
                        MachineTree(connectionId: "demo", naming: $naming, coloring: $coloring)
                        Button { model.stopPreview() } label: { Label("Exit preview", systemImage: "xmark.circle") }
                    } header: { Text("Read-only preview") }
                }
                ForEach(model.connections) { connection in
                    Section {
                        MachineTree(connectionId: connection.id, naming: $naming, coloring: $coloring)
                        if model.pendingEncryptedPairings.contains(connection.id) {
                            Button("Resume pairing") { Task { await model.resumeEncryptedPair(connection) } }.disabled(model.busy)
                        }
                        Button(role: .destructive) { disconnecting = connection } label: {
                            Label(connection.protocolVersion == 2 ? "Forget encrypted connection" : "Disconnect workspace", systemImage: "link.badge.minus")
                        }
                        .disabled(model.busy)
                    } header: {
                        Text(connection.displayName).textCase(nil)
                    } footer: {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(connection.endpoint).textSelection(.enabled)
                            Text(connection.protocolVersion == 2 ? "Encrypted workspace. Refresh to check enrollment and connected computers." : "Legacy connection. This relay can read relayed content.")
                            if let status = model.notifications.pushStatus[connection.id] { Text(status) }
                        }
                    }
                }
                if !model.connections.isEmpty {
                    Section {
                        Button { pairing = PairingRequest(server: ManagedRelay.current, code: "", fromLink: false) } label: {
                            Label("Pair another workspace", systemImage: "link.badge.plus")
                        }
                    }
                    NotificationSettingsSection()
                    MobileDeliverySection()
                }
            }
            .refreshable { model.refresh(explicit: true) }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Machines")
            .zerusScreen()
            .overlay(alignment: .top) {
                if model.catalogProgress || model.busy { ProgressView().progressViewStyle(.linear).tint(Theme.mint) }
            }
            .confirmationDialog(disconnecting.map { "\($0.protocolVersion == 2 ? "Forget" : "Disconnect") \($0.displayName)?" } ?? "",
                                isPresented: Binding(get: { disconnecting != nil }, set: { if !$0 { disconnecting = nil } }),
                                titleVisibility: .visible, presenting: disconnecting) { connection in
                Button(connection.protocolVersion == 2 ? "Forget connection" : "Disconnect", role: .destructive) { Task { await model.disconnect(connection) } }
            } message: { connection in
                Text(connection.protocolVersion == 2 ? "This erases this phone's connection keys. Saved drafts remain available. To revoke this phone across the workspace, remove it from an enrolled computer." : "This removes the gateway credential from this phone. Saved drafts remain available.")
            }
            .sheet(item: $naming) { machine in MachineNameSheet(machine: machine) }
            .sheet(item: $coloring) { machine in MachineColorSheet(machine: machine) }
        }
    }
}

private struct MachineTree: View {
    @Environment(AppModel.self) private var model
    let connectionId: String
    @Binding var naming: Machine?
    @Binding var coloring: Machine?

    var body: some View {
        let groups = MachineCatalog.groups(connectionId, model.machines)
        if groups.isEmpty {
            Text(model.catalogBusy ? "Loading computers…" : "No computers reported yet.").foregroundStyle(Theme.muted)
        }
        ForEach(groups, id: \.root.machine.id) { group in
            MachineRow(row: group.root, child: false, naming: $naming, coloring: $coloring)
            ForEach(group.children, id: \.machine.id) { child in
                MachineRow(row: child, child: true, naming: $naming, coloring: $coloring)
            }
        }
    }
}

private struct MachineRow: View {
    @Environment(AppModel.self) private var model
    let row: MachineTreeRow
    let child: Bool
    @Binding var naming: Machine?
    @Binding var coloring: Machine?

    var body: some View {
        let machine = row.machine
        HStack(alignment: .top, spacing: 8) {
            if child { Image(systemName: "arrow.turn.down.right").font(.caption).foregroundStyle(Theme.muted).padding(.top, 4) }
            VStack(alignment: .leading, spacing: 4) {
                MachineLabel(name: machine.name, colorHex: model.machineColor(machine.key))
                if !caption.isEmpty { Text(caption).font(.footnote).foregroundStyle(Theme.muted) }
                Text(MachineCatalog.status(machine)).font(.footnote)
                    .foregroundStyle(machine.online && !machine.lastKnown ? Theme.mint : Theme.muted)
            }
            Spacer(minLength: 0)
            if !model.demo {
                Menu {
                    Button { naming = machine } label: { Label("Rename machine", systemImage: "pencil") }
                    Button { coloring = machine } label: { Label("Label color", systemImage: "paintpalette") }
                } label: {
                    Image(systemName: "ellipsis").frame(width: 44, height: 44)
                }
                .disabled(!model.storageReady)
                .accessibilityLabel("Actions for \(machine.name)")
            }
        }
        .padding(.leading, child ? 12 : 0)
    }

    private var caption: String {
        switch row.machine.route {
        case .direct: "Gateway"
        case .unknown: ""
        case .via: row.gatewayName.isEmpty ? "Through another computer" : "Through \(row.gatewayName)"
        }
    }
}

/// A private phone label; it never changes the hostname, SSH settings or session identity.
private struct MachineNameSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let machine: Machine
    @State private var name = ""
    @State private var prepared = false

    var body: some View {
        let saving = model.machineNameSaving.contains(machine.key)
        let valid = (try? MachineNames.checked(name)) != nil
        NavigationStack {
            Form {
                Section {
                    TextField("Machine name", text: $name).disabled(saving).submitLabel(.done).onSubmit(save)
                } footer: {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("This private label applies only to this machine in this workspace. Its hostname, SSH settings and session identity stay unchanged.")
                        Text("Reported name: \(machine.nativeName)")
                    }
                }
                if !model.machineAlias(machine.key).isEmpty {
                    Section {
                        Button("Reset to reported name") {
                            Task { if await model.renameMachine(machine.key, nil) { dismiss() } }
                        }
                        .disabled(saving)
                    }
                }
            }
            .navigationTitle("Name on this phone")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(saving) }
                ToolbarItem(placement: .confirmationAction) { Button(saving ? "Saving…" : "Save", action: save).disabled(!valid || saving) }
            }
            .interactiveDismissDisabled(saving)
            .onAppear {
                guard !prepared else { return }
                prepared = true
                name = machine.name
            }
        }
        .presentationDetents([.medium])
    }

    private func save() {
        guard (try? MachineNames.checked(name)) != nil else { return }
        Task { if await model.renameMachine(machine.key, name) { dismiss() } }
    }
}

/// Palette and automatic reset edit only this phone's exact machine profile.
private struct MachineColorSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let machine: Machine
    @State private var choice: String?
    @State private var prepared = false

    var body: some View {
        let saving = model.machineColorSaving.contains(machine.key)
        let preview = choice ?? MachineColors.automatic(machine.key)
        NavigationStack {
            Form {
                Section {
                    MachineLabel(name: model.machineName(machine.key, fallback: machine.name), colorHex: preview)
                    Text("Selected: \(choice == nil ? "Automatic" : (MachineColors.palette.firstIndex(of: choice!).map { MachineColors.names[$0] } ?? "Custom")) (\(preview))")
                        .font(.footnote)
                } footer: {
                    Text("Saved only on this phone for this machine in this workspace. You can change it while offline.")
                }
                Section {
                    LazyVGrid(columns: Array(repeating: GridItem(.fixed(48)), count: 4), alignment: .leading, spacing: 8) {
                        ForEach(MachineColors.palette, id: \.self) { hex in
                            Button { choice = hex } label: {
                                Circle().fill(Color(projectHex: hex) ?? .gray).frame(width: 32, height: 32)
                                    .overlay { if choice == hex { Image(systemName: "checkmark").foregroundStyle(.black) } }
                                    .frame(width: 48, height: 48)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel("\(MachineColors.names[MachineColors.palette.firstIndex(of: hex)!]), \(hex)")
                            .accessibilityAddTraits(choice == hex ? .isSelected : [])
                        }
                    }
                    Button { choice = nil } label: {
                        HStack { Text("Automatic (reset)"); Spacer(); if choice == nil { Image(systemName: "checkmark").foregroundStyle(Theme.mint) } }
                    }
                }
                .disabled(saving)
            }
            .navigationTitle("Machine label color")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() }.disabled(saving) }
                ToolbarItem(placement: .confirmationAction) {
                    Button(saving ? "Saving…" : "Save") { Task { if await model.setMachineColor(machine.key, choice) { dismiss() } } }.disabled(saving)
                }
            }
            .interactiveDismissDisabled(saving)
            .onAppear {
                guard !prepared else { return }
                prepared = true
                choice = model.machineColorOverride(machine.key)
            }
        }
    }
}

/// Which session updates can alert this phone, and whether iOS lets Zerus show them.
private struct NotificationSettingsSection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let notifications = model.notifications!
        let preferences = notifications.preferences
        Section {
            Toggle(isOn: Binding(get: { preferences.master }, set: { notifications.setMaster($0) })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Session alerts")
                    Text("Choose which session updates can alert you.").font(.caption).foregroundStyle(Theme.muted)
                }
            }
            .disabled(!notifications.ready)
            ForEach([(SessionAlertKind.input, "Input and approvals"), (.optional, "Optional questions"), (.error, "Errors"), (.finished, "Turn finished")],
                    id: \.0) { kind, caption in
                Toggle(caption, isOn: Binding(get: { preferences.allows(kind) || (!preferences.master && enabled(preferences, kind)) },
                                              set: { notifications.set(kind, $0) }))
                    .padding(.leading, 12)
                    .disabled(!preferences.master || !notifications.ready)
            }
            switch notifications.authorization {
            case .notDetermined:
                Button("Allow notifications") { Task { await notifications.requestAuthorization() } }
            case .denied:
                Button("Allow notifications in Settings") {
                    if let url = URL(string: UIApplication.openNotificationSettingsURLString) { UIApplication.shared.open(url) }
                }
            default:
                EmptyView()
            }
        } header: {
            Text("Notifications")
        } footer: {
            Text("Alerts show the session title and this phone's name for the computer. A push only wakes Zerus; it never carries conversation text, and Zerus reads the current state before showing a card.")
        }
        .task { await notifications.refreshAuthorization() }
    }

    private func enabled(_ preferences: NotificationPreferences, _ kind: SessionAlertKind) -> Bool {
        switch kind {
        case .input: preferences.input
        case .error: preferences.errors
        case .finished: preferences.finished
        case .optional: preferences.optional
        }
    }
}

/// One shared timing per workspace; the gateway computer's hgs stores it for every swarm desktop.
private struct MobileDeliverySection: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        let conversation = model.conversation!
        Section {
            ForEach(model.connections) { connection in
                let gateway = conversation.mobileDeliveryGateway(connection.id)
                let action = conversation.mobileDeliveryAction(connection.id)
                let reason = conversation.mobileDeliveryReason(connection.id)
                let now = Clock.milliseconds()
                let selected = MobileDelivery.shown(reported: gateway?.mobileDelivery ?? "", action: action, now: now)
                VStack(alignment: .leading, spacing: 6) {
                    if model.connections.count > 1 { Text(connection.displayName).font(.subheadline.weight(.semibold)) }
                    ForEach(MobileDelivery.values, id: \.self) { value in
                        Button { conversation.setMobileDelivery(connection.id, value) } label: {
                            HStack {
                                Image(systemName: value == selected ? "largecircle.fill.circle" : "circle")
                                    .foregroundStyle(value == selected ? Theme.mint : Theme.muted)
                                Text(MobileDelivery.label(value)).foregroundStyle(.primary)
                                Spacer()
                            }
                            .frame(minHeight: 36)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .disabled(!reason.isEmpty)
                        .accessibilityAddTraits(value == selected ? [.isSelected] : [])
                    }
                    Text(MobileDelivery.status(gateway: gateway?.name ?? "", reported: gateway?.mobileDelivery ?? "", action: action, reason: reason, now: now))
                        .font(.caption)
                        .foregroundStyle(Theme.muted)
                }
            }
            if !conversation.mobileDeliveryError.isEmpty {
                HStack {
                    Text(conversation.mobileDeliveryError).font(.caption).foregroundStyle(Theme.amber)
                    Spacer()
                    Button("Dismiss") { conversation.showMobileDeliveryError("") }.font(.caption)
                }
            }
        } header: {
            Text("When I'm not at a computer, send notifications to my phone")
        }
    }
}
