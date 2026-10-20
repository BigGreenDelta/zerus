import SwiftUI
import ZerusKit

/// Read-only provider accounts grouped by reported identity, as on the desktop.
struct AccountsScreen: View {
    @Environment(AppModel.self) private var model
    @Binding var settings: Bool
    let onMachines: () -> Void
    @State private var selection: AccountSelection?

    var body: some View {
        NavigationStack {
            TimelineView(.periodic(from: .now, by: 30)) { context in
                content(now: context.date.timeIntervalSince1970)
            }
            .toolbar { MainToolbar(settings: $settings) }
            .navigationTitle("Accounts")
            .zerusScreen()
        }
    }

    private func content(now: Double) -> some View {
        let catalogs = model.displayedAccounts
        let groups = AccountGrouping.group(catalogs)
        var workspaces: [String] = []
        for catalog in catalogs where !workspaces.contains(catalog.key.connectionId) { workspaces.append(catalog.key.connectionId) }
        let resolved = selection.flatMap { AccountGrouping.selected(groups, $0) }
        return List {
            if model.demo {
                Text("PREVIEW / Sample account data").font(.caption.weight(.semibold)).foregroundStyle(Theme.mint).listRowBackground(Color.clear)
            }
            if !model.accountsError.isEmpty {
                Text(model.accountsError).font(.footnote).foregroundStyle(.red).listRowBackground(Color.clear)
            }
            if catalogs.isEmpty {
                Section {
                    Text(model.connections.isEmpty ? "Pair a workspace to view its account profiles." : "No account snapshots available yet. Refresh to check your machines.")
                    Button("Open Machines", action: onMachines)
                }
            }
            ForEach(workspaces, id: \.self) { connectionId in
                let machines = catalogs.filter { $0.key.connectionId == connectionId }
                Section {
                    ForEach(machines.filter { !$0.supported || !$0.available || $0.accounts.isEmpty }, id: \.key) { catalog in
                        VStack(alignment: .leading, spacing: 4) {
                            AccountMachineLabel(catalog: catalog)
                            Text(availability(catalog)).font(.footnote)
                            if !catalog.online { Text("Offline").font(.footnote).foregroundStyle(Theme.muted) }
                        }
                    }
                    ForEach(groups.filter { $0.key.connectionId == connectionId }) { group in
                        Button { selection = group.members[0].selection } label: { CompactAccountCard(group: group, now: now) }
                            .buttonStyle(.plain)
                    }
                } header: {
                    Text(workspaceName(machines[0])).textCase(nil)
                }
            }
        }
        .refreshable { model.refresh(explicit: true) }
        .sheet(isPresented: Binding(get: { resolved != nil }, set: { if !$0 { selection = nil } })) {
            if let resolved {
                NavigationStack {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 12) {
                            Text(workspaceName(resolved.snapshot.catalog)).font(.caption2).foregroundStyle(Theme.muted)
                            AccountDetails(group: resolved, now: now)
                            Text("Limits update automatically about every five minutes. Refresh reads the latest report from your machine. Saved values remain available offline.")
                                .font(.footnote).foregroundStyle(Theme.muted)
                        }
                        .padding(16)
                    }
                    .navigationTitle("Account details")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Close") { selection = nil } } }
                }
                .presentationDetents([.large])
            }
        }
    }

    private func availability(_ catalog: AccountCatalog) -> String {
        if !catalog.supported && !catalog.online { return "Account profiles appear when this machine is online." }
        if !catalog.supported { return "Update the machine connector to view accounts." }
        if !catalog.available { return "Account catalog is not available yet." }
        return "No account profiles reported on this machine."
    }

    private func workspaceName(_ catalog: AccountCatalog) -> String {
        model.connections.first { $0.id == catalog.key.connectionId }?.displayName ?? (model.demo ? "Preview workspace" : "Workspace")
    }
}

private struct AccountMachineLabel: View {
    @Environment(AppModel.self) private var model
    let catalog: AccountCatalog
    var body: some View {
        MachineLabel(name: model.machineName(catalog.key, fallback: catalog.machineName), colorHex: model.machineColor(catalog.key))
    }
}

private let warningTone = Color(hex: 0xF0A35B)

private func windowTone(_ catalog: AccountCatalog, _ account: ReportedAccount, _ window: AccountWindow, _ now: Double) -> Color {
    guard let used = window.usedPercent, !AccountPresentation.stale(catalog, account, now: now), !AccountPresentation.ended(window, now: now) else { return Theme.muted }
    if used >= 90 { return Color(hex: 0xF07878) }
    if used >= 70 { return warningTone }
    return Color(hex: 0x72CDB2)
}

