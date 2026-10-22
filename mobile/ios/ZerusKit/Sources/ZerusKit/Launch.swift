import Foundation

public struct LaunchAccount: Hashable, Sendable {
    public var id: String
    public var provider: String
    public var label: String
    public var native = false
    public var isDefault = false
    public var installed = true
    public var authStatus = "unknown"
    public var usage: ReportedAccount?

    public init(id: String, provider: String, label: String, native: Bool = false, isDefault: Bool = false, installed: Bool = true,
                authStatus: String = "unknown", usage: ReportedAccount? = nil) {
        self.id = id
        self.provider = provider
        self.label = label
        self.native = native
        self.isDefault = isDefault
        self.installed = installed
        self.authStatus = authStatus
        self.usage = usage
    }
}

public struct LaunchAccountChoice: Hashable, Sendable, Identifiable {
    public let account: LaunchAccount
    public let title: String
    public let limits: String
    public let usage: ReportedAccount?
    public var key: String { account.provider + "/" + account.id }
    public var id: String { key }
}

public enum LaunchLimitTone: Sendable { case normal, warning, critical }

public struct LaunchLimitPace: Hashable, Sendable {
    public let used: Double
    public let elapsed: Double?
    public let tone: LaunchLimitTone
}

public struct LaunchModel: Hashable, Sendable {
    public let value: String
    public let label: String
    /// The model's own native effort variants (OpenCode), in catalog order.
    public var efforts: [EffortOption] = []
    public var provider = "", providerLabel = ""
    public var free = false, favorite = false, isDefault = false
}

/// One native effort choice; `label` is the computer's name for it, or the familiar name of a standard level.
public struct EffortOption: Hashable, Sendable {
    public let value: String
    public let label: String

    public init(value: String, label: String) {
        self.value = value
        self.label = label
    }

    /// The display name of an effort that has no native label; "" is the native default.
    public static func text(_ value: String) -> String {
        switch value {
        case "": "Default"
        case "xhigh": "Extra high"
        default: value.prefix(1).uppercased() + value.dropFirst()
        }
    }

    /// The native catalog's variant ID shape, shared by every layer (tests/fixtures/opencode-effort-ids.json): non-empty,
    /// at most 40 UTF-8 bytes, without Unicode whitespace, control characters or "/". Model membership is checked separately.
    public static func valid(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.count <= 40 &&
            !value.unicodeScalars.contains { $0 == "/" || $0.properties.isWhitespace || $0.properties.generalCategory == .control }
    }

    /// `effort_labels` names `effort_options` by position; a missing or unusable label keeps the familiar name.
    /// At most the first 16 well-formed variants are offered, never malformed or repeated ones.
    public static func options(_ values: [String], labels: [String]) -> [EffortOption] {
        let named = labels.count == values.count
        var seen = Set<String>()
        var result: [EffortOption] = []
        for (index, value) in values.enumerated() {
            if result.count == 16 { break }
            guard valid(value), seen.insert(value).inserted else { continue }
            result.append(EffortOption(value: value, label: named && LaunchPresentation.usableLabel(labels[index]) ? labels[index] : text(value)))
        }
        return result
    }
}

public struct LaunchFolder: Hashable, Sendable {
    public let path: String
    public let name: String
}

public struct LaunchProjectFolder: Hashable, Sendable {
    public let id: String
    public let path: String
    public let name: String
}

public struct LaunchProject: Hashable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let color: String
    public let folders: [LaunchProjectFolder]
}

/// Everything that can keep New session from starting; `LaunchPresentation.disabledReason` explains the first problem.
public struct LaunchReadiness: Hashable, Sendable {
    public var machineReason = ""
    public var catalogLoading = false
    public var catalogAvailable = true
    public var catalogChanged = false
    public var projectRequired = false
    public var projectChosen = true
    public var projectHasFolder = true
    public var folderChosen = true
    public var folderLoading = false
    public var folderVerified = true
    public var repositoryLoading = false
    public var providers = 1
    public var providerChosen = true
    public var accounts = 1
    public var accountChosen = true
    public var nameError = ""
    public var automatic = false
    public var automaticFailed = false
    public var automaticUsed = false
    public var worktreeReason = ""
    public var confirmed = false
    public init() {}
}

public enum LaunchPresentation {
    public static let agentIds: Set<String> = ["codex", "claude", "kimi", "dsh", "opencode"]
    /// Relay and connector feature: an OpenCode launch may carry one of the chosen model's `effort_options`, and a
    /// model change may request any of its native variants instead of only the relay's fixed levels.
    public static let effortFeature = "opencode_launch_effort"

