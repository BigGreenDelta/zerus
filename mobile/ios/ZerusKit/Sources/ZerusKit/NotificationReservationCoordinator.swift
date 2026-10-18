import Foundation

/// Serializes this process's reservations (zerus-gfph.11): every caller in the app or in one extension process goes
/// through one coordinator, and each call is one conditional reservation against the shared ledger. Another process
/// is fenced by the ledger's revision, not by this actor.
public actor NotificationReservationCoordinator {
    private let store: RevisionedItemStore

    public init(store: RevisionedItemStore) { self.store = store }

    /// The largest batch; a longer one fails closed before the store is touched.
    public static let maxBatch = ReservationLedger.maxProofs

    /// Applies `observations` in order in one conditional write. The result is per observation; only `.fresh` ones may
    /// be announced, and only when the outcome is `.committed`. A malformed or unknown ledger fails closed and is never
    /// reset, and a batch longer than 32 is refused without reading the store.
    public func observe(_ observations: [ReservationLedger.Observation]) -> ReservationOutcome<[ReservationLedger.Announcement]> {
        guard observations.count <= Self.maxBatch else { return .failedClosed(.rejected) }
        return NotificationReservations.reserve(store) { try Self.decide($0, observations) }
    }

    /// The pure decision of one write: the ledger to store (nil when nothing changed) and the per-observation results.
    static func decide(_ data: Data?, _ observations: [ReservationLedger.Observation]) throws -> (ledger: Data?, result: [ReservationLedger.Announcement]) {
        var ledger = try ReservationLedger.decode(data)
        var results: [ReservationLedger.Announcement] = []
        var changed = false
        for observation in observations {
            let result = ledger.observing(observation)
            switch result {
            case .fresh(let next), .observed(let next):
                ledger = next
                changed = true
            case .alreadyAnnounced, .stale, .full, .invalid:
                break
            }
            results.append(result)
        }
        return (changed ? try ledger.encoded() : nil, results)
    }
}
