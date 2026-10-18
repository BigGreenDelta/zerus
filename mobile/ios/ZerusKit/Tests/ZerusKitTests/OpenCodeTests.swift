import XCTest
@testable import ZerusKit

/// OpenCode 2 phase 1 on phones: launch effort, model-change effort, queued delivery
/// labels and the shared form contract (tests/fixtures/opencode-form-contract.json).
final class OpenCodeTests: XCTestCase {
    private let target = Target(computerId: "computer", session: "opencode/project/work", run: "run", conversation: "ses_A", connectionId: "workspace")
    private let catalog: JSONObject = [
        "agents": ["opencode"],
        "accounts": [["id": "native-opencode", "provider": "opencode", "label": "OpenCode", "native": true, "model_options": [
            ["id": "fixture/local", "value": "fixture/local", "label": "Local", "provider": "fixture", "free": true,
             "effort_options": ["low", "high"], "effort_labels": ["Low", "High"]],
            ["id": "fixture/plain", "value": "fixture/plain", "label": "Plain", "effort_options": [], "effort_labels": []],
        ]]],
    ]

    // MARK: Launch

    func testLaunchOffersEffortOnlyForTheChosenModelWhenRelayAndConnectorAcceptIt() {
        let sanitized = LaunchPresentation.sanitizeCatalog(catalog)
        func efforts(_ model: String, supported: Bool = true, provider: String = "opencode") -> [EffortOption] {
            LaunchPresentation.efforts(sanitized, provider: provider, accountId: "native-opencode", model: model, supported: supported)
        }
        XCTAssertEqual(LaunchPresentation.effortFeature, "opencode_launch_effort")
        // Both the relay and this computer's connector must accept launch effort.
        let feature: Set<String> = ["inspect_after", "opencode_launch_effort"]
        XCTAssertTrue(LaunchPresentation.effortSupported(relay: feature, computer: feature))
        XCTAssertFalse(LaunchPresentation.effortSupported(relay: ["inspect_after"], computer: feature), "an older relay refuses launch effort")
        XCTAssertFalse(LaunchPresentation.effortSupported(relay: nil, computer: feature))
        XCTAssertFalse(LaunchPresentation.effortSupported(relay: feature, computer: ["inspect_after"]), "an older connector refuses launch effort")
        XCTAssertFalse(LaunchPresentation.effortSupported(relay: feature, computer: nil))
        XCTAssertEqual(efforts("fixture/local").map(\.value), ["low", "high"])
        XCTAssertEqual(efforts("fixture/local").map(\.label), ["Low", "High"])
        XCTAssertEqual(efforts("fixture/local", supported: false), [], "an older connector never sees an effort row")
        XCTAssertEqual(efforts("fixture/plain"), [], "a model without variants has no effort row")
        XCTAssertEqual(efforts(""), [], "the native default model has no effort row")
        XCTAssertEqual(efforts("fixture/local", provider: "codex"), [])
        // A catalog without variants is cached exactly as before.
        var plain = catalog
        plain["accounts"] = [["id": "native-opencode", "provider": "opencode", "label": "OpenCode", "model_options": [["id": "fixture/plain", "label": "Plain"]]]]
        XCTAssertFalse(JSON.object(LaunchPresentation.sanitizeCatalog(plain)).compact.contains("effort"))
    }

