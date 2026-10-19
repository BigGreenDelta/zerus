# Phone Task Exchange through the Relay Implementation Plan (5b1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A phone can exchange a project's tasks with a computer through relay → Rust connector → `hgs task exchange --json`, and every computer's heartbeat carries each project's phone-view digest. This is phase 5b1; the app's replica and sync loop are 5b2.

**Architecture:**
- **`zerus-replica`** learns the phone view of a project: the project document, the tasks that are not Done, and any task the phone already holds. It caches each task's stage beside its version, so the view costs no document loads while files are unchanged.
- **hgs** adds two commands:
  - `task digests --json`: each project's phone-view digest;
  - `task exchange --json`: imports the phone's updates and answers with a page of what it lacks, sending a document larger than a page in SHA-256-named parts.
  Both are admitted on the mobile peer allowlist.
- **The relay** admits the computer-scoped operation `task_exchange` as a short read (two minutes).
- **The Rust connector**:
  - advertises `task_exchange` when its hgs's `--help` names the command;
  - runs it with the payload on stdin and keeps it as a short read in its journal;
  - merges the digests into `mobile_projects`.

**Tech Stack:** Rust 1.99.1 (hgs, `zerus-replica`, relay `services/mobile/relay`, connector `services/mobile/connector`), serde_json, base64 and sha2. Tests are Python `unittest` through the real CLI and the relay's and connector's Rust test suites (PostgreSQL when `ZERUS_RELAY_TEST_DATABASE_URL` is set, SQLite otherwise), plus a Python end-to-end test with the real relay, connector and hgs.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`, "Phone" (rewritten with the owner decisions of 2026-10-19), "Done tasks", "Compatibility and upgrades", "Limits". `services/mobile/relay/README.md` holds the relay contracts.

## Global Constraints

- Spec, verbatim:
  - "The exchange is the mobile operation `task_exchange`: additive to mobile API v1, scoped to a computer, gated by a capability that the connector advertises only when its hgs supports it, and admitted on the mobile peer allowlist. The relay and the connector keep it as a short read, two minutes at most, never as a mutation."
  - "The project document, every task that is not Done, and any task it already holds."
  - "A request and an answer each stay within the relay's 1 MiB; Loro bytes travel as base64. An answer carries at most 512 KiB of updates, and a document whose updates are larger travels in parts the phone asks for one by one, each naming the whole update's SHA-256 and its offset; a part of an update that changed meanwhile starts it again."
  - "Each computer's heartbeat carries, for every project, the digest of that phone view (`hgs task digests --json`, read with the catalog)."
- The legacy Python connector (`services/mobile/zerus_mobile`) is not changed. The vector tests that compare the operation tables with the Python connector's `operations.json` get an explicit exemption for `task_exchange`, as relay `FEATURES` already has for Rust-only features.
- Keep mobile API v1, the frozen journal schema and `READ_CLASS_SQL` compatible. The new short-read cleanup is a separate `DELETE` by operation name.
- Builds:
  - hgs: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo`; never `target/debug` or `target/release`.
  - relay: `cd services/mobile/relay && cargo +1.99.1 test --locked`.
  - connector: `cd services/mobile/connector && cargo +1.99.1 test --locked`.
  - The relay and connector folders are their own Cargo projects with their own `target/`.
- Python suites run from the repository root, `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.<module>`; the mobile suites run as `services/mobile/README.md` says.
- Fixtures use invented names: project `orchard` (`project-1`), computers `a` and `b`.
- Repository text is in English.
- Commits:
  - No co-author lines.
  - Stage only this plan's files.
  - Never push, merge, rebase or switch branches.

## Review Focus

