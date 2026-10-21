import Foundation

public struct DetailTask: Hashable, Sendable {
    public let id: String
    public let title: String
    public let status: String
    public let owner: String
}

public struct DetailTaskList: Hashable, Sendable {
    public let owner: String
    public let label: String
    public let tasks: [DetailTask]
    public let recorded: Bool
    public let truncated: Bool
}

public struct DetailAgent: Hashable, Sendable {
    public let id: String
    public let label: String
    public let state: String
    public let task: String
    public let preview: String
    public let group: Bool
    public let recorded: Bool
    public let active: Int64?
    public let total: Int64?
}

public struct DetailProcess: Hashable, Sendable {
    public let id: String
    public let command: String
    public let description: String
    public let status: String
    public let owner: String
    public let directory: String
    public let run: String
    public let conversation: String
    public let stale: Bool
    public let output: Bool
    public let stop: Bool
}

public struct SessionDetailsView: Hashable, Sendable {
    public let fields: [ContextDetail]
    public let goal: [ContextDetail]
    public let goalNotice: String
    public let tasks: [DetailTaskList]
    public let agents: [DetailAgent]
    public let agentNotice: String
    public let processes: [DetailProcess]
    public let processNotes: [String]
    public let processComplete: Bool
    public let generation: String
    public let recorded: Bool
}

public struct NativeModelChoice: Hashable, Sendable {
    public let id: String
    public let label: String
    public let efforts: [String]
    /// The native names of `efforts` by position (OpenCode `effort_labels`); empty when not reported.
    public var effortLabels: [String] = []
    public var provider = "", providerLabel = ""
    public var free = false, favorite = false, isDefault = false
}

public struct NativeSettings: Hashable, Sendable {
    public let model: String
    public let effort: String
    public let models: [NativeModelChoice]
    public let efforts: [String]
    public let pendingModel: String
    public let pendingEffort: String
    public let pendingId: String
    public let applyWhen: String
    public let supported: Bool
    public let reason: String
    public let notice: String
    /// The inspected provider; only OpenCode offers native variants beyond the relay's fixed levels.
    public var agent = ""
    public var modelsTruncated = false

    /// Relevant semantic fields only; timestamps and unrelated history polls do not invalidate a dialog.
    public var signature: String {
        let choices: [JSON] = models.map { .array([.string($0.id), .string($0.label), JSON($0.efforts), JSON($0.effortLabels), .string($0.provider), .string($0.providerLabel), .bool($0.free), .bool($0.favorite), .bool($0.isDefault)]) }
        return JSON.array([.string(model), .string(effort), .array(choices), JSON(efforts), .string(pendingModel), .string(pendingEffort),
                           .string(pendingId), .string(applyWhen), .bool(supported), .string(reason), .string(notice), .bool(modelsTruncated)]).compact
    }

    /// The model's own effort variants (OpenCode `effort_options` with `effort_labels`), or the session's list when the
    /// model is not in the catalog, after the native default "". Variants beyond the relay's fixed levels are offered
    /// only for OpenCode when the relay and connector advertise `LaunchPresentation.effortFeature`.
    public func effortOptions(_ model: String, effortFeature: Bool) -> [EffortOption] {
        let choice = models.first { $0.id == model }
        let open = agent == "opencode" && effortFeature
        let offered = EffortOption.options(choice?.efforts ?? efforts, labels: choice?.effortLabels ?? [])
        return [EffortOption(value: "", label: EffortOption.text(""))] + offered.filter { open || SessionActionPolicies.efforts.contains($0.value) }
    }

    /// The selected effort after choosing `model`: kept only when that model offers it here, else the native default.
    public func keptEffort(_ effort: String, model: String, effortFeature: Bool) -> String {
        effortOptions(model, effortFeature: effortFeature).contains { $0.value == effort } ? effort : ""
    }

    /// The native name of `effort` for `model` (OpenCode `effort_labels`), or nil when it has none.
    public func effortLabel(_ effort: String, model: String) -> String? {
        guard !effort.isEmpty, let choice = models.first(where: { $0.id == model }), choice.effortLabels.count == choice.efforts.count,
              let index = choice.efforts.firstIndex(of: effort), LaunchPresentation.usableLabel(choice.effortLabels[index]) else { return nil }
        return choice.effortLabels[index]
    }
}

public enum SessionDetailsPresentation {
    static func numeric(_ raw: JSONObject, _ key: String) -> Int64? {
        guard let value = raw.number(key), value.isFinite, value >= 0, value <= 9_007_199_254_740_991 else { return nil }
        return Int64(value)
    }

