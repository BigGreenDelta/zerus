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
