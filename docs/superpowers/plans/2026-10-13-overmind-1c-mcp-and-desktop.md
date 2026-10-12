# Overmind 1c — MCP and Desktop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents reach Overmind through an MCP server attached at launch, and people use
Overmind from the desktop: a project-level **Tasks** tab with a kanban board and a card panel,
**Add as task** in the composer, queue badges and Your Call notifications.

**Architecture:** Rust: `hgs overmind mcp` (stdio JSON-RPC 2.0, newline-delimited) calls the
same agent functions as the CLI; launch code injects the MCP server into Claude Code and Codex
at spawn time (never into saved resume recipes); a `decisions` command lists Your Call tasks
across projects. Qt: `HgsClient::requestOvermind` runs `hgs overmind <command> --json` with a
JSON request on stdin; an `OvermindModel` caches boards per project and polls the cheap `rev`
command; `TasksView` (board + card panel) is a new tab in `SessionsWindow`; the composer gains an
**Add as task** split button and a **Tasks N** chip; `TrayAgent` raises Your Call notifications.

**Tech Stack:** Rust 2021; Qt 6 Widgets (C++17), QtTest with `FakeCli` fake `hgs` scripts;
Python `unittest` for CLI and launch tests.

**Spec:** `docs/superpowers/specs/2026-10-11-overmind-core-design.md` — Part 4 (MCP,
Templates), Part 6 (Desktop GUI), Part 8. Plans 1a, 1b and the 1b follow-ups are on the branch.

## Decisions this plan takes (record them in the spec in Task 8)

1. **The desktop reads the local replica** with `hgs overmind ...` (no `@host`): every machine
   holds a full replica, and `send` routes to the session's host by itself. A Windows console,
   which has no local Overmind, uses `@host` of the machine of the selected session.
2. **Change detection is a cheap `rev` poll** (`hgs overmind rev`): every 2 s while the Tasks
   tab is visible, every 10 s otherwise; a changed cursor reloads the affected boards. No
   `QFileSystemWatcher` (it cannot watch other machines and `rev` is cheap since the store opens
   without the write lock).
3. **MCP config is inline**: Claude Code gets `--mcp-config '<json>'` and Codex gets
   `-c mcp_servers.overmind.command=...` / `-c mcp_servers.overmind.args=[...]`, appended to the
   spawned argv only (like `accounts::apply_permissions`), never to the saved recipe. The run
   record keeps `overmind_mcp: true|false` so templates name MCP tools or CLI commands.
4. **The setting** is the machine-local config key `HGS_OVERMIND_MCP` (`1` default, `0` off) in
   `~/.config/hgs/config` (and the environment), following `HGS_TAB`.
5. **Kimi and DeepSeek fall back to the CLI** in 1c (no MCP injection); templates name the CLI.

## Global Constraints

- Rust: minimum 1.85, edition 2021, no let-chains, no new crates or features; `cargo build --locked`; 0 warnings. `src/overmind/` and every call into it are Unix-only (`#[cfg(unix)]` outside it); Windows CI runs `cargo +1.85.0 clippy --all-targets -- -D warnings`.
- Qt: follow the existing code (Qt 6 Widgets, `tr()` for every UI string, `&&` for a literal ampersand in tab text, colors from `SessionsWindow::applyTheme()` tokens, icons from `workspaceIcon`). No new third-party dependencies.
- Agent-facing text is English; journal markers are ASCII and frozen. UI text is English with `tr()`.
- Never restart or kill running agents, tmux servers or DeepSeek hosts; running sessions never gain MCP retroactively. Launch argument changes need isolated real-agent contract coverage for the reviewed agent versions (the keyless pinned Codex matrix in `scripts/ci/agent-compatibility.json`).
- Tests never touch live state or the live tmux server: Python fixtures use a temporary `HOME` with shims in `$HOME/.local/bin`, no `HGS_SSH`/`TMUX`/`HGS_SESSION`/`HGS_RUN_ID`; Qt tests use `FakeCli` scripts in a `QTemporaryDir` and `QT_QPA_PLATFORM=offscreen`.
- Build the tray in the worktree with `cmake -S tray -B tray/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DQt6_DIR=$HOME/w/zerus/.deps/Qt/6.11.3/gcc_64/lib/cmake/Qt6 -DKF6StatusNotifierItem_DIR=$HOME/w/zerus/.deps/kf6/lib/x86_64-linux-gnu/cmake/KF6StatusNotifierItem -DKF6WindowSystem_DIR=$HOME/w/zerus/.deps/kf6/lib/x86_64-linux-gnu/cmake/KF6WindowSystem` (`tray/build/` is ignored by git), then `cmake --build tray/build -j8` and `ctest --test-dir tray/build -R <name> --output-on-failure`. Never start the GUI against the live state; never restart the user's running GUI.
- Commit after each task on branch `overmind` with `git commit --only <own paths>`; plain sentence messages; no Claude attribution; `python3 scripts/ci/check-source.py` passes.

