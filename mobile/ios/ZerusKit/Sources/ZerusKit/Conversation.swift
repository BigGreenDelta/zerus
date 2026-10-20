import Foundation

// MARK: - Context and cache

public enum ContextTone: Sendable { case muted, normal, warning, critical }

public struct ContextDetail: Hashable, Sendable {
    public let label: String
    public let value: String
    public init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }
}

public struct ContextView: Hashable, Sendable {
    public let summary: String
    public let tone: ContextTone
    public let cacheSummary: String
    public let cacheTone: ContextTone
    public let compactionSummary: String
    public let compacting: Bool
    public let cold: Bool
    public let recorded: Bool
    public let details: [ContextDetail]
    public let notes: [String]
    public let compactReason: String
    public let clearReason: String

    public func value(_ label: String) -> String? { details.first { $0.label == label }?.value }
}

/// Presentation of native SessionUsage/CacheStatus fields, without inferred limits or cache lifetimes.
public enum ContextPresentation {
    static let maxNumber = 9007199254740991.0
    static let maxTimestamp = 253402300799.0

    static func number(_ data: JSONObject?, _ key: String) -> Double? {
        guard let value = data?.number(key), value.isFinite, value >= 0, value <= maxNumber else { return nil }
        return value
    }

    static func expiry(_ raw: JSONObject?) -> Double? {
        guard let value = number(raw?.object("session_usage")?.object("prompt_cache"), "expires_at"), value > 0, value <= maxTimestamp else { return nil }
        return value
    }

    public static func isCold(_ raw: JSONObject?, now: Double = Date().timeIntervalSince1970) -> Bool {
        let cache = raw?.object("session_usage")?.object("prompt_cache")
        return raw?.object("cache_hint")?.string("status") == "cold" || cache?.string("status") == "warm" && (expiry(raw).map { $0 <= now } ?? false)
    }

    /// Cheap clock key: formatting and detail parsing need change only once per remaining minute.
    public static func countdownMinute(_ raw: JSONObject?, now: Double) -> Int64? {
        if isCold(raw, now: now) { return 0 }
        guard raw?.object("session_usage")?.object("prompt_cache")?.string("status") == "warm", let deadline = expiry(raw) else { return nil }
        return max(Int64(((deadline - now) / 60).rounded(.up)), 0)
    }

