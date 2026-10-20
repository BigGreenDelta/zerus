# Phone Task Actions through the Relay Implementation Plan (5c1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A phone can ask a session's computer, through relay → Rust connector → hgs, to send a task (`task_send`) or cancel it (`task_cancel`), and read a project's Done list in pages (`task_done`). This is the server side of 5c; the Android Tasks screen is 5c2.

**Architecture:**
- **`zerus-replica`**:
  - caches each task's `stage_at` beside its stage;
  - answers `ProjectStore::done_page`: Done tasks newest first, a page after a cursor, with the total and the number accepted since a given time. Only the page's documents are loaded.
- **hgs** adds three stdin-JSON commands that refuse in JSON, as `exchange` does, all admitted on the mobile peer allowlist:
  - `task mobile-send --json` and `task mobile-cancel --json` reuse the existing send and cancel paths (`change`) and answer a compact result, never the task view;
  - `task mobile-done --json` answers the page.
- **The relay** adds the three operations to the `TASKS` family:
  - `task_send` and `task_cancel` are mutations: durable identity, mutation quota and retention;
  - `task_done` is a short read with two-minute retention and its own index.
- **The connector**:
  - advertises the three when hgs's help names the marker;
  - runs the sends and cancels as mutations: a JSON refusal before anything happened is failed, an unconfirmed outcome is uncertain;
  - keeps `task_done` as a short read.

**Tech Stack:** Rust 1.99.1 (`zerus-replica`, hgs, relay, connector); Python `unittest` (hgs, black-box, end to end); PostgreSQL for the relay suite.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`, "Phone" ("Send now and Cancel from the phone", "Done on the phone") and "Actions with side effects". The relay contract is in `services/mobile/relay/README.md`.

## Global Constraints

- The spec says, verbatim:
  - "Both are computer-scoped mutations of mobile API v1 with the relay's durable request identity: the connector runs each identity once (`hgs task mobile-send` and `mobile-cancel`), a refusal before anything happened is a failed receipt with hgs's reason, and an outcome that cannot be confirmed is uncertain."
  - "the last 7 days, then \"Show older (N)\", in pages of 50 … newest first."
  - "Computers keep every Done task."
- The answers stay compact: a send or cancel answer never carries the task view (a document can reach 4 MiB); the phone exchanges to see the result.
- The legacy Python connector is unchanged. Keep the Python-parity exemptions on `protocol::TASKS`.
- Keep mobile API v1, the frozen connector journal schema and `READ_CLASS_SQL` compatible.
- Builds:
  - hgs: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo`;
  - the relay and the connector build in their own folders;
  - the replica FFI crate builds into the job's tmp target dir.
- Fixtures use invented names (`orchard`, `codex/api`).
- Commits:
  - No co-author lines.
  - Stage only these files.
  - Never push, merge, rebase or switch branches.

## Review Focus

1. **A send that may have pasted.**
   - A connector failure after the native send began (timeout, crash, unreadable answer) is uncertain, never failed.
   - hgs's "the prompt was sent, but the task could not record it" is uncertain.
   - A refusal before the paste is failed with hgs's reason.
   - Pinned by `a_send_that_may_have_pasted_is_uncertain` (connector) and `test_a_send_that_could_not_record_is_uncertain` (hgs).
2. **Exactly once.** The same request identity delivered twice runs hgs once and replays the receipt. Pinned by the connector's journal test and the relay's duplicate test.
3. **Done pages.**
   - Pages are stable and gapless while tasks are accepted meanwhile (cursor `(stage_at, uuid)`, newest first).
   - The total and the recent count are right.
   - Only the page's documents are loaded.
   - Pinned by the crate's `done_pages_walk_every_task_once` and `a_task_accepted_meanwhile_does_not_shift_the_pages`.
