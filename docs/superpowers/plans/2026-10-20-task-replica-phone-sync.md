# Phone Task Replica and Sync Implementation Plan (5b2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the Android app is visible, it keeps a replica of every catalogued project's tasks and exchanges it through the relay with every online computer whose tasks digest differs from its own. There is no tasks UI yet (5c).

**Architecture:**
- **`zerus-replica`** gets a `phone` module that owns the whole protocol:
  - the wire types;
  - the computer's answer, moved out of hgs;
  - the phone's `Exchange`, which walks its inventory in ranges that fit a request, puts large documents together from parts, and sends what the answer's inventory lacks.
  - Protocol tests run phone against computer in one process.
- **`zerus-replica-ffi`** exposes:
  - `TaskStore.liveDigest()`;
  - a `TaskExchange` object that hands out request payloads as JSON and takes answers as JSON;
  - `TaskStore.answerExchange()` (the computer's side), which the app's tests use as a fake computer.
- **The app** adds:
  - `TaskReplicas` (storage in `noBackupFilesDir`, one root per connection and swarm);
  - `TaskSync`, which carries each payload to the relay as `task_exchange`, with a per-project lock and a one-minute backoff per computer;
  - `TaskSyncPlan` (which computers qualify);
  - digests parsed from `mobile_projects`;
  - a 15-second loop while the activity is started.

**Tech Stack:**
- Rust 1.99.1: `zerus-replica` and `zerus-replica-ffi` with UniFFI 0.32.2; base64 0.22, which hgs already uses.
- Kotlin: JUnit 4, org.json, OkHttp MockWebServer 4.12.0, already in the build.
- Python `unittest` for hgs.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`, "Phone", including "The phone's side", "Pages" and "Done tasks". The relay contract is in `services/mobile/relay/README.md` ("Phone task exchange").

## Global Constraints

- The spec says, verbatim:
  - "The project document, every task that is not Done, and any task it already holds."
  - "A request and an answer each stay within the relay's 1 MiB."
  - "A request carries at most 256 updates."
  - "An inventory too large for one request goes in ranges of task UUIDs, each answered for its own tasks; the project document belongs to the first."
  - "While the app is visible it checks every 15 seconds."
- The relay counts every key, scalar, object and array toward 50,000 JSON values, and its requests and results are each at most 1 MiB. The app's `RelayApi` caps responses at 1 MiB.
- The hgs exchange keeps its wire form and its refusal texts. `tests/test_task_exchange.py` must pass unchanged after the answer moves into the crate.
- `TaskStore` calls run on `Dispatchers.IO`. `CancellationException` is always rethrown.
- The repository's Android, Rust and Python suites keep passing.
- Builds:
  - hgs: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo`. Never `target/debug`, `target/release` or `tray/build`.
  - Android, from `mobile/android`: `./gradlew --no-daemon testDebugUnitTest lintRelease checkReplicaPackaging -PzerusFirebase=true`.
- Fixtures use invented names: projects `orchard` (`project-1`) and `meadow`, invented connection and computer IDs.
- Commits:
  - No co-author lines.
  - Stage only these files.
  - Never push, merge, rebase or switch branches.

## Review Focus

1. **Termination.** An exchange always ends: a document refused on either side, a document too large to send, and an answer that changes nothing all leave the exchange instead of looping until its request budget. Pinned by `refused_documents_are_not_sent_again`, `an_answer_that_only_repeats_a_refusal_ends_the_exchange` and `a_document_too_large_to_send_is_named_and_skipped`.
2. **Request bounds while the phone grows.** A fresh phone that pulls thousands of tasks into one range splits that range before its next request. No request exceeds 1 MiB or 50,000 values. Pinned by `every_request_fits_the_relay` with 2,000 tasks.
3. **Parts.** A document larger than a page arrives in parts. A part out of step, or one whose SHA-256 does not match, starts the document again, and is never imported damaged. Pinned by `a_large_document_arrives_in_parts` and `a_part_out_of_step_starts_again`.
4. **Cross-connection isolation.** Two connections reaching the same swarm keep separate replicas, and removing a connection removes its replicas. Pinned by `replicasAreKeptPerConnectionAndSwarm`.
5. **Backoff and gating.** No exchange runs for:
  - a computer that is offline or last-known;
  - a computer that does not advertise `task_exchange`;
  - a relay without it;
  - a computer whose digest matches;
  - a computer in its minute of backoff.
  Pinned by `TaskSyncPlanTest` and `aFailedExchangeWaitsAMinute`.

---

### Task 1: The exchange's wire form and the computer's answer move into `zerus-replica`

**Files:**
- Create: `crates/replica/src/phone.rs`. It holds:
  - `PAGE` (512 KiB) and `MAX_UPDATES` (256);
  - the wire types `WireUpdate`, `Range`, `Resume`, `Request`, `Part` and `Answer` (serde);
  - `pub fn answer(store: &ProjectStore, request: Request) -> Result<Answer>`.
- Modify:
  - `crates/replica/src/lib.rs`: `pub mod phone;`.
  - `crates/replica/Cargo.toml`: `base64 = "0.22"`.
  - The lock files: `crates/replica/Cargo.lock`, `crates/replica-ffi/Cargo.lock` and the root `Cargo.lock`, refreshed with `cargo +1.99.1 update -p zerus-replica --offline` or a plain build.
- Modify: `src/tasks/phone.rs`. It keeps reading stdin, the 1 MiB cap, the JSON parse into `zerus_replica::phone::Request`, the known-project check and opening the store, then calls `zerus_replica::phone::answer`. The `Range`, `WireUpdate`, `Continue`, `part` and `only` helpers move into the crate.
- Test: `crates/replica/tests/phone.rs` (new) for the answer's own rules; the Python suite `tests/test_task_exchange.py` unchanged.

**Interfaces:**
- Produces, used by Tasks 2 and 3:

  ```rust
  pub const PAGE: usize = 512 * 1024;
  pub const MAX_UPDATES: usize = 256;
  pub struct WireUpdate { pub task: Option<String>, pub bytes: String }
  pub struct Range { pub after: Option<String>, pub through: Option<String> }
  impl Range {
      pub fn check(&self) -> Result<()>;
      pub fn contains(&self, uuid: &str) -> bool;
      pub fn narrow(&self, inventory: Inventory) -> Inventory;
  }
  pub struct Resume { pub task: Option<String>, pub sha256: String, pub offset: usize }
  pub struct Request {
      pub schema: u32, pub project: String, pub inventory: Inventory,
      pub range: Option<Range>, pub updates: Vec<WireUpdate>,
      #[serde(rename = "continue")] pub resume: Option<Resume>,
  }
  pub struct Part { pub task: Option<String>, pub size: usize, pub sha256: String, pub offset: usize, pub bytes: String }
  pub struct Answer {
      pub schema: u32, pub imported: usize, pub refused: Vec<Damaged>,
      pub updates: Vec<WireUpdate>, pub part: Option<Part>, pub more: bool,
      pub digest: String, pub inventory: Option<Inventory>,
  }
  pub fn answer(store: &ProjectStore, request: Request) -> Result<Answer>;
  ```

  Serialization:
  - optional fields skip when absent: `range`, `updates` when empty, `continue`, `part`, `inventory`;
  - `Range` skips each absent bound;
  - `WireUpdate.task` serializes `null` for the project document.

  `Damaged` gains `Clone, PartialEq, Eq, Deserialize` if it lacks them.
- Errors keep the hgs texts:
  - "unknown exchange schema N";
  - "the range bound \"…\" is not a task UUID";
  - "an empty range";
  - "the inventory's task \"…\" lies outside its range";
  - "an exchange carries at most 256 updates";
  - "invalid base64 in an update";
  - new: "the request is for project \"…\"" when `request.project` is not the store's.

- [ ] **Step 1: Write the failing crate tests**

`crates/replica/tests/phone.rs`, with two stores in temp dirs (copy the `store`/`add` helpers from `tests/sync.rs`):
- `an_empty_phone_is_answered_the_live_tasks`: two To do tasks and one accepted into Done (`accept` as in `tests/sync.rs`). An answer to `Request{inventory: Inventory::default(), ..}` has the two live tasks' updates and `more == false`. Its `digest` is `store.live_inventory()?.0.digest()`.
- `the_answer_s_rules_refuse_what_hgs_refused`: each refusal text above. A request for `project-2` against the `project-1` store is refused with "the request is for project \"project-2\"".
- `a_range_answers_its_own_tasks`: the uuid split as in the Python test `test_an_inventory_in_ranges_covers_the_whole_project`, in Rust.
- `the_wire_form_round_trips`: a `Request` and an `Answer` serialize to the hgs JSON shape and parse back:
  - `continue` is spelled so;
  - `task: null` appears for the project document;
  - absent optional fields are absent.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --test phone`.

Expected: FAIL to compile; `zerus_replica::phone` does not exist.

- [ ] **Step 3: Move the answer**

Create `crates/replica/src/phone.rs`:
- Port `Range` (`check`, `contains`, `narrow`), `only`, `part` and the body of `exchange` from `src/tasks/phone.rs` into `pub fn answer(store, request)`.
- `part` returns a `Part`, not JSON.
- Decode the updates inside `answer`.

`src/tasks/phone.rs` then keeps:
- `digests`;
- the stdin read and the 1 MiB check;
- `serde_json::from_slice::<zerus_replica::phone::Request>` ("invalid exchange request: …");
- `known` (the "no project … here" refusal before any folder is created);
- `open_within(PROJECT_WAIT)` ("the project's tasks are busy; try again");
- `serde_json::to_value(answer(&store, request)?)`.

Keep the `range`, `updates.len()` and schema checks before the known-project check, as now. Call `range.check()` and the other cheap checks from a new `pub fn Request::check(&self) -> Result<()>` in the crate, which hgs calls before `known`. `answer` calls it again.

- [ ] **Step 4: Run the crate tests and the hgs suite**

Run:
- `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml`;
- `cargo +1.99.1 clippy --locked --manifest-path crates/replica/Cargo.toml --all-targets -- -D warnings`;
- `cargo +1.99.1 fmt --check --manifest-path crates/replica/Cargo.toml`;
- `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_exchange tests.test_tasks tests.test_swarm`;
- `cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings`.

Expected: all pass; the Python exchange suite is unchanged.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/src/phone.rs crates/replica/src/lib.rs crates/replica/src/store.rs crates/replica/Cargo.toml crates/replica/Cargo.lock crates/replica-ffi/Cargo.lock Cargo.lock crates/replica/tests/phone.rs src/tasks/phone.rs
git commit -m "Move the phone exchange's wire form and the computer's answer into the replica crate"
```

---

### Task 2: The phone's `Exchange`

**Files:**
- Modify: `crates/replica/src/phone.rs`. Add `Budget`, `Progress` and `Exchange`.
- Test: `crates/replica/tests/phone.rs`.

**Interfaces:**
- Produces, used by Task 3:

  ```rust
  /// What the phone's own part of a request may take, so the whole request
  /// stays within the relay's 1 MiB and 50,000 JSON values.
  pub struct Budget {
      pub inventory_bytes: usize,  // 256 KiB
      pub inventory_values: usize, // 20,000
      pub push_bytes: usize,       // 512 KiB of updates before base64
      pub requests: usize,         // 512 requests per exchange
  }
  impl Default for Budget { … }
  pub struct Progress {
      pub changed: usize,              // documents the answer changed here
      pub refused_here: Vec<Damaged>,  // the computer's updates this replica refused
      pub refused_there: Vec<Damaged>, // this replica's updates the computer refused
      pub unsent: Vec<String>,         // documents too large to send ("project" or a UUID)
      pub finished: bool,
  }
  pub struct Exchange { … }
  impl Exchange {
      pub fn new() -> Self;                       // Budget::default()
      pub fn with_budget(budget: Budget) -> Self;
      pub fn next_request(&mut self, store: &ProjectStore) -> Result<Option<Request>>;
      pub fn accept(&mut self, store: &ProjectStore, answer: Answer) -> Result<Progress>;
      pub fn finished(&self) -> bool;
  }
  ```

- Behaviour:
  - `ranges: Vec<Range>` starts as `[Range::default()]`; the first entry is the current range, and the exchange is finished when none remains.
  - `next_request`:
    - **Range.** Narrow `store.inventory()` to the current range. When it exceeds the budget, its bytes estimated per task entry as `uuid.len() + 4 + serde_json::to_vec(version).len()` and its values counted as the relay counts them, the range ends at the last task that fits, and the rest becomes the next range. At least one task always stays.
    - **Updates to send.** With the computer's inventory of this range known from the last answer, add the updates it lacks: `store.updates_for(&narrowed minus skipped documents, theirs, push_bytes)`, at most `MAX_UPDATES`. A single update larger than `push_bytes` is skipped and named in `unsent`.
    - **Continue.** Ask for the next part with `continue` while one is pending.
    - **Request budget.** Count requests; past `requests` it fails with "the exchange did not settle within N requests".
  - `accept`:
    - Import the answer's updates. A part with offset 0 starts a document. A part that continues it, with the same `sha256`, `task` and `offset == bytes so far`, appends. Anything else drops the document, and the next request asks afresh. A complete document whose SHA-256 matches is imported; one that does not match is dropped.
    - Answer refusals add those documents to the skip set (`refused_there`).
    - An answer that changed nothing, whose updates were all refused here, and that repeats a refusal already met in this exchange, fails with "the computer's updates do not apply here: …". The caller backs off.
    - The answer's `inventory`, narrowed to the current range, becomes the computer's inventory. The range is settled when no part is pending, `more` is false and the inventory shows nothing left to send. Then the next range starts with no computer inventory.

- [ ] **Step 1: Write the failing tests**

In `crates/replica/tests/phone.rs`, a helper `run(phone, computer, budget) -> (Vec<serde_json::Value>, Vec<Progress>)` loops:
- `next_request`;
- `serde_json::to_value`, asserting `≤ 1 MiB` serialized and `≤ 50,000` values with a `values(&Value)` helper that counts as the relay does;
- `serde_json::from_value::<Request>` (the round trip a relay makes);
- `answer(computer, request)`, also serialized, parsed back and bounded;
- `accept`.

It runs until `next_request` is `None`. Tests:
- `a_fresh_phone_gets_every_live_task`: 30 tasks, one in Done. Afterwards the phone's live digest equals the computer's, and the phone holds no Done task.
- `the_phone_s_edits_reach_the_computer`: the phone comments on a task it pulled. One run brings the comment to the computer, and the digests agree.
- `both_sides_changed`: concurrent comments on the same task on each side converge in one run.
- `a_held_task_follows_into_done`: the phone holds a task, the computer accepts it. After a run the phone's copy is Done.
- `every_request_fits_the_relay`: 2,000 tasks with seven comments each (as many peers per task) and the default budget. A fresh phone converges, the run uses more than one range, every request and answer fits, and the second run makes exactly one request per range and changes nothing.
- `small_budgets_split_ranges_and_still_converge`: `Budget { inventory_bytes: 4096, .. }` with 200 tasks converges.
- `a_large_document_arrives_in_parts`: one task with five 60,000-character comments, as in the Python test. It arrives in at least two parts and imports.
- `a_part_out_of_step_starts_again`: hand the `Exchange` a forged second part with a wrong offset. The next request carries no `continue`, and the document still arrives.
- `refused_documents_are_not_sent_again`: the computer's copy of a task is damaged (write garbage into its `.loro` file), and the phone holds a good copy. The phone sends its whole document, which replaces the damaged one there (`import_task` sets the damaged copy aside). For a document the computer refuses, plant a task whose project field names another project on the phone side via `TaskDoc::create` with `project: "project-2"`, saved into the phone store's folder with `save`. That document is sent once, comes back in `refused_there`, and is not sent again. The run finishes.
- `an_answer_that_only_repeats_a_refusal_ends_the_exchange`: on the phone, damage a task file the computer has a newer version of, so the computer keeps sending what the phone refuses. The run ends with that error and does not use its whole request budget.
- `a_document_too_large_to_send_is_named_and_skipped`: `Budget { push_bytes: 64 * 1024, .. }`. A task with 100 KiB of new comments on the phone appears in `unsent`, and the run finishes.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --test phone`.

Expected: FAIL to compile; `Exchange` is not defined.

- [ ] **Step 3: Implement**

Implement `Exchange` as described.

- [ ] **Step 4: Run the crate's checks**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml`, plus clippy and fmt.

Expected: all pass. The 2,000-task test stays under about 30 s in debug; otherwise lower to 1,500 and ledger it.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/src/phone.rs crates/replica/tests/phone.rs
git commit -m "Give the replica the phone's side of the exchange: ranges that fit, parts and what the computer lacks"
```

---

### Task 3: The bindings

**Files:**
- Modify: `crates/replica-ffi/src/lib.rs`.
- Test: `crates/replica-ffi/tests/api.rs`; `mobile/android/app/src/test/java/app/zerus/mobile/replica/TaskStoreBindingTest.kt`.

**Interfaces:**
- Produces, used by Task 4:

  ```rust
  #[uniffi::export] impl TaskStore {
      /// The digest of the project document and every task here that is not Done.
      pub fn live_digest(&self) -> Result<String>;
      /// A new exchange of this project's tasks with one computer.
      pub fn exchange(self: Arc<Self>) -> Arc<TaskExchange>;
      /// The computer's side of one request: tests play a computer with it.
      pub fn answer_exchange(&self, request: String) -> Result<String>;
  }
  #[derive(uniffi::Object)] pub struct TaskExchange { store: Arc<TaskStore>, state: Mutex<phone::Exchange> }
  #[uniffi::export] impl TaskExchange {
      /// The next `task_exchange` payload, as JSON, or none once it is done.
      pub fn next_request(&self) -> Result<Option<String>>;
      /// The computer's answer: the receipt's `result`, as JSON.
      pub fn accept(&self, answer: String) -> Result<ExchangeProgress>;
  }
  #[derive(uniffi::Record)] pub struct ExchangeProgress {
      pub changed: u32, pub refused_here: Vec<DamagedTask>, pub refused_there: Vec<DamagedTask>,
      pub unsent: Vec<String>, pub finished: bool,
  }
  ```

  Errors:
  - a JSON that does not parse → `Refused("invalid exchange answer: …")`;
  - crate errors map as the other methods map them;
  - a busy store → `Failed("the project's tasks are busy; try again")`.

  Each call opens the store under its lock, as the other methods do.

- [ ] **Step 1: Write the failing tests**

- `crates/replica-ffi/tests/api.rs`, `two_stores_exchange_through_json`: store A holds tasks and store B is empty. Loop: `B.exchange().next_request()` → `A.answer_exchange(request)` → `B…accept(answer)` until `None`. Then `A.live_digest() == B.live_digest()`. Invalid answer JSON → `ReplicaError::Refused`.
- Kotlin, `TaskStoreBindingTest.aPhoneExchangesWithAComputerThroughJson`: the same through the generated bindings, closing the exchange with `use`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica-ffi/Cargo.toml`.

