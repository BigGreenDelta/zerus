import Foundation

/// A display-only projection. Never retain provider payloads, credentials or arbitrary source strings.
public struct AccountIdentity: Codable, Hashable, Sendable {
    public var name = "", email = "", organization = "", plan = "", authMethod = "", accountId = ""
    public init(name: String = "", email: String = "", organization: String = "", plan: String = "", authMethod: String = "", accountId: String = "") {
        self.name = name
        self.email = email
        self.organization = organization
        self.plan = plan
        self.authMethod = authMethod
        self.accountId = accountId
    }
}

public struct AccountWindow: Codable, Hashable, Sendable {
    public var id: String
    public var label: String
    public var usedPercent: Double?
    public var minutes: Double?
    public var resetsAt: Double?
    public init(id: String, label: String, usedPercent: Double?, minutes: Double?, resetsAt: Double?) {
        self.id = id
        self.label = label
        self.usedPercent = usedPercent
        self.minutes = minutes
        self.resetsAt = resetsAt
    }
}

public struct AccountBalance: Codable, Hashable, Sendable {
    public var kind: String
    public var balance: String?
    public var currency: String
}

public struct ReportedAccount: Codable, Hashable, Sendable {
    public var id: String
    public var provider: String
    public var label: String
    public var native: Bool
    public var installed: Bool
    public var isDefault: Bool
    public var status: String
    public var authStatus: String
    public var identity: AccountIdentity
    public var windows: [AccountWindow]
    public var creditBalance: String?
    public var unlimited: Bool?
    public var balances: [AccountBalance]
    public var checkedAt: Double?
    public var identityCached: Bool
    public var refreshing: Bool
    public var refreshError: Bool
    public var stale: Bool
    public var openCode: OpenCodeAccountCatalog? = nil
}

public struct AccountCatalog: Codable, Hashable, Sendable {
    public var key: MachineKey
    public var machineName: String
    public var supported: Bool
    public var available: Bool
    public var stale: Bool
    public var online: Bool
    public var observedAt: Double?
    public var accounts: [ReportedAccount]
}

public enum AccountSnapshots {
    static let accountIdPattern = try! NSRegularExpression(pattern: "^[A-Za-z0-9_][A-Za-z0-9_-]{0,79}$")
    static let providers: Set<String> = ["codex", "claude", "kimi", "dsh", "opencode"]
    static let statuses: Set<String> = ["ok", "loading", "configured", "offline", "expired", "signed_out", "credentials_locked",
                                        "desktop_session_unavailable", "credentials_unavailable", "unavailable", "error", "unknown"]
    static let authStatuses: Set<String> = ["signed_in", "signed_out", "expired", "locked", "unavailable", "unknown", "configured", "credentials_locked"]

    static func str(_ raw: JSONObject, _ key: String, _ max: Int = 256) -> String {
        guard case .string(let value)? = raw[key], value.utf16.count <= max,
              !value.unicodeScalars.contains(where: { PairingInvite.isISOControl($0) && $0 != "\n" }) else { return "" }
        return value
    }

    static func number(_ raw: JSONObject, _ key: String) -> Double? {
        guard let value = raw[key]?.numberValue, value.isFinite, value >= 0, value <= 9_007_199_254_740_991 else { return nil }
        return value
    }

    /// A decimal string normalized like `BigDecimal.stripTrailingZeros().toPlainString()`.
    static func money(_ raw: JSONObject, _ key: String) -> String? {
        guard case .string(let value)? = raw[key], value.count <= 80,
              value.range(of: "^[+-]?[0-9]+(\\.[0-9]+)?$", options: .regularExpression) != nil else { return nil }
        var text = Substring(value)
        var negative = false
        if text.first == "+" || text.first == "-" { negative = text.first == "-"; text = text.dropFirst() }
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        var integer = String(parts[0].drop(while: { $0 == "0" }))
        var fraction = parts.count > 1 ? String(parts[1]) : ""
        while fraction.hasSuffix("0") { fraction.removeLast() }
        if integer.isEmpty { integer = "0" }
        let body = fraction.isEmpty ? integer : "\(integer).\(fraction)"
        return negative && body != "0" ? "-\(body)" : body
    }

