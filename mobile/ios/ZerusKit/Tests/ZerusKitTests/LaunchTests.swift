import XCTest
@testable import ZerusKit

/// Ported from Android LaunchPresentationTest and DirectoryCreationTest.
final class LaunchPresentationTests: XCTestCase {
    func testOpenCodeMarksSurviveTheSanitizedCatalog() throws {
        let contract = try XCTUnwrap(SharedFixture.json("opencode-provider-catalog.json").object)
        let raw: JSONObject = ["agents": ["opencode"], "accounts": .array([.object(try XCTUnwrap(contract.object("stopped_account")))])]
        let clean = LaunchPresentation.sanitizeCatalog(raw)
        let models = LaunchPresentation.models(clean, provider: "opencode", accountId: "native-opencode")
        XCTAssertEqual(models.map(\.value), ["fixture/local"]); XCTAssertTrue(models[0].isDefault)
        XCTAssertEqual(models[0].providerLabel, "Private keyless fixture")
        XCTAssertEqual(LaunchPresentation.catalogNotice(clean, provider: "opencode", accountId: "native-opencode"),
                       "OpenCode is not running on fixture Showing a shortened list. Choose other models on the desktop.")
        XCTAssertEqual(LaunchPresentation.catalogNotice(clean, provider: "codex", accountId: "native-opencode"), "")
        XCTAssertEqual(LaunchPresentation.catalogNotice(clean, provider: "opencode", accountId: "missing"), "")
    }
    private let machine = MachineKey(connectionId: "workspace", computerId: "computer")
    private func choices(_ raw: JSONObject?, _ catalogs: [AccountCatalog], _ now: Double, provider: String = "", account: String = "") -> [LaunchAccountChoice] {
        LaunchPresentation.accountChoices(raw, machine: machine, catalogs: catalogs, now: now, selectedProvider: provider, selectedAccount: account) { $0.lowercased() }
    }
    private func usage(_ percent: Double = 17, reset: Double = 2000, email: String = "shared@example.invalid", accountId: String? = nil,
                       organization: String? = nil, checkedAt: Double = 1000, extra: JSONObject = [:]) -> JSON {
        var identity: JSONObject = ["email": .string(email)]
        if let accountId { identity["account_id"] = .string(accountId) }
        if let organization { identity["organization"] = .string(organization) }
        var value: JSONObject = ["status": "ok", "checked_at": .double(checkedAt), "identity": .object(identity),
                                 "windows": [["window_minutes": 300, "used_percent": .double(percent), "resets_at": .double(reset)]]]
        for key in extra.keys { value[key] = extra[key] }
        return .object(value)
    }
    private func catalog(_ first: JSON? = nil, _ second: JSON? = nil, firstAuth: String? = nil, single: Bool = false) -> JSONObject {
        var one: JSONObject = ["id": "one", "provider": "codex", "label": "Alias", "is_default": true]
        if let first { one["usage"] = first }
        if let firstAuth { one["auth_status"] = .string(firstAuth) }
        var two: JSONObject = ["id": "two", "provider": "codex", "label": "Alias"]
        if let second { two["usage"] = second }
        return ["agents": ["codex"], "accounts": .array(single ? [.object(one)] : [.object(one), .object(two)])]
    }
    private func cached(_ usage: JSON, key: MachineKey? = nil) -> AccountCatalog {
        AccountCatalog(key: key ?? machine, machineName: "Example", supported: true, available: true, stale: false, online: true, observedAt: 1000,
                       accounts: [AccountSnapshots.account(["id": "one", "provider": "codex", "label": "Alias", "usage": usage])!])
    }

    func testCachedRowsUseEmailAndOneLimitLineWithoutDefaultOrFailureDetails() {
        let raw = catalog(usage(extra: ["refresh_error": true, "error": "private detail"]))
        let result = choices(LaunchPresentation.sanitizeCatalog(raw), [], 1000)
        XCTAssertEqual("shared@example.invalid", result[0].title)
        XCTAssertEqual("5h 17% used", result[0].limits)
        XCTAssertEqual("limits n/a", result[1].limits)
        XCTAssertEqual(["codex/one", "codex/two"], result.map(\.key))
        XCTAssertFalse(result.contains { $0.title.contains("default") || $0.limits.contains("detail") })
        XCTAssertFalse(JSON.object(LaunchPresentation.sanitizeCatalog(raw)).compact.contains("private detail"))
    }