    /// Both hops must accept it: an older relay or connector refuses an effort it does not know.
    public static func effortSupported(relay: Set<String>?, computer: Set<String>?) -> Bool {
        relay?.contains(effortFeature) == true && computer?.contains(effortFeature) == true
    }

    /// Catalog order is kept; the query matches project names and this computer's folder names and paths.
    public static func filterProjects(_ projects: [LaunchProject], _ query: String) -> [LaunchProject] {
        let needle = query.trimmingCharacters(in: .whitespaces)
        guard !needle.isEmpty else { return projects }
        return projects.filter { project in
            project.name.localizedCaseInsensitiveContains(needle) ||
                project.folders.contains { $0.name.localizedCaseInsensitiveContains(needle) || $0.path.localizedCaseInsensitiveContains(needle) }
        }
    }

    public static func folderSummary(_ project: LaunchProject) -> String {
        switch project.folders.count {
        case 0: "No folder on this computer"
        case 1: project.folders[0].path
        default: "\(project.folders.count) folders"
        }
    }

    /// One concise reason for a disabled Create session action, or "" when it can start.
    public static func disabledReason(_ form: LaunchReadiness) -> String {
        if form.confirmed { return "This session was already created." }
        if !form.machineReason.isEmpty { return form.machineReason }
        if form.catalogLoading { return "Loading agents and projects…" }
        if !form.catalogAvailable { return "Agents and projects are unavailable. Close and try again." }
        if form.catalogChanged { return "Creation choices changed. Review them first." }
        if form.projectRequired && !form.projectChosen { return "Choose a project." }
        if !form.folderChosen { return form.projectHasFolder ? "Choose a folder." : "Browse to choose a folder." }
        if form.folderLoading { return "Checking folder…" }
        if !form.folderVerified { return "Check this folder again or browse to it." }
        if form.repositoryLoading { return "Checking repository…" }
        if form.providers == 0 { return "No agents are installed on this computer." }
        if !form.providerChosen { return "Choose an agent." }
        if form.accounts == 0 { return "No account is available for this agent on this computer." }
        if !form.accountChosen { return "Choose an account." }
        if !form.nameError.isEmpty { return form.nameError }
        if form.automatic && form.automaticFailed { return "Automatic worktree creation failed. Choose None or another worktree." }
        if form.automatic && form.automaticUsed { return "This form already created its worktree. Choose None to start there." }
        if form.automatic && !form.worktreeReason.isEmpty { return form.worktreeReason }
        return ""
    }

    public static func agents(_ raw: JSONObject?) -> [String] {
        var seen = Set<String>()
        return (raw?.array("agents") ?? []).compactMap { value in
            guard case .string(let agent) = value, agentIds.contains(agent), seen.insert(agent).inserted else { return nil }
            return agent
        }
    }

    static let accountIdPattern = try! NSRegularExpression(pattern: "^[A-Za-z0-9_][A-Za-z0-9_-]{0,79}$")
    static let authStatuses: Set<String> = ["signed_in", "signed_out", "expired", "credentials_locked", "desktop_session_unavailable", "credentials_unavailable"]

    public static func accounts(_ raw: JSONObject?, _ provider: String) -> [LaunchAccount] {
        var seen = Set<String>()
        return (raw?.objects("accounts") ?? []).compactMap { row in
            guard case .string(let id)? = row["id"], !id.isEmpty,
                  accountIdPattern.firstMatch(in: id, range: NSRange(id.startIndex..., in: id)) != nil,
                  case .string(let agent)? = row["provider"], agent == provider, case .string(let label)? = row["label"],
                  seen.insert(id).inserted else { return nil }
            var status = "unknown"
            if case .string(let reported)? = row["auth_status"], authStatuses.contains(reported) { status = reported }
            return LaunchAccount(id: id, provider: agent, label: label, native: row["native"] == .bool(true), isDefault: row["is_default"] == .bool(true),
                                 installed: row["installed"] != .bool(false), authStatus: status,
                                 usage: row.object("usage") != nil ? AccountSnapshots.account(row) : nil)
        }
    }

    /// A native display name: non-blank, at most 160 UTF-16 units and free of control characters.
    public static func usableLabel(_ text: String) -> Bool {
        !text.trimmingCharacters(in: .whitespaces).isEmpty && text.utf16.count <= 160 && !text.unicodeScalars.contains(where: PairingInvite.isISOControl)
    }

