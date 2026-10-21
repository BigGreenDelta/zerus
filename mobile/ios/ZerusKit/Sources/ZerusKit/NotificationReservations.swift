import Foundation

/// One shared item with a revision, as the app and its notification service extension see it (zerus-gfph.11).
public struct RevisionedItem: Sendable, Equatable {
    public let data: Data
    public let revision: String

    public init(data: Data, revision: String) {
        self.data = data
        self.revision = revision
    }
}

/// The conditional store both processes write through. Its implementation must make each call atomic:
/// `read` returns one consistent snapshot of data and revision, `create` adds only when no item exists, and `replace`
/// installs the data with its new revision only while the stored revision still equals `expected`. No native store has
/// been shown to meet this contract yet; that is a separate device-proof gate.
public protocol RevisionedItemStore: Sendable {
    func read(_ account: String) throws -> RevisionedItem?
    /// False when an item already exists.
    func create(_ account: String, _ data: Data, revision: String) throws -> Bool
    /// False when the item is absent or another writer changed it.
    func replace(_ account: String, _ data: Data, expected: String, revision: String) throws -> Bool
}

public enum ReservationFailure: Error, Sendable, Equatable {
    /// The store could not be read or written, for example while the phone is locked before its first unlock. A write
    /// that throws has an unknown outcome: the decided ledger may already be stored.
    case unavailable
    /// The stored or decided ledger exceeds its bound.
    case oversized
    /// The ledger cannot be understood; it is never reset or replaced.
    case rejected
    /// Other writers kept winning for every attempt.
    case contended
}

public enum ReservationOutcome<Result: Sendable>: Sendable {
    /// The decided ledger was written over the exact snapshot it was decided from.
    case committed(Result)
    /// Nothing needed to be written.
    case unchanged(Result)
    /// This call reserved nothing it may announce. After a write with an unknown outcome the decided ledger may already
    /// be stored; the caller still announces nothing and never replays the write, and the next independent call re-reads
    /// and finds whatever was stored.
    case failedClosed(ReservationFailure)
}

/// Decides against one consistent snapshot of the shared reservation ledger and commits only if no other writer
/// changed it in between. A reservation is durable at most once; delivery by the system is not promised.
public enum NotificationReservations {
    public static let account = "notification-reservations-v1"
    public static let maxBytes = 16 * 1024
    public static let attempts = 3

    /// `decide` receives the stored ledger, or nil when none exists yet, and returns the ledger to store (nil to store
    /// nothing) with its result. It throws when it cannot understand the ledger, which then fails closed unchanged. A
    /// lost write is decided again from a fresh snapshot, at most `attempts` times.
    public static func reserve<Result: Sendable>(_ store: RevisionedItemStore, revision: () -> String = { UUID().uuidString.lowercased() },
                                                 decide: (Data?) throws -> (ledger: Data?, result: Result)) -> ReservationOutcome<Result> {
        for _ in 0..<attempts {
            let snapshot: RevisionedItem?
            do { snapshot = try store.read(account) } catch { return .failedClosed(.unavailable) }
            if let snapshot, snapshot.data.count > maxBytes { return .failedClosed(.oversized) }
            let decision: (ledger: Data?, result: Result)
            do { decision = try decide(snapshot?.data) } catch { return .failedClosed(.rejected) }
            guard let ledger = decision.ledger else { return .unchanged(decision.result) }
            guard ledger.count <= maxBytes else { return .failedClosed(.oversized) }
            let next = revision()
            guard next != snapshot?.revision else { return .failedClosed(.rejected) }
            let written: Bool
            do {
                if let snapshot {
                    written = try store.replace(account, ledger, expected: snapshot.revision, revision: next)
                } else {
                    written = try store.create(account, ledger, revision: next)
                }
            } catch {
                return .failedClosed(.unavailable)
            }
            if written { return .committed(decision.result) }
        }
        return .failedClosed(.contended)
    }
}
