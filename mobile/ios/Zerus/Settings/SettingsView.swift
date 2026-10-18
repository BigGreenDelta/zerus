import SwiftUI
import ZerusKit

struct SettingsView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    private let build = BuildInfo.current

    var body: some View {
        NavigationStack {
            List {
                Section("About Zerus") {
                    HStack(alignment: .firstTextBaseline) {
                        Text("Version")
                        Spacer()
                        Text(build.displayVersion).foregroundStyle(Theme.muted).textSelection(.enabled)
                    }
                    Text(build.detail).font(.caption).foregroundStyle(Theme.muted).textSelection(.enabled)
                    Link(destination: URL(string: "https://github.com/ufna/zerus")!) {
                        Label("ufna/zerus on GitHub", systemImage: "chevron.left.forwardslash.chevron.right")
                    }
                }
                Section("Computers") {
                    if model.machines.isEmpty {
                        Text("No connected computers").foregroundStyle(Theme.muted)
                    }
                    ForEach(model.machines, id: \.key) { machine in
                        HStack {
                            Text(machine.name).lineLimit(1)
                            Spacer()
                            Text(cliVersion(machine)).font(.caption).foregroundStyle(Theme.muted)
                        }
                    }
                }
                Section("Privacy") {
                    Text("Zerus for iOS sends no analytics or crash reports of its own. Session content travels only between this phone, your gateway and your computers.")
                        .font(.footnote).foregroundStyle(Theme.muted)
                }
                Section {
                    NavigationLink("Voice messages") { VoiceSettingsSheet(target: nil) }
                    NavigationLink("Speech providers") { SpeechProvidersView(defaultSelection: model.voice.selection) }
                    NavigationLink("Third-party notices") { ThirdPartyNotices() }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
    }

    private func cliVersion(_ machine: Machine) -> String {
        if machine.hgsVersion.isEmpty {
            return machine.online ? "version unavailable" : "version unavailable while offline"
        }
        return machine.online && !machine.lastKnown ? machine.hgsVersion : "\(machine.hgsVersion) · last known"
    }
}

/// Notices for adapted third-party code bundled with the app.
struct ThirdPartyNotices: View {
    @State private var license = "Loading license…"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("MuxPod — Copyright 2025 mox").font(.headline)
                Text("Terminal ANSI colors, styles, and selection behavior adapted to Swift and SwiftUI from MuxPod (Apache License 2.0). Zerus adds bounded control suppression and explicit relay input.")
                Text("Source: github.com/moezakura/mux-pod\nCommit c33d0fdeb66ca7bbdf4745203578088961982c13").font(.footnote).foregroundStyle(Theme.muted)
                Text(license).font(.caption.monospaced()).foregroundStyle(Theme.muted)
            }
            .textSelection(.enabled)
            .padding()
        }
        .navigationTitle("Third-party notices")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            let text = await Task.detached {
                Bundle.main.url(forResource: "mux-pod", withExtension: "txt").flatMap { try? String(contentsOf: $0, encoding: .utf8) }
            }.value
            license = text ?? "License text is unavailable."
        }
    }
}