1. **A phone whose copy of a task is behind and the task became Done on the computers.** The phone must still receive that task's updates; otherwise the digests never agree and the phone exchanges every 15 s forever. Pinned by `test_a_task_the_phone_holds_follows_it_into_done`.
2. **An update larger than a page.** It comes in parts whose concatenation imports, and a part asked for after the document changed restarts the update. Pinned by `test_a_large_document_travels_in_parts` and `test_a_part_of_a_changed_update_starts_again`.
3. **A phone payload with a damaged update or an unknown project.** The damaged update is refused and named while the others import; an unknown project is refused without creating a folder. Pinned by `test_damaged_updates_are_refused_and_named` and `test_an_unknown_project_is_refused`.
4. **A computer whose hgs predates the command.** The connector does not advertise `task_exchange`, and the relay refuses a request for it (409, as for any op the target does not list). Pinned in Task 4's capability test and Task 3's admission test.
5. **Journal and relay growth.** A delivered exchange leaves the connector journal and the relay within two minutes, and it never counts as a retained mutation. Pinned by the relay retention test and the connector journal test of Tasks 3 and 4.

---

### Task 1: The phone view in `zerus-replica`

**Files:**
- Modify: `crates/replica/src/sync.rs`. The cache records each task's stage; add `live_inventory` and `phone_inventory`.
- Test: in `crates/replica/tests/sync.rs`.

**Interfaces:**
- Produces:
  - `ProjectStore::live_inventory(&self) -> Result<(Inventory, Vec<Damaged>)>`: the project document and every task not Done.
  - `ProjectStore::phone_inventory(&self, theirs: &Inventory) -> Result<Inventory>`: the live inventory plus every task `theirs` holds that this store has.
  - `Inventory::digest()` is unchanged.

- [ ] **Step 1: Write the failing tests**

Append to `crates/replica/tests/sync.rs`. Use the file's existing helpers to open a store and file tasks, and read the file's top first for their names (`open`, `task`/`file`, `noise`). The tests:

```rust
#[test]
fn the_live_inventory_leaves_done_tasks_out() {
    // Two tasks; one is accepted into Done (Todo -> Done is refused: send it
    // and report, or set the stage through review, as the helpers allow).
    // live_inventory() lists the project document and only the other task;
    // inventory() still lists both.
}

#[test]
fn the_phone_view_keeps_what_the_phone_holds() {
    // theirs holds the Done task (an older version): phone_inventory(&theirs)
    // includes it with this store's version, plus every live task.
}

#[test]
fn the_cached_stage_follows_a_change() {
    // live_inventory() once (the cache now records the stage), change a task
    // to Done, save, wait for the files to settle (the test helper `settle()`),
    // live_inventory() again: the task is gone from it.
}
```

Write each body completely with the file's helpers. To get a task into Done, set its stage the way `crates/replica/src/task.rs` allows: Todo → Done is refused, so record a send, then a report, then `set_stage(Done)`, or use whatever the crate's tests do. Each test asserts on `inventory.tasks.keys()`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --test sync live phone cached`.

Expected: FAIL to compile; `live_inventory` and `phone_inventory` are not defined.

- [ ] **Step 3: Cache the stage and add the views**

In `crates/replica/src/sync.rs`:
- `Cached` gets `#[serde(default, skip_serializing_if = "Option::is_none")] stage: Option<Stage>`, set from `task.stage()` where a task is loaded. A cached entry without a stage counts as a miss, so caches written before this change are read again once.
- Split `inventory()` into a private `fn inventory_with_stages(&self) -> Result<(Inventory, BTreeMap<String, Stage>, Vec<Damaged>)>`. `inventory()` returns its first and last parts, unchanged in behaviour.
- Add:

  ```rust
  /// What a phone holds of this project: the project document and every task
  /// that is not Done (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
  pub fn live_inventory(&self) -> Result<(Inventory, Vec<Damaged>)> {
      let (mut inventory, stages, damaged) = self.inventory_with_stages()?;
      inventory.tasks.retain(|uuid, _| stages.get(uuid) != Some(&Stage::Done));
      Ok((inventory, damaged))
  }

  /// What a phone holding `theirs` is answered from: the live documents, and
  /// every task it already holds, so a task that became Done still reaches it.
  pub fn phone_inventory(&self, theirs: &Inventory) -> Result<Inventory> {
      let (mut inventory, stages, _) = self.inventory_with_stages()?;
      inventory.tasks.retain(|uuid, _| stages.get(uuid) != Some(&Stage::Done) || theirs.tasks.contains_key(uuid));
      Ok(inventory)
  }
  ```

