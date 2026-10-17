import Foundation
import Observation
import SwiftUI
import ZerusKit

/// Provider management is pinned to an authorized processing computer, independent of open conversations.
/// Each mutation's identity and sealed envelope are durable before its only submission.
@MainActor
@Observable
final class SpeechProviders {
    @ObservationIgnored private unowned let app: AppModel
    @ObservationIgnored private var store: SealedFileStore?
    @ObservationIgnored private var saving: Task<Void, Error>?
    private(set) var ready = false
    private(set) var host: SpeechConfigHost?
    private(set) var configuration: SpeechConfiguration?
    private(set) var loading = false
    var notice = ""
    private(set) var pending: [SpeechConfigOperation] = []
    private(set) var active: Set<String> = []
    private(set) var result: SpeechConfigResult?
    var busy: Bool { !active.isEmpty }
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var refreshTask: Task<Void, Never>?

    init(app: AppModel) { self.app = app }

    func open(_ sealed: SealedFileStore) async {
        store = sealed
        do {
            pending = try await Task.detached { try SpeechConfigProtocol.restored(sealed.read("speech_config_operations", as: [SpeechConfigOperation].self) ?? []) }.value
            ready = true
        } catch {
            notice = "Private configuration recovery could not be opened."
        }
    }

    private func persist() async throws {
        guard let store else { throw ZerusError.invalid("Private storage is not ready.") }
        let snapshot = pending, previous = saving
        let task = Task.detached(priority: .userInitiated) {
            _ = await previous?.result
            try store.write("speech_config_operations", snapshot)
        }
        saving = task
        try await task.value
    }

    var hosts: [SpeechConfigHost] {
        app.machines.filter { machine in app.connections.contains { $0.id == machine.connectionId } }
            .map { SpeechConfigHost(key: $0.key, machineId: $0.machineId, name: $0.name) }
    }

    func reason(_ captured: SpeechConfigHost) -> String {
        if app.demo || !app.storageReady { return "Connect a computer to manage its speech providers." }
        if !app.connections.contains(where: { $0.id == captured.key.connectionId }) { return "The workspace is disconnected." }
        if !app.machines.contains(where: { $0.key == captured.key && $0.machineId == captured.machineId && $0.online && !$0.lastKnown }) {
            return "The selected computer is unavailable or its identity changed."
        }
        if !AttachmentPolicy.validId(captured.machineId) { return "Refresh to verify this computer's identity." }
        if !(app.relayOperations[captured.key.connectionId] ?? []).isSuperset(of: SpeechConfigProtocol.operations) {
            return "Update this gateway to manage speech providers."
        }
        if !(app.machineOperations[captured.key] ?? []).isSuperset(of: SpeechConfigProtocol.operations) {
            return "Update Zerus on this computer to manage speech providers."
        }
        return ""
    }

    func select(_ selected: SpeechConfigHost) {
        if host == selected && configuration != nil { return }
        host = selected
        configuration = nil
        result = nil
        notice = ""
        refresh()
    }

    func refresh() {
        guard let captured = host, ready else { return }
        generation += 1
        let version = generation
        refreshTask?.cancel()
        configuration = nil
        loading = false
        let reason = reason(captured)
        guard reason.isEmpty, let connection = app.connections.first(where: { $0.id == captured.key.connectionId }) else {
            notice = reason
            return
        }
        loading = true
        let client = app.client
        refreshTask = Task { [weak self] in
            let raw = try? await client.speech(connection, host: captured.key, operation: "speech_config", payload: [:],
                                               fallback: "Could not load this computer's speech configuration.")
            guard let self, generation == version else { return }
            loading = false
            guard !Task.isCancelled, host == captured else { return }
            if let raw, let parsed = try? SpeechConfigProtocol.parse(raw, host: captured), self.reason(captured).isEmpty {
                configuration = parsed
                notice = ""
            } else {
                notice = "Could not load this computer's speech configuration. Refresh to try again."
            }
        }
    }

    func canMutate(_ captured: SpeechConfigHost) -> Bool {
        ready && host == captured && !loading && reason(captured).isEmpty && configuration?.machineId == captured.machineId &&
            !pending.contains { $0.host.key == captured.key && $0.host.machineId == captured.machineId }
    }

    func useForNewRecordings(_ profile: SpeechConfigProfile) -> Bool {
        guard let captured = host, canMutate(captured), profile.available, configuration?.profiles.contains(profile) == true else { return false }
        return app.voice.chooseDefault(SpeechSelection(host: MachineKey(connectionId: captured.key.connectionId, computerId: captured.machineId),
                                                       profileId: profile.id, profileRevision: profile.revision))
    }

