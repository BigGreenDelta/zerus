import Foundation

/// One subagent row beneath an expanded session card.
public struct SessionCardChild: Hashable, Sendable {
    public let id: String
    public let name: String
    public let status: String
    public let detail: String
    public let provider: String
    public let kind: DesktopBadgeKind
    public let group: Bool
    public var description: String { "Subagent \(name), \(status)" }
}

/// The card's subagent counter: compact label, spoken form, and whether it shows child rows.
public struct SessionChildCounter: Hashable, Sendable {
    public let label: String
    public let description: String
    public let expandable: Bool
}

/// The card's optional-question chip: compact count and its spoken form. Information only.
public struct OptionalQuestionChip: Hashable, Sendable {
    public let count: Int
    public let label: String
    public let description: String
}

/// Session card checkout and subagent semantics from the desktop (SessionPresentation.h,
/// SessionsWindow.cpp), ported from Android `SessionCardPresentation`.
public enum SessionCardPresentation {
    // Qt reads these fields strictly: a value of another JSON type is absent.
    static func text(_ raw: JSONObject, _ key: String) -> String { if case .string(let value)? = raw[key] { value } else { "" } }
    static func flag(_ raw: JSONObject, _ key: String) -> Bool { raw[key] == .bool(true) }
    static func integer(_ raw: JSONObject, _ key: String, _ fallback: Int) -> Int {
        guard let value = raw[key]?.numberValue, value.isFinite, value == value.rounded(.down),
              value >= Double(Int32.min), value <= Double(Int32.max) else { return fallback }
        return Int(value)
    }
    static func simplified(_ value: String) -> String {
        value.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    /// The branch, or detached HEAD. Ordinary folders have none.
    public static func branch(_ raw: JSONObject) -> String {
        if text(raw, "git_metadata_state") == "not_repo" { return "" }
        let branch = text(raw, "git_branch")
        return !branch.isEmpty ? branch : (flag(raw, "git_detached") ? "detached HEAD" : "")
    }

    static func linked(_ raw: JSONObject) -> Bool { flag(raw, "git_worktree") && text(raw, "git_metadata_state") != "not_repo" }

    static func parts(_ path: String) -> [String] { path.split(whereSeparator: { $0 == "/" || $0 == "\\" }).map(String.init) }

    /// checkoutFolder: a linked worktree names its main checkout.
    public static func folder(_ raw: JSONObject) -> String {
        let common = linked(raw) ? parts(text(raw, "git_common_dir")) : []
        var repository = (common.count > 1 && common.last!.hasPrefix(".") ? Array(common.dropLast()) : common).last ?? ""
        if repository.count > 4 && repository.hasSuffix(".git") { repository = String(repository.dropLast(4)) }
        if !repository.isEmpty { return repository }
        let root = text(raw, "git_metadata_state") == "not_repo" ? "" : text(raw, "git_root")
        let source = !root.isEmpty ? root : (!text(raw, "cwd").isEmpty ? text(raw, "cwd") : text(raw, "cwd_canonical"))
        return parts(source).last ?? ""
    }

    /// The card's checkout line: folder / branch (or detached HEAD); an ordinary folder shows its name.
    public static func line(_ raw: JSONObject) -> String { [folder(raw), branch(raw)].filter { !$0.isEmpty }.joined(separator: " / ") }

    /// projectContext: the card line plus a linked-worktree mark.
    public static func checkout(_ raw: JSONObject) -> String {
        let value = line(raw)
        return linked(raw) && !value.isEmpty ? "\(value) · worktree" : value
    }

    /// The spoken checkout line keeps the linked worktree's own folder.
    public static func checkoutDescription(_ raw: JSONObject) -> String {
        let folder = folder(raw), branch = branch(raw), name = text(raw, "git_worktree_name")
        let worktree = branch.isEmpty || !linked(raw) ? "" : (name.isEmpty ? ", linked worktree" : ", linked worktree \(name)")
        let branchText = branch.isEmpty ? "" : (branch == "detached HEAD" ? "Detached HEAD" : "Git branch \(branch)")
        return [folder.isEmpty ? "" : "Folder \(folder)", branchText].filter { !$0.isEmpty }.joined(separator: ", ") + worktree
    }

    public static func childState(_ child: JSONObject) -> String {
        let display = text(child, "display_state")
        return display.isEmpty ? text(child, "state") : display
    }

    public static func inactiveChild(_ child: JSONObject, countsComplete: Bool = false) -> Bool {
        let state = childState(child)
        let inactive = ["finished", "idle", "ready"].contains(state)
        // Explicit display evidence wins over an older raw turn state or count.
        if !state.isEmpty && !inactive { return false }
        if flag(child, "group") { return countsComplete && child["active_count"]?.numberValue == 0 }
        return inactive
    }

    public static func childStatus(_ child: JSONObject) -> String {
        switch childState(child) {
        case "working": "Working"
        case "finished", "ready", "idle": "Ready"
        case "error": "Error"
        case "paused": "Paused"
        case "approval", "input", "attention": "Needs input"
        default: "Unknown"
        }
    }

    /// Only explicit requests for user intervention escalate; results and failures belong to the parent.
    public static func childAction(_ child: JSONObject) -> Bool { ["approval", "input", "attention"].contains(childState(child)) }

    public static func childName(_ child: JSONObject, _ id: String) -> String {
        let label: String
        if case .string(let value)? = child["label"] { label = value }
        else if case .string(let value)? = child["name"] { label = value }
        else { label = "" }
        return label.isEmpty ? id : label
    }

    /// Profile groups have no individual identities, so they never become rows.
    public static func roster(_ raw: JSONObject) -> [(String, JSONObject)] {
        if text(raw, "subagent_source") == "hook_profiles" { return [] }
        guard let roster = raw.object("subagents") else { return [] }
        // Stable ID order avoids moving a row under the finger when a child finishes.
        return roster.keys.sorted().map { ($0, roster.object($0) ?? JSONObject()) }
    }

    /// Children with an unsent draft on this phone stay visible after their turn finishes.
    public static func draftedChildren(_ parent: Target, _ drafts: [Draft]) -> Set<String> {
        Set(drafts.filter { draft in
            let child = draft.target
            return !child.agentId.isEmpty && draft.hasContent &&
                child.with(conversation: child.parentConversation, agentId: "", parentConversation: "") == parent
        }.map(\.target.agentId))
    }

    /// The expanded rows. Finished, idle and ready turns are hidden unless completed children are included.
    public static func children(_ session: Session, _ online: Bool, includeCompleted: Bool = false, drafted: Set<String> = []) -> [SessionCardChild] {
        let current = SessionFilters.currentActivity(session, online)
        let complete = flag(session.raw, "subagent_counts_complete")
        return roster(session.raw).filter { id, child in
            includeCompleted || !flag(child, "group") && drafted.contains(id) || !inactiveChild(child, countsComplete: complete)
        }.map { id, child in
            let status = current ? childStatus(child) : "Recorded"
            let kind: DesktopBadgeKind = current && childAction(child) ? .attention
                : current && childState(child) == "working" ? .working
                : status == "Paused" ? .paused : .neutral
            let detail: String
            if case .string(let value)? = child["detail"] { detail = value } else { detail = text(child, "task") }
            let provider: String
            if case .string(let value)? = child["provider"] { provider = value } else { provider = session.agent }
            return SessionCardChild(id: id, name: childName(child, id), status: status, detail: simplified(detail), provider: provider,
                                    kind: kind, group: flag(child, "group"))
        }
    }

    /// Compact counter forms such as 3, 2+, 1/3, 2+/5, 3/? and ?; expanded forms are spoken.
    public static func childCount(_ session: Session, _ online: Bool, expanded: Bool = false, includeCompleted: Bool = false) -> String {
        let raw = session.raw
        let source = text(raw, "subagent_source")
        if !SessionFilters.currentActivity(session, online) || source == "unavailable" { return "" }
        let count = integer(raw, "subagent_count", 0)
        let active = max(integer(raw, "subagent_active_count", count), count)
        let total = integer(raw, "subagent_total_count", -1)
        let complete = flag(raw, "subagent_counts_complete")
        if source == "hook_profiles" && !complete { return expanded ? "Observed runs: incomplete counts" : "?" }
        if !includeCompleted {
            if source != "hook_profiles" && !complete && total > 0 { return expanded ? "\(active) confirmed active; some states unknown" : "\(active)+" }
            if active > 0 { return expanded ? "\(active) active" : "\(active)" }
            return ""
        }
        if source != "hook_profiles" && !complete && total > 0 {
            return expanded ? "\(active) confirmed active / \(total) observed; some states unknown" : "\(active)+/\(total)"
        }
        if total > 0 {
            return expanded ? (source == "hook_profiles" ? "\(active) active / \(total) observed runs" : "\(active) active / \(total) total") : "\(active)/\(total)"
        }
        if total < 0 && source == "hook_profiles" { return expanded ? "Observed runs: count unavailable" : "?" }
        if total < 0 && active > 0 { return expanded ? "\(active) active / total unknown" : "\(active)/?" }
        return ""
    }

    /// A counter without a count still opens visible rows, labeled Agents.
    public static func counter(_ session: Session, _ online: Bool, _ children: [SessionCardChild], includeCompleted: Bool = false) -> SessionChildCounter? {
        let count = childCount(session, online, includeCompleted: includeCompleted)
        let expandable = !children.isEmpty
        let label = count.isEmpty && expandable ? "Agents" : count
        if label.isEmpty { return nil }
        let spoken = childCount(session, online, expanded: true, includeCompleted: includeCompleted)
        return SessionChildCounter(label: label, description: spoken.isEmpty ? "Subagents" : "Subagents, \(spoken)", expandable: expandable)
    }

    /// Pending optional Codex questions in the current conversation (optional-question-contract.json).
    public static func optionalQuestions(_ session: Session, _ online: Bool) -> Int {
        let raw = session.raw
        if !online || SessionFilters.archived(session) || SessionFilters.state(session) != "running" || text(raw, "process_state") == "exited" { return 0 }
        if case .object(let summary)? = raw["optional_questions"] {
            let count = integer(summary, "count", -1)
            if count >= 0 { return count }
        }
        var ids = Set<String>()
        for item in raw.objects("mobile_attention") where QuestionPolicies.optionalAttention(item) {
            if case .string(let id)? = item["question_id"], !id.trimmingCharacters(in: .whitespaces).isEmpty { ids.insert(id) }
        }
        return ids.count
    }

    public static func optionalQuestionLabel(_ count: Int) -> String { count > 99 ? "99+" : String(count) }

    public static func optionalQuestionDescription(_ count: Int) -> String {
        count == 1 ? "1 optional question waiting" : "\(count) optional questions waiting"
    }

    public static func optionalQuestionChip(_ session: Session, _ online: Bool) -> OptionalQuestionChip? {
        let count = optionalQuestions(session, online)
        guard count > 0 else { return nil }
        return OptionalQuestionChip(count: count, label: optionalQuestionLabel(count), description: optionalQuestionDescription(count))
    }

    /// Native child inspection accepts only these agent identities.
    public static func openableAgent(_ id: String) -> Bool {
        !id.trimmingCharacters(in: .whitespaces).isEmpty && id != "main" && id.utf16.count <= 256 &&
            !id.unicodeScalars.contains { PairingInvite.isISOControl($0) || $0 == "/" || $0 == "\\" }
    }
}

public enum QuestionPolicies {
    /// Whether one snapshot `mobile_attention` item is an optional Codex question the agent can
    /// continue without. Older connectors omit `optional`; `codex-async:` IDs are optional.
    public static func optionalAttention(_ item: JSONObject) -> Bool {
        if let value = item["optional"] { return value == .bool(true) }
        if case .string(let id)? = item["question_id"] { return id.hasPrefix("codex-async:") }
        return false
    }
}

public struct SessionReferenceValue: Hashable, Sendable {
    public let uri: String?
    public let reason: String
}

public enum SessionReference {
    public static func identifiers(_ target: Target) -> [String] {
        var result: [String] = []
        if !target.conversation.isEmpty { result.append("Session ID: \(target.conversation)") }
        else if target.agentId.isEmpty && !target.run.isEmpty { result.append("Run ID: \(target.run)") }
        if !target.agentId.isEmpty { result.append("Agent ID: \(target.agentId)") }
        return result
    }

