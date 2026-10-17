import XCTest
@testable import ZerusKit

/// Ported from Android AccountSnapshotsTest and AccountGroupingTest.
final class AccountsTests: XCTestCase {
    private func account(_ id: String = "profile", provider: String = "codex", usage: JSONObject = [:]) -> JSON {
        ["id": .string(id), "provider": .string(provider), "label": "Same label", "usage": .object(usage)]
    }
    private func computer(_ accounts: [JSON]? = nil, schema: JSON = 1, id: String = "node", checked: Double = 1000) -> JSONObject {
        ["id": .string(id), "name": "Reported machine", "online": true,
         "snapshot": ["mobile_capabilities": ["features": ["accounts_snapshot"]],
                      "mobile_accounts": ["schema": schema, "available": true, "checked_at": .double(checked), "accounts": .array(accounts ?? [account()])]]]
    }
    private func parse(_ usage: JSONObject) -> AccountCatalog { AccountSnapshots.parse("workspace", computer([account(usage: usage)])) }

    func testQuotasRemainUnknownWithoutStrictFiniteNumbers() {
        let rows = parse(["windows": [["used_percent": true, "resets_at": "bad"], ["used_percent": "42"], ["used_percent": -1], ["used_percent": 150]]]).accounts[0].windows
        XCTAssertNil(rows[0].usedPercent); XCTAssertNil(rows[0].resetsAt); XCTAssertNil(rows[1].usedPercent); XCTAssertNil(rows[2].usedPercent)
        XCTAssertEqual(AccountPresentation.percent(rows[3].usedPercent), "150% used")
        XCTAssertEqual(AccountPresentation.percent(nil), "Usage unknown")
        XCTAssertNil(parse(["checked_at": "NaN"]).accounts[0].checkedAt)
        XCTAssertNil(parse(["checked_at": true]).accounts[0].checkedAt)
        XCTAssertNil(AccountPresentation.date(9_007_199_254_740_991))
    }

    func testCatalogTimeDoesNotFabricateUsageFreshness() {
        let catalog = parse(["status": "ok", "windows": [["label": "Weekly", "used_percent": 91, "window_minutes": 10080, "resets_at": "1970-01-01T00:10:00Z"]]])
        let item = catalog.accounts[0]
        XCTAssertNil(item.checkedAt)
        XCTAssertTrue(AccountPresentation.stale(catalog, item, now: 1000))
        XCTAssertTrue(AccountPresentation.ended(item.windows[0], now: 1000))
        XCTAssertEqual(AccountPresentation.percent(item.windows[0].usedPercent), "91% used")
        XCTAssertEqual(AccountPresentation.period(item.windows[0]), "Weekly 7d")
        let future = parse(["checked_at": 1061]); XCTAssertTrue(AccountPresentation.stale(future, future.accounts[0], now: 1000))
        let fresh = parse(["checked_at": 999]); XCTAssertFalse(AccountPresentation.stale(fresh, fresh.accounts[0], now: 1000))
    }

    func testDecimalMoneyAndAllowlistedFields() throws {
        let usage: JSONObject = ["status": "ok", "auth_status": "signed_in", "checked_at": 990,
                                 "identity": ["name": "Synthetic person", "plan": "Pro", "api_key": "NEVER_RETAIN"],
                                 "credits": ["balance": "-12.3400"], "balances": [["kind": "wallet", "balance": "1000000000000000000.123456789", "currency": "USD"]],
                                 "source": "https://private.example/path"]
        let catalog = parse(usage)
        let encoded = String(decoding: try JSONEncoder().encode([catalog]), as: UTF8.self)
        XCTAssertFalse(encoded.contains("NEVER_RETAIN"))
        XCTAssertFalse(encoded.contains("private.example"))
        let restored = AccountSnapshots.restored(try JSONDecoder().decode([AccountCatalog].self, from: Data(encoded.utf8)))[0]
        let item = restored.accounts[0]
        XCTAssertEqual(item.creditBalance, "-12.34")
        XCTAssertEqual(item.balances[0].balance, "1000000000000000000.123456789")
        XCTAssertEqual(item.authStatus, "signed_in")
        XCTAssertTrue(restored.stale)
        XCTAssertFalse(restored.online)
        let invalid = parse(["credits": ["balance": "NaN"], "balances": [["balance": 10]]]).accounts[0]
        XCTAssertNil(invalid.creditBalance); XCTAssertNil(invalid.balances[0].balance)
        XCTAssertEqual(parse(["balances": [["balance": "1.25", "currency": "https://x"]]]).accounts[0].balances[0].currency, "")
    }