4. **Compact answers.** A send of a task with 4 MiB of comments answers well under 1 MiB. Pinned by `test_mobile_answers_stay_small`.
5. **Routing and gating.**
   - An older hgs is not offered the operations.
   - A peer's actions go through `mobile-peer`.
   - A send for a session this computer does not run is refused with hgs's reason.
   - Pinned by the connector capability test, `test_bb_task_exchange` and the end-to-end test.

---

### Task 1: The Done page in `zerus-replica`

**Files:**
- Modify: `crates/replica/src/sync.rs`. `Cached` gains `stage_at: Option<i64>`, filled where `stage` is; an entry missing it counts as a miss, as one missing `stage` does.
- Create or modify: a `done` function in `crates/replica/src/sync.rs`, or a new `crates/replica/src/done.rs`, holding `DonePage` and `DoneRow`.
- Test: `crates/replica/tests/sync.rs` or a new `tests/done.rs`.

**Interfaces:**
- Produces, used by Task 2:

  ```rust
  pub struct DoneRow { pub uuid: String, pub id: String, pub text: String /* the first 200 characters */,
                       pub session: Option<SessionKey>, pub accepted_at: i64 }
  pub struct DonePage { pub tasks: Vec<DoneRow>, pub next: Option<(i64, String)>, pub total: usize, pub recent: usize }
  impl ProjectStore {
      /// Done tasks newest first (`stage_at`, then UUID, descending), at most
      /// `limit` after `before`; `recent` counts those accepted at or after `since`.
      pub fn done_page(&self, before: Option<(i64, String)>, limit: usize, since: i64) -> Result<DonePage>;
  }
  ```

  The sort uses the cached stages and `stage_at`. Damaged tasks are left out. Only the page's tasks are loaded for `id`, `text` and `session`.

- [ ] **Step 1: Write the failing tests**
  - `done_pages_walk_every_task_once`: 120 tasks accepted at distinct times (`accept` with `now` from 10 to 129). Pages of 50 go 50, 50, 20, newest first, no task twice. `total == 120`, and `recent` counts those at or after a `since`.
  - `a_task_accepted_meanwhile_does_not_shift_the_pages`: after the first page, another task is accepted with a later time. The second page continues exactly after the cursor.
  - `the_page_loads_only_its_tasks`: settle the files, then make one Done task's file unreadable but keep its cached entry. The other pages still list it from the cache, and only a page containing it reports it as damaged and leaves it out. In short, `done_page` does not open documents outside its page.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml`.

Expected: FAIL to compile.

- [ ] **Step 3: Implement**

Implement as described.

- [ ] **Step 4: Run the crate's checks**

Run: tests, clippy and fmt.

Expected: all pass.

- [ ] **Step 5: Commit**

Message: "Give the replica the Done list in pages, newest first, from its cached stages".

---

### Task 2: `hgs task mobile-send`, `mobile-cancel`, `mobile-done`

**Files:**
- Create: `src/tasks/mobile.rs`.
- Modify:
  - `src/tasks.rs`: `mod mobile;`, dispatch, and `USAGE`. The three refuse in JSON, as `exchange` does.
  - `src/cli.rs`: a help line `hgs task mobile-send|mobile-cancel|mobile-done --json   the phone's task actions (mobile)`. This is the connector's marker.
  - `src/mobile_peers.rs`: `validate()` admits exactly `["task","mobile-send","--json"]`, `["task","mobile-cancel","--json"]` and `["task","mobile-done","--json"]`, each with an object payload.
- Test: `tests/test_task_mobile.py` (new), and a unit test in `src/mobile_peers.rs`.

**Interfaces:**
- **Requests, on stdin:**
  - mobile-send: `{"schema": 1, "project": ID, "task": UUID, "session": NAME}`.
  - mobile-cancel: `{"schema": 1, "project": ID, "task": UUID, "reason": TEXT?}`.
  - mobile-done: `{"schema": 1, "project": ID, "before": {"at": MS, "uuid": UUID}?, "limit": 1..50}`.