    public static func validModel(_ value: String) -> Bool {
        !value.trimmingCharacters(in: .whitespaces).isEmpty && value.utf16.count <= 120 && value.contains("/") &&
            !value.unicodeScalars.contains(where: PairingInvite.isISOControl)
    }

    /// Only exact models offered by this computer's selected native OpenCode account.
    public static func models(_ raw: JSONObject?, provider: String, accountId: String) -> [LaunchModel] {
        guard provider == "opencode" else { return [] }
        let matching = (raw?.objects("accounts") ?? []).filter { $0.string("provider") == provider && $0.string("id") == accountId }
        guard matching.count == 1 else { return [] }
        return OpenCodeCatalog.models(matching[0].array("model_options")).map { model in
            LaunchModel(value: model.value, label: model.label, efforts: model.efforts, provider: model.provider,
                providerLabel: model.providerLabel, free: model.free, favorite: model.favorite, isDefault: model.isDefault)
        }
    }

    public static func catalogNotice(_ raw: JSONObject?, provider: String, accountId: String) -> String {
        guard provider == "opencode" else { return "" }
        let matching = (raw?.objects("accounts") ?? []).filter { $0.string("provider") == provider && $0.string("id") == accountId }
        guard matching.count == 1, let catalog = OpenCodeCatalog.account(matching[0]) else { return "" }
        return [OpenCodeCatalog.stateMessage(state: catalog.state, reason: catalog.reason),
                OpenCodeCatalog.truncationMessage(models: catalog.modelsTruncated, providers: false)]
            .filter { !$0.isEmpty }.joined(separator: " ")
    }

    /// The chosen OpenCode model's effort variants, offered only when this computer's connector accepts launch effort.
    public static func efforts(_ raw: JSONObject?, provider: String, accountId: String, model: String, supported: Bool) -> [EffortOption] {
        guard supported, provider == "opencode", !model.isEmpty else { return [] }
        return models(raw, provider: provider, accountId: accountId).first { $0.value == model }?.efforts ?? []
    }

    /// The exact launch request for the current catalog. `effort` is sent only for an offered variant of the chosen
    /// model and only when the computer's connector accepts it; otherwise the request is exactly the older one.
    public static func arguments(_ catalog: JSONObject, agent: String, directory: String, tag: String, accountId: String?, projectId: String,
                                 worktrees: JSONObject?, model: String = "", effort: String = "", effortSupported: Bool = false, implicitInbox: Bool = false) throws -> JSONObject {
        guard agents(catalog).contains(agent) else { throw ZerusError.invalid("This native provider is unavailable.") }
        guard let accountId, accounts(catalog, agent).contains(where: { $0.id == accountId }) else {
            throw ZerusError.invalid("Choose an offered account for the selected provider.")
        }
        var args: JSONObject = ["agent": .string(agent), "directory": .string(directory), "tag": .string(tag), "account_id": .string(accountId)]
        if !model.isEmpty {
            guard models(catalog, provider: agent, accountId: accountId).contains(where: { $0.value == model }) else {
                throw ZerusError.invalid("The native model changed. Review the current choices.")
            }
            args["model"] = .string(model)
        }
        if !effort.isEmpty {
            guard efforts(catalog, provider: agent, accountId: accountId, model: model, supported: effortSupported).contains(where: { $0.value == effort }) else {
                throw ZerusError.invalid("The native effort changed. Review the current choices.")
            }
            args["effort"] = .string(effort)
        }
        if catalog["project_launch_supported"] == .bool(true) {
            // A private client hint, never a new backend protocol field. Keep
            // implicit Inbox passive if native creation finds fresher evidence.
            if implicitInbox && projectId == "ungrouped" {
                guard projects(catalog).contains(where: { $0.id == projectId }) else { throw ZerusError.invalid("Choose a current native project.") }
            } else {
                let fields = try projectArguments(catalog, projectId: projectId, directory: directory, worktrees: worktrees)
                for key in fields.keys { args[key] = fields[key] }
            }
        } else if !projectId.isEmpty {
            throw ZerusError.invalid("This computer does not support scoped project assignment.")
        }
        return try SessionActionPolicies.arguments("launch", args)
    }

