import Foundation
import Observation

public enum ArchivePeriod: String, CaseIterable, Sendable {
    case all, today, week, month, custom
    public var caption: String {
        switch self {
        case .all: "All dates"
        case .today: "Today"
        case .week: "Last 7 days"
        case .month: "Last 30 days"
        case .custom: "Date range"
        }
    }
}

/// Archive time window in Unix seconds, `[from, until)`.
public struct ArchiveRange: Hashable, Sendable {
    public static let maximum: Int64 = 253_402_300_800
    public var from: Int64
    public var until: Int64

    public init(from: Int64 = 0, until: Int64 = ArchiveRange.maximum) {
        self.from = from
        self.until = until
    }

    public var valid: Bool { from >= 0 && from < until && until <= ArchiveRange.maximum }

    public static func days(_ first: Date, _ last: Date, timeZone: TimeZone = .current) -> ArchiveRange {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let start = calendar.startOfDay(for: first)
        let end = calendar.date(byAdding: .day, value: 1, to: calendar.startOfDay(for: last))!
        return ArchiveRange(from: Int64(start.timeIntervalSince1970), until: Int64(end.timeIntervalSince1970))
    }

    public static func period(_ period: ArchivePeriod, first: Date, last: Date, today: Date = Date(), timeZone: TimeZone = .current) -> ArchiveRange {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        switch period {
        case .all: return ArchiveRange()
        case .today: return days(today, today, timeZone: timeZone)
        case .week: return days(calendar.date(byAdding: .day, value: -6, to: today)!, today, timeZone: timeZone)
        case .month: return days(calendar.date(byAdding: .day, value: -29, to: today)!, today, timeZone: timeZone)
        case .custom: return days(first, last, timeZone: timeZone)
        }
    }
}

public struct ArchivePage: Sendable {
    public var sessions: [Session]
    public var cursor: String
    public var total: Int
    public var previous: String
    public var position: Int?
    public init(sessions: [Session], cursor: String, total: Int, previous: String = "", position: Int? = nil) {
        self.sessions = sessions
        self.cursor = cursor
        self.total = total
        self.previous = previous
        self.position = position
    }
}

public struct ArchiveWindow: Hashable, Sendable {
    public let position: Int?
    public let count: Int
    public let total: Int
}

/// Archive browsing by bounded per-computer pages. Generation checks apply even
/// when a transport cannot cancel an already claimed read. Ported from Android.
@MainActor
@Observable
public final class ArchivePaging {
    public typealias Loader = @MainActor (MachineKey, JSONObject) async throws -> ArchivePage

    public private(set) var period: ArchivePeriod = .all
    public private(set) var first: Date
    public private(set) var last: Date
    public private(set) var loading = false

    private struct Page {
        var rows: [Session] = []
        var cursor = ""
        var total = 0
        var loaded = false
        var error = ""
        var previous = ""
        var position: Int?
    }

    private var pages: [MachineKey: Page] = [:]
    private var pageOrder: [MachineKey] = []
    @ObservationIgnored private var keys: Set<MachineKey> = []
    @ObservationIgnored private var search = ""
    @ObservationIgnored private var active = false
    @ObservationIgnored private var currentRange = ArchiveRange()
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var jobs: [MachineKey: Task<Void, Never>] = [:]
    @ObservationIgnored private let load: Loader
    @ObservationIgnored private let reset: @MainActor () -> Void

    public init(load: @escaping Loader, reset: @escaping @MainActor () -> Void = {}) {
        self.load = load
        self.reset = reset
        let now = Date()
        last = now
        first = Calendar(identifier: .gregorian).date(byAdding: .day, value: -29, to: now)!
    }

    public var rows: [Session] { pageOrder.compactMap { pages[$0] }.flatMap(\.rows) }
    public var total: Int { pages.values.reduce(0) { $0 + $1.total } }
    public var totalsKnown: Bool { !keys.isEmpty && keys.allSatisfy { pages[$0]?.loaded == true } }
    public var errors: [MachineKey: String] { pages.filter { !$0.value.error.isEmpty }.mapValues(\.error) }
    public var hasMore: Bool { keys.contains { key in pages[key].map { !$0.loaded || !$0.cursor.isEmpty } ?? true } }
    public var hasNewer: Bool { pages.values.contains { !$0.previous.isEmpty } }
    public var partial: Bool { !totalsKnown || rows.count < total }
    public var windows: [MachineKey: ArchiveWindow] {
        pages.filter(\.value.loaded).mapValues { ArchiveWindow(position: $0.position, count: $0.rows.count, total: $0.total) }
    }
    public var range: ArchiveRange { ArchiveRange.period(period, first: first, last: last) }