    public static func from(_ raw: JSONObject?, recorded: Bool, now: Double = Date().timeIntervalSince1970,
                            locale: Locale = .current, zone: TimeZone = .current) -> ContextView {
        let usage = raw?.object("session_usage")
        let context = usage?.object("context")
        let cache = usage?.object("prompt_cache")
        let hint = raw?.object("cache_hint")
        let used = number(context, "used")
        let limit = number(context, "limit").flatMap { $0 > 0 ? $0 : nil }
        let percent = used.flatMap { used in limit.map { used / $0 * 100 } }.flatMap { $0.isFinite ? $0 : nil }
        let approximate = context?.bool("estimated") == true

        func decimal(_ value: Double, _ places: Int) -> String {
            let formatter = NumberFormatter()
            formatter.locale = locale
            formatter.numberStyle = .decimal
            formatter.minimumFractionDigits = places
            formatter.maximumFractionDigits = places
            formatter.roundingMode = .halfUp
            return formatter.string(from: NSNumber(value: value)) ?? String(value)
        }
        func tokens(_ value: Double?) -> String { value.map { decimal(Double(Int64($0)), 0) } ?? "Not reported" }
        func short(_ value: Double) -> String {
            if value < 1000 { return tokens(value) }
            let formatter = NumberFormatter()
            formatter.locale = locale
            formatter.numberStyle = .decimal
            formatter.maximumFractionDigits = 1
            formatter.roundingMode = .halfUp
            let scaled = value / (value >= 1_000_000 ? 1_000_000 : 1000)
            return (formatter.string(from: NSNumber(value: scaled)) ?? String(scaled)) + (value >= 1_000_000 ? "M" : "k")
        }
        func timestamp(_ value: Double?) -> String {
            guard let value, value > 0, value <= maxTimestamp else { return "Not reported" }
            let formatter = DateFormatter()
            formatter.locale = locale
            formatter.timeZone = zone
            formatter.dateStyle = .medium
            formatter.timeStyle = .medium
            return formatter.string(from: Date(timeIntervalSince1970: value))
        }

        let mark = approximate ? "≈" : ""
        let summary: String
        if let used {
            if let percent, let limit { summary = "Context \(mark)\(decimal(percent, 0))% (\(short(used))/\(short(limit)))" }
            else { summary = "Context \(mark)\(short(used)) / Unknown window" }
        } else { summary = "Context: Not reported" }
        let tone: ContextTone
        if recorded || percent == nil { tone = .muted }
        else if percent! >= 90 { tone = .critical }
        else if percent! >= 70 { tone = .warning }
        else { tone = .normal }

        let cold = isCold(raw, now: now)
        let deadline = expiry(raw)
        let cacheStatus = cache?.string("status") ?? ""
        let remaining = deadline.map { $0 - now }
        let cacheSummary: String
        if cold { cacheSummary = "Cold cache" }
        else if cacheStatus == "warm", let remaining, remaining > 0 { cacheSummary = "Cache ~\(Int64((remaining / 60).rounded(.up)))m" }
        else if cacheStatus == "warm" { cacheSummary = "Warm cache / Expiry unknown" }
        else { cacheSummary = "Cache: Unknown" }

        var details = [ContextDetail("Context tokens", (approximate && used != nil ? "Approx. " : "") + tokens(used)),
                       ContextDetail("Context window", tokens(limit)),
                       ContextDetail("Window used", percent.map { decimal($0, 1) + "%" } ?? "Not reported")]
        if context?.string("limit_source") == "model_config" { details.append(ContextDetail("Window source", "Native model configuration")) }
        if let model = usage?.string("model"), !model.isEmpty { details.append(ContextDetail("Model", model)) }
        let provider: String
        if hint?.string("status") == "cold" { provider = "Cold (reported by native agent)" }
        else if cold { provider = "Estimated lifetime ended" }
        else if cacheStatus == "warm" { provider = "Warm (native usage estimate)" }
        else { provider = "Unknown" }
        details.append(ContextDetail("Provider cache", provider))
        details.append(ContextDetail("Estimated cache expiry", timestamp(deadline)))
        if let ttl = number(cache, "ttl_seconds") { details.append(ContextDetail("Reported cache TTL", tokens(ttl) + " s")) }
        if let observed = number(cache, "observed_at"), observed > 0 { details.append(ContextDetail("Cache observation", timestamp(observed))) }
        if let source = cache?.string("source"), !source.isEmpty { details.append(ContextDetail("Cache evidence", source.replacingOccurrences(of: "_", with: " "))) }
        let last = usage?.object("last_request")
        for (key, label) in [("input", "Last request input"), ("cache_read", "Last request cache read"), ("cache_write", "Last request cache write"),
                             ("output", "Last request output"), ("reasoning", "Last request reasoning")] {
            if let value = number(last, key) ?? number(cache, key) { details.append(ContextDetail(label, tokens(value))) }
        }

        var notes: [String] = []
        if recorded { notes.append("Last known values. Live context and cache state are not verified.") }
        if used == nil { notes.append("The provider has not reported context usage for this conversation yet.") }
        else if limit == nil { notes.append("The provider has not reported the window size. Percentage is unavailable.") }
        if approximate { notes.append("Approximate context size reported by the provider.") }
        if let percent, percent >= 90 { notes.append("Context is nearly full. Native compaction may reduce history but can omit details and uses tokens.") }
        else if let percent, percent >= 70 { notes.append("Context is growing. Consider native compaction if you no longer need the full history.") }
        if hint?.string("status") == "cold" {
            let count = number(hint, "tokens").flatMap { $0 > 0 ? " (\(tokens($0)) tokens)" : nil } ?? ""
            notes.append("The agent reports a cold cache\(count). Continuing rebuilds cached context.")
        } else if cold { notes.append("The estimated cache lifetime has ended. Continuing may rebuild cached context.") }
        if hint?.string("status") == "saving_hint" {
            let saving = number(hint, "tokens").map { " to save \(tokens($0)) context tokens" } ?? ""
            notes.append("The native agent suggests /clear for a new task\(saving). This does not prove the cache is cold.")
        }
        if deadline == nil { notes.append("The agent has not reported a cache expiry. No countdown is available.") }
        notes.append("Prompt, model or tool changes can invalidate cache earlier. Last-request cache hits do not prove current warmth.")

        if let usage, usage.string("status") == "ok" {
            let totals = usage.object("totals")
            details.append(ContextDetail("Total tokens", tokens(number(totals, "total"))))
            for (key, label) in [("input", "Input (including cache)"), ("uncached_input", "Uncached input"), ("cache_read", "Cache read"),
                                 ("cache_write", "Cache write"), ("output", "Output"), ("reasoning", "Reasoning (in output)"),
                                 ("web_searches", "Web searches"), ("web_fetches", "Web fetches")] {
                if let value = number(totals, key) { details.append(ContextDetail(label, tokens(value))) }
            }
            if let requests = number(usage, "requests") { details.append(ContextDetail("Recorded requests", tokens(requests))) }
            let cost = number(usage, "cost_usd")
            details.append(ContextDetail(cost == nil ? "Cost" : "Native cost estimate", cost.map { cost in
                let formatter = NumberFormatter()
                formatter.locale = locale
                formatter.numberStyle = .currency
                formatter.currencyCode = "USD"
                formatter.minimumFractionDigits = 4
                formatter.maximumFractionDigits = 4
                return formatter.string(from: NSNumber(value: cost)) ?? String(cost)
            } ?? "Not reported"))
            if cost != nil {
                if usage.bool("cost_incomplete") { notes.append("The provider reports incomplete cost data.") }
                else if usage.bool("cost_stale") { notes.append("Last saved native cost snapshot. Newer requests are not included yet.") }
                else { notes.append("Provider estimate from its latest saved snapshot; may differ from the bill.") }
            }
            let extras = usage.object("extras")
            for (key, label) in [("api_ms", "API time"), ("tool_ms", "Tool time")] {
                if let value = number(extras, key) { details.append(ContextDetail(label, decimal(value / 1000, 1) + " s")) }
            }
            for (key, label) in [("lines_added", "Lines added"), ("lines_removed", "Lines removed")] {
                if let value = number(extras, key) { details.append(ContextDetail(label, tokens(value))) }
            }
            notes.append(usage.bool("partial") ? "Reading session history. Totals are incomplete."
                         : "This conversation only. Subagent usage is separate. Cache is included in input; reasoning is included in output.")
        } else { notes.append("No native usage totals reported for this conversation yet.") }

        let compactRequest = raw?.object("compact_context_request").flatMap { request in
            request.string("run_id") == raw?.string("run_id") && request.string("conversation_id") == raw?.string("conversation_id") ? request : nil
        }
        let compacting = raw?.string("phase") == "compacting" && raw?.string("runtime_state") == "live" &&
            raw?.string("process_state") == "running" && raw?.string("state") != "archived"
        let compactionSummary: String
        if compacting { compactionSummary = "Compacting context…" }
        else {
            switch compactRequest?.string("status") {
            case "submitted": compactionSummary = "Compaction requested"
            case "compacting": compactionSummary = "Compaction in progress"
            case "completed": compactionSummary = "Last compaction completed"
            case "unchanged": compactionSummary = "No history compacted"
            case "failed": compactionSummary = "Compaction failed"
            case "cancelled": compactionSummary = "Compaction cancelled"
            case "uncertain": compactionSummary = "Compaction unconfirmed"
            default: compactionSummary = ""
            }
        }
        if !compactionSummary.isEmpty { details.append(ContextDetail("Native compaction", compactionSummary)) }
        if let started = number(raw, "compaction_started"), started > 0 { details.append(ContextDetail("Compaction started", timestamp(started))) }
        if let completed = number(compactRequest, "completed_at") { details.append(ContextDetail("Compaction completed", timestamp(completed))) }
        let cacheTone: ContextTone = recorded ? .muted : cold ? .critical : cacheStatus == "warm" ? .normal : .muted
        return ContextView(summary: summary, tone: tone, cacheSummary: cacheSummary, cacheTone: cacheTone, compactionSummary: compactionSummary,
                           compacting: compacting && !recorded, cold: cold, recorded: recorded, details: details, notes: notes,
                           compactReason: actionReason(raw, recorded, "compact_context"), clearReason: actionReason(raw, recorded, "clear_context"))
    }