    static func flag(_ raw: JSONObject, _ key: String) -> Bool { raw[key] == .bool(true) }

    static func reset(_ raw: JSONObject) -> Double? {
        if let value = number(raw, "resets_at") { return value }
        guard case .string(let text)? = raw["resets_at"], text.count <= 80 else { return nil }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime]
        var date = formatter.date(from: text)
        if date == nil {
            formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
            date = formatter.date(from: text)
        }
        guard let date, date.timeIntervalSince1970 >= 0 else { return nil }
        return date.timeIntervalSince1970.rounded(.down)
    }

    public static func parse(_ connectionId: String, _ computer: JSONObject) -> AccountCatalog {
        let key = MachineKey(connectionId: connectionId, computerId: str(computer, "id"))
        let snapshot = computer.object("snapshot")
        let raw = snapshot?.object("mobile_accounts")
        let declared = (snapshot?.object("mobile_capabilities")?.array("features") ?? []).contains(.string("accounts_snapshot"))
        let schema = raw?["schema"]?.numberValue
        let online = flag(computer, "online")
        guard declared, let raw, schema == 1, JSON.object(raw).compact.utf8.count <= 128 * 1024 else {
            return AccountCatalog(key: key, machineName: str(computer, "name"), supported: false, available: false, stale: true, online: online, observedAt: nil, accounts: [])
        }
        let list = raw.array("accounts")
        let eligible = flag(raw, "available") && list != nil && list!.count <= 100
        var seen = Set<String>()
        let accounts = eligible ? list!.compactMap { $0.object.flatMap(account) }.filter { seen.insert($0.provider + "\u{0}" + $0.id).inserted } : []
        let complete = eligible && accounts.count == list!.count
        return AccountCatalog(key: key, machineName: str(computer, "name"), supported: true, available: complete,
                              stale: flag(raw, "stale") || !online || !complete, online: online, observedAt: number(raw, "checked_at"), accounts: accounts)
    }

    static func account(_ raw: JSONObject) -> ReportedAccount? {
        let id = str(raw, "id", 80), provider = str(raw, "provider", 16)
        guard accountIdPattern.firstMatch(in: id, range: NSRange(id.startIndex..., in: id)) != nil, providers.contains(provider) else { return nil }
        let usage = raw.object("usage") ?? JSONObject()
        let identity = usage.object("identity") ?? JSONObject()
        let windows = (usage.array("windows") ?? []).prefix(16).compactMap(\.object).map {
            AccountWindow(id: str($0, "id", 80), label: str($0, "label", 160), usedPercent: number($0, "used_percent"),
                          minutes: number($0, "window_minutes"), resetsAt: reset($0))
        }
        let balances = (usage.array("balances") ?? []).prefix(16).compactMap(\.object).map {
            let currency = str($0, "currency", 16)
            return AccountBalance(kind: str($0, "kind", 80), balance: money($0, "balance"), currency: ["USD", "CNY"].contains(currency) ? currency : "")
        }
        let credits = usage.object("credits") ?? JSONObject()
        let label = str(raw, "label", 160)
        let status = str(usage, "status", 48), auth = str(usage, "auth_status", 48)
        var unlimited: Bool?
        if case .bool(let value)? = credits["unlimited"] { unlimited = value }
        return ReportedAccount(id: id, provider: provider, label: label.trimmingCharacters(in: .whitespaces).isEmpty ? id : label,
                               native: flag(raw, "native"), installed: flag(raw, "installed"), isDefault: flag(raw, "is_default"),
                               status: statuses.contains(status) ? status : "unknown", authStatus: authStatuses.contains(auth) ? auth : "unknown",
                               identity: AccountIdentity(name: str(identity, "name"), email: str(identity, "email"), organization: str(identity, "organization"),
                                                         plan: str(identity, "plan"), authMethod: str(identity, "auth_method", 80), accountId: str(identity, "account_id")),
                               windows: windows, creditBalance: money(credits, "balance"), unlimited: unlimited, balances: balances,
                               checkedAt: number(usage, "checked_at"), identityCached: flag(usage, "identity_cached"), refreshing: flag(usage, "refreshing"),
                               refreshError: flag(usage, "refresh_error"), stale: flag(usage, "stale"), openCode: OpenCodeCatalog.account(raw))
    }

    /// The typed fields of one account, as the connector reports them; anything else is dropped.
    static func accountJSON(_ a: ReportedAccount) -> JSONObject {
        func optional(_ value: Double?) -> JSON { value.map(JSON.double) ?? .null }
        let identity: JSONObject = ["name": .string(a.identity.name), "email": .string(a.identity.email), "organization": .string(a.identity.organization),
                                    "plan": .string(a.identity.plan), "auth_method": .string(a.identity.authMethod), "account_id": .string(a.identity.accountId)]
        let windows: [JSON] = a.windows.map { .object(["id": .string($0.id), "label": .string($0.label), "used_percent": optional($0.usedPercent),
                                                       "window_minutes": optional($0.minutes), "resets_at": optional($0.resetsAt)]) }
        let balances: [JSON] = a.balances.map { .object(["kind": .string($0.kind), "balance": $0.balance.map(JSON.string) ?? .null, "currency": .string($0.currency)]) }
        let credits: JSONObject = ["balance": a.creditBalance.map(JSON.string) ?? .null, "unlimited": a.unlimited.map(JSON.bool) ?? .null]
        let usage: JSONObject = ["status": .string(a.status), "auth_status": .string(a.authStatus), "identity": .object(identity), "windows": .array(windows),
                                 "credits": .object(credits), "balances": .array(balances), "checked_at": optional(a.checkedAt),
                                 "identity_cached": .bool(a.identityCached), "refreshing": .bool(a.refreshing), "refresh_error": .bool(a.refreshError), "stale": .bool(a.stale)]
        return ["id": .string(a.id), "provider": .string(a.provider), "label": .string(a.label), "native": .bool(a.native), "installed": .bool(a.installed),
                "is_default": .bool(a.isDefault), "usage": .object(usage)]
    }

    /// A successful catalog is authoritative for removals. Failed workspaces keep explicitly stale values.
    public static func merge(_ previous: [AccountCatalog], _ incoming: [AccountCatalog], successful: Set<String>, connected: Set<String>) -> [AccountCatalog] {
        var old: [MachineKey: AccountCatalog] = [:]
        for catalog in previous where old[catalog.key] == nil { old[catalog.key] = catalog }
        let updated = incoming.filter { connected.contains($0.key.connectionId) }.map { next -> AccountCatalog in
            guard let prior = old[next.key] else { return next }
            if let observed = next.observedAt, let priorObserved = prior.observedAt, observed < priorObserved {
                var copy = prior
                copy.stale = true
                copy.online = next.online
                return copy
            }
            if !next.available {
                var copy = next
                copy.accounts = prior.accounts
                copy.stale = true
                return copy
            }
            return next
        }
        let retained = previous.filter { connected.contains($0.key.connectionId) && !successful.contains($0.key.connectionId) }.map { catalog -> AccountCatalog in
            var copy = catalog
            copy.online = false
            copy.stale = true
            return copy
        }
        var seen = Set<MachineKey>()
        return Array((updated + retained).filter { seen.insert($0.key).inserted }.prefix(64))
    }

    /// Cached catalogs are last-known data: always stale and offline until refreshed.
    public static func restored(_ catalogs: [AccountCatalog]) -> [AccountCatalog] {
        catalogs.prefix(64).map { catalog in
            var copy = catalog
            copy.stale = true
            copy.online = false
            return copy
        }
    }

    public static func demo(now: Double) -> [AccountCatalog] {
        func personal(_ id: String = "sample", isDefault: Bool = true, refreshError: Bool = false) -> JSON {
            ["id": .string(id), "provider": "codex", "installed": true, "native": true, "label": "Personal (preview)", "is_default": .bool(isDefault),
             "usage": ["status": "ok", "checked_at": .double(now), "refresh_error": .bool(refreshError),
                       "identity": ["name": "Example account", "plan": "Example plan", "account_id": "example-personal", "email": "personal@example.com"],
                       "windows": [["id": "current", "label": "Session", "used_percent": 42, "window_minutes": 300, "resets_at": .double(now + 7200)],
                                   ["id": "ended", "label": "Weekly", "used_percent": 91, "window_minutes": 10080, "resets_at": .double(now - 60)]]]]
        }
        func alias(_ profile: String, _ accountId: String) -> JSON {
            ["id": .string(profile), "provider": "codex", "installed": true, "native": true, "label": "Default account", "is_default": false,
             "usage": ["status": "ok", "checked_at": .double(now),
                       "identity": ["name": "Example account", "plan": "Example plan", "account_id": .string(accountId), "email": "shared@example.com"]]]
        }
        let work: JSON = ["id": "unknown", "provider": "claude", "installed": true, "native": true, "label": "Work (preview)",
                          "usage": ["status": "unavailable", "identity": ["plan": "Not reported"]]]
        func catalog(_ id: String, _ name: String, _ online: Bool, _ accounts: [JSON]) -> AccountCatalog {
            parse("demo", ["id": .string(id), "name": .string(name), "online": .bool(online),
                           "snapshot": ["mobile_accounts": ["schema": 1, "available": true, "checked_at": .double(now), "accounts": .array(accounts)],
                                        "mobile_capabilities": ["features": ["accounts_snapshot"]]]])
        }
        return [catalog("demo", "Preview machine", true, [personal(), work, alias("shared-one", "example-shared-one"), alias("shared-two", "example-shared-two")]),
                catalog("demo-laptop", "Preview laptop", true, [personal("laptop-personal", isDefault: false), alias("shared-email", "")]),
                catalog("demo-build", "Preview build machine with a long display name", false, [personal("build-personal", refreshError: true)])]
    }
}

