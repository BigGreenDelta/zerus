import CryptoKit
import Foundation

/// Optional: only pending optional questions, which the agent keeps working without.
public enum SessionAlertKind: String, Codable, CaseIterable, Sendable {
    case input = "Input", error = "Error", finished = "Finished", optional = "Optional"

    /// Only needs that cannot be posted individually join the "More sessions need attention" summary.
    public var overflows: Bool { self == .input || self == .error }
}

public struct NotificationPreferences: Codable, Hashable, Sendable {
    public var input = true
    public var errors = true
    public var finished = false
    public var generation: Int64 = 0
    public var master = true
    public var inputEnabledAt: Int64 = 0
    public var errorsEnabledAt: Int64 = 0
    public var finishedEnabledAt: Int64 = 0
    public var optional = false
    public var optionalEnabledAt: Int64 = 0

    public init(input: Bool = true, errors: Bool = true, finished: Bool = false, generation: Int64 = 0, master: Bool = true, inputEnabledAt: Int64 = 0,
                errorsEnabledAt: Int64 = 0, finishedEnabledAt: Int64 = 0, optional: Bool = false, optionalEnabledAt: Int64 = 0) {
        self.input = input
        self.errors = errors
        self.finished = finished
        self.generation = generation
        self.master = master
        self.inputEnabledAt = inputEnabledAt
        self.errorsEnabledAt = errorsEnabledAt
        self.finishedEnabledAt = finishedEnabledAt
        self.optional = optional
        self.optionalEnabledAt = optionalEnabledAt
    }

    public func enabledAt(_ kind: SessionAlertKind) -> Int64 {
        switch kind {
        case .input: inputEnabledAt
        case .error: errorsEnabledAt
        case .finished: finishedEnabledAt
        case .optional: optionalEnabledAt
        }
    }

    public func allows(_ kind: SessionAlertKind) -> Bool {
        guard master else { return false }
        switch kind {
        case .input: return input
        case .error: return errors
        case .finished: return finished
        case .optional: return optional
        }
    }

    static func next(_ value: Int64) -> Int64 { value == .max ? 0 : value + 1 }

    /// The master switch; enabling it restores each enabled type's existing cards.
    public func settingMaster(_ enabled: Bool) -> NotificationPreferences {
        var copy = self
        let generation = Self.next(self.generation)
        copy.master = enabled
        copy.generation = generation
        if enabled && input { copy.inputEnabledAt = generation }
        if enabled && errors { copy.errorsEnabledAt = generation }
        if enabled && finished { copy.finishedEnabledAt = generation }
        if enabled && optional { copy.optionalEnabledAt = generation }
        return copy
    }

    public func setting(_ kind: SessionAlertKind, _ enabled: Bool) -> NotificationPreferences {
        var copy = self
        let generation = Self.next(self.generation)
        switch kind {
        case .input:
            if enabled && !input { copy.inputEnabledAt = generation }
            copy.input = enabled
        case .error:
            if enabled && !errors { copy.errorsEnabledAt = generation }
            copy.errors = enabled
        case .finished:
            if enabled && !finished { copy.finishedEnabledAt = generation }
            copy.finished = enabled
        case .optional:
            if enabled && !optional { copy.optionalEnabledAt = generation }
            copy.optional = enabled
        }
        copy.generation = generation
        return copy
    }
}

public struct NotificationSlot: Codable, Hashable, Sendable {
    public var connection: String
    public var computer: String
    public var session: String

    public init(connection: String, computer: String, session: String) {
        self.connection = connection
        self.computer = computer
        self.session = session
    }

    public var key: String { JSON.array([.string(connection), .string(computer), .string(session)]).compact }
    public var tag: String { "zerus:session:v2:" + key }
}

public struct NotificationEvent: Codable, Hashable, Sendable {
    public var id: Int64
    public var computer: String
    public var session: String
    public var kind: String
    public var at: Double
    public var alertKind: String?
    public var nativeReplyId: String?

    public init(id: Int64, computer: String, session: String, kind: String, at: Double, alertKind: String? = nil, nativeReplyId: String? = nil) {
        self.id = id
        self.computer = computer
        self.session = session
        self.kind = kind
        self.at = at
        self.alertKind = alertKind
        self.nativeReplyId = nativeReplyId
    }

