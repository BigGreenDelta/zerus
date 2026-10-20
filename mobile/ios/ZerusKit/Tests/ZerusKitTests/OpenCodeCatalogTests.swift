import XCTest
@testable import ZerusKit

/// OpenCode providers phase 1 (opencode-provider-catalog.json), shared with hgs, the connector, the desktop and Android.
final class OpenCodeCatalogTests: XCTestCase {
    func testCapsKeepTheFirstDistinctValidModelsAndProviders() {
        var rows: [JSON] = [["value": "invalid"], ["value": "p/m0", "label": "First 0"], ["value": "p/m0", "label": "Replacement"]]
        rows += (0..<300).map { .object(["value": .string("p/m\($0)"), "label": .string("First \($0)")]) }
        rows.append(["value": "p/m0", "label": "Replacement"])
        let models = OpenCodeCatalog.models(rows)
        XCTAssertEqual(models.count, 256); XCTAssertEqual(models.last?.value, "p/m255"); XCTAssertEqual(models.first?.label, "First 0")
        let providers: [JSON] = [["id": "bad id"]] + (0..<70).map { .object(["id": .string("p\($0)")]) }
        XCTAssertEqual(OpenCodeCatalog.providers(providers).count, 64)
        XCTAssertEqual(OpenCodeCatalog.providers(providers, limit: 16).count, 16)
        XCTAssertTrue(OpenCodeCatalog.providers(providers, limit: -1).isEmpty)
    }

    func testTypedMetadataRejectsCoercionAndRetainsNativeEffortMembership() throws {
        let row: JSON = ["value": "p/model", "provider": "bad id", "provider_label": "\u{0000}", "label": true,
                         "free": 1, "favorite": "true", "default": false,
                         "effort_options": ["native-fast", "bad value", "native-fast", "other"],
                         "effort_labels": ["Native fast", "Bad", "Duplicate", "Other"]]
        let model = try XCTUnwrap(OpenCodeCatalog.models([row]).first)
        XCTAssertEqual(model.label, "p/model"); XCTAssertEqual(model.provider, ""); XCTAssertEqual(model.providerLabel, "")
        XCTAssertTrue(OpenCodeCatalog.marks(model).isEmpty); XCTAssertEqual(model.efforts.map(\.value), ["native-fast", "other"])
        let provider = try XCTUnwrap(OpenCodeCatalog.providers([["id": "p", "credential_count": "2", "model_count": 1.5,
                                                                "active_label": .string(String(repeating: "x", count: 81))]]).first)
        XCTAssertNil(provider.credentials); XCTAssertNil(provider.models); XCTAssertEqual(provider.activeLabel, "")
        let account = try XCTUnwrap(OpenCodeCatalog.account(["provider": "opencode", "catalog_at": true,
                                                            "providers_truncated": "true", "model_options_truncated": 1]))
        XCTAssertNil(account.catalogAt); XCTAssertFalse(account.providersTruncated); XCTAssertFalse(account.modelsTruncated)
    }

    func testSessionMarksAndTruncationChangeSignatureAndKeepNativeVariantMembership() throws {
        var option: JSONObject = ["value": "p/model", "provider": "p", "favorite": false, "effort_options": ["native-fast", "high"]]
        var raw: JSONObject = ["agent": "opencode", "model_options": .array([.object(option)])]
        let original = SessionDetailsPresentation.settings(raw)
        XCTAssertEqual(original.models.map(\.id), ["p/model"])
        XCTAssertEqual(original.effortOptions("p/model", effortFeature: false).map(\.value), ["", "high"])
        XCTAssertEqual(original.effortOptions("p/model", effortFeature: true).map(\.value), ["", "native-fast", "high"])
        XCTAssertEqual(original.keptEffort("not-offered", model: "p/model", effortFeature: true), "")
        option["favorite"] = true; raw["model_options"] = .array([.object(option)])
        XCTAssertNotEqual(original.signature, SessionDetailsPresentation.settings(raw).signature)
        option["favorite"] = false; raw["model_options"] = .array([.object(option)]); raw["model_options_truncated"] = true
        XCTAssertNotEqual(original.signature, SessionDetailsPresentation.settings(raw).signature)
        raw["agent"] = "codex"; raw["model_options"] = [["id": "codex-model", "label": "Codex"]]
        let plain = SessionDetailsPresentation.settings(raw)
        XCTAssertEqual(plain.models.map(\.id), ["codex-model"]); XCTAssertEqual(plain.models.first?.provider, ""); XCTAssertFalse(plain.modelsTruncated)
    }