    public static func defaultAccount(_ accounts: [LaunchAccount], selected: String = "") -> String {
        if let match = accounts.first(where: { $0.id == selected }) { return match.id }
        let defaults = accounts.filter(\.isDefault)
        if defaults.count == 1 { return defaults[0].id }
        return accounts.first?.id ?? ""
    }

    public static func accountTitle(_ account: LaunchAccount) -> String {
        if let email = account.usage?.identity.email, !email.isEmpty { return email }
        if !account.label.trimmingCharacters(in: .whitespaces).isEmpty && !["Default account", "Native account"].contains(account.label) { return account.label }
        return "Account n/a"
    }

    /// Exact machine/profile cache reads only; presentation never fetches provider limits.
    public static func accountChoices(_ raw: JSONObject?, machine: MachineKey, catalogs: [AccountCatalog], now: Double, selectedProvider: String = "",
                                      selectedAccount: String = "", fold: (String) -> String = AccountGrouping.unicodeFold) -> [LaunchAccountChoice] {
        let cache = catalogs.first { $0.key == machine }?.accounts ?? []
        let local: [(LaunchAccount, ReportedAccount)] = agents(raw).flatMap { provider in accounts(raw, provider) }.compactMap { account in
            let cached = cache.first { $0.provider == account.provider && $0.id == account.id }.flatMap { cached -> ReportedAccount? in
                guard ["unknown", "signed_in"].contains(account.authStatus) else { return nil }
                if let usage = account.usage {
                    guard !usage.identity.email.isEmpty, usage.identity.email == cached.identity.email, usage.identity.accountId == cached.identity.accountId,
                          usage.identity.organization == cached.identity.organization else { return nil }
                }
                return cached
            }
            let reported: ReportedAccount?
            if let cached, account.usage == nil || (cached.checkedAt ?? 0) > (account.usage?.checkedAt ?? 0) { reported = cached } else { reported = account.usage }
            guard var snapshot = reported ?? AccountSnapshots.account(["id": .string(account.id), "provider": .string(account.provider), "label": .string(account.label)])
            else { return nil }
            snapshot.id = account.id
            snapshot.provider = account.provider
            snapshot.label = account.label
            snapshot.native = account.native
            snapshot.isDefault = account.isDefault
            snapshot.installed = account.installed
            return (account, snapshot)
        }
        let catalog = AccountCatalog(key: machine, machineName: "", supported: true, available: true, stale: false, online: true, observedAt: nil,
                                     accounts: local.map(\.1))
        // Match Accounts grouping within the selected computer; never borrow another computer's binding.
        return AccountGrouping.group([catalog], fold: fold).compactMap { group in
            let members = group.members.compactMap { member in local.first { $0.0.provider == member.account.provider && $0.0.id == member.account.id }?.0 }
            guard let first = members.first else { return nil }
            let chosen = members.first { $0.provider == selectedProvider && $0.id == selectedAccount } ?? members.first(where: \.isDefault) ??
                members.first(where: \.native) ?? first
            let reported = group.snapshot.account
            var titled = chosen
            titled.usage = reported
            return LaunchAccountChoice(account: chosen, title: accountTitle(titled), limits: accountLimits(reported, now: now), usage: reported)
        }
    }

    public static func limitWindows(_ account: ReportedAccount?, now: Double) -> [AccountWindow] {
        (account?.windows ?? []).filter { ($0.usedPercent?.isFinite ?? false) && !AccountPresentation.ended($0, now: now) }
    }

    /// Same quota/elapsed rails and pace colors as the desktop footer.
    public static func limitPace(_ window: AccountWindow, now: Double) -> LaunchLimitPace {
        let used = min(max(window.usedPercent.flatMap { $0.isFinite ? $0 : nil } ?? 0, 0), 100)
        let threshold: LaunchLimitTone = used >= 90 ? .critical : used >= 70 ? .warning : .normal
        let length = (window.minutes ?? 0) * 60
        let left = (window.resetsAt ?? now) - now
        guard length.isFinite, length > 0, left.isFinite, left > 0 else { return LaunchLimitPace(used: used, elapsed: nil, tone: threshold) }
        let passed = max(length - left, 0), elapsed = passed / length
        if used >= 100 || elapsed < 0.08 { return LaunchLimitPace(used: used, elapsed: elapsed, tone: threshold) }
        let tone: LaunchLimitTone
        if used / elapsed > 100 {
            let runsOut = (100 - used) * passed / used
            tone = runsOut >= left / 2 ? .warning : .critical
        } else {
            tone = used >= 90 ? .warning : .normal
        }
        return LaunchLimitPace(used: used, elapsed: elapsed, tone: tone)
    }

