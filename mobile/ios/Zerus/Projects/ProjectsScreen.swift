import SwiftUI
import ZerusKit

struct ProjectsScreen: View {
    @Environment(AppModel.self) private var model
    @Binding var settings: Bool
    @Binding var path: NavigationPath
    let onPair: () -> Void
    let onViewSessions: (ProjectSummary) -> Void
    @State private var query = ""

    var body: some View {
        NavigationStack(path: $path) {
            Group {
                if model.connections.isEmpty && !model.demo {
                    WelcomeView(onPair: onPair)
                } else {
                    list
                }
            }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Projects")
            .navigationDestination(for: ProjectKey.self) { key in
                if let project = model.projects.first(where: { $0.key == key }) {
                    ProjectDetails(project: project, onViewSessions: { onViewSessions(project) })
                }
            }
            .zerusScreen()
        }
    }

    private var list: some View {
        let visible = model.projects.filter(\.catalogued).filter { project in
            query.isEmpty || project.name.localizedCaseInsensitiveContains(query) || project.folders.contains { folder in
                (folder.path + model.machineName(MachineKey(connectionId: project.key.connectionId, computerId: folder.computerId), fallback: folder.computerName))
                    .localizedCaseInsensitiveContains(query)
            }
        }
        return List {
            Section {
                Text("Your work across connected machines").foregroundStyle(Theme.muted).listRowBackground(Color.clear)
                if model.demo {
                    Text("Preview data. Pair a workspace to view your projects.").font(.footnote).foregroundStyle(Theme.amber).listRowBackground(Color.clear)
                }
                if !model.projectWarnings.isEmpty {
                    Text("Project catalog unavailable on \(model.projectWarnings.joined(separator: ", ")). Sessions and ordinary folders remain available.")
                        .font(.footnote).foregroundStyle(Theme.amber).listRowBackground(Color.clear)
                }
            }
            ForEach(visible) { project in
                NavigationLink(value: project.key) { ProjectCard(project: project) }
                    .listRowBackground(Theme.surface)
            }
            if visible.isEmpty {
                Text(model.catalogBusy && model.projects.isEmpty ? "Checking your projects…" : "No projects match this view. Sessions remain available in Sessions.")
                    .foregroundStyle(Theme.muted).padding(.vertical, 24).listRowBackground(Color.clear)
            }
        }
        .listStyle(.insetGrouped)
        .searchable(text: $query, prompt: "Search projects")
        .refreshable { model.refresh(explicit: true) }
    }
}

private struct ProjectCard: View {
    @Environment(AppModel.self) private var model
    let project: ProjectSummary

    var body: some View {
        let count = model.sessions.filter { $0.projectKey == project.key && !SessionFilters.archived($0) }.count
        let accent = Color(projectHex: project.color) ?? Theme.mint
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                GlyphView(glyph: .projects, color: accent).frame(width: 22, height: 22)
                Text(project.name).font(.system(size: 19, weight: .semibold))
            }
            HStack(spacing: 8) {
                Tag(text: "\(count) \(count == 1 ? "session" : "sessions")", color: accent)
                if project.stale { Tag(text: "Last known catalog", color: Theme.amber) }
            }
            ProjectFoldersPreview(project: project)
            if model.connections.count > 1 {
                Text(model.connections.first { $0.id == project.key.connectionId }?.displayName ?? "").font(.caption2).foregroundStyle(Theme.muted)
            }
        }
        .padding(.vertical, 6)
    }
}

/// Folder rows grouped by machine, in the catalog's machine and folder order.
private func folderGroups(_ project: ProjectSummary) -> [(String, [ProjectFolder])] {
    var order: [String] = []
    var groups: [String: [ProjectFolder]] = [:]
    for folder in project.folders {
        if groups[folder.computerId] == nil { order.append(folder.computerId) }
        groups[folder.computerId, default: []].append(folder)
    }
    return order.map { ($0, groups[$0]!) }
}

private struct ProjectFolderRow: View {
    let folder: ProjectFolder

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "folder").font(.footnote).foregroundStyle(Theme.muted).padding(.top, 2)
            VStack(alignment: .leading, spacing: 2) {
                Text(name).font(.footnote.weight(.semibold))
                Text(folder.path).font(.footnote).foregroundStyle(Theme.muted).textSelection(.enabled)
            }
        }
    }

    private var name: String {
        if !folder.name.isEmpty { return folder.name }
        var trimmed = Substring(folder.path)
        while trimmed.hasSuffix("/") { trimmed = trimmed.dropLast() }
        let leaf = trimmed.split(separator: "/").last.map(String.init) ?? ""
        return leaf.isEmpty ? folder.path : leaf
    }
}

private struct ProjectFoldersPreview: View {
    @Environment(AppModel.self) private var model
    let project: ProjectSummary

    var body: some View {
        let groups = folderGroups(project)
        VStack(alignment: .leading, spacing: 10) {
            ForEach(groups.prefix(2), id: \.0) { computerId, folders in
                let key = MachineKey(connectionId: project.key.connectionId, computerId: computerId)
                VStack(alignment: .leading, spacing: 8) {
                    MachineLabel(name: model.machineName(key, fallback: folders[0].computerName), colorHex: model.machineColor(key))
                    ForEach(Array(folders.prefix(2).enumerated()), id: \.offset) { _, folder in ProjectFolderRow(folder: folder) }
                    if folders.count > 2 {
                        Text("+\(folders.count - 2) more \(folders.count == 3 ? "folder" : "folders")").font(.caption2).foregroundStyle(Theme.muted)
                    }
                }
            }
            if groups.isEmpty {
                Text("No saved folders on connected machines").font(.footnote).foregroundStyle(Theme.muted)
            } else if groups.count > 2 {
                Text("+\(groups.count - 2) more \(groups.count == 3 ? "machine" : "machines")").font(.footnote).foregroundStyle(Theme.muted)
            }
        }
    }
}

struct ProjectDetails: View {
    @Environment(AppModel.self) private var model
    let project: ProjectSummary
    let onViewSessions: () -> Void

    var body: some View {
        let count = model.sessions.filter { $0.projectKey == project.key && !SessionFilters.archived($0) }.count
        List {
            Section {
                Text(model.connections.first { $0.id == project.key.connectionId }?.displayName ?? (model.demo ? "Preview workspace" : ""))
                    .foregroundStyle(Theme.muted)
                if project.stale { Text("Last known project catalog").font(.footnote).foregroundStyle(Theme.amber) }
                Text("\(count) \(count == 1 ? "session" : "sessions") across \(project.computers.count) \(project.computers.count == 1 ? "machine" : "machines")")
                    .foregroundStyle(Theme.muted)
                Button(action: onViewSessions) {
                    Label { Text("View sessions") } icon: { GlyphView(glyph: .sessions, color: Theme.mint).frame(width: 20, height: 20) }
                }
            }
            ForEach(folderGroups(project), id: \.0) { computerId, folders in
                let key = MachineKey(connectionId: project.key.connectionId, computerId: computerId)
                Section {
                    ForEach(Array(folders.enumerated()), id: \.offset) { _, folder in ProjectFolderRow(folder: folder) }
                } header: {
                    MachineLabel(name: model.machineName(key, fallback: folders[0].computerName), colorHex: model.machineColor(key)).textCase(nil)
                }
            }
            if project.folders.isEmpty {
                Text("No saved folders on connected machines").foregroundStyle(Theme.muted)
            }
        }
        .navigationTitle(project.name)
        .zerusScreen()
    }
}