## Review Focus

- An MCP client that sends malformed JSON, an unknown method, or a tool call with bad arguments gets a JSON-RPC error or an `isError` result, never a crash or a hang. Test in Task 1 (`malformed_and_unknown_requests_get_errors`).
- A resumed or forked session gets the MCP flag again, and a saved resume recipe never contains it; turning the setting off removes it from the next launch. Test in Task 2 (`test_mcp_flags_on_start_resume_and_fork_never_in_the_recipe`).
- A board with hundreds of tasks stays responsive: one reload per `rev` change, no reload per card. Test in Task 4 (`reloadsOnlyWhenRevChanges`).
- A refused drag (a move the workflow does not allow) leaves the card where it was and shows the gate's reason. Test in Task 5 (`refusedDropShowsTheReason`).
- **Add as task** keeps the draft when creation fails and clears it only on success. Test in Task 7 (`addAsTaskKeepsTheDraftOnFailure`).

---

### Task 1: MCP server `hgs overmind mcp`

**Files:** Create `src/overmind/mcp.rs`; modify `src/overmind/mod.rs`, `src/overmind/cli.rs` (route `mcp` before reading stdin as a request), tests in `mcp.rs` and `tests/test_overmind.py`.

**Interfaces:** `pub fn serve(config: &Config, input: impl BufRead, output: impl Write) -> anyhow::Result<()>` (testable with in-memory streams); CLI `hgs overmind mcp` calls it with stdin/stdout.

