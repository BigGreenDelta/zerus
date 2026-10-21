import Foundation
import Observation
import UIKit
import UserNotifications
import ZerusKit

/// Session alerts: drains each workspace's event pages, plans cards from one current catalog and
/// posts replaceable local notifications. A push is only a wake hint; authenticated reads decide
/// every card, and the generic wake alert is replaced by them.
@MainActor
@Observable
final class SessionNotifications {
    @ObservationIgnored private unowned let app: AppModel
    @ObservationIgnored private var store: SealedFileStore?
    @ObservationIgnored private var states: [String: NotificationState] = [:]
    @ObservationIgnored private var registered: [String: String] = [:]
    @ObservationIgnored private var locks: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var listeners: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var lastPost: TimeInterval = -1
    @ObservationIgnored private var lastRefresh: TimeInterval = 0
    @ObservationIgnored private var deviceToken: String?

    private(set) var ready = false
    private(set) var preferences = NotificationPreferences()
    private(set) var authorization: UNAuthorizationStatus = .notDetermined
    private(set) var pushStatus: [String: String] = [:]
    private(set) var registrationUnavailable = false
    var error = ""

    static let cardPrefix = "zerus:session:v2:"
    static let summaryIdentifier = "zerus:overflow:v2"
    /// The collapse identifier of the relay's generic APNs wake alert.
    static let wakeIdentifier = "zerus-wake"
    static let refreshTask = "app.zerus.mobile.refresh"
    static let environment = Bundle.main.object(forInfoDictionaryKey: "ZerusApnsEnvironment") as? String ?? "development"

    private var center: UNUserNotificationCenter { .current() }

    init(app: AppModel) { self.app = app }

    // MARK: Storage

    struct Saved: Codable {
        var preferences = NotificationPreferences()
        var states: [String: NotificationState] = [:]
        var registered: [String: String] = [:]
    }

    func open(_ sealed: SealedFileStore) async {
        store = sealed
        let connections = Set(app.connections.map(\.id))
        let saved = (try? await Task.detached { try sealed.read("session_notifications", as: Saved.self) }.value) ?? nil
        preferences = saved?.preferences ?? NotificationPreferences()
        states = (saved?.states ?? [:]).filter { connections.contains($0.key) }.compactMapValues { try? $0.restored() }
        registered = (saved?.registered ?? [:]).filter { connections.contains($0.key) }
        ready = true
        await refreshAuthorization()
    }

    private func save() {
        guard let store else { return }
        let snapshot = Saved(preferences: preferences, states: states, registered: registered)
        Task.detached(priority: .utility) { try? store.write("session_notifications", snapshot) }
    }

    func refreshAuthorization() async {
        authorization = await center.notificationSettings().authorizationStatus
        updateStatuses()
    }

    var allowed: Bool { [.authorized, .provisional, .ephemeral].contains(authorization) }

    // MARK: Settings

    func requestAuthorization() async {
        _ = try? await center.requestAuthorization(options: [.alert, .sound, .badge])
        await refreshAuthorization()
        if allowed { UIApplication.shared.registerForRemoteNotifications() }
    }

    func setMaster(_ enabled: Bool) {
        preferences = preferences.settingMaster(enabled)
        saveAndReconcile()
        if enabled && authorization == .notDetermined { Task { await requestAuthorization() } }
    }

    func set(_ kind: SessionAlertKind, _ enabled: Bool) {
        preferences = preferences.setting(kind, enabled)
        saveAndReconcile()
    }

    private func saveAndReconcile() {
        save()
        Task {
            await cancelDisabled()
            for connection in app.connections { schedule(connection) }
        }
    }

    private func cancelDisabled() async {
        let preferences = self.preferences
        let stale = await center.deliveredNotifications().filter { notification in
            let info = notification.request.content.userInfo
            guard notification.request.identifier.hasPrefix(Self.cardPrefix) else { return false }
            guard let kind = (info["zerus_kind"] as? String).flatMap(SessionAlertKind.init(rawValue:)) else { return true }
            return !preferences.allows(kind)
        }.map(\.request.identifier)
        center.removeDeliveredNotifications(withIdentifiers: stale + [Self.summaryIdentifier])
    }

    // MARK: Push registration

    func registered(deviceToken data: Data) {
        deviceToken = PushRegistration.token(data)
        registrationUnavailable = false
        Task { await registerAll() }
    }

    func registrationFailed() {
        registrationUnavailable = true
        updateStatuses()
    }