    /// The transition this row announces; relay alert rows carry it in alert_kind.
    public var signal: String { kind == "alert" ? alertKind ?? "" : kind }
    public var isAlert: Bool { kind == "alert" && alertKind != nil }
}

/// One live session as the current catalog shows it.
public struct NotificationCurrent: Hashable, Sendable {
    public var slot: NotificationSlot
    public var target: String
    public var title: String
    public var machine: String
    public var kind: SessionAlertKind?
    public var identity: String
    public var idle: Bool
    public var online: Bool
    public var live: Bool
    public var conversation: String
    /// Sorted IDs of pending optional questions in the session's conversation.
    public var optional: [String]
    public var suppressed: Bool
    public var notificationPreference: SessionNotificationPreference?

    public init(slot: NotificationSlot, target: String, title: String, machine: String, kind: SessionAlertKind?, identity: String, idle: Bool,
                online: Bool = true, live: Bool = true, conversation: String = "", optional: [String] = [], suppressed: Bool = false,
                notificationPreference: SessionNotificationPreference? = nil) {
        self.slot = slot
        self.target = target
        self.title = title
        self.machine = machine
        self.kind = kind
        self.identity = identity
        self.idle = idle
        self.online = online
        self.live = live
        self.conversation = conversation
        self.optional = optional
        self.suppressed = suppressed
        self.notificationPreference = notificationPreference
    }
}

public struct NotificationRecord: Codable, Hashable, Sendable {
    public var slot: NotificationSlot
    public var target: String
    public var observedAt: Double
    public var observedFingerprint = ""
    public var postedFingerprint = ""
    public var postedKind: SessionAlertKind?
    public var postedTarget = ""
    public var eventId: Int64 = 0
    public var eventAt: Double = 0
    public var summaryOnly = false
    public var candidateFingerprint = ""
    public var candidateTarget = ""
    public var candidateEventId: Int64 = 0
    public var candidateEventAt: Double = 0
    public var restoredAt: Int64 = -1
    public var optionalSeen: [String] = []

    public init(slot: NotificationSlot, target: String, observedAt: Double, observedFingerprint: String = "", postedFingerprint: String = "",
                postedKind: SessionAlertKind? = nil, postedTarget: String = "", eventId: Int64 = 0, eventAt: Double = 0, summaryOnly: Bool = false,
                candidateFingerprint: String = "", candidateTarget: String = "", candidateEventId: Int64 = 0, candidateEventAt: Double = 0,
                restoredAt: Int64 = -1, optionalSeen: [String] = []) {
        self.slot = slot
        self.target = target
        self.observedAt = observedAt
        self.observedFingerprint = observedFingerprint
        self.postedFingerprint = postedFingerprint
        self.postedKind = postedKind
        self.postedTarget = postedTarget
        self.eventId = eventId
        self.eventAt = eventAt
        self.summaryOnly = summaryOnly
        self.candidateFingerprint = candidateFingerprint
        self.candidateTarget = candidateTarget
        self.candidateEventId = candidateEventId
        self.candidateEventAt = candidateEventAt
        self.restoredAt = restoredAt
        self.optionalSeen = optionalSeen
    }

    enum CodingKeys: String, CodingKey {
        case slot, target, observedAt, observedFingerprint, postedFingerprint, postedKind, postedTarget, eventId, eventAt, summaryOnly
        case candidateFingerprint, candidateTarget, candidateEventId, candidateEventAt, restoredAt, optionalSeen
    }

