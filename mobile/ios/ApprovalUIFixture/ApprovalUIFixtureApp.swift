import Observation
import SwiftUI
import ZerusKit

// This file belongs only to the disposable simulator fixture target. It never links the live AppModel,
// RelayClient, keychain, persistence, notification delegate or application entry point.
@MainActor @Observable final class AppModel {
    let conversation = ConversationModel()
    let generation = UUID().uuidString
}

/// Memory callback sink, not an implementation of production delivery or admission.
/// Exact QuestionCard.swift and ZerusKit policies decide whether a callback is made.
@MainActor @Observable final class ConversationModel {
    var drafts: [String: Draft] = [:]
    var callbacks: [String] = []
    var pendingQuestionSend: Bool? = nil
    var receiptFlights: Set<String> = []
    func draft(_ target: Target, _ question: Question) -> Draft {
        let key = target.key + ":" + QuestionPolicies.key(question)
        if let value = drafts[key] { return value }
        return Draft(target: target, text: "", questionId: question.id, questionHash: question.hash,
                     updatedAt: 0, revision: "fixture-revision", generation: "fixture-generation")
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
    @State private var serial = 0
    @State private var available = true
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
        let card: JSONObject = ["question_id": "fixture-approval", "question_hash": .string("fixture-hash-\(serial)"),
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
            Text("Callbacks: \(app.conversation.callbacks.count)").accessibilityIdentifier("callback-count")
            Text(app.generation).font(.caption2).accessibilityIdentifier("fixture-generation")
            Text(app.conversation.callbacks.last ?? "none").font(.caption).accessibilityIdentifier("last-callback")
            HStack {
                Button("New card") { serial += 1; available = true; answerable = true; shown = true }
                Button("Hide card") { shown = false }
                Button("Show card") { shown = true }
            }.font(.caption)
            HStack {
                Button("Replace in 2 seconds") { Task { try? await Task.sleep(for: .seconds(2)); serial += 1 } }
                Button("Unavailable in 2 seconds") { Task { try? await Task.sleep(for: .seconds(2)); available = false } }
            }.font(.caption)
            HStack {
                Button("Read-only card") { serial += 1; answerable = false }
                Button("Unavailable card") { available = false }
            }.font(.caption)
            // Like the real conversation, the card has an actual parent scroll viewport. Its unchanged
            // onScrollVisibilityChange must observe native visibility; the fixture never sets it directly.
            ScrollView {
                if shown {
                    QuestionCard(target: target, question: question, available: available, display: $display,
                                 expanded: expanded, onReview: { _ in preconditionFailure("No receipt review in fixture") })
                }
            }
            Spacer(minLength: 0)
        }
        .padding(12)
        .background(Theme.background)
        .foregroundStyle(Theme.onSurface)
        .onChange(of: display.expanded) { _, value in expanded = value }
    }
}