    static func actionReason(_ raw: JSONObject?, _ recorded: Bool, _ operation: String) -> String {
        if raw?.string("state") == "archived" || !(raw?.string("archive_id") ?? "").isEmpty { return "Archived conversations are read-only." }
        guard let raw, !recorded else { return "Refresh to verify the session before changing context." }
        if raw.string("runtime_state") != "live" || raw.string("process_state") != "running" { return "The native agent is not running." }
        if raw.string("run_id").isEmpty || raw.string("conversation_id").isEmpty { return "Wait for the native conversation to be initialized." }
        if raw.string("phase") != "idle" || raw.string("activity") != "idle" { return "Wait until the session is idle." }
        if raw.bool(operation + "_supported") { return "" }
        if !raw.string(operation + "_reason").isEmpty { return raw.string(operation + "_reason") }
        return operation == "compact_context" ? "Native compaction is not available for this session." : "Native context clearing is not available for this session."
    }

    /// ARGB colors for the dark theme; the app is dark only.
    public static func color(_ tone: ContextTone) -> UInt32 {
        switch tone {
        case .muted: 0xFFA1ADBB
        case .normal: 0xFF72CDB2
        case .warning: 0xFFF0A35B
        case .critical: 0xFFF07878
        }
    }
    public static func cacheColor(_ tone: ContextTone) -> UInt32 { tone == .critical ? 0xFFFF9CA8 : color(tone) }
}