    @discardableResult
    func apply(_ operation: SpeechConfigOperation) -> Bool {
        guard let config = configuration, canMutate(operation.host), pending.count < 32, SpeechConfigProtocol.mutations.contains(operation.operation) else { return false }
        let body = operation.payload
        if operation.operation == "speech_profile_test" {
            guard config.profiles.contains(where: { $0.id == body.string("profile_id") && $0.revision == body.int64("expected_profile_revision") }) else { return false }
        } else if body.int64("expected_revision") != config.revision || !body.has("expected_revision") {
            return false
        }
        pending.append(operation)
        active.insert(operation.id)
        result = nil
        notice = ""
        let client = app.client
        Task {
            var submitted = false, running = false
            do {
                try await persist() // The exact UUID and sealed envelope are durable before the only mutation.
                guard reason(operation.host).isEmpty, let connection = app.connections.first(where: { $0.id == operation.host.key.connectionId }) else {
                    throw ZerusError.invalid(reason(operation.host))
                }
                submitted = true
                let raw = try await client.speech(connection, host: operation.host.key, operation: operation.operation, payload: body, id: operation.id,
                                                  fallback: "The computer has not confirmed this configuration operation. Check its status.")
                running = try await accept(operation, raw).state == "running"
            } catch {
                if !submitted {
                    pending.removeAll { $0.id == operation.id }
                    try? await persist()
                }
                if host == operation.host {
                    notice = submitted ? "The outcome is unconfirmed. Check this operation's status; it has not been repeated."
                        : "The operation was not sent. Refresh the computer and try again."
                }
            }
            active.remove(operation.id)
            if running, pending.contains(where: { $0.id == operation.id }) { checkStatus(operation) }
        }
        return true
    }

    private func accept(_ operation: SpeechConfigOperation, _ raw: JSONObject) async throws -> SpeechConfigResult {
        let receipt = try SpeechConfigProtocol.result(raw, operation)
        if receipt.terminal && receipt.errorCode != "operation_not_found" {
            pending.removeAll { $0.id == operation.id }
            try await persist()
        }
        if host == operation.host {
            result = receipt
            switch receipt.state {
            case "completed": notice = operation.operation == "speech_profile_test" ? "Provider test succeeded." : "Speech configuration saved."
            case "failed":
                switch receipt.errorCode {
                case "revision_conflict": notice = "The configuration changed on another device. Refresh and review your edits before saving again."
                case "invalid_credential": notice = "The API key could not be accepted. Refresh and enter it again."
                case "operation_not_found": notice = "The computer has not seen this operation yet. Its original request may still arrive; check status again."
                default: notice = receipt.error.nonEmpty ?? "The computer could not complete this operation."
                }
            case "running": notice = "Testing the saved provider…"
            default: notice = "The outcome is unconfirmed. Check its status; it has not been repeated."
            }
            if receipt.state == "completed" {
                let saved = notice
                refresh()
                // Voice selection and any retained preview remain unchanged; only discovery refreshes.
                app.voice.refreshProfiles(app.voice.draft?.target ?? app.conversation.selected?.target)
                notice = saved
            }
        }
        return receipt
    }

    func checkStatus(_ operation: SpeechConfigOperation) {
        guard ready, !active.contains(operation.id), pending.contains(where: { $0.id == operation.id }), reason(operation.host).isEmpty,
              let connection = app.connections.first(where: { $0.id == operation.host.key.connectionId }) else { return }
        active.insert(operation.id)
        let client = app.client
        Task {
            defer { active.remove(operation.id) }
            do {
                for _ in 0..<60 {
                    let receipt = try await accept(operation, client.speech(connection, host: operation.host.key, operation: "speech_config_status",
                                                                             payload: ["operation_id": .string(operation.id), "expected_machine_id": .string(operation.host.machineId)],
                                                                             fallback: "Status could not be confirmed."))
                    guard pending.contains(where: { $0.id == operation.id }), receipt.state == "running" else { return }
                    try await Task.sleep(for: .seconds(1))
                }
            } catch {
                if host == operation.host { notice = "Status could not be confirmed. The original operation has not been repeated." }
            }
        }
    }
}

struct SpeechProvidersView: View {
    @Environment(AppModel.self) private var app
    @Environment(\.openURL) private var openURL
    let defaultSelection: SpeechSelection
    @State private var editor: EditorTarget?
    @State private var removal: SpeechConfigProfile?
    @State private var actionError = ""

