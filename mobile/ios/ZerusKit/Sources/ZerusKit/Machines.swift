import Foundation

public struct MachineKey: Hashable, Codable, Sendable {
    public var connectionId: String
    public var computerId: String
    public init(connectionId: String, computerId: String) {
        self.connectionId = connectionId
        self.computerId = computerId
    }
}

public enum MachineRoute: Hashable, Sendable {
    case unknown
    case direct
    case via(gatewayId: String, gatewayName: String)
}

/// One computer row of a paired workspace, ported from Android `Machine`.
public struct Machine: Hashable, Sendable, Identifiable {
    public var connectionId: String
    public var id: String
    public var name: String
    public var online: Bool
    public var nativeName: String
    public var serverAliases: Set<String>
    public var route: MachineRoute
    public var lastKnown: Bool
    public var hgsVersion: String
    public var mobileDelivery: String
    public var lastSeenAt: Double?
    public var machineId: String
    public var nativeHost: String

    public init(connectionId: String, id: String, name: String, online: Bool, nativeName: String? = nil,
                serverAliases: Set<String> = [], route: MachineRoute = .unknown, lastKnown: Bool = false,
                hgsVersion: String = "", mobileDelivery: String = "", lastSeenAt: Double? = nil,
                machineId: String = "", nativeHost: String = "") {
        self.connectionId = connectionId
        self.id = id
        self.name = name
        self.online = online
        self.nativeName = nativeName ?? name
        self.serverAliases = serverAliases
        self.route = route
        self.lastKnown = lastKnown
        self.hgsVersion = hgsVersion
        self.mobileDelivery = mobileDelivery
        self.lastSeenAt = lastSeenAt
        self.machineId = machineId
        self.nativeHost = nativeHost
    }

    public var key: MachineKey { MachineKey(connectionId: connectionId, computerId: id) }
}

public struct MachineTreeRow: Hashable, Sendable {
    public let machine: Machine
    public let parent: Machine?
    public let gatewayName: String
}

public struct MachineTreeGroup: Hashable, Sendable {
    public let root: MachineTreeRow
    public let children: [MachineTreeRow]
}

/// Selected relay routes describe one hop, not every enrollment of a computer.
public enum MachineCatalog {
    static let maxAliases = 4096
    static let versionFormat = try! NSRegularExpression(pattern: "^[0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$")

    public static func parse(_ connectionId: String, _ raw: JSONObject) throws -> Machine {
        guard let idValue = raw["id"], let id = idValue.textValue else { throw ZerusError.invalid("A computer row has no identity.") }
        let snapshot = raw.object("snapshot")
        var version = ""
        if case .string(let text)? = snapshot?["hgs_version"], text.utf16.count <= 128,
           versionFormat.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil, !text.contains("\n") {
            version = text
        }
        var lastSeen: Double?
        if let value = raw["last_seen_at"]?.numberValue, value.isFinite, value >= 0 { lastSeen = value }
        var host = ""
        if case .string(let text)? = snapshot?["host"], text.utf16.count <= 80 { host = text }
        let name = raw.string("name", "id")
        return Machine(connectionId: connectionId, id: id, name: name, online: raw.bool("online"),
                       serverAliases: aliases(raw["aliases"]), route: route(raw),
                       hgsVersion: version, mobileDelivery: MobileDelivery.parse(snapshot), lastSeenAt: lastSeen,
                       machineId: canonicalUUID(raw["machine_id"]) ?? "", nativeHost: host)
    }

    /// Relay availability only; an offline row is never a usable target.
    public static func status(_ machine: Machine) -> String {
        if machine.lastKnown { return "Last known" }
        if machine.online { return "Online" }
        if let date = displayDate(machine.lastSeenAt) { return "Offline · last seen \(date)" }
        return "Offline"
    }

    public static func displayDate(_ seconds: Double?) -> String? {
        guard let seconds, seconds.isFinite, seconds >= 0, seconds < 253_402_300_800 else { return nil }
        return Date(timeIntervalSince1970: seconds.rounded(.down)).formatted(date: .abbreviated, time: .shortened)
    }

