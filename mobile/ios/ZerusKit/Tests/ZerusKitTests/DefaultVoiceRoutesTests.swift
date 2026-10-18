import XCTest
@testable import ZerusKit

/// Ported from Android DefaultVoiceRoutesTest.
final class DefaultVoiceRoutesTests: XCTestCase {
    private let native = "b4075bf0-9c22-4a67-b768-55af2d2936a4"
    private let connections = [Connection(id: "workspace", label: "Fixture", url: "https://relay.example.test", token: "fixture-token")]
    private var machine: Machine { Machine(connectionId: "workspace", id: "relay-computer", name: "Fixture computer", online: true, machineId: native) }

    private func with(_ machine: Machine, _ change: (inout Machine) -> Void) -> Machine {
        var copy = machine
        change(&copy)
        return copy
    }

    func testGlobalDefaultsResolveNativeOrLegacyIdsThroughTheExactWorkspace() {
        let expected = SpeechConfigHost(key: MachineKey(connectionId: "workspace", computerId: machine.id), machineId: native, name: machine.name)
        XCTAssertEqual(DefaultVoiceRoutes.resolve(MachineKey(connectionId: "workspace", computerId: native), machines: [machine], connections: connections), expected)
        XCTAssertEqual(DefaultVoiceRoutes.resolve(MachineKey(connectionId: "workspace", computerId: machine.id), machines: [machine], connections: connections), expected)
        XCTAssertNil(DefaultVoiceRoutes.resolve(MachineKey(connectionId: "another-workspace", computerId: native), machines: [machine], connections: connections))
    }

    func testOfflineStaleAmbiguousAndUnverifiedMachinesCannotSupplyProfiles() {
        let selected = MachineKey(connectionId: "workspace", computerId: native)
        for rows in [[with(machine) { $0.online = false }], [with(machine) { $0.lastKnown = true }], [with(machine) { $0.machineId = "" }],
                     [machine, with(machine) { $0.id = "another-relay-id" }]] {
            XCTAssertNil(DefaultVoiceRoutes.resolve(selected, machines: rows, connections: connections))
        }
        XCTAssertNil(DefaultVoiceRoutes.resolve(selected, machines: [machine], connections: []))
        XCTAssertNil(DefaultVoiceRoutes.resolve(nil, machines: [machine], connections: connections))
    }

    func testGlobalComputerPickerIsBoundedAndCanFindAComputerBeyondItsFirstPage() {
        let rows = (0...150).map { index in
            with(machine) {
                $0.id = "route-\(index)"
                $0.name = "Fixture \(index)"
                $0.machineId = String(format: "00000000-0000-4000-8000-%012d", index + 1)
            }
        }
        XCTAssertEqual(DefaultVoiceRoutes.hosts(rows, connections: connections).count, 100)
        XCTAssertEqual(DefaultVoiceRoutes.hosts(rows, connections: connections, query: "150").map(\.name), ["Fixture 150"])
        XCTAssertEqual(DefaultVoiceRoutes.resolve(MachineKey(connectionId: "workspace", computerId: rows[150].machineId), machines: rows, connections: connections)?.name,
                       "Fixture 150")
        XCTAssertTrue(DefaultVoiceRoutes.hosts(rows.map { row in self.with(row) { $0.online = false } }, connections: connections).isEmpty)
    }

    func testProfilesAreAComputerScopedRead() async throws {
        StubProtocol.reset { request in
            request.method == "POST" ? StubProtocol.json(200, #"{"request_id":"profiles","state":"completed","result":{"schema":1,"profiles":[]}}"#)
                : StubProtocol.json(500, "{}")
        }
        let host = MachineKey(connectionId: "workspace", computerId: "relay-computer")
        let result = try await StubProtocol.client().speech(connections[0], host: host, operation: "speech_profiles", payload: [:], id: "profiles",
                                                            fallback: "unavailable")
        XCTAssertEqual(try SpeechProtocol.profiles(result), [])
        let request = try XCTUnwrap(StubProtocol.requests.first)
        XCTAssertEqual(request.json?.string("operation"), "speech_profiles")
        XCTAssertEqual(request.json?.string("computer_id"), "relay-computer")
        XCTAssertEqual(StubProtocol.requests.count, 1)
    }
}