    func testPrivateUsageCacheCannotCrossComputerOrProfileAndAuthChangesRejectOldQuotas() {
        let cache = cached(usage())
        let result = choices(catalog(), [cache], 1000)
        XCTAssertEqual("5h 17% used", result[0].limits)
        XCTAssertEqual("limits n/a", result[1].limits)
        XCTAssertEqual("limits n/a", choices(catalog(), [cached(usage(), key: MachineKey(connectionId: "other", computerId: "computer"))], 1000)[0].limits)
        XCTAssertEqual("limits n/a", choices(catalog(firstAuth: "signed_out"), [cache], 1000)[0].limits)
        XCTAssertEqual("new@example.invalid", choices(catalog(usage(email: "new@example.invalid")), [cache], 1000)[0].title)
        let newScope = catalog(usage(accountId: "new-scope"))
        XCTAssertEqual("5h 17% used", choices(newScope, [cached(usage(99, checkedAt: 1100))], 1100)[0].limits)
    }

    func testEndedAndUnknownCachedWindowsStayUnavailableAndNewerSnapshotsWin() {
        XCTAssertEqual("limits n/a", choices(catalog(usage(reset: 999)), [], 1000)[0].limits)
        let unknown: JSON = ["status": "ok", "checked_at": 1000.0, "identity": ["email": "shared@example.invalid"],
                             "windows": [["window_minutes": 300, "resets_at": 2000.0]]]
        XCTAssertEqual("limits n/a", choices(catalog(unknown), [], 1000)[0].limits)
        XCTAssertEqual("5h 29% used", choices(catalog(usage()), [cached(usage(29, checkedAt: 1100))], 1100)[0].limits)
    }

    func testAliasesMatchAccountsGroupingOnlyOnSelectedComputerAndKeepSelectedBinding() {
        let raw = catalog(usage(), usage(29, checkedAt: 1100))
        let merged = choices(raw, [], 1100)
        XCTAssertEqual(1, merged.count)
        XCTAssertEqual("one", merged.first?.account.id)
        XCTAssertEqual("5h 29% used", merged.first?.limits)
        XCTAssertEqual("two", choices(raw, [], 1100, provider: "codex", account: "two").first?.account.id)
        let scoped = catalog(usage(), usage(29, organization: "Other", checkedAt: 1100))
        XCTAssertEqual(2, choices(scoped, [], 1100).count)
        let ids = catalog(usage(accountId: "first"), usage(29, accountId: "second", checkedAt: 1100))
        XCTAssertEqual(2, choices(ids, [], 1100).count)
        let other = cached(usage(91), key: MachineKey(connectionId: "workspace", computerId: "other-computer"))
        XCTAssertEqual(1, choices(catalog(single: true), [other], 1100).count)
        XCTAssertEqual("limits n/a", choices(catalog(single: true), [other], 1100).first?.limits)
        XCTAssertTrue(choices(nil, [other], 1100).isEmpty)
    }

    func testProvidersAndAccountsComeOnlyFromTypedNativeCatalog() {
        let raw: JSONObject = ["agents": ["codex", "unknown", 1, "codex", "kimi"],
                               "accounts": [["id": "safe-id", "provider": "codex", "label": "Work"], ["id": 1, "provider": "codex", "label": "Wrong type"],
                                            ["id": "other", "provider": "kimi", "label": "Other"]]]
        XCTAssertEqual(["codex", "kimi"], LaunchPresentation.agents(raw))
        XCTAssertEqual([LaunchAccount(id: "safe-id", provider: "codex", label: "Work")], LaunchPresentation.accounts(raw, "codex"))
    }

    func testLimitBarsUseDesktopFooterPaceAndSkipUnavailableWindows() {
        let now = 1000.0
        func meter(_ minutes: Double, _ used: Double, _ left: Double) -> LaunchLimitPace {
            LaunchPresentation.limitPace(AccountWindow(id: "", label: "", usedPercent: used, minutes: minutes, resetsAt: now + left), now: now)
        }
        XCTAssertEqual(.normal, meter(300, 80, 30 * 60).tone)
        XCTAssertEqual(.critical, meter(10080, 48, 145 * 3600).tone)
        XCTAssertEqual(.warning, meter(10080, 92, 3 * 3600).tone)
        XCTAssertEqual(.warning, meter(10080, 61, 76 * 3600).tone)
        XCTAssertEqual(.normal, meter(10080, 6, 165 * 3600).tone)
        XCTAssertEqual(.critical, meter(300, 100, 47 * 60).tone)
        XCTAssertEqual(0.9, meter(300, 80, 30 * 60).elapsed!, accuracy: 0.0001)
        XCTAssertNil(LaunchPresentation.limitPace(AccountWindow(id: "", label: "", usedPercent: 78, minutes: 300, resetsAt: nil), now: now).elapsed)
        var account = AccountSnapshots.account(["id": "one", "provider": "codex", "usage": usage()])!
        account.windows = [AccountWindow(id: "", label: "", usedPercent: nil, minutes: 300, resetsAt: now + 60),
                           AccountWindow(id: "", label: "", usedPercent: 100, minutes: 300, resetsAt: now - 1)]
        XCTAssertTrue(LaunchPresentation.limitWindows(account, now: now).isEmpty)
        XCTAssertEqual("limits n/a", LaunchPresentation.accountLimits(account, now: now))
    }