public struct AccountSelection: Hashable, Sendable {
    public var machine: MachineKey
    public var provider: String
    public var accountId: String
    public init(machine: MachineKey, provider: String, accountId: String) {
        self.machine = machine
        self.provider = provider
        self.accountId = accountId
    }
}

public enum AccountPresentation {
    public static func selected(_ catalogs: [AccountCatalog], _ selection: AccountSelection) -> (AccountCatalog, ReportedAccount)? {
        guard let catalog = catalogs.first(where: { $0.key == selection.machine }),
              let account = catalog.accounts.first(where: { $0.provider == selection.provider && $0.id == selection.accountId }) else { return nil }
        return (catalog, account)
    }

    public static func resetSummary(_ window: AccountWindow, now: Double) -> String {
        switch remaining(window, now: now) {
        case "Unknown": "Reset unknown"
        case "Ended": "Window ended"
        case let value: "Resets in \(value)"
        }
    }

    public static func remaining(_ window: AccountWindow, now: Double) -> String {
        guard let reset = window.resetsAt, reset.isFinite, reset >= 0, reset < 253_402_300_800, now.isFinite else { return "Unknown" }
        if reset <= now { return "Ended" }
        let minutes = Int64(((reset - now) / 60).rounded(.up))
        if minutes >= 1440 { return "\(minutes / 1440)d" + (minutes % 1440 / 60 > 0 ? " \(minutes % 1440 / 60)h" : "") }
        if minutes >= 60 { return "\(minutes / 60)h" + (minutes % 60 > 0 ? " \(minutes % 60)m" : "") }
        return "\(minutes)m"
    }