    struct EditorTarget: Identifiable {
        let configuration: SpeechConfiguration
        let profile: SpeechConfigProfile
        var id: String { profile.id }
    }

    var body: some View {
        let manager = app.speechProviders!
        let hosts = manager.hosts
        let selected = manager.host
        List {
            Section {
                Picker("Processing computer", selection: Binding(get: { selected?.key }, set: { key in
                    guard let host = hosts.first(where: { $0.key == key }) else { return }
                    editor = nil
                    removal = nil
                    actionError = ""
                    manager.select(host)
                })) {
                    if selected == nil { Text("Choose processing computer").tag(MachineKey?.none) }
                    ForEach(hosts, id: \.key) { Text($0.name).tag(Optional($0.key)) }
                }
                .disabled(!manager.ready)
                Link(SpeechConfigProtocol.documentation().label, destination: URL(string: SpeechConfigProtocol.documentation().url)!)
                if hosts.isEmpty { Text("Connect a computer to manage its speech providers.").foregroundStyle(Theme.muted) }
                if let selected {
                    if let reason = manager.reason(selected).nonEmpty { Text(reason).font(.footnote).foregroundStyle(Theme.amber) }
                    Button("Refresh configuration") { manager.refresh() }.disabled(!manager.ready || manager.loading || !manager.reason(selected).isEmpty)
                }
                if manager.loading { ProgressView().frame(maxWidth: .infinity) }
                if !manager.notice.isEmpty { Text(manager.notice).font(.footnote).foregroundStyle(Theme.muted) }
                if !actionError.isEmpty { Text(actionError).font(.footnote).foregroundStyle(Theme.amber) }
            } footer: {
                Text("Providers are saved on the processing computer. Changing them does not select a different provider for a saved recording.")
            }
            ForEach(manager.pending.filter { $0.host == selected }, id: \.id) { operation in
                Section("Unconfirmed configuration operation") {
                    Text("This operation has not been repeated.").font(.footnote)
                    Button("Check operation status") { manager.checkStatus(operation) }
                        .disabled(manager.active.contains(operation.id) || !manager.reason(operation.host).isEmpty)
                }
            }
            if let selected, let config = manager.configuration {
                Section {
                    Text(SpeechConfigProtocol.readiness(selected, config, loading: manager.loading)).font(.footnote).foregroundStyle(Theme.muted)
                }
                ForEach(config.profiles, id: \.id) { profile in
                    Section { profileRow(selected, config, profile) }
                }
                Section {
                    Button("Add provider") {
                        guard let adapter = config.adapters.first(where: { ["gigaam", "openai_http"].contains($0.id) }) else { return }
                        editor = EditorTarget(configuration: config, profile: SpeechConfigProfile(id: UUID().uuidString.lowercased(), revision: 0, name: "",
                                                                                              adapter: adapter.id, model: adapter.models.first ?? "", language: "ru",
                                                                                              device: adapter.devices.first ?? ""))
                    }
                    .disabled(!manager.canMutate(selected) || config.profiles.count >= 32 || !config.adapters.contains { ["gigaam", "openai_http"].contains($0.id) })
                } footer: {
                    Text("Test sends a short synthetic recording from this computer to the saved provider. It does not use your microphone or saved recordings.")
                }
            }
        }
        .navigationTitle("Speech providers")
        .navigationBarTitleDisplayMode(.inline)
        .zerusScreen()
        .task(id: manager.ready) {
            guard manager.ready else { return }
            if let host = hosts.first(where: { $0.key.connectionId == defaultSelection.host?.connectionId && $0.machineId == defaultSelection.host?.computerId }) {
                manager.select(host)
            }
            if manager.host != nil && manager.configuration == nil { manager.refresh() }
        }
        .sheet(item: $editor) { target in
            if let selected { SpeechProviderEditor(host: selected, initial: target.configuration, original: target.profile) }
        }
        .alert("Remove speech provider?", isPresented: Binding(get: { removal != nil }, set: { if !$0 { removal = nil } }), presenting: removal) { profile in
            Button("Remove provider", role: .destructive) {
                removal = nil
                guard let selected, let config = manager.configuration, manager.apply(SpeechConfigProtocol.remove(selected, config, profile)) else {
                    actionError = "The configuration changed. Refresh and confirm removal again."
                    return
                }
            }
            Button("Keep provider", role: .cancel) { removal = nil }
        } message: { profile in
            Text("Remove \(profile.name) from \(selected?.name ?? "this computer")? Saved recordings and voice history are kept. Clients using this provider will need an explicit new selection.")
        }
    }