    /// A kind written by a newer version decodes as no posted kind; seen questions stay bounded.
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        slot = try c.decode(NotificationSlot.self, forKey: .slot)
        target = try c.decode(String.self, forKey: .target)
        observedAt = try c.decode(Double.self, forKey: .observedAt)
        observedFingerprint = try c.decodeIfPresent(String.self, forKey: .observedFingerprint) ?? ""
        postedFingerprint = try c.decodeIfPresent(String.self, forKey: .postedFingerprint) ?? ""
        postedKind = (try? c.decodeIfPresent(String.self, forKey: .postedKind)).flatMap { $0.flatMap(SessionAlertKind.init(rawValue:)) }
        postedTarget = try c.decodeIfPresent(String.self, forKey: .postedTarget) ?? ""
        eventId = try c.decodeIfPresent(Int64.self, forKey: .eventId) ?? 0
        eventAt = try c.decodeIfPresent(Double.self, forKey: .eventAt) ?? 0
        summaryOnly = try c.decodeIfPresent(Bool.self, forKey: .summaryOnly) ?? false
        candidateFingerprint = try c.decodeIfPresent(String.self, forKey: .candidateFingerprint) ?? ""
        candidateTarget = try c.decodeIfPresent(String.self, forKey: .candidateTarget) ?? ""
        candidateEventId = try c.decodeIfPresent(Int64.self, forKey: .candidateEventId) ?? 0
        candidateEventAt = try c.decodeIfPresent(Double.self, forKey: .candidateEventAt) ?? 0
        restoredAt = try c.decodeIfPresent(Int64.self, forKey: .restoredAt) ?? -1
        optionalSeen = Array((try c.decodeIfPresent([String].self, forKey: .optionalSeen) ?? []).suffix(NotificationPolicy.maxOptionalSeen))
    }
}

/// Per-workspace durable delivery state: the event cursor, coalesced pending rows and posted evidence.
public struct NotificationState: Codable, Hashable, Sendable {
    public var cursor: Int64 = 0
    public var bootstrap = true
    public var draining = false
    public var batchActive = false
    public var batchSoundUsed = false
    public var lastSoundElapsed: Int64 = -1
    public var settingsGeneration: Int64 = -1
    public var pending: [NotificationEvent] = []
    public var records: [NotificationRecord] = []
    /// Ordered newest first; a set in meaning.
    public var pendingPosts: [String] = []
    public var truncated = false
    public var deliveryNeeded = false
    public var overflowKinds: Set<SessionAlertKind> = []
    public var pendingRestoreKinds: Set<SessionAlertKind> = []
    public var presenceAlerts = false

    public init(cursor: Int64 = 0, bootstrap: Bool = true, draining: Bool = false, batchActive: Bool = false, batchSoundUsed: Bool = false,
                lastSoundElapsed: Int64 = -1, settingsGeneration: Int64 = -1, pending: [NotificationEvent] = [], records: [NotificationRecord] = [],
                pendingPosts: [String] = [], truncated: Bool = false, deliveryNeeded: Bool = false, overflowKinds: Set<SessionAlertKind> = [],
                pendingRestoreKinds: Set<SessionAlertKind> = [], presenceAlerts: Bool = false) {
        self.cursor = cursor
        self.bootstrap = bootstrap
        self.draining = draining
        self.batchActive = batchActive
        self.batchSoundUsed = batchSoundUsed
        self.lastSoundElapsed = lastSoundElapsed
        self.settingsGeneration = settingsGeneration
        self.pending = pending
        self.records = records
        self.pendingPosts = pendingPosts
        self.truncated = truncated
        self.deliveryNeeded = deliveryNeeded
        self.overflowKinds = overflowKinds
        self.pendingRestoreKinds = pendingRestoreKinds
        self.presenceAlerts = presenceAlerts
    }

    /// Saved state must stay within the policy's bounds.
    public func restored() throws -> NotificationState {
        guard pending.count <= NotificationPolicy.maxPending, records.count <= NotificationPolicy.maxRecords, pendingPosts.count <= 24, cursor >= 0 else {
            throw ZerusError.invalid("Saved notification state is invalid.")
        }
        return self
    }
}

public struct SessionNotice: Hashable, Sendable {
    public var current: NotificationCurrent
    public var kind: SessionAlertKind
    public var fingerprint: String
    public var fresh: Bool
    public var eventId: Int64
    public var eventAt: Double

    public init(current: NotificationCurrent, kind: SessionAlertKind, fingerprint: String, fresh: Bool, eventId: Int64 = 0, eventAt: Double = 0) {
        self.current = current
        self.kind = kind
        self.fingerprint = fingerprint
        self.fresh = fresh
        self.eventId = eventId
        self.eventAt = eventAt
    }

    public var body: String {
        let label = switch kind {
        case .input: "Needs input"
        case .error: "Error"
        case .finished: "Turn finished"
        case .optional: "Optional question"
        }
        return "\(label) on \(current.machine)"
    }
}