Expected: FAIL to compile.

- [ ] **Step 3: Implement**

Implement as above. `TaskExchange` is `Send + Sync` through its `Mutex`.

- [ ] **Step 4: Run**

Run:
- `cargo +1.99.1 test --locked --manifest-path crates/replica-ffi/Cargo.toml`, plus clippy and fmt;
- `cd mobile/android && ./gradlew --no-daemon testDebugUnitTest --tests 'app.zerus.mobile.replica.*'`.

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add crates/replica-ffi/src/lib.rs crates/replica-ffi/tests/api.rs mobile/android/app/src/test/java/app/zerus/mobile/replica/TaskStoreBindingTest.kt
git commit -m "Expose the phone's exchange, the live digest and the computer's answer through the bindings"
```

---

### Task 4: Digests, replicas and `TaskSync` in the app

**Files:**
- Modify: `mobile/android/app/src/main/java/app/zerus/mobile/Projects.kt`. `ProjectCatalog` gets `taskDigests: Map<ProjectKey, Map<String, String>> = emptyMap()`, from computer ID to `tasks_digest`. Only a lowercase 64-hex digest counts. It is parsed in the project loop of `ProjectParser.parse` for every computer whose catalog lists the project, folders or not.
- Create: `mobile/android/app/src/main/java/app/zerus/mobile/TaskSync.kt` with `TaskReplicas`, `TaskPeer`, `TaskSyncPlan` and `TaskSync`.
- Test:
  - `mobile/android/app/src/test/java/app/zerus/mobile/ProjectsTest.kt`: digests per computer.
  - `mobile/android/app/src/test/java/app/zerus/mobile/TaskSyncTest.kt` (new).

**Interfaces:**
- Produces, used by Task 5:

  ```kotlin
  /** The phone's task replicas: one root per relay connection and swarm, below `root`. */
  class TaskReplicas(private val root: java.io.File) {
      fun store(key: ProjectKey, name: String): TaskStore   // TaskStore(folder(key).path, key.id, name)
      fun folder(key: ProjectKey): java.io.File             // root/sha256(connectionId)/sha256(swarmId)
      fun purge(connectionId: String)                       // deletes root/sha256(connectionId)
  }
  data class TaskPeer(val computerId: String, val digest: String)
  object TaskSyncPlan {
      /** Each catalogued project's computers that can exchange now. */
      fun peers(digests: Map<ProjectKey, Map<String, String>>, relayOperations: Map<String, Set<String>>,
                machineOperations: Map<MachineKey, Set<String>>, machines: List<Machine>): Map<ProjectKey, List<TaskPeer>>
  }
  class TaskSync(private val api: () -> RelayApi, private val replicas: TaskReplicas,
                 private val clock: () -> Long = System::currentTimeMillis) {
      /** Exchanges `key`'s tasks with each of `peers` whose digest is not the phone's, one at a time; the failures by computer. */
      suspend fun sync(connection: Connection, key: ProjectKey, name: String, peers: List<TaskPeer>): Map<String, String>
  }
  ```

- Behaviour:
  - `TaskSyncPlan.peers` keeps a computer only when all hold:
    - the relay's operations for its connection include `task_exchange`;
    - the computer's own operations include it;
    - its `Machine` is `online && !lastKnown`.
  - `TaskSync.sync` holds a per-key `Mutex`. It opens the store once and closes it at the end, and calls the store on `Dispatchers.IO`. For each peer, sorted by computer ID:
    - skip it while its backoff (60 s after a failure) runs;
    - skip it when `liveDigest()` equals its digest, read again before each peer;
    - otherwise loop:
      - `nextRequest()`;
      - `api().submit(connection, Target(peer.computerId, "", "", "", connection.id), "task_exchange", JSONObject(payload), id)`;
      - `api().await(connection, receipt, maxPolls = 80, pollDelayMs = 400)`;
      - require `state == "completed"`, otherwise fail with the receipt's error;
      - `accept(result.toString())`.
    - A failure records `clock() + 60_000` for that computer and its message in the result. Success clears the backoff.
  - `CancellationException` passes through.

- [ ] **Step 1: Write the failing tests**

- `ProjectsTest.taskDigestsAreReadPerComputer`:
  - two computers list `project-1` with different digests;
  - one lists it without folders or sessions, and still has its digest;
  - an invalid digest is left out;
  - an uncatalogued computer contributes nothing.
- `TaskSyncTest`:
  - Shared setup: a `MockWebServer` plays the relay. Its computer is a `TaskStore` in a temp folder, answering with `answerExchange(payload.toString())`. The receipt is `completed` with the answer as `result`.
  - `aPhonePullsAComputersTasksThroughTheRelay`:
    - three tasks on the computer;
    - one `sync` brings them, and `liveDigest()` agrees;
    - every request is `POST /v1/requests` with `operation == "task_exchange"`, `session == ""` and the peer's `computer_id`.
  - `thePhonesEditsReachTheComputer`: a comment added through the phone's store reaches the computer.
  - `aMatchingDigestSendsNothing`.
  - `aFailedExchangeWaitsAMinute`:
    - the relay answers `failed` with "the project's tasks are busy; try again";
    - `sync` reports it;
    - a second `sync` at the same clock makes no request; at +61 s it tries again.
  - `replicasAreKeptPerConnectionAndSwarm`:
    - two keys differing only in connection get different folders;
    - `purge` of one leaves the other.
  - `TaskSyncPlanTest`: each gate alone removes the computer.

- [ ] **Step 2: Run them to verify they fail**

Run: `cd mobile/android && ./gradlew --no-daemon testDebugUnitTest --tests 'app.zerus.mobile.TaskSync*' --tests 'app.zerus.mobile.ProjectsTest'`.

Expected: FAIL to compile.

- [ ] **Step 3: Implement**

Implement as described.

- [ ] **Step 4: Run them**

Run the same command.

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add mobile/android/app/src/main/java/app/zerus/mobile/Projects.kt mobile/android/app/src/main/java/app/zerus/mobile/TaskSync.kt mobile/android/app/src/test/java/app/zerus/mobile/ProjectsTest.kt mobile/android/app/src/test/java/app/zerus/mobile/TaskSyncTest.kt
git commit -m "Keep a task replica per connection and swarm and exchange it through the relay with each computer whose digest differs"
```

