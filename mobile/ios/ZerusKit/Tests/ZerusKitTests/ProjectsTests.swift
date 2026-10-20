import XCTest
@testable import ZerusKit

/// Ported from Android ProjectsTest.
final class ProjectsTests: XCTestCase {
    private let connection = Connection(id: "workspace-a", label: "A", url: "https://gateway.example", token: "synthetic")

    private func project(_ id: String, _ name: String, _ names: [String] = [], paths: [String] = []) -> JSON {
        ["id": .string(id), "name": .string(name), "sessions": JSON(names),
         "folders": .array(paths.map { ["id": .string($0), "name": "Folder", "path": .string($0), "local": true] })]
    }
    private func computer(_ id: String, _ groups: [JSON], _ native: [JSON] = [], available: Bool = true) -> JSONObject {
        ["id": .string(id), "name": .string(id.uppercased()),
         "snapshot": ["sessions": .array(native),
                      "mobile_projects": ["schema": 1, "available": .bool(available), "swarm_id": "swarm", "projects": .array(groups)]]]
    }
    private func session(_ name: String, _ cwd: String, _ project: String = "", id: String = "") -> JSON {
        ["name": .string(name), "cwd": .string(cwd), "project": .string(project), "mobile_project_id": .string(id),
         "run_id": .string("run-\(name)"), "conversation_id": "conversation"]
    }
    private func parse(_ computers: [JSONObject], _ workspace: Connection? = nil) -> ProjectCatalog {
        let workspace = workspace ?? connection
        return ProjectParser.parse(workspace, computers, computers.flatMap { NativeSessions.sessions(workspace, $0) })
    }

    func testTaskDigestsComeFromCataloguedProjectsOnly() {
        func orchard(_ digest: String?) -> JSON {
            var fields: JSONObject = ["id": "orchard", "name": "Orchard", "sessions": .array([]), "folders": .array([])]
            if let digest { fields = fields.putting("tasks_digest", .string(digest)) }
            return .object(fields)
        }
        let a = String(repeating: "a", count: 64), b = String(repeating: "b", count: 64)
        let computers = [computer("computer-1", [orchard(a)]), computer("computer-2", [orchard(b)]),
                         computer("computer-3", [orchard("xyz")]), computer("computer-4", [orchard(String(repeating: "A", count: 64))]),
                         computer("computer-5", [orchard(nil)], [session("loose", "/elsewhere")])]
        let result = parse(computers)
        let key = ProjectKey(connectionId: "workspace-a", swarmId: "swarm", id: "orchard")
        XCTAssertEqual(result.taskDigests, [key: ["computer-1": a, "computer-2": b]])
        // The session outside every project forms an uncatalogued group, which has no tasks.
        XCTAssertTrue(result.projects.contains { !$0.catalogued })
    }

    func testSharedCanonicalIdsMergeAcrossMachinesButDuplicateNamesDoNot() {
        let a = computer("a", [project("p", "Same", ["one"]), project("q", "Same")], [session("one", "/a", id: "p")])
        let b = computer("b", [project("p", "Same", ["two"]), project("q", "Same")], [session("two", "/b", id: "p")])
        let result = parse([a, b])
        XCTAssertEqual(result.projects.count, 2)
        XCTAssertEqual(Set(result.projects.first { $0.key.id == "p" }!.computers.keys), ["a", "b"])
        XCTAssertEqual(Set(result.sessions.map(\.projectKey)).count, 1)
        let another = parse([a], Connection(id: "workspace-b", label: "A", url: "https://gateway.example", token: "synthetic"))
        XCTAssertNotEqual(result.sessions[0].projectKey, another.sessions[0].projectKey)
    }

    func testProjectsAndSessionGroupsKeepTheSharedCatalogOrder() {
        let result = parse([computer("a", [project("z-first", "Zeta", ["one"]), project("m", "Alpha", ["two"]), project("b-last", "Mid", ["three"])],
                                     [session("three", "/c"), session("one", "/a"), session("two", "/b")])])
        XCTAssertEqual(result.projects.map(\.key.id), ["z-first", "m", "b-last"])
        XCTAssertEqual(SessionFilters.grouped(result.sessions, result.projects).map(\.name), ["Zeta", "Alpha", "Mid"])
    }

    func testOfflinePeerWithoutSnapshotIsNotACatalogFailure() {
        let online = computer("a", [project("p", "Project")]).putting("online", true)
        let offline: JSONObject = ["id": "mac", "name": "Mac", "online": false, "snapshot": .null]
        let pending: JSONObject = ["id": "new", "name": "New", "online": true, "snapshot": .null]
        let result = parse([online, offline, pending])
        XCTAssertEqual(result.unavailableComputers, ["New"])
        XCTAssertEqual(result.projects.map(\.key.id), ["p"])
    }

    func testEmptyHeadersAreVisibleWithZeroComputers() {
        let result = parse([computer("a", [project("empty", "Empty")]), computer("b", [project("empty", "Empty")])])
        XCTAssertEqual(result.projects.count, 1)
        XCTAssertEqual(result.projects[0].name, "Empty")
        XCTAssertTrue(result.projects[0].computers.isEmpty)
        XCTAssertTrue(result.sessions.isEmpty)
    }

    func testCanonicalMembershipWinsOverFolderAndLegacyName() {
        let result = parse([computer("a", [project("assigned", "Logical", ["one"]), project("folder", "Folder", paths: ["/work"])], [session("one", "/work", "Legacy")])])
        XCTAssertEqual(result.sessions[0].projectKey?.id, "assigned")
        XCTAssertEqual(result.sessions[0].project, "Logical")
        let explicit = parse([computer("a", [project("explicit", "Selected"), project("named", "Other", ["one"])], [session("one", "/work", id: "explicit")])])
        XCTAssertEqual(explicit.sessions[0].projectKey?.id, "explicit")
    }