    public static func gateway(_ machine: Machine, _ catalog: [Machine]) -> Machine? {
        GatewayIndex(catalog.filter { $0.connectionId == machine.connectionId }).resolve(machine)
    }

    /// Only explicit direct routes can be parents of a one-level presentation group.
    public static func directGateway(_ machine: Machine, _ catalog: [Machine]) -> Machine? {
        gateway(machine, catalog).flatMap { $0.route == .direct ? $0 : nil }
    }

    public static func gatewayName(_ machine: Machine, _ catalog: [Machine]) -> String {
        if let gateway = gateway(machine, catalog) { return gateway.name }
        if case .via(_, let name) = machine.route { return name }
        return ""
    }

    public static func groupsByConnection(_ catalog: [Machine]) -> [String: [MachineTreeGroup]] {
        Dictionary(grouping: catalog, by: \.connectionId).mapValues { groups($0.first!.connectionId, $0) }
    }

    public static func tree(_ connectionId: String, _ catalog: [Machine]) -> [MachineTreeRow] {
        groups(connectionId, catalog).flatMap { [$0.root] + $0.children }
    }

    public static func groups(_ connectionId: String, _ catalog: [Machine]) -> [MachineTreeGroup] {
        var seen = Set<String>()
        let machines = catalog.filter { $0.connectionId == connectionId && seen.insert($0.id).inserted }
        let index = GatewayIndex(machines)
        let rows = machines.map { machine -> MachineTreeRow in
            let gateway = index.resolve(machine)
            var name = gateway?.name ?? ""
            if gateway == nil, case .via(_, let caption) = machine.route { name = caption }
            return MachineTreeRow(machine: machine, parent: gateway?.route == .direct ? gateway : nil, gatewayName: name)
        }
        var children: [String: [MachineTreeRow]] = [:]
        for row in rows { if let parent = row.parent { children[parent.id, default: []].append(row) } }
        let roots = rows.filter { $0.machine.route == .direct } + rows.filter { $0.machine.route != .direct && $0.parent == nil }
        return roots.map { MachineTreeGroup(root: $0, children: children[$0.machine.id] ?? []) }
    }

    private struct GatewayIndex {
        var identities: [String: Machine?] = [:]
        init(_ machines: [Machine]) {
            for machine in machines {
                for identity in machine.serverAliases.sorted() + [machine.id] {
                    if let existing = identities[identity] {
                        if existing?.id != machine.id { identities[identity] = .some(nil) }
                    } else {
                        identities[identity] = machine
                    }
                }
            }
        }
        func resolve(_ machine: Machine) -> Machine? {
            guard case .via(let gatewayId, _) = machine.route, let found = identities[gatewayId], let gateway = found,
                  gateway.id != machine.id else { return nil }
            return gateway
        }
    }

    static func route(_ raw: JSONObject) -> MachineRoute {
        guard let value = raw["via"] else { return .unknown }
        if value.isNull { return .direct }
        guard let via = value.object, let id = canonicalUUID(via["gateway_id"]) else { return .unknown }
        var name = ""
        if case .string(let caption)? = via["gateway_name"], caption.unicodeScalars.count <= 128,
           !caption.unicodeScalars.contains(where: { isControlOrFormat($0) }) {
            name = caption
        }
        return .via(gatewayId: id, gatewayName: name)
    }

    static func aliases(_ value: JSON?) -> Set<String> {
        guard let values = value?.array, values.count <= maxAliases else { return [] }
        var result = Set<String>()
        for item in values {
            guard let id = canonicalUUID(item) else { return [] }
            result.insert(id)
        }
        return result
    }

    /// A lowercase canonical UUID string, or nil.
    public static func canonicalUUID(_ value: JSON?) -> String? {
        guard case .string(let text)? = value, text.count == 36, let uuid = UUID(uuidString: text),
              uuid.uuidString.lowercased() == text else { return nil }
        return text
    }

    static func isControlOrFormat(_ scalar: Unicode.Scalar) -> Bool {
        scalar.value <= 0x1F || (0x7F...0x9F).contains(scalar.value) || scalar.properties.generalCategory == .format
    }
}