---

### Task 5: The loop while the app is visible; docs

**Files:**
- Modify: `mobile/android/app/src/main/java/app/zerus/mobile/ZerusViewModel.kt`:
  - `taskDigests`, the time they were read, `taskSync` and `syncTasks()`;
  - `refresh` publishes the catalogs' digests;
  - `disconnect` purges the connection's replicas.
- Modify: `mobile/android/app/src/main/java/app/zerus/mobile/MainActivity.kt`. A `repeatOnLifecycle(STARTED)` loop runs `model.syncTasks()` and then waits 15 s.
- Modify:
  - `mobile/android/README.md`: the task replica, its folder, cadence and backoff;
  - `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md` if anything changed from "The phone's side".

**Interfaces:**
- Consumes (Task 4): `TaskReplicas`, `TaskSync` and `TaskSyncPlan`.
- Behaviour of `syncTasks()`:
  - **Guards.** It does nothing in demo mode or while a run is going on (one `Boolean` guard on Main).
  - **Inputs.** It uses the published digests, operations and machines when the catalog was read within 20 s. Otherwise, for each connection, it calls `api.computers` and `SessionCatalogParser.parse` on `Dispatchers.Default`, and uses that result for this run without publishing it.
  - **Work.** It runs `TaskSyncPlan.peers`, then `taskSync.sync` per project, one project at a time. The project's name comes from `projects`; otherwise "Project".
  - **Discard.** It drops a run whose `connections` changed meanwhile.
  - **Errors.** It never shows an error; failures wait in `TaskSync`'s backoff.

- [ ] **Step 1: Wire it**

As described. A ViewModel test needs Android, so this glue stays thin over the tested `TaskSync` and `TaskSyncPlan`.

- [ ] **Step 2: Build and test**

Run: `cd mobile/android && ./gradlew --no-daemon testDebugUnitTest lintRelease checkReplicaPackaging -PzerusFirebase=true`.

Expected: BUILD SUCCESSFUL.

- [ ] **Step 3: Docs and the whole check**

Write the README section. Then run:
- `python3 -m unittest discover -s tests -p test_ci_security.py`;
- `python3 scripts/ci/check-source.py`;
- the crate, ffi and hgs suites.

Expected: pass.

- [ ] **Step 4: Commit**

```bash
git add mobile/android/app/src/main/java/app/zerus/mobile/ZerusViewModel.kt mobile/android/app/src/main/java/app/zerus/mobile/MainActivity.kt mobile/android/README.md docs/superpowers/specs/2026-10-18-task-replication-loro-design.md
git commit -m "Sync the phone's tasks every 15 seconds while the app is visible and drop them with their connection"
```