public struct NotificationPlan: Sendable {
    public var state: NotificationState
    public var desired: [SessionNotice]
    public var retained: Set<String>
    public var reconcile: Bool
    public var overflow: Bool
    public var overflowKinds: Set<SessionAlertKind>
    public var restoreKinds: Set<SessionAlertKind>
}

public struct NotificationPage: Sendable {
    public var state: NotificationState
    public var full: Bool
    public var empty: Bool
}

/// Events wake current-slot presentation; they cannot identify a historical run.
public enum NotificationPolicy {
    public static let maxPending = 128
    public static let maxRecords = 256
    public static let cooldownMillis: Int64 = 30_000
    /// Optional question IDs remembered per slot, so each one announces at most once.
    public static let maxOptionalSeen = 64

    public static func fingerprint(_ value: String) -> String { SHA256.hash(data: Data(value.utf8)).map { String(format: "%02x", $0) }.joined() }

    /// Each alert kind survives both other alerts and ordinary refresh rows.
    static func pendingKey(_ event: NotificationEvent) -> String {
        event.kind == "alert" ? JSON.array([.string(event.computer), .string(event.session), "alert", event.alertKind.map(JSON.string) ?? .null]).compact
            : JSON.array([.string(event.computer), .string(event.session)]).compact
    }

    public static func ingest(_ state: NotificationState, _ events: [NotificationEvent]) throws -> NotificationPage {
        if events.isEmpty {
            var copy = state
            copy.draining = false
            return NotificationPage(state: copy, full: false, empty: true)
        }
        var keys: [String] = []
        var pending: [String: NotificationEvent] = [:]
        for event in state.pending {
            let key = pendingKey(event)
            if pending[key] == nil { keys.append(key) }
            pending[key] = event
        }
        func ordered() -> [NotificationEvent] { keys.compactMap { pending[$0] } }
        var cursor = state.cursor
        func page(_ full: Bool) -> NotificationPage {
            var copy = state
            copy.cursor = cursor
            copy.pending = ordered()
            copy.draining = true
            copy.batchActive = true
            copy.batchSoundUsed = state.batchActive ? state.batchSoundUsed : false
            return NotificationPage(state: copy, full: full, empty: false)
        }
        for event in events where event.id > cursor {
            let key = pendingKey(event)
            if pending[key] == nil && pending.count >= maxPending { return page(true) }
            if pending[key] == nil { keys.append(key) }
            pending[key] = event
            cursor = event.id
        }
        guard cursor > state.cursor else { throw ZerusError.invalid("Notification page did not advance.") }
        return page(false)
    }

