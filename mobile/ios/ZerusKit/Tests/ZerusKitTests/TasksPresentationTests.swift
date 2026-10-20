import Foundation
import XCTest
import ZerusReplica
@testable import ZerusKit

/// Ported from Android TasksPresentationTest: what the Tasks screen shows and where its actions go.
final class TasksPresentationTests: XCTestCase {
    private let here = Machine(connectionId: "workspace-a", id: "computer-1", name: "Studio", online: true, machineId: "node-1")
    private let laptop = Machine(connectionId: "workspace-a", id: "computer-2", name: "Laptop", online: true, machineId: "node-2")
    private let chosen = ChosenSession(name: "codex/api", computerId: "computer-1", machineId: "node-1")
    private let exchange: Set<String> = ["task_exchange", "task_send", "task_cancel", "task_done"]
    private var serial = 0

    private func task(_ stage: TaskStage, _ target: TaskTarget = .anySession, session: SessionRef? = nil,
                      summary: String = "", cancelledAt: Int64? = nil, reason: String = "") -> TaskInfo {
        serial += 1
        return TaskInfo(uuid: String(format: "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e%02d", serial), id: String(format: "T-a1b2%02d", serial),
                        project: "project-1", schema: 1, writable: true, createdAt: 1, stageAt: 1, author: .person, stage: stage,
                        target: target, rank: "V", session: session, summary: summary, cancelledAt: cancelledAt,
                        cancelReason: reason, text: "Task \(serial)", log: [])
    }
    private func pinned(_ name: String, _ machine: String = "node-1") -> TaskTarget { .session(session: SessionRef(machine: machine, session: name)) }
    private func session(_ name: String, _ computer: String = "computer-1", _ raw: JSONObject = ["activity": "idle"]) -> Session {
        Session(target: Target(computerId: computer, session: name, run: "run", conversation: "conversation", connectionId: "workspace-a"),
                title: name, agent: String(name.split(separator: "/").first ?? ""), status: "", project: "orchard", preview: "",
                pendingCount: 0, raw: raw)
    }
    private func route(_ task: TaskInfo, _ machines: [Machine]? = nil, ops: Set<String>? = nil, sessions: [Session]? = nil) -> RouteState {
        let ops = ops ?? exchange
        return TasksPresentation.route(task, chosen: chosen, machines: machines ?? [here, laptop], connectionId: "workspace-a",
                                       operations: { _ in ops }, sessions: sessions ?? [session("codex/api"), session("claude/web", "computer-2")])
    }
    private func cards(_ tasks: [TaskInfo], _ tab: TaskTab, only: Bool = false,
                       send: ((TaskInfo) -> RouteState)? = nil) -> [TaskCard] {
        TasksPresentation.cards(tasks, tab: tab, chosen: chosen, onlyThisSession: only,
                                send: send ?? { self.route($0) }, cancel: { self.route($0) })
    }

    func testStagesKeepTheDesktopOrderAndDefaultToReviewWhenItHasTasks() {
        XCTAssertEqual(TaskTab.allCases.map(\.label), ["In review", "In progress", "To do", "Backlog", "Done"])
        let tasks = [task(.todo), task(.backlog)]
        XCTAssertEqual(TasksPresentation.defaultTab(TasksPresentation.counts(tasks, chosen: chosen, onlyThisSession: false)), .todo)
        let review = tasks + [task(.review)]
        XCTAssertEqual(TasksPresentation.defaultTab(TasksPresentation.counts(review, chosen: chosen, onlyThisSession: false)), .review)
    }

    func testEachStageHasTheSpecsActionsInOrder() {
        XCTAssertEqual(TasksPresentation.actions(.todo), [.edit, .toBacklog, .toTop, .send])
        XCTAssertEqual(TasksPresentation.actions(.backlog), [.edit, .toTodo, .send])
        XCTAssertEqual(TasksPresentation.actions(.active), [.cancel])
        XCTAssertEqual(TasksPresentation.actions(.review), [.backToTodo, .accept])
        XCTAssertEqual(TasksPresentation.actions(.done), [.backToTodo])
        XCTAssertEqual([TaskAction.edit, .toBacklog, .toTop, .send, .toTodo, .cancel, .backToTodo, .accept].map(\.label),
                       ["Edit", "Move to Backlog", "Move to top", "Send now", "Move to To do", "Cancel", "Back to To do", "Accept"])
    }

