import Foundation

/// The explicitly labelled read-only preview, ported from Android `Demo` and `preview()`.
public enum DemoCatalog {
    public static let connection = Connection(id: "demo", label: "Design workstation", url: "https://workstation.example", token: "")

    /// Preview transcript: the Android demo exchange plus a reply that exercises Markdown rendering.
    public static let events: [Event] = [
        Event(id: "1", role: "You", text: "Review the API migration and call out any compatibility risks.", time: "10:42", at: 1_700_000_520,
              attachments: [DisplayedAttachment(name: "orders-traffic.csv", mime: "text/csv", bytes: 48_213,
                                                messageId: "00000000-0000-4000-8000-0000000000d1", index: 0)]),
        Event(id: "2", role: "Codex", text: """
            I found **two clients** still using the previous endpoint:

            1. `billing-sync` calls `/v1/orders` every minute.
            2. The dashboard export reads `/v1/orders?format=csv`.

            | Client | Calls per day | Risk |
            | :--- | ---: | :---: |
            | billing-sync | 1,440 | High |
            | dashboard export | 12 | Low |

            The full call log is in [orders-traffic.md](docs/orders-traffic.md:12).

            I can keep a compatibility route while the new API rolls out:

            ```swift
            router.get("/v1/orders") { request in
                try await orders.list(request, version: .legacy)
            }
            ```

            - [x] Map every caller
            - [ ] Announce the removal date
            """, time: "10:43", at: 1_700_000_580),
        Event(id: "3", role: "Codex", text: "Should I keep the previous endpoint available during migration?", time: "10:44", at: 1_700_000_640),
    ]

    public static func make() -> (projects: [ProjectSummary], sessions: [Session]) {
        let machines = [("demo", "Design workstation"), ("demo-laptop", "Preview laptop"), ("demo-build", "Preview build machine with a long display name")]
        let name = Dictionary(uniqueKeysWithValues: machines)
        let key = ProjectKey(connectionId: "demo", swarmId: "demo", id: "zerus")
        let project = ProjectSummary(key: key, name: "Zerus", color: "#67E8CB", folders: [
            ProjectFolder(id: "demo", name: "Zerus", path: "/workspace/zerus", computerId: "demo", computerName: name["demo"]!),
            ProjectFolder(id: "demo-design", name: "Design sandbox", path: "/workspace/zerus/design", computerId: "demo", computerName: name["demo"]!),
            ProjectFolder(id: "demo-long", name: "mobile", path: "/workspace/research/experiments/a-long-folder-name-for-previewing-wrapped-paths/mobile",
                          computerId: "demo", computerName: name["demo"]!),
            ProjectFolder(id: "demo-laptop-main", name: "zerus", path: "/workspace/zerus", computerId: "demo-laptop", computerName: name["demo-laptop"]!),
            ProjectFolder(id: "demo-laptop-mobile", name: "mobile", path: "/workspace/zerus/mobile", computerId: "demo-laptop", computerName: name["demo-laptop"]!),
            ProjectFolder(id: "demo-laptop-test", name: "mobile", path: "/workspace/testing/mobile", computerId: "demo-laptop", computerName: name["demo-laptop"]!),
            ProjectFolder(id: "demo-build-main", name: "zerus", path: "/workspace/build/zerus", computerId: "demo-build", computerName: name["demo-build"]!),
        ], computers: machines)
        let empty = ProjectSummary(key: ProjectKey(connectionId: "demo", swarmId: "demo", id: "empty-preview"), name: "Empty preview project",
                                   color: "#8DA9E8", folders: [], computers: [])
        let rows: [(String, String, String)] = [
            ("API migration", "Needs your input", "Should I keep the previous endpoint available during migration?"),
            ("Dashboard polish", "Working", "Checking spacing and keyboard navigation across the main views."),
            ("Release notes", "Idle", "The changelog is ready for review."),
        ]
        let agents = ["Codex", "Claude Code", "Kimi Code"]
        let branches = ["api-migration", "ui-polish", "main"]
        let sessions = rows.enumerated().map { index, row -> Session in
            var raw: JSONObject = ["state": "running", "process_state": "running", "tracked": true, "tag": .string(row.0),
                                   "activity": index < 2 ? "busy" : "idle", "phase": index == 0 ? "input" : (index == 1 ? "working" : "idle"),
                                   "git_metadata_state": "ok", "git_branch": .string(branches[index]), "cwd": "/workspace/zerus"]
            if index == 1 {
                raw["git_worktree"] = true
                raw["git_worktree_name"] = "zerus-ui-polish"
                raw["git_common_dir"] = "/workspace/zerus/.git"
                raw["subagent_source"] = "hooks"
                raw["subagent_count"] = 1
                raw["subagent_total_count"] = 1
                raw["subagent_counts_complete"] = true
                raw["subagents"] = ["reviewer": ["label": "Accessibility review", "state": "working", "task": "Checking focus order and contrast in the session list."]]
            }
            return Session(target: Target(computerId: "demo", session: "example-\(index)", run: "demo-run", conversation: "demo-conversation"),
                           title: row.0, agent: agents[index], status: row.1, project: "Zerus", preview: row.2,
                           pendingCount: index == 0 ? 1 : 0, raw: raw, projectKey: key)
        }
        return ([project, empty], sessions)
    }
}