    public static func accountLimits(_ account: ReportedAccount?, now: Double) -> String {
        let windows = limitWindows(account, now: now).map { window in
            "\(AccountPresentation.shortPeriod(window)) \(Int(min(max(window.usedPercent!, 0), 100) + 0.5))% used"
        }
        let balances = (account?.balances ?? []).compactMap { balance in balance.balance.map { "Balance: \($0) \(balance.currency)".trimmingCharacters(in: .whitespaces) } }
        let text = (windows + balances).joined(separator: "  ")
        return text.isEmpty ? "limits n/a" : text
    }

    public static func projects(_ raw: JSONObject?) -> [LaunchProject] {
        guard let raw, raw["project_launch_supported"] == .bool(true), case .string(let swarm)? = raw["swarm_id"], !swarm.isEmpty else { return [] }
        var seen = Set<String>()
        return raw.objects("projects").compactMap { row in
            guard case .string(let id)? = row["id"], validProjectId(id), case .string(let name)? = row["name"],
                  !name.trimmingCharacters(in: .whitespaces).isEmpty, row["accessible"] != .bool(false), seen.insert(id).inserted else { return nil }
            var folderIds = Set<String>()
            let folders = row.objects("folders").compactMap { folder -> LaunchProjectFolder? in
                guard case .string(let folderId)? = folder["id"], validProjectId(folderId), case .string(let path)? = folder["path"], absoluteFolder(path),
                      folder["local"] != .bool(false), folderIds.insert(folderId).inserted else { return nil }
                let name = folder.string("name")
                return LaunchProjectFolder(id: folderId, path: path, name: name.isEmpty ? leaf(path) : name)
            }
            return LaunchProject(id: id, name: name, color: row.string("color"), folders: folders)
        }
    }

    static func validProjectId(_ id: String) -> Bool {
        !id.trimmingCharacters(in: .whitespaces).isEmpty && id.trimmingCharacters(in: .whitespacesAndNewlines) == id && id.utf8.count <= 128 &&
            !id.unicodeScalars.contains(where: PairingInvite.isISOControl)
    }

    /// Only an untouched Inbox follows exact native folder/worktree membership.
    /// Explicit project choices, ambiguity and incomplete evidence keep selection.
    public static func inferredProject(_ raw: JSONObject?, selected: String, explicit: Bool, directory: String,
                                       browser: JSONObject?, worktrees: JSONObject?) -> String {
        guard !explicit, selected == "ungrouped", let raw, raw.objects("projects").count <= 500,
              let browser, browser.string("path") == directory || browser.string("requested_path") == directory else { return selected }
        let canonical = browser.string("path")
        guard absoluteFolder(canonical) else { return selected }
        let offered = projects(raw)
        guard offered.contains(where: { $0.id == selected }) else { return selected }
        let candidates = offered.filter { $0.id != "ungrouped" }
        guard candidates.reduce(0, { $0 + $1.folders.count }) <= 512 else { return selected }
        let related = WorktreePresentation.forFolder(worktrees, directory).flatMap { value -> JSONObject? in
            value["partial"] == .bool(false) && WorktreePresentation.healthy(value) && value.objects("worktrees").count <= 512 ? value : nil
        }
        let matching = candidates.filter { project in
            project.folders.contains { $0.path == canonical } ||
                (related != nil && WorktreePresentation.anchor(related, project, canonical) != nil)
        }
        return matching.count == 1 ? matching[0].id : selected
    }
    public static func projectArguments(_ raw: JSONObject, projectId: String, directory: String, worktrees: JSONObject? = nil) throws -> JSONObject {
        let matching = projects(raw).filter { $0.id == projectId }
        guard matching.count == 1 else { throw ZerusError.invalid("Choose a current native project.") }
        let project = matching[0]
        let folders = project.folders.filter { $0.path == directory }
        let folder = folders.count == 1 ? folders[0] : nil
        let anchor = folder == nil && raw["worktree_supported"] == .bool(true) ? WorktreePresentation.anchor(worktrees, project, directory) : nil
        var result: JSONObject = ["swarm_id": .string(raw.string("swarm_id")), "project_id": .string(project.id), "add_folder": .bool(folder == nil && anchor == nil)]
        if let folder {
            result["project_folder_id"] = .string(folder.id)
        } else if let anchor, let worktrees {
            result["worktree_folder_id"] = .string(anchor.id)
            result["worktree_common_dir"] = .string(worktrees.string("common_dir"))
        }
        return result
    }