    func testOnlyThisSessionKeepsItsOwnAndAnySessionTasksInTodoAndBacklog() {
        let mine = task(.todo, pinned("codex/api"))
        let other = task(.todo, pinned("claude/web", "node-2"))
        let any = task(.backlog)
        let sentHere = task(.active, session: SessionRef(machine: "node-1", session: "codex/api"))
        let sentElsewhere = task(.review, session: SessionRef(machine: "node-2", session: "claude/web"))
        let anyReview = task(.review)
        let all = [mine, other, any, sentHere, sentElsewhere, anyReview]
        XCTAssertEqual(TasksPresentation.counts(all, chosen: chosen, onlyThisSession: false), [.review: 2, .active: 1, .todo: 2, .backlog: 1])
        XCTAssertEqual(TasksPresentation.counts(all, chosen: chosen, onlyThisSession: true), [.review: 0, .active: 1, .todo: 1, .backlog: 1])
        XCTAssertEqual(cards(all, .todo).map(\.highlighted), [true, false])
        XCTAssertEqual(cards(all, .active).map(\.highlighted), [true])
    }

    func testCardsNameTheirSessionAndInReviewShowsTheReportOrTheCancellation() {
        let any = task(.todo)
        let reported = task(.review, session: SessionRef(machine: "node-1", session: "codex/api"), summary: "Token bucket; 6 tests.")
        let cancelled = task(.review, session: SessionRef(machine: "node-1", session: "codex/api"), summary: "Started",
                             cancelledAt: 5, reason: "Wrong approach")
        let all = [any, reported, cancelled]
        XCTAssertEqual(cards(all, .todo).map(\.sessionLabel), ["any session"])
        let review = cards(all, .review)
        XCTAssertEqual(review.map(\.sessionLabel), ["codex/api", "codex/api"])
        XCTAssertEqual(review.map(\.note), ["Token bucket; 6 tests.", "Cancelled: Wrong approach"])
    }

    func testSendNowGoesToThePinnedSessionsComputerOrTheChosenSession() {
        XCTAssertEqual(route(task(.todo, pinned("claude/web", "node-2"))), .ready(SendRoute(computerId: "computer-2", session: "claude/web")))
        XCTAssertEqual(route(task(.backlog)), .ready(SendRoute(computerId: "computer-1", session: "codex/api")))
        XCTAssertEqual(TasksPresentation.route(task(.todo), chosen: nil, machines: [here], connectionId: "workspace-a",
                                               operations: { _ in self.exchange }, sessions: []),
                       .blocked("Choose a session for this task"))
    }

    func testSendNowIsBlockedWithTheReason() {
        let elsewhere = task(.todo, pinned("claude/web", "node-2"))
        var offline = laptop
        offline.online = false
        var lastKnown = laptop
        lastKnown.lastKnown = true
        XCTAssertEqual(route(elsewhere, [here, offline]), .blocked("Laptop is offline"))
        XCTAssertEqual(route(elsewhere, [here, lastKnown]), .blocked("Laptop is offline"))
        XCTAssertEqual(route(elsewhere, ops: ["task_exchange"]), .blocked("Laptop needs an update to send tasks"))
        XCTAssertEqual(route(elsewhere, sessions: [session("codex/api")]), .blocked("claude/web no longer exists — choose another session"))
        XCTAssertEqual(route(elsewhere, [here]), .blocked("claude/web is on a computer this phone does not reach"))
        let card = cards([elsewhere], .todo, send: { self.route($0, [self.here, offline]) })[0]
        let send = card.actions.first { $0.action == .send }
        XCTAssertEqual(send?.enabled, false)
        XCTAssertEqual(send?.reason, "Laptop is offline")
        XCTAssertTrue(card.actions.filter { $0.action != .send }.allSatisfy(\.enabled))
    }