    public func configure(machines: Set<MachineKey>, query: String, visible: Bool) {
        let changed = keys != machines || search != query || currentRange != range
        active = visible
        if changed || (!visible && !jobs.isEmpty) {
            generation += 1
            jobs.values.forEach { $0.cancel() }
            jobs = [:]
            loading = false
            if changed {
                pages = [:]
                pageOrder = []
                if visible { reset() }
            }
            keys = machines
            search = query
            currentRange = range
        }
        if visible && !loading && currentRange.valid && keys.contains(where: { key in pages[key].map { !$0.loaded && $0.error.isEmpty } ?? true }) {
            loadMore()
        }
    }

    public func pause() { configure(machines: keys, query: search, visible: false) }

    public func dates(_ value: ArchivePeriod, from: Date? = nil, through: Date? = nil) {
        period = value
        if let from { first = from }
        if let through { last = through }
        configure(machines: keys, query: search, visible: active)
    }

    public func refresh() {
        generation += 1
        jobs.values.forEach { $0.cancel() }
        jobs = [:]
        loading = false
        pages = [:]
        pageOrder = []
        currentRange = range
        if active {
            reset()
            loadMore()
        }
    }

    public func loadMore() { navigate(newer: false) }
    public func loadNewer() { navigate(newer: true) }

    private func navigate(newer: Bool) {
        guard active, currentRange.valid, !loading else { return }
        let own = generation, dates = currentRange, text = search
        for key in keys.sorted(by: { ($0.connectionId, $0.computerId) < ($1.connectionId, $1.computerId) }) {
            let page = pages[key] ?? Page()
            let cursor = newer ? page.previous : page.cursor
            if newer && cursor.isEmpty || !newer && page.loaded && cursor.isEmpty { continue }
            var cleared = page
            cleared.error = ""
            store(key, cleared)
            loading = true
            jobs[key] = Task { [weak self] in
                guard let self else { return }
                do {
                    let query: JSONObject = ["from": .int(dates.from), "until": .int(dates.until), "limit": 50, "cursor": .string(cursor), "query": .string(text)]
                    let result = try await self.load(key, query)
                    guard result.sessions.count <= 50, result.total >= result.sessions.count, result.cursor.count <= 1024, result.previous.count <= 1024,
                          result.position.map({ $0 >= 0 && Int64($0) + Int64(result.sessions.count) <= Int64(result.total) }) ?? true,
                          !result.sessions.isEmpty || result.cursor.isEmpty,
                          cursor.isEmpty || (result.cursor != cursor && result.previous != cursor) else {
                        throw ZerusError.invalid("Computer returned an invalid archive page. Refresh the archive.")
                    }
                    guard !result.sessions.isEmpty || page.rows.isEmpty else { throw ZerusError.invalid("Archive changed. Refresh to start a new view.") }
                    if self.generation == own {
                        var seen = Set<String>()
                        // Replace the visible window. No retained row/cursor history grows with browsing.
                        self.store(key, Page(rows: result.sessions.filter { seen.insert($0.target.key).inserted }, cursor: result.cursor, total: result.total,
                                             loaded: true, previous: result.previous, position: result.position))
                        self.reset()
                    }
                } catch is CancellationError {
                } catch {
                    if self.generation == own {
                        var failed = page
                        failed.error = ReadFailure.message(error, "archive")
                        self.store(key, failed)
                    }
                }
                if self.generation == own {
                    self.jobs[key] = nil
                    self.loading = !self.jobs.isEmpty
                }
            }
        }
    }

    private func store(_ key: MachineKey, _ page: Page) {
        if pages.updateValue(page, forKey: key) == nil { pageOrder.append(key) }
    }
}

/// Presentation for reads only; mutation receipt uncertainty remains separate.
public enum ReadFailure {
    public static func message(_ error: Error, _ saved: String) -> String {
        if error is URLError { return "No connection. Showing saved \(saved). Refresh to try again." }
        let text = (error as? LocalizedError)?.errorDescription ?? ""
        return text.trimmingCharacters(in: .whitespaces).isEmpty ? "Could not refresh \(saved). Refresh to try again." : text
    }
}