    func testSelectionMustStillBeInCurrentVerifiedBrowser() {
        let browser: JSONObject = ["path": "/srv/projects", "directories": [["path": "/srv/projects/example", "name": "Example"]]]
        XCTAssertTrue(LaunchPresentation.selectedFolder(browser, "/srv/projects"))
        XCTAssertTrue(LaunchPresentation.selectedFolder(browser, "/srv/projects/example"))
        XCTAssertFalse(LaunchPresentation.selectedFolder(browser, "/srv/other"))
        XCTAssertFalse(LaunchPresentation.selectedFolder(browser, "~"))
        XCTAssertFalse(LaunchPresentation.absoluteFolder("/srv/new\nfolder"))
    }

    func testNativeTagLimitsAreExplicitAndUtf8Bounded() {
        XCTAssertEqual("", LaunchPresentation.nameError("mobile launch"))
        XCTAssertNotEqual("", LaunchPresentation.nameError("../escape"))
        XCTAssertNotEqual("", LaunchPresentation.nameError(" example "))
        XCTAssertEqual("", LaunchPresentation.nameError(String(repeating: "界", count: 40)))
        XCTAssertNotEqual("", LaunchPresentation.nameError(String(repeating: "界", count: 41)))
    }
}

final class DirectoryCreationTests: XCTestCase {
    private let machine = MachineKey(connectionId: "workspace", computerId: "computer")
    private var action: SessionAction {
        SessionAction(requestId: "11111111-1111-4111-8111-111111111111", target: Target(computerId: "computer", session: "", run: "", conversation: "", connectionId: "workspace"),
                      operation: DirectoryCreation.operation, arguments: ["path": "/alias/New project ; $ literal"])
    }
    private func receipt(_ change: JSONObject = [:], id: String? = nil) -> JSONObject {
        let request = id ?? action.requestId
        var native: JSONObject = ["request_id": .string(request), "status": "created", "requested_path": "/alias/New project ; $ literal",
                                  "path": "/canonical/New project ; $ literal", "parent": "/canonical"]
        for key in change.keys { native[key] = change[key] }
        return ["request_id": .string(request), "state": "completed", "result": .object(native)]
    }

    func testAbsoluteLiteralPathIsPreservedAndUtf8Bounded() throws {
        XCTAssertEqual("/alias/New project ; $ literal", try SessionActionPolicies.arguments(DirectoryCreation.operation, action.arguments).string("path"))
        XCTAssertEqual("", DirectoryCreation.pathError("/" + String(repeating: "я", count: 2047)))
        XCTAssertFalse(DirectoryCreation.pathError("/" + String(repeating: "я", count: 2048)).isEmpty)
        for bad in ["relative", "~/new", "/", "/new/", "/new/../other", "/new//other", "/new\nother", "/new\u{7f}other"] {
            XCTAssertFalse(DirectoryCreation.pathError(bad).isEmpty, bad)
        }
        XCTAssertThrowsError(try SessionActionPolicies.arguments(DirectoryCreation.operation, ["path": "/new", "agent": "codex"]))
    }

    func testConfirmedCanonicalFolderHasNoSessionResult() {
        let settled = SessionActionPolicies.result(action, receipt())
        XCTAssertEqual("completed", settled.status)
        XCTAssertEqual("/canonical/New project ; $ literal", settled.resultPath)
        XCTAssertNil(settled.resultTarget)
        XCTAssertFalse(settled.blocksSending)
    }