    func registerAll() async {
        guard ready, let token = deviceToken, allowed else {
            updateStatuses()
            return
        }
        let provider = PushRegistration.provider(environment: Self.environment)
        for connection in app.connections {
            guard let capabilities = try? await app.client.capabilities(connection) else { continue }
            guard capabilities.strings("push_providers").contains(provider) else {
                pushStatus[connection.id] = "This gateway does not offer iOS push yet. Alerts arrive while Zerus is open."
                continue
            }
            let registration = provider + ":" + token
            guard PushRegistration.needsRegistration(registration, registered: registered[connection.id]) else {
                pushStatus[connection.id] = "Push notifications are on."
                continue
            }
            pushStatus[connection.id] = "Registering for push…"
            do {
                try await app.client.registerPush(connection, provider: provider, token: token)
                registered[connection.id] = registration
                pushStatus[connection.id] = "Push notifications are on."
                save()
            } catch let failure as RelayError {
                pushStatus[connection.id] = PushRegistration.retry(failure.status) ? "Push registration will be retried when Zerus opens."
                    : "This gateway rejected push registration (\(failure.status))."
            } catch {
                pushStatus[connection.id] = "Push registration will be retried when Zerus opens."
            }
        }
    }

    private func updateStatuses() {
        for connection in app.connections where pushStatus[connection.id] == nil || !allowed || registrationUnavailable {
            pushStatus[connection.id] = !allowed ? (authorization == .denied ? "Notifications are off for Zerus. Allow them in Settings to receive session alerts."
                                                      : "Allow notifications to receive session alerts.")
                : registrationUnavailable ? "Push is unavailable in this build. Alerts arrive while Zerus is open."
                : "Setting up push…"
        }
    }

    func forget(connection: String) {
        listeners.removeValue(forKey: connection)?.cancel()
        states[connection] = nil
        registered[connection] = nil
        pushStatus[connection] = nil
        save()
        Task {
            let cards = await center.deliveredNotifications().filter { ($0.request.content.userInfo["zerus_connection"] as? String) == connection }
            center.removeDeliveredNotifications(withIdentifiers: cards.map(\.request.identifier))
        }
    }

    // MARK: Event drain

    /// While Zerus is active each workspace long-polls its events; leaving the app stops them.
    func setActive(_ active: Bool) {
        if !active {
            listeners.values.forEach { $0.cancel() }
            listeners = [:]
            return
        }
        guard ready, !app.demo else { return }
        for connection in app.connections where listeners[connection.id] == nil {
            listeners[connection.id] = Task { [weak self] in
                while !Task.isCancelled {
                    guard let self, self.app.connections.contains(where: { $0.id == connection.id }) else { return }
                    do {
                        let more = try await self.sync(connection, wait: 25)
                        try await Task.sleep(for: more ? .seconds(1) : .milliseconds(200))
                    } catch let failure as RelayError where [401, 403].contains(failure.status) {
                        return
                    } catch is CancellationError {
                        return
                    } catch {
                        try? await Task.sleep(for: .seconds(15))
                    }
                }
            }
        }
    }

    /// One bounded catch-up for every workspace, for a wake push or background refresh.
    func syncAll(forceCatalog: Bool = false) async {
        await withTaskGroup(of: Void.self) { group in
            for connection in app.connections {
                group.addTask { [weak self] in _ = try? await self?.sync(connection, wait: 0, forceCatalog: forceCatalog) }
            }
        }
    }

    func schedule(_ connection: Connection) {
        Task { _ = try? await sync(connection, wait: 0, forceCatalog: true) }
    }

    /// true means durable work remains; the caller continues instead of starting another drain.
    @discardableResult
    func sync(_ connection: Connection, wait: Int = 0, forceCatalog: Bool = false) async throws -> Bool {
        // One drain per workspace at a time, in order.
        let previous = locks[connection.id]
        let run = Task { @MainActor [weak self] () throws -> Bool in
            await previous?.value
            guard let self else { return false }
            return try await self.drain(connection, wait: wait, forceCatalog: forceCatalog)
        }
        locks[connection.id] = Task { _ = await run.result }
        return try await withTaskCancellationHandler { try await run.value } onCancel: { run.cancel() }
    }

    private func connected(_ connection: Connection) -> Bool {
        app.connections.contains { $0.id == connection.id && $0.url == connection.url && $0.token == connection.token }
    }

    private static func elapsed() -> Int64 { Int64(ProcessInfo.processInfo.systemUptime * 1000) }

