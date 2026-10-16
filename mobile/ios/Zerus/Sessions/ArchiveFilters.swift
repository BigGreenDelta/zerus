import SwiftUI
import ZerusKit

/// Archive date range and page controls, ported from Android `ArchiveFiltersUi`.
struct ArchiveFilters: View {
    let archive: ArchivePaging
    let machineNames: [MachineKey: String]
    let unsupported: [String]
    let visibleCount: Int
    @State private var picker = false
    @State private var details = false

    var body: some View {
        let windows = archive.windows
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Button { picker = true } label: {
                    HStack(spacing: 8) {
                        GlyphView(glyph: .calendar, color: Theme.mint).frame(width: 20, height: 20)
                        Text(rangeCaption).lineLimit(1)
                        Spacer(minLength: 0)
                        Image(systemName: "chevron.down").font(.caption)
                    }
                    .padding(.horizontal, 12)
                    .frame(minHeight: 44)
                    .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Archived date: \(rangeCaption). Choose period")
                Button { archive.refresh() } label: {
                    Image(systemName: "arrow.clockwise").frame(width: 44, height: 44)
                        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Theme.outline))
                }
                .buttonStyle(.plain)
                .disabled(archive.loading || !archive.range.valid)
                .accessibilityLabel("Refresh archive")
            }
            HStack {
                if windows.count > 1 {
                    Button { details.toggle() } label: {
                        HStack { Text(summary(windows)); Image(systemName: details ? "chevron.up" : "chevron.down").font(.caption2) }
                    }
                    .buttonStyle(.plain)
                    .font(.footnote).foregroundStyle(Theme.muted)
                } else {
                    Text(summary(windows)).font(.footnote).foregroundStyle(Theme.muted)
                }
                Spacer()
                if archive.hasMore || archive.hasNewer || archive.loading {
                    Button { archive.loadNewer() } label: { Image(systemName: "chevron.left").frame(width: 44, height: 44) }
                        .disabled(!archive.hasNewer || archive.loading || !archive.range.valid)
                        .accessibilityLabel("Newer archived sessions")
                    Button { archive.loadMore() } label: { Image(systemName: "chevron.right").frame(width: 44, height: 44) }
                        .disabled(!archive.hasMore || archive.loading || !archive.range.valid)
                        .accessibilityLabel("Older archived sessions")
                }
            }
            .buttonStyle(.plain)
            if details && windows.count > 1 {
                ForEach(windows.keys.sorted { $0.computerId < $1.computerId }, id: \.self) { key in
                    let page = windows[key]!
                    Text("\(machineNames[key] ?? "Computer"): " + (page.position != nil && page.count > 0
                         ? "\(page.position! + 1)–\(page.position! + page.count) of \(page.total)" : "\(page.count) shown of \(page.total)"))
                        .font(.footnote).foregroundStyle(Theme.muted)
                }
            }
            ForEach(archive.errors.keys.sorted { $0.computerId < $1.computerId }, id: \.self) { key in
                Text("\(machineNames[key] ?? "Computer"): \(archive.errors[key]!)").font(.footnote).foregroundStyle(.red)
            }
            if !unsupported.isEmpty {
                Text("Archive paging is unavailable on \(unsupported.joined(separator: ", ")). Keep the computer online and update its relay, connector and CLI.")
                    .font(.footnote).foregroundStyle(Theme.muted)
            }
        }
        .sheet(isPresented: $picker) { ArchivePeriodSheet(archive: archive) }
    }

    private func summary(_ windows: [MachineKey: ArchiveWindow]) -> String {
        if archive.loading { return "Loading archive…" }
        if !archive.totalsKnown && !archive.hasMore { return "\(visibleCount) loaded sessions" }
        if !archive.totalsKnown { return "\(archive.rows.count) loaded" }
        if windows.count == 1, let window = windows.values.first, let position = window.position, window.count > 0 {
            return "\(position + 1)–\(position + window.count) of \(window.total)"
        }
        return "\(archive.rows.count) loaded of \(archive.total)"
    }

    private var rangeCaption: String {
        if archive.period != .custom { return archive.period.caption }
        let calendar = Calendar.current
        if calendar.isDate(archive.first, inSameDayAs: archive.last) { return archive.first.formatted(.dateTime.day().month(.abbreviated).year()) }
        let sameYear = calendar.component(.year, from: archive.first) == calendar.component(.year, from: archive.last)
        let start = sameYear ? archive.first.formatted(.dateTime.day().month(.abbreviated)) : archive.first.formatted(.dateTime.day().month(.abbreviated).year())
        return "\(start) – \(archive.last.formatted(.dateTime.day().month(.abbreviated).year()))"
    }
}

private struct ArchivePeriodSheet: View {
    let archive: ArchivePaging
    @Environment(\.dismiss) private var dismiss
    @State private var period: ArchivePeriod = .all
    @State private var first = Date()
    @State private var last = Date()
    @State private var prepared = false

    var body: some View {
        let valid = period == .all || (Calendar.current.startOfDay(for: first) <= Calendar.current.startOfDay(for: last) && ArchiveRange.days(first, last).valid)
        NavigationStack {
            Form {
                Section("Quick ranges") {
                    ForEach(ArchivePeriod.allCases.filter { $0 != .custom }, id: \.self) { preset in
                        Button { stage(preset) } label: {
                            HStack { Text(preset.caption); Spacer(); if period == preset { Image(systemName: "checkmark").foregroundStyle(Theme.mint) } }
                        }
                    }
                }
                Section {
                    DatePicker("From", selection: Binding(get: { first }, set: { first = $0; period = .custom }), in: Date(timeIntervalSince1970: 86_400)..., displayedComponents: .date)
                    DatePicker("Through", selection: Binding(get: { last }, set: { last = $0; period = .custom }), in: Date(timeIntervalSince1970: 86_400)..., displayedComponents: .date)
                } header: {
                    Text("Custom range")
                } footer: {
                    Text(period == .all ? "All archived dates (no date limits)" : (valid ? "Both dates included (local time)" : "End date must be on or after the start date."))
                        .foregroundStyle(valid ? Theme.muted : .red)
                }
            }
            .navigationTitle("Archived date")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply range") { archive.dates(period, from: first, through: last); dismiss() }.disabled(!valid)
                }
            }
            .onAppear {
                guard !prepared else { return }
                prepared = true
                period = archive.period
                first = archive.first
                last = archive.last
                if [.today, .week, .month].contains(period) { stage(period) }
            }
        }
        .presentationDetents([.medium, .large])
    }

    private func stage(_ preset: ArchivePeriod) {
        period = preset
        guard preset != .all else { return }
        let now = Date()
        let calendar = Calendar.current
        first = preset == .today ? now : calendar.date(byAdding: .day, value: preset == .week ? -6 : -29, to: now)!
        last = now
    }
}
