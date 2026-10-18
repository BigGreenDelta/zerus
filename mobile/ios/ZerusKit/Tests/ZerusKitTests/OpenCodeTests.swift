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

    private func prompt(_ text: String, _ id: String, _ at: Double, _ state: String? = nil, request: JSON? = nil) -> JSON {
        var row: JSONObject = ["type": "UserPromptSubmit", "detail": .string(text), "source": "opencode_native", "agent_id": "", "message_id": .string(id),
                               "at": .double(at)]
        if let state { row["delivery_state"] = .string(state) }
        if let request { row["request_id"] = request }
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

    // MARK: Request identity

    /// This phone's outbox request; OpenCode prompts Zerus sent name theirs in `request_id`.
    private let request = "6f0e3f43-4b4c-4f2d-9f43-3d6b6c1f1a01"
    private let otherRequest = "6f0e3f43-4b4c-4f2d-9f43-3d6b6c1f1a02"

    /// This phone's message, submitted at 1000.
    private func sent(_ text: String = "Waited in the queue") -> OutgoingMessage {
        OutgoingMessage(requestId: request, target: target, text: text, attachments: [], createdAt: 999, status: "submitted", submittedAt: 1_000)
    }

    private func merge(_ rows: JSON..., outgoing: OutgoingMessage? = nil) -> ConversationMerge.Result {
        ConversationMerge.merge(target: target, native: NativeEvents.events(["events": .array(rows)]), outgoing: [outgoing ?? sent()])
    }

    /// The same rows as if no OpenCode prompt named a request.
    private func unnamed(_ rows: [Event]) -> [Event] {
        rows.map { row in
            var copy = row
            if copy.source == "opencode_native" { copy.requestId = "" }
            return copy
        }
    }

    func testAPromptNamingThisPhonesRequestIsItsSentMessage() {
        // OpenCode dates a queued prompt by its delivery: here 12 s after this phone submitted it.
        let rows = NativeEvents.events(["events": [prompt("Waited in the queue", "msg_1", 1_012, request: .string(request))]])
        XCTAssertEqual(rows.map(\.requestId), [request])
        XCTAssertEqual(rows.map(\.receiptRequestId), [""], "it stays a native row, not a receipt")
        let result = ConversationMerge.merge(target: target, native: rows, outgoing: [sent()])
        XCTAssertEqual(result.events.map(\.id), ["outgoing:\(request)"])
        XCTAssertEqual(result.events.map(\.outgoingId), [request])
        XCTAssertEqual(result.events.map(\.delivery), ["recorded"])
        XCTAssertEqual(result.events.map(\.deliveryState), [nil])
        XCTAssertEqual(result.events.map(\.at), [1_012])
        XCTAssertEqual(result.recordedRequestIds, [request])
    }

    func testAQueuedPromptNamingThisPhonesRequestKeepsItsNativeState() {
        for state in ["queued", "not_delivered"] {
            let result = merge(prompt("hello", "msg_0", 990), prompt("Waited in the queue", "msg_1", 1_012, state, request: .string(request)))
            XCTAssertEqual(result.events.map(\.text), ["hello", "Waited in the queue"], state)
            XCTAssertEqual(result.events.map(\.id).last, "outgoing:\(request)", state)
            XCTAssertEqual(result.events.map(\.deliveryState), [nil, state], state)
            XCTAssertEqual(result.events.map { QueuedDelivery.label($0.deliveryState) }, ["", QueuedDelivery.label(state)], state)
            XCTAssertEqual(result.recordedRequestIds, [request], state)
        }
    }

    func testAPromptNamingAnotherRequestIsNeverThisPhonesMessage() {
        // Not even with the same text inside the 10 s window that matches a prompt naming no request.
        for at in [1_001.0, 1_012] {
            let result = merge(prompt("Waited in the queue", "msg_1", at, request: .string(otherRequest)))
            XCTAssertEqual(result.events.count, 2, "\(at)")
            XCTAssertEqual(result.events.map(\.source), ["mobile_outbox", "opencode_native"], "\(at)")
            XCTAssertEqual(result.events.map(\.outgoingId), [request, ""], "\(at)")
            XCTAssertEqual(result.events[0].delivery, "submitted", "\(at)")
            XCTAssertTrue(result.recordedRequestIds.isEmpty, "\(at)")
        }
    }

    func testAPromptNamingNoRequestKeepsTheTextAndTimeMatch() {
        // The known limit: a prompt naming no request that OpenCode dated 12 s after submission stays a separate row.
        let late = merge(prompt("Waited in the queue", "msg_1", 1_012))
        XCTAssertEqual(late.events.map(\.source), ["mobile_outbox", "opencode_native"])
        XCTAssertTrue(late.recordedRequestIds.isEmpty)
        let early = merge(prompt("Waited in the queue", "msg_1", 1_010))
        XCTAssertEqual(early.events.map(\.id), ["outgoing:\(request)"])
        XCTAssertEqual(early.recordedRequestIds, [request])
    }

    func testOnlyACanonicalLowercaseUuidNamesARequest() {
        let invalid: [JSON] = [.string(request.uppercased()), .string("{\(request)}"), .string(request.replacingOccurrences(of: "-", with: "")),
                               .string(" \(request)"), .string(request + "0"), .string("request-queued"), .string(""), .int(7), .bool(true), .null,
                               .array([.string(request)]), .object(["id": .string(request)])]
        for value in invalid {
            XCTAssertEqual(NativeEvents.events(["events": [prompt("Waited in the queue", "msg_1", 1_012, request: value)]]).map(\.requestId), [""], value.compact)
            // Ignored: the prompt is unmatched 12 s later and still matches by text and time.
            XCTAssertEqual(merge(prompt("Waited in the queue", "msg_1", 1_012, request: value)).events.count, 2, value.compact)
            XCTAssertEqual(merge(prompt("Waited in the queue", "msg_1", 1_001, request: value)).events.map(\.id), ["outgoing:\(request)"], value.compact)
        }
    }

    func testOtherSourcesAndRowsIgnoreRequestId() {
        let named: JSONObject = ["type": "UserPromptSubmit", "detail": "Waited in the queue", "agent_id": "", "message_id": "native-1", "at": 1_012,
                                 "request_id": .string(request)]
        for source in ["", "codex_transcript", "claude_transcript", "kimi_wire", "dsh_native", "opencode"] {
            let row = source.isEmpty ? named : named.putting("source", .string(source))
            XCTAssertEqual(NativeEvents.events(["events": [.object(row)]]).map(\.requestId), [""], source)
            XCTAssertEqual(merge(.object(row)).events.count, 2, source)
        }
        // Receipts keep naming their request by message_id.
        let receipt = named.putting("source", "hgs_delivery").putting("message_id", .string(otherRequest))
        XCTAssertEqual(NativeEvents.events(["events": [.object(receipt)]]).map(\.requestId), [otherRequest])
        XCTAssertEqual(NativeEvents.events(["events": [.object(receipt)]]).map(\.receiptRequestId), [otherRequest])
        XCTAssertEqual(merge(.object(receipt)).events.count, 2)
        // Only an OpenCode prompt names its request, never a reply or an answer.
        for type in ["AgentMessage", "QuestionAnswered"] {
            let row = named.putting("source", "opencode_native").putting("type", .string(type))
            XCTAssertEqual(NativeEvents.events(["events": [.object(row)]]).map(\.requestId), [""], type)
        }
    }

    func testAPromptNamingItsRequestStaysANativeRowForAttachmentReceipts() {
        // The OpenCode adapter refuses attachments today. A receipt treats a prompt naming its request like one naming none.
        let text = "Compare images"
        let receipt: JSON = ["type": "UserPromptSubmit", "source": "hgs_delivery", "message_id": .string(request), "at": 1_000, "submitted_text": .string(text),
                             "detail": .string(text), "attachments": [["name": "image.png", "mime": "image/png", "bytes": 120]]]
        let receipts = NativeEvents.events(["attachment_messages": [receipt]])
        for (at, count) in [(1_002.0, 1), (1_012, 2)] {
            let named = prompt(text, "msg_1", at, request: .string(request)), plain = prompt(text, "msg_1", at)
            let joint = NativeEvents.events(["events": [named], "attachment_messages": [receipt]])
            XCTAssertEqual(joint.count, count, "\(at)")
            XCTAssertEqual(unnamed(joint), NativeEvents.events(["events": [plain], "attachment_messages": [receipt]]), "\(at)")
            let rolling = RollingConversationHistory.merge(target: target, previousTarget: target, previous: NativeEvents.events(["events": [named]]), incoming: receipts)
            let expected = RollingConversationHistory.merge(target: target, previousTarget: target, previous: NativeEvents.events(["events": [plain]]), incoming: receipts)
            XCTAssertEqual(rolling.events.count, count, "\(at)")
            XCTAssertEqual(unnamed(rolling.events), expected.events, "\(at)")
            XCTAssertEqual(rolling.supersededIds, expected.supersededIds, "\(at)")
        }
        // The receipt keeps precedence for this phone's message, whatever the row order.
        let rows = NativeEvents.events(["events": [prompt(text, "msg_1", 1_012, request: .string(request))], "attachment_messages": [receipt]])
        for order in [rows, Array(rows.reversed())] {
            let result = ConversationMerge.merge(target: target, native: order, outgoing: [sent(text)])
            XCTAssertEqual(result.events.count, 2)
            XCTAssertEqual(result.events.filter { $0.outgoingId == request }.map(\.source), ["hgs_delivery"])
        }
    }

    func testOnlyACanonicalOutboxRequestClaimsARowByRequest() {
        // A malformed persisted outbox entry claims nothing by request: not a prompt typed in OpenCode, not a receipt.
        let rows = NativeEvents.events(["events": [prompt("Typed in OpenCode", "msg_1", 1_012),
                                                   ["type": "UserPromptSubmit", "source": "hgs_delivery", "detail": "Unnamed receipt", "at": 1_013],
                                                   ["type": "UserPromptSubmit", "source": "hgs_delivery", "message_id": "receipt", "detail": "Receipt", "at": 1_014]]])
        XCTAssertEqual(rows.map(\.requestId), ["", "", "receipt"])
        for id in ["", "receipt", request.uppercased()] {
            let message = OutgoingMessage(requestId: id, target: target, text: "Waited in the queue", attachments: [], createdAt: 999, status: "submitted",
                                          submittedAt: 1_000)
            let result = ConversationMerge.merge(target: target, native: rows, outgoing: [message])
            XCTAssertEqual(result.events.map(\.source), ["mobile_outbox", "opencode_native", "hgs_delivery", "hgs_delivery"], id)
            XCTAssertEqual(Array(result.events.dropFirst()), rows, id)
            XCTAssertTrue(result.recordedRequestIds.isEmpty, id)
            let typed = ConversationMerge.merge(target: target, native: [rows[0]], outgoing: [message])
            XCTAssertEqual(typed.events.map(\.source), ["mobile_outbox", "opencode_native"], id)
            XCTAssertTrue(typed.recordedRequestIds.isEmpty, id)
        }
    }

    func testANamedPromptRecordsAnUncertainOrFailedMessageAndEndsItsRecovery() {
        // The phone lost the connection after submitting; OpenCode queued the message and delivered it 12 s later.
        for status in ["uncertain", "failed"] {
            var message = sent()
            message.status = status
            message.submittedAt = 0
            message.error = "Delivery was interrupted. Check its receipt before sending again."
            XCTAssertTrue(message.recoverable, status)
            let state = MessageState(outgoing: [message])
            let named = merge(prompt("Waited in the queue", "msg_1", 1_012, request: .string(request)), outgoing: message)
            XCTAssertEqual(named.events.map(\.id), ["outgoing:\(request)"], status)
            XCTAssertEqual(named.recordedRequestIds, [request], status)
            let recorded = MessageChanges.recorded(state, named.recordedRequestIds)
            XCTAssertEqual(recorded.outgoing.map(\.status), ["recorded"], status)
            XCTAssertEqual(recorded.outgoing.map(\.error), [""], status)
            XCTAssertEqual(recorded.outgoing.map(\.recoverable), [false], status)
            // A row naming another request leaves the message to its recovery.
            let other = merge(prompt("Waited in the queue", "msg_1", 1_012, request: .string(otherRequest)), outgoing: message)
            XCTAssertEqual(other.events.count, 2, status)
            XCTAssertTrue(other.recordedRequestIds.isEmpty, status)
            XCTAssertEqual(MessageChanges.recorded(state, other.recordedRequestIds), state, status)
        }
        var reviewed = sent()
        reviewed.status = "reviewed"
        XCTAssertEqual(MessageChanges.recorded(MessageState(outgoing: [reviewed]), [request]).outgoing, [reviewed])
    }

    func testSameTextMessagesEachTakeTheirOwnNamedPrompt() {
        let first = sent()
        let second = OutgoingMessage(requestId: otherRequest, target: target, text: "Waited in the queue", attachments: [], createdAt: 1_000, status: "submitted",
                                     submittedAt: 1_001)
        // OpenCode delivered them in the other order, outside and inside the text and time window.
        for at in [1_020.0, 1_002] {
            let rows = NativeEvents.events(["events": [prompt("Waited in the queue", "msg_2", at, request: .string(otherRequest)),
                                                       prompt("Waited in the queue", "msg_1", at + 1, request: .string(request))]])
            for outgoing in [[first, second], [second, first]] {
                let result = ConversationMerge.merge(target: target, native: rows, outgoing: outgoing)
                XCTAssertEqual(result.events.map(\.outgoingId), [otherRequest, request], "\(at)")
                XCTAssertEqual(result.events.map(\.originalId), rows.map(\.originalId), "\(at)")
                XCTAssertEqual(result.recordedRequestIds, [request, otherRequest], "\(at)")
            }
        }
    }

    /// A canonical history page row of an OpenCode prompt.
    private func paged(_ text: String, _ id: String, _ at: Double, request: JSON? = nil) throws -> Event {
        var row = try XCTUnwrap(prompt(text, id, at, request: request).object)
        row["history_id"] = "h1"
        row["history_stream"] = "journal"
        row["history_cursor"] = "c1"
        return try XCTUnwrap(NativeEvents.events(["history_epoch": "epoch", "events": [.object(row)]]).first)
    }

    func testAnOlderPagedPromptTakesTheRequestItsOwnInspectionNames() throws {
        // An older hgs indexed the prompt without its request; this window's inspection of the same message names it now.
        let canonical = try paged("Waited in the queue", "msg_1", 1_012)
        XCTAssertEqual(canonical.requestId, "")
        let window = HistoryWindow(target: target, epoch: "epoch", events: [canonical], before: nil, after: "c1", hasBefore: false, hasAfter: false, atHead: true)
        let named = NativeEvents.events(["events": [prompt("Waited in the queue", "msg_1", 1_012, request: .string(request))]])
        var expected = canonical
        expected.requestId = request
        XCTAssertEqual(window.inspection(named, scope: target).events, [expected], "identity, cursor and read epoch stay exact")
        XCTAssertEqual(HistoryInspection.retain(window, nil, named, [], scope: target).events, [expected])
        // This phone's message, pending for more than 10 s, merges once.
        let merged = ConversationMerge.merge(target: target, native: window.inspection(named, scope: target).events, outgoing: [sent()])
        XCTAssertEqual(merged.events.map(\.id), ["outgoing:\(request)"])
        XCTAssertEqual(merged.events.map(\.historyId), ["h1"])
        XCTAssertEqual(merged.recordedRequestIds, [request])
    }

    func testAPagedPromptTakesNoRequestFromAnyOtherRow() throws {
        let canonical = try paged("Waited in the queue", "msg_1", 1_012)
        let window = HistoryWindow(target: target, epoch: "epoch", events: [canonical], before: nil, after: "c1", hasBefore: false, hasAfter: false, atHead: true)
        let named = NativeEvents.events(["events": [prompt("Waited in the queue", "msg_1", 1_012, request: .string(request))]])
        func separate(_ rows: [Event], _ label: String) {
            XCTAssertEqual(rows.map(\.requestId), [""], label)
            XCTAssertEqual(ConversationMerge.merge(target: target, native: rows, outgoing: [sent()]).events.count, 2, label)
        }
        // Another OpenCode message naming this request.
        let other = window.inspection(NativeEvents.events(["events": [prompt("Waited in the queue", "msg_2", 1_012, request: .string(request))]]), scope: target)
        XCTAssertEqual(other.events, [canonical])
        separate(other.events, "message")
        // The body changed, so hgs names no request on the inspection row; the phone guesses none.
        let edited = window.inspection(NativeEvents.events(["events": [prompt("Edited in OpenCode", "msg_1", 1_012)]]), scope: target)
        XCTAssertEqual(edited.events.map(\.text), ["Edited in OpenCode"])
        separate(edited.events, "body")
        // Another conversation's or run's inspection, or one of unknown scope.
        for scope in [target.with(conversation: "ses_B"), target.with(run: "other"), nil] as [Target?] {
            XCTAssertEqual(window.inspection(named, scope: scope).events, [canonical], scope?.key ?? "nil")
            XCTAssertEqual(HistoryInspection.retain(window, nil, named, [], scope: scope).events, [canonical], scope?.key ?? "nil")
        }
        separate(window.inspection(named).events, "scope")
        // A non-canonical request.
        var odd = named[0]
        odd.requestId = request.uppercased()
        XCTAssertEqual(window.inspection([odd], scope: target).events, [canonical])
        // A request the history row already names is never overwritten.
        let recorded = try paged("Waited in the queue", "msg_1", 1_012, request: .string(otherRequest))
        XCTAssertEqual(recorded.requestId, otherRequest)
        let kept = HistoryWindow(target: target, epoch: "epoch", events: [recorded], before: nil, after: "c1", hasBefore: false, hasAfter: false, atHead: true)
        XCTAssertEqual(kept.inspection(named, scope: target).events, [recorded])
        XCTAssertEqual(ConversationMerge.merge(target: target, native: kept.inspection(named, scope: target).events, outgoing: [sent()]).events.count, 2)
    }

    func testAPagedPromptTakesARequestOnlyFromItsOwnPromptRow() throws {
        func head(_ canonical: Event) -> HistoryWindow {
            HistoryWindow(target: target, epoch: "epoch", events: [canonical], before: nil, after: "c1", hasBefore: false, hasAfter: false, atHead: true)
        }
        func page(_ row: JSONObject) throws -> Event {
            let stored = row.putting("history_id", "h1").putting("history_stream", "journal").putting("history_cursor", "c1")
            return try XCTUnwrap(NativeEvents.events(["history_epoch": "epoch", "events": [.object(stored)]]).first)
        }
        // An identity that is not an OpenCode msg_ message ID.
        let native = try paged("Waited in the queue", "native-1", 1_012)
        let nativeRows = NativeEvents.events(["events": [prompt("Waited in the queue", "native-1", 1_012, request: .string(request))]])
        XCTAssertEqual(nativeRows.map(\.requestId), [request])
        XCTAssertEqual(head(native).inspection(nativeRows, scope: target).events, [native])
        // A row that is not an OpenCode user prompt, even when its inspection row names a request.
        let reply: JSONObject = ["type": "AgentMessage", "detail": "Done", "source": "opencode_native", "agent_id": "", "message_id": "msg_1", "at": 1_013,
                                 "request_id": .string(request)]
        let codex = try XCTUnwrap(prompt("Waited in the queue", "msg_1", 1_012, request: .string(request)).object).putting("source", "codex_transcript")
        for row in [reply, codex] {
            let label = row.string("source") + " " + row.string("type")
            let stored = try page(row)
            var named = try XCTUnwrap(NativeEvents.events(["events": [.object(row)]]).first)
            XCTAssertEqual([stored.requestId, named.requestId], ["", ""], label)
            named.requestId = request
            XCTAssertEqual(head(stored).inspection([named], scope: target).events, [stored], label)
        }
        // An inspection that no longer reports the row, as its bounded window moved on, copies nothing.
        let canonical = try paged("Waited in the queue", "msg_1", 1_012)
        let later = NativeEvents.events(["events": [prompt("Later", "msg_3", 1_030, request: .string(otherRequest))]])
        for incoming in [[], later] {
            XCTAssertEqual(head(canonical).inspection(incoming, scope: target).events, [canonical])
        }
        // A shortened inspection row of the same message still names its request; the full history text stays.
        var shortened = try XCTUnwrap(prompt("Waited in", "msg_1", 1_012, request: .string(request)).object)
        shortened["detail_truncated"] = true
        let truncated = NativeEvents.events(["events": [.object(shortened)]])
        XCTAssertEqual(truncated.map(\.detailTruncated), [true])
        var expected = canonical
        expected.requestId = request
        XCTAssertEqual(head(canonical).inspection(truncated, scope: target).events, [expected])
        XCTAssertEqual(ConversationMerge.merge(target: target, native: [expected], outgoing: [sent()]).events.map(\.id), ["outgoing:\(request)"])
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
        XCTAssertEqual(cases.count, 17)
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
            if parsed.approval {
                // An OpenCode permission is answered with its native choices, never the allow/deny pair the computer refuses.
                XCTAssertTrue(QuestionPolicies.openCodePermission(parsed), name)
                XCTAssertFalse(QuestionPolicies.approvalSupported(parsed), name)
                XCTAssertNotNil(PermissionConfirmation(parsed), name)
                let answers = try XCTUnwrap(example.array("answers"), name)
                let option = answers.first?.object?.strings("selected_option_ids").first ?? ""
                let offered = parsed.prompts.first?.choices.contains { $0.id == option } == true
                XCTAssertEqual(offered, example.has("reply"), name)
                if offered { XCTAssertEqual(QuestionPolicies.approvalAnswers(parsed, option), .array(answers), name) }
                continue
            }
            guard let answers = example.array("answers")?.compactMap(\.object), !answers.isEmpty else { continue }
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

    /// pl2u.5: a form answer reaches OpenCode's form reply, never a terminal, so text starting with / or ! is ordinary text there.
    func testContractFormTextStartingWithSlashOrBangIsAnOrdinaryAnswer() throws {
        let cases = Dictionary(uniqueKeysWithValues: try SharedFixture.json("opencode-form-contract.json").objects("cases").map { ($0.string("name"), $0) })
        for name in ["plain-string-slash", "plain-string-bang"] {
            let example = try XCTUnwrap(cases[name])
            let card = try question(example)
            let answers = try XCTUnwrap(example.array("answers"))
            let text = try XCTUnwrap(answers.first?.object?.string("text"))
            XCTAssertTrue(text.hasPrefix("/") || text.hasPrefix("!"), name)
            XCTAssertEqual(QuestionPolicies.answerError(card.prompts, options: [:], texts: ["k": text]), "", name)
            XCTAssertTrue(QuestionPolicies.complete(card.prompts, options: [:], texts: ["k": text]), name)
            XCTAssertEqual(QuestionPolicies.answers(card.prompts, options: [:], texts: ["k": text]), .array(answers), name)
            // The same card from any other agent keeps the text on the computer.
            var other = try XCTUnwrap(example.object("card"))
            other["source"] = "codex_tui"
            let codex = try question(example.putting("card", .object(other)))
            XCTAssertEqual(QuestionPolicies.answerError(codex.prompts, options: [:], texts: ["k": text]), "Enter command-like answers on your computer.", name)
            XCTAssertFalse(QuestionPolicies.complete(codex.prompts, options: [:], texts: ["k": text]), name)
        }
        // Other text on an OpenCode choice item is form text too.
        let choice = try question(try XCTUnwrap(cases["question-custom-text"]))
        XCTAssertEqual(QuestionPolicies.answerError(choice.prompts, options: [:], texts: ["q0": "/purple"]), "")
        XCTAssertTrue(QuestionPolicies.complete(choice.prompts, options: [:], texts: ["q0": "!purple"]))
        XCTAssertEqual(QuestionPolicies.answers(choice.prompts, options: [:], texts: ["q0": "/purple"]).array?.first?.string("text"), "/purple")
    }

    /// pl2u.6: an optional form item may stay empty in the policy, Send, the answer rows and the send guard; required items do not.
    func testOptionalFormItemsMayStayEmptyThroughSend() throws {
        let contract = Dictionary(uniqueKeysWithValues: try SharedFixture.json("opencode-form-contract.json").objects("cases").map { ($0.string("name"), $0) })
        let example = try XCTUnwrap(contract["plain-string-optional"])
        let single = try question(example)
        XCTAssertEqual(QuestionPolicies.opening(single.prompts, draftAnswers: "", native: nil).texts, ["k": ""])
        XCTAssertTrue(QuestionPolicies.complete(single.prompts, options: [:], texts: [:]))
        XCTAssertEqual(QuestionPolicies.answers(single.prompts, options: [:], texts: [:]), .array(try XCTUnwrap(example.array("answers"))))
        XCTAssertTrue(QuestionSendPolicies.available(single, target, QuestionPolicies.answers(single.prompts, options: [:], texts: [:]).compact))

        // A multi-part form: every part stays reachable and only the required one must be answered.
        let name: JSONObject = ["id": "name", "header": "Name", "prompt": "Name", "options": [], "allow_other": true, "required": true, "input_kind": "text"]
        let note: JSONObject = ["id": "note", "header": "Note", "prompt": "Note", "options": [], "allow_other": true, "required": false, "input_kind": "text"]
        let pick: JSONObject = ["id": "pick", "header": "Pick", "prompt": "Pick", "options": [["id": "a", "label": "A"], ["id": "b", "label": "B"]],
                                "multi_select": true, "min_items": 2, "allow_other": false, "required": false, "input_kind": "choice"]
        let card = try XCTUnwrap(NativeEvents.questions(["pending_questions": [["question_id": "frm_2", "question_hash": "hash", "can_answer": true,
                                                                                "source": "opencode_native", "run_id": "run", "conversation_id": "ses_A",
                                                                                "questions": [.object(name), .object(note), .object(pick)]]]]).first)
        XCTAssertFalse(QuestionPolicies.complete(card.prompts, options: [:], texts: [:]), "a required item is still enforced")
        XCTAssertTrue(QuestionPolicies.complete(card.prompts, options: [:], texts: ["name": "Ada"]))
        XCTAssertEqual(QuestionPolicies.answerError(card.prompts, options: [:], texts: ["name": "Ada"]), "")
        let rows = QuestionPolicies.answers(card.prompts, options: [:], texts: ["name": "Ada"])
        XCTAssertEqual(rows, [["question_id": "name", "selected_option_ids": [], "text": "Ada"], ["question_id": "note", "selected_option_ids": [], "text": ""],
                              ["question_id": "pick", "selected_option_ids": [], "text": ""]])
        XCTAssertTrue(QuestionSendPolicies.available(card, target, rows.compact))
        // A partly answered optional item still meets its bounds.
        XCTAssertEqual(QuestionPolicies.answerError(card.prompts, options: ["pick": ["a"]], texts: ["name": "Ada"]), "Choose at least 2 answers")
        // Codex optional questions have no input_kind: they still need an answer or Skip.
        var codex = card
        codex.source = "codex_async"
        codex.prompts = codex.prompts.map { prompt in
            var copy = prompt
            copy.inputKind = ""
            return copy
        }
        XCTAssertFalse(QuestionPolicies.complete(codex.prompts, options: [:], texts: ["name": "Ada"]))
    }

    /// pl2u.4: OpenCode permissions are answered with their own choices; each button sends the contract's decision row.
    func testPermissionButtonsSendTheContractDecisions() throws {
        let cases = Dictionary(uniqueKeysWithValues: try SharedFixture.json("opencode-form-contract.json").objects("cases").map { ($0.string("name"), $0) })
        let card = try question(try XCTUnwrap(cases["permission"]))
        XCTAssertTrue(QuestionPolicies.openCodePermission(card))
        XCTAssertFalse(QuestionPolicies.showsSubmit(card), "decisions use their own buttons")
        // Each button sends the decision of its contract case.
        XCTAssertEqual(PermissionAction.actions(card), [.reject, .allowOnce, .alwaysAllow])
        let decisions: [(PermissionAction, String)] = [(.allowOnce, "permission-once"), (.alwaysAllow, "permission"), (.reject, "permission-reject")]
        for (action, name) in decisions {
            let expected = JSON.array(try XCTUnwrap(cases[name]?.array("answers")))
            XCTAssertEqual(QuestionPolicies.approvalAnswers(card, action.option), expected, name)
            XCTAssertTrue(QuestionSendPolicies.available(card, target, try XCTUnwrap(QuestionPolicies.approvalAnswers(card, action.option)).compact), name)
        }
        // Always allow reaches beyond this request: it is sent only through its confirmation.
        let confirmation = try XCTUnwrap(PermissionConfirmation(card))
        XCTAssertEqual(confirmation.answers(card, editing: true, available: true), JSON.array(try XCTUnwrap(cases["permission"]?.array("answers"))))
        // The computer refuses the old allow id; no button here produces it.
        let refused = try XCTUnwrap(cases["permission-allow-refused"])
        XCTAssertEqual(refused.string("error"), "Unknown answer option")
        XCTAssertFalse(QuestionPolicies.approvalSupported(try question(refused)))
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