// MARK: - Search

public struct SearchableMessage: Hashable, Sendable {
    public let eventId: String
    public let role: String
    public let text: String
    public let at: Double
}

public struct MessageSearchHit: Hashable, Sendable {
    public let eventId: String
    public let role: String
    public let snippet: String
    public let at: Double
}

public struct MessageSearchResults: Hashable, Sendable {
    public let matches: Int
    public let hits: [MessageSearchHit]
    public static let empty = MessageSearchResults(matches: 0, hits: [])
}

/// Loaded public messages only. Search is presentation and never advances read state.
public enum ConversationSearch {
    public static func prepare(_ events: [Event]) -> [SearchableMessage] {
        events.map { event in
            let replies = QuestionReplyPresentation.isOwnRole(event.role) ? QuestionReplyPresentation.parse(event.text) : nil
            let body = replies?.map { "Question\n\($0.question)\nYour answer\n\($0.answer)" }.joined(separator: "\n\n") ?? event.text
            let text = [body, event.attachments.map(\.name).joined(separator: "\n")]
                .filter { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }.joined(separator: "\n")
            let role = replies != nil ? "You (answer)" : event.role == ActivityNotices.role
                ? ActivityNotices.label(origin: event.origin ?? "", sender: event.sender ?? "") : event.role
            return SearchableMessage(eventId: event.id, role: role, text: text, at: event.at)
        }
    }

    public static func find(_ messages: [SearchableMessage], _ query: String, limit: Int = 100) -> MessageSearchResults {
        if query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || query.utf16.count > 2048 { return .empty }
        var hits: [MessageSearchHit] = []
        var count = 0
        let bound = min(max(limit, 1), 100)
        for message in messages {
            guard let range = message.text.range(of: query, options: [.caseInsensitive]) else { continue }
            count += 1
            if hits.count >= bound { continue }
            let text = message.text
            let start = text.index(range.lowerBound, offsetBy: -45, limitedBy: text.startIndex) ?? text.startIndex
            let end = text.index(start, offsetBy: 180, limitedBy: text.endIndex) ?? text.endIndex
            let snippet = (start > text.startIndex ? "…" : "") + text[start..<end].replacingOccurrences(of: "\n", with: " ") +
                (end < text.endIndex ? "…" : "")
            hits.append(MessageSearchHit(eventId: message.eventId, role: message.role, snippet: snippet, at: message.at))
        }
        return MessageSearchResults(matches: count, hits: hits)
    }

    public static func resolve(_ events: [Event], _ id: String) -> Event? {
        let matching = events.filter { $0.id == id }
        return matching.count == 1 ? matching[0] : nil
    }
}

// MARK: - Selected session

