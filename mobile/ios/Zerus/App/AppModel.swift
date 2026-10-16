import Observation
import SwiftUI
import ZerusKit

/// The application state owner, mirroring Android `ZerusViewModel`. It keeps
/// navigation and request identity on the main actor and runs parsing off it.
@MainActor
@Observable
final class AppModel {
    let client: RelayClient
    private let connectionStore: ConnectionStore

    private(set) var connections: [Connection] = []
    private(set) var machines: [Machine] = []
    private(set) var relayOperations: [String: Set<String>] = [:]
    private(set) var relayFeatures: [String: Set<String>] = [:]
    private(set) var machineOperations: [MachineKey: Set<String>] = [:]
    private(set) var machineFeatures: [MachineKey: Set<String>] = [:]
    private(set) var storageReady = false
    private(set) var demo = false
    private(set) var busy = false
    private(set) var catalogBusy = false
    var error = ""
    /// A pasted or scanned `zerus://pair` link waiting for explicit confirmation.
    var invitation: PairingInvite?

    private var started = false
    private var refreshTask: Task<Void, Never>?
    private var catalogGeneration = 0

    init(client: RelayClient, connectionStore: ConnectionStore) {
        self.client = client
        self.connectionStore = connectionStore
    }

    static func live() -> AppModel { AppModel(client: RelayClient(), connectionStore: KeychainConnectionStore()) }

    func start() async {
        guard !started else { return }
        started = true
        do {
            let store = connectionStore
            connections = try await Task.detached { try store.load() }.value
            storageReady = true
            refresh()
        } catch {
            self.error = "Private storage could not be opened. Existing data has been preserved. \(error.localizedDescription)"
        }
    }

    func scenePhaseChanged(_ phase: ScenePhase) {
        if phase == .active, storageReady { refresh() }
    }

    // MARK: Pairing

    func handle(url: URL) {
        guard let invite = PairingInvite.parse(url.absoluteString) else {
            if url.scheme == "zerus" { error = "This pairing link is not valid. Ask for a new invitation." }
            return
        }
        invitation = invite
    }

    /// Pairs and saves the credential; returns true on success.
    func pair(url: String, code: String) async -> Bool {
        guard storageReady, !busy else { return false }
        busy = true
        defer { busy = false }
        do {
            let connection = try await client.pair(url: url, code: code)
            let next = connections + [connection]
            let store = connectionStore
            try await Task.detached { try store.save(next) }.value
            connections = next
            demo = false
            refresh(explicit: true)
            return true
        } catch {
            self.error = Self.message(error, fallback: "Pairing failed.")
            return false
        }
    }

    /// Revokes this phone on the gateway before removing the local credential.
    func disconnect(_ connection: Connection) async {
        guard !busy else { return }
        busy = true
        defer { busy = false }
        do {
            do { try await client.revoke(connection) } catch let failure as RelayError where failure.status == 401 {}
            let next = connections.filter { $0.id != connection.id }
            let store = connectionStore
            try await Task.detached { try store.save(next) }.value
            connections = next
            machines.removeAll { $0.connectionId == connection.id }
            relayOperations[connection.id] = nil
            relayFeatures[connection.id] = nil
            machineOperations = machineOperations.filter { $0.key.connectionId != connection.id }
            machineFeatures = machineFeatures.filter { $0.key.connectionId != connection.id }
        } catch {
            self.error = "Could not revoke this phone on the gateway. The connection is preserved. \(Self.message(error, fallback: ""))"
        }
    }

    // MARK: Catalog

    func refresh(explicit: Bool = false) {
        guard !demo, storageReady else { return }
        if refreshTask != nil && !explicit { return }
        refreshTask?.cancel()
        catalogGeneration += 1
        let generation = catalogGeneration
        let original = connections
        catalogBusy = true
        refreshTask = Task { [weak self] in
            await self?.loadCatalog(original, explicit: explicit, generation: generation)
        }
    }