    public static func warning(_ account: ReportedAccount) -> Bool {
        ["expired", "signed_out", "credentials_locked", "desktop_session_unavailable", "credentials_unavailable", "error"].contains(account.status) ||
            ["expired", "signed_out", "locked", "credentials_locked"].contains(account.authStatus)
    }

    public static func type(_ account: ReportedAccount) -> String {
        let plan = account.identity.plan.trimmingCharacters(in: .whitespaces)
        if !plan.isEmpty { return account.identity.plan }
        return account.identity.authMethod.trimmingCharacters(in: .whitespaces).isEmpty ? "Plan not reported" : account.identity.authMethod
    }

    public static func compactStatus(_ catalog: AccountCatalog, _ account: ReportedAccount) -> String {
        let problem: String = switch account.authStatus {
        case "expired": "Sign-in expired"
        case "signed_out": "Signed out"
        case "locked", "credentials_locked": "Credentials locked"
        default: warning(account) || account.status == "offline" ? status(account) : ""
        }
        return [problem, catalog.online ? "" : "Offline", account.installed ? "" : "Provider not installed",
                account.refreshError ? "Refresh failed" : (account.refreshing ? "Refreshing" : "")].filter { !$0.isEmpty }.joined(separator: " / ")
    }

    public static func stale(_ catalog: AccountCatalog, _ account: ReportedAccount, now: Double) -> Bool {
        guard catalog.online, !catalog.stale, !account.stale, !account.refreshError, let checked = account.checkedAt else { return true }
        return checked > now + 60 || now - checked > 600
    }