    @ViewBuilder
    private func profileRow(_ selected: SpeechConfigHost, _ config: SpeechConfiguration, _ profile: SpeechConfigProfile) -> some View {
        let manager = app.speechProviders!
        let isDefault = app.voice.selection == SpeechSelection(host: MachineKey(connectionId: selected.key.connectionId, computerId: selected.machineId),
                                                               profileId: profile.id, profileRevision: profile.revision)
        VStack(alignment: .leading, spacing: 3) {
            Text(profile.name).font(.headline)
            Text("\(SpeechProviderEditor.adapterName(profile.adapter)) (\(profile.model))").font(.footnote).foregroundStyle(Theme.muted)
            if profile.adapter == "openai_http" { Text(profile.baseUrl).font(.footnote).foregroundStyle(Theme.muted) }
            Text(profile.available ? "Available" : (profile.reason.nonEmpty ?? "Unavailable")).font(.footnote)
                .foregroundStyle(profile.available ? Theme.mint : Theme.amber)
            if profile.credentialConfigured { Text("API key configured").font(.caption).foregroundStyle(Theme.muted) }
            if isDefault { Text("Used for new recordings").font(.caption).foregroundStyle(Theme.mint) }
        }
        if !isDefault {
            Button("Use for new recordings") { if !manager.useForNewRecordings(profile) { actionError = "Refresh and review this provider before selecting it." } }
                .disabled(!manager.canMutate(selected) || !profile.available || app.voice.recording || app.voice.savingRecording)
        }
        Button("Edit") { editor = EditorTarget(configuration: config, profile: profile) }.disabled(!manager.canMutate(selected))
        Button("Test") { if !manager.apply(SpeechConfigProtocol.test(selected, profile)) { actionError = "Refresh and review this provider before testing." } }
            .disabled(!manager.canMutate(selected) || !config.serviceAvailable)
        Button("Remove", role: .destructive) { removal = profile }.disabled(!manager.canMutate(selected))
    }
}