- [ ] **Step 4: Run the crate's checks**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml && cargo +1.99.1 clippy --locked --manifest-path crates/replica/Cargo.toml --all-targets -- -D warnings && cargo +1.99.1 fmt --check --manifest-path crates/replica/Cargo.toml`.

Expected: all pass. The existing cache tests still pass: a cache without stages is read again once.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/src/sync.rs crates/replica/tests/sync.rs
git commit -m "Give the replica a phone view: the project, its live tasks and what the phone holds"
```

---

### Task 2: `hgs task digests` and `hgs task exchange`

**Files:**
- Create: `src/tasks/phone.rs`, the two commands.
- Modify: `src/tasks.rs` (`mod phone;`, dispatch arms, `USAGE`), `src/cli.rs` (the usage line that names them, which the connector's capability marker matches) and `src/mobile_peers.rs` (`validate()` admits exactly `["task","digests","--json"]` and `["task","exchange","--json"]`, with a unit test next to `directory_creation_admits_only_one_exact_computer_scoped_mkdir`).
- Create: `tests/test_task_exchange.py`.

**Interfaces:**
- Consumes (Task 1): `live_inventory` and `phone_inventory`.
- Produces:
  - `hgs task digests --json` prints `{"schema": 1, "projects": {"<project id>": {"digest": "<sha256 hex>"}}}` for every project alive in the catalog or stored here. A project that cannot be read is left out and named on stderr.
  - `hgs task exchange --json` reads a request on stdin:

    ```json
    {"schema": 1, "project": "<id>", "inventory": {"project": {...}, "tasks": {...}},
     "updates": [{"task": "<uuid>" | null, "bytes": "<base64>"}],
     "continue": {"task": "<uuid>" | null, "sha256": "<hex>", "offset": 0}}
    ```

    `updates` and `continue` are optional. It answers:

    ```json
    {"schema": 1, "imported": 1, "refused": [{"uuid": "...", "error": "..."}],
     "updates": [{"task": "<uuid>" | null, "bytes": "<base64>"}],
     "part": {"task": "<uuid>" | null, "size": 1300000, "sha256": "<hex>", "offset": 0, "bytes": "<base64>"},
     "more": true, "digest": "<this computer's phone-view digest after the import>"}
    ```

    `part` appears only when the next document is larger than a page. `more` is true while updates or the rest of a part remain.
  - The `--help` text names `hgs task exchange|digests --json`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_task_exchange.py`. Base it on `tests.test_tasks.Fixture` for an isolated catalog and state. Give it a second, independent state folder as the "other replica" whose exchange answers stand in for a phone: a phone's updates are what another replica holds and this one lacks.

```python
#!/usr/bin/env python3
"""`hgs task exchange` and `hgs task digests`: what the relay hands a phone,
through the real CLI. A second isolated replica stands in for the phone."""
import base64
import hashlib
import json
import subprocess
import unittest

from tests import test_tasks

HGS = test_tasks.HGS


class Exchange(test_tasks.Fixture):
    def exchange(self, request, env=None, ok=True):
        result = subprocess.run([str(HGS), "task", "exchange", "--json"], input=json.dumps(request),
                                env=env or self.env, text=True, capture_output=True, timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr
```