    /// Only the typed launch catalog fields; provider error details and unknown keys are dropped before caching.
    public static func sanitizeCatalog(_ raw: JSONObject) -> JSONObject {
        let providers = agents(raw)
        var result: JSONObject = ["agents": JSON(providers)]
        result["accounts"] = .array(providers.flatMap { provider in accounts(raw, provider) }.map { account in
            var row: JSONObject = ["id": .string(account.id), "provider": .string(account.provider), "label": .string(account.label), "native": .bool(account.native),
                                   "is_default": .bool(account.isDefault), "installed": .bool(account.installed), "auth_status": .string(account.authStatus),
                                   "model_options": .array(models(raw, provider: account.provider, accountId: account.id).map { model in
                                       var option: JSONObject = ["value": .string(model.value), "label": .string(model.label),
                                           "provider": .string(model.provider), "provider_label": .string(model.providerLabel), "free": .bool(model.free)]
                                       if model.favorite { option["favorite"] = .bool(true) }
                                       if model.isDefault { option["default"] = .bool(true) }
                                       if !model.efforts.isEmpty {
                                           option["effort_options"] = JSON(model.efforts.map(\.value))
                                           option["effort_labels"] = JSON(model.efforts.map(\.label))
                                       }
                                       return .object(option)
                                   })]
            if account.provider == "opencode" {
                let matching = raw.objects("accounts").filter { $0.string("provider") == account.provider && $0.string("id") == account.id }
                if matching.count == 1, let catalog = OpenCodeCatalog.account(matching[0]) {
                    row["catalog_state"] = .string(catalog.state)
                    row["catalog_reason"] = .string(catalog.reason)
                    row["model_options_truncated"] = .bool(catalog.modelsTruncated)
                }
            }
            if let usage = account.usage { row["usage"] = AccountSnapshots.accountJSON(usage)["usage"] }
            return .object(row)
        })
        let supported = raw["project_launch_supported"] == .bool(true)
        result["project_launch_supported"] = .bool(supported)
        result["worktree_supported"] = .bool(raw["worktree_supported"] == .bool(true))
        if supported {
            result["swarm_id"] = .string(raw.string("swarm_id"))
            result["default_project"] = .string(raw.string("default_project"))
            result["projects"] = .array(projects(raw).map { project in
                .object(["id": .string(project.id), "name": .string(project.name), "color": .string(project.color),
                         "folders": .array(project.folders.map { .object(["id": .string($0.id), "name": .string($0.name), "path": .string($0.path)]) })])
            })
        }
        return result
    }

    public static func folders(_ raw: JSONObject?) -> [LaunchFolder] {
        var seen = Set<String>()
        return (raw?.objects("directories") ?? []).compactMap { row in
            guard case .string(let path)? = row["path"], absoluteFolder(path), seen.insert(path).inserted else { return nil }
            let name = row.string("name")
            return LaunchFolder(path: path, name: name.isEmpty ? leaf(path) : name)
        }
    }

    public static func absoluteFolder(_ path: String) -> Bool {
        path.hasPrefix("/") && path.utf16.count <= 4096 && !path.unicodeScalars.contains(where: PairingInvite.isISOControl)
    }

    public static func selectedFolder(_ raw: JSONObject?, _ selected: String) -> Bool {
        absoluteFolder(selected) && (raw?["path"] == .string(selected) || raw?["requested_path"] == .string(selected) || folders(raw).contains { $0.path == selected })
    }

    public static func generatedName() -> String { "work-" + String(UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "").prefix(8)) }

    public static func nameError(_ tag: String) -> String {
        if tag.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return "Enter a session name." }
        if tag != tag.trimmingCharacters(in: .whitespacesAndNewlines) { return "Remove spaces at the beginning and end." }
        if tag.utf8.count > 120 { return "Session name exceeds 120 UTF-8 bytes." }
        if tag.unicodeScalars.contains(where: { "/\\:.".unicodeScalars.contains($0) || PairingInvite.isISOControl($0) }) {
            return "Use a name without path separators, dots, colons, or control characters."
        }
        return ""
    }

    public static func leaf(_ path: String) -> String { path.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? path }
}

public struct LaunchWorktree: Hashable, Sendable {
    public let path: String
    public let branch: String
    public let kind: String
}

