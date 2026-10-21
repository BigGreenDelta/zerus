import Observation
import SwiftUI
import ZerusKit
import Darwin
import Dispatch

/// Four fixed, payload-free notifications inside this disposable simulator fixture only.
/// The random names avoid cross-test collisions; they do not authenticate a sender.
@MainActor @Observable final class FixtureControls {
    enum Mode: String { case normal, countdown, feedback, replacement, unavailable }
    enum Action: String, Hashable, Sendable {
        case beginFeedbackReview = "begin-feedback-review", replace, beginCountdownReview = "begin-countdown-review"
        case becomeUnavailable = "become-unavailable"
        var acknowledgementCode: String? {
            switch self { case .beginCountdownReview: "c"; case .beginFeedbackReview: "f"; case .replace, .becomeUnavailable: nil }
        }
    }
    let launch: String
    let acknowledgementNonce: String
    let generation: String
    let mode: Mode
    var serial = 0
    var available: Bool
    private(set) var status = "starting"
    private var tokens: [Action: Int32] = [:]
    private var consumed: Set<Action> = []
    private var failure = ""
    private var closed = false

    init(generation: String) {
        let args = CommandLine.arguments
        func selected(_ key: String) -> String {
            let positions = args.indices.filter { args[$0] == key }
            precondition(positions.count == 1 && positions[0] + 1 < args.count, "Fixture selection required")
            return args[positions[0] + 1]
        }
        precondition(args.filter { $0.hasPrefix("--zerus-fixture-") }.sorted() == ["--zerus-fixture-ack", "--zerus-fixture-launch", "--zerus-fixture-mode"])
        let selectedLaunch = selected("--zerus-fixture-launch")
        precondition(UUID(uuidString: selectedLaunch)?.uuidString.lowercased() == selectedLaunch)
        let selectedAcknowledgement = selected("--zerus-fixture-ack")
        precondition(UUID(uuidString: selectedAcknowledgement)?.uuidString.lowercased() == selectedAcknowledgement)
        precondition(selectedAcknowledgement != selectedLaunch && selectedAcknowledgement != generation.lowercased())
        guard let selectedMode = Mode(rawValue: selected("--zerus-fixture-mode")) else { preconditionFailure("Unknown fixture mode") }
        launch = selectedLaunch; acknowledgementNonce = selectedAcknowledgement; self.generation = generation; mode = selectedMode
        available = selectedMode != .feedback && selectedMode != .countdown
    }
    private var actions: [Action] {
        switch mode { case .normal: []; case .feedback: [.beginFeedbackReview]; case .replacement: [.replace]; case .countdown: [.beginCountdownReview]
        case .unavailable: [.becomeUnavailable] }
    }
    private func name(_ action: Action) -> String {
        let value = "test.example.zerus.ui.\(launch).\(generation.lowercased()).\(action.rawValue)"
        precondition(value.utf8.count <= 128)
        return value
    }
    private func failed(_ reason: String) {
        if failure.isEmpty { failure = reason }
        status = "error|\(failure)"
        print("FIXTURE-CONTROL error \(failure) uptime=\(ProcessInfo.processInfo.systemUptime)")
    }
    func register() {
        precondition(status == "starting" && tokens.isEmpty)
        for action in actions {
            var token: Int32 = -1
            let code = name(action).withCString { pointer in
                notify_register_dispatch(pointer, &token, DispatchQueue.main) { [weak self] received in
                    MainActor.assumeIsolated { self?.consume(action, token: received) }
                }
            }
            guard code == 0 && token >= 0 else { failed("registration-\(code)"); close(); return }
            tokens[action] = token
        }
        status = "ready|\(mode.rawValue)|\(launch)|\(generation)"
    }
    private func consume(_ action: Action, token: Int32) {
        guard !closed, failure.isEmpty, actions.contains(action), !consumed.contains(action), tokens[action] == token else {
            failed("stale-or-repeated-action"); return
        }
        tokens.removeValue(forKey: action) // A cancellation is attempted once, including an unknown result.
        guard notify_cancel(token) == 0 else { failed("consumption-cancel"); return }
        consumed.insert(action)
        switch action { case .beginFeedbackReview, .beginCountdownReview: available = true; case .replace: serial += 1
        case .becomeUnavailable: available = false }
        let now = ProcessInfo.processInfo.systemUptime
        status = "ack|\(action.rawValue)|\(serial)|\(now)"
        print("FIXTURE-CONTROL \(status) generation=\(generation)")
        if let code = action.acknowledgementCode {
            let reverse = "zui.ack.\(launch).\(generation.lowercased()).\(acknowledgementNonce).\(code)"
            precondition(reverse.utf8.count <= 128)
            guard reverse.withCString({ notify_post($0) }) == 0 else { failed("reverse-ack-post"); return }
        }
    }
    func close() {
        closed = true
        for action in Array(tokens.keys) {
            guard let token = tokens.removeValue(forKey: action) else { failed("missing-owned-token"); continue }
            if notify_cancel(token) != 0 { failed("close-cancel") }
        }
        if failure.isEmpty { status = "closed" }
    }
}

// This file belongs only to the disposable simulator fixture target. It never links the live AppModel,
// RelayClient, keychain, persistence, notification delegate or application entry point.
@MainActor @Observable final class AppModel {
    let conversation: ConversationModel
    let generation: String
    let controls: FixtureControls
    init() {
        let actualGeneration = UUID().uuidString // Fresh per app process; never replaced by the launch nonce.
        generation = actualGeneration
        let selectedControls = FixtureControls(generation: actualGeneration)
        controls = selectedControls
        conversation = ConversationModel(feedbackPrefix: selectedControls.mode == .feedback)
    }
}