    func testWrongReceiptPathParentStatusAndTargetRemainUncertain() {
        var wrong = receipt()
        wrong["request_id"] = "other"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(action, wrong).status)
        let changes: [(String, JSON)] = [("request_id", "other"), ("requested_path", "/different"), ("parent", "/wrong"), ("path", "/canonical/../other"),
                                         ("path", "/canonical/another-leaf"), ("status", "exists")]
        for (field, value) in changes {
            XCTAssertEqual("uncertain", SessionActionPolicies.result(action, receipt([field: value])).status, field)
        }
        var existing = action
        existing.target.session = "codex/project/existing"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(existing, receipt()).status)
        var invalid = action
        invalid.requestId = "request"
        XCTAssertEqual("uncertain", SessionActionPolicies.result(invalid, receipt(id: "request")).status)
    }

    func testInterruptionBlocksOnlyExactComputerAndDoesNotBecomeReplay() throws {
        let interrupted = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(MessageState(sessionActions: [action]))).recovered().sessionActions[0]
        XCTAssertEqual("uncertain", interrupted.status)
        XCTAssertEqual(interrupted, DirectoryCreation.pending([interrupted], machine))
        XCTAssertNil(DirectoryCreation.pending([interrupted], MachineKey(connectionId: "workspace", computerId: "other")))
        XCTAssertNil(DirectoryCreation.pending([interrupted], MachineKey(connectionId: "other", computerId: "computer")))
        var failed = action
        failed.status = "failed"
        XCTAssertNil(DirectoryCreation.pending([failed], machine))
        XCTAssertEqual("failed", SessionActionPolicies.result(interrupted, ["request_id": .string(action.requestId), "state": "failed", "error": "Already exists"]).status)
    }

    func testChildNamesAreSingleLiteralComponents() {
        XCTAssertEqual("/New project", DirectoryCreation.child("/", "New project"))
        XCTAssertEqual("/fixture/New $ project", DirectoryCreation.child("/fixture", "New $ project"))
        for bad in ["", "..", ".", "nested/name", "bad\nname"] { XCTAssertEqual("", DirectoryCreation.child("/fixture", bad)) }
    }
}

/// Ported from Android LaunchProjectTest and LaunchFormTest.
final class LaunchProjectTests: XCTestCase {
    private func account(_ id: String, _ isDefault: Bool = false) -> JSON {
        ["id": .string(id), "provider": "claude", "label": "Same label", "native": false, "installed": true, "is_default": .bool(isDefault), "auth_status": "signed_in"]
    }
    private func catalog() -> JSONObject {
        ["agents": ["claude"], "accounts": [account("profile-one"), account("profile-two", true)], "project_launch_supported": true,
         "swarm_id": "swarm-exact", "default_project": "project-one",
         "projects": [["id": "project-one", "name": "Work", "accessible": true, "folders": [["id": "folder-one", "name": "Source", "path": "/example/source"]]],
                      ["id": "hidden", "name": "Unavailable", "accessible": false]]]
    }

    private func inferenceCatalog() -> JSONObject {
        var raw = catalog(); var projects = raw.objects("projects")
        projects.append(["id": "ungrouped", "name": "Inbox", "folders": []]); raw["projects"] = .array(projects.map(JSON.object))
        return raw
    }
    private func infer(_ raw: JSONObject, directory: String = "/example/source", explicit: Bool = false,
                       worktrees: JSONObject? = nil, browser: JSONObject? = nil) -> String {
        LaunchPresentation.inferredProject(raw, selected: "ungrouped", explicit: explicit, directory: directory,
                                           browser: browser ?? ["path": .string(directory)], worktrees: worktrees)
    }
    func testUntouchedInboxFollowsOnlyExactVerifiedFolderAndExplicitChoiceWins() {
        let raw = inferenceCatalog()
        XCTAssertEqual("project-one", infer(raw)); XCTAssertEqual("ungrouped", infer(raw, explicit: true))
        XCTAssertEqual("ungrouped", infer(raw, directory: "/example/source/nested"))
        XCTAssertEqual("ungrouped", infer(raw, browser: ["path": "/other"]))
        XCTAssertEqual("project-one", infer(raw, directory: "/example/link", browser: ["path": "/example/source", "requested_path": "/example/link"]))
        XCTAssertEqual("project-one", LaunchPresentation.inferredProject(raw, selected: "project-one", explicit: false, directory: "/other", browser: ["path": "/other"], worktrees: nil))
    }
    func testAmbiguousOrOtherComputerFoldersNeverChooseAutomatically() {
        var raw = inferenceCatalog(); var projects = raw.objects("projects")
        projects.append(["id": "other", "name": "Other", "folders": [["id": "other-folder", "path": "/example/source"]]])
        raw["projects"] = .array(projects.map(JSON.object)); XCTAssertEqual("ungrouped", infer(raw))
        projects[projects.count-1]["folders"] = [["id": "other-folder", "path": "/example/source", "local": false]]
        raw["projects"] = .array(projects.map(JSON.object)); XCTAssertEqual("project-one", infer(raw))
        projects[0]["folders"] = [["id": "folder-one", "path": "/example/source", "local": false]]
        raw["projects"] = .array(projects.map(JSON.object)); XCTAssertEqual("ungrouped", infer(raw))
    }
    func testRelatedWorktreeInferenceRequiresCompleteExactAvailableCatalog() {
        let raw = inferenceCatalog()
        let trees: JSONObject = ["state": "ok", "stale": false, "partial": false, "path": "/example/linked", "common_dir": "/example/source/.git",
                                 "worktrees": [["path": "/example/source", "kind": "main", "available": true], ["path": "/example/linked", "kind": "linked", "available": true]]]
        XCTAssertEqual("project-one", infer(raw, directory: "/example/linked", worktrees: trees))
        for field in ["partial", "stale"] { var changed = trees; changed[field] = true; XCTAssertEqual("ungrouped", infer(raw, directory: "/example/linked", worktrees: changed)) }
        var changed = trees; changed["path"] = "/other"; XCTAssertEqual("ungrouped", infer(raw, directory: "/example/linked", worktrees: changed))
        changed = trees; changed["worktrees"] = [["path": "/example/source", "kind": "bare", "available": true], ["path": "/example/linked", "kind": "linked", "available": true]]
        XCTAssertEqual("ungrouped", infer(raw, directory: "/example/linked", worktrees: changed))
    }
    func testDistinctSameLabelProfilesHaveConcreteDefaultAndExactIdentifiers() {
        let accounts = LaunchPresentation.accounts(catalog(), "claude")
        XCTAssertEqual(["profile-one", "profile-two"], accounts.map(\.id))
        XCTAssertEqual("profile-two", LaunchPresentation.defaultAccount(accounts))
        XCTAssertEqual("profile-one", LaunchPresentation.defaultAccount(accounts, selected: "profile-one"))
        XCTAssertEqual(["Same label", "Same label"], accounts.map(LaunchPresentation.accountTitle))
    }