    public static func plan(_ state: NotificationState, current: [NotificationCurrent], events: [(NotificationSlot, NotificationEvent)],
                            preferences: NotificationPreferences, now: Double, active: Set<String> = [], alertsOnly: Bool = false) -> NotificationPlan {
        let old = Dictionary(state.records.map { ($0.slot.key, $0) }, uniquingKeysWith: { first, _ in first })
        // With presence alerts only relay alert rows announce; other rows refresh state.
        let announcing = events.filter { $0.1.kind != "notification_preferences" && (alertsOnly ? $0.1.isAlert : $0.1.kind != "alert") }
        let candidates = Dictionary(grouping: announcing, by: { $0.0.key })
        // Relay and phone order input and error differently, so either need alert announces either card.
        let needs: Set<String> = ["attention", "error"]
        let alerted = Set(events.filter { $0.1.isAlert && needs.contains($0.1.signal) }.map { $0.0.key })
        let transitions = Set(events.filter { needs.contains($0.1.kind) }.map { $0.0.key })
        let reconcile = preferences.generation != state.settingsGeneration
        let restoreKinds = state.pendingRestoreKinds.union(SessionAlertKind.allCases.filter { preferences.enabledAt($0) > state.settingsGeneration })
            .filter(preferences.allows)
        var desired: [SessionNotice] = []
        var records: [NotificationRecord] = []
        var retained: Set<String> = []
        var recoverable: Set<String> = []
        var seenSlots: Set<String> = []
        for session in current where seenSlots.insert(session.slot.key).inserted {
            let key = session.slot.key
            let previous = old[key]
            if session.suppressed {
                records.append(NotificationRecord(slot:session.slot,target:session.target,observedAt:now,
                    observedFingerprint:session.kind.map { fingerprint(JSON.array([.string(session.target),.string($0.rawValue),.string(session.identity)]).compact) } ?? "",
                    optionalSeen:Array(Set((previous?.optionalSeen ?? []) + session.optional)).sorted().suffix(maxOptionalSeen).map { $0 }))
                continue
            }
            if !session.live {
                // A stopped session keeps only the optional questions it announced; resuming the
                // same conversation does not announce them again.
                if let previous, !previous.optionalSeen.isEmpty {
                    records.append(NotificationRecord(slot: session.slot, target: previous.target, observedAt: previous.observedAt,
                                                      summaryOnly: state.truncated, optionalSeen: previous.optionalSeen))
                }
                continue
            }
            if !session.online {
                if let posted = previous?.postedKind, preferences.allows(posted) { retained.insert(key) }
                if var previous {
                    if state.pendingPosts.contains(key) && !active.contains(key) {
                        previous.postedFingerprint = ""
                        previous.postedKind = nil
                    }
                    records.append(previous)
                }
                continue
            }
            let event = candidates[key]?.filter { pair in
                guard alertsOnly else { return true }
                return session.kind != nil && session.kind != .finished ? needs.contains(pair.1.signal) : pair.1.signal == "completed"
            }.max { $0.1.id < $1.1.id }?.1
            let recent = event.map { $0.at <= now && $0.at >= now - 120 } ?? false
            var kind = session.kind
            var freshCompletion = false
            var identity = kind.map { fingerprint(JSON.array([.string(session.target), .string($0.rawValue), .string(session.identity)]).compact) } ?? ""
            // Optional questions announce once per question ID, across resumes and removals.
            let seenOptional = previous?.optionalSeen ?? []
            let newOptional = session.optional.filter { !seenOptional.contains($0) }
            if kind == .optional {
                // Fewer questions keep the posted card, and a dismissed card stays dismissed.
                if let previous, newOptional.isEmpty, previous.postedKind == .optional, !previous.postedFingerprint.isEmpty {
                    identity = previous.postedFingerprint
                } else {
                    identity = fingerprint(JSON.array([.string(session.conversation), .string(SessionAlertKind.optional.rawValue),
                                                       .array(session.optional.map(JSON.string))]).compact)
                }
                // When disabled, a completion can still apply.
                if !preferences.allows(.optional) {
                    kind = nil
                    identity = ""
                }
            }
            if kind == nil && session.idle && !state.bootstrap {
                if preferences.finished, let event, event.signal == "completed", recent, let previous, !session.target.isEmpty,
                   previous.target == session.target, event.at > previous.observedAt {
                    kind = .finished
                    freshCompletion = true
                    identity = fingerprint(JSON.array([.string(session.target), "Finished", event.nativeReplyId.map(JSON.string) ?? .int(event.id)]).compact)
                } else if let previous, !previous.candidateFingerprint.isEmpty, previous.candidateTarget == session.target,
                          (now - 120...now).contains(previous.candidateEventAt) {
                    kind = .finished
                    identity = previous.candidateFingerprint
                } else if let previous, previous.postedKind == .finished, previous.postedTarget == session.target, (now - 120...now).contains(previous.eventAt) {
                    kind = .finished
                    identity = previous.postedFingerprint
                }
            }
            var detailOnly = false
            var held = false
            if alertsOnly, let current = kind, current != .finished, previous?.postedFingerprint != identity, !alerted.contains(key) {
                // A posted input or error card follows detail changes silently. A need the relay
                // announced without an alert, or one never posted, waits for its alert.
                if let previous, !previous.postedFingerprint.isEmpty, let posted = previous.postedKind, posted != .finished,
                   previous.postedTarget == session.target, !transitions.contains(key) {
                    detailOnly = true
                }
                if !detailOnly {
                    held = current == .optional
                    kind = nil
                    identity = ""
                }
            }
            let freshTransition = recent && !detailOnly && (kind == .optional ? !newOptional.isEmpty : identity != previous?.observedFingerprint)
            // Questions held for a presence alert stay new; any other outcome has shown or superseded them.
            let optionalSeen = held ? seenOptional : Array((seenOptional.filter { !session.optional.contains($0) } + session.optional).suffix(maxOptionalSeen))
            let summaryOnly = previous.map { $0.summaryOnly && !freshTransition } ?? state.truncated
            var observed = previous ?? NotificationRecord(slot: session.slot, target: session.target, observedAt: now)
            let recoveringCompletion = kind == .finished && !freshCompletion
            let finishedEventId: Int64, finishedEventAt: Double
            if recoveringCompletion {
                finishedEventId = (previous?.candidateEventId).flatMap { $0 > 0 ? $0 : nil } ?? previous?.eventId ?? 0
                finishedEventAt = (previous?.candidateEventAt).flatMap { $0 > 0 ? $0 : nil } ?? previous?.eventAt ?? 0
            } else {
                finishedEventId = event?.id ?? previous?.eventId ?? 0
                finishedEventAt = event?.at ?? previous?.eventAt ?? 0
            }
            observed.target = session.target
            observed.observedAt = now
            if !detailOnly { observed.observedFingerprint = identity }
            observed.candidateFingerprint = kind == .finished ? identity : ""
            observed.candidateTarget = kind == .finished ? session.target : ""
            observed.candidateEventId = kind == .finished ? finishedEventId : 0
            observed.candidateEventAt = kind == .finished ? finishedEventAt : 0
            if kind == nil {
                observed.postedFingerprint = ""
                observed.postedKind = nil
            }
            observed.summaryOnly = summaryOnly
            observed.optionalSeen = optionalSeen
            records.append(observed)
            if let kind, preferences.allows(kind) {
                retained.insert(key)
                recoverable.insert(key)
                let fresh = !state.bootstrap && !reconcile && freshTransition && !summaryOnly
                desired.append(SessionNotice(current: session, kind: kind, fingerprint: identity, fresh: fresh, eventId: finishedEventId, eventAt: finishedEventAt))
            }
        }
        // Prefer retained delivery evidence before observation-only entries.
        let bounded = Array(records.sorted { a, b in
            let ka = a.slot.key, kb = b.slot.key
            let ra = active.contains(ka) || state.pendingPosts.contains(ka), rb = active.contains(kb) || state.pendingPosts.contains(kb)
            if ra != rb { return ra }
            let oa = old[ka]?.observedAt ?? now, ob = old[kb]?.observedAt ?? now
            if oa != ob { return oa > ob }
            return ka < kb
        }.prefix(maxRecords))
        let eligible = Set(bounded.filter { !$0.summaryOnly }.map(\.slot.key))
        let individual = desired.filter { eligible.contains($0.current.slot.key) }
        let overflowKinds = Set(desired.filter { !eligible.contains($0.current.slot.key) && $0.kind.overflows }.map(\.kind))
        var next = state
        next.pending = []
        next.records = bounded
        next.bootstrap = state.bootstrap && state.draining
        next.settingsGeneration = preferences.generation
        next.batchActive = false
        next.deliveryNeeded = true
        next.pendingRestoreKinds = Set(restoreKinds)
        next.pendingPosts = state.pendingPosts.filter(recoverable.contains)
        next.truncated = state.truncated || records.count > maxRecords
        return NotificationPlan(state: next, desired: individual, retained: retained, reconcile: reconcile, overflow: !overflowKinds.isEmpty,
                                overflowKinds: overflowKinds, restoreKinds: Set(restoreKinds))
    }