    private func loadCatalog(_ original: [Connection], explicit: Bool, generation: Int) async {
        defer {
            if generation == catalogGeneration { catalogBusy = false; refreshTask = nil }
        }
        var nextMachines: [Machine] = []
        var operations: [MachineKey: Set<String>] = [:]
        var features: [MachineKey: Set<String>] = [:]
        var reached = Set<String>()
        for connection in original {
            if explicit || relayOperations[connection.id] == nil {
                if let capabilities = try? await client.capabilities(connection) {
                    relayOperations[connection.id] = Set(capabilities.strings("operations"))
                    relayFeatures[connection.id] = Set(capabilities.strings("features"))
                }
            }
            do {
                let computers = try await client.computers(connection)
                let parsed = await Task.detached { Self.parseComputers(connection, computers) }.value
                guard generation == catalogGeneration, connections == original else { return }
                nextMachines += parsed.machines
                operations.merge(parsed.operations) { $1 }
                features.merge(parsed.features) { $1 }
                reached.insert(connection.id)
            } catch is CancellationError {
                return
            } catch {
                if generation == catalogGeneration { self.error = "\(connection.displayName): \(Self.message(error, fallback: "Sessions are unavailable."))" }
            }
        }
        guard generation == catalogGeneration, connections == original else { return }
        let unavailable = Set(original.map(\.id)).subtracting(reached)
        nextMachines += machines.filter { unavailable.contains($0.connectionId) }.map { machine in
            var copy = machine
            copy.online = false
            copy.lastKnown = true
            return copy
        }
        machines = nextMachines
        machineOperations = operations
        machineFeatures = features
    }

    nonisolated private static func parseComputers(_ connection: Connection, _ response: JSONObject)
        -> (machines: [Machine], operations: [MachineKey: Set<String>], features: [MachineKey: Set<String>]) {
        var machines: [Machine] = []
        var operations: [MachineKey: Set<String>] = [:]
        var features: [MachineKey: Set<String>] = [:]
        for raw in response.objects("computers") {
            guard let machine = try? MachineCatalog.parse(connection.id, raw) else { continue }
            let capabilities = raw.object("snapshot")?.object("mobile_capabilities")
            operations[machine.key] = capabilities?.int("protocol_version") == 1 ? Set(capabilities?.strings("operations") ?? []) : []
            features[machine.key] = Set(capabilities?.strings("features") ?? [])
            machines.append(machine)
        }
        return (machines, operations, features)
    }

    // MARK: Preview

    func preview() {
        demo = true
        machines = [
            Machine(connectionId: "demo", id: "demo", name: "Design workstation", online: true, route: .direct, hgsVersion: "1.49.0"),
            Machine(connectionId: "demo", id: "demo-laptop", name: "Preview laptop", online: true,
                    route: .via(gatewayId: "demo", gatewayName: "Design workstation")),
            Machine(connectionId: "demo", id: "demo-build", name: "Preview build machine with a long display name", online: false,
                    route: .via(gatewayId: "demo", gatewayName: "Design workstation"), lastSeenAt: Date().timeIntervalSince1970 - 3600),
        ]
    }

    func stopPreview() {
        demo = false
        machines = []
        refresh(explicit: true)
    }

    static func message(_ error: Error, fallback: String) -> String {
        if let urlError = error as? URLError {
            switch urlError.code {
            case .notConnectedToInternet, .networkConnectionLost: return "No network connection."
            case .timedOut: return "The gateway did not respond in time."
            case .cannotFindHost, .cannotConnectToHost, .dnsLookupFailed: return "The gateway is unreachable."
            case .serverCertificateUntrusted, .serverCertificateHasBadDate, .serverCertificateNotYetValid, .serverCertificateHasUnknownRoot:
                return "The gateway's HTTPS certificate is not trusted."
            default: break
            }
        }
        let text = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        return text.isEmpty ? fallback : text
    }
}