/// Memory callback sink, not an implementation of production delivery or admission.
/// Exact QuestionCard.swift and ZerusKit policies decide whether a callback is made.
@MainActor @Observable final class ConversationModel {
    private let feedbackPrefix: Bool
    var drafts: [String: Draft] = [:]
    var callbacks: [String] = []
    var pendingQuestionSend: Bool? = nil
    var receiptFlights: Set<String> = []
    init(feedbackPrefix: Bool) { self.feedbackPrefix = feedbackPrefix }
    func draft(_ target: Target, _ question: Question) -> Draft {
        let key = target.key + ":" + QuestionPolicies.key(question)
        if let value = drafts[key] { return value }
        var value = Draft(target: target, text: "", questionId: question.id, questionHash: question.hash,
                          updatedAt: 0, revision: "fixture-revision", generation: "fixture-generation")
        if feedbackPrefix && question.id == "fixture-approval" && question.hash == "fixture-hash-0" {
            value.answers = JSON.array([["question_id": "approval", "skip": false, "selected_option_ids": [], "text": "Use a safer comman"]]).compact
        }
        return value
    }
    func edit(_ target: Target, text: String, question: Question, answers: String) {
        var value = draft(target, question)
        value.text = text; value.answers = answers
        drafts[target.key + ":" + QuestionPolicies.key(question)] = value
    }
    func questionSendNotice(_ target: Target, _ question: Question) -> String { "" }
    func answerStatus(_ value: Draft) -> String { value.status }
    func checkReceipt(_ value: Draft) async { preconditionFailure("No receipt reads in this fixture") }
    func sendQuestion(_ target: Target, _ question: Question, answers: String) async {
        callbacks.append(question.hash + "|" + answers)
        var value = draft(target, question); value.status = "submitted"
        drafts[target.key + ":" + QuestionPolicies.key(question)] = value
    }
}

@main struct ApprovalUIFixtureApp: App {
    @State private var model = AppModel()
    var body: some Scene { WindowGroup { ApprovalFixtureScreen().environment(model) } }
}

/// Controls outside the production card only choose synthetic snapshots. Deferred changes are one-shot
/// host events so a card can genuinely change while its native confirmation dialog owns the window.
struct ApprovalFixtureScreen: View {
    @Environment(AppModel.self) private var app
    @Environment(\.horizontalSizeClass) private var horizontalSizeClass
    @State private var answerable = true
    @State private var shown = true
    @State private var expanded = false
    @State private var display = QuestionDisplayState(owner: "")
    private let target = Target(computerId: "fixture-computer", session: "kimi/fixture/approval", run: "fixture-run",
                                conversation: "fixture-conversation", connectionId: "fixture-workspace")
    private var question: Question {
        let options: [JSON] = [
            ["id": "1", "label": "Approve once", "description": "Only this request", "scope": "once"],
            ["id": "2", "label": "Approve for this session", "description": "Exact command for this session only", "scope": "broader"],
            ["id": "3", "label": "Reject", "description": "Do not run this request", "scope": "once"]]
        let card: JSONObject = ["question_id": "fixture-approval", "question_hash": .string("fixture-hash-\(app.controls.serial)"),
            "run_id": .string(target.run), "conversation_id": .string(target.conversation), "source": "kimi_tool_approval",
            "approval": true, "approval_choices": true, "can_answer": .bool(answerable),
            "answer_unavailable_reason": "Synthetic request is no longer current", "questions": [[
                "id": "approval", "question": "Review synthetic approval", "body": "printf fixture # *literal*",
                "allow_other": true, "other_label": "Reject with feedback", "options": .array(options)]]]
        return NativeEvents.questions(["pending_questions": [.object(card)]]).first!
    }
    var body: some View {
        VStack(spacing: 8) {
            Text("Synthetic UI only — no native delivery").font(.caption)
                .accessibilityIdentifier("fixture-dismiss-anchor")
                .accessibilityValue(horizontalSizeClass == .regular ? "regular" : horizontalSizeClass == .compact ? "compact" : "unknown")
            Text("Callbacks: \(app.conversation.callbacks.count)").accessibilityIdentifier("callback-count")
            Text(app.generation).font(.caption2).accessibilityIdentifier("fixture-generation")
            Text(app.controls.status).font(.caption2).lineLimit(1).accessibilityIdentifier("fixture-control-status")
            Text(app.conversation.callbacks.last ?? "none").font(.caption).accessibilityIdentifier("last-callback")
            HStack {
                Button("New card") { app.controls.serial += 1; app.controls.available = true; answerable = true; shown = true }
                Button("Hide card") { shown = false }
                Button("Show card") { shown = true }
            }.font(.caption)
            HStack {
                Button("Read-only card") { app.controls.serial += 1; answerable = false }
                Button("Unavailable card") { app.controls.available = false }
                Button("Close controls") { app.controls.close() }
            }.font(.caption)
            // Like the real conversation, the card has an actual parent scroll viewport. Its unchanged
            // onScrollVisibilityChange must observe native visibility; the fixture never sets it directly.
            ScrollView {
                if shown {
                    QuestionCard(target: target, question: question, available: app.controls.available, display: $display,
                                 expanded: expanded, onReview: { _ in preconditionFailure("No receipt review in fixture") })
                }
            }
            Spacer(minLength: 0)
        }
        .padding(12)
        .background(Theme.background)
        .foregroundStyle(Theme.onSurface)
        .onAppear { if app.controls.status == "starting" { app.controls.register() } }
        .onChange(of: display.expanded) { _, value in expanded = value }
    }
}
