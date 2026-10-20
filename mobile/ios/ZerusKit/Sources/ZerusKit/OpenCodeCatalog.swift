import Foundation

public struct OpenCodeModel: Hashable, Sendable {
    public let value, label, provider, providerLabel: String
    public let free, favorite, isDefault: Bool
    public let efforts: [EffortOption]
    public init(value: String, label: String, provider: String = "", providerLabel: String = "", free: Bool = false,
                favorite: Bool = false, isDefault: Bool = false, efforts: [EffortOption] = []) {
        self.value = value; self.label = label; self.provider = provider; self.providerLabel = providerLabel
        self.free = free; self.favorite = favorite; self.isDefault = isDefault; self.efforts = efforts
    }
}
public struct OpenCodeProvider: Codable, Hashable, Sendable {
    public var id, label, kind: String
    public var credentials: Int?
    public var activeLabel: String
    public var models: Int?
}
public struct OpenCodeGroup: Hashable, Sendable {
    public let provider, label: String
    public let models: [OpenCodeModel]
}
public struct OpenCodeAccountCatalog: Codable, Hashable, Sendable {
    public var state, reason: String
    public var catalogAt: Double?
    public var providers: [OpenCodeProvider]
    public var providersTruncated, modelsTruncated: Bool
}

/// The bounded display projection shared by launch, session settings and account details.
public enum OpenCodeCatalog {
    public static let maxModels = 256, maxProviders = 64
    private static let kinds: Set<String> = ["free", "key", "oauth", "env", "config", "command", "external", "local"]
    private static func text(_ raw: JSONObject, _ key: String, _ max: Int) -> String {
        guard case .string(let value)? = raw[key], value.utf16.count <= max,
              !value.trimmingCharacters(in: .whitespaces).isEmpty,
              !value.unicodeScalars.contains(where: PairingInvite.isISOControl) else { return "" }
        return value
    }
    private static func validProvider(_ value: String) -> Bool {
        value.range(of: "^[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}$", options: .regularExpression) != nil
    }
    private static func flag(_ raw: JSONObject, _ key: String) -> Bool { raw[key] == .bool(true) }
    private static func count(_ raw: JSONObject, _ key: String, _ max: Int) -> Int? {
        guard let value = raw[key]?.numberValue, value.isFinite, value >= 0, value <= Double(max), value.rounded() == value else { return nil }
        return Int(value)
    }
    public static func models(_ raw: [JSON]?) -> [OpenCodeModel] {
        var seen = Set<String>(), result: [OpenCodeModel] = []
        for item in raw ?? [] {
            guard let row = item.object, case .string(let value)? = row["value"] ?? row["id"],
                  LaunchPresentation.validModel(value), seen.insert(value).inserted else { continue }
            let candidate = text(row, "provider", 80)
            let provider = validProvider(candidate) ? candidate : ""
            let label = text(row, "label", 160), providerLabel = text(row, "provider_label", 160)
            result.append(OpenCodeModel(value: value, label: label.isEmpty ? value : label, provider: provider,
                providerLabel: providerLabel.isEmpty ? provider : providerLabel, free: flag(row, "free"),
                favorite: flag(row, "favorite"), isDefault: flag(row, "default"),
                efforts: EffortOption.options(row.strings("effort_options"), labels: row.strings("effort_labels"))))
            if result.count == maxModels { break }
        }
        return result
    }
    public static func providers(_ raw: [JSON]?, limit: Int = maxProviders) -> [OpenCodeProvider] {
        let cap = min(maxProviders, max(0, limit))
        guard cap > 0 else { return [] }
        var seen = Set<String>(), result: [OpenCodeProvider] = []
        for item in raw ?? [] {
            guard let row = item.object else { continue }
            let id = text(row, "id", 80)
            guard validProvider(id), seen.insert(id).inserted else { continue }
            let label = text(row, "label", 160), kind = text(row, "kind", 32)
            result.append(OpenCodeProvider(id: id, label: label.isEmpty ? id : label, kind: kinds.contains(kind) ? kind : "",
                credentials: count(row, "credential_count", 64), activeLabel: text(row, "active_label", 80), models: count(row, "model_count", 10_000)))
            if result.count == cap { break }
        }
        return result
    }
    public static func account(_ raw: JSONObject) -> OpenCodeAccountCatalog? {
        guard raw["provider"] == .string("opencode") else { return nil }
        let at = (raw["catalog_at"]?.numberValue).flatMap { $0.isFinite && $0 >= 0 && $0 <= 9_007_199_254_740_991 ? $0 : nil }
        return OpenCodeAccountCatalog(state: text(raw, "catalog_state", 32), reason: text(raw, "catalog_reason", 500), catalogAt: at,
            providers: providers(raw.array("providers")),
            providersTruncated: flag(raw, "providers_truncated") || (raw.array("providers")?.count ?? 0) > maxProviders,
            modelsTruncated: flag(raw, "model_options_truncated") || (raw.array("model_options")?.count ?? 0) > maxModels)
    }
    public static func group(_ models: [OpenCodeModel]) -> [OpenCodeGroup] {
        var order: [String] = [], groups: [String: [OpenCodeModel]] = [:]
        for model in models {
            if groups[model.provider] == nil { order.append(model.provider) }
            groups[model.provider, default: []].append(model)
        }
        return order.map { id in
            let entries = groups[id]!, label = groups[id]!.first!.providerLabel
            return OpenCodeGroup(provider: id, label: label.isEmpty ? (id.isEmpty ? "Other models" : id) : label, models: entries)
        }
    }
    public static func filter(_ models: [OpenCodeModel], query: String) -> [OpenCodeModel] {
        let words = query.split(whereSeparator: \.isWhitespace).map(String.init)
        return models.filter { model in words.allSatisfy { word in
            [model.label, model.value, model.providerLabel, model.provider].contains {
                $0.range(of: word, options: .caseInsensitive, locale: Locale(identifier: "en_US_POSIX")) != nil
            }
        } }
    }
    public static func marks(_ model: OpenCodeModel) -> [String] {
        [(model.isDefault, "Default"), (model.favorite, "Favorite"), (model.free, "Free")].compactMap { $0.0 ? $0.1 : nil }
    }
    public static func kindLabel(_ kind: String) -> String {
        switch kind {
        case "free": "Free, no key"
        case "key": "API key"
        case "oauth": "Signed in"
        case "env": "Environment variable"
        case "config": "Configuration"
        case "command": "Command"
        case "external": "External credentials"
        case "local": "Detected locally"
        default: "Connected"
        }
    }
    public static func stateMessage(state: String, reason: String) -> String {
        if state == "ready" { return "" }
        if state == "loading" { return "OpenCode is still loading providers." }
        if !reason.isEmpty { return reason }
        switch state {
        case "not_running": return "OpenCode is not running on this computer."
        case "disabled": return "The OpenCode service is disabled on this computer."
        case "incompatible": return "This OpenCode version is not supported."
        case "unreachable": return "OpenCode did not answer."
        default: return ""
        }
    }
    public static func truncationMessage(models: Bool, providers: Bool) -> String {
        [(models, "Showing a shortened list. Choose other models on the desktop."), (providers, "Some providers are not shown.")]
            .compactMap { $0.0 ? $0.1 : nil }.joined(separator: " ")
    }
}