- **Answers:**
  - send: `{"schema": 1, "ok": true, "task": UUID, "stage": "active", "session": {"machine", "session"}, "receipt": {...}}`.
  - cancel: `{"schema": 1, "ok": true, "task": UUID, "stage": "review", "cancel_message": …}`. `cancel_message` is the existing field that says whether the message was pasted.
  - done: `{"schema": 1, "tasks": [{"uuid", "id", "text", "session", "accepted_at"}], "next": {"at", "uuid"} | null, "total": N, "recent": M}`, where `recent` counts the last 7 days.
- **Refusal:** `{"schema": 1, "ok": false, "error": TEXT}`, or with `"uncertain": true` when the paste happened but could not be recorded. Exit status 1.
- **Implementation:**
  - The commands build the existing commands' arguments and call `change(config, "send" | "cancel", …)`:
    - send: `[uuid, "--project", id, "--session", name, "--json"]`;
    - cancel: `[uuid, "--project", id, "--reason", reason?, "--json"]`.
  - The view's `stage`, `session` and `receipt` or `cancel_message` are kept; nothing else.
  - The "sent but not recorded" text becomes one constant, used where it is made and where it is recognized.

- [ ] **Step 1: Write the failing tests**

`tests/test_task_mobile.py` on `tests.test_tasks.DeliveryFixture`:
- `test_a_phone_sends_a_task_to_a_session_here`:
  - the scripted agent receives the prompt, and the answer is ok with stage `active`;
  - the task is In progress here;
  - the answer is small: no `log`, no `text`.
- `test_a_send_refused_before_the_paste_says_why`:
  - a task already In progress elsewhere, a session that does not exist and a task the project lacks are each refused with hgs's own text;
  - exit status 1, and `uncertain` is absent.