    /// Records delivery evidence before the system shows the card; a crash resumes it silently.
    public static func reserve(_ state: NotificationState, _ notice: SessionNotice, audible: Bool, elapsed: Int64, enabledAt: Int64 = 0) -> NotificationState {
        let key = notice.current.slot.key
        var record = state.records.first { $0.slot.key == key } ?? NotificationRecord(slot: notice.current.slot, target: notice.current.target, observedAt: notice.eventAt)
        record.postedFingerprint = notice.fingerprint
        record.postedKind = notice.kind
        record.postedTarget = notice.current.target
        record.eventId = notice.eventId
        record.eventAt = notice.eventAt
        record.restoredAt = max(record.restoredAt, enabledAt)
        var copy = state
        copy.records = Array(([record] + state.records.filter { $0.slot.key != key }).prefix(maxRecords))
        copy.pendingPosts = Array(([key] + state.pendingPosts.filter { $0 != key }).prefix(24))
        copy.batchSoundUsed = state.batchSoundUsed || audible
        if audible { copy.lastSoundElapsed = elapsed }
        return copy
    }

    public static func canSound(_ state: NotificationState, elapsed: Int64) -> Bool {
        !state.batchSoundUsed && (state.lastSoundElapsed < 0 || (elapsed >= state.lastSoundElapsed && elapsed - state.lastSoundElapsed >= cooldownMillis))
    }