    func testMalformedDuplicateAndFutureSchemasNeverClaimVerifiedEmpty() {
        XCTAssertFalse(AccountSnapshots.parse("w", computer(schema: 2)).supported)
        XCTAssertFalse(AccountSnapshots.parse("w", computer(schema: true)).supported)
        XCTAssertFalse(AccountSnapshots.parse("w", computer([["provider": "codex"]])).available)
        XCTAssertFalse(AccountSnapshots.parse("w", computer([account(), account()])).available)
        let empty = AccountSnapshots.parse("w", computer([]))
        XCTAssertTrue(empty.available); XCTAssertTrue(empty.accounts.isEmpty)
        XCTAssertFalse(AccountSnapshots.parse("w", ["id": "node"]).supported)
    }

    func testMergeKeepsIdentitiesAndRemovals() {
        let a = AccountSnapshots.parse("a", computer([account("one"), account("two"), account("one", provider: "claude")]))
        let rows = AccountSnapshots.merge([], [a, AccountSnapshots.parse("b", computer()), AccountSnapshots.parse("a", computer(id: "other"))],
                                         successful: ["a", "b"], connected: ["a", "b"])
        XCTAssertEqual(rows.count, 3); XCTAssertEqual(rows[0].accounts.count, 3)
        let old = AccountSnapshots.parse("w", computer(checked: 1000)), fresh = AccountSnapshots.parse("w", computer([], checked: 2000))
        let removed = AccountSnapshots.merge([old], [fresh], successful: ["w"], connected: ["w"])
        XCTAssertTrue(removed[0].accounts.isEmpty)
        XCTAssertTrue(AccountSnapshots.merge(removed, [old], successful: ["w"], connected: ["w"])[0].accounts.isEmpty)
        XCTAssertTrue(AccountSnapshots.merge([old], [], successful: [], connected: []).isEmpty)
        let offline = AccountSnapshots.merge([old], [], successful: [], connected: ["w"])[0]
        XCTAssertFalse(offline.online); XCTAssertTrue(offline.stale); XCTAssertEqual(offline.accounts.count, 1)
    }

    func testPresentationFormats() {
        XCTAssertEqual(AccountPresentation.percent(42.8571428571429), "42.9% used")
        func window(_ at: Double?) -> AccountWindow { AccountWindow(id: "window", label: "Session", usedPercent: 42, minutes: 300, resetsAt: at) }
        XCTAssertEqual(AccountPresentation.remaining(window(1000.001), now: 1000), "1m")
        XCTAssertEqual(AccountPresentation.remaining(window(1060), now: 1000), "1m")
        XCTAssertEqual(AccountPresentation.remaining(window(4600.001), now: 1000), "1h 1m")
        XCTAssertEqual(AccountPresentation.remaining(window(8800), now: 1000), "2h 10m")
        XCTAssertEqual(AccountPresentation.remaining(window(184_600), now: 1000), "2d 3h")
        XCTAssertEqual(AccountPresentation.remaining(window(1000), now: 1000), "Ended")
        XCTAssertEqual(AccountPresentation.remaining(window(nil), now: 1000), "Unknown")
        XCTAssertEqual(AccountPresentation.remaining(window(.nan), now: 1000), "Unknown")
        func period(_ minutes: Double?, _ label: String = "Session") -> AccountWindow { AccountWindow(id: "w", label: label, usedPercent: 0, minutes: minutes, resetsAt: nil) }
        XCTAssertEqual(AccountPresentation.shortPeriod(period(300)), "5h")
        XCTAssertEqual(AccountPresentation.shortPeriod(period(10080)), "7d")
        XCTAssertEqual(AccountPresentation.shortPeriod(period(300.5)), "Session")
        XCTAssertEqual(AccountPresentation.shortPeriod(period(nil, String(repeating: "😀", count: 20))), String(repeating: "😀", count: 12) + "…")
        XCTAssertEqual(AccountPresentation.period(period(300)), "Session 5h")
        var ended = period(300); ended.resetsAt = 1000
        XCTAssertEqual(AccountPresentation.resetSummary(ended, now: 1000), "Window ended")
        let status = parse(["status": "ok", "auth_status": "expired", "checked_at": 1000, "identity": ["auth_method": "API key"]])
        var item = status.accounts[0]; item.installed = true
        XCTAssertEqual(AccountPresentation.type(item), "API key")
        var offline = status; offline.online = false
        XCTAssertTrue(AccountPresentation.compactStatus(offline, item).contains("Sign-in expired"))
        XCTAssertTrue(AccountPresentation.compactStatus(offline, item).contains("Offline"))
        item.authStatus = "signed_in"; item.status = "offline"
        XCTAssertEqual(AccountPresentation.compactStatus(status, item), "Provider offline")
    }

