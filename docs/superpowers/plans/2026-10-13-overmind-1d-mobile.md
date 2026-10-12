# Overmind 1d — Mobile Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full parity with the desktop on the phone: read boards and cards, act on tasks, add
tasks from the composer, send a task to a session, and get a **Decision** alert for Your Call.

**Architecture:** Two new relay operations, `overmind_read` (a read) and `overmind_write` (a
mutation with a durable `request_id`), both **gateway-scoped** (no session; served by the
gateway's connector). The relay validates only their envelope (action allowlist, sizes,
identity); `hgs overmind` on the gateway validates fields and gates. The connector runs
`hgs overmind <command> --json [--device <label>]` with the request on stdin and publishes a
small `mobile_overmind` digest in the gateway snapshot; the relay derives `decision` alerts from
it. Android adds Tasks screens, a task card, composer and session-sheet entry points, an encrypted
board cache and a write outbox with explicit retry.

**Tech Stack:** Rust relay (`services/mobile/relay`, Rust 1.85, SQLite/PostgreSQL), Python
reference relay and connector (`services/mobile/zerus_mobile`), `src/mobile_peers.rs` and
`src/overmind/` in `hgs`, Android (Kotlin, Jetpack Compose, `org.json`, JUnit4 JVM tests).

**Spec:** `docs/superpowers/specs/2026-10-11-overmind-core-design.md`, Part 7 (Mobile client),
Part 9 (rollout). Plans 1a–1c and the 1b follow-ups are on the branch; 1c adds `decisions`,
`projects` and the extended `board`.

## Decisions this plan takes (record them in the spec in Task 7)

1. **Gateway scope** is a new relay operation class `GATEWAY` next to `COMPUTER`: `session` must be
   empty and the request is routed to the gateway's own connector (never to a peer route); the
   connector refuses a gateway op that carries a peer `gateway_route`.
2. **`overmind_read` has its own retention class** like `attachment_read` (it is not added to the
   persisted `READ_SQL` predicate, so no index migration).
3. **Read actions:** `projects`, `board` (with `since`), `task`, `decisions`, `attachment`
   (`{task, sha256, offset, length ≤ 524288}` → `{size, data_base64}`). The spec's `sessions`
   action is dropped: the phone already knows every session and its state from the snapshots.
4. **Write actions:** `create`, `edit`, `move`, `reorder`, `pin`, `unpin`, `label`, `link`,
   `comment`, `send`, `review-send`, `rework-send`, `answer`, `resolve-delivery`, `done`, `reopen`,
   `delete`. The envelope `request_id` is the Overmind `request_id` (and the task `uuid` for
   `create`), so a retry never duplicates.
5. **Device label:** `hgs overmind ... --device <label>` sets `actor.device` for people (refused
   for agents); the phone sends its own label in the payload, the connector passes it as the flag;
   it is informational, not identity.
6. **Attachments for the phone** are read by the connector from the path `hgs overmind attachment`
   returns (it fetches from other machines when needed), in ranges of at most 512 KiB.
7. **Rollout:** hgs + connector first, then Android, then the relay (all PostgreSQL workers
   together), as the relay README requires; each side tolerates older neighbours through the
   capability lists.

## Global Constraints

- Relay: Rust 1.85 (`cargo +1.85.0 fmt --check`, `cargo +1.85.0 clippy --locked --all-targets -- -D warnings`, `cargo +1.85.0 test --locked` in `services/mobile/relay`); API v1 stays additive; schema version stays 1; new columns nullable; the Python reference relay changes in parallel and stays a strict compatibility oracle; the relay README's lock order and deployment notes hold.
- hgs: Rust 1.85, no new crates, 0 warnings; `src/overmind/` Unix-only.
- Connector: compact Python like the surrounding code; tests run with `PYTHONPATH=services/mobile .deps/mobile-venv/bin/python -m unittest discover -s services/mobile/tests` (create the venv in the worktree with `python3 -m venv .deps/mobile-venv && .deps/mobile-venv/bin/pip install ./services/mobile`).
- Android: Kotlin + Compose + `org.json`, hardcoded English strings like the rest of the app, compact style; JVM tests only; run from `mobile/android` with `JAVA_HOME=$WORKTREE/.deps/android/jdk ANDROID_HOME=$WORKTREE/.deps/android/sdk ./gradlew testDebugUnitTest lintDebug` (`$WORKTREE` = /home/n-prudnikov/w/zerus-overmind).
- Limits: results ≤ 1 MiB; reads paginated; attachment ranges ≤ 512 KiB; writes ≤ 29 MiB (images inline base64 as in `send`); relay queue TTL 120 s.
- Tests use synthetic fixtures, temporary SQLite files and fake `hgs` scripts only; never the production relay, live agents, tmux or DeepSeek hosts. The PostgreSQL contract tests stay `#[ignore]` locally unless a disposable PostgreSQL is available; note that in reports.
- Commit after each task on branch `overmind` with `git commit --only <own paths>`; plain sentence messages; no Claude attribution; `python3 scripts/ci/check-source.py` passes.