    static let hostPattern = try! NSRegularExpression(pattern: "^[A-Za-z0-9_.][A-Za-z0-9_.-]{0,79}$")

    /// Relay IDs and display labels route the phone; only native identity belongs in a portable reference.
    public static func resolve(_ target: Target, _ machines: [Machine]) -> SessionReferenceValue {
        let candidates = machines.filter {
            $0.connectionId == target.connectionId && !target.computerId.isEmpty && ($0.id == target.computerId || $0.serverAliases.contains(target.computerId))
        }
        guard candidates.count == 1, let machine = candidates.first else {
            return SessionReferenceValue(uri: nil, reason: "Refresh the computer list to verify this session's computer.")
        }
        guard let id = UUID(uuidString: machine.machineId), id.uuidString.lowercased() == machine.machineId,
              machine.machineId != "00000000-0000-0000-0000-000000000000" else {
            return SessionReferenceValue(uri: nil, reason: "This computer has not reported its Zerus identity. Refresh its connection.")
        }
        guard hostPattern.firstMatch(in: machine.nativeHost, range: NSRange(machine.nativeHost.startIndex..., in: machine.nativeHost)) != nil else {
            return SessionReferenceValue(uri: nil, reason: "This computer has not reported its native host name. Refresh its connection.")
        }
        let conversation = target.agentId.isEmpty ? target.conversation : target.parentConversation
        if !target.agentId.isEmpty && conversation.isEmpty {
            return SessionReferenceValue(uri: nil, reason: "The parent conversation identity is not available yet.")
        }
        if target.session.isEmpty || !target.session.contains("/") || (conversation.isEmpty && target.run.isEmpty) {
            return SessionReferenceValue(uri: nil, reason: "The session identity is not available yet. Refresh the session.")
        }
        let fields = [("machine", machine.machineId), ("host", machine.nativeHost), ("name", target.session), ("conversation", conversation),
                      ("run", target.run), ("archive", target.archiveId), ("agent", target.agentId)]
        if fields.contains(where: { $0.1.utf8.count > 512 || $0.1.unicodeScalars.contains(where: PairingInvite.isISOControl) }) {
            return SessionReferenceValue(uri: nil, reason: "The reported session identity cannot form a valid reference.")
        }
        let allowed = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._")
        func encode(_ value: String) -> String { value.addingPercentEncoding(withAllowedCharacters: allowed) ?? "" }
        let uri = "zerus://session?" + fields.filter { !$0.1.isEmpty }.map { "\($0.0)=\(encode($0.1))" }.joined(separator: "&")
        return uri.count <= 8192 ? SessionReferenceValue(uri: uri, reason: "")
            : SessionReferenceValue(uri: nil, reason: "The reported session identity is too long to copy as a reference.")
    }
}