    func testPathFallbackIsExactCaseSensitiveAndNeverAncestorGuess() {
        let result = parse([computer("a", [project("p", "Project", paths: ["/Work"])], [session("exact", "/Work"), session("case", "/work"), session("child", "/Work/child")])])
        XCTAssertEqual(result.sessions.first { $0.target.session == "exact" }?.projectKey?.id, "p")
        XCTAssertNotEqual(result.sessions.first { $0.target.session == "case" }?.projectKey?.id, "p")
        XCTAssertNotEqual(result.sessions.first { $0.target.session == "child" }?.projectKey?.id, "p")
        XCTAssertEqual(result.projects.count, 3)
    }

    func testUncataloguedAliasesStayMachineLocalAndPreserveCase() {
        let result = parse([computer("a", [], [session("one", "/a", "Named"), session("lower", "/lower", "named")], available: false),
                            computer("b", [], [session("two", "/b", "Named")], available: false)])
        XCTAssertEqual(result.projects.count, 3)
        XCTAssertTrue(result.projects.allSatisfy { !$0.catalogued })
        XCTAssertNotEqual(result.sessions[0].projectKey, result.sessions[2].projectKey)
        XCTAssertEqual(Set(result.unavailableComputers), ["A", "B"])
    }

    func testOrdinaryFoldersAndUnassignedSessionsRemainAccessible() {
        let raw: JSONObject = ["id": "a", "snapshot": ["sessions": [session("plain", "/ordinary"), session("unknown", "")]]]
        let result = parse([raw])
        XCTAssertEqual(result.sessions.count, 2)
        XCTAssertEqual(result.projects.count, 2)
        XCTAssertTrue(result.projects.contains { $0.name == "ordinary" && $0.folders.first?.path == "/ordinary" })
        XCTAssertTrue(result.projects.contains { $0.name == "Unassigned sessions" })
    }

    func testArchiveRangesCoverWholeLocalDays() {
        let zone = TimeZone(identifier: "UTC")!
        var calendar = Calendar(identifier: .gregorian); calendar.timeZone = zone
        let today = calendar.date(from: DateComponents(year: 2026, month: 10, day: 16))!
        let week = ArchiveRange.period(.week, first: today, last: today, today: today, timeZone: zone)
        XCTAssertEqual(week.from, Int64(calendar.date(from: DateComponents(year: 2026, month: 10, day: 10))!.timeIntervalSince1970))
        XCTAssertEqual(week.until, Int64(calendar.date(from: DateComponents(year: 2026, month: 10, day: 17))!.timeIntervalSince1970))
        XCTAssertEqual(ArchiveRange.period(.all, first: today, last: today, today: today, timeZone: zone), ArchiveRange())
        XCTAssertTrue(ArchiveRange().valid)
        XCTAssertFalse(ArchiveRange(from: 10, until: 10).valid)
    }
}

final class ReadFailureTests: XCTestCase {
    func testReadNetworkFailureDoesNotExposeEndpoint() {
        let text = ReadFailure.message(URLError(.cannotFindHost, userInfo: [NSURLErrorFailingURLStringErrorKey: "https://private.invalid"]), "conversation")
        XCTAssertEqual(text, "No connection. Showing saved conversation. Refresh to try again.")
        XCTAssertEqual(ReadFailure.message(RelayError(status: 404, message: "Session is no longer available."), "conversation"), "Session is no longer available.")
    }
}

@MainActor
final class ArchivePagingTests: XCTestCase {
    private let key = MachineKey(connectionId: "w", computerId: "c")
    private func row(_ id: String) -> Session {
        Session(target: Target(computerId: "c", session: "codex/p/\(id)", run: "r", conversation: "v", connectionId: "w", archiveId: id),
                title: id, agent: "codex", status: "archived", project: "", preview: "", pendingCount: 0, raw: ["state": "archived"])
    }

    func testPagesReplaceTheWindowAndStaleGenerationsAreDropped() async throws {
        var queries: [JSONObject] = []
        let paging = ArchivePaging(load: { _, query in
            queries.append(query)
            return query.string("cursor").isEmpty ? ArchivePage(sessions: [self.row("a")], cursor: "next", total: 2)
                : ArchivePage(sessions: [self.row("b")], cursor: "", total: 2, previous: "prev", position: 1)
        })
        paging.configure(machines: [key], query: "", visible: true)
        while paging.loading { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertEqual(paging.rows.map(\.title), ["a"])
        XCTAssertTrue(paging.hasMore)
        paging.loadMore()
        while paging.loading { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertEqual(paging.rows.map(\.title), ["b"])
        XCTAssertFalse(paging.hasMore)
        XCTAssertTrue(paging.hasNewer)
        XCTAssertEqual(queries.map { $0.string("cursor") }, ["", "next"])
        XCTAssertEqual(queries[0].int("limit"), 50)
    }

    func testInvalidPagesReportAnErrorWithoutRows() async throws {
        let paging = ArchivePaging(load: { _, _ in ArchivePage(sessions: [], cursor: "more", total: 0) })
        paging.configure(machines: [key], query: "", visible: true)
        while paging.loading { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertTrue(paging.rows.isEmpty)
        XCTAssertEqual(paging.errors[key], "Computer returned an invalid archive page. Refresh the archive.")
    }
}
