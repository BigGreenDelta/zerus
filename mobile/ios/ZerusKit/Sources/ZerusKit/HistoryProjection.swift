import Foundation

/// Same-epoch display corrections. These values never grant read or send authority.
public enum HistoryProjection {
    public static let maxBytes = 880 * 1024
    public static let maxPages = 4
    public struct Entry: Sendable {
        public var generation: Int64
        public var id: String
        public var change: String
        public var successor: Event?
        public var resolved: Bool
    }
    public struct Page: Sendable {
        public var entries: [Entry]
        public var next: Int64
        public var complete: Bool
        public var prefix: [Entry] { Array(entries.prefix(while: \.resolved)) }
        public func applied(after: Int64) -> Int64 { prefix.count == entries.count && complete ? next : (prefix.last?.generation ?? after) }
    }
    public struct Run: Sendable {
        public let start: Int64
        public let until: Int64
        public private(set) var after: Int64
        public private(set) var pages = 0
        public private(set) var stopped = false
        public init(start: Int64, until: Int64) { precondition(start >= 0 && until >= start); self.start = start; self.until = until; after = start }
        public var canRead: Bool { pages < maxPages && after < until && !stopped }
        public mutating func accepted(_ page: Page) {
            precondition(canRead)
            after = page.applied(after: after)
            pages += 1
            stopped = page.complete || page.prefix.count != page.entries.count
        }
    }
    public static func supported(relay: Set<String>?, native: Set<String>?) -> Bool {
        relay?.contains("history_projection") == true && native?.contains("history_projection") == true
    }
    static func require(_ value: Bool) throws { if !value { throw ZerusError.mismatch("Computer returned invalid history changes.") } }
    static func number(_ raw: JSONObject, _ key: String, missing: Int64? = nil) throws -> Int64 {
        if raw[key] == nil, let missing { return missing }
        guard case .int(let value)? = raw[key], value >= 0 else { throw ZerusError.mismatch("Computer returned invalid history changes.") }
        return value
    }
    public static func generation(_ raw: JSONObject) throws -> Int64 {
        guard let upgrade = raw.object("source_status")?.object("projection_upgrade") else { return 0 }
        return try number(upgrade, "generation", missing: 0)
    }
    static func text(_ value: JSON?, _ maximum: Int) throws -> String {
        guard case .string(let string)? = value, !string.isEmpty, string.utf8.count <= maximum,
              !string.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else {
            throw ZerusError.mismatch("Computer returned invalid history changes.")
        }
        return string
    }
    static func identifier(_ value: JSON?) throws -> String {
        let id = try text(value, 64)
        try require(id.utf8.count == 64 && id.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) })
        return id
    }
    /// Compact Python ASCII JSON, without the platform JSON writer's float/escape differences.
    static func asciiBytes(_ value: JSON) throws -> Int {
        func string(_ value: String) -> Int {
            2 + value.utf16.reduce(0) { total, unit in
                total + ([8, 9, 10, 12, 13, 34, 92].contains(Int(unit)) ? 2 : ((32...126).contains(unit) ? 1 : 6))
            }
        }
        switch value {
        case .null, .bool(true): return 4
        case .bool(false): return 5
        case .int(let number): return String(number).count
        case .double(let number):
            try require(number.isFinite)
            return pythonFloat(number).count
        case .string(let text): return string(text)
        case .array(let values): return try 2 + values.reduce(0) { try $0 + asciiBytes($1) } + max(0, values.count - 1)
        case .object(let values): return try 2 + values.reduce(0) { try $0 + string($1.key) + 1 + asciiBytes($1.value) } + max(0, values.count - 1)
        }
    }
    static func pythonFloat(_ value: Double) -> String {
        let raw = String(value).lowercased(), sign = raw.hasPrefix("-") ? "-" : ""
        let unsigned = raw.hasPrefix("-") ? String(raw.dropFirst()) : raw
        let parts = unsigned.split(separator: "e", omittingEmptySubsequences: false)
        let mantissa = String(parts[0]), exponent = parts.count > 1 ? Int(parts[1])! : 0
        let point = mantissa.firstIndex(of: ".").map { mantissa.distance(from: mantissa.startIndex, to: $0) } ?? mantissa.count
        let all = mantissa.replacingOccurrences(of: ".", with: "")
        let first = all.firstIndex(where: { $0 != "0" }).map { all.distance(from: all.startIndex, to: $0) } ?? all.count
        let digits = String(all.dropFirst(first).reversed().drop(while: { $0 == "0" }).reversed())
        if digits.isEmpty { return sign + "0.0" }
        let power = exponent + point - first - 1
        if !(-4..<16).contains(power) {
            let fraction = digits.count > 1 ? "." + String(digits.dropFirst()) : ""
            return sign + String(digits.prefix(1)) + fraction + "e" + (power < 0 ? "-" : "+") + String(format: "%02d", abs(power))
        }
        let position = power + 1
        if position <= 0 { return sign + "0." + String(repeating: "0", count: -position) + digits }
        if position >= digits.count { return sign + digits + String(repeating: "0", count: position - digits.count) + ".0" }
        return sign + String(digits.prefix(position)) + "." + String(digits.dropFirst(position))
    }
    public static func payload(_ target: Target, id: String, epoch: String, after: Int64, until: Int64) throws -> JSONObject {
        try require(after >= 0 && after <= until)
        _ = try text(.string(epoch), 128)
        var raw = try HistoryRequests.payload(target, id: id)
        raw["limit"] = nil
        raw["history_epoch"] = .string(epoch)
        raw["projection_after"] = .int(after)
        raw["projection_until"] = .int(until)
        return raw
    }
    public static func parse(target: Target, id: String, epoch: String, after: Int64, until: Int64, raw: JSONObject) throws -> Page {
        try require(asciiBytes(.object(raw)) <= maxBytes && after >= 0 && until >= after)
        try require(raw["request_id"] == .string(id) && raw["name"] == .string(target.session) && raw["run_id"] == .string(target.run) && raw["conversation_id"] == .string(target.conversation))
        func echo(_ field: String, _ expected: String) -> Bool { raw[field].map { $0 == .string(expected) } ?? expected.isEmpty }
        try require(echo("archive_id", target.archiveId) && echo("agent_id", target.agentId) &&
                    echo("parent_conversation_id", target.agentId.isEmpty ? "" : target.parentConversation))
        try require(raw["history_epoch"] == .string(epoch) && number(raw, "projection_generation") >= until)
        guard case .bool(let complete)? = raw["complete"], let values = raw.array("projection_changes"), values.count <= 256 else {
            throw ZerusError.mismatch("Computer returned invalid history changes.")
        }
        let next = try number(raw, "next_projection_after")
        var previous = after
        let entries = try values.map { value -> Entry in
            guard let row = value.object else { throw ZerusError.mismatch("Computer returned invalid history changes.") }
            let required: Set<String> = ["generation", "history_id", "change", "resolution", "successor"]
            try require(required.isSubset(of: Set(row.keys)) && Set(row.keys).subtracting(required.union(["reason"])).isEmpty)
            let generation = try number(row, "generation")
            try require(generation > previous && generation <= until)
            previous = generation
            let old = try identifier(row["history_id"])
            let change = row.string("change"), resolution = row.string("resolution")
            try require(change == "retired" || change == "refreshed")
            var successor: Event?
            if resolution == "unresolved" {
                try require(row["successor"] == .null && (row["reason"] == nil || ["no_successor", "unexpected_successor", "oversize"].contains(row.string("reason"))))
            } else if resolution == "resolved" {
                try require(row["reason"] == nil)
                if change == "retired" { try require(row["successor"] == .null) }
                else {
                    guard let body = row.object("successor") else { throw ZerusError.mismatch("Computer returned invalid history changes.") }
                    _ = try identifier(body["history_id"])
                    _ = try text(body["history_cursor"], 8192)
                    try require(["journal", "provider", "attachment", "voice"].contains(body.string("history_stream")) && body["native_match_hash"] == nil)
                    try require(["UserPromptSubmit", "UserPromptQueued", "UserMessage", "TurnStarted", "QuestionAnswered", "AgentMessage", "Stop"].contains(body.string("type")))
                    guard let aliases = body.array("original_ids"), aliases.count <= 8 else { throw ZerusError.mismatch("Computer returned invalid history changes.") }
                    for alias in aliases { _ = try text(alias, 8192) }
                    if let sequence = body["incoming_seq"], !sequence.isNull { _ = try number(body, "incoming_seq") }
                    if let agent = body["agent_id"], !agent.isNull {
                        try require(agent == .string("") || agent == .string(target.agentId.isEmpty ? "main" : target.agentId))
                    }
                    let parsed = NativeEvents.events(["history_epoch": .string(epoch), "agent_id": .string(target.agentId), "events": .array([.object(body)])])
                    try require(parsed.count == 1)
                    successor = parsed[0]
                    try require(parsed[0].at.isFinite && parsed[0].historyEpoch == epoch && parsed[0].id == "history:\(parsed[0].historyId)")
                }
            } else { throw ZerusError.mismatch("Computer returned invalid history changes.") }
            return Entry(generation: generation, id: old, change: change, successor: successor, resolved: resolution == "resolved")
        }
        try require(complete ? next == until : (!entries.isEmpty && next == previous && next < until))
        return Page(entries: entries, next: next, complete: complete)
    }
    /// Rebase the validated prefix on the latest exact-scope window. No unseen range is imported.
    public static func apply(_ window: HistoryWindow, target: Target, epoch: String, start: Int64, entries: [Entry], applied: Int64) -> HistoryWindow? {
        guard window.target == target, window.epoch == epoch else { return nil }
        var rows = window.events
        var dropped = false
        for entry in entries {
            precondition(entry.resolved)
            if entry.generation <= window.projectionApplied { continue } // A newer page already covers this correction.
            let old = rows.first { $0.id == "history:\(entry.id)" && $0.historyId == entry.id && $0.historyEpoch == epoch }
            if entry.change == "retired" {
                if let old, old.role == "You", old.requestId.isEmpty, old.outgoingId.isEmpty, old.voice == nil,
                   !["hgs_delivery", "mobile_outbox", "hgs_voice"].contains(old.source) { rows.removeAll { $0.id == old.id } }
            } else {
                guard let successor = entry.successor else { preconditionFailure("Missing history successor") }
                let known = rows.contains { $0.id == successor.id && $0.historyEpoch == epoch }
                if old == nil && !known { continue }
                if let old, old.id != successor.id {
                    rows = rows.compactMap { $0.id == old.id ? (known ? nil : successor) : $0 }
                }
                let merged = RollingConversationHistory.merge(target: target, previousTarget: target, previous: rows, incoming: [successor])
                rows = merged.events
                dropped = dropped || merged.truncated
            }
        }
        var result = window
        result.events = rows
        if dropped { result.hasBefore = true; result.before = rows.first(where: { !$0.historyCursor.isEmpty })?.historyCursor }
        result.projectionApplied = window.projectionApplied >= start ? max(window.projectionApplied, applied) : window.projectionApplied
        return result
    }
}