/// Edits one provider. An API key stays only in memory until it is sealed for the processing computer.
struct SpeechProviderEditor: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    let host: SpeechConfigHost
    let initial: SpeechConfiguration
    let original: SpeechConfigProfile
    @State private var baseline: SpeechConfiguration?
    @State private var profile: SpeechConfigProfile?
    @State private var credentialMode = "keep"
    @State private var secret = ""
    @State private var error = ""
    @State private var operationId: String?

    static func adapterName(_ id: String) -> String {
        switch id {
        case "gigaam": "Local GigaAM"
        case "openai_http": "OpenAI-compatible server"
        default: id
        }
    }

    var body: some View {
        let manager = app.speechProviders!
        let base = baseline ?? initial
        let edited = Binding(get: { profile ?? original }, set: { profile = $0 })
        let current = profile ?? original
        let pending = manager.pending.first { $0.id == operationId }
        let editable = pending == nil && manager.canMutate(host)
        let latest = manager.configuration
        NavigationStack {
            Form {
                Section {
                    Text("Processing computer: \(host.name)").font(.footnote)
                    Text(SpeechConfigProtocol.readiness(host, latest, loading: manager.loading)).font(.footnote).foregroundStyle(Theme.muted)
                    Text("Provider ID: \(current.id)").font(.caption).foregroundStyle(Theme.muted)
                    TextField("Name", text: edited.name).disabled(!editable)
                    Picker("Type", selection: Binding(get: { current.adapter }, set: { id in
                        guard let adapter = base.adapters.first(where: { $0.id == id }) else { return }
                        var next = current
                        next.adapter = id
                        next.model = adapter.models.first ?? ""
                        next.device = adapter.devices.first ?? ""
                        next.baseUrl = ""
                        next.allowInsecureHttp = false
                        profile = next
                        secret = ""
                        credentialMode = id == "gigaam" ? "remove" : "keep"
                    })) {
                        ForEach(base.adapters.filter { ["gigaam", "openai_http"].contains($0.id) }, id: \.id) { Text(Self.adapterName($0.id)).tag($0.id) }
                    }
                    .disabled(!editable)
                    Link(SpeechConfigProtocol.documentation(current.adapter).label, destination: URL(string: SpeechConfigProtocol.documentation(current.adapter).url)!)
                }
                if current.adapter == "gigaam" {
                    let adapter = base.adapters.first { $0.id == "gigaam" }
                    Section {
                        Picker("Model", selection: edited.model) { ForEach(adapter?.models ?? [], id: \.self) { Text($0).tag($0) } }.disabled(!editable)
                        Picker("Device", selection: edited.device) { ForEach(adapter?.devices ?? [], id: \.self) { Text($0).tag($0) } }.disabled(!editable)
                    } footer: {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("Local inference runs on \(host.name). Prepare its Python/PyTorch speech service before testing this profile.")
                            Text("v3_e2e_rnnt is the GigaAM v3 end-to-end model used by the standard Russian setup.")
                            Text(current.device == "cpu" ? "CPU runs without a GPU. It is the baseline setup choice."
                                 : current.device.hasPrefix("cuda") ? "CUDA requires an NVIDIA GPU, compatible driver and PyTorch build on \(host.name). A numbered device selects that GPU index."
                                 : current.device == "mps" ? "MPS requires a compatible Apple GPU and installed runtime. Verify this model with Test before using it."
                                 : "Choose a device prepared on \(host.name), then verify the saved profile with Test.")
                            Text("Saving does not install packages or download model weights.")
                        }
                    }
                } else if current.adapter == "openai_http" {
                    Section {
                        TextField("Server base URL", text: edited.baseUrl, prompt: Text(verbatim: "https://speech.example.test/v1"))
                            .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled().disabled(!editable)
                        TextField("Model", text: edited.model).textInputAutocapitalization(.never).autocorrectionDisabled().disabled(!editable)
                        Toggle("Allow HTTP on local/private networks", isOn: edited.allowInsecureHttp).disabled(!editable)
                        Picker("API key", selection: Binding(get: { credentialMode }, set: { credentialMode = $0; secret = "" })) {
                            Text(original.credentialConfigured ? "Keep saved key" : "No key").tag("keep")
                            Text("Enter new key").tag("replace")
                            Text("Remove saved key").tag("remove")
                        }
                        .disabled(!editable)
                        if credentialMode == "replace" { SecureField("New API key", text: $secret).disabled(!editable) }
                    } footer: {
                        Text("\(host.name) sends WAV audio to this server's /audio/transcriptions endpoint. The key is encrypted for \(host.name) before upload. Saved keys are never returned to this phone.")
                    }
                }
                Section {
                    TextField("Language", text: edited.language).textInputAutocapitalization(.never).autocorrectionDisabled().disabled(!editable)
                } footer: {
                    if current.adapter == "gigaam" { Text("Use ru for Russian speech.") }
                }
                Section {
                    if let latest, latest.machineId == base.machineId, latest.revision != base.revision {
                        Text("The configuration has changed. Review your edits against revision \(latest.revision) before saving.").font(.footnote)
                        Button("Use refreshed revision") {
                            baseline = latest
                            error = ""
                        }
                        .disabled(pending != nil)
                    }
                    if !error.isEmpty { Text(error).font(.footnote).foregroundStyle(Theme.amber) }
                    if let pending {
                        Text(manager.notice.nonEmpty ?? "Waiting for the computer. This operation will not be repeated.").font(.footnote)
                        Button("Check operation status") { manager.checkStatus(pending) }.disabled(manager.active.contains(pending.id))
                    } else {
                        Button("Refresh configuration") { manager.refresh() }.disabled(manager.loading)
                    }
                    if manager.loading || pending.map({ manager.active.contains($0.id) }) == true { ProgressView().frame(maxWidth: .infinity) }
                }
            }
            .navigationTitle(original.revision == 0 ? "Add speech provider" : "Edit speech provider")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { close() } }
                ToolbarItem(placement: .confirmationAction) { Button("Save provider") { save(base, current) }.disabled(!editable) }
            }
            .zerusScreen()
        }
        .interactiveDismissDisabled(pending != nil)
        .onChange(of: manager.result) { _, result in
            guard let result, result.id == operationId else { return }
            if result.state == "completed" { close() } else if result.state == "failed" { error = manager.notice }
        }
    }

    private func save(_ base: SpeechConfiguration, _ current: SpeechConfigProfile) {
        do {
            let operation = try SpeechConfigProtocol.upsert(host, base, current, credentialMode: credentialMode, secret: secret)
            secret = "" // Only the sealed envelope may survive transport or recovery.
            if app.speechProviders.apply(operation) {
                operationId = operation.id
                error = ""
            } else {
                error = "The configuration changed. Refresh and review your edits before saving."
            }
        } catch {
            self.error = (error as? ZerusError)?.message ?? "Check the provider fields."
        }
    }

    private func close() {
        secret = ""
        dismiss()
    }
}