    func testDefaultAbsentOrGoneStillChoosesAnExplicitOfferedProfile() {
        let accounts = LaunchPresentation.accounts(catalog(), "claude").map { account -> LaunchAccount in
            var copy = account
            copy.isDefault = false
            return copy
        }
        XCTAssertEqual("profile-one", LaunchPresentation.defaultAccount(accounts, selected: "removed"))
        XCTAssertEqual("", LaunchPresentation.defaultAccount([]))
        XCTAssertTrue(LaunchPresentation.accounts(catalog(), "codex").isEmpty)
    }

    func testSanitizationPreservesOnlyBoundedLaunchMetadataAndCurrentLocalProjects() {
        var raw = catalog()
        var rows = raw.array("accounts")!
        var first = rows[0].object!
        first["home"] = "/private"
        first["identity"] = ["email": "private@example.test"]
        first["credentials"] = "private-token"
        rows[0] = .object(first)
        raw["accounts"] = .array(rows)
        let sanitized = LaunchPresentation.sanitizeCatalog(raw)
        XCTAssertFalse(JSON.object(sanitized).compact.contains("private"))
        XCTAssertEqual("profile-two", LaunchPresentation.defaultAccount(LaunchPresentation.accounts(sanitized, "claude")))
        XCTAssertEqual(["project-one"], LaunchPresentation.projects(sanitized).map(\.id))
        XCTAssertEqual("/example/source", LaunchPresentation.projects(sanitized).first?.folders.first?.path)
        XCTAssertEqual("project-one", sanitized.string("default_project"))
    }

    func testOnlyUntouchedInboxOmitsAssignmentAndNeverAddsAProtocolField() throws {
        let raw = inferenceCatalog()
        func args(_ project: String = "ungrouped", implicitInbox: Bool = false) throws -> JSONObject {
            try LaunchPresentation.arguments(raw, agent: "claude", directory: "/example/linked", tag: "new-session", accountId: "profile-two",
                                             projectId: project, worktrees: nil, implicitInbox: implicitInbox)
        }
        let implicit = try args(implicitInbox: true)
        XCTAssertEqual(Set(["agent", "directory", "tag", "account_id"]), Set(implicit.keys))
        XCTAssertFalse(implicit.has("implicitInbox")); XCTAssertFalse(implicit.has("implicit_inbox"))
        let explicit = try args()
        XCTAssertEqual("ungrouped", explicit.string("project_id")); XCTAssertEqual("swarm-exact", explicit.string("swarm_id")); XCTAssertEqual(.bool(true), explicit["add_folder"])
        XCTAssertEqual(explicit, try args(implicitInbox: false))
        XCTAssertEqual(try args("project-one"), try args("project-one", implicitInbox: true))
        XCTAssertEqual("project-one", try args("project-one", implicitInbox: true).string("project_id"))
        XCTAssertThrowsError(try LaunchPresentation.arguments(catalog(), agent: "claude", directory: "/example/linked", tag: "new-session", accountId: "profile-two",
                                                            projectId: "ungrouped", worktrees: nil, implicitInbox: true))
    }
    func testProjectFolderIsExactAndOutsideFolderAdditionIsExplicit() throws {
        let raw = catalog()
        let existing = try LaunchPresentation.projectArguments(raw, projectId: "project-one", directory: "/example/source")
        XCTAssertEqual("swarm-exact", existing.string("swarm_id"))
        XCTAssertEqual("folder-one", existing.string("project_folder_id"))
        XCTAssertEqual(.bool(false), existing["add_folder"])
        let outside = try LaunchPresentation.projectArguments(raw, projectId: "project-one", directory: "/example/outside")
        XCTAssertEqual(.bool(true), outside["add_folder"])
        XCTAssertFalse(outside.has("project_folder_id"))
        XCTAssertThrowsError(try LaunchPresentation.projectArguments(raw, projectId: "hidden", directory: "/example/source"))
        var unsupported = raw
        unsupported["project_launch_supported"] = false
        XCTAssertTrue(LaunchPresentation.projects(unsupported).isEmpty)
    }