    func testCancelGoesToTheSessionsComputerOrAnyOnlineComputerOfTheProject() {
        let active = task(.active, session: SessionRef(machine: "node-2", session: "claude/web"))
        var offline = laptop
        offline.online = false
        var down = here
        down.online = false
        XCTAssertEqual(TasksPresentation.cancelRoute(active, machines: [here, laptop], operations: { _ in self.exchange }),
                       .ready(SendRoute(computerId: "computer-2", session: "claude/web")))
        // That computer is offline: another one cancels it there, as hgs does.
        XCTAssertEqual(TasksPresentation.cancelRoute(active, machines: [here, offline], operations: { _ in self.exchange }),
                       .ready(SendRoute(computerId: "computer-1", session: "claude/web", direct: false)))
        XCTAssertEqual(TasksPresentation.cancelRoute(active, machines: [down], operations: { _ in self.exchange }),
                       .blocked("No computer of this project is online"))
    }

    func testASessionOnAComputerWithoutANativeIdTakesNoTasks() {
        let old = Machine(connectionId: "workspace-a", id: "computer-3", name: "Old", online: true)
        let unknown = ChosenSession(name: "codex/old", computerId: "computer-3", machineId: "")
        XCTAssertEqual(TasksPresentation.addReason(unknown), "codex/old is on a computer that needs an update to take tasks")
        XCTAssertEqual(TasksPresentation.addReason(nil), "Choose a session for new tasks")
        XCTAssertEqual(TasksPresentation.addReason(chosen), "")
        // Neither an Any session task nor a blank pin reaches the computer that reports no ID.
        let sessions = [session("codex/old", "computer-3")]
        XCTAssertEqual(TasksPresentation.route(task(.todo), chosen: unknown, machines: [old], connectionId: "workspace-a",
                                               operations: { _ in self.exchange }, sessions: sessions),
                       .blocked("codex/old is on a computer that needs an update to take tasks"))
        XCTAssertEqual(TasksPresentation.route(task(.todo, pinned("codex/old", "")), chosen: chosen, machines: [old], connectionId: "workspace-a",
                                               operations: { _ in self.exchange }, sessions: sessions),
                       .blocked("codex/old is on a computer that needs an update to take tasks"))
        let active = task(.active, session: SessionRef(machine: "", session: "codex/old"))
        XCTAssertEqual(TasksPresentation.cancelRoute(active, machines: [old, here], operations: { _ in self.exchange }),
                       .ready(SendRoute(computerId: "computer-1", session: "codex/old", direct: false)))
    }

    func testChipsShowEachSessionsStatus() {
        let sessions = [session("codex/api", "computer-1", ["activity": "busy"]), session("claude/web", "computer-2", ["phase": "input"]),
                        session("codex/docs")]
        let chips = TasksPresentation.chips(sessions, machines: [here, laptop])
        XCTAssertEqual(chips.map(\.dot), [.working, .attention, .ready])
        XCTAssertEqual(chips[1].session, ChosenSession(name: "claude/web", computerId: "computer-2", machineId: "node-2"))
    }

    func testDoneShowsTheLastSevenDaysThenOlderOnRequest() {
        XCTAssertEqual(TasksPresentation.olderLabel(total: 10, recent: 3, shown: 3, expanded: false, more: true), "Show older (7)")
        XCTAssertNil(TasksPresentation.olderLabel(total: 3, recent: 3, shown: 3, expanded: false, more: false))
        XCTAssertEqual(TasksPresentation.olderLabel(total: 10, recent: 3, shown: 8, expanded: true, more: true), "Show older (2)")
        XCTAssertNil(TasksPresentation.olderLabel(total: 10, recent: 3, shown: 10, expanded: true, more: false))
    }
}
