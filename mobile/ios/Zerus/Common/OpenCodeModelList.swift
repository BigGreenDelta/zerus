import SwiftUI
import ZerusKit

/// A read-only catalog: selecting a model stages it in the caller's form.
struct OpenCodeModelList: View {
    @Environment(\.dismiss) private var dismiss
    let models: [OpenCodeModel]
    let selected: String
    var nativeDefault: String? = nil
    let onSelect: (String) -> Void
    @State private var query = ""

    var body: some View {
        List {
            OpenCodeModelSections(models: OpenCodeCatalog.filter(models, query: query), selected: selected,
                nativeDefault: nativeDefault) { value in
                    onSelect(value)
                    dismiss()
                }
        }
        .modifier(OpenCodeModelSearch(enabled: models.count > 8, query: $query))
        .navigationTitle("Model")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct OpenCodeModelSearch: ViewModifier {
    let enabled: Bool
    @Binding var query: String
    @ViewBuilder func body(content: Content) -> some View {
        if enabled { content.searchable(text: $query, prompt: "Search models") }
        else { content }
    }
}

/// Sections can also be embedded in the existing session settings List, below its pinned Effort row.
struct OpenCodeModelSections: View {
    let models: [OpenCodeModel]
    let selected: String
    var nativeDefault: String? = nil
    let onSelect: (String) -> Void

    var body: some View {
        if let nativeDefault {
            Section {
                modelRow(label: nativeDefault, value: "", marks: [], selected: selected.isEmpty) { onSelect("") }
            }
        }
        ForEach(OpenCodeCatalog.group(models), id: \.provider) { group in
            Section(group.label) {
                ForEach(group.models, id: \.value) { model in
                    modelRow(label: model.label, value: model.value, marks: OpenCodeCatalog.marks(model), selected: selected == model.value) {
                        onSelect(model.value)
                    }
                }
            }
        }
        if models.isEmpty { Section { Text("No models match this search.").foregroundStyle(Theme.muted) } }
    }

    private func modelRow(label: String, value: String, marks: [String], selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(label).foregroundStyle(Theme.onSurface)
                    if !value.isEmpty && value != label { Text(value).font(.caption).foregroundStyle(Theme.muted).lineLimit(2) }
                    FlowLayout(spacing: 6) {
                        ForEach(marks, id: \.self) { mark in
                            HStack(spacing: 3) {
                                if mark == "Favorite" { Image(systemName: "star.fill") }
                                Text(mark)
                            }
                            .font(.caption2)
                            .padding(.horizontal, 6).padding(.vertical, 3)
                            .background(Theme.surface, in: Capsule())
                            .foregroundStyle(Theme.muted)
                        }
                    }
                }
                Spacer()
                if selected { Image(systemName: "checkmark").foregroundStyle(Theme.mint) }
            }
        }
        .accessibilityLabel((["Model", label] + marks).joined(separator: ", "))
        .accessibilityValue(selected ? "Selected" : "")
    }
}

struct OpenCodeProvidersDetails: View {
    let catalog: OpenCodeAccountCatalog
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Providers").font(.headline)
            ForEach(catalog.providers, id: \.id) { provider in
                Text(provider.label)
                Text(details(provider)).font(.caption).foregroundStyle(Theme.muted)
            }
            let state = OpenCodeCatalog.stateMessage(state: catalog.state, reason: catalog.reason)
            if !state.isEmpty { Text(state).font(.footnote).foregroundStyle(Theme.muted) }
            if catalog.state != "ready", let date = AccountPresentation.date(catalog.catalogAt) {
                Text("Last known catalog from \(date)").font(.footnote).foregroundStyle(Theme.muted)
            }
            let notice = OpenCodeCatalog.truncationMessage(models: catalog.modelsTruncated, providers: catalog.providersTruncated)
            if !notice.isEmpty { Text(notice).font(.footnote).foregroundStyle(Theme.muted) }
        }
    }
    private func details(_ provider: OpenCodeProvider) -> String {
        let active = !provider.activeLabel.isEmpty ? provider.activeLabel : provider.credentials.flatMap { $0 > 1 ? "\($0) accounts" : nil } ?? ""
        return [OpenCodeCatalog.kindLabel(provider.kind), active, provider.models.map { "\($0) models" } ?? ""].filter { !$0.isEmpty }.joined(separator: " · ")
    }
}