    private func drain(_ connection: Connection, wait: Int, forceCatalog: Bool) async throws -> Bool {
        var state = states[connection.id] ?? NotificationState()
        var force = forceCatalog
        var operations = 0
        var capabilitiesChecked = false
        while operations < 10 {
            try Task.checkCancellation()
            guard connected(connection) else { return false }
            let waitNow = operations == 0 && !state.bootstrap && !state.draining && state.pending.isEmpty && !force &&
                state.settingsGeneration == preferences.generation && state.pendingPosts.isEmpty && !state.deliveryNeeded ? wait : 0
            let response = try await app.client.events(connection, after: state.cursor, wait: waitNow)
            operations += 1
            try Task.checkCancellation()
            let page = try NotificationPolicy.ingest(state, NotificationCatalogParser.events(response))
            state = page.state
            states[connection.id] = state
            save()
            if !page.empty { eventsArrived() }
            let needsCatalog = page.full || page.empty || operations >= 9
            if needsCatalog && (state.bootstrap || !state.pending.isEmpty || force || state.settingsGeneration != preferences.generation ||
                                !state.pendingPosts.isEmpty || state.deliveryNeeded) {
                if !capabilitiesChecked {
                    capabilitiesChecked = true
                    operations += 1
                    if let capabilities = try? await app.client.capabilities(connection) { state.presenceAlerts = NotificationCatalogParser.presenceAlerts(capabilities) }
                }
                let raw = try await app.client.computers(connection).objects("computers")
                operations += 1
                try Task.checkCancellation()
                guard connected(connection) else { return false }
                let catalog = NotificationCatalogParser.parse(connection, records: raw, aliases: app.messageState.machineAliases, actions: app.messageState.sessionActions)
                let active = Set(await cards().compactMap { $0.request.content.userInfo["zerus_slot"] as? String })
                let plan = NotificationPolicy.plan(NotificationPolicy.clockGuard(state, elapsed: Self.elapsed()), current: catalog.sessions,
                                                   events: state.pending.compactMap { catalog.resolve(connection.id, $0) }, preferences: preferences,
                                                   now: Date().timeIntervalSince1970, active: active, alertsOnly: state.presenceAlerts)
                state = plan.state
                states[connection.id] = state
                save()
                state = await deliver(connection, state, plan)
                force = false
            }
            if page.empty { return false }
        }
        return true
    }

    /// Refreshes the visible catalog soon after new events, without repeating it for every page.
    private func eventsArrived() {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastRefresh > 2, UIApplication.shared.applicationState == .active else { return }
        lastRefresh = now
        app.refresh()
    }

    // MARK: Delivery

    private func cards() async -> [UNNotification] {
        await center.deliveredNotifications().filter { $0.request.identifier.hasPrefix(Self.cardPrefix) }
    }

    private func pace() async {
        let now = ProcessInfo.processInfo.systemUptime
        if lastPost >= 0 && now >= lastPost { try? await Task.sleep(for: .milliseconds(max(0, Int((0.35 - (now - lastPost)) * 1000)))) }
    }