Write the following tests completely. A "phone" inventory is built from what the exchange answered, by keeping an empty-start phone as a second hgs state folder that imports the answers through its own `task exchange`. The exchange imports what it is sent, so feeding one replica's answers into the other replica's exchange is the phone's role.
- `test_digests_name_every_project_and_change_with_a_task`: `task digests` lists `project-1` and `project-2`; adding a task to orchard changes only orchard's digest; accepting a task into Done changes it as well, since the task leaves the view.
- `test_an_empty_phone_gets_the_project_and_its_live_tasks`: two To do tasks and one Done task. An exchange with an empty inventory answers updates for the project document and the two live tasks, not the Done one; after importing those into the second replica, that replica's `task digests` equals this one's.
- `test_the_phone_s_updates_are_imported`: the second replica files a task; its updates for this replica go in `updates`. The answer's `imported` is at least 1, `task get` here shows the task with the person as author, and the answer's `digest` equals `task digests` afterwards.
- `test_a_task_the_phone_holds_follows_it_into_done`: the phone holds a task, the task is accepted here, and the next exchange with the phone's inventory still answers that task's updates.
- `test_a_large_document_travels_in_parts`: a task whose comments total more than 600 KB, as incompressible text (the `noise()` style of `tests/test_swarm.py`, at most 64 KiB per comment, about ten comments). The empty phone's answers come as a `part` series with `offset` growing to `size`. The concatenated bytes hash to `sha256` and import into the second replica (send them back as one update); the task then appears there.
- `test_a_part_of_a_changed_update_starts_again`: the first part of a large update arrives; the task gets a comment here; a `continue` with the first part's sha256 and the next offset is answered from offset 0, with a new sha256.
- `test_damaged_updates_are_refused_and_named`: an update with base64 of random bytes for a task UUID is refused and named in `refused`; another valid update in the same request is imported.
- `test_an_unknown_project_is_refused`: project `nowhere` is refused with a message, and no folder appears below `state/replica`.
- `test_invalid_requests_are_refused`: schema 2, missing inventory, base64 that does not decode, and a request over 1 MiB on stdin are each refused with a message.

In `src/mobile_peers.rs`, add a unit test next to `directory_creation_admits_only_one_exact_computer_scoped_mkdir`:
- `validate` admits `["task","exchange","--json"]` and `["task","digests","--json"]`;
- it refuses `["task","exchange"]`, `["task","exchange","--json","extra"]`, `["task","send","T-a1b2c3","--json"]` and `["task","list","--project","p"]`.

- [ ] **Step 2: Run them to verify they fail**

Run:
- `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_exchange -v`. Expected: FAIL with "usage" errors, since `task exchange` is unknown.
- `cargo +1.99.1 test --locked --target-dir .ci-build/cargo mobile_peers`. Expected: the new unit test FAILs ("native operation is not allowed").

- [ ] **Step 3: The commands**

Create `src/tasks/phone.rs`. It follows `src/swarm/replica.rs`'s `Wire`, `encode` and `decode` (base64 `STANDARD`):

```rust
//! What a phone exchanges with this computer through the relay: `hgs task
//! digests --json` for the heartbeat and `hgs task exchange --json` for one
//! page (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use zerus_replica::{Document, Inventory, ProjectStore, Update};

/// The update bytes one answer carries; base64 and JSON keep it under the relay's 1 MiB.
const PAGE: usize = 512 * 1024;
/// The most a request may be on stdin: the relay's 1 MiB canonical size.
const MAX_REQUEST: u64 = 1 << 20;
```

The rest of the module:
- `fn wire(document: &Document) -> Value` gives `null` for the project, else the task UUID.
- `fn document(value: &Value) -> Result<Document>` is the reverse; it refuses a value that is not a string or null.
- `#[derive(Deserialize)] struct Request { schema: u32, project: String, inventory: Inventory, #[serde(default)] updates: Vec<WireUpdate>, #[serde(default)] r#continue: Option<Continue> }`, with `WireUpdate { task: Option<String>, bytes: String }` and `Continue { task: Option<String>, sha256: String, offset: usize }`.
- `pub(super) fn digests(config) -> Result<Value>`:
  - for each project id in `crate::swarm::projects(config)` (alive) ∪ `ProjectStore::stored`, `ProjectStore::open_within(root, id, 2 s)` and `live_inventory()?.0.digest()`;
  - a busy or unreadable project is left out with an `eprintln!`, as `src/swarm/replica.rs::digests` does.