private func compactBalance(_ account: ReportedAccount) -> String? {
    if account.unlimited == true { return "Credits: Unlimited" }
    if let credits = account.creditBalance { return "Credits: \(credits)" }
    if let first = account.balances.first {
        return [first.kind.isEmpty ? "Balance" : first.kind, first.balance ?? "Unknown", first.currency].filter { !$0.isEmpty }.joined(separator: " / ")
    }
    return nil
}

private struct CompactAccountCard: View {
    let group: GroupedAccount
    let now: Double

    var body: some View {
        let catalog = group.snapshot.catalog
        var account = group.snapshot.account
        account.label = group.label
        var machines: [MachineKey] = []
        for member in group.members where !machines.contains(member.catalog.key) { machines.append(member.catalog.key) }
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                ProviderBadge(agent: account.provider, size: 24)
                VStack(alignment: .leading, spacing: 1) {
                    Text(account.label).font(.subheadline.weight(.semibold)).lineLimit(1)
                    Text(DesktopGlyph.providerName(account.provider) + " / " + AccountPresentation.type(account))
                        .font(.footnote).foregroundStyle(Theme.muted).lineLimit(1)
                }
                Spacer(minLength: 4)
                Image(systemName: "chevron.right").font(.footnote).foregroundStyle(Theme.muted).accessibilityLabel("View account details")
            }
            VStack(alignment: .leading, spacing: 6) {
                FlowLayout(spacing: 6) {
                    ForEach(machines, id: \.self) { key in
                        let members = group.members.filter { $0.catalog.key == key }
                        VStack(alignment: .leading, spacing: 3) {
                            AccountMachineLabel(catalog: members[0].catalog)
                            ForEach(markers(members), id: \.self) { marker in
                                Text(marker).font(.caption2)
                                    .foregroundStyle(members.contains { AccountPresentation.warning($0.account) || $0.account.refreshError } ? warningTone : Theme.muted)
                            }
                        }
                    }
                }
                ForEach(Array(account.windows.prefix(2).enumerated()), id: \.offset) { _, window in
                    HStack(spacing: 8) {
                        HStack(spacing: 4) {
                            Image(systemName: "repeat").font(.system(size: 11))
                            Text(AccountPresentation.shortPeriod(window)).lineLimit(1)
                        }
                        .frame(width: 56, alignment: .leading)
                        HStack(spacing: 4) {
                            Image(systemName: "hourglass").font(.system(size: 11))
                            Text(AccountPresentation.remaining(window, now: now)).lineLimit(1)
                                .foregroundStyle(AccountPresentation.ended(window, now: now) ? warningTone : Theme.muted)
                        }
                        Spacer(minLength: 4)
                        Text(AccountPresentation.percent(window.usedPercent)).foregroundStyle(windowTone(catalog, account, window, now))
                    }
                    .font(.footnote)
                    .foregroundStyle(Theme.muted)
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("\(AccountPresentation.period(window)), \(AccountPresentation.resetSummary(window, now: now)), \(AccountPresentation.percent(window.usedPercent))")
                }
                if account.windows.count > 2 {
                    Text("+\(account.windows.count - 2) more usage windows").font(.caption2).foregroundStyle(Theme.muted)
                }
                if account.windows.isEmpty {
                    Text(compactBalance(account) ?? "Usage unknown").font(.footnote).foregroundStyle(Theme.muted).lineLimit(1)
                }
            }
            .padding(.leading, 34)
        }
        .padding(.vertical, 4)
        .contentShape(Rectangle())
    }

    private func markers(_ members: [AccountMember]) -> [String] {
        var result: [String] = []
        for member in members {
            let marker = AccountPresentation.compactStatus(member.catalog, member.account)
            guard !marker.isEmpty else { continue }
            let text = (members.count > 1 ? "\(member.account.id) / " : "") + marker
            if !result.contains(text) { result.append(text) }
        }
        return result
    }
}

private struct AccountDetails: View {
    let group: GroupedAccount
    let now: Double