    public static func ended(_ window: AccountWindow, now: Double) -> Bool { window.resetsAt.map { $0 <= now } == true }

    public static func percent(_ value: Double?) -> String {
        guard let value, var decimal = Decimal(string: String(value)) else { return "Usage unknown" }
        var rounded = Decimal()
        NSDecimalRound(&rounded, &decimal, 1, value < 0 ? .down : .plain)
        return "\(NSDecimalNumber(decimal: rounded).stringValue)% used"
    }

    public static func date(_ seconds: Double?) -> String? { MachineCatalog.displayDate(seconds) }

    static func duration(_ window: AccountWindow) -> String {
        guard let minutes = window.minutes, minutes > 0, minutes <= 52_560_000, minutes.truncatingRemainder(dividingBy: 1) == 0 else { return "" }
        let n = Int64(minutes)
        return n % 1440 == 0 ? "\(n / 1440)d" : n % 60 == 0 ? "\(n / 60)h" : "\(n)m"
    }

    public static func shortPeriod(_ window: AccountWindow) -> String {
        let value = duration(window)
        if !value.isEmpty { return value }
        let label = window.label.trimmingCharacters(in: .whitespaces).isEmpty ? "Window" : window.label
        let scalars = Array(label.unicodeScalars)
        if scalars.count <= 12 { return label }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars.prefix(12))
        return String(view) + "…"
    }

    public static func period(_ window: AccountWindow) -> String {
        [window.label.trimmingCharacters(in: .whitespaces).isEmpty ? "Usage window" : window.label, duration(window)].filter { !$0.isEmpty }.joined(separator: " ")
    }

    public static func status(_ account: ReportedAccount) -> String {
        switch account.status {
        case "ok": "Reported usage"
        case "configured": "Configured"
        case "loading": "Loading provider data"
        case "expired": "Sign-in expired"
        case "signed_out": "Signed out"
        case "credentials_locked": "Credentials locked"
        case "desktop_session_unavailable": "Desktop session unavailable"
        case "credentials_unavailable": "Credentials unavailable"
        case "offline": "Provider offline"
        default: "Usage unavailable"
        }
    }
}