    func testVerifiedBrowserAcceptsSelectedSymlinkPathWithoutInventingAnotherPath() {
        let browser: JSONObject = ["path": "/example/canonical", "requested_path": "/example/symlink"]
        XCTAssertTrue(LaunchPresentation.selectedFolder(browser, "/example/symlink"))
        XCTAssertFalse(LaunchPresentation.selectedFolder(browser, "/example/other"))
    }

    func testCreatedSessionWithMissingAssignmentRemainsCompletedAndDurablyReviewable() throws {
        let target = Target(computerId: "computer", session: "", run: "", conversation: "", connectionId: "workspace")
        let args: JSONObject = ["agent": "claude", "directory": "/example/source", "tag": "new session", "account_id": "profile-two", "swarm_id": "swarm-exact",
                                "project_id": "project-one", "project_folder_id": "folder-one", "add_folder": false]
        XCTAssertNoThrow(try SessionActionPolicies.arguments("launch", args))
        let action = SessionAction(requestId: "request", target: target, operation: "launch", arguments: args)
        var native: JSONObject = ["request_id": "request", "status": "created",
                                  "result_target": ["name": "claude/source/new session", "run_id": "new-run", "conversation_id": "new-conversation"]]
        func receipt() -> JSONObject { ["request_id": "request", "state": "completed", "result": .object(native)] }
        let completed = SessionActionPolicies.result(action, receipt())
        XCTAssertEqual("completed", completed.status)
        XCTAssertFalse(completed.blocksSending)
        XCTAssertTrue(completed.needsProjectReview)
        XCTAssertEqual("new-run", completed.resultTarget?.run)
        var state = MessageState().action(completed)
        for index in 0..<150 {
            var other = action
            other.requestId = "other-\(index)"
            other.status = "completed"
            other.arguments = [:]
            state = state.action(other)
        }
        let restored = try JSONDecoder().decode(MessageState.self, from: JSONEncoder().encode(state))
        XCTAssertEqual(true, restored.sessionActions.first { $0.requestId == "request" }?.needsProjectReview)
        var reviewedAction = completed
        reviewedAction.status = "reviewed"
        XCTAssertNotEqual(true, state.action(reviewedAction).sessionActions.first { $0.requestId == "request" }?.needsProjectReview)
        native["project_assignment"] = ["status": "assigned", "swarm_id": "wrong", "project_id": "project-one"]
        XCTAssertTrue(SessionActionPolicies.result(action, receipt()).needsProjectReview)
        native["project_assignment"] = ["status": "assigned", "swarm_id": "swarm-exact", "project_id": "project-one"]
        XCTAssertFalse(SessionActionPolicies.result(action, receipt()).needsProjectReview)
    }
}

final class LaunchFormTests: XCTestCase {
    private let projects = [
        LaunchProject(id: "p1", name: "Zerus", color: "#67e8cb", folders: []),
        LaunchProject(id: "p2", name: "Infra", color: "#ffcb7d", folders: [LaunchProjectFolder(id: "f1", path: "/srv/deploy/terraform", name: "Terraform")]),
        LaunchProject(id: "p3", name: "Website", color: "", folders: [LaunchProjectFolder(id: "f2", path: "/example/site", name: "Site"),
                                                                      LaunchProjectFolder(id: "f3", path: "/example/site-docs", name: "Docs")]),
    ]
    private func entry(_ path: String, _ kind: String = "linked", _ available: Bool = true) -> JSON {
        ["path": .string(path), "kind": .string(kind), "available": .bool(available)]
    }
    private func catalog(_ requested: String = "/repo", _ path: String? = nil, _ change: JSONObject = [:]) -> JSONObject {
        var raw: JSONObject = ["state": "ok", "stale": false, "partial": false, "path": .string(path ?? requested), "requested_path": .string(requested),
                               "selected_root": "/repo", "common_dir": "/repo/.git", "worktrees": [entry("/repo", "main"), entry("/repo-feature")]]
        for key in change.keys { raw[key] = change[key] }
        return raw
    }