    /// A reboot resets the elapsed clock; never let that bypass the cooldown.
    public static func clockGuard(_ state: NotificationState, elapsed: Int64) -> NotificationState {
        guard state.lastSoundElapsed > elapsed else { return state }
        var copy = state
        copy.lastSoundElapsed = elapsed
        return copy
    }
}

// MARK: Catalog

public struct NotificationCatalog: Sendable {
    public var sessions: [NotificationCurrent]
    /// Every known identity of a computer, mapped to its canonical ID; nil when ambiguous.
    public var identities: [String: String?]

    public func resolve(_ connection: String, _ event: NotificationEvent) -> (NotificationSlot, NotificationEvent)? {
        guard let entry = identities[event.computer], let canonical = entry else { return nil }
        return (NotificationSlot(connection: connection, computer: canonical, session: event.session), event)
    }
}

public enum NotificationCatalogParser {
    static let alertKinds: Set<String> = ["attention", "error", "completed"]

    public static func parse(_ connection: Connection, records: [JSONObject], aliases: [MachineAlias], actions: [SessionAction] = []) -> NotificationCatalog {
        var identities: [String: String?] = [:]
        let known = SessionNotificationPreferences.known(actions:actions)
        var current: [NotificationCurrent] = []
        var counts: [String: Int] = [:]
        for raw in records { counts[raw.string("id"), default: 0] += 1 }
        for raw in records where counts[raw.string("id")] == 1 {
            guard let parsed = try? MachineCatalog.parse(connection.id, raw) else { continue }
            let machine = MachineNames.apply(parsed, aliases)
            for id in machine.serverAliases.sorted() + [machine.id] {
                if let existing = identities[id] {
                    if existing != machine.id { identities[id] = .some(nil) }
                } else {
                    identities[id] = .some(machine.id)
                }
            }
            for session in NativeSessions.sessions(connection, raw) {
                if session.target.session.isEmpty || session.target.session.count > 1024 || SessionFilters.archived(session) { continue }
                let value = SessionNotificationPreferences.merge(session.raw,session.target,known)
                let phase = value.string("phase")
                let recovery = value.object("recovery")?.string("state") ?? ""
                let items = value.objects("mobile_attention")
                let blocking = items.filter { !QuestionPolicies.optionalAttention($0) }
                // Optional questions never change the identity of a request that needs action.
                let pending = blocking.map { JSON.array([.string($0.string("question_id")), .string($0.string("question_hash"))]).compact }.sorted()
                let optional = Array(Set(items.filter(QuestionPolicies.optionalAttention).map { $0.string("question_id") }.filter { !$0.isEmpty })).sorted()
                let nativeKind: SessionAlertKind?
                if (phase == "error" && !["waiting", "dispatching", "retrying"].contains(recovery)) || ["uncertain", "blocked", "exhausted"].contains(recovery) {
                    nativeKind = .error
                } else if ["input", "approval"].contains(phase) || !pending.isEmpty || value.int("pending_question_count") > 0 || SessionNotificationPreferences.hasChildAttention(value) {
                    nativeKind = .input
                } else if !optional.isEmpty {
                    nativeKind = .optional
                } else {
                    nativeKind = nil
                }
                let kind = SessionNotificationPreferences.alertKind(value,session.target,kind:nativeKind,optional:optional)
                let providerError: JSON = value.object("provider_error").map { error in
                    .array([.string(error.string("source")), .string(error.string("message_id")),
                            .string(error.string("code").isEmpty ? error.string("type") : error.string("code")), .string(error.string("message"))])
                } ?? .null
                let recoveryDetail: JSON = value.object("recovery").map { detail in
                    .array([.string(detail.string("request_id")), .string(detail.string("error").isEmpty ? detail.string("reason") : detail.string("error"))])
                } ?? .null
                let identity = NotificationPolicy.fingerprint(JSON.array([.string(phase), .string(value.string("attention_id")), value["question_request"] ?? .null,
                                                                          value["question_requests"] ?? .null, .array(pending.map(JSON.string)), .string(recovery),
                                                                          providerError, recoveryDetail,SessionNotificationPreferences.attentionIdentity(value,session.target),.int(Int64(value.int("pending_question_count")))]).compact)
                let target = !session.target.run.isEmpty && !session.target.conversation.isEmpty
                    ? JSON.array([.string(session.target.run), .string(session.target.conversation)]).compact : ""
                current.append(NotificationCurrent(slot: NotificationSlot(connection: connection.id, computer: machine.id, session: session.target.session),
                                                   target: target, title: clean(SessionFilters.label(session).nonEmpty ?? session.title, "Session"),
                                                   machine: clean(machine.name, "Computer"), kind: kind, identity: identity, idle: value.string("activity") == "idle",
                                                   online: machine.online,
                                                   live: SessionFilters.state(session) == "running" && value.string("process_state") != "exited",
                                                   conversation: session.target.conversation, optional: optional,
                                                   suppressed:SessionNotificationPreferences.suppressed(value,session.target,kind:kind,optional:optional),
                                                   notificationPreference:SessionNotificationPreferences.parse(value,session.target)))
            }
        }
        return NotificationCatalog(sessions: current, identities: identities)
    }