    // MARK: Grouping

    private let fold: (String) -> String = { $0.lowercased() }
    private func reported(_ profile: String = "native", id: String = "identity", email: String = "one@example.com", scope: String = " Team ",
                          provider: String = "codex", status: String = "ok", checked: Double? = 10, label: String = "Default account") -> ReportedAccount {
        ReportedAccount(id: profile, provider: provider, label: label, native: true, installed: true, isDefault: false, status: status, authStatus: "signed_in",
                        identity: AccountIdentity(email: email, organization: scope, accountId: id),
                        windows: [AccountWindow(id: "hour", label: "Session", usedPercent: 42, minutes: 300, resetsAt: 1000)],
                        creditBalance: nil, unlimited: nil, balances: [], checkedAt: checked, identityCached: false, refreshing: false, refreshError: false, stale: false)
    }
    private func catalog(_ machine: String = "a", _ accounts: [ReportedAccount]? = nil, workspace: String = "w", online: Bool = true) -> AccountCatalog {
        AccountCatalog(key: MachineKey(connectionId: workspace, computerId: machine), machineName: "Same machine label", supported: true, available: true,
                       stale: !online, online: online, observedAt: 20, accounts: accounts ?? [reported()])
    }
    private func group(_ catalogs: AccountCatalog...) -> [GroupedAccount] { AccountGrouping.group(catalogs, fold: fold) }

    func testReportedIdentityJoinsProfiles() {
        let joined = group(catalog("a", [reported("one", label: "Personal")]), catalog("b", [reported("two", email: "other@example.com", scope: "team")]))
        XCTAssertEqual(joined.count, 1)
        XCTAssertEqual(joined[0].members.count, 2); XCTAssertEqual(joined[0].label, "Personal")
        XCTAssertEqual(group(catalog("a", [reported(id: "first"), reported("other", id: "second")])).count, 2)
    }

    func testEmailAliasJoinsOnlyOneDistinctId() {
        let a = catalog("a", [reported(email: " ONE@example.com ")])
        let b = catalog("b", [reported("email-only", id: "", email: "one@example.COM", scope: "team")])
        XCTAssertEqual(group(a, b).first?.members.count, 2)
        XCTAssertEqual(group(a, b, catalog("c", [reported("repeat")])).first?.members.count, 3)
        XCTAssertEqual(group(a, b, catalog("c", [reported("other", id: "different")])).count, 3)
        XCTAssertEqual(group(catalog(), catalog("b", [reported(provider: "claude")]), catalog("c", [reported(scope: "other")]), catalog("a", workspace: "other-workspace")).count, 4)
    }

    func testSignedOutAndUnidentifiedRemainExactProfiles() {
        let signed = reported(status: "signed_out"), unknown = reported("unknown", id: "", email: "", label: "Personal")
        let groups = group(catalog("a", [signed, unknown]), catalog("b", [signed, unknown]))
        XCTAssertEqual(groups.count, 4)
        XCTAssertTrue(groups.allSatisfy { $0.key.kind == "profile" })
    }

    func testSnapshotQualityWinsBeforeTime() {
        let online = catalog("a", [reported(checked: 10)])
        let offline = catalog("b", [reported(checked: 100)], online: false)
        let failed = catalog("c", [reported(status: "error", checked: 1000)])
        let joined = group(offline, failed, online)[0]
        XCTAssertEqual(joined.snapshot.catalog.key.computerId, "a"); XCTAssertEqual(joined.members.count, 3)
        XCTAssertEqual(group(failed, offline)[0].snapshot.catalog.key.computerId, "b")
        XCTAssertEqual(group(online, catalog("d", [reported(checked: 11)]))[0].snapshot.catalog.key.computerId, "d")
        XCTAssertEqual(group(online, catalog("d", [reported(checked: 10)]))[0].snapshot.catalog.key.computerId, "a")
    }

    func testBuiltinPreviewKeepsAmbiguousProfilesSeparate() {
        let groups = AccountGrouping.group(AccountSnapshots.demo(now: 1000), fold: fold)
        XCTAssertEqual(groups.count, 5)
        let personal = groups.first { $0.snapshot.account.identity.accountId == "example-personal" }!
        XCTAssertEqual(personal.members.count, 3)
        XCTAssertTrue(personal.snapshot.catalog.online)
        XCTAssertTrue(personal.members.contains { !$0.catalog.online && $0.account.refreshError })
        XCTAssertEqual(groups.filter { $0.snapshot.account.identity.email == "shared@example.com" }.count, 3)
    }
}