/// Exact cached inspection may describe status, but never authorizes a native action.
public enum SelectedSessionPresentation {
    public static func session(_ selected: Session, _ inspection: JSONObject?) -> Session {
        let target = selected.target
        guard let inspection, inspection.string("name") == target.session, inspection.string("run_id") == target.run,
              inspection.string("conversation_id") == target.conversation,
              target.archiveId.isEmpty || inspection.string("archive_id") == target.archiveId,
              target.agentId.isEmpty || inspection.string("parent_conversation_id") == target.parentConversation else { return selected }
        let currentWait = selected.raw.object("coordination_wait")
        let inspectedWait = inspection.object("coordination_wait")
        let inspectedRevision: Int64 = inspectedWait.map { wait in
            wait.int("schema") == 1 && wait.string("status") == "ok" && wait.string("run_id") == target.run && wait.string("conversation_id") == target.conversation
                ? wait.int64("revision") : -1
        } ?? -1
        var copy = selected
        if let currentWait, currentWait.string("status") == "ok", currentWait.string("run_id") == target.run,
           currentWait.string("conversation_id") == target.conversation, currentWait.int64("revision") > inspectedRevision {
            // An older cache never resurrects a settled wait or drops a newer one.
            var combined = inspection
            combined["coordination_wait"] = .object(currentWait)
            copy.raw = combined
            return copy
        }
        copy.raw = inspection
        return copy
    }

    public static func status(_ selected: Session, _ inspection: JSONObject?) -> String {
        let displayed = session(selected, inspection)
        if !selected.target.agentId.isEmpty {
            return SessionDetailsPresentation.agentStatus(displayed.raw.string("display_state", "state", "status")) +
                (displayed.raw.bool("read_only") ? " / History only" : "")
        }
        if displayed.raw.string("activity").isEmpty && !displayed.raw.has("tracked") { return "Status not reported" }
        return SessionFilters.status(displayed, true)
    }
}

// MARK: - Loading

/// A resolved empty inspection is content too: polling must not restore the cold placeholder.
public enum ConversationLoadPresentation {
    public struct Status: Hashable, Sendable {
        public let text: String
        public let initial: Bool
        public let spinner: Bool
    }

    public static func status(hasContent: Bool, detailLoading: Bool, detailProgress: Bool, historyLoading: Bool, historyProgress: Bool,
                              demo: Bool, resolved: Bool = false, opening: Bool = false) -> Status? {
        if demo || !detailLoading && !historyLoading { return nil }
        // Opening cached messages shows its first reads at once; later polls wait for the delayed gate.
        let progress = opening || detailLoading && detailProgress || historyLoading && historyProgress
        let initial = !hasContent && !resolved
        if !initial && !progress { return nil }
        let text = initial ? "Loading conversation…"
            : opening ? "Updating messages…"
            : historyLoading && detailLoading ? "Refreshing conversation and history…"
            : historyLoading ? "Loading message history…" : "Refreshing conversation…"
        return Status(text: text, initial: initial, spinner: progress)
    }

    public static func readAllOutcome(marked: Int, skipped: Int, partial: Int) -> String {
        var parts = [marked == 0 ? "No conversations marked read." : "\(marked == 1 ? "1 conversation" : "\(marked) conversations") marked read on this phone."]
        if partial > 0 { parts.append("\(partial) marked loaded replies only.") }
        if skipped > 0 { parts.append("\(skipped) could not be checked.") }
        return parts.joined(separator: " ")
    }
}

// MARK: - Composer and drafts

public enum ComposerIdentity {
    /// Initial parent conversation promotion keeps the same editor; child lanes never share it.
    public static func lane(_ target: Target) -> [String] {
        [target.connectionId, target.computerId, target.session, target.run, target.archiveId, target.agentId, target.parentConversation]
    }
}

/// A saved draft is not evidence that the app still owns an external picker.
public enum AttachmentSelection {
    public static func cancel(_ state: MessageState, _ selectionId: String) -> MessageState {
        if state.pickerId != selectionId || (state.pickerDraft == nil && state.pickerId.isEmpty) { return state }
        var copy = state
        copy.pickerDraft = nil
        copy.pickerId = ""
        return copy
    }

    public static func recover(_ state: MessageState, _ selectionId: String, launchedId: String, filesBusy: Bool) -> MessageState {
        if filesBusy || state.pickerId != selectionId { return state }
        if state.pickerDraft != nil && !selectionId.isEmpty && selectionId == launchedId { return state }
        return cancel(state, selectionId)
    }
}

/// Keyed changes preserve attachment additions and never replace the rest of the private state.
public enum MessageChanges {
    public static func draft(_ state: MessageState, _ value: Draft) -> MessageState {
        let current = state.drafts.first { $0.key == value.key }
        var recovery: Draft?
        if let current, current.status == "editing", current.generation != value.generation,
           current.attachments.contains(where: { file in !value.attachments.contains { $0.id == file.id } }) {
            var detached = current
            detached.detachedId = "selection-" + current.revision
            recovery = detached
        }
        var copy = state
        copy.drafts = state.drafts.filter { $0.key != value.key && (recovery == nil || $0.key != recovery!.key) } + (recovery.map { [$0] } ?? []) + [value]
        return copy
    }