## Review Focus

- A phone retry of a write after a lost connection never creates a second task or a second delivery. Tests in Task 3 (`test_overmind_write_replays_by_request_id`) and Task 5 (`OvermindOutboxTest`).
- An older relay or connector without Overmind: the phone hides Tasks instead of failing. Test in Task 5 (`tasksHiddenWithoutCapabilities`).
- A `decision` alert resolves when the task leaves Your Call or its question changes, and older apps treat it as a refresh. Tests in Task 2 (`decision_alerts_follow_the_digest`) and Task 5.
- A gateway op that reaches a peer route is refused, never run on another machine's connector. Test in Task 3.
- Disconnecting a workspace purges its cached boards and cards. Test in Task 5.

---

### Task 1: hgs — device label, help marker, peer allowlist

**Files:** `src/overmind/cli.rs`, `src/overmind/model.rs` (if needed), `src/cli.rs` (help), `src/mobile_peers.rs`, tests.

- [ ] `hgs overmind <command> --json --device <label>`: the label (1–64 printable characters) sets `actor.device` for a person; inside a session environment it is refused. Journal entries and events carry it.
- [ ] Help marker: add a usage line containing `overmind <command> --json (mobile ABI overmind-v1)` to `hgs --help` (src/cli.rs), so the connector can probe it.
- [ ] `src/mobile_peers.rs` `validate`: allow `["overmind", <cmd>, "--json"]` and `["overmind", <cmd>, "--json", "--device", <label>]` for the read and write commands of Decisions 3 and 4 (plus `attachment`, `decisions`, `projects`), payload an object; add accept and deny cases to its tests (unknown command, extra args, agent-only commands like `advance`).
- [ ] Commit "Accept a device label and the mobile overmind shapes in hgs".

### Task 2: Relay (Rust) — operations and decision alerts

**Files:** `services/mobile/relay/src/protocol.rs`, `registry.rs`, `alerts.rs`, `store/*.rs`, `http.rs` as needed, `tests/contracts.rs`, `tests/alerts.rs`, README.