    func testLaunchRequestCarriesEffortOnlyWhenOfferedAndAccepted() throws {
        let sanitized = LaunchPresentation.sanitizeCatalog(catalog)
        func args(_ model: String, _ effort: String, supported: Bool) throws -> JSONObject {
            try LaunchPresentation.arguments(sanitized, agent: "opencode", directory: "/example/project", tag: "work", accountId: "native-opencode",
                                             projectId: "", worktrees: nil, model: model, effort: effort, effortSupported: supported)
        }
        let chosen = try args("fixture/local", "high", supported: true)
        XCTAssertEqual(chosen["effort"], "high")
        XCTAssertEqual(chosen["model"], "fixture/local")
        XCTAssertFalse(try args("fixture/local", "", supported: true).has("effort"), "the model default sends no effort")
        let old: JSONObject = ["agent": "opencode", "directory": "/example/project", "tag": "work", "account_id": "native-opencode", "model": "fixture/local"]
        XCTAssertEqual(try args("fixture/local", "", supported: false), old, "an older connector receives exactly today's request")
        XCTAssertThrowsError(try args("fixture/local", "high", supported: false))
        XCTAssertThrowsError(try args("fixture/local", "ultra", supported: true))
        XCTAssertThrowsError(try args("fixture/plain", "high", supported: true))
        XCTAssertThrowsError(try args("", "high", supported: true))
        // The request guard keeps effort to an OpenCode model launch with a native variant token.
        let base: JSONObject = ["agent": "opencode", "directory": "/example/project", "tag": "work", "account_id": "native-opencode"]
        XCTAssertNoThrow(try SessionActionPolicies.arguments("launch", base.putting("model", "fixture/local").putting("effort", "high")))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("launch", base.putting("effort", "high")))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("launch", base.putting("model", "fixture/local").putting("effort", "two words")))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("launch", ["agent": "codex", "directory": "/example/project", "tag": "work", "effort": "high"]))
    }

    // MARK: Model change

    func testModelChangeOffersThatModelsVariantsInsteadOfTheFixedList() {
        let raw: JSONObject = ["agent": "opencode", "model": "local", "settings_model": "fixture/local", "effort": nil, "settings_change_supported": true,
                               "model_options": [
                                   ["id": "fixture/local", "value": "fixture/local", "label": "Local", "effort_options": ["low", "thinking"],
                                    "effort_labels": ["Low", "Deep thinking"]],
                                   ["id": "fixture/plain", "value": "fixture/plain", "label": "Plain", "effort_options": [], "effort_labels": []],
                               ]]
        let settings = SessionDetailsPresentation.settings(raw)
        XCTAssertEqual(settings.model, "fixture/local")
        XCTAssertEqual(settings.effort, "")
        XCTAssertEqual(settings.effortOptions("fixture/local", effortFeature: true).map(\.value), ["", "low", "thinking"])
        XCTAssertEqual(settings.effortOptions("fixture/local", effortFeature: true).map(\.label), ["Default", "Low", "Deep thinking"])
        XCTAssertEqual(settings.effortOptions("fixture/plain", effortFeature: true).map(\.value), [""])
        // Without the relay and connector feature only the relay's fixed levels can be requested.
        XCTAssertEqual(settings.effortOptions("fixture/local", effortFeature: false).map(\.value), ["", "low"])
        XCTAssertEqual(settings.effortOptions("fixture/local", effortFeature: false).map(\.label), ["Default", "Low"])
        // Unlabeled native efforts keep their existing names.
        let codex = SessionDetailsPresentation.settings(["model": "gpt", "model_options": [["id": "gpt", "label": "GPT", "effort_options": ["medium", "xhigh"]]]])
        XCTAssertEqual(codex.effortOptions("gpt", effortFeature: false).map(\.label), ["Default", "Medium", "Extra high"])
        XCTAssertEqual(codex.effortOptions("gpt", effortFeature: true), codex.effortOptions("gpt", effortFeature: false))
        // Other providers keep the relay's fixed levels even when the feature is advertised.
        let other = SessionDetailsPresentation.settings(["agent": "dsh", "model": "x/y", "model_options": [["id": "x/y", "effort_options": ["high", "default"]]]])
        XCTAssertEqual(other.effortOptions("x/y", effortFeature: true).map(\.value), ["", "high"])
        XCTAssertEqual(settings.effortLabel("thinking", model: "fixture/local"), "Deep thinking")
        XCTAssertNil(codex.effortLabel("medium", model: "gpt"), "unlabeled efforts keep today's text")
        // Switching model keeps the effort only when the new model offers it through this relay and computer.
        XCTAssertEqual(settings.keptEffort("thinking", model: "fixture/local", effortFeature: true), "thinking")
        XCTAssertEqual(settings.keptEffort("thinking", model: "fixture/local", effortFeature: false), "", "an older relay resets a native variant")
        XCTAssertEqual(settings.keptEffort("low", model: "fixture/plain", effortFeature: true), "")
        XCTAssertEqual(settings.keptEffort("low", model: "fixture/local", effortFeature: false), "low")
        // Variant lists are bounded like the launch catalog.
        let many = SessionDetailsPresentation.settings(["agent": "opencode", "model_options": [["id": "m/x", "effort_options": .array((0..<20).map { .string("v\($0)") })]]])
        XCTAssertEqual(many.effortOptions("m/x", effortFeature: true).count, 17)
        // The fresh inspection decides whether a variant is offered for the chosen model.
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/local", "effort": "thinking"], raw), "")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/local", "effort": "high"], raw),
                       "The selected effort is unavailable for this model.")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/plain", "effort": "low"], raw),
                       "The selected effort is unavailable for this model.")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/plain", "effort": ""], raw), "")
    }

    func testSettingsEffortVariantsNeedOpenCodeAndBothFeatures() {
        let feature: Set<String> = ["inspect_after", "opencode_launch_effort"]
        let open = SessionActionPolicies.effortVariants(agent: "opencode", relay: feature, computer: feature)
        XCTAssertTrue(open)
        // OpenCode with an updated relay and connector: the native variant is a valid request; inspection decides whether it is offered.
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "thinking"], effortVariants: open))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "deep thinking"], effortVariants: open))
        // OpenCode behind an older relay or connector: refused here with a clear reason.
        let older: Set<String> = ["inspect_after"]
        let hops: [(Set<String>?, Set<String>?)] = [(older, feature), (feature, older), (nil, feature)]
        for (relay, computer) in hops {
            let variants = SessionActionPolicies.effortVariants(agent: "opencode", relay: relay, computer: computer)
            XCTAssertFalse(variants)
            XCTAssertThrowsError(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "thinking"], effortVariants: variants)) { error in
                XCTAssertEqual((error as? ZerusError)?.message, SessionActionPolicies.effortUpdateRequired)
            }
        }
        // Other agents keep the relay's fixed levels even when the feature is advertised.
        let codex = SessionActionPolicies.effortVariants(agent: "codex", relay: feature, computer: feature)
        XCTAssertFalse(codex)
        XCTAssertThrowsError(try SessionActionPolicies.arguments("settings", ["model": "gpt", "effort": "turbo"], effortVariants: codex))
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", ["model": "gpt", "effort": "high"], effortVariants: codex))
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "low"]))
    }

    // MARK: Queued messages

    private func prompt(_ text: String, _ id: String, _ at: Double, _ state: String? = nil) -> JSON {
        var row: JSONObject = ["type": "UserPromptSubmit", "detail": .string(text), "source": "opencode_native", "agent_id": "", "message_id": .string(id),
                               "at": .double(at)]
        if let state { row["delivery_state"] = .string(state) }
        return .object(row)
    }

    func testQueuedMessagesShowTheirDeliveryStateAndPhonesOfferNoRemove() throws {
        let events = NativeEvents.events(["events": [prompt("hello", "msg_1", 1), prompt("queued later", "msg_2", 2, "queued"),
                                                     prompt("after stop", "msg_3", 3, "not_delivered"), prompt("odd", "msg_4", 4, "removed")]])
        XCTAssertEqual(events.map(\.text), ["hello", "queued later", "after stop", "odd"])
        XCTAssertEqual(events.map(\.deliveryState), [nil, "queued", "not_delivered", nil])
        XCTAssertEqual(events.map { QueuedDelivery.label($0.deliveryState) }, ["", "Queued", "Not delivered", ""])
        XCTAssertEqual(try JSONDecoder().decode([Event].self, from: JSONEncoder().encode(events)), events)
        // Cached rows written before this field existed still decode.
        let legacy = try JSONEncoder().encode([events[0]])
        XCTAssertFalse(String(decoding: legacy, as: UTF8.self).contains("deliveryState"))
        XCTAssertEqual(try JSONDecoder().decode([Event].self, from: legacy), [events[0]])
        // Phase 1 phones never remove queued messages.
        XCTAssertFalse(SessionActionPolicies.operations.contains("remove_queued"))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("remove_queued", ["message_id": "msg_3"]))
    }

    func testQueuedRowsAreALiveProjectionThatHistoryNeverRetains() {
        let pending = NativeEvents.events(["events": [prompt("hello", "msg_1", 1), prompt("queued later", "msg_2", 2, "queued"),
                                                      prompt("removed elsewhere", "msg_3", 3, "not_delivered")]])
        let delivered = NativeEvents.events(["events": [prompt("hello", "msg_1", 1), prompt("queued later", "msg_2", 2)]])
        let merged = RollingConversationHistory.merge(target: target, previousTarget: target, previous: pending, incoming: delivered, inspection: true)
        XCTAssertEqual(merged.events.map(\.text), ["hello", "queued later"])
        XCTAssertEqual(merged.events.map(\.deliveryState), [nil, nil])
        // History pages are not inspections; they keep what is shown.
        XCTAssertEqual(RollingConversationHistory.merge(target: target, previousTarget: target, previous: pending, incoming: []).events.count, 3)

        // A canonical head window shows the inspection's pending rows after its history and drops them once gone.
        let native = NativeEvents.events(["events": [prompt("hello", "msg_1", 1)]])[0]
        var canonical = native
        canonical.id = "history:h1"
        canonical.historyId = "h1"
        canonical.historyCursor = "c1"
        canonical.historyEpoch = "epoch"
        let window = HistoryWindow(target: target, epoch: "epoch", events: [canonical], before: nil, after: "c1", hasBefore: false, hasAfter: false, atHead: true)
        let shown = window.inspection(pending)
        XCTAssertEqual(shown.events.map(\.text), ["hello", "queued later", "removed elsewhere"])
        XCTAssertEqual(shown.events.map(\.deliveryState), [nil, "queued", "not_delivered"])
        XCTAssertFalse(HistoryInspection.changed(window, pending, []), "queued rows are not new canonical history")
        let later = shown.inspection(NativeEvents.events(["events": [prompt("hello", "msg_1", 1), prompt("queued later", "msg_2", 2, "not_delivered")]]))
        XCTAssertEqual(later.events.map(\.text), ["hello", "queued later"])
        XCTAssertEqual(later.events.map(\.deliveryState), [nil, "not_delivered"])
        XCTAssertEqual(later.inspection([native]).events.map(\.id), ["history:h1"])
    }

    // MARK: Form contract

    /// The card the computer sends for one contract case: the asserted fields, with `item` as its question item.
    private func question(_ example: JSONObject) throws -> Question {
        var card = try XCTUnwrap(example.object("card"))
        card["question_hash"] = "hash"
        card["questions"] = card.object("item").map { .array([.object($0)]) } ?? .array([])
        card["item"] = nil
        card["question_count"] = nil
        return try XCTUnwrap(NativeEvents.questions(["pending_questions": [.object(card)]]).first)
    }

    func testFormContractFixtureIsSharedByteForByte() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../tests/fixtures/opencode-form-contract.json").standardized
        XCTAssertEqual(try Data(contentsOf: SharedFixture.url("opencode-form-contract.json")), try Data(contentsOf: root))
    }

    func testFormContractCardsParseAnswerAndRefuseLikeTheComputer() throws {
        let contract = try SharedFixture.json("opencode-form-contract.json")
        let cases = contract.objects("cases")
        XCTAssertEqual(cases.count, 11)
        for example in cases {
            let name = example.string("name")
            let card = try XCTUnwrap(example.object("card"), name)
            let parsed = try question(example)
            XCTAssertEqual(parsed.id, card.string("question_id"), name)
            XCTAssertEqual(parsed.canAnswer, card.bool("can_answer"), name)
            XCTAssertEqual(parsed.approval, card.bool("approval"), name)
            XCTAssertEqual(parsed.unavailableReason, card.string("unavailable_reason"), name)
            XCTAssertEqual(QuestionPolicies.showsSubmit(parsed), card.bool("can_answer") && !card.bool("approval"), name)
            if let item = card.object("item") {
                let prompt = try XCTUnwrap(parsed.prompts.first, name)
                XCTAssertEqual(prompt.id, item.string("id"), name)
                XCTAssertEqual(prompt.header, item.string("header"), name)
                XCTAssertEqual(prompt.text, item.string("prompt"), name)
                XCTAssertEqual(prompt.choices.map(\.id), item.objects("options").map { $0.string("id") }, name)
                XCTAssertEqual(prompt.choices.map(\.label), item.objects("options").map { $0.string("label") }, name)
                XCTAssertEqual(prompt.choices.map(\.description), item.objects("options").map { $0.string("description") }, name)
                XCTAssertEqual(prompt.multi, item.bool("multi_select"), name)
                XCTAssertEqual(prompt.other, item.bool("allow_other"), name)
                XCTAssertEqual(prompt.required, item.bool("required", default: true), name)
                XCTAssertEqual(prompt.inputKind, item.string("input_kind"), name)
                for (value, key) in [(prompt.minLength, "min_length"), (prompt.maxLength, "max_length"), (prompt.minItems, "min_items"), (prompt.maxItems, "max_items")] {
                    XCTAssertEqual(value, item.has(key) ? item.int(key) : nil, "\(name).\(key)")
                }
                XCTAssertEqual(prompt.defaultOptionIds, item.strings("default_option_ids"), name)
            }
            guard !parsed.approval, let answers = example.array("answers")?.compactMap(\.object), !answers.isEmpty else { continue }
            var options: [String: Set<String>] = [:], texts: [String: String] = [:]
            for answer in answers {
                options[answer.string("question_id")] = Set(answer.strings("selected_option_ids"))
                texts[answer.string("question_id")] = answer.string("text")
            }
            let error = QuestionPolicies.answerError(parsed.prompts, options: options, texts: texts)
            if example.has("reply") {
                XCTAssertEqual(error, "", name)
                XCTAssertTrue(QuestionPolicies.complete(parsed.prompts, options: options, texts: texts), name)
                XCTAssertEqual(QuestionPolicies.answers(parsed.prompts, options: options, texts: texts), .array(answers.map(JSON.object)), name)
            } else {
                XCTAssertFalse(error.isEmpty, name)
                XCTAssertFalse(QuestionPolicies.complete(parsed.prompts, options: options, texts: texts), name)
                if let item = card.object("item"), item.has("max_length") || item.has("max_items") { XCTAssertEqual(error, example.string("error"), name) }
            }
        }
    }

    func testContractCasesRenderTextLimitDefaultAndUnavailableCards() throws {
        let cases = Dictionary(uniqueKeysWithValues: try SharedFixture.json("opencode-form-contract.json").objects("cases").map { ($0.string("name"), $0) })
        // plain-string: a text field limited to 5 characters.
        let plain = try question(try XCTUnwrap(cases["plain-string"]))
        let text = try XCTUnwrap(plain.prompts.first)
        XCTAssertEqual(text.inputKind, "text")
        XCTAssertTrue(text.other && text.choices.isEmpty, "answered through the text field")
        XCTAssertEqual(text.maxLength, 5)
        XCTAssertEqual(QuestionPolicies.textHint(text), "Up to 5 characters")
        XCTAssertEqual(QuestionPolicies.answerError([text], options: [:], texts: ["k": "five!"]), "")
        XCTAssertEqual(QuestionPolicies.answerError([text], options: [:], texts: ["k": "toolong"]), "Answer is longer than 5 characters")
        // boolean: Yes is preselected.
        let boolean = try question(try XCTUnwrap(cases["boolean"]))
        XCTAssertEqual(QuestionPolicies.defaults(boolean.prompts), ["k": ["yes"]])
        XCTAssertTrue(QuestionPolicies.complete(boolean.prompts, options: QuestionPolicies.defaults(boolean.prompts), texts: [:]))
        // multiselect: at most two answers.
        let multiselect = try question(try XCTUnwrap(cases["multiselect"]))
        let multi = try XCTUnwrap(multiselect.prompts.first)
        XCTAssertEqual(QuestionPolicies.choiceHint(multi), "Choose up to 2")
        // unsupported-number: the computer's reason and no Submit.
        let unsupported = try question(try XCTUnwrap(cases["unsupported-number"]))
        XCTAssertFalse(unsupported.canAnswer)
        XCTAssertEqual(unsupported.unavailableReason, "Answer this form in OpenCode")
        XCTAssertFalse(QuestionPolicies.showsSubmit(unsupported))
    }

    func testAnUnanswerableFormWithoutItemsStillShowsItsReason() throws {
        // hgs clears the items of a form whose text exceeds 16 KiB; the card must not disappear.
        let cleared: JSON = ["question_id": "frm_2", "question_hash": "hash", "kind": "opencode_form", "status": "pending", "answerable": false,
                             "can_answer": false, "approval": false, "source": "opencode_native", "run_id": "run", "conversation_id": "ses_A",
                             "unavailable_reason": "Answer this form in OpenCode", "questions": []]
        let questions = NativeEvents.questions(["pending_questions": [cleared]])
        let shown = try XCTUnwrap(QuestionPolicies.queue(questions, target, chosen: "").selected)
        XCTAssertEqual(shown.id, "frm_2")
        XCTAssertTrue(shown.prompts.isEmpty)
        XCTAssertEqual(shown.unavailableReason, "Answer this form in OpenCode")
        XCTAssertFalse(QuestionPolicies.showsSubmit(shown))
        XCTAssertTrue(QuestionPolicies.shown(shown))
        // Without a reason it still shows, with the card's fallback text; only an "answerable" card without items is hidden.
        var silent = cleared.object!
        silent["unavailable_reason"] = nil
        let fallback = try XCTUnwrap(QuestionPolicies.queue(NativeEvents.questions(["pending_questions": [.object(silent)]]), target, chosen: "").selected)
        XCTAssertEqual(fallback.unavailableReason, "")
        XCTAssertFalse(QuestionPolicies.showsSubmit(fallback))
        var answerable = cleared.object!
        answerable["can_answer"] = true
        XCTAssertNil(QuestionPolicies.queue(NativeEvents.questions(["pending_questions": [.object(answerable)]]), target, chosen: "").selected)
    }

    func testEffortIdFixtureIsSharedByteForByte() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../../tests/fixtures/opencode-effort-ids.json").standardized
        XCTAssertEqual(try Data(contentsOf: SharedFixture.url("opencode-effort-ids.json")), try Data(contentsOf: root))
    }

    /// Every layer shares the native catalog's shape rule (opencode-effort-ids.json); the model's effort_options decide membership.
    func testEffortIdsFollowTheSharedNativeRule() throws {
        let fixture = try SharedFixture.json("opencode-effort-ids.json")
        let accept = fixture.strings("accept"), reject = fixture.strings("reject")
        XCTAssertEqual(accept.count, 10)
        XCTAssertEqual(reject.count, 13)
        XCTAssertTrue(accept.contains("think:deep"))
        func launch(_ effort: String) throws -> JSONObject {
            try SessionActionPolicies.arguments("launch", ["agent": "opencode", "directory": "/example/project", "tag": "work", "account_id": "native-opencode",
                                                           "model": "fixture/local", "effort": .string(effort)])
        }
        func settings(_ effort: String) throws -> JSONObject {
            try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": .string(effort)], effortVariants: true)
        }
        for id in accept {
            XCTAssertTrue(EffortOption.valid(id), id)
            XCTAssertEqual(EffortOption.options([id], labels: []).map(\.value), [id], id)
            XCTAssertNoThrow(try launch(id), id)
            XCTAssertNoThrow(try settings(id), id)
        }
        for id in reject {
            XCTAssertFalse(EffortOption.valid(id), id.debugDescription)
            XCTAssertEqual(EffortOption.options([id], labels: []), [], id.debugDescription)
            XCTAssertThrowsError(try launch(id), id.debugDescription)
            // In a model change "" is not a variant but the request for the model's default.
            if id.isEmpty { XCTAssertNoThrow(try settings(id)); continue }
            XCTAssertThrowsError(try settings(id), id.debugDescription) { XCTAssertEqual(($0 as? ZerusError)?.message, "Invalid effort.") }
        }
        // Accepted ids reach the launch form and the sheet only when the model offers them, still at most 16.
        let offered = accept + ["v12", "v13", "v14", "v15", "v16", "v17", "v18"]
        var raw = catalog
        raw["accounts"] = [["id": "native-opencode", "provider": "opencode", "label": "OpenCode", "model_options": [
            ["id": "fixture/local", "value": "fixture/local", "label": "Local", "effort_options": JSON(reject + offered)]]]]
        let sanitized = LaunchPresentation.sanitizeCatalog(raw)
        let launchable = LaunchPresentation.efforts(sanitized, provider: "opencode", accountId: "native-opencode", model: "fixture/local", supported: true)
        XCTAssertEqual(launchable.map(\.value), Array(offered.prefix(16)), "malformed ids never take one of the 16 places")
        let sheet = SessionDetailsPresentation.settings(["agent": "opencode", "model_options": [["id": "m/x", "effort_options": JSON(accept)]]])
        XCTAssertEqual(sheet.effortOptions("m/x", effortFeature: true).map(\.value), [""] + accept)
        XCTAssertEqual(try launch("think:deep")["effort"], "think:deep")
    }

    /// A well-formed variant the model no longer offers is refused here, at launch and in a model change.
    func testWithdrawnEffortIsRefusedLocally() throws {
        let before: JSONObject = ["agent": "opencode", "settings_model": "fixture/local", "settings_change_supported": true,
                                  "model_options": [["id": "fixture/local", "effort_options": ["low", "think:deep"]]]]
        let after = before.putting("model_options", [["id": "fixture/local", "effort_options": ["low"]]])
        let args: JSONObject = ["model": "fixture/local", "effort": "think:deep"]
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", args, effortVariants: true))
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", args, before), "")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", args, after), "The selected effort is unavailable for this model.")
        XCTAssertEqual(SessionDetailsPresentation.settings(after).keptEffort("think:deep", model: "fixture/local", effortFeature: true), "")
        func catalog(_ efforts: [String]) -> JSONObject {
            LaunchPresentation.sanitizeCatalog(["agents": ["opencode"], "accounts": [["id": "native-opencode", "provider": "opencode", "label": "OpenCode",
                                                                                     "model_options": [["id": "fixture/local", "effort_options": JSON(efforts)]]]]])
        }
        func launch(_ raw: JSONObject) throws -> JSONObject {
            try LaunchPresentation.arguments(raw, agent: "opencode", directory: "/example/project", tag: "work", accountId: "native-opencode", projectId: "",
                                             worktrees: nil, model: "fixture/local", effort: "think:deep", effortSupported: true)
        }
        XCTAssertNoThrow(try launch(catalog(["low", "think:deep"])))
        XCTAssertThrowsError(try launch(catalog(["low"]))) { XCTAssertEqual(($0 as? ZerusError)?.message, "The native effort changed. Review the current choices.") }
    }

    func testOpenCodeTextDefaultSeedsOnlyAQuestionWithoutASavedDraft() throws {
        func card(_ item: JSONObject, source: String = "opencode_native", hash: String = "h1") throws -> Question {
            try XCTUnwrap(NativeEvents.questions(["pending_questions": [["question_id": "frm_1", "question_hash": .string(hash), "can_answer": true,
                                                                         "source": .string(source), "questions": [.object(item)]]]]).first)
        }
        let item: JSONObject = ["id": "k", "header": "Name", "prompt": "Name", "options": [], "allow_other": true, "required": true,
                                "input_kind": "text", "default_text": "Ada"]
        let question = try card(item)
        XCTAssertEqual(question.prompts[0].defaultText, "Ada")
        // Prefilled and complete, but only shown: nothing is saved or sent until the person acts.
        let opened = QuestionPolicies.opening(question.prompts, draftAnswers: "", native: nil)
        XCTAssertEqual(opened.texts, ["k": "Ada"])
        XCTAssertTrue(QuestionPolicies.complete(question.prompts, options: opened.options, texts: opened.texts))
        // A saved draft of this exact question wins, including an intentionally empty text.
        let empty = QuestionPolicies.answers(question.prompts, options: [:], texts: ["k": ""]).compact
        let reopened = QuestionPolicies.opening(question.prompts, draftAnswers: empty, native: nil)
        XCTAssertEqual(reopened.texts, ["k": ""])
        XCTAssertFalse(QuestionPolicies.complete(question.prompts, options: reopened.options, texts: reopened.texts))
        let edited = QuestionPolicies.answers(question.prompts, options: [:], texts: ["k": "Grace"]).compact
        XCTAssertEqual(QuestionPolicies.opening(question.prompts, draftAnswers: edited, native: nil).texts, ["k": "Grace"])
        // A changed question has no saved draft of its own (drafts are keyed by question ID and hash) and gets the default again.
        let saved = Draft(target: target, text: "", questionId: question.id, questionHash: question.hash, answers: empty)
        let changed = try card(item, hash: "h2")
        let changedDraft = [saved].first { $0.target == target && $0.questionId == changed.id && $0.questionHash == changed.hash }
        XCTAssertNil(changedDraft)
        XCTAssertEqual(QuestionPolicies.opening(changed.prompts, draftAnswers: changedDraft?.answers ?? "", native: nil).texts, ["k": "Ada"])
        // A submitted native answer is shown as submitted, never seeded.
        let native = NativeQuestionDelivery(requestId: "r", questionId: question.id, questionHash: question.hash, run: "run", conversation: "ses_A",
                                            status: "submitted", answers: [])
        XCTAssertEqual(QuestionPolicies.opening(question.prompts, draftAnswers: "", native: native).texts, ["k": ""])
        // A legacy card without default_text is unchanged.
        var legacy = item
        legacy["default_text"] = nil
        XCTAssertEqual(QuestionPolicies.opening(try card(legacy).prompts, draftAnswers: "", native: nil).texts, ["k": ""])
        // Only explicit native OpenCode text items carry it.
        XCTAssertEqual(try card(item, source: "codex_tui").prompts[0].defaultText, "")
        let choice: JSONObject = ["id": "c", "header": "Color", "prompt": "Color", "options": [["id": "Green", "label": "Green"]], "allow_other": true,
                                  "input_kind": "choice", "default_text": "Ada"]
        let chooser = try card(choice)
        XCTAssertEqual(chooser.prompts[0].defaultText, "")
        XCTAssertEqual(QuestionPolicies.opening(chooser.prompts, draftAnswers: "", native: nil).texts, ["c": ""])
        // Sanitized like other text: control characters are ignored and the value is clipped to the reported bound.
        XCTAssertEqual(try card(item.putting("default_text", "Ada\nLovelace")).prompts[0].defaultText, "")
        XCTAssertEqual(try card(item.putting("default_text", 7)).prompts[0].defaultText, "")
        XCTAssertEqual(try card(item.putting("max_length", 2)).prompts[0].defaultText, "Ad")
        XCTAssertEqual(try card(item.putting("default_text", .string(String(repeating: "é", count: 3000)))).prompts[0].defaultText.utf8.count, 4096)
    }

    func testFormItemLimitsApplyOnlyWhenReported() {
        let base = Prompt(id: "k", text: "Pick", choices: ["a", "b", "c"].map { Choice(id: $0, label: $0.uppercased(), description: "") },
                          multi: true, required: true, other: false, header: "", body: "", otherLabel: "", otherDescription: "")
        var bounded = base
        bounded.inputKind = "choice"
        bounded.minItems = 2
        bounded.maxItems = 2
        XCTAssertEqual(QuestionPolicies.choiceHint(bounded), "Choose 2")
        XCTAssertEqual(QuestionPolicies.answerError([bounded], options: ["k": ["a"]], texts: [:]), "Choose at least 2 answers")
        XCTAssertEqual(QuestionPolicies.answerError([bounded], options: ["k": ["a", "b", "c"]], texts: [:]), "Choose at most 2 answers")
        XCTAssertTrue(QuestionPolicies.complete([bounded], options: ["k": ["a", "c"]], texts: [:]))
        XCTAssertEqual(QuestionPolicies.answerError([base], options: ["k": ["a", "b", "c"]], texts: [:]), "", "other agents keep today's rules")
        var text = Prompt(id: "t", text: "Name", choices: [], multi: false, required: false, other: true, header: "", body: "", otherLabel: "", otherDescription: "")
        XCTAssertFalse(QuestionPolicies.complete([text], options: [:], texts: [:]), "other agents still need every answer")
        text.inputKind = "text"
        text.minLength = 3
        XCTAssertEqual(QuestionPolicies.textHint(text), "At least 3 characters")
        XCTAssertEqual(QuestionPolicies.answerError([text], options: [:], texts: ["t": "ab"]), "Answer is shorter than 3 characters")
        XCTAssertTrue(QuestionPolicies.complete([text], options: [:], texts: [:]), "an optional form field may stay empty")
        text.maxLength = 8
        XCTAssertEqual(QuestionPolicies.textHint(text), "3–8 characters")
    }
}