    func testSessionModelSheetReadsMarksAndTruncation() throws {
        let contract = try contract()
        let raw: JSONObject = ["settings_model": "fixture/local", "settings_change_supported": true, "model_options_truncated": true,
                               "model_options": .array(try XCTUnwrap(contract.object("row")?.array("model_options")))]
        let settings = SessionDetailsPresentation.settings(raw)
        XCTAssertTrue(settings.modelsTruncated)
        XCTAssertEqual(settings.models.first { $0.id == "groq/groq/compound" }?.providerLabel, "Groq")
        XCTAssertEqual(settings.models.first { $0.id == "groq/groq/compound" }?.free, true)
        XCTAssertEqual(settings.models.first { $0.id == "keyed/paid-model" }?.favorite, true)
    }
    private func contract() throws -> JSONObject { try XCTUnwrap(SharedFixture.json("opencode-provider-catalog.json").object) }

    func testProviderCatalogFixtureIsSharedByteForByte() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../tests/fixtures/opencode-provider-catalog.json").standardized
        XCTAssertEqual(try Data(contentsOf: SharedFixture.url("opencode-provider-catalog.json")), try Data(contentsOf: root))
    }

    func testHeartbeatAccountParsesProvidersAndState() throws {
        let catalog = try XCTUnwrap(AccountSnapshots.account(try XCTUnwrap(contract().object("heartbeat_account")))?.openCode)
        XCTAssertEqual(catalog.providers.map(\.kind), ["free", "env", "config", "local", "key", "oauth"])
        XCTAssertEqual(catalog.providers.first { $0.id == "keyed" }?.credentials, 2)
        XCTAssertEqual(catalog.providers.last?.activeLabel, "Work")
        XCTAssertEqual(catalog.state, "ready"); XCTAssertEqual(catalog.catalogAt, 1792267200.5)
        XCTAssertFalse(catalog.providersTruncated); XCTAssertFalse(catalog.modelsTruncated)
    }

    func testLaunchShortListKeepsOrderMarksAndGroups() throws {
        let models = OpenCodeCatalog.models(try XCTUnwrap(contract().object("launch_account")?.array("model_options")))
        XCTAssertEqual(models.map(\.value), ["fixture/local", "keyed/paid-model", "groq/openai/gpt-oss-20b", "fixture/alternate",
                                             "ollama/qwen3-coder:30b", "opencode/glm-5.3-flash-free", "opencode/big-pickle", "groq/groq/compound"])
        XCTAssertTrue(models[0].isDefault)
        XCTAssertEqual(models.filter(\.favorite).map(\.value), ["keyed/paid-model", "groq/openai/gpt-oss-20b"])
        let groups = OpenCodeCatalog.group(models)
        XCTAssertEqual(groups.map(\.label), ["Private keyless fixture", "Keyed fixture", "Groq", "Ollama", "OpenCode"])
        XCTAssertEqual(groups[2].models.map(\.value), ["groq/openai/gpt-oss-20b", "groq/groq/compound"])
        XCTAssertEqual(OpenCodeCatalog.marks(models[0]), ["Default"]); XCTAssertEqual(OpenCodeCatalog.marks(models[5]), ["Free"])
    }

    func testSearchMatchesEveryWord() throws {
        let models = OpenCodeCatalog.models(try XCTUnwrap(contract().object("launch_account")?.array("model_options")))
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "gpt 20B").map(\.value), ["groq/openai/gpt-oss-20b"])
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "keyless alternate").map(\.value), ["fixture/alternate"])
        XCTAssertEqual(OpenCodeCatalog.filter(models, query: "  ").count, 8)
    }

    func testStoppedAccountExplainsStateAndTruncation() throws {
        let catalog = try XCTUnwrap(AccountSnapshots.account(try XCTUnwrap(contract().object("stopped_account")))?.openCode)
        XCTAssertEqual(OpenCodeCatalog.stateMessage(state: catalog.state, reason: catalog.reason), "OpenCode is not running on fixture")
        XCTAssertEqual(OpenCodeCatalog.stateMessage(state: "not_running", reason: ""), "OpenCode is not running on this computer.")
        XCTAssertEqual(OpenCodeCatalog.truncationMessage(models: true, providers: true),
                       "Showing a shortened list. Choose other models on the desktop. Some providers are not shown.")
    }

    func testProvidersSurviveTheCodableCacheAndOldCachesDecode() throws {
        var raw = try XCTUnwrap(contract().object("heartbeat_account"))
        raw["providers"] = .array((0..<64).map { .object(["id": .string("p\($0)"), "label": .string("P\($0)"), "kind": "env",
                                                          "credential_count": .int(0), "model_count": .int(1)]) })
        let account = try XCTUnwrap(AccountSnapshots.account(raw))
        XCTAssertEqual(account.openCode?.providers.count, 64)
        let restored = try JSONDecoder().decode(ReportedAccount.self, from: JSONEncoder().encode(account))
        XCTAssertEqual(restored.openCode, account.openCode)
        // A cache written before this field existed still decodes.
        var stored = try XCTUnwrap(JSONSerialization.jsonObject(with: JSONEncoder().encode(account)) as? [String: Any])
        stored.removeValue(forKey: "openCode")
        let legacy = try JSONSerialization.data(withJSONObject: stored)
        XCTAssertNil(try JSONDecoder().decode(ReportedAccount.self, from: legacy).openCode)
    }
}