- [ ] Protocol: one JSON-RPC 2.0 message per line in, one per line out (no Content-Length). Handle `initialize` (answer `protocolVersion`: the client's requested version when it is one of `2025-06-18`, `2025-03-26`, `2024-11-05`, else `2025-06-18`; `capabilities: {"tools": {}}`; `serverInfo: {"name": "overmind", "version": <hgs version>}`; `instructions`: one paragraph telling the agent what Overmind is and that Done is for people), `notifications/initialized` and any other notification (no answer), `ping` (`{}`), `tools/list`, `tools/call`. Unknown method → error `-32601`; unparsable line → `-32700` with `id: null`; a request without `jsonrpc: "2.0"` or a bad `params` → `-32600`/`-32602`. EOF ends the server cleanly.
- [ ] Tools (names from the spec): `my_tasks` → `agent::mine`; `get_task` {task} → `commands::show`; `comment` {task, text}; `advance` {task, to, spec?|worklog?, evidence?, root_cause?}; `call_human` {task, question}; `return_task` {task, reason}; `decompose` {task, subtasks, ordered?}; `file_task` {prompt, breaks, title?, related?, project?}; `attach_file` {task, path, note?} (reads a local image file the agent names, at most 10 MiB, MIME from the extension png/jpg/jpeg/gif/webp, then `agent::attach`); `review_task` {task, verdict, report}; `download_attachment` {task, sha256} → `remote::attachment` (returns the local path, never base64). Each has a JSON Schema `inputSchema` with `required` fields and a one-line English `description`.
- [ ] Identity: compute the caller with the same check as the CLI (`identity::caller`) on every call (a session may be renamed while the server runs). A refusal (`Refusal`) becomes a `tools/call` result with `isError: true` and the refusal JSON as text; other errors become `isError: true` with the message. A successful result is `{content: [{type: "text", text: <JSON result>}]}`.
- [ ] Tests (Rust, in-memory streams, a temp store and an agent `Ctx` via a test hook that bypasses the live-session identity check): initialize → tools/list lists the 11 tools; a `tools/call` `my_tasks` works; Done is unreachable through every tool (`advance` to Done, `review_task` etc. refuse); `malformed_and_unknown_requests_get_errors`; a notification produces no output. Python: `hgs overmind mcp` with HGS_SESSION unset answers `initialize` and `tools/list` and refuses tool calls with `isError` (`agent_only`/identity).
- [ ] Commit "Serve Overmind's agent commands over MCP".

### Task 2: Attach the MCP server at launch

**Files:** `src/config.rs` (key `HGS_OVERMIND_MCP`), `src/accounts.rs` (new `apply_overmind_mcp(agent, enabled, executable, argv)` next to `apply_permissions`), `src/state/lifecycle.rs` (call it at the spawn-time hook next to `apply_permissions` for `base` and `first`; record `overmind_mcp` in the run record), `src/overmind/templates.rs` / `deliver.rs` (`{tools}` = MCP or CLI by the target session's record), tests (Rust unit tests for argv; Python launch tests in the style of `tests/test_account_lifecycle.py` `test_permission_modes_start_resume_and_fork_without_baking_in_flags`), `scripts/ci/agent-compatibility.py` (+ a native test class) and `docs/ci-and-aur.md`.

- [ ] Claude: insert `--mcp-config` and the inline JSON `{"mcpServers":{"overmind":{"type":"stdio","command":"<hgs executable>","args":["overmind","mcp"]}}}` before any `--`. Codex: insert `-c`, `mcp_servers.overmind.command="<hgs executable>"`, `-c`, `mcp_servers.overmind.args=["overmind","mcp"]` (TOML values, quoted correctly for paths with spaces or quotes). Kimi, DeepSeek: nothing. The executable is `storage::runner_executable()` (honours `HGS_EXECUTABLE`). Off when `HGS_OVERMIND_MCP=0` (environment wins over the config file) or when the platform has no Overmind.
- [ ] The run record stores `overmind_mcp: true` when the flags were added; `deliver.rs` renders `{tools}` as `MCP "overmind" (tools my_tasks, advance, call_human, ...) or \`hgs overmind --help\`` for such sessions and as the CLI hint otherwise.
- [ ] Tests: `test_mcp_flags_on_start_resume_and_fork_never_in_the_recipe` (fake claude and codex record argv in `$HOME/argv.jsonl`; start, resume, fork, fresh all carry the flags; the saved recipe and `recipes::resume_argv` never contain them; `HGS_OVERMIND_MCP=0` removes them; kimi gets none).
- [ ] Contract: add a native test class (for example `tests/test_overmind_mcp_native.py` `NativeCodexMcp`) that runs the pinned Codex binary keyless with the `-c mcp_servers.overmind...` overrides and an isolated `CODEX_HOME`, and checks Codex accepts them (e.g. `codex mcp list` or `codex mcp get overmind --json` shows the server with our command); register it in `scripts/ci/agent-compatibility.py`'s native runner. It skips locally when no pinned binary is provided, like the existing native classes. Document in `docs/ci-and-aur.md` (native compatibility) and note that Claude Code has no pinned version in CI.
- [ ] Commit "Attach the Overmind MCP server when Zerus launches Claude Code or Codex".

### Task 3: Commands the desktop and the phone need

**Files:** `src/overmind/commands.rs`, `src/overmind/cli.rs`, tests.

- [ ] `decisions --json {}` → `{ok, cursor, decisions: [{task, ref, project, title (≤120 chars), stage, question (last [needs-human] text, ≤500 chars), assignee, signature}]}` for every non-deleted task whose stage role is `needs_human`, at most 50, oldest first; `signature` = SHA-256 hex of `uuid + head`. (The phone's `mobile_overmind.decisions` digest in 1d reuses it.)
- [ ] `board` result also carries the project's `workflow` (stages with roles) and, per task, `queue_of` (the pinned session) and `markers` (`working` when the assignee is set and the role is active, `reviewed`, `needs_work`, `conflict`, `uncertain`, `orphaned`), so the GUI needs one call per project.
- [ ] `projects --json {}` → projects that have tasks, with counts per role.
- [ ] Tests (Rust) and help text; commit "Add the Overmind commands the desktop needs".

### Task 4: Qt data layer

**Files:** `tray/src/HgsClient.{h,cpp}` (`requestOvermind`), new `tray/src/OvermindModel.{h,cpp}`, `tray/CMakeLists.txt`, new `tray/tests/test_overmindmodel.cpp` (+ `tests/CMakeLists.txt`).

- [ ] `quint64 HgsClient::requestOvermind(const QString &host, const QStringList &args, const QJsonObject &input = {})` runs `[@host] overmind <args> --json` through `runHgs` with the request on stdin (compact JSON), timeouts 10 s local / 30 s remote; signals `overmindReady(quint64 request, const QString &host, const QJsonObject &reply)` and `overmindFailed(quint64 request, const QString &host, const QJsonObject &refusal, const QString &detail)` — a refusal printed as JSON on stdout with exit 1 must reach `overmindFailed` with the parsed `error` object (extend `runHgs` or use a variant that hands stdout to the failure path).
- [ ] `OvermindModel` (QObject): `setHost(QString)` (empty = local), `watchProject(QString id)`, `board(QString project) -> const Board&` (stages, tasks, markers), `decisions()`, `queueCount(machine, session)`, `write(command, QJsonObject request, callback)` that adds a `request_id` (QUuid) and reloads on success; polls `rev` per Decision 2 (`setActive(bool)` switches 2 s/10 s) and reloads only boards of watched projects when the cursor changes; signals `boardChanged(project)`, `decisionsChanged()`, `writeFailed(command, refusal)`.
- [ ] Tests with a `FakeCli` python script that serves `rev`, `board`, `decisions` from files and records calls: `reloadsOnlyWhenRevChanges`, refusals reach `writeFailed` with `code`/`field`/`state`, a write carries a `request_id`.
- [ ] Commit "Read and write Overmind boards from the desktop".

### Task 5: The Tasks tab — board

**Files:** new `tray/src/TasksView.{h,cpp}` (and helpers such as `TaskCardDelegate` if needed), `tray/src/SessionsWindow.{h,cpp}`, `tray/CMakeLists.txt`, `tray/tests/test_tasksview.cpp`, `tray/tests/CMakeLists.txt` (also add the new sources to the `test_sessionswindow` target).

- [ ] A main tab **Tasks** after Activity, Terminal and Processes (mind the hard-coded Native UI index). The tab belongs to a project: a project selector (accessible groups of `SessionOrganization`, icon and color like `NewSessionDialog`) persisted in `workspace/tasksProject`; on first use it opens the selected session's project. Switching sessions never changes the board, its scroll, filters or open card.
- [ ] Columns follow the workflow; Done and Icebox start collapsed; Your Call is a warning strip above the board (click opens the card). Cards show `ref`, title, labels, session tag and markers ▶ working, ✓ reviewed, ✗ needs work, ⚠ conflict or unconfirmed delivery, orphaned. When the selected session belongs to the shown project, its tasks are highlighted and a strip shows "This session's queue: N · Send next" (Send next = `send` of the first pinned Queue task, else the first pool task).
- [ ] Drag between columns runs `move` (people's moves only); a refused drop leaves the card and shows the refusal message inline; drag inside a column runs `reorder` with `before`.
- [ ] Theme: follow `applyTheme` tokens and `setTheme(bool dark)`.
- [ ] Tests: columns from a workflow, Done/Icebox collapsed, highlight of the selected session, `refusedDropShowsTheReason`, project persistence across sessions.
- [ ] Commit "Show Overmind tasks as a board in a Tasks tab".

### Task 6: The card panel and actions

**Files:** new `tray/src/TaskPanel.{h,cpp}`, `TasksView`, tests.

- [ ] The card panel opens inside the tab on the right: editable prompt (reuse the composer editor component if it can be embedded, else a plain-text editor with the same limits), images (thumbnails; fetched through `attachment`), labels, priority, pinning, relations; the journal rendered with `MarkdownHtml`. Edits run `edit`, `label`, `pin`/`unpin`, `link`.
- [ ] Actions: **Send to session ▾** (sessions of the project from the window's fleet with idle/working hints; `send` with `{machine, name}`), **Send for review ▾** (`review-send`), **Send rework** (`rework-send`, shown when `review-failed`), **Answer** (Your Call; `answer`), **Move to…**, **Done**, **Delete** (confirmation), **It was delivered / Not delivered** when `delivery_uncertain` (`resolve-delivery`).
- [ ] Confirmable refusals (`field == "confirm"`: `agent_busy`, `wip_exceeded`, `predecessors_open`, `pinned_elsewhere`) show the message with **Send anyway**, which repeats the request with the same `request_id` and `confirm` extended by that code; other refusals show their message.
- [ ] Tests with FakeCli: each action sends the right command and request; a confirmable refusal round-trips with the code added and the same `request_id`; Delete asks first.
- [ ] Commit "Act on Overmind tasks from the card panel".

### Task 7: Composer, chip, badges, inspector

**Files:** `tray/src/MessageComposer.{h,cpp}`, `tray/src/ComposerToolbar.{h,cpp}`, `tray/src/SessionsWindow.cpp`, `tray/src/SessionCardDelegate.h`, `tray/src/SessionList.h`, tests (`test_messagecomposer`, `test_sessionswindow` shards as fitting).

- [ ] **Add as task** split button next to Send and Alt+Enter: default creates the task in Queue of this session, pinned to it (project = the session's group); the arrow menu offers "To project pool" (Queue, unpinned) and "To Backlog". The request carries the draft's text and images (same attachment JSON as `hgs send`). Success clears the composer and flashes the chip; failure keeps the draft and shows the reason (`addAsTaskKeepsTheDraftOnFailure`).
- [ ] **Tasks N** chip in the composer toolbar (new `ComposerToolbar::Slot::Tasks`; this session's queue count; Danger tone when one of its tasks is in Your Call). Clicking it is the one action that switches the board: it opens the Tasks tab on that session's project with its tasks highlighted.
- [ ] Session rows show a queue badge (new item role).
- [ ] The inspector tab "Tasks && agents" becomes "Plan && agents" (update tests that name it).
- [ ] Commit "Add tasks from the composer and show session queues".

### Task 8: Your Call notifications, documentation and spec

**Files:** `tray/src/TrayAgent.cpp` (and `AttentionTracker` if it fits), docs/reference.md, the spec, `docs/ci-and-aur.md` if not done in Task 2.

- [ ] `TrayAgent` polls `decisions` (through `OvermindModel` or directly, 10 s, and after a `rev` change) and raises the usual attention notification for each new decision (`signature` edge detection, persisted like `attention/observed`); clicking it opens the Tasks tab on the task's card.
- [ ] Reference: MCP (tools, attachment at launch, `HGS_OVERMIND_MCP`), the desktop Tasks tab, Add as task, `decisions`/`projects`. Spec: Decisions 1–5 of this plan.
- [ ] Commit "Notify Your Call decisions and document Overmind on the desktop".