    public static func settings(_ raw: JSONObject?) -> NativeSettings {
        let model = raw?.string("settings_model") ?? ""
        let options = raw?.objects("model_options") ?? []
        let openCode = raw?.string("agent") == "opencode" || options.contains { option in
            if case .string(let provider)? = option["provider"] { return !provider.isEmpty }
            return false
        }
        let models: [NativeModelChoice]
        if openCode {
            models = OpenCodeCatalog.models(raw?.array("model_options")).map { model in
                NativeModelChoice(id: model.value, label: model.label, efforts: model.efforts.map(\.value),
                    effortLabels: model.efforts.map(\.label), provider: model.provider, providerLabel: model.providerLabel,
                    free: model.free, favorite: model.favorite, isDefault: model.isDefault)
            }
        } else {
            models = options.map { option in
                let label = option.string("label")
                return NativeModelChoice(id: option.string("id"), label: label.isEmpty ? option.string("id") : label,
                    efforts: option.strings("effort_options"), effortLabels: option.strings("effort_labels"))
            }
        }
        return NativeSettings(
            model: model.isEmpty ? raw?.string("model") ?? "" : model, effort: raw?.string("effort") ?? "",
            models: models,
            efforts: raw?.strings("effort_options") ?? [], pendingModel: raw?.string("pending_model") ?? "", pendingEffort: raw?.string("pending_effort") ?? "",
            pendingId: raw?.string("pending_settings_id") ?? "", applyWhen: raw?.string("settings_apply_when") ?? "",
            supported: raw?.bool("settings_change_supported") == true, reason: raw?.string("settings_change_reason") ?? "",
            notice: raw?.string("settings_change_notice") ?? "", agent: raw?.string("agent") ?? "",
            modelsTruncated: openCode && (raw?["model_options_truncated"] == .bool(true) || options.count > OpenCodeCatalog.maxModels))
    }

    public static func parse(_ raw: JSONObject?, _ target: Target, verified: Bool) -> SessionDetailsView {
        let record = raw ?? JSONObject()
        let recorded = !verified || !target.archiveId.isEmpty
        var fields = [ContextDetail("Session", target.session)]
        for identifier in SessionReference.identifiers(target) {
            let parts = identifier.components(separatedBy: ": ")
            fields.append(ContextDetail(parts[0], parts.dropFirst().joined(separator: ": ")))
        }
        if !target.archiveId.isEmpty { fields.append(ContextDetail("Archive ID", target.archiveId)) }
        for (key, label) in [("agent", "Provider"), ("model", "Model"), ("effort", "Effort"), ("cwd", "Directory"), ("project", "Project")] {
            let value = record.string(key)
            if !value.isEmpty { fields.append(ContextDetail(label, value)) }
        }
        fields.append(ContextDetail("Process", record.string("process_state").ifEmpty("Not reported")))
        fields.append(ContextDetail("Activity", record.string("activity").ifEmpty("Not reported")))
        if !record.string("error").isEmpty { fields.append(ContextDetail("Native error", record.string("error"))) }

        var goal: [ContextDetail] = []
        if let goalRaw = record.object("goal"), !goalRaw.string("objective").isEmpty {
            goal.append(ContextDetail("Goal", goalRaw.string("objective")))
            goal.append(ContextDetail("Status", goalStatus(goalRaw)))
            for (key, label) in [("completion_criterion", "Completion criterion"), ("reason", "Reason")] where !goalRaw.string(key).isEmpty {
                goal.append(ContextDetail(label, goalRaw.string(key)))
            }
            if let used = numeric(goalRaw, "tokens_used") {
                let budget = numeric(goalRaw, "token_budget").flatMap { $0 > 0 ? " / \($0)" : nil } ?? " used"
                goal.append(ContextDetail("Tokens", "\(used)\(budget)"))
            }
            if let time = numeric(goalRaw, "time_used_seconds") { goal.append(ContextDetail("Recorded active time", elapsed(time))) }
            if let time = numeric(goalRaw, "time_budget_seconds") { goal.append(ContextDetail("Time budget", elapsed(time))) }
            for (used, budget, label) in [("rounds_used", "round_budget", "Goal rounds"), ("turns_used", "turn_budget", "Continuation turns")] {
                if let value = numeric(goalRaw, used) { goal.append(ContextDetail(label, "\(value)" + (numeric(goalRaw, budget).map { " / \($0)" } ?? ""))) }
            }
            if let checks = numeric(goalRaw, "evaluations") { goal.append(ContextDetail("Completion checks", "\(checks)")) }
        }

        let roster = record.object("subagents") ?? JSONObject()
        let taskLists = record.object("task_lists") ?? JSONObject()
        let owners = taskLists.keys.sorted { ($0 != "main" ? 1 : 0, $0) < ($1 != "main" ? 1 : 0, $1) }
        let tasks: [DetailTaskList] = owners.compactMap { owner in
            guard let list = taskLists.object(owner) else { return nil }
            let label: String
            if owner.hasPrefix("agent:") {
                let id = String(owner.dropFirst("agent:".count))
                label = (roster.object(id)?.string("label", "name") ?? "").ifEmpty(id)
            } else { label = list.string("source").hasPrefix("Task") ? "Session tasks" : "Main agent" }
            return DetailTaskList(owner: owner, label: label,
                                  tasks: list.objects("items").map { DetailTask(id: $0.string("id"), title: $0.string("title"), status: $0.string("status"), owner: $0.string("owner")) },
                                  recorded: recorded || list.string("run_id") != target.run, truncated: list.bool("truncated"))
        }

        let groups = record.string("subagent_source") == "hook_profiles"
        let agentRows = groups ? record.object("subagent_groups") ?? JSONObject() : roster
        var rows: [(String, JSONObject)] = agentRows.keys.compactMap { id in agentRows.object(id).map { (id, $0) } }
        if rows.isEmpty && !groups {
            rows = record.objects("subagent_previews").compactMap { child in
                let id = child.string("id", "agent_id")
                return id.isEmpty ? nil : (id, child)
            }
        }
        let agents = rows.map { id, child in
            DetailAgent(id: id, label: child.string("label", "name", "profile").ifEmpty(id), state: child.string("display_state", "state").ifEmpty("unknown"),
                        task: child.string("task", "description"), preview: child.string("preview", "last_message"), group: groups,
                        recorded: recorded || { let run = child.string("run_id"); return !run.isEmpty && run != target.run }(),
                        active: numeric(child, "active_count"), total: numeric(child, "total_count"))
        }

        let processesRaw = record.object("processes") ?? JSONObject()
        let processes = processesRaw.objects("items").map { process in
            DetailProcess(id: process.string("id"), command: process.string("command"), description: process.string("description"),
                          status: process.string("status").ifEmpty("unknown"), owner: process.string("owner"), directory: process.string("cwd"),
                          run: process.string("run_id"), conversation: process.string("conversation_id"),
                          stale: recorded || process.bool("stale") || process.string("run_id") != target.run || process.string("conversation_id") != target.conversation,
                          output: process.object("capabilities")?.bool("output") == true, stop: process.object("capabilities")?.bool("stop") == true)
        }
        let goalNotice = !goal.isEmpty ? "Reported by the agent. A finished response does not complete this goal."
            : record.string("goal_source") == "unavailable" ? "Goal data is unavailable for this conversation or agent version." : "No goal reported."
        let agentNotice = groups ? "Observed runs grouped by profile. Individual agent identities are not reported."
            : record.string("subagent_source") == "unavailable" ? "Subagent activity is unavailable for this session." : ""
        return SessionDetailsView(fields: fields, goal: goal, goalNotice: goalNotice, tasks: tasks, agents: agents, agentNotice: agentNotice,
                                  processes: processes, processNotes: processesRaw.strings("notes"), processComplete: processesRaw.bool("complete"),
                                  generation: processesRaw.string("generation").ifEmpty(record.string("generation")), recorded: recorded)
    }