- `pub(super) fn exchange(config) -> Result<Value>`, step by step:
  1. Read stdin with a `MAX_REQUEST + 1` cap; refuse when over: "the exchange request exceeds 1 MiB".
  2. Parse it; refuse `schema != 1`.
  3. Admit the project: alive in the catalog or stored (`ProjectStore::stored`), else "no project {id:?} here", with no folder created.
  4. Open the store; decode and `import_updates`. A base64 that does not decode is refused as a whole request, "invalid base64 in an update".
  5. `ours = store.phone_inventory(&request.inventory)?`.
  6. If `continue` is given: find the update for that document with `store.updates_for(&ours, &theirs, usize::MAX)` (filtered to that document). If its SHA-256 matches, answer the part from `offset`; otherwise answer the new update from offset 0.
  7. Otherwise `store.updates_for(&ours, &request.inventory, PAGE)`. If the first update alone exceeds `PAGE`, answer it as a part from offset 0 with `updates: []`. Otherwise answer the page.
  8. `more`: true when `updates_for` said so, or when a part's `offset + len < size`.
  9. `digest` is `store.live_inventory()?.0.digest()` after the import.

  `updates_for` against `theirs` = `request.inventory` is correct: the phone sends its whole inventory.

In `src/tasks.rs`:
- add `mod phone;`;
- in `execute`, `"digests" => phone::digests(config)` and `"exchange" => phone::exchange(config)`. Both take only `--json` (refuse other arguments with `USAGE`); `dispatch` prints their JSON (they have no `answer`);
- in `USAGE`, add `| digests --json | exchange --json (stdin)`.

In `src/cli.rs`, add a usage line next to the `hgs task …` line: `       hgs task exchange|digests --json        the phone's task exchange (mobile)`. The connector's marker matches `hgs task exchange|digests --json`.

In `src/mobile_peers.rs` `validate()`, add an arm modeled on the `"dirs"` arm (336-348): `"task"` with exactly `["exchange","--json"]` or `["digests","--json"]`.

- [ ] **Step 4: Run them to verify they pass**

Run: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_exchange tests.test_tasks tests.test_task_routing tests.test_swarm && cargo +1.99.1 test --locked --target-dir .ci-build/cargo && cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings`.

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/tasks/phone.rs src/tasks.rs src/cli.rs src/mobile_peers.rs tests/test_task_exchange.py
git commit -m "Answer a phone's task exchange and each project's phone-view digest in hgs"
```

---

### Task 3: The relay admits `task_exchange` as a short read

**Files:**
- Modify: `services/mobile/relay/src/protocol.rs`:
  - `OPERATIONS`, `READS` and the computer-scoped set, the one `directory_create` and the speech config ops use, which requires `session == ""`;
  - the payload check in `request()`;
  - a short-read class if one exists, otherwise its definition next to the 120 s classes.
- Modify: `services/mobile/relay/src/store/background.rs`, retention: a delivered `task_exchange` goes after 120 s and counts as a read.
- Modify: `services/mobile/relay/tests/vectors.rs`, the exemption for the Rust-only op.
- Create: `services/mobile/relay/tests/task_exchange.rs`, modeled on `tests/directory_create.rs` (`command`, `beat`, admission, duplicate, claim and retention) and `tests/operation_contracts.rs` (`computer_command`, `advertise`, `complete`, `age`, `stored`).
- Modify: `services/mobile/relay/README.md`, the contract of the op.

**Interfaces:**
- Produces:
  - `POST /v1/requests` with `operation: "task_exchange"`, `session: ""` and the Task 2 request as `payload` is admitted for a computer that advertises the op, and refused with 409 otherwise.
  - A canonical body above 1 MiB is refused.
  - The result is the Task 2 answer.
  - After delivery the request and its result go within two minutes, and the op never reserves mutation quota.

- [ ] **Step 1: Write the failing tests**

