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
        // Any native variant token is a valid request; the fresh inspection decides whether it is offered.
        XCTAssertNoThrow(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "thinking"]))
        XCTAssertThrowsError(try SessionActionPolicies.arguments("settings", ["model": "fixture/local", "effort": "deep thinking"]))
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/local", "effort": "thinking"], raw), "")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/local", "effort": "high"], raw),
                       "The selected effort is unavailable for this model.")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/plain", "effort": "low"], raw),
                       "The selected effort is unavailable for this model.")
        XCTAssertEqual(SessionActionPolicies.inspectReason(target, "settings", ["model": "fixture/plain", "effort": ""], raw), "")
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
        // A card without items that claims to be answerable, or gives no reason, is still not shown.
        var answerable = cleared.object!
        answerable["can_answer"] = true
        XCTAssertNil(QuestionPolicies.queue(NativeEvents.questions(["pending_questions": [.object(answerable)]]), target, chosen: "").selected)
        var silent = cleared.object!
        silent["unavailable_reason"] = nil
        XCTAssertNil(QuestionPolicies.queue(NativeEvents.questions(["pending_questions": [.object(silent)]]), target, chosen: "").selected)
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
