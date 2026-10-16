import XCTest
@testable import ZerusKit

/// Ported from Android LaunchPresentationTest and DirectoryCreationTest.
final class LaunchPresentationTests: XCTestCase {
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