In `services/mobile/relay/tests/task_exchange.rs`, after reading `tests/directory_create.rs` and `tests/operation_contracts.rs` for the fixture's helpers, write these tests:
- `task_exchange_is_admitted_only_for_a_computer_that_advertises_it`: without the op in the snapshot → 409; with it → queued.
- `task_exchange_requires_an_empty_session`: a non-empty session is refused, as for `directory_create`.
- `the_payload_is_checked`: schema 1, a string project, an object inventory, and updates as `{task: string|null, bytes: base64 string}`. Refused when wrong. Over 1 MiB canonical: refused.
- `a_delivered_exchange_goes_within_two_minutes_and_is_no_mutation`: complete, age by 121 s, run the background retention: the request is gone. While queued it did not count against the mutation cap or retained payload.

In `tests/vectors.rs`, the operation-table comparison with `operations.json` fails as soon as the op is added; Step 3 adds the exemption.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd services/mobile/relay && cargo +1.99.1 test --locked --test task_exchange`.

Expected: FAIL, because the relay refuses the unknown operation.

- [ ] **Step 3: The operation**

In `src/protocol.rs`:
- add `"task_exchange"` to `OPERATIONS`, to `READS` and to the computer-scoped set (`COMPUTER`, or whichever set `directory_create` is in for `session == ""`);
- add a payload check modeled on `directory_create`'s exact-fields check (674-683), with the fields of Task 2;
- use the ordinary 1 MiB canonical limit.

In `src/store/background.rs`, put `task_exchange` in the 120-second class beside the short reads there. In `tests/vectors.rs`, exempt `task_exchange` from the comparison with `operations.json`, next to the FEATURES exemptions (`opencode_launch_effort`, `presence_alerts`, `voice_video`), with a comment that only the Rust connector offers it.

- [ ] **Step 4: Run the relay's suite**

Run: `cd services/mobile/relay && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked`.

Expected: all pass. With `ZERUS_RELAY_TEST_DATABASE_URL` set to a local PostgreSQL the same suite runs against it; without one it falls back to SQLite. Ledger which backend ran.

- [ ] **Step 5: Commit**

```bash
git add services/mobile/relay/src services/mobile/relay/tests services/mobile/relay/README.md
git commit -m "Admit the phone's task exchange in the relay as a computer-scoped short read"
```

---

### Task 4: The connector runs `task_exchange` and carries the digests

**Files:**
- Modify: `services/mobile/connector/src/capabilities.rs`:
  - `Flags` gets `task_exchange: has("hgs task exchange|digests --json")`;
  - `IMPLEMENTED`;
  - `advertised_for`, which advertises it when the flag is set; it is a Rust-only addition like `opencode_launch_effort`.
- Modify: `services/mobile/connector/src/validate.rs` and `services/mobile/connector/src/execute.rs`:
  - the op is computer-scoped;
  - it checks its `gateway_route` against `computer_id` as `directory_create` does (162-176);
  - it runs `Call::new(["task","exchange","--json"]).payload(payload)`, modeled on the speech config call (`media.rs:593-607`);
  - a result over `MAX_RESULT` fails, as for other ops.
- Modify: `services/mobile/connector/src/journal.rs`. A delivered `task_exchange` row is deleted after `SHORT_READ_SECONDS` by its own `DELETE`; `READ_CLASS_SQL` stays frozen.
- Modify: `services/mobile/connector/src/runtime.rs` (`read_projects`) and `services/mobile/connector/src/projects.rs`. With the flag set, `hgs task digests --json` is read with the catalog every 15 s; `normalize` keeps `tasks_digest` per project; a failed read leaves the digests out.
- Modify: the connector's operation tables and markers in its tests (`tests/runtime_vectors.rs` `ported_operations_name_what_this_build_implements` and `capability_markers_match_the_python_connector`), with the Rust-only exemption, and `services/mobile/tests/ported.json` (`rust_operations`).
- Create: tests in `services/mobile/connector/tests/` using the `Scripted` native (see `tests/mutations.rs:24-97`).

**Interfaces:**
- Consumes (Tasks 2 and 3): the hgs commands and the relay op.
- Produces:
  - the heartbeat's `mobile_capabilities.operations` contains `task_exchange` when hgs supports it;
  - `mobile_projects.projects[].tasks_digest`;
  - claimed `task_exchange` requests answered with hgs's JSON.

- [ ] **Step 1: Write the failing tests**

Using the connector's test helpers (`tests/common/mod.rs` and the `Scripted` native):
- `task_exchange_is_advertised_only_when_hgs_names_it`: with and without the usage line in `--help`.
- `task_exchange_runs_hgs_with_its_payload`: the claimed request runs `["task","exchange","--json"]` with the payload on stdin and returns its JSON.
- `a_failing_exchange_fails_and_an_oversized_answer_is_refused`.
- `a_delivered_exchange_leaves_the_journal_after_two_minutes`: a journal test, with the clock moved.
- `heartbeat_projects_carry_their_tasks_digest`: `swarm get` and `task digests` scripted; the heartbeat's project has `tasks_digest`; without the flag there is none.
- `an_exchange_for_a_peer_goes_through_mobile_peer`: modeled on the peer tests (`fleet.rs` with `PeerNative`); the argv inside the envelope is `["task","exchange","--json"]`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd services/mobile/connector && cargo +1.99.1 test --locked`.

