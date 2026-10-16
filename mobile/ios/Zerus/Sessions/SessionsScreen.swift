import SwiftUI
import ZerusKit

/// A pushed conversation, keyed by its exact target.
struct SessionRoute: Hashable {
    let target: Target
}

struct SessionsScreen: View {
    @Environment(AppModel.self) private var model
    @Binding var settings: Bool
    @Binding var path: NavigationPath
    let onPair: () -> Void
    let onBackToProject: () -> Void
    @State private var machineFilter = false

    var body: some View {
        @Bindable var model = model
        NavigationStack(path: $path) {
            Group {
                if model.connections.isEmpty && !model.demo {
                    WelcomeView(onPair: onPair)
                } else {
                    sessionList
                }
            }
            .toolbar { MainToolbar(settings: $settings) }
            .toolbar {
                if model.sessionProjectScope != nil {
                    ToolbarItem(placement: .topBarLeading) {
                        Button { onBackToProject() } label: { Image(systemName: "chevron.backward") }
                            .accessibilityLabel("Back to project")
                    }
                }
            }
            .navigationTitle(title)
            .navigationDestination(for: SessionRoute.self) { route in ConversationView(route: route) }
            .zerusScreen()
        }
        .onChange(of: path.count) { _, count in
            // Leaving the conversation stops its reads; switching tabs keeps it open.
            if count == 0 { model.conversation.close() }
        }
    }

    private var title: String {
        if let project = model.sessionProjectScope { return project.name }
        return model.sessionFilter == .archive ? "Archive" : "Sessions"
    }

    @ViewBuilder
    private var sessionList: some View {
        @Bindable var model = model
        let projection = model.listPresentation
        List {
            Section {
                FilterChips(counts: projection.counts, onMachines: { machineFilter = true })
                    .listRowInsets(EdgeInsets(top: 4, leading: 12, bottom: 4, trailing: 12))
                if model.sessionFilter == .archive {
                    ArchiveFilters(archive: model.archive, machineNames: Dictionary(model.archiveMachines.map { ($0.key, $0.name) }) { a, _ in a },
                                   unsupported: model.archiveMachines.filter { machine in !model.archiveSupportedMachines.contains(where: { $0.key == machine.key }) }.map(\.name),
                                   visibleCount: projection.visibleCount)
                        .listRowBackground(Color.clear)
                }
                Text("\(model.sessionFilter.caption) (\(projection.visibleCount))" + (model.selectedMachines.isEmpty ? "" : " on selected machines"))
                    .font(.subheadline).foregroundStyle(Theme.muted)
                    .listRowBackground(Color.clear)
                if model.demo {
                    Text("Preview data. Pair a workspace to interact with real sessions.").font(.footnote).foregroundStyle(Theme.amber)
                        .listRowBackground(Color.clear)
                }
                if !model.projectWarnings.isEmpty && model.sessionFilter != .archive {
                    Text("Project catalog unavailable on \(model.projectWarnings.joined(separator: ", ")). Sessions and ordinary folders remain available.")
                        .font(.footnote).foregroundStyle(Theme.amber)
                        .listRowBackground(Color.clear)
                }
            }
            .listRowBackground(Color.clear)
            .listRowSeparator(.hidden)
            ForEach(projection.groups) { group in
                let collapsed = model.collapsedProjects.contains(group.key.key) && model.sessionQuery.isEmpty
                Section {
                    if !collapsed {
                        ForEach(group.sessions) { session in
                            SessionCard(session: session)
                                .listRowInsets(EdgeInsets(top: 5, leading: 12, bottom: 5, trailing: 12))
                                .listRowBackground(Color.clear)
                                .listRowSeparator(.hidden)
                        }
                    }
                } header: {
                    ProjectGroupHeader(group: group, collapsed: collapsed) { model.toggleProject(group.key) }
                }
            }
            if projection.visibleCount == 0 && !(model.sessionFilter == .archive && model.archive.loading) && model.listReady {
                Text(emptyText).foregroundStyle(Theme.muted).padding(.vertical, 24)
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
            }
        }
        .listStyle(.plain)
        .listSectionSpacing(.compact)
        .scrollDismissesKeyboard(.immediately)
        .searchable(text: $model.sessionQuery, placement: .navigationBarDrawer(displayMode: .automatic),
                    prompt: model.sessionFilter == .archive ? "Search archive names and IDs" : "Search sessions and IDs")
        .refreshable { model.refresh(explicit: true) }
        .overlay(alignment: .top) {
            if model.catalogProgress { ProgressView().progressViewStyle(.linear).tint(Theme.mint) }
        }
        .overlay {
            if !model.listReady && !model.sessions.isEmpty { ProgressView("Preparing sessions…").foregroundStyle(Theme.muted) }
        }
        .sheet(isPresented: $machineFilter) { MachineFilterSheet(machineCounts: projection.machineCounts) }
        .onAppear { model.configureArchive() }
        .onDisappear { model.archive.pause() }
    }

    private var emptyText: String {
        if model.catalogBusy && model.sessions.isEmpty { return "Checking your sessions…" }
        if model.sessionFilter == .archive && model.archive.partial { return "No sessions in this page match. Browse older/newer sessions or narrow the date range." }
        return "No sessions match this view."
    }
}