public enum WorktreePresentation {
    public static func healthy(_ raw: JSONObject?) -> Bool {
        guard let raw else { return false }
        return raw.string("state") == "ok" && raw["stale"] == .bool(false) &&
            LaunchPresentation.absoluteFolder(raw.string("common_dir")) && LaunchPresentation.absoluteFolder(raw.string("path"))
    }

    public static func entries(_ raw: JSONObject?) -> [LaunchWorktree] {
        guard healthy(raw), let raw else { return [] }
        var seen = Set<String>()
        return Array(raw.objects("worktrees").compactMap { row -> LaunchWorktree? in
            guard case .string(let path)? = row["path"], LaunchPresentation.absoluteFolder(path), row["available"] == .bool(true),
                  case .string(let kind)? = row["kind"], ["main", "linked"].contains(kind), seen.insert(path).inserted else { return nil }
            return LaunchWorktree(path: path, branch: row.string("branch"), kind: kind)
        }.prefix(512))
    }

    static func inside(_ path: String, _ root: String) -> Bool {
        LaunchPresentation.absoluteFolder(root) && (path == root || path.hasPrefix(root == "/" ? root : root + "/"))
    }

    public static func contains(_ raw: JSONObject?, _ path: String) -> Bool { entries(raw).contains { $0.path == path } }

    /// The catalog read for this exact selection: its requested or canonical folder.
    public static func forFolder(_ raw: JSONObject?, _ folder: String) -> JSONObject? {
        guard let raw, !folder.isEmpty, raw.string("requested_path") == folder || raw.string("path") == folder else { return nil }
        return raw
    }

    public static func anchor(_ raw: JSONObject?, _ project: LaunchProject, _ directory: String) -> LaunchProjectFolder? {
        guard healthy(raw), contains(raw, directory), let raw else { return nil }
        return project.folders.first { folder in
            contains(raw, folder.path) || (folder.path == raw.string("requested_path") && contains(raw, raw.string("path")))
        }
    }

    /// Desktop automaticWorktreeAvailable: a fresh, complete catalog of this selection inside a named, available checkout.
    public static func automaticAvailable(_ raw: JSONObject?, _ folder: String) -> Bool {
        guard let catalog = forFolder(raw, folder), catalog.string("state") == "ok", catalog["stale"] == .bool(false), catalog["partial"] != .bool(true),
              LaunchPresentation.absoluteFolder(catalog.string("common_dir")), !automaticRoot(catalog).isEmpty else { return false }
        let path = catalog.string("path")
        let selected = path.isEmpty ? folder : path
        return catalog.objects("worktrees").contains { row in
            row["available"] == .bool(true) && ["main", "linked"].contains(row.string("kind")) && inside(selected, row.string("path"))
        }
    }

    static func automaticRoot(_ raw: JSONObject?) -> String {
        var root = raw?.string("selected_root") ?? ""
        while root.hasSuffix("/") { root.removeLast() }
        return LaunchPresentation.absoluteFolder(root) && !LaunchPresentation.leaf(root).isEmpty ? root : ""
    }

    /// The generated sibling checkout `<root>-<branch>` that Auto creates when the session starts.
    public static func automaticPath(_ raw: JSONObject?, branch: String) -> String {
        let root = automaticRoot(raw)
        return root.isEmpty || branch.trimmingCharacters(in: .whitespaces).isEmpty ? "" : sibling(root, branch)
    }

    /// Why Auto is unavailable for the selected folder; "" when it is available or nothing is selected.
    public static func automaticNote(_ raw: JSONObject?, folder: String, supported: Bool, loading: Bool) -> String {
        let catalog = forFolder(raw, folder)
        if folder.isEmpty { return "" }
        if !supported { return "Update Zerus on this computer to create worktrees. The session starts in this folder." }
        if loading { return "Checking repository…" }
        if automaticAvailable(catalog, folder) { return "" }
        if catalog?.string("state") == "not_repo" { return "Not a Git repository. The session starts in this folder." }
        return "Repository details are unavailable. The session starts in this folder."
    }

    /// Creation evidence retained by the form: the confirmed checkout joins the repository it was created from.
    public static func withCreated(_ raw: JSONObject, path: String) -> JSONObject {
        var copy = raw
        var rows = raw.array("worktrees") ?? []
        if !rows.compactMap(\.object).contains(where: { $0.string("path") == path }) {
            rows.append(["path": .string(path), "kind": "linked", "available": true])
        }
        copy["worktrees"] = .array(rows)
        return copy
    }

