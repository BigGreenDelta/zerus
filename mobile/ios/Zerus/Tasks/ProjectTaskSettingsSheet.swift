import SwiftUI
import ZerusKit
import ZerusReplica

/// The project's task settings: the templates, agents acting on other sessions and their hourly limit.
struct ProjectTaskSettingsSheet: View {
    @Environment(AppModel.self) private var app
    @Environment(\.dismiss) private var dismiss
    @State private var original: TaskSettings?
    @State private var task = ""
    @State private var cancel = ""
    @State private var agentsOther = true
    @State private var limit = ""

    var body: some View {
        let tasks = app.tasks!
        NavigationStack {
            Group {
                if let original {
                    form(tasks, original)
                } else {
                    Text("Reading the project's task settings…").foregroundStyle(Theme.muted)
                }
            }
            .navigationTitle("Project tasks · \(tasks.tasksKey.map(tasks.taskProjectName) ?? "")")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } } }
        }
        .presentationDetents([.large])
        .presentationDragIndicator(.visible)
        .presentationBackground(TasksColors.sheet)
        .task {
            guard let settings = await tasks.taskSettings() else { return }
            original = settings
            task = settings.taskTemplate
            cancel = settings.cancelTemplate
            agentsOther = settings.agentsOther
            limit = String(settings.agentLimit)
        }
    }

    private func form(_ tasks: TasksModel, _ original: TaskSettings) -> some View {
        let trimmed = limit.trimmingCharacters(in: .whitespaces)
        let parsed = Int64(trimmed.isEmpty ? "0" : trimmed).flatMap { (0...1000).contains($0) ? $0 : nil }
        let edited = TaskSettings(taskTemplate: task, cancelTemplate: cancel, agentsOther: agentsOther, agentLimit: parsed ?? original.agentLimit)
        return ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("Task prompt (Send now)").font(.subheadline.weight(.semibold))
                editor($task, minHeight: 140)
                Text("Cancel message").font(.subheadline.weight(.semibold))
                editor($cancel, minHeight: 100)
                Text("Placeholders: {ref}, {author}, {prompt}, {tools}, {history}").font(.caption).foregroundStyle(Theme.muted)
                Toggle("Agents may start and cancel tasks in other sessions", isOn: $agentsOther).tint(Theme.mint)
                Text("Limit tasks started by agents, per hour").font(.subheadline.weight(.semibold))
                TextField("0", text: Binding(get: { limit }, set: { limit = String($0.filter(\.isNumber).prefix(4)) }))
                    .keyboardType(.numberPad)
                    .padding(10)
                    .background(Theme.background, in: RoundedRectangle(cornerRadius: 10))
                    .overlay { RoundedRectangle(cornerRadius: 10).stroke(parsed == nil ? TasksColors.danger : Theme.outline) }
                Text(parsed == nil ? "From 0 to 1000" : "0 is no limit").font(.caption).foregroundStyle(parsed == nil ? TasksColors.danger : Theme.muted)
                if edited != original {
                    Button("Save") {
                        tasks.saveTaskSettings(original: original, edited: edited)
                        dismiss()
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(parsed == nil)
                }
            }
            .padding(20)
        }
    }

    private func editor(_ text: Binding<String>, minHeight: CGFloat) -> some View {
        TextEditor(text: text)
            .font(.system(.footnote, design: .monospaced))
            .frame(minHeight: minHeight)
            .scrollContentBackground(.hidden)
            .padding(8)
            .background(Theme.background, in: RoundedRectangle(cornerRadius: 10))
            .overlay { RoundedRectangle(cornerRadius: 10).stroke(Theme.outline) }
    }
}