/// The first-run screen before any workspace is paired.
struct WelcomeView: View {
    @Environment(AppModel.self) private var model
    let onPair: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                BrandMark().fill(Theme.mint).frame(width: 72, height: 72).accessibilityHidden(true)
                Text("Your agents.\nWithin reach.")
                    .font(.system(size: 34, weight: .semibold))
                    .padding(.top, 24)
                Text("Follow sessions across your machines, answer questions, and pick up the conversation from your phone.")
                    .foregroundStyle(Theme.muted)
                    .padding(.top, 16)
                Button(action: onPair) {
                    Text("Open Machines").frame(maxWidth: .infinity).padding(.vertical, 6)
                }
                .buttonStyle(.borderedProminent)
                .foregroundStyle(Theme.background)
                .padding(.top, 28)
                Button("Try demo") { model.preview() }
                    .frame(maxWidth: .infinity)
                    .padding(.top, 12)
            }
            .padding(28)
            .frame(maxWidth: 560)
        }
        .scrollBounceBehavior(.basedOnSize)
    }
}

private struct FilterChips: View {
    @Environment(AppModel.self) private var model
    let counts: [SessionFilter: Int]
    let onMachines: () -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                ForEach(SessionFilter.allCases, id: \.self) { filter in
                    let selected = model.sessionFilter == filter
                    let count = countText(filter)
                    let accent = filter == .attention && (counts[filter] ?? 0) > 0 ? Theme.amber : (selected ? Theme.mint : Theme.muted)
                    Button { model.sessionFilter = filter } label: {
                        HStack(spacing: 5) {
                            GlyphView(glyph: .named(filter.id), color: accent).frame(width: 20, height: 20)
                            Text(count).font(.footnote.weight(.semibold)).monospacedDigit()
                        }
                        .foregroundStyle(accent)
                        .frame(minWidth: 40, minHeight: 34)
                        .padding(.horizontal, 6)
                        .background(selected ? Theme.selection : Color.clear, in: RoundedRectangle(cornerRadius: 12))
                        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline, lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(filter.caption), \(count) sessions")
                    .accessibilityAddTraits(selected ? .isSelected : [])
                }
                Button(action: onMachines) {
                    HStack(spacing: 5) {
                        GlyphView(glyph: .machines, color: model.selectedMachines.isEmpty ? Theme.muted : Theme.mint).frame(width: 20, height: 20)
                        Text("\(model.selectedMachines.isEmpty ? model.machines.count : model.selectedMachines.count)")
                            .font(.footnote.weight(.semibold)).monospacedDigit()
                    }
                    .foregroundStyle(model.selectedMachines.isEmpty ? Theme.muted : Theme.mint)
                    .frame(minWidth: 40, minHeight: 34)
                    .padding(.horizontal, 6)
                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline, lineWidth: 1))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Filter machines")
            }
            .padding(.vertical, 2)
        }
    }

    private func countText(_ filter: SessionFilter) -> String {
        let value = counts[filter] ?? 0
        if filter == .archive && !model.demo && !model.archiveSupportedMachines.isEmpty {
            if !model.archive.totalsKnown && value == 0 { return "…" }
            return "\(value)" + (model.archive.partial ? "+" : "")
        }
        return "\(value)"
    }
}

private struct ProjectGroupHeader: View {
    let group: SessionGroup
    let collapsed: Bool
    let toggle: () -> Void

    var body: some View {
        Button(action: toggle) {
            HStack(spacing: 10) {
                RoundedRectangle(cornerRadius: 2).fill(Color(projectHex: group.color) ?? Theme.mint).frame(width: 4, height: 22)
                Text(group.name).font(.subheadline.weight(.semibold)).foregroundStyle(Theme.onSurface).lineLimit(1)
                Spacer(minLength: 4)
                Text("\(group.sessions.count)").font(.caption).foregroundStyle(Theme.muted)
                Image(systemName: collapsed ? "chevron.down" : "chevron.up").font(.caption).foregroundStyle(Theme.muted)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .background(Theme.surface, in: RoundedRectangle(cornerRadius: 10))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .textCase(nil)
        .accessibilityLabel("\(group.name), \(group.sessions.count) sessions, \(collapsed ? "collapsed" : "expanded")")
    }
}

private struct MachineFilterSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let machineCounts: [MachineKey: Int]

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Button {
                        model.allMachines()
                    } label: {
                        HStack {
                            Text("All machines")
                            Spacer()
                            Text("\(machineCounts.values.reduce(0, +))").foregroundStyle(Theme.muted)
                            if model.selectedMachines.isEmpty { Image(systemName: "checkmark").foregroundStyle(Theme.mint) }
                        }
                    }
                } footer: {
                    Text("\(model.sessionFilter.caption) before search")
                }
                Section {
                    ForEach(model.machines, id: \.key) { machine in
                        Button { model.toggleMachine(machine.key) } label: {
                            HStack {
                                Image(systemName: model.selectedMachines.contains(machine.key) ? "checkmark.square.fill" : "square")
                                    .foregroundStyle(model.selectedMachines.contains(machine.key) ? Theme.mint : Theme.muted)
                                MachineLabel(name: machine.name, colorHex: model.machineColor(machine.key))
                                Spacer()
                                Text("\(machineCounts[machine.key] ?? 0)").foregroundStyle(Theme.muted).monospacedDigit()
                            }
                        }
                        .accessibilityLabel("\(machine.name), \(machineCounts[machine.key] ?? 0) sessions")
                        .accessibilityAddTraits(model.selectedMachines.contains(machine.key) ? .isSelected : [])
                    }
                }
            }
            .navigationTitle("Filter machines")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
        .presentationDetents([.medium, .large])
    }
}
