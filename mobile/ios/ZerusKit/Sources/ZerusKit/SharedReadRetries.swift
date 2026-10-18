import Foundation

/// Bounded bookkeeping for read marks a computer completed without applying (zerus-rk5b.11).
///
/// A pure value: the caller owns one timer, sleeps until `nextDue`, then calls `release(now:)` and allows exactly the
/// returned marks to be sent again. Times are monotonic seconds. At capacity a new mark is not scheduled at all: it
/// stays marked as sent, so nothing is ever resent beyond the bound (fail closed).
public struct SharedReadRetries: Sendable, Equatable {
    public static let capacity = 256
    public static let maxExponent = 3

    struct Entry: Sendable, Equatable {
        var connectionId: String
        var exponent: Int
        /// When the next attempt is allowed; nil once released.
        var due: Double?
        /// When a released entry is forgotten, so its next unapplied completion starts over at two minutes.
        var retireAt: Double
    }

    private(set) var entries: [String: Entry] = [:]

    public init() {}

    public var count: Int { entries.count }

    /// Two minutes, doubling to 16: 120 s << exponent, with the exponent saturating at 0...3.
    public static func delay(_ exponent: Int) -> Double { Double(120 << min(max(exponent, 0), maxExponent)) }

    /// Records an unapplied completion of `key` and returns when it may be sent again, or nil at capacity.
    public mutating func unapplied(_ key: String, connectionId: String, now: Double) -> Double? {
        prune(now)
        let exponent: Int
        if let entry = entries[key] {
            exponent = entry.due == nil ? min(entry.exponent + 1, Self.maxExponent) : entry.exponent
        } else {
            guard entries.count < Self.capacity else { return nil }
            exponent = 0
        }
        let due = now + Self.delay(exponent)
        entries[key] = Entry(connectionId: connectionId, exponent: exponent, due: due, retireAt: due + Self.delay(Self.maxExponent))
        return due
    }

    /// An applied receipt ends the backoff for this mark.
    public mutating func applied(_ key: String) { entries[key] = nil }

    /// The earliest pending attempt, for the caller's single timer.
    public var nextDue: Double? { entries.values.compactMap(\.due).min() }

    /// Marks whose attempt is due at `now`; they keep their exponent until retired.
    public mutating func release(now: Double) -> [String] {
        var released: [String] = []
        for (key, entry) in entries where (entry.due ?? .infinity) <= now {
            entries[key]?.due = nil
            released.append(key)
        }
        prune(now)
        return released.sorted()
    }

    /// Forgetting a workspace drops its marks; nothing of it is released later.
    public mutating func forget(connection: String) { entries = entries.filter { $0.value.connectionId != connection } }

    public mutating func reset() { entries = [:] }

    private mutating func prune(_ now: Double) { entries = entries.filter { $0.value.due != nil || $0.value.retireAt > now } }
}