Expected: the new tests FAIL; the op is unknown.

- [ ] **Step 3: Implement**

As in Files above, following `directory_create` and the speech config ops in each module. Run the connector's black-box suite (`services/mobile/tests/test_bb_*.py` through `connector_harness.py`, as `services/mobile/README.md` says) to see the tables match.

- [ ] **Step 4: Run the connector's suites**

Run:
- `cd services/mobile/connector && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked`;
- the black-box tests as the mobile README describes.

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add services/mobile/connector services/mobile/tests/ported.json
git commit -m "Run the phone's task exchange in the Rust connector and carry each project's tasks digest"
```

---

### Task 5: End to end through the real relay, connector and hgs; CI and docs

**Files:**
- Create: `services/mobile/tests/test_mobile_task_exchange.py`, modeled on `test_mobile_directory_create.py` (`RustDirectoryHttp`, `relay_fixture.start_relay`, `ZERUS_RELAY_BINARY`, `ZERUS_CONNECTOR_E2E_COMMAND`, `ZERUS_DIRECTORY_TEST_HGS`).
- Modify: the CI job that runs the mobile end-to-end suites (`.github/workflows/checks.yml`), so the new file runs there. Pin it in `tests/test_ci_security.py` if that test lists the suites.
- Modify: `services/mobile/README.md`, `services/mobile/connector/README.md` (if it lists ops) and `docs/reference.md` (Project tasks: the phone's exchange).

- [ ] **Step 1: Write the end-to-end test**

With a real relay (SQLite or PostgreSQL), the Rust connector and a real hgs (the `.ci-build` build) with an isolated catalog that has project `orchard`:
1. A paired fake phone (HTTP client) reads the heartbeat and finds `task_exchange` in the computer's operations and `tasks_digest` in its `orchard` project.
2. It submits a `task_exchange` with an empty inventory, polls the receipt, and gets the project document and a live task filed beforehand with `hgs task add`.
3. It submits one carrying a task created on a second isolated replica (as in Task 2), and `hgs task list` then shows it.
4. After a delivered exchange and 121 s of relay and connector clock (or the fixture's ageing helpers), neither keeps the request.

Run: as the mobile README describes, with the binaries built. Expected: pass.

- [ ] **Step 2: CI and docs**

Add the test to the job that runs the other `test_mobile_*` end-to-end tests. Document:
- `task_exchange` and `tasks_digest` in `services/mobile/README.md` (operations and capabilities);
- the two hgs commands in `docs/reference.md`'s Project tasks section, one paragraph.

Run: `python3 -m unittest discover -s tests -p test_ci_security.py`, `python3 scripts/ci/check-source.py` and the whole set of Tasks 1-4. Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add services/mobile/tests/test_mobile_task_exchange.py .github/workflows/checks.yml tests/test_ci_security.py services/mobile/README.md docs/reference.md
git commit -m "Test the phone's task exchange end to end through the relay and the Rust connector"
```