    func testProjectSearchKeepsCatalogOrderAndMatchesNamesFoldersAndPaths() {
        XCTAssertEqual(["p1", "p2", "p3"], LaunchPresentation.filterProjects(projects, "  ").map(\.id))
        XCTAssertEqual(["p1"], LaunchPresentation.filterProjects(projects, "zer").map(\.id))
        XCTAssertEqual(["p2"], LaunchPresentation.filterProjects(projects, "TERRAFORM").map(\.id))
        XCTAssertEqual(["p2"], LaunchPresentation.filterProjects(projects, "/srv/deploy").map(\.id))
        XCTAssertEqual(["p3"], LaunchPresentation.filterProjects(projects, " docs ").map(\.id))
        XCTAssertEqual(["p1", "p2"], LaunchPresentation.filterProjects(projects, "r").map(\.id))
        XCTAssertTrue(LaunchPresentation.filterProjects(projects, "absent").isEmpty)
    }

    func testProjectSubtitleNamesThisComputersFolders() {
        XCTAssertEqual(["No folder on this computer", "/srv/deploy/terraform", "2 folders"], projects.map(LaunchPresentation.folderSummary))
    }

    func testAutomaticWorktreeNeedsAFreshCompleteCatalogOfTheSelectedCheckout() {
        XCTAssertTrue(WorktreePresentation.automaticAvailable(catalog(), "/repo"))
        XCTAssertTrue(WorktreePresentation.automaticAvailable(catalog("/repo/app"), "/repo/app"))
        XCTAssertTrue(WorktreePresentation.automaticAvailable(catalog("~/repo", "/repo"), "/repo"))
        XCTAssertFalse(WorktreePresentation.automaticAvailable(catalog(), "/other"))
        XCTAssertFalse(WorktreePresentation.automaticAvailable(nil, "/repo"))
        let failing: [JSONObject] = [["state": "not_repo"], ["stale": true], ["partial": true], ["common_dir": ""], ["selected_root": "/"], ["selected_root": "relative"],
                                     ["worktrees": [entry("/repo", "bare")]], ["worktrees": [entry("/repo", "main", false)]]]
        for change in failing { XCTAssertFalse(WorktreePresentation.automaticAvailable(catalog("/repo", nil, change), "/repo"), "\(change)") }
        XCTAssertFalse(WorktreePresentation.automaticAvailable(catalog("/repository", nil, ["worktrees": [entry("/repo", "main")]]), "/repository"))
    }

    func testAutomaticPathIsTheGeneratedSiblingOfTheSelectedRoot() {
        XCTAssertEqual("/repo-work-1a2b3c4d", WorktreePresentation.automaticPath(catalog(), branch: "work-1a2b3c4d"))
        XCTAssertEqual("/srv/app-work-1a2b3c4d", WorktreePresentation.automaticPath(catalog("/repo", nil, ["selected_root": "/srv/app/"]), branch: "work-1a2b3c4d"))
        XCTAssertEqual("", WorktreePresentation.automaticPath(catalog("/repo", nil, ["selected_root": ""]), branch: "work-1a2b3c4d"))
        XCTAssertEqual("", WorktreePresentation.automaticPath(catalog(), branch: ""))
        XCTAssertNotNil(LaunchPresentation.generatedName().range(of: "^work-[0-9a-f]{8}$", options: .regularExpression))
    }

    func testAutomaticNotesExplainTheDirectFolderFallback() {
        XCTAssertEqual("", WorktreePresentation.automaticNote(catalog(), folder: "/repo", supported: true, loading: false))
        XCTAssertEqual("", WorktreePresentation.automaticNote(nil, folder: "", supported: true, loading: false))
        XCTAssertEqual("Checking repository…", WorktreePresentation.automaticNote(nil, folder: "/repo", supported: true, loading: true))
        XCTAssertTrue(WorktreePresentation.automaticNote(catalog(), folder: "/repo", supported: false, loading: false).hasPrefix("Update Zerus"))
        XCTAssertTrue(WorktreePresentation.automaticNote(catalog("/repo", nil, ["state": "not_repo"]), folder: "/repo", supported: true, loading: false).hasPrefix("Not a Git repository"))
        XCTAssertTrue(WorktreePresentation.automaticNote(catalog("/repo", nil, ["stale": true]), folder: "/repo", supported: true, loading: false).hasPrefix("Repository details are unavailable"))
    }