public struct MachineAlias: Hashable, Codable, Sendable {
    public var key: MachineKey
    public var name: String
    public init(key: MachineKey, name: String) {
        self.key = key
        self.name = name
    }
}

public enum MachineNames {
    public static func checked(_ value: String) throws -> String {
        let name = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, name.unicodeScalars.count <= 80, !name.unicodeScalars.contains(where: MachineCatalog.isControlOrFormat) else {
            throw ZerusError.invalid("Use a name of 1–80 characters without control characters.")
        }
        return name
    }

    public static func apply(_ machine: Machine, _ aliases: [MachineAlias]) -> Machine {
        var copy = machine
        copy.name = aliases.first { $0.key == machine.key }?.name ?? machine.nativeName
        return copy
    }
}

public struct MachineColorOverride: Hashable, Codable, Sendable {
    public var key: MachineKey
    public var hex: String
    public init(key: MachineKey, hex: String) {
        self.key = key
        self.hex = hex
    }
}

/// Phone-local appearance uses immutable computer identity, never the editable label.
public enum MachineColors {
    public static let palette = ["#5fbfa5", "#8ea9f4", "#c098e6", "#e7ae66", "#e38f9e", "#75bfcf", "#acc577"]
    public static let names = ["Mint", "Blue", "Purple", "Amber", "Rose", "Cyan", "Lime"]

    public static func checked(_ value: String) throws -> String {
        let scalars = Array(value.unicodeScalars)
        guard scalars.count == 7, scalars[0] == "#",
              scalars.dropFirst().allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) || ("A"..."F").contains($0) }) else {
            throw ZerusError.invalid("Use a six-digit RGB color.")
        }
        return value.lowercased()
    }

    public static func automatic(_ key: MachineKey) -> String {
        var hash: UInt64 = 2_166_136_261
        for byte in key.computerId.utf8 { hash = ((hash ^ UInt64(byte)) &* 16_777_619) & 0xFFFF_FFFF }
        return palette[Int(((hash >> 16) ^ hash) % UInt64(palette.count))]
    }

    public static func color(_ overrides: [MachineColorOverride], _ key: MachineKey) -> String {
        overrides.first { $0.key == key }?.hex ?? automatic(key)
    }

    /// Exact desktop MachineAppearance tonal blends, as opaque RGB.
    public static func foreground(_ hex: String, dark: Bool) throws -> UInt32 {
        try blend(hex, other: dark ? 0xFFFFFF : 0x111820, amount: dark ? 0.38 : 0.48)
    }
    public static func background(_ hex: String, dark: Bool) throws -> UInt32 {
        try blend(hex, other: dark ? 0x171E25 : 0xFFFFFF, amount: dark ? 0.78 : 0.83)
    }

    private static func blend(_ hex: String, other: UInt32, amount: Double) throws -> UInt32 {
        let rgb = UInt32(try checked(hex).dropFirst(), radix: 16)!
        var result: UInt32 = 0
        for shift: UInt32 in [16, 8, 0] {
            let mixed = Double((rgb >> shift) & 255) * (1 - amount) + Double((other >> shift) & 255) * amount
            result |= UInt32(mixed.rounded(.toNearestOrAwayFromZero)) << shift
        }
        return result
    }
}

/// The swarm-wide phone delivery choice; the gateway computer's hgs stores it.
public enum MobileDelivery {
    public static let operation = "set_mobile_delivery"
    public static let values = ["immediate", "away", "away_5", "away_10", "away_15", "away_30"]

    public static func label(_ value: String) -> String {
        switch value {
        case "immediate": "Immediately, even if I'm at a computer"
        case "away": "As soon as I'm away"
        case "away_5": "After 5 minutes away"
        case "away_10": "After 10 minutes away"
        case "away_15": "After 15 minutes away"
        case "away_30": "After 30 minutes away"
        default: ""
        }
    }

    /// Unknown or missing values stay unreported instead of pretending to be the default.
    public static func parse(_ snapshot: JSONObject?) -> String {
        guard case .string(let value)? = snapshot?.object("preferences")?["mobile_delivery"], values.contains(value) else { return "" }
        return value
    }
}