    public static func discard(_ state: MessageState, _ captured: Draft) -> MessageState {
        guard let current = state.drafts.first(where: { $0.key == captured.key }),
              current.generation == captured.generation, current.requestId == captured.requestId else { return state }
        var copy = state
        if current.revision != captured.revision {
            if current.status != "editing" || current.attachments.isEmpty { return state }
            var recovery = current
            recovery.detachedId = "selection-" + current.revision
            copy.drafts = state.drafts.filter { $0.key != current.key && $0.key != recovery.key } + [recovery]
            return copy
        }
        copy.drafts = state.drafts.filter { $0.key != captured.key }
        return copy
    }

    public static func text(_ state: MessageState, _ editor: Draft) -> MessageState {
        let current = state.drafts.first { $0.key == editor.key } ?? state.drafts.first { moved in
            editor.questionId.isEmpty && editor.detachedId.isEmpty && moved.questionId.isEmpty && moved.detachedId.isEmpty &&
                moved.generation == editor.generation &&
                (ContextPolicies.clearMoved(state, editor.target, moved.target) || ActionDraftPolicies.moved(state, editor.target, moved.target))
        }
        if let current, current.status != "editing" { return state }
        var updated = editor
        if let current {
            updated = current
            updated.text = editor.text
            updated.answers = editor.answers
            updated.revision = editor.revision
            updated.updatedAt = editor.updatedAt
        }
        return draft(state, updated)
    }

    public static func enqueueAcknowledged(_ state: MessageState, original: Draft, written: MessageState, requestId: String) -> MessageState {
        guard let message = written.outgoing.first(where: { $0.requestId == requestId }),
              let cleared = written.drafts.first(where: { $0.key == original.key }) else { return state }
        let current = state.drafts.first { $0.key == original.key }
        var acknowledged: Draft?
        if let current {
            if current.revision == original.revision { acknowledged = cleared }
            else if current.generation != original.generation { acknowledged = current }
            else {
                var merged = cleared
                merged.text = current.text
                merged.answers = current.answers
                merged.revision = current.revision
                merged.updatedAt = current.updatedAt
                merged.attachments = current.attachments.filter { file in !original.attachments.contains { $0.id == file.id } }
                acknowledged = merged
            }
        }
        var copy = state
        copy.drafts = state.drafts.filter { $0.key != original.key } + (acknowledged.map { [$0] } ?? [])
        if !state.outgoing.contains(where: { $0.requestId == message.requestId }) { copy.outgoing = state.outgoing + [message] }
        return copy
    }

    public static func selectionAcknowledged(_ state: MessageState, origin: Draft, written: MessageState, addedIds: Set<String>, selectionId: String) -> MessageState {
        guard let committed = written.drafts.first(where: { $0.attachments.contains { addedIds.contains($0.id) } }) else { return state }
        let current = state.drafts.first { draft in
            draft.generation == origin.generation && draft.detachedId.isEmpty && draft.status == "editing" &&
                (draft.target == origin.target || origin.target.resolves(to: draft.target) ||
                 ContextPolicies.clearMoved(state, origin.target, draft.target) || ActionDraftPolicies.moved(state, origin.target, draft.target))
        }
        var recovered: Draft
        if let current {
            recovered = current
            recovered.attachments = current.attachments + committed.attachments.filter { file in
                addedIds.contains(file.id) && !current.attachments.contains { $0.id == file.id }
            }
            recovered.revision = committed.revision
        } else {
            recovered = committed
            if recovered.detachedId.isEmpty { recovered.detachedId = "selection-" + committed.revision }
        }
        var copy = state
        copy.drafts = state.drafts.filter { $0.key != recovered.key } + [recovered]
        if state.pickerId == selectionId {
            copy.pickerDraft = nil
            copy.pickerId = ""
        }
        return copy
    }

    /// Messages a native row proved delivered (`ConversationMerge.Result.recordedRequestIds`) become recorded, which ends
    /// their recovery; a reviewed message stays reviewed.
    public static func recorded(_ state: MessageState, _ requestIds: Set<String>) -> MessageState {
        var copy = state
        copy.outgoing = state.outgoing.map { message in
            guard requestIds.contains(message.requestId), message.status != "reviewed" else { return message }
            var updated = message
            updated.status = "recorded"
            updated.error = ""
            return updated
        }
        return copy
    }