    func testCreatedCheckoutIsRelatedThroughRetainedEvidenceOnlyWhenFreshEvidenceCannotProveIt() throws {
        let project = LaunchProject(id: "p", name: "Repo", color: "", folders: [LaunchProjectFolder(id: "main", path: "/repo", name: "Repo")])
        let source = catalog()
        let created = WorktreePresentation.withCreated(source, path: "/repo-work-1a2b3c4d")
        XCTAssertEqual(3, created.array("worktrees")?.count)
        XCTAssertEqual(3, WorktreePresentation.withCreated(created, path: "/repo-work-1a2b3c4d").array("worktrees")?.count)
        XCTAssertEqual(2, source.array("worktrees")?.count)
        XCTAssertEqual("main", WorktreePresentation.anchor(created, project, "/repo-work-1a2b3c4d")?.id)
        XCTAssertNil(WorktreePresentation.anchor(catalog(), project, "/repo-work-1a2b3c4d"))
        XCTAssertEqual(created, WorktreePresentation.placement(fresh: nil, created: created, project: project, directory: "/repo-work-1a2b3c4d"))
        let fresh = catalog("/repo-work-1a2b3c4d", nil, ["worktrees": [entry("/repo", "main"), entry("/repo-work-1a2b3c4d")]])
        XCTAssertEqual(fresh, WorktreePresentation.placement(fresh: fresh, created: created, project: project, directory: "/repo-work-1a2b3c4d"))
        let unrelated = LaunchProject(id: "u", name: "Other", color: "", folders: [LaunchProjectFolder(id: "x", path: "/elsewhere", name: "Else")])
        XCTAssertEqual(fresh, WorktreePresentation.placement(fresh: fresh, created: created, project: unrelated, directory: "/repo-work-1a2b3c4d"))
        let raw: JSONObject = ["project_launch_supported": true, "worktree_supported": true, "swarm_id": "swarm",
                               "projects": [["id": "p", "name": "Repo", "folders": [["id": "main", "path": "/repo"]]]]]
        let related = try LaunchPresentation.projectArguments(raw, projectId: "p", directory: "/repo-work-1a2b3c4d", worktrees: created)
        XCTAssertEqual(.bool(false), related["add_folder"])
        XCTAssertEqual("main", related.string("worktree_folder_id"))
        XCTAssertEqual("/repo/.git", related.string("worktree_common_dir"))
        XCTAssertEqual(.bool(true), try LaunchPresentation.projectArguments(raw, projectId: "p", directory: "/repo-work-1a2b3c4d", worktrees: nil)["add_folder"])
    }

    private func reason(_ change: (inout LaunchReadiness) -> Void) -> String {
        var readiness = LaunchReadiness()
        change(&readiness)
        return LaunchPresentation.disabledReason(readiness)
    }

    func testDisabledCreateExplainsTheFirstProblem() {
        XCTAssertEqual("", reason { _ in })
        XCTAssertEqual("This session was already created.", reason { $0.confirmed = true; $0.catalogLoading = true })
        XCTAssertEqual("Refresh the computer list.", reason { $0.machineReason = "Refresh the computer list."; $0.folderChosen = false })
        XCTAssertEqual("Loading agents and projects…", reason { $0.catalogLoading = true; $0.catalogAvailable = false })
        XCTAssertEqual("Choose a project.", reason { $0.projectRequired = true; $0.projectChosen = false; $0.folderChosen = false })
        XCTAssertEqual("Browse to choose a folder.", reason { $0.projectHasFolder = false; $0.folderChosen = false })
        XCTAssertEqual("Choose a folder.", reason { $0.folderChosen = false })
        XCTAssertEqual("Checking folder…", reason { $0.folderLoading = true; $0.folderVerified = false })
        XCTAssertEqual("Check this folder again or browse to it.", reason { $0.folderVerified = false })
        XCTAssertEqual("Checking repository…", reason { $0.repositoryLoading = true })
        XCTAssertEqual("No agents are installed on this computer.", reason { $0.providers = 0; $0.providerChosen = false })
        XCTAssertEqual("Choose an account.", reason { $0.accountChosen = false })
        XCTAssertEqual("Enter a session name.", reason { $0.nameError = LaunchPresentation.nameError("") })
        XCTAssertEqual("", reason { $0.worktreeReason = "Check the previous worktree creation receipt."; $0.automaticFailed = true })
        XCTAssertEqual("Check the previous worktree creation receipt.", reason { $0.automatic = true; $0.worktreeReason = "Check the previous worktree creation receipt." })
        XCTAssertTrue(reason { $0.automatic = true; $0.automaticFailed = true }.hasPrefix("Automatic worktree creation failed"))
        XCTAssertTrue(reason { $0.automatic = true; $0.automaticUsed = true }.hasPrefix("This form already created its worktree"))
        XCTAssertEqual("", reason { $0.automaticUsed = true })
    }
}