    private func deliver(_ connection: Connection, _ initial: NotificationState, _ plan: NotificationPlan) async -> NotificationState {
        var state = initial
        let delivered = await center.deliveredNotifications()
        // The generic wake alert already sounded; cards that replace it arrive silently.
        let woke = delivered.contains { $0.request.identifier == Self.wakeIdentifier }
        let withdrawn = delivered.filter {
            $0.request.identifier.hasPrefix(Self.cardPrefix) && ($0.request.content.userInfo["zerus_connection"] as? String) == connection.id &&
                !plan.retained.contains($0.request.content.userInfo["zerus_slot"] as? String ?? "")
        }.map(\.request.identifier)
        center.removeDeliveredNotifications(withIdentifiers: withdrawn)
        await refreshAuthorization()
        guard allowed else {
            state.deliveryNeeded = false
            state.overflowKinds = []
            state.records = state.records.map { record in
                guard state.pendingPosts.contains(record.slot.key) else { return record }
                var copy = record
                copy.postedFingerprint = ""
                copy.postedKind = nil
                return copy
            }
            state.pendingPosts = []
            states[connection.id] = state
            save()
            await summary()
            return state
        }
        let alreadyActive = Set(await cards().compactMap { $0.request.content.userInfo["zerus_slot"] as? String })
        var overflowKinds = plan.overflowKinds
        let ordered = plan.desired.sorted { a, b in
            let aActive = alreadyActive.contains(a.current.slot.key), bActive = alreadyActive.contains(b.current.slot.key)
            if aActive != bActive { return aActive }
            if a.kind.overflows != b.kind.overflows { return a.kind.overflows }
            if (a.kind == .finished) != (b.kind == .finished) { return b.kind == .finished }
            return a.current.slot.key < b.current.slot.key
        }
        for notice in ordered {
            if Task.isCancelled { break }
            let key = notice.current.slot.key
            let record = state.records.first { $0.slot == notice.current.slot }
            let active = await cards()
            let existing = active.first { $0.request.identifier == notice.current.slot.tag }
            let changed = existing.map { card in
                card.request.content.userInfo["zerus_fingerprint"] as? String != notice.fingerprint || card.request.content.title != notice.current.title ||
                    card.request.content.body != notice.body
            } ?? false
            let recovering = state.pendingPosts.contains(key)
            let restore = plan.restoreKinds.contains(notice.kind) && (record?.restoredAt ?? -1) < preferences.enabledAt(notice.kind)
            if existing == nil && record?.postedFingerprint == notice.fingerprint && !recovering && !restore && !notice.fresh { continue }
            if existing != nil && !changed && !recovering && !notice.fresh { continue }
            await pace()
            guard preferences.allows(notice.kind), connected(connection),
                  SessionNotificationPreferences.deliveryAllowed(notice.current,actions:app.messageState.sessionActions) else { continue }
            var admitted = existing != nil || active.count < 24
            if !admitted && notice.kind.overflows {
                // A need replaces a finished turn first, then an optional question.
                let replaceable = active.first { ($0.request.content.userInfo["zerus_kind"] as? String) == SessionAlertKind.finished.rawValue }
                    ?? active.first { ($0.request.content.userInfo["zerus_kind"] as? String) == SessionAlertKind.optional.rawValue }
                if let replaceable {
                    center.removeDeliveredNotifications(withIdentifiers: [replaceable.request.identifier])
                    admitted = true
                }
            }
            guard admitted else {
                if notice.kind.overflows { overflowKinds.insert(notice.kind) }
                continue
            }
            let elapsed = Self.elapsed()
            let audible = notice.fresh && !woke && (state.presenceAlerts || record?.postedFingerprint != notice.fingerprint) && !recovering &&
                NotificationPolicy.canSound(state, elapsed: elapsed)
            state = NotificationPolicy.reserve(state, notice, audible: audible, elapsed: elapsed, enabledAt: preferences.enabledAt(notice.kind))
            // Evidence is saved before delivery; an interrupted post resumes silently.
            states[connection.id] = state
            save()
            guard preferences.allows(notice.kind), connected(connection),
                  SessionNotificationPreferences.deliveryAllowed(notice.current,actions:app.messageState.sessionActions) else { continue }
            do {
                try await center.add(request(notice, audible: audible))
                lastPost = ProcessInfo.processInfo.systemUptime
                state.pendingPosts.removeAll { $0 == key }
                states[connection.id] = state
                save()
            } catch {
                continue
            }
        }
        state.overflowKinds = preferences.generation == plan.state.settingsGeneration ? overflowKinds.filter(preferences.allows) : []
        states[connection.id] = state
        save()
        await summary()
        state.deliveryNeeded = false
        state.pendingRestoreKinds = []
        state.records = state.records.map { record in
            var copy = record
            copy.candidateFingerprint = ""
            copy.candidateTarget = ""
            copy.candidateEventId = 0
            copy.candidateEventAt = 0
            return copy
        }
        states[connection.id] = state
        save()
        if woke { center.removeDeliveredNotifications(withIdentifiers: [Self.wakeIdentifier]) }
        return state
    }

    private func request(_ notice: SessionNotice, audible: Bool) -> UNNotificationRequest {
        let content = UNMutableNotificationContent()
        content.title = notice.current.title
        content.body = notice.body
        content.threadIdentifier = "zerus_workspace:" + notice.current.slot.connection
        content.sound = audible ? .default : nil
        content.interruptionLevel = audible ? .active : .passive
        content.userInfo = ["zerus_connection": notice.current.slot.connection, "zerus_slot": notice.current.slot.key, "zerus_kind": notice.kind.rawValue,
                            "zerus_fingerprint": notice.fingerprint, "zerus_computer": notice.current.slot.computer, "zerus_session": notice.current.slot.session]
        return UNNotificationRequest(identifier: notice.current.slot.tag, content: content, trigger: nil)
    }

    /// One aggregate card for needs that could not be posted individually.
    private func summary() async {
        let preferences = self.preferences
        let needed = preferences.master && states.values.contains { $0.overflowKinds.contains(where: preferences.allows) }
        guard needed else {
            center.removeDeliveredNotifications(withIdentifiers: [Self.summaryIdentifier])
            return
        }
        guard !(await center.deliveredNotifications()).contains(where: { $0.request.identifier == Self.summaryIdentifier }) else { return }
        await pace()
        let content = UNMutableNotificationContent()
        content.title = "More sessions need attention"
        content.body = "Open Zerus to review all workspaces"
        content.interruptionLevel = .passive
        try? await center.add(UNNotificationRequest(identifier: Self.summaryIdentifier, content: content, trigger: nil))
        lastPost = ProcessInfo.processInfo.systemUptime
    }
}