/// Connection IDs delimit paired workspaces; labels, endpoints and profile names are presentation.
public struct AccountGroupKey: Hashable, Sendable {
    public var connectionId: String
    public var provider: String
    public var kind: String
    public var identity: String
    public var scope: String
    public var computerId = ""
    public var profileId = ""
}

public struct AccountMember: Hashable, Sendable {
    public let catalog: AccountCatalog
    public let account: ReportedAccount
    public var selection: AccountSelection { AccountSelection(machine: catalog.key, provider: account.provider, accountId: account.id) }
}

public struct GroupedAccount: Hashable, Sendable, Identifiable {
    public let key: AccountGroupKey
    public let label: String
    public let members: [AccountMember]
    public let snapshot: AccountMember
    public var id: AccountGroupKey { key }
}

/// Mirrors tray/src/AccountCatalog.h: provider identity owns quotas; profiles remain exact machine bindings.
public enum AccountGrouping {
    public static let unicodeFold: @Sendable (String) -> String = { $0.folding(options: [.caseInsensitive], locale: nil) }

    static func key(_ member: AccountMember, _ fold: (String) -> String, emailOnly: Bool = false) -> AccountGroupKey {
        let account = member.account, identity = account.identity
        let email = fold(identity.email.trimmingCharacters(in: .whitespacesAndNewlines))
        if account.status == "signed_out" || (email.isEmpty && identity.accountId.isEmpty) {
            return AccountGroupKey(connectionId: member.catalog.key.connectionId, provider: account.provider, kind: "profile", identity: "", scope: "",
                                   computerId: member.catalog.key.computerId, profileId: account.id)
        }
        let byId = !emailOnly && !identity.accountId.isEmpty
        return AccountGroupKey(connectionId: member.catalog.key.connectionId, provider: account.provider, kind: byId ? "id" : "email",
                               identity: byId ? identity.accountId : email, scope: fold(identity.organization.trimmingCharacters(in: .whitespacesAndNewlines)))
    }

    static func named(_ label: String) -> Bool { !label.isEmpty && label != "Default account" && label != "Native DeepSeek account" }

    static func quality(_ member: AccountMember) -> Int {
        ["ok", "configured"].contains(member.account.status) ? (member.catalog.online ? 3 : 2) : 1
    }

    public static func group(_ catalogs: [AccountCatalog], fold: (String) -> String = unicodeFold) -> [GroupedAccount] {
        let members = catalogs.flatMap { catalog in catalog.accounts.map { AccountMember(catalog: catalog, account: $0) } }
        var aliases: [AccountGroupKey: Set<AccountGroupKey>] = [:]
        for member in members where member.account.status != "signed_out" && !member.account.identity.accountId.isEmpty && !member.account.identity.email.isEmpty {
            aliases[key(member, fold, emailOnly: true), default: []].insert(key(member, fold))
        }
        var groups: [AccountGroupKey: [AccountMember]] = [:]
        var order: [AccountGroupKey] = []
        for member in members {
            let initial = key(member, fold)
            let id = aliases[initial].flatMap { $0.count == 1 ? $0.first : nil } ?? initial
            if groups[id] == nil { order.append(id) }
            groups[id, default: []].append(member)
        }
        return order.map { id in
            let rows = groups[id]!
            let best = rows.dropFirst().reduce(rows[0]) { previous, next in
                quality(next) > quality(previous) || quality(next) == quality(previous) && (next.account.checkedAt ?? 0) > (previous.account.checkedAt ?? 0) ? next : previous
            }
            let label = rows.first { named($0.account.label) }?.account.label ??
                (best.account.identity.email.trimmingCharacters(in: .whitespaces).isEmpty ? rows[0].account.label : best.account.identity.email)
            return GroupedAccount(key: id, label: label, members: rows, snapshot: best)
        }
    }

    /// Recompute against current membership, anchored to the exact profile originally opened.
    public static func selected(_ groups: [GroupedAccount], _ selection: AccountSelection) -> GroupedAccount? {
        groups.first { $0.members.contains { $0.selection == selection } }
    }
}
