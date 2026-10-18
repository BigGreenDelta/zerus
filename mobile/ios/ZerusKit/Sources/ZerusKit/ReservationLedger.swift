import Foundation

/// The shared reservation ledger's content (zerus-gfph.11): per scoped session slot, the durable fingerprint of its
/// last fresh announcement and the highest relay event observed for it. Decoding is strict and fails closed; nothing is
/// ever evicted, reset or removed here.
public struct ReservationLedger: Equatable, Sendable {
    public static let schema: Int64 = 1
    public static let maxProofs = 32

    public enum Kind: String, Sendable, CaseIterable { case input, error }
    public enum Writer: String, Sendable, CaseIterable { case app, `extension` }

    /// One observation of a need: session slot `slot` in workspace `workspace` has identity fingerprint `fingerprint`
    /// as of relay event `eventId`.
    public struct Observation: Equatable, Sendable {
        public var workspace: String
        public var slot: String
        public var fingerprint: String
        public var kind: Kind
        public var eventId: Int64
        public var by: Writer

        public init(workspace: String, slot: String, fingerprint: String, kind: Kind, eventId: Int64, by: Writer) {
            self.workspace = workspace
            self.slot = slot
            self.fingerprint = fingerprint
            self.kind = kind
            self.eventId = eventId
            self.by = by
        }

        var scope: String { JSON.array([.string(workspace), .string(slot)]).compact }
    }

    /// The persistent proof for one scope: `announced` is the fingerprint of its last fresh announcement, made at
    /// `announcedEventId` by `by`; `observedEventId` (≥ `announcedEventId`) is the highest relay event observed for the
    /// scope, by any writer. Only an observation with a newer event and another fingerprint can announce again.
    public struct SlotProof: Equatable, Sendable {
        public var workspace: String
        public var slot: String
        public var announced: String
        public var kind: Kind
        public var announcedEventId: Int64
        public var observedEventId: Int64
        public var by: Writer

        var scope: String { JSON.array([.string(workspace), .string(slot)]).compact }
    }

    public enum Announcement: Equatable, Sendable {
        /// A fresh announcement; the returned ledger records it.
        case fresh(ReservationLedger)
        /// The same fingerprint at a newer event: nothing is announced, but the returned ledger raises the scope's fence.
        case observed(ReservationLedger)
        /// The same fingerprint at an equal or older event: nothing changes.
        case alreadyAnnounced
        /// Another fingerprint at an event no newer than the scope's fence: nothing changes and nothing is announced.
        case stale
        /// A new scope would exceed the global bound; nothing is evicted.
        case full
        /// The observation itself is malformed; nothing is recorded.
        case invalid
    }

    public private(set) var proofs: [SlotProof]

    public init() { proofs = [] }

    /// A missing ledger is empty; anything else must be exactly schema 1 with valid, uniquely scoped proofs.
    public static func decode(_ data: Data?) throws -> ReservationLedger {
        guard let data else { return ReservationLedger() }
        guard data.count <= NotificationReservations.maxBytes, let root = try JSON.parse(data).object,
              Set(root.keys) == ["schema", "proofs"], root["schema"] == .int(schema), case .array(let rows)? = root["proofs"],
              rows.count <= maxProofs else { throw invalid }
        var ledger = ReservationLedger()
        var scopes = Set<String>()
        for row in rows {
            guard case .object(let fields) = row,
                  Set(fields.keys) == ["workspace", "slot", "announced", "kind", "announced_event_id", "observed_event_id", "by"],
                  case .string(let workspace)? = fields["workspace"], case .string(let slot)? = fields["slot"],
                  case .string(let announced)? = fields["announced"], case .string(let kindValue)? = fields["kind"],
                  let kind = Kind(rawValue: kindValue), case .int(let announcedEventId)? = fields["announced_event_id"],
                  case .int(let observedEventId)? = fields["observed_event_id"],
                  case .string(let byValue)? = fields["by"], let by = Writer(rawValue: byValue) else { throw invalid }
            let proof = SlotProof(workspace: workspace, slot: slot, announced: announced, kind: kind, announcedEventId: announcedEventId,
                                  observedEventId: observedEventId, by: by)
            guard valid(proof), scopes.insert(proof.scope).inserted else { throw invalid }
            ledger.proofs.append(proof)
        }
        return ledger
    }

    /// Canonical bytes: fixed key order, proofs in their stored order; throws beyond 16 KiB.
    public func encoded() throws -> Data {
        let rows: [JSON] = proofs.map { proof in
            .object(JSONObject([("workspace", .string(proof.workspace)), ("slot", .string(proof.slot)), ("announced", .string(proof.announced)),
                                ("kind", .string(proof.kind.rawValue)), ("announced_event_id", .int(proof.announcedEventId)),
                                ("observed_event_id", .int(proof.observedEventId)), ("by", .string(proof.by.rawValue))]))
        }
        let data = try JSON.object(JSONObject([("schema", .int(Self.schema)), ("proofs", .array(rows))])).data()
        guard data.count <= NotificationReservations.maxBytes else { throw Self.invalid }
        return data
    }

    /// Applies one observation. Every event at or below a scope's fence is ignored or stale; a newer event raises the
    /// fence, and announces only when its fingerprint differs from the last fresh announcement.
    public func observing(_ observation: Observation) -> Announcement {
        guard Self.valid(observation) else { return .invalid }
        let proof = SlotProof(workspace: observation.workspace, slot: observation.slot, announced: observation.fingerprint, kind: observation.kind,
                              announcedEventId: observation.eventId, observedEventId: observation.eventId, by: observation.by)
        guard let index = proofs.firstIndex(where: { $0.scope == observation.scope }) else {
            guard proofs.count < Self.maxProofs else { return .full }
            var next = self
            next.proofs.append(proof)
            return .fresh(next)
        }
        let existing = proofs[index]
        guard observation.eventId > existing.observedEventId else {
            return observation.fingerprint == existing.announced ? .alreadyAnnounced : .stale
        }
        var next = self
        if observation.fingerprint == existing.announced {
            // The durable announced fingerprint and its event stay; only the fence rises.
            next.proofs[index].observedEventId = observation.eventId
            return .observed(next)
        }
        next.proofs[index] = proof
        return .fresh(next)
    }

    static let invalid = ZerusError.invalid("The notification reservation ledger is invalid.")

    static func valid(_ proof: SlotProof) -> Bool {
        text(proof.workspace, 256) && text(proof.slot, 4096) && fingerprint(proof.announced) && proof.announcedEventId > 0 &&
            proof.observedEventId >= proof.announcedEventId
    }

    static func valid(_ observation: Observation) -> Bool {
        text(observation.workspace, 256) && text(observation.slot, 4096) && fingerprint(observation.fingerprint) && observation.eventId > 0
    }

    static func fingerprint(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }

    static func text(_ value: String, _ limit: Int) -> Bool {
        !value.isEmpty && value.utf8.count <= limit && !value.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }
}