    public static func taskStatus(_ status: String) -> String {
        switch status {
        case "completed": "Done"
        case "in_progress": "In progress"
        case "pending": "Planned"
        default: status.ifEmpty("Unknown")
        }
    }

    public static func agentStatus(_ status: String) -> String {
        switch status {
        case "finished", "ready", "idle": "Ready"
        case "working", "running", "busy": "Working"
        case "approval": "Needs approval"
        case "input": "Needs input"
        case "attention": "Needs attention"
        case "error", "failed": "Error"
        case "completed": "Finished"
        case "stopped": "Stopped"
        case "paused": "Paused"
        default: status.trimmingCharacters(in: .whitespaces).isEmpty ? "Unknown" : status
        }
    }

    public static func goalStatus(_ goal: JSONObject) -> String {
        switch goal.string("status") {
        case "active": goal.string("activation") == "disarmed" ? "Goal waiting to resume" : "Pursuing goal"
        case "paused": "Goal paused"
        case "blocked": "Goal blocked"
        case "usage_limited": "Goal usage limit reached"
        case "budget_limited": "Goal token budget reached"
        case "complete": "Goal complete"
        case let other: "Goal status unknown" + (other.isEmpty ? "" : " (\(other))")
        }
    }

    static func elapsed(_ seconds: Int64) -> String {
        if seconds >= 86400 { return "\(seconds / 86400)d \(seconds / 3600 % 24)h" }
        if seconds >= 3600 { return "\(seconds / 3600)h \(seconds / 60 % 60)m" }
        if seconds >= 60 { return "\(seconds / 60)m" }
        return "\(seconds)s"
    }
}

extension String {
    /// Kotlin `ifBlank`: the fallback when this text is empty or whitespace.
    func ifEmpty(_ fallback: @autoclosure () -> String) -> String {
        trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? fallback() : self
    }
}