- [ ] `GATEWAY` operations `overmind_read` and `overmind_write` in `OPERATIONS`; `overmind_read` in `READS` with its own retention class (Decision 2). Envelope validation: `session` empty; payload fields exactly `{action, request, device?}` for reads and `{request_id, action, request, device?}` for writes (`request_id` equals the envelope's); `action` in the allowlists of Decisions 3/4; `request` an object; `device` ≤ 64 characters; read payload ≤ 64 KiB, write ≤ the request maximum; reads of `attachment` validate `offset`/`length` like `attachment_read`.
- [ ] Routing: gateway ops are submitted to the gateway node only (never a peer route), and only when the gateway snapshot's `mobile_capabilities.operations` lists them.
- [ ] `decision` alerts: `event_changes` reads `mobile_overmind.decisions` of the gateway snapshot (`[{task, project, title, signature}]`, at most 50); a new entry or a changed `signature` records a candidate with kind `decision` and slot `overmind:<task uuid>`; `live()`/`resolved()` look the slot up in `mobile_overmind.decisions` (resolved when absent or the signature changed); presence rules as for other kinds.
- [ ] Tests: envelope accept/deny, routing to the gateway only, capability gating, read retention, `decision_alerts_follow_the_digest` (raise, resolve on leave, re-raise on signature change). PostgreSQL contract test updated but run only when a database URL is available.
- [ ] README: operations, limits, rollout note.
- [ ] Commit "Add gateway-scoped Overmind operations and decision alerts to the relay".

### Task 3: Python reference relay and connector

**Files:** `services/mobile/zerus_mobile/{context.py,server.py,store.py,connector.py}` (and helpers), `services/mobile/tests/*`.

- [ ] Reference relay: the same operations, classes, validation and decision alerts as Task 2 (keep `tests/test_rust_relay.py` parity green).
- [ ] Connector: `overmind_supported` from the `--help` marker; advertise the two ops only on the gateway (`fleet` present) and only when supported; handlers run `native(["overmind", <command>, "--json", ("--device", label)], request, failure_json=True)` so refusals (`{"ok": false, "error": ...}` with exit 1) reach the phone as results; `attachment` reads `hgs overmind attachment` for the path and returns the requested range (≤ 512 KiB) base64; a gateway op with a peer `gateway_route` is refused.
- [ ] Snapshot: `mobile_overmind = {schema: 1, rev, projects: {<id>: {<role>: count}}, decisions: [...]}` from `hgs overmind projects` and `decisions`, refreshed at most every 15 s with a stale fallback, gateway only, small (≤ 64 KiB).
- [ ] Tests: `test_overmind_write_replays_by_request_id`, refusals pass through as results, peer-route refusal, digest only on the gateway, attachment ranges.
- [ ] Commit "Serve Overmind reads and writes from the gateway connector".

### Task 4: Android — data layer

**Files:** `mobile/android/app/src/main/java/app/zerus/mobile/` (new `Overmind*.kt` files; `RelayApi.kt`, `ZerusViewModel.kt`, `MessageState.kt`, `PrivateStore.kt`, `NotificationCatalog.kt`, `NotificationPolicy.kt`, `NotificationSettings.kt`), tests under `app/src/test/...`.

- [ ] `RelayApi.submitGateway(connection, computerId, operation, payload, id)` (no session) next to `submit`.
- [ ] `OvermindRepository`: reads (`projects`, `board` with `since`, `task`, `decisions`, attachment ranges) with an encrypted cache (`EncryptedConversationFiles` instance with its own root and Keystore alias, existing bounds), purged in `ZerusViewModel.disconnect`; writes through an outbox in `MessageState` (`OvermindWrite(requestId, action, request, status, error)`, statuses like `OutgoingMessage`), persisted durably before the network, settled from the receipt; never auto-retried; a retry is explicit and reuses the `request_id`; `create` uses the request UUID as the task uuid.
- [ ] Capabilities: Tasks are available only when the relay's operations and the gateway machine's operations both list the ops (`tasksHiddenWithoutCapabilities`).
- [ ] Notifications: `decision` in the alert kinds, `SessionAlertKind.Decision`, a preference toggle in `NotificationTypeSettings`, a card whose tap opens the task.
- [ ] `mobile_overmind` digest parser (like `AccountSnapshots`) for project badges and decisions.
- [ ] JVM tests: repository with MockWebServer, outbox replay and settlement (`OvermindOutboxTest`), cache purge on disconnect, digest parser, decision notification policy.
- [ ] Commit "Read and write Overmind tasks from the Android app".

### Task 5: Android — screens

**Files:** new `mobile/android/app/src/main/java/app/zerus/mobile/Overmind*Ui.kt`, `MainActivity.kt`, `SessionActionsMenu.kt`, composer code, tests for presentation logic.

- [ ] Projects → project → **Tasks**: stages as horizontally swiped columns (one per screen); cards with ref, title, labels, session tag, markers; long-press drag to reorder inside a column (`reorder`); "Move to…" between stages (`move`); Your Call strip.
- [ ] Full-screen task card: prompt, images (ranges through the repository), journal (commonmark), all actions of the desktop card panel with the same confirmations (`confirm` codes) and refusals shown as text.
- [ ] **Add as task** in the session composer (Queue of this session, pinned; overflow: project pool, Backlog); **Send a task here** in the session long-press sheet (choose a Queue task of the session's project); queue and Your Call badges on projects and sessions.
- [ ] JVM tests for the presentation logic (column building from a workflow, badge counts, confirmation flow, Add as task payload).
- [ ] Commit "Add Overmind task screens to the Android app".

### Task 6: End-to-end

**Files:** `services/mobile/tests/test_end_to_end.py` or a new `test_overmind_end_to_end.py`.

- [ ] With the fixture relay (Rust binary when available, else the reference), a connector and a fake or real isolated `hgs`: create a task through `overmind_write`, read it through `overmind_read`, send it to a fake session, see the decision digest and alert appear and resolve.
- [ ] Commit "Test Overmind from the phone end to end".

### Task 7: Documentation and spec

- [ ] `docs/mobile-architecture.md` (the two operations, gateway scope, digest, decision alerts, limits), `services/mobile/relay/README.md` (rollout), docs/reference.md (`--device`), spec Part 7 with Decisions 1–7.
- [ ] Commit "Document Overmind on the phone".