    var body: some View {
        let catalog = group.snapshot.catalog
        var account = group.snapshot.account
        account.label = group.label
        var machines: [MachineKey] = []
        for member in group.members where !machines.contains(member.catalog.key) { machines.append(member.catalog.key) }
        let identityLines = [account.identity.name, account.identity.email, account.identity.organization].filter { !$0.isEmpty }
        return VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                ProviderBadge(agent: account.provider, size: 24)
                VStack(alignment: .leading) {
                    Text(account.label).font(.headline)
                    Text(DesktopGlyph.providerName(account.provider)).font(.footnote).foregroundStyle(Theme.muted)
                }
            }
            VStack(alignment: .leading, spacing: 3) {
                ForEach(Array(Set(identityLines)).sorted(by: { identityLines.firstIndex(of: $0)! < identityLines.firstIndex(of: $1)! }), id: \.self) { Text($0) }
                Text("Plan: " + (account.identity.plan.isEmpty ? "Not reported" : account.identity.plan))
                if !account.identity.authMethod.isEmpty { Text("Authentication: \(account.identity.authMethod)").font(.footnote).foregroundStyle(Theme.muted) }
                if !account.identity.accountId.isEmpty { Text("Provider account: \(account.identity.accountId)").font(.footnote).foregroundStyle(Theme.muted) }
            }
            .textSelection(.enabled)
            ForEach(machines, id: \.self) { key in
                let members = group.members.filter { $0.catalog.key == key }
                VStack(alignment: .leading, spacing: 4) {
                    AccountMachineLabel(catalog: members[0].catalog)
                    ForEach(members, id: \.account.id) { member in
                        let item = member.account
                        Text("Profile: \(item.id)" + (item.isDefault ? " / Default" : "")).font(.footnote)
                        Text(AccountPresentation.status(item)).font(.footnote).foregroundStyle(AccountPresentation.warning(item) ? warningTone : Theme.muted)
                        let problem = AccountPresentation.compactStatus(member.catalog, item)
                        if !problem.isEmpty { Text(problem).font(.footnote).foregroundStyle(Theme.muted) }
                        if AccountPresentation.stale(member.catalog, item, now: now) { Text("Last known report").font(.footnote).foregroundStyle(Theme.muted) }
                        if let date = AccountPresentation.date(item.checkedAt) { Text("Provider updated \(date)").font(.footnote).foregroundStyle(Theme.muted) }
                        if let openCode = item.openCode { OpenCodeProvidersDetails(catalog: openCode) }
                    }
                }
            }
            HStack(spacing: 6) {
                Text("Usage report").font(.caption.weight(.medium)).foregroundStyle(Theme.muted)
                AccountMachineLabel(catalog: catalog)
            }
            Text(AccountPresentation.status(account)).font(.caption.weight(.medium)).foregroundStyle(AccountPresentation.warning(account) ? warningTone : Theme.muted)
            if account.identityCached { Text("Identity retained from the last provider report").font(.footnote).foregroundStyle(Theme.muted) }
            if account.refreshing { Text("Provider refresh in progress").font(.footnote).foregroundStyle(Theme.muted) }
            if account.refreshError { Text("The last provider refresh failed. Retained values may be out of date.").font(.footnote).foregroundStyle(Theme.muted) }
            ForEach(Array(account.windows.enumerated()), id: \.offset) { _, window in
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Label(AccountPresentation.period(window), systemImage: "repeat")
                        Spacer()
                        Text(AccountPresentation.percent(window.usedPercent)).foregroundStyle(windowTone(catalog, account, window, now))
                    }
                    .font(.subheadline)
                    if let used = window.usedPercent {
                        ProgressView(value: min(max(used / 100, 0), 1)).tint(windowTone(catalog, account, window, now))
                    }
                    Label(AccountPresentation.resetSummary(window, now: now) + (AccountPresentation.ended(window, now: now) ? " / Refresh needed" : ""), systemImage: "hourglass")
                        .font(.footnote).foregroundStyle(Theme.muted)
                    if let date = AccountPresentation.date(window.resetsAt) { Text("Reported reset: \(date)").font(.footnote).foregroundStyle(Theme.muted) }
                }
            }
            if account.windows.isEmpty { Text("Usage limits not reported").font(.footnote).foregroundStyle(Theme.muted) }
            if account.unlimited == true { Text("Credits: Unlimited") }
            else if let credits = account.creditBalance { Text("Credit balance: \(credits)") }
            ForEach(Array(account.balances.enumerated()), id: \.offset) { _, balance in
                Text([balance.kind.isEmpty ? "Balance" : balance.kind, balance.balance ?? "Unknown", balance.currency].filter { !$0.isEmpty }.joined(separator: " / "))
            }
            let checked = account.checkedAt.flatMap { $0 <= now + 60 ? AccountPresentation.date($0) : nil }
            Text(checked.map { "Provider updated \($0)" } ?? "Provider update time not reported").font(.footnote).foregroundStyle(Theme.muted)
        }
    }
}

/// A simple wrapping row layout.
struct FlowLayout: Layout {
    var spacing: CGFloat = 6

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        var x: CGFloat = 0, y: CGFloat = 0, row: CGFloat = 0, widest: CGFloat = 0
        for subview in subviews {
            let size = subview.sizeThatFits(.unspecified)
            if x > 0 && x + size.width > width { x = 0; y += row + spacing; row = 0 }
            x += size.width + spacing
            row = max(row, size.height)
            widest = max(widest, x - spacing)
        }
        return CGSize(width: min(widest, width), height: y + row)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = bounds.minX, y = bounds.minY, row: CGFloat = 0
        for subview in subviews {
            let size = subview.sizeThatFits(.unspecified)
            if x > bounds.minX && x + size.width > bounds.maxX { x = bounds.minX; y += row + spacing; row = 0 }
            subview.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            row = max(row, size.height)
        }
    }
}