    /// Fresh evidence first; retained creation evidence only when it alone proves a related project folder.
    public static func placement(fresh: JSONObject?, created: JSONObject?, project: LaunchProject?, directory: String) -> JSONObject? {
        guard let project else { return fresh }
        return [fresh, created].compactMap { $0 }.first { anchor($0, project, directory) != nil } ?? fresh
    }

    public static func createError(branch: String, base: String, destination: String) -> String {
        if branch.trimmingCharacters(in: .whitespaces).isEmpty || base.trimmingCharacters(in: .whitespaces).isEmpty { return "Enter a new branch and starting revision." }
        if [branch, base].contains(where: { $0.trimmingCharacters(in: .whitespacesAndNewlines) != $0 || $0.hasPrefix("-") || $0.utf16.count > 256 ||
            $0.unicodeScalars.contains(where: PairingInvite.isISOControl) }) { return "Use a valid branch and revision." }
        if !LaunchPresentation.absoluteFolder(destination) { return "Choose an absolute new folder path." }
        return ""
    }

    public static func destination(_ raw: JSONObject?, branch: String) -> String {
        var root = raw?.string("selected_root") ?? ""
        if root.isEmpty { root = raw?.string("path") ?? "" }
        while root.hasSuffix("/") { root.removeLast() }
        return LaunchPresentation.absoluteFolder(root) ? sibling(root, branch) : ""
    }

    static func sibling(_ root: String, _ branch: String) -> String {
        let safe = String(branch.map { $0.isLetter || $0.isNumber || "._-".contains($0) ? $0 : "-" })
        return "\(root)-\(safe)"
    }
}

/// Explicit folder creation is separate from browsing and native session launch.
public enum DirectoryCreation {
    public static let operation = "directory_create"
    public static let uncertain = "Folder creation is unconfirmed. Check its original receipt; it has not been retried."

    public static func pathError(_ path: String) -> String {
        if !path.hasPrefix("/") || path == "/" { return "Enter an absolute path for a new folder." }
        if path.utf8.count > 4096 || path.unicodeScalars.contains(where: PairingInvite.isISOControl) { return "Enter a valid native folder path." }
        if path.split(separator: "/", omittingEmptySubsequences: false).dropFirst().contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) {
            return "Use a path without empty, dot, or parent components."
        }
        return ""
    }

    public static func child(_ parent: String, _ name: String) -> String {
        if name.trimmingCharacters(in: .whitespaces).isEmpty || name == "." || name == ".." || name.contains("/") ||
            name.unicodeScalars.contains(where: PairingInvite.isISOControl) { return "" }
        return (parent == "/" ? "/" : parent + "/") + name
    }

    public static func computerTarget(_ action: SessionAction, _ machine: MachineKey) -> Bool {
        action.operation == operation && action.target == Target(computerId: machine.computerId, session: "", run: "", conversation: "", connectionId: machine.connectionId)
    }

    public static func pending(_ actions: [SessionAction], _ machine: MachineKey) -> SessionAction? {
        actions.last { computerTarget($0, machine) && $0.blocksSending }
    }

    public static func result(_ action: SessionAction, _ receipt: JSONObject) -> SessionAction {
        let native = receipt.object("result")
        let requested = action.arguments.string("path")
        let path = native?.string("path") ?? "", parent = native?.string("parent") ?? ""
        let target = action.target
        let computerOnly = target.session.isEmpty && target.run.isEmpty && target.conversation.isEmpty && target.archiveId.isEmpty && target.agentId.isEmpty
        let parentOfPath: String = {
            guard let slash = path.lastIndex(of: "/") else { return "" }
            let value = String(path[..<slash])
            return value.isEmpty ? "/" : value
        }()
        var copy = action
        guard computerOnly, AttachmentPolicy.validId(action.requestId), pathError(requested).isEmpty, receipt.string("state") == "completed",
              let native, native.string("request_id") == action.requestId, native.string("status") == "created", native.string("requested_path") == requested,
              pathError(path).isEmpty, LaunchPresentation.leaf(path) == LaunchPresentation.leaf(requested), LaunchPresentation.absoluteFolder(parent),
              parentOfPath == parent else {
            copy.status = "uncertain"
            copy.error = uncertain
            return copy
        }
        copy.status = "completed"
        copy.error = ""
        copy.resultPath = path
        return copy
    }
}