    /// Display text without control or format characters, bounded so it cannot spoof card lines.
    static func clean(_ value: String, _ fallback: String) -> String {
        let scalars = value.unicodeScalars.filter { scalar in
            let category = scalar.properties.generalCategory
            return category != .control && category != .format
        }
        let text = String(String.UnicodeScalarView(scalars).prefix(256))
        return text.trimmingCharacters(in: .whitespaces).isEmpty ? fallback : text
    }

    /// Relay capability that makes alert rows the only source of new cards.
    public static func presenceAlerts(_ capabilities: JSONObject) -> Bool { capabilities.strings("features").contains("presence_alerts") }

    public static func events(_ raw: JSONObject) throws -> [NotificationEvent] {
        guard let values = raw["events"]?.array, values.count <= 100 else { throw ZerusError.invalid("Invalid notification page.") }
        let events = try values.map { item -> NotificationEvent in
            guard let row = item.object, case .int(let id)? = row["id"] else { throw ZerusError.invalid("Invalid notification page.") }
            let computer = row.string("computer_id"), session = row.string("session"), at = row.double("created_at"), kind = row.string("kind")
            guard id > 0, (1...512).contains(computer.count), (1...1024).contains(session.count),
                  !session.unicodeScalars.contains(where: { $0.properties.generalCategory == .control }), at.isFinite, at > 0 else {
                throw ZerusError.invalid("Invalid notification page.")
            }
            // Unknown alert kinds stay plain refresh rows instead of failing the page.
            let alertKind = kind == "alert" && alertKinds.contains(row.string("alert_kind")) ? row.string("alert_kind") : nil
            let reply = row.string("native_reply_id")
            let nativeReplyId = (1...256).contains(reply.utf8.count) && !reply.unicodeScalars.contains(where: { $0.properties.generalCategory == .control }) ? reply : nil
            return NotificationEvent(id: id, computer: computer, session: session, kind: kind, at: at, alertKind: alertKind, nativeReplyId: nativeReplyId)
        }
        guard zip(events, events.dropFirst()).allSatisfy({ $0.id < $1.id }) else { throw ZerusError.invalid("Invalid notification page.") }
        return events
    }
}

/// APNs registration with a gateway: the device token, its build environment and when to register again.
public enum PushRegistration {
    public static let providers: Set<String> = ["apns", "apns_sandbox"]

    /// Development builds use the APNs sandbox; TestFlight and App Store builds use production.
    public static func provider(environment: String) -> String { environment == "production" ? "apns" : "apns_sandbox" }

    public static func token(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }

    public static func validToken(_ value: String) -> Bool {
        (64...200).contains(value.count) && value.count % 2 == 0 && value.allSatisfy { $0.isHexDigit && !$0.isUppercase }
    }

    public static func needsRegistration(_ registration: String, registered: String?) -> Bool { !registration.isEmpty && registration != registered }

    /// Transient gateway failures are retried on the next start; a rejection is final until the token changes.
    public static func retry(_ status: Int) -> Bool { status == 408 || status == 429 || status >= 500 }
}