- `test_a_send_that_could_not_record_is_uncertain`: make the task's document unwritable after the paste, for example by replacing the project folder's `tasks` directory with a read-only one before the send. If that cannot be arranged deterministically, assert on the constant in a unit test and ledger it.
- `test_a_phone_cancels_a_task`: send, then mobile-cancel with a reason. The answer is ok with stage `review`, the agent receives the cancel message, and the reason is kept as a comment.
- `test_done_pages`: accept three tasks. mobile-done with limit 2 gives the newest two and `next`; `before: next` gives the last one and no `next`; `total` is 3 and `recent` is 3.
- `test_mobile_answers_stay_small`: a task with 4 MiB of comments, the limit, sent through mobile-send. The answer is under 4 KiB.
- `test_invalid_mobile_requests_are_refused_in_json`: schema 2, an unknown project, a limit of 51 and a bad UUID.
- In `mobile_peers.rs`: the three admitted exactly, and other shapes refused.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_mobile -v`.

Expected: FAIL, because the commands do not exist.

- [ ] **Step 3: Implement**

Implement as described.

- [ ] **Step 4: Run them**

Run:
- `tests.test_task_mobile tests.test_task_exchange tests.test_tasks tests.test_task_routing tests.test_swarm`;
- `cargo test`;
- clippy.

Expected: all pass.

- [ ] **Step 5: Commit**

Message: "Answer the phone's Send now, Cancel and Done list in hgs, in JSON and compact".

---

### Task 3: The relay admits `task_send`, `task_cancel` and `task_done`

**Files:**
- Modify: `services/mobile/relay/src/protocol.rs`:
  - `TASKS`, `OPERATIONS`, and `READS` with `task_done` only;
  - payload validation per operation in the `TASKS` branch: project ≤ 128 bytes without controls, task UUID, session text ≤ 1024 without controls, reason ≤ 4096 characters (the cancel reason), `before` with `at` ≥ 0 and a UUID, and a limit of 1–50.
- Modify:
  - `services/mobile/relay/src/store/background.rs`: `task_done` in the 120-second class;
  - `services/mobile/relay/src/sqlite.sql` and `postgres.sql`: a `task_done_history` index;
  - `services/mobile/relay/README.md`.
- Test: `services/mobile/relay/tests/task_exchange.rs`, extended or renamed to `tasks.rs`:
  - the wire of each operation;
  - admission requires the advertised operation (409 otherwise);
  - `task_send` and `task_cancel` count as mutations, a duplicate replays its receipt, and their result outlives two minutes;
  - `task_done` leaves after two minutes.

- [ ] **Step 1: Write the failing tests**

As above.

- [ ] **Step 2: Run them to verify they fail**

Expected: 400 for the unknown operations.

- [ ] **Step 3: Implement**

Implement as described.

- [ ] **Step 4: Run the relay suite on PostgreSQL**

Run it with a disposable container, then:
- `tests/operation_contracts.rs` and `tests/vectors.rs` keep filtering `TASKS`;
- `services/mobile/tests/test_rust_relay.py` keeps `RUST_OPERATIONS` in step.

Expected: all pass.

- [ ] **Step 5: Commit**

Message: "Admit the phone's Send now and Cancel as computer-scoped mutations and its Done list as a short read".

---

### Task 4: The connector runs them

**Files:**
- Modify `services/mobile/connector/src/`:
  - `capabilities.rs`: `task_actions: has("hgs task mobile-send|mobile-cancel|mobile-done --json")`, `IMPLEMENTED`, and advertising the three with the flag;
  - `validate.rs`: computer-scoped, payload checked with the relay's own validator;
  - `execute.rs`:
    - a mutation branch with `started` for send and cancel;
    - `task_done` in `run` with `failure_json`;
    - `ok:false` maps to failed, or to uncertain with `uncertain: true`;
    - a native failure after `started` is uncertain;
  - `journal.rs`: `TASK_READ_SQL` becomes `('task_exchange','task_done')`, and `TASK_READ_PRUNE` follows.
- Modify:
  - `services/mobile/tests/ported.json`;
  - `services/mobile/tests/test_bb_task_exchange.py`: send and cancel through a peer, with a refusal and an uncertain answer;
  - `services/mobile/connector/README.md`.
- Test: `services/mobile/connector/tests/task_exchange.rs`, extended:
  - `the_task_actions_are_advertised_only_with_their_marker`;
  - `a_send_runs_hgs_once_and_replays` (a second delivery is replayed from the journal; the scripted native is called once);
  - `a_send_that_may_have_pasted_is_uncertain` (the native fails or times out → uncertain; an `uncertain: true` refusal → uncertain; a plain refusal → failed with its text);
  - `a_delivered_done_page_leaves_the_journal_after_two_minutes`.

- [ ] **Step 1: Write the failing tests**

As above.

- [ ] **Step 2: Run them to verify they fail**

Expected: unsupported operation.

- [ ] **Step 3: Implement**

Implement as described.

- [ ] **Step 4: Run the connector suite, the black-box run with traces for both connectors and the comparison**

Expected: all pass, with 0 unexpected differences.

- [ ] **Step 5: Commit**

Message: "Run the phone's Send now, Cancel and Done list in the Rust connector".

---

### Task 5: End to end; docs

**Files:**
- Modify: `services/mobile/tests/test_mobile_task_exchange.py`, through the real relay, connector and hgs:
  - `task_done` lists the Done tasks the computer holds;
  - `task_send` for a session the computer does not run comes back failed with hgs's reason;
  - `task_cancel` of a task not In progress comes back failed with hgs's reason.
- Modify: `docs/reference.md` (Project tasks) and `services/mobile/README.md`.

- [ ] **Step 1: Write and run the end-to-end tests**

Run with the relay on PostgreSQL, as in 5b1.

- [ ] **Step 2: Docs and the whole check**

Run:
- `tests.test_ci_security`;
- `scripts/ci/check-source.py`;
- all `test_[!b]*.py` mobile suites as CI does.

Expected: pass.

- [ ] **Step 3: Commit**

Message: "Test the phone's task actions end to end and document them".