    public static func result(_ state: MessageState, _ requestId: String, _ update: (OutgoingMessage) -> OutgoingMessage) -> MessageState {
        var copy = state
        copy.outgoing = state.outgoing.map { $0.requestId == requestId && !["recorded", "reviewed"].contains($0.status) ? update($0) : $0 }
        return copy
    }
}

// MARK: - Viewport

/// UI-only reading position. Message keys survive incoming rows and transcript windows.
public struct ConversationAnchor: Hashable, Sendable {
    public let itemKey: String
    public let offset: Int
    public let following: Bool
    public init(itemKey: String, offset: Int, following: Bool) {
        self.itemKey = itemKey
        self.offset = offset
        self.following = following
    }
}

public struct ConversationFollowState: Hashable, Sendable {
    public var opened = false
    public var following = true
    public var ownSendId = ""
    public var anchor: ConversationAnchor?
    public init(opened: Bool = false, following: Bool = true, ownSendId: String = "", anchor: ConversationAnchor? = nil) {
        self.opened = opened
        self.following = following
        self.ownSendId = ownSendId
        self.anchor = anchor
    }
}

public struct ConversationScroll: Hashable, Sendable {
    public var state: ConversationFollowState
    public var index: Int?
    public var offset = 0
    public var failure = ""
}

public enum ConversationViewport {
    public static func content(_ state: ConversationFollowState, keys: [String], ownSendId: String, ownItemKey: String?) -> ConversationScroll {
        guard let last = keys.indices.last else { return ConversationScroll(state: state) }
        if !ownSendId.isEmpty, ownSendId != state.ownSendId, let ownItemKey, let ownIndex = keys.firstIndex(of: ownItemKey) {
            return ConversationScroll(state: ConversationFollowState(opened: true, following: true, ownSendId: ownSendId), index: ownIndex)
        }
        if !state.opened {
            if let anchor = state.anchor, !anchor.following, let index = keys.firstIndex(of: anchor.itemKey) {
                return ConversationScroll(state: ConversationFollowState(opened: true, following: false, ownSendId: ownSendId, anchor: anchor),
                                          index: index, offset: anchor.offset)
            }
            return ConversationScroll(state: ConversationFollowState(opened: true, following: true, ownSendId: ownSendId), index: last)
        }
        return ConversationScroll(state: state, index: state.following ? last : nil)
    }

    public static func userScrolled(_ state: ConversationFollowState, atEnd: Bool) -> ConversationFollowState {
        var copy = state
        copy.following = atEnd
        return copy
    }

    public static func settled(_ state: ConversationFollowState, atEnd: Bool, hasNewer: Bool) -> ConversationFollowState {
        guard state.opened, atEnd, !hasNewer else { return state }
        return latest(state)
    }

    public static func latest(_ state: ConversationFollowState) -> ConversationFollowState {
        var copy = state
        copy.following = true
        copy.anchor = nil
        return copy
    }

    /// An explicit jump consumes its own read result, even when no message key changed.
    public static func loadedLatest(_ state: ConversationFollowState, keys: [String], hasNewer: Bool, error: String) -> ConversationScroll {
        if !error.isEmpty { return ConversationScroll(state: state, failure: "Could not reach the latest messages. \(error)") }
        if hasNewer { return ConversationScroll(state: state, failure: "The latest messages are not available in the loaded history. Refresh to try again.") }
        guard let last = keys.indices.last else {
            return ConversationScroll(state: state, failure: "No messages are available to jump to. Refresh to try again.")
        }
        var next = latest(state)
        next.opened = true
        return ConversationScroll(state: next, index: last)
    }
}

/// Persistence echoes never edit an active text buffer. Only a new composition
/// generation (send clear, restore or explicit replacement) owns a text reset.
public struct ComposerUpdate: Hashable, Sendable {
    public let generation: String
    public let replacement: String?
}

public enum ComposerReconciler {
    public static func external(current: String, generation: String, text: String) -> ComposerUpdate {
        current == generation ? ComposerUpdate(generation: current, replacement: nil) : ComposerUpdate(generation: generation, replacement: text)
    }
}
