import XCTest
@testable import ZerusKit

/// Ported from Android SessionCardPresentationTest, SessionReferenceTest and SessionPreviewTest.
final class SessionCardsTests: XCTestCase {
    private let parent = Target(computerId: "mac", session: "claude/zerus/mobile", run: "run", conversation: "conversation", connectionId: "workspace")
    private func session(_ raw: String, archive: String = "") -> Session {
        Session(target: parent.with(archiveId: archive), title: "Mobile", agent: "claude", status: "", project: "zerus", preview: "", pendingCount: 0, raw: object(raw))
    }
    private func live(_ fields: String) -> Session { session(#"{"activity":"busy",\#(fields)}"#) }
    private func count(_ fields: String, _ includeCompleted: Bool, online: Bool = true) -> [String] {
        let item = live(fields)
        return [SessionCardPresentation.childCount(item, online, includeCompleted: includeCompleted),
                SessionCardPresentation.childCount(item, online, expanded: true, includeCompleted: includeCompleted)]
    }

    func testOptionalQuestionChipFollowsSharedContract() throws {
        let contract = try SharedFixture.json("optional-question-contract.json")
        for example in contract.objects("cases") {
            let name = example.string("name")
            let chip = SessionCardPresentation.optionalQuestionChip(session(example.object("raw")!.compactJSON), example.bool("online"))
            XCTAssertEqual(chip?.count ?? 0, example.has("android_count") ? example.int("android_count") : example.int("count"), name)
            if !example.has("android_count") { XCTAssertEqual(chip?.label ?? "", example.string("label"), name) }
        }
        XCTAssertEqual(SessionCardPresentation.optionalQuestionDescription(1), "1 optional question waiting")
        XCTAssertEqual(SessionCardPresentation.optionalQuestionDescription(2), "2 optional questions waiting")
        let repeated = #""mobile_attention":[{"question_id":"codex-async:call-a:0","question_hash":"h","optional":true},{"question_id":"codex-async:call-a:0","question_hash":"h2","optional":true}]"#
        XCTAssertEqual(SessionCardPresentation.optionalQuestions(live(repeated), true), 1)
        XCTAssertEqual(SessionCardPresentation.optionalQuestions(session(#"{"activity":"busy",\#(repeated)}"#, archive: "archive"), true), 0)
    }

    func testCheckoutLineNamesTheFolderBesideTheBranch() {
        var raw = object(#"{"cwd":"/work/zerus-work-76f5c0ef","git_root":"/work/zerus-work-76f5c0ef","git_branch":"work-76f5c0ef","git_worktree":true,"git_worktree_name":"zerus-work-76f5c0ef","git_common_dir":"/work/zerus/.git","git_metadata_state":"ok"}"#)
        XCTAssertEqual(SessionCardPresentation.branch(raw), "work-76f5c0ef")
        XCTAssertEqual(SessionCardPresentation.folder(raw), "zerus")
        XCTAssertEqual(SessionCardPresentation.line(raw), "zerus / work-76f5c0ef")
        XCTAssertEqual(SessionCardPresentation.checkout(raw), "zerus / work-76f5c0ef · worktree")
        XCTAssertEqual(SessionCardPresentation.checkoutDescription(raw), "Folder zerus, Git branch work-76f5c0ef, linked worktree zerus-work-76f5c0ef")
        for common in ["/srv/git/zerus.git", "/srv/zerus/.bare/", "C:\\Users\\user\\zerus\\.git"] {
            XCTAssertEqual(SessionCardPresentation.line(raw.putting("git_common_dir", .string(common))), "zerus / work-76f5c0ef", common)
        }
        raw["git_common_dir"] = nil
        XCTAssertEqual(SessionCardPresentation.line(raw), "zerus-work-76f5c0ef / work-76f5c0ef")
        let detached = object(#"{"git_root":"/work/zerus-ui","git_branch":"","git_detached":true,"git_worktree":true,"git_common_dir":"/work/zerus/.git","git_metadata_state":"ok"}"#)
        XCTAssertEqual(SessionCardPresentation.branch(detached), "detached HEAD")
        XCTAssertEqual(SessionCardPresentation.checkout(detached), "zerus / detached HEAD · worktree")
        XCTAssertEqual(SessionCardPresentation.checkoutDescription(detached), "Folder zerus, Detached HEAD, linked worktree")
        let main = object(#"{"cwd":"/work/zerus/tray","git_root":"/work/zerus","git_branch":"main","git_common_dir":"/work/zerus/.git"}"#)
        XCTAssertEqual(SessionCardPresentation.line(main), "zerus / main")
        XCTAssertEqual(SessionCardPresentation.checkout(main), "zerus / main")
        XCTAssertEqual(SessionCardPresentation.branch(object(#"{"git_branch":"main","git_detached":true}"#)), "main")
        XCTAssertEqual(SessionCardPresentation.checkoutDescription(object(#"{"git_branch":"main"}"#)), "Git branch main")
    }

    func testOrdinaryFoldersShowOnlyTheirName() {
        let folder = object(#"{"cwd":"/home/user/notes/","git_branch":"main","git_worktree":true,"git_root":"/repo","git_common_dir":"/repo/.git","git_metadata_state":"not_repo"}"#)
        XCTAssertEqual(SessionCardPresentation.branch(folder), "")
        XCTAssertEqual(SessionCardPresentation.line(folder), "notes")
        XCTAssertEqual(SessionCardPresentation.checkout(folder), "notes")
        XCTAssertEqual(SessionCardPresentation.checkoutDescription(folder), "Folder notes")
        for text in [#"{"git_branch":"","git_metadata_state":"not_repo"}"#, "{}", #"{"git_branch":null,"git_detached":"true","cwd":7}"#, #"{"git_branch":7}"#] {
            let raw = object(text)
            XCTAssertEqual(SessionCardPresentation.line(raw), "", text)
            XCTAssertEqual(SessionCardPresentation.checkout(raw), "", text)
            XCTAssertEqual(SessionCardPresentation.checkoutDescription(raw), "", text)
        }
    }

    func testChildStatusAndInactivity() {
        let captions = ["working": "Working", "finished": "Ready", "ready": "Ready", "idle": "Ready", "error": "Error", "paused": "Paused",
                        "approval": "Needs input", "input": "Needs input", "attention": "Needs input", "unknown": "Unknown", "": "Unknown", "failed": "Unknown"]
        for (state, caption) in captions { XCTAssertEqual(SessionCardPresentation.childStatus(["state": .string(state)]), caption, state) }
        XCTAssertEqual(SessionCardPresentation.childStatus(object(#"{"state":"finished","display_state":"input"}"#)), "Needs input")
        XCTAssertEqual(SessionCardPresentation.childStatus(object(#"{"state":"working","display_state":null}"#)), "Working")
        XCTAssertTrue(SessionCardPresentation.childAction(object(#"{"state":"working","display_state":"approval"}"#)))
        XCTAssertFalse(SessionCardPresentation.childAction(object(#"{"state":"error"}"#)))
        for state in ["finished", "idle", "ready", "error"] { XCTAssertTrue(SessionCardPresentation.inactiveChild(["state": .string(state)])) }
        XCTAssertTrue(SessionCardPresentation.inactiveChild(object(#"{"state":"finished","display_state":"error"}"#)))
        for state in ["working", "paused", "input", "unknown", ""] { XCTAssertFalse(SessionCardPresentation.inactiveChild(["state": .string(state)]), state) }
        XCTAssertFalse(SessionCardPresentation.inactiveChild(object(#"{"state":"finished","display_state":"unknown"}"#)))
        let group = object(#"{"group":true,"active_count":0}"#)
        XCTAssertTrue(SessionCardPresentation.inactiveChild(group, countsComplete: true))
        XCTAssertFalse(SessionCardPresentation.inactiveChild(group, countsComplete: false))
        XCTAssertFalse(SessionCardPresentation.inactiveChild(object(#"{"group":true,"active_count":2}"#), countsComplete: true))
        XCTAssertFalse(SessionCardPresentation.inactiveChild(object(#"{"group":true,"state":"working","active_count":0}"#), countsComplete: true))
    }

    func testRowsHideCompletedTurnsKeepDraftsAndUseStableIdOrder() {
        let item = live(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":5,"subagents":{"c-ready":{"name":"Ready child","state":"finished","detail":"Done"},"a-input":{"name":"Reviewer","state":"working","display_state":"input","detail":"  Approve\n the   plan  ","provider":"codex"},"b-working":{"label":"Explorer","name":"ignored","state":"working","task":"Search the code"},"d-paused":{"state":"paused","detail":null,"task":"Resume later"},"e-error":{"name":"Builder","state":"error"}}"#)
        // Finished and failed turns are both complete.
        let rows = SessionCardPresentation.children(item, true)
        XCTAssertEqual(rows.map(\.id), ["a-input", "b-working", "d-paused"])
        XCTAssertEqual(rows[0], SessionCardChild(id: "a-input", name: "Reviewer", status: "Needs input", detail: "Approve the plan", provider: "codex", kind: .attention, group: false))
        XCTAssertEqual(rows[1], SessionCardChild(id: "b-working", name: "Explorer", status: "Working", detail: "Search the code", provider: "claude", kind: .working, group: false))
        XCTAssertEqual(rows[2], SessionCardChild(id: "d-paused", name: "d-paused", status: "Paused", detail: "Resume later", provider: "claude", kind: .paused, group: false))
        XCTAssertEqual(rows[0].description, "Subagent Reviewer, Needs input")
        let all = SessionCardPresentation.children(item, true, includeCompleted: true)
        XCTAssertEqual(all.map(\.id), ["a-input", "b-working", "c-ready", "d-paused", "e-error"])
        XCTAssertEqual(all[4].kind, .neutral)
        XCTAssertEqual(all[4].status, "Error")
        XCTAssertEqual(SessionCardPresentation.children(item, true, drafted: ["c-ready"]).map(\.id), ["a-input", "b-working", "c-ready", "d-paused"])
        XCTAssertEqual(SessionCardPresentation.children(item, true, drafted: ["e-error"]).map(\.id), ["a-input", "b-working", "d-paused", "e-error"])
    }

    func testRowsOfUnavailableOrArchivedParentAreRecorded() {
        let fields = #""subagents":{"one":{"name":"One","state":"working","display_state":"input"},"two":{"state":"bogus"}}"#
        let offline = SessionCardPresentation.children(live(fields), false)
        XCTAssertEqual(offline.map(\.status), ["Recorded", "Recorded"])
        XCTAssertEqual(offline.map(\.kind), [.neutral, .neutral])
        XCTAssertEqual(SessionCardPresentation.children(session(#"{"state":"archived","activity":"idle",\#(fields)}"#, archive: "archive"), true).map(\.status), ["Recorded", "Recorded"])
        XCTAssertEqual(SessionCardPresentation.children(live(fields), true)[1].status, "Unknown")
    }

    func testProfileGroupsNeverBecomeRows() {
        let profiles = live(#""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":8,"subagents":{"collapsed":{"label":"Do not invent a child","state":"working"}},"subagent_previews":[{"id":"profile:code","label":"code","state":"working","group":true,"active_count":2}]"#)
        XCTAssertTrue(SessionCardPresentation.roster(profiles.raw).isEmpty)
        XCTAssertTrue(SessionCardPresentation.children(profiles, true, includeCompleted: true).isEmpty)
        XCTAssertEqual(SessionCardPresentation.counter(profiles, true, [], includeCompleted: true),
                       SessionChildCounter(label: "2/8", description: "Subagents, 2 active / 8 observed runs", expandable: false))
        XCTAssertEqual(SessionCardPresentation.children(live(#""subagents":{"odd":"text"}"#), true).map { [$0.name, $0.status] }, [["odd", "Unknown"]])
    }

    func testChildCountMatchesEveryDesktopBranch() {
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3,"subagent_total_count":10"#, false), ["3", "3 active"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0,"subagent_total_count":175"#, false), ["", ""])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":false,"subagent_active_count":0,"subagent_total_count":175"#, false), ["0+", "0 confirmed active; some states unknown"])
        XCTAssertEqual(count(#""subagent_source":"hook_profiles","subagent_counts_complete":false,"subagent_active_count":3,"subagent_total_count":3"#, false), ["?", "Observed runs: incomplete counts"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":1,"subagent_total_count":3"#, true), ["1/3", "1 active / 3 total"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":false,"subagent_active_count":2,"subagent_total_count":5"#, true), ["2+/5", "2 confirmed active / 5 observed; some states unknown"])
        XCTAssertEqual(count(#""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":8"#, true), ["2/8", "2 active / 8 observed runs"])
        XCTAssertEqual(count(#""subagent_source":"hook_profiles","subagent_counts_complete":true,"subagent_active_count":2,"subagent_total_count":null"#, true), ["?", "Observed runs: count unavailable"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3"#, true), ["3/?", "3 active / total unknown"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0"#, true), ["", ""])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_count":4,"subagent_active_count":2,"subagent_total_count":9"#, true), ["4/9", "4 active / 9 total"])
        XCTAssertEqual(count(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_count":5,"subagent_active_count":null"#, false), ["5", "5 active"])
        let fields = #""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":3,"subagent_total_count":10"#
        XCTAssertEqual(count(fields, true, online: false), ["", ""])
        XCTAssertEqual(count(#""subagent_source":"unavailable","subagent_active_count":3,"subagent_total_count":10"#, true), ["", ""])
        XCTAssertEqual(SessionCardPresentation.childCount(session(#"{"state":"archived","activity":"busy",\#(fields)}"#, archive: "archive"), true, includeCompleted: true), "")
        XCTAssertEqual(SessionCardPresentation.childCount(session(#"{"activity":"idle",\#(fields)}"#), true), "3")
    }

    func testCounterOpensVisibleRowsAndNamesUncountedRowsAgents() {
        let paused = session(#"{"state":"paused","activity":"idle","subagent_source":"hooks","subagent_active_count":1,"subagent_total_count":2,"subagents":{"one":{"state":"working"}}}"#)
        XCTAssertEqual(SessionCardPresentation.counter(paused, true, SessionCardPresentation.children(paused, true)),
                       SessionChildCounter(label: "Agents", description: "Subagents", expandable: true))
        let finished = live(#""subagent_source":"hooks","subagent_counts_complete":true,"subagent_active_count":0,"subagent_total_count":1,"subagents":{"two":{"state":"finished"}}"#)
        XCTAssertNil(SessionCardPresentation.counter(finished, true, SessionCardPresentation.children(finished, true)))
        XCTAssertEqual(SessionCardPresentation.counter(finished, true, SessionCardPresentation.children(finished, true, includeCompleted: true), includeCompleted: true),
                       SessionChildCounter(label: "0/1", description: "Subagents, 0 active / 1 total", expandable: true))
    }

    func testDraftsKeepOnlyTheirExactParentsChildrenVisible() {
        func child(_ agent: String, conversation: String? = nil, text: String = "Continue") -> Draft {
            let parentConversation = conversation ?? parent.conversation
            return Draft(target: parent.with(conversation: "\(parentConversation)/\(agent)", agentId: agent, parentConversation: parentConversation), text: text)
        }
        let file = Attachment(id: "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", name: "notes.txt", mime: "text/plain", bytes: 4, sha256: String(repeating: "a", count: 64))
        let drafts = [child("kept"), child("blank", text: " "), child("other", conversation: "earlier"), Draft(target: parent, text: "Parent draft"),
                      Draft(target: parent.with(conversation: parent.conversation + "/elsewhere", agentId: "elsewhere", parentConversation: parent.conversation, computerId: "arch"), text: "x"),
                      Draft(target: parent.with(conversation: parent.conversation + "/attached", agentId: "attached", parentConversation: parent.conversation), text: "", attachments: [file])]
        XCTAssertEqual(SessionCardPresentation.draftedChildren(parent, drafts), ["kept", "attached"])
    }

    func testOnlyNativeChildIdentitiesOpen() {
        XCTAssertTrue(SessionCardPresentation.openableAgent("a62c99618f031a11b"))
        for id in ["", " ", "main", "a/b", "a\\b", "a\u{0}b", String(repeating: "x", count: 257)] { XCTAssertFalse(SessionCardPresentation.openableAgent(id), id) }
        XCTAssertTrue(SessionCardPresentation.openableAgent(String(repeating: "x", count: 256)))
    }

    // MARK: Session references

    private let referenceTarget = Target(computerId: "11111111-1111-4111-8111-111111111111", session: "claude/project/a & б", run: "run", conversation: "parent")
    private let machineId = "22222222-2222-4222-8222-222222222222"
    private var referenceMachine: Machine {
        Machine(connectionId: referenceTarget.connectionId, id: referenceTarget.computerId, name: "Work computer", online: true, machineId: machineId, nativeHost: "fixture")
    }
    private func reference(_ target: Target? = nil, _ computers: [Machine]? = nil) -> SessionReferenceValue {
        SessionReference.resolve(target ?? referenceTarget, computers ?? [referenceMachine])
    }

    func testReferenceIdentifiersAndEscaping() {
        XCTAssertEqual(SessionReference.identifiers(referenceTarget), ["Session ID: parent"])
        XCTAssertEqual(SessionReference.identifiers(referenceTarget.with(conversation: "")), ["Run ID: run"])
        XCTAssertEqual(SessionReference.identifiers(referenceTarget.with(conversation: "native-child", agentId: "child", parentConversation: "parent")), ["Session ID: native-child", "Agent ID: child"])
        XCTAssertTrue(SessionReference.identifiers(referenceTarget.with(run: "", conversation: "")).isEmpty)
        let uri = reference().uri!
        XCTAssertTrue(uri.hasPrefix("zerus://session?machine=\(machineId)&host=fixture&"))
        XCTAssertTrue(uri.contains("name=claude%2Fproject%2Fa%20%26%20%D0%B1"), uri)
        XCTAssertTrue(uri.contains("conversation=parent&run=run"))
        let child = reference(referenceTarget.with(conversation: "child", archiveId: "archive", agentId: "external:codex:child", parentConversation: "parent")).uri!
        XCTAssertTrue(child.contains("conversation=parent") && child.contains("agent=external%3Acodex%3Achild") && child.contains("archive=archive"))
    }

    func testMissingOrInvalidIdentityCannotBecomeAReference() {
        var badHost = referenceMachine; badHost.nativeHost = "-option"
        let invalid = [reference(referenceTarget.with(computerId: "demo")), reference(nil, [badHost]), reference(referenceTarget.with(run: "", conversation: "")),
                       reference(referenceTarget.with(agentId: "child", parentConversation: "")), reference(referenceTarget.with(session: "claude/project/\n")),
                       reference(referenceTarget.with(session: "unqualified"))]
        for value in invalid { XCTAssertNil(value.uri); XCTAssertFalse(value.reason.isEmpty) }
        for id in ["", referenceTarget.computerId.uppercased().replacingOccurrences(of: "1", with: "A"), "00000000-0000-0000-0000-000000000000", "relay-node"] {
            var machine = referenceMachine; machine.machineId = id
            XCTAssertNil(reference(nil, [machine]).uri, id)
        }
        var alias = referenceMachine; alias.id = "canonical-computer"; alias.serverAliases = [referenceTarget.computerId]
        XCTAssertEqual(reference(nil, [alias]).uri, reference().uri)
        var foreign = alias; foreign.connectionId = "other-workspace"
        XCTAssertNil(reference(nil, [foreign]).uri)
        XCTAssertNil(reference(nil, [alias, referenceMachine]).uri)
        for target in [referenceTarget.with(conversation: "bad\u{0}id"), referenceTarget.with(run: String(repeating: "x", count: 513)),
                       referenceTarget.with(session: "claude/" + String(repeating: "я", count: 257))] {
            XCTAssertNil(reference(target).uri)
        }
    }

    // MARK: Previews

    private let open = "<send_user_message_question_reply>"
    private let close = "</send_user_message_question_reply>"
    private func reply(_ answer: String) -> String { JSON.object(["questionItemId": "synthetic-question", "question": "Choose an approach", "answer": .string(answer)]).compact }
    private func envelope(_ json: String) -> String { open + json + close }

    func testPreviewShowsHumanAnswersAndKeepsLiterals() {
        XCTAssertEqual(SessionPreview.render(envelope(reply("Continue"))), "Answer: Continue")
        XCTAssertEqual(SessionPreview.render(envelope("[\(reply("First")),\(reply("Second"))]")), "Answer: First; Second")
        XCTAssertEqual(SessionPreview.render(envelope(reply(""))), "Answer: (empty answer)")
        XCTAssertEqual(SessionPreview.render(envelope("[\n\(reply("Pretty")),\r\n\(reply("Printed"))\n]")), "Answer: Pretty; Printed")
        XCTAssertEqual(SessionPreview.render(open + "[{\"answer\":\"\",…"), "Question response")
        let prefix = "# Context from my IDE setup:\nOpen files: synthetic.kt\n## My request for Codex:\n"
        XCTAssertEqual(SessionPreview.render(prefix + envelope(reply("Proceed"))), "Answer: Proceed")
        XCTAssertEqual(SessionPreview.render(prefix + "Ordinary request"), "Ordinary request")
        for text in ["Use <button> literally", "<other>example</other>", "Example: \(open){}\(close)", "```xml\n\(open){}\(close)\n```"] {
            XCTAssertEqual(SessionPreview.render(text), text)
        }
        for json in ["{}", "[]", "42", "{\"questionItemId\":\"q\",\"question\":\"Q\",\"answer\":42}", "{'questionItemId':'q','question':'Q','answer':'bad'}",
                     "{\"questionItemId\":\"q\",\"question\":\"Q\",\"answer\":\"bad\",}", "[\(reply("bad")),]", "\(reply("bad")) trailing"] {
            XCTAssertEqual(SessionPreview.render(envelope(json)), envelope(json), json)
        }
    }

    func testClippedPreviewAnswers() {
        XCTAssertEqual(SessionPreview.render(open + "[{\"answer\":\"Continue\",\"question\":\"long…"), "Answer: Continue…")
        XCTAssertEqual(SessionPreview.render(open + "{\"answer\":\"Continue\"}…"), "Answer: Continue…")
        XCTAssertEqual(SessionPreview.render(open + "[{\"answer\":\"caf\\u00e9\",…"), "Answer: café…")
        XCTAssertEqual(SessionPreview.render(open + "[{\"answer\":\"\\/\",…"), "Answer: /…")
        XCTAssertEqual(SessionPreview.render(open + "[{\"question\":\"answer: yes\",\"answer\":\"Continue\",…"), "Question response")
        let unclosed = open + "[{\"answer\":\"Continue\",unfinished"
        XCTAssertEqual(SessionPreview.render(unclosed), unclosed)
        for body in ["[{\"answer\":\"unfinished…", "[{\"answer\":\"bad\\…", "[{\"answer\":42,…", "[{\"answer\":\"bad\" unexpected…"] {
            XCTAssertEqual(SessionPreview.render(open + body), "Question response", body)
        }
    }

    func testBoundedPreviewKeepsWholeScalarsAndParserUsesRenderedText() {
        let rendered = SessionPreview.render(envelope(reply(String(repeating: "😀", count: 500))))
        XCTAssertEqual(rendered.unicodeScalars.count, 240)
        XCTAssertTrue(rendered.hasSuffix("…"))
        let raw = envelope(reply("Proceed carefully"))
        let row: JSONObject = ["name": "session", "run_id": "run", "conversation_id": "conversation", "prompt": .string(raw)]
        let machine: JSONObject = ["id": "computer", "snapshot": ["sessions": [.object(row)]]]
        let connection = Connection(id: "workspace", label: "Test", url: "https://gateway.example", token: "synthetic")
        let item = NativeSessions.sessions(connection, machine).first!
        XCTAssertEqual(item.preview, "Answer: Proceed carefully")
        XCTAssertEqual(item.raw.string("prompt"), raw)
        XCTAssertEqual(item.target, Target(computerId: "computer", session: "session", run: "run", conversation: "conversation", connectionId: "workspace"))
        XCTAssertEqual(SessionFilters.scoped([item], [], [], "Proceed carefully").count, 1)
        XCTAssertTrue(SessionFilters.scoped([item], [], [], "questionItemId").isEmpty)
    }

    func testPendingCountKeepsBlockingRequestsWithoutOptionalQuestions() {
        let optional: JSON = ["question_id": "codex-async:c:0", "question_hash": "h0", "optional": true]
        let olderOptional: JSON = ["question_id": "codex-async:c:1", "question_hash": "h1"]
        let required: JSON = ["question_id": "request", "question_hash": "h2"]
        func row(_ name: String, _ extra: JSONObject) -> JSON {
            var raw: JSONObject = ["name": .string(name), "run_id": "r", "conversation_id": "c", "activity": "busy"]
            for (key, value) in extra { raw[key] = value }
            return .object(raw)
        }
        let machine: JSONObject = ["id": "machine", "snapshot": ["sessions": [
            row("optional", ["mobile_attention": [optional, olderOptional]]),
            row("required", ["mobile_attention": [optional, required]]),
            row("empty", ["mobile_attention": [], "pending_question_count": 2]),
            row("older", ["pending_question_count": 2]),
        ]]]
        let connection = Connection(id: "gateway", label: "Test", url: "https://gateway.example", token: "synthetic")
        XCTAssertEqual(NativeSessions.sessions(connection, machine).map(\.pendingCount), [0, 1, 0, 2])
    }
}
