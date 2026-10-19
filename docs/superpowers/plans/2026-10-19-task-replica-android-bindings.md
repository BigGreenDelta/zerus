# Task Replica in the Android Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Android app with the task replica. `zerus-replica` is compiled for arm64-v8a, armeabi-v7a and x86_64 with cargo-ndk, the app calls it through Kotlin bindings that UniFFI generates, and binding tests run on the JVM against the host library. This is phase 5a of the replication spec; the relay exchange (5b) and the Tasks UI (5c) follow.

**Architecture:**
- A new crate `crates/replica-ffi` (`zerus-replica-ffi`) is the only code UniFFI sees. It wraps `zerus-replica` and never imports Loro.
- The rules that both hgs and the phone need move into `zerus-replica` as store-level operations: where a new task goes, whether its prompt can be sent, the top of a new list, reordering. hgs uses them too, so they hold the same everywhere.
- The app's Gradle build runs cargo with the pinned toolchain:
  - once for the host library, which the bindings are generated from and the JVM tests load;
  - once with cargo-ndk for the three ABIs, packaged as jniLibs.
- The Rust crates' licence notices are collected into the app's assets at build time.

**Tech Stack:** Rust 1.99.1, UniFFI `=0.32.2`, cargo-ndk 4.1.2, Android NDK 28.2.13676358 (r28c), AGP 8.11.1, Kotlin 2.1.20, JNA 5.17.0, JUnit 4, JDK 17.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`, sections "The replica crate", "Android build" and phase 5.

## Global Constraints

- Spec, verbatim:
  - "The app's Gradle build compiles `zerus-replica` for arm64-v8a, armeabi-v7a and x86_64 with `cargo-ndk`, generates the Kotlin bindings with UniFFI, and packages the libraries."
  - "CI gets an Android job that builds the crate for those targets with the pinned Rust and runs the binding tests; the APK grows by the size of the library per ABI, measured and reported in the plan."
  - "The app keeps its documents in private storage, never in shared storage or backups that leave the device unencrypted." The replica's files are opened in 5b; 5a adds no storage location.
  - "It is the only code that imports `loro`." The FFI crate depends on `zerus-replica` only.
  - "It exposes our operations, not Loro's … Nothing outside the crate sees a Loro type."
- Owner decisions (2026-10-19):
  - The toolchain lives in the home folder: JDK 17 in `~/.jdks/jdk-17.0.20.1+1`, SDK and NDK in `~/Android/Sdk`, cargo-ndk 4.1.2 in `~/.cargo/bin`.
  - Binding tests run on the JVM against the host library, without an emulator.
  - The phone exchanges tasks through the Rust connector only (5b).
- Local Gradle runs: `cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon …`.
- Never build hgs into `target/debug` or `target/release` (live agents and the install use them):
  - the hgs suites use `--target-dir .ci-build/cargo`;
  - the replica crate builds in its own folder (`crates/replica/target`, as before);
  - the FFI crate builds below `mobile/android/app/build/rust`, or `target/replica-ffi` in CI.
- 64-bit libraries are 16 KB page aligned. NDK r28 does this by default; check `LOAD` alignment `0x4000` with `llvm-readelf -lW`.
- Repository text is in English; fixtures use invented names (project `orchard`, `project-1`).
- Commits:
  - No co-author lines.
  - Stage only this plan's files.
  - Never push, merge, rebase or switch branches; work stays on `task-tracker`.

## Review Focus

1. **A rule the phone could break that hgs keeps.** The phone and hgs share the rules from Task 1: a prompt that could not be sent, a new task in a stage other than To do or Backlog, a task that changes list landing anywhere but the top. Pinned by the crate tests of Task 1 and `refusalsArriveAsRefusedWithTheirMessage`.
2. **A refusal against a failure.** A Kotlin caller must tell "nothing changed, show the reason" apart from "storage failed". The error crosses as `ReplicaException.Refused` or `ReplicaException.Failed` with the crate's message. Pinned by `refusalsArriveAsRefusedWithTheirMessage` and `a_damaged_inventory_is_refused`.
3. **Two replicas meeting through the binding.** Bytes handed out and back in through `ByteArray` and JSON must round-trip, so the second replica ends with the same inventory. Pinned by `twoReplicasMeetThroughTheBindings`.
4. **A release build that strips the bindings.** R8 must keep JNA and the generated classes, which are reached by reflection. Checked by `minifyReleaseWithR8` in Task 3 and by the keep rules; device checks are 5b's.
5. **A licence notice missing for a crate in the APK.** The build fails rather than ship without it. `collect-licenses.py` already exits on a missing notice; Task 4 runs it in the Gradle build for the Android target.

---

### Task 1: The rules hgs and the phone share live in `zerus-replica`

**Files:**
- Create: `crates/replica/src/ops.rs`
- Modify: `crates/replica/src/lib.rs` (`mod ops;`)
- Modify: `src/tasks.rs`. `add`, `edit`, `move` and `stage` use the store operations; `check_prompt` goes.

**Interfaces:**
- Produces, in `impl ProjectStore`:
  - `check_prompt(&self, task: &TaskDoc, project_name: &str) -> Result<()>`
  - `new_task(&self, text: &str, stage: Stage, target: Target, author: Actor, position: &Position, project_name: &str, now: i64) -> Result<TaskDoc>`, which returns the task unsaved
  - `edit_task(&self, task: &mut TaskDoc, text: &str, actor: &Actor, project_name: &str, now: i64) -> Result<()>`
  - `change_stage(&self, task: &mut TaskDoc, to: Stage, actor: &Actor, now: i64) -> Result<()>`
  - `reorder(&self, task: &mut TaskDoc, position: &Position) -> Result<()>`

- [ ] **Step 1: Write the failing tests**

Create `crates/replica/src/ops.rs` with only the test module:

```rust
#[cfg(test)]
mod tests {
    use crate::{Actor, ErrorKind, Position, ProjectStore, Stage, Target, MAX_TEXT};

    fn open() -> (tempfile::TempDir, ProjectStore) {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        (root, store)
    }

    fn file(store: &ProjectStore, text: &str, stage: Stage, position: Position) -> String {
        let mut task = store
            .new_task(text, stage, Target::Any, Actor::Person, &position, "orchard", 1)
            .unwrap();
        store.save(&mut task).unwrap();
        task.uuid().to_owned()
    }

    fn texts(store: &ProjectStore, stage: Stage) -> Vec<String> {
        store.ordered(stage).unwrap().iter().map(|t| t.view().unwrap().text).collect()
    }

    #[test]
    fn a_new_task_goes_where_asked_and_only_to_todo_or_backlog() {
        let (_root, store) = open();
        file(&store, "Second", Stage::Todo, Position::End);
        file(&store, "First", Stage::Todo, Position::Top);
        file(&store, "Later", Stage::Backlog, Position::End);
        assert_eq!(texts(&store, Stage::Todo), ["First", "Second"]);
        assert_eq!(texts(&store, Stage::Backlog), ["Later"]);
        for stage in [Stage::Active, Stage::Review, Stage::Done] {
            let error = store
                .new_task("Nope", stage, Target::Any, Actor::Person, &Position::End, "orchard", 1)
                .err()
                .unwrap();
            assert_eq!(error.kind(), ErrorKind::Refused);
        }
    }

    #[test]
    fn a_prompt_that_could_not_be_sent_is_refused() {
        let (_root, store) = open();
        let text = "x".repeat(MAX_TEXT);
        let error = store
            .new_task(&text, Stage::Todo, Target::Any, Actor::Person, &Position::End, "orchard", 1)
            .err()
            .unwrap();
        assert_eq!(error.kind(), ErrorKind::Refused);
        let uuid = file(&store, "Short", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        assert!(store.edit_task(&mut task, &text, &Actor::Person, "orchard", 2).is_err());
        store.edit_task(&mut task, "Still short", &Actor::Person, "orchard", 2).unwrap();
        assert_eq!(task.view().unwrap().text, "Still short");
    }

    #[test]
    fn a_task_that_changes_list_goes_to_its_top() {
        let (_root, store) = open();
        file(&store, "Old backlog", Stage::Backlog, Position::End);
        let uuid = file(&store, "Moved", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        store.change_stage(&mut task, Stage::Backlog, &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        assert_eq!(texts(&store, Stage::Backlog), ["Moved", "Old backlog"]);
    }

    #[test]
    fn reorder_moves_a_task_within_its_list() {
        let (_root, store) = open();
        file(&store, "One", Stage::Todo, Position::End);
        file(&store, "Two", Stage::Todo, Position::End);
        let uuid = file(&store, "Three", Stage::Todo, Position::End);
        let mut task = store.load(&uuid).unwrap();
        store.reorder(&mut task, &Position::Index(0)).unwrap();
        store.save(&mut task).unwrap();
        assert_eq!(texts(&store, Stage::Todo), ["Three", "One", "Two"]);
    }
}
```

In `crates/replica/src/lib.rs`, add `mod ops;` after `mod project;`.

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml ops`.

Expected: FAIL to compile; there is no method `new_task` on `ProjectStore`. If `MAX_TEXT` or `load` is not public, the compiler says so as well.

- [ ] **Step 3: The operations**

Put this above the test module in `crates/replica/src/ops.rs`:

```rust
//! The task operations that need their project: where a task goes in its
//! list and whether its prompt could be sent. hgs and the mobile apps use
//! these, so the rules hold the same everywhere.
use crate::render::{task_prompt, Tools};
use crate::schema::{Actor, Stage, Target};
use crate::store::{Position, ProjectStore};
use crate::task::{NewTask, TaskDoc};
use crate::{Error, Result};

impl ProjectStore {
    /// Refuses a task whose prompt could not be sent: the rendered prompt
    /// follows hgs's input limits. `project_name` names the project in it.
    pub fn check_prompt(&self, task: &TaskDoc, project_name: &str) -> Result<()> {
        let settings = self.project()?.settings();
        task_prompt(&settings.task_template, &task.view()?, project_name, Tools::Cli).map(|_| ())
    }

    /// A new task at `position` of its stage, To do or Backlog; unsaved.
    #[allow(clippy::too_many_arguments)]
    pub fn new_task(
        &self,
        text: &str,
        stage: Stage,
        target: Target,
        author: Actor,
        position: &Position,
        project_name: &str,
        now: i64,
    ) -> Result<TaskDoc> {
        if !matches!(stage, Stage::Todo | Stage::Backlog) {
            return Err(Error::new("a new task goes to To do or Backlog"));
        }
        let rank = self.rank_for(stage, position, None)?;
        let id = self.unused_short_id()?;
        let task = TaskDoc::create(NewTask {
            project: self.project_id(),
            id: &id,
            text,
            stage,
            target,
            author,
            rank: &rank,
            now,
        })?;
        self.check_prompt(&task, project_name)?;
        Ok(task)
    }

    /// Replaces the prompt of a task in To do or Backlog.
    pub fn edit_task(
        &self,
        task: &mut TaskDoc,
        text: &str,
        actor: &Actor,
        project_name: &str,
        now: i64,
    ) -> Result<()> {
        task.edit_text(text, actor, now)?;
        self.check_prompt(task, project_name)
    }

    /// Moves a task to stage `to`; a task that changes list goes to the top
    /// of its new list.
    pub fn change_stage(&self, task: &mut TaskDoc, to: Stage, actor: &Actor, now: i64) -> Result<()> {
        let from = task.stage()?;
        task.set_stage(to, actor, now)?;
        if from != to && matches!(to, Stage::Todo | Stage::Backlog) {
            let rank = self.rank_for(to, &Position::Top, Some(task.uuid()))?;
            task.set_rank(&rank)?;
        }
        Ok(())
    }

    /// Puts a task of To do or Backlog at `position` of its list.
    pub fn reorder(&self, task: &mut TaskDoc, position: &Position) -> Result<()> {
        let rank = self.rank_for(task.stage()?, position, Some(task.uuid()))?;
        task.set_rank(&rank)
    }
}
```

If `task_prompt` or `Tools` are not visible as `crate::render::…`, use the `pub use` names from `lib.rs`.

- [ ] **Step 4: Run the crate tests**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml && cargo +1.99.1 clippy --locked --manifest-path crates/replica/Cargo.toml --all-targets -- -D warnings && cargo +1.99.1 fmt --check --manifest-path crates/replica/Cargo.toml`.

Expected: all pass. Run `cargo +1.99.1 fmt --manifest-path crates/replica/Cargo.toml` first if formatting differs.

- [ ] **Step 5: hgs uses them**

In `src/tasks.rs`:
- `add`: replace the `rank_for` / `unused_short_id` / `TaskDoc::create` / `check_prompt` lines with:

  ```rust
      let store = store(config, &id)?;
      let mut task = store
          .new_task(&text, stage, target, author.clone(), &position, &name, now())
          .map_err(fail)?;
      store.save(&mut task).map_err(fail)?;
  ```

- `change`, arm `"edit"`: `store.edit_task(&mut task, &text, &who, project_name, now()).map_err(fail)?;`.
- `change`, arm `"move"`: keep the computation of `position`; replace the `rank_for`/`set_rank` lines with `store.reorder(&mut task, &position).map_err(fail)?;`.
- `change`, arm `"stage"`: `let to = Stage::parse(args.get(1).ok_or_else(|| fail(USAGE))?).map_err(fail)?; store.change_stage(&mut task, to, &who, now()).map_err(fail)?;`.
- Remove `fn check_prompt` and the now-unused imports (`NewTask`, `Tools` if nothing else uses them).

Run:
- `cargo +1.99.1 build --locked --target-dir .ci-build/cargo`
- `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks tests.test_task_routing -v`
- `cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings`

Expected: all pass, unchanged in count.

- [ ] **Step 6: Commit**

```bash
git add crates/replica/src/ops.rs crates/replica/src/lib.rs src/tasks.rs
git commit -m "Keep the placement and prompt rules of tasks in zerus-replica for hgs and the phone"
```

---

### Task 2: `crates/replica-ffi`, the replica through UniFFI

**Files:**
- Create: `crates/replica-ffi/Cargo.toml`, `crates/replica-ffi/Cargo.lock` (generated), `crates/replica-ffi/uniffi.toml`, `crates/replica-ffi/src/lib.rs`, `crates/replica-ffi/src/bin/uniffi-bindgen.rs`, `crates/replica-ffi/tests/api.rs`
- Modify: `.github/dependabot.yml`. Add `/crates/replica-ffi` to the Cargo directories.

**Interfaces:**
- Consumes (Task 1): `ProjectStore::{new_task, edit_task, change_stage, reorder}`.
- Produces (Kotlin package `app.zerus.mobile.replica`):
  - The object `TaskStore(root, project, projectName)` with methods `list`, `get`, `add`, `editText`, `setStage`, `setTarget`, `moveTask`, `comment`, `settings`, `setSettings`, `inventory` (a JSON string), `updatesFor(theirs, limit)` and `importUpdates(updates)`.
  - The records `TaskInfo`, `TaskLogEntry`, `SessionRef`, `TaskListing`, `DamagedTask`, `TaskSettings`, `DocumentUpdate`, `UpdatePage` and `ImportResult`.
  - The enums `TaskStage`, `TaskTarget`, `TaskActor` and `TaskPosition`.
  - The error `ReplicaException` (`Refused`, `Failed`).
  - The library `libzerus_replica_ffi.so`.

- [ ] **Step 1: The crate skeleton and the failing tests**

`crates/replica-ffi/Cargo.toml`:

```toml
[package]
name = "zerus-replica-ffi"
version = "0.1.0"
edition = "2021"
rust-version = "1.99"
license = "MIT"
description = "The Zerus task replica for the mobile apps, through UniFFI"
publish = false

[lib]
name = "zerus_replica_ffi"
crate-type = ["cdylib", "lib"]

[[bin]]
name = "uniffi-bindgen"
path = "src/bin/uniffi-bindgen.rs"
required-features = ["bindgen"]

[features]
# Only for generating the bindings: the apps' library never contains the generator.
bindgen = ["uniffi/cli"]

[dependencies]
serde_json = "1"
uniffi = "=0.32.2"
zerus-replica = { path = "../replica" }

[dev-dependencies]
tempfile = "3"

[profile.release]
# The library ships once per ABI in the APK: small and without symbols.
opt-level = "s"
lto = true
codegen-units = 1
strip = true
```

`crates/replica-ffi/uniffi.toml`:

```toml
[bindings.kotlin]
package_name = "app.zerus.mobile.replica"
cdylib_name = "zerus_replica_ffi"
```

`crates/replica-ffi/src/bin/uniffi-bindgen.rs`:

```rust
//! Generates the Kotlin bindings from the built library (`--features bindgen`).
fn main() {
    uniffi::uniffi_bindgen_main()
}
```

`crates/replica-ffi/tests/api.rs`:

```rust
//! The binding surface, called from Rust as the apps call it.
use zerus_replica_ffi::{
    ReplicaError, TaskPosition, TaskSettings, TaskStage, TaskStore, TaskTarget,
};

fn store(root: &tempfile::TempDir) -> std::sync::Arc<TaskStore> {
    TaskStore::new(root.path().display().to_string(), "project-1".into(), "orchard".into())
}

#[test]
fn tasks_are_filed_listed_and_changed() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let second = store.add("Add rate limiting".into(), TaskStage::Todo, TaskTarget::AnySession, TaskPosition::End).unwrap();
    let first = store.add("Measure first".into(), TaskStage::Todo, TaskTarget::AnySession, TaskPosition::Top).unwrap();
    let listed: Vec<_> = store.list().unwrap().tasks.into_iter().map(|t| t.text).collect();
    assert_eq!(listed, ["Measure first", "Add rate limiting"]);
    assert!(first.id.starts_with("T-"));
    assert_eq!(store.get(second.id.clone()).unwrap().uuid, second.uuid);
    store.edit_text(second.uuid.clone(), "Add rate limiting, edited".into()).unwrap();
    store.move_task(second.uuid.clone(), TaskPosition::Top).unwrap();
    let moved = store.set_stage(first.uuid.clone(), TaskStage::Backlog).unwrap();
    assert_eq!(moved.stage, TaskStage::Backlog);
    let commented = store.comment(second.uuid.clone(), "Measured: 40 rps.".into()).unwrap();
    let entry = commented.log.iter().find(|e| e.kind == "comment").unwrap();
    assert_eq!(entry.text, "Measured: 40 rps.");
    let todo: Vec<_> = store.list().unwrap().tasks.into_iter().filter(|t| t.stage == TaskStage::Todo).map(|t| t.text).collect();
    assert_eq!(todo, ["Add rate limiting, edited"]);
}

#[test]
fn refusals_and_failures_stay_apart() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let task = store.add("Add rate limiting".into(), TaskStage::Todo, TaskTarget::AnySession, TaskPosition::End).unwrap();
    match store.edit_text(task.uuid.clone(), "   ".into()) {
        Err(ReplicaError::Refused(message)) => assert!(message.contains("empty"), "{message}"),
        other => panic!("expected a refusal, got {other:?}"),
    }
    match store.add("Nope".into(), TaskStage::Done, TaskTarget::AnySession, TaskPosition::End) {
        Err(ReplicaError::Refused(message)) => assert!(message.contains("To do or Backlog"), "{message}"),
        other => panic!("expected a refusal, got {other:?}"),
    }
    assert!(matches!(store.get("T-zzzzzz".into()), Err(ReplicaError::Refused(_)) | Err(ReplicaError::Failed(_))));
}

#[test]
fn settings_change_and_come_back() {
    let root = tempfile::tempdir().unwrap();
    let store = store(&root);
    let mut settings: TaskSettings = store.settings().unwrap();
    assert!(settings.agents_other);
    settings.agents_other = false;
    settings.agent_limit = 3;
    let saved = store.set_settings(settings).unwrap();
    assert_eq!((saved.agents_other, saved.agent_limit), (false, 3));
    assert_eq!(store.settings().unwrap().agent_limit, 3);
}

#[test]
fn two_replicas_meet_through_the_api() {
    let (a_root, b_root) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(&a_root), store(&b_root));
    a.add("Add rate limiting".into(), TaskStage::Todo, TaskTarget::AnySession, TaskPosition::End).unwrap();
    let page = a.updates_for(b.inventory().unwrap(), 1 << 20).unwrap();
    assert!(!page.more);
    let imported = b.import_updates(page.updates).unwrap();
    assert!(imported.changed >= 1 && imported.refused.is_empty());
    assert_eq!(b.list().unwrap().tasks[0].text, "Add rate limiting");
    assert_eq!(a.inventory().unwrap(), b.inventory().unwrap());
}

#[test]
fn a_damaged_inventory_is_refused() {
    let root = tempfile::tempdir().unwrap();
    assert!(matches!(store(&root).updates_for("not json".into(), 1 << 20), Err(ReplicaError::Refused(_))));
}
```

`crates/replica-ffi/src/lib.rs`, a stub so that the tests compile and fail:

```rust
uniffi::setup_scaffolding!();
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cargo +1.99.1 test --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi`. This also creates `Cargo.lock`; run with `--locked` from now on.

Expected: FAIL to compile; there is no `TaskStore` and no `ReplicaError` in `zerus_replica_ffi`.

`target/replica-ffi` is inside the repository's `target/`, but it is a folder of its own: it never touches `target/debug/hgs`.

- [ ] **Step 3: The binding surface**

Replace `crates/replica-ffi/src/lib.rs` with:

```rust
//! The task replica for the mobile apps through UniFFI: the typed operations
//! of `zerus-replica`, on documents below a folder the app chooses in its
//! private files. The person acts on the phone. Sends and cancels run on the
//! session's computer, over the relay exchange. See "The replica crate" in
//! docs/superpowers/specs/2026-10-18-task-replication-loro-design.md.
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use zerus_replica as replica;

uniffi::setup_scaffolding!();

/// A rule refused the change and nothing changed (show the message), or
/// storage failed.
#[derive(Debug, uniffi::Error)]
#[uniffi(flat_error)]
pub enum ReplicaError {
    Refused(String),
    Failed(String),
}

impl std::fmt::Display for ReplicaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Refused(message) | Self::Failed(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for ReplicaError {}

impl From<replica::Error> for ReplicaError {
    fn from(error: replica::Error) -> Self {
        match error.kind() {
            replica::ErrorKind::Refused => Self::Refused(error.to_string()),
            replica::ErrorKind::Failed => Self::Failed(error.to_string()),
        }
    }
}

type Result<T> = std::result::Result<T, ReplicaError>;

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskStage {
    Backlog,
    Todo,
    Active,
    Review,
    Done,
}

impl From<replica::Stage> for TaskStage {
    fn from(stage: replica::Stage) -> Self {
        match stage {
            replica::Stage::Backlog => Self::Backlog,
            replica::Stage::Todo => Self::Todo,
            replica::Stage::Active => Self::Active,
            replica::Stage::Review => Self::Review,
            replica::Stage::Done => Self::Done,
        }
    }
}

impl From<TaskStage> for replica::Stage {
    fn from(stage: TaskStage) -> Self {
        match stage {
            TaskStage::Backlog => Self::Backlog,
            TaskStage::Todo => Self::Todo,
            TaskStage::Active => Self::Active,
            TaskStage::Review => Self::Review,
            TaskStage::Done => Self::Done,
        }
    }
}

/// A session on a computer: the computer's swarm identity and the session name.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct SessionRef {
    pub machine: String,
    pub session: String,
}

impl From<replica::SessionKey> for SessionRef {
    fn from(key: replica::SessionKey) -> Self {
        Self { machine: key.machine, session: key.session }
    }
}

impl From<SessionRef> for replica::SessionKey {
    fn from(key: SessionRef) -> Self {
        Self { machine: key.machine, session: key.session }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
/// Any session, or one; `AnySession`, since Kotlin's `Any` is taken.
pub enum TaskTarget {
    AnySession,
    Session { session: SessionRef },
}

impl From<replica::Target> for TaskTarget {
    fn from(target: replica::Target) -> Self {
        match target {
            replica::Target::Any => Self::AnySession,
            replica::Target::Session(key) => Self::Session { session: key.into() },
        }
    }
}

impl From<TaskTarget> for replica::Target {
    fn from(target: TaskTarget) -> Self {
        match target {
            TaskTarget::AnySession => Self::Any,
            TaskTarget::Session { session } => Self::Session(session.into()),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskActor {
    Person,
    Agent { session: SessionRef },
}

impl From<replica::Actor> for TaskActor {
    fn from(actor: replica::Actor) -> Self {
        match actor {
            replica::Actor::Person => Self::Person,
            replica::Actor::Agent(key) => Self::Agent { session: key.into() },
        }
    }
}

/// One entry of a task's history.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskLogEntry {
    pub kind: String,
    pub actor: TaskActor,
    pub at: i64,
    pub text: String,
    pub session: Option<SessionRef>,
    pub request_id: Option<String>,
}

impl From<replica::LogEntry> for TaskLogEntry {
    fn from(entry: replica::LogEntry) -> Self {
        Self {
            kind: entry.kind,
            actor: entry.actor.into(),
            at: entry.at,
            text: entry.text,
            session: entry.session.map(Into::into),
            request_id: entry.request_id,
        }
    }
}

/// A task as the apps read it; times are Unix milliseconds.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskInfo {
    pub uuid: String,
    pub id: String,
    pub project: String,
    pub schema: i64,
    pub writable: bool,
    pub created_at: i64,
    pub stage_at: i64,
    pub author: TaskActor,
    pub stage: TaskStage,
    pub target: TaskTarget,
    pub rank: String,
    pub session: Option<SessionRef>,
    pub summary: String,
    pub cancelled_at: Option<i64>,
    pub cancel_reason: String,
    pub text: String,
    pub log: Vec<TaskLogEntry>,
}

impl From<replica::TaskView> for TaskInfo {
    fn from(view: replica::TaskView) -> Self {
        Self {
            uuid: view.uuid,
            id: view.id,
            project: view.project,
            schema: view.schema,
            writable: view.writable,
            created_at: view.created_at,
            stage_at: view.stage_at,
            author: view.author.into(),
            stage: view.stage.into(),
            target: view.target.into(),
            rank: view.rank,
            session: view.session.map(Into::into),
            summary: view.summary,
            cancelled_at: view.cancelled_at,
            cancel_reason: view.cancel_reason,
            text: view.text,
            log: view.log.into_iter().map(Into::into).collect(),
        }
    }
}

/// A task file that failed to load; it stays on disk and never blocks the others.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct DamagedTask {
    pub uuid: String,
    pub error: String,
}

impl From<replica::Damaged> for DamagedTask {
    fn from(damaged: replica::Damaged) -> Self {
        Self { uuid: damaged.uuid, error: damaged.error }
    }
}

/// Every task of the project in stage order, each stage in its order.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskListing {
    pub tasks: Vec<TaskInfo>,
    pub damaged: Vec<DamagedTask>,
}

/// Where a task goes in the order of its stage.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskPosition {
    Top,
    End,
    /// From the top, zero-based; beyond the end means the end.
    Index { index: u32 },
    /// Before or after the task with this short ID or UUID.
    Before { reference: String },
    After { reference: String },
}

impl From<TaskPosition> for replica::Position {
    fn from(position: TaskPosition) -> Self {
        match position {
            TaskPosition::Top => Self::Top,
            TaskPosition::End => Self::End,
            TaskPosition::Index { index } => Self::Index(index as usize),
            TaskPosition::Before { reference } => Self::Before(reference),
            TaskPosition::After { reference } => Self::After(reference),
        }
    }
}

/// The project's task settings.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskSettings {
    pub task_template: String,
    pub cancel_template: String,
    /// Agents may start and cancel tasks in other sessions.
    pub agents_other: bool,
    /// Tasks agents may start per hour in the project; 0 is no limit.
    pub agent_limit: i64,
}

impl From<replica::Settings> for TaskSettings {
    fn from(settings: replica::Settings) -> Self {
        Self {
            task_template: settings.task_template,
            cancel_template: settings.cancel_template,
            agents_other: settings.agents_other,
            agent_limit: settings.agent_limit,
        }
    }
}

impl From<TaskSettings> for replica::Settings {
    fn from(settings: TaskSettings) -> Self {
        Self {
            task_template: settings.task_template,
            cancel_template: settings.cancel_template,
            agents_other: settings.agents_other,
            agent_limit: settings.agent_limit,
        }
    }
}

/// Loro updates for one document: `task` is its UUID, or none for the project document.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct DocumentUpdate {
    pub task: Option<String>,
    pub bytes: Vec<u8>,
}

impl From<replica::Update> for DocumentUpdate {
    fn from(update: replica::Update) -> Self {
        let task = match update.document {
            replica::Document::Project => None,
            replica::Document::Task(uuid) => Some(uuid),
        };
        Self { task, bytes: update.bytes }
    }
}

impl From<DocumentUpdate> for replica::Update {
    fn from(update: DocumentUpdate) -> Self {
        let document = match update.task {
            None => replica::Document::Project,
            Some(uuid) => replica::Document::Task(uuid),
        };
        Self { document, bytes: update.bytes }
    }
}

/// A page of what another replica lacks; `more` when the next page has more.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct UpdatePage {
    pub updates: Vec<DocumentUpdate>,
    pub more: bool,
}

/// What an import changed, and the documents it refused.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct ImportResult {
    pub changed: u32,
    pub refused: Vec<DamagedTask>,
}

/// The tasks of one project below `root`. Each call opens the project under
/// its lock and releases it on return, so several callers never hold it.
#[derive(uniffi::Object)]
pub struct TaskStore {
    root: PathBuf,
    project: String,
    project_name: String,
}

impl TaskStore {
    fn store(&self) -> Result<replica::ProjectStore> {
        Ok(replica::ProjectStore::open(&self.root, &self.project)?)
    }

    /// Runs `change` on the task `reference` and saves it.
    fn change(
        &self,
        reference: &str,
        change: impl FnOnce(&replica::ProjectStore, &mut replica::TaskDoc) -> replica::Result<()>,
    ) -> Result<TaskInfo> {
        let store = self.store()?;
        let mut task = store.find(reference)?;
        change(&store, &mut task)?;
        store.save(&mut task)?;
        Ok(task.view()?.into())
    }
}

#[uniffi::export]
impl TaskStore {
    /// `project` is the project's catalog ID and `project_name` its current
    /// name, which task prompts carry.
    #[uniffi::constructor]
    pub fn new(root: String, project: String, project_name: String) -> Arc<Self> {
        Arc::new(Self { root: PathBuf::from(root), project, project_name })
    }

    pub fn list(&self) -> Result<TaskListing> {
        let (tasks, damaged) = self.store()?.listing()?;
        Ok(TaskListing {
            tasks: tasks
                .iter()
                .map(|task| task.view().map(TaskInfo::from))
                .collect::<replica::Result<_>>()?,
            damaged: damaged.into_iter().map(Into::into).collect(),
        })
    }

    /// The task with this short ID or UUID.
    pub fn get(&self, reference: String) -> Result<TaskInfo> {
        Ok(self.store()?.find(&reference)?.view()?.into())
    }

    /// Files a task by the person, to To do or Backlog.
    pub fn add(&self, text: String, stage: TaskStage, target: TaskTarget, position: TaskPosition) -> Result<TaskInfo> {
        let store = self.store()?;
        let mut task = store.new_task(
            &text,
            stage.into(),
            target.into(),
            replica::Actor::Person,
            &position.into(),
            &self.project_name,
            now(),
        )?;
        store.save(&mut task)?;
        Ok(task.view()?.into())
    }

    pub fn edit_text(&self, reference: String, text: String) -> Result<TaskInfo> {
        let name = self.project_name.clone();
        self.change(&reference, |store, task| {
            store.edit_task(task, &text, &replica::Actor::Person, &name, now())
        })
    }

    pub fn set_stage(&self, reference: String, stage: TaskStage) -> Result<TaskInfo> {
        self.change(&reference, |store, task| {
            store.change_stage(task, stage.into(), &replica::Actor::Person, now())
        })
    }

    pub fn set_target(&self, reference: String, target: TaskTarget) -> Result<TaskInfo> {
        self.change(&reference, |_, task| {
            task.set_target(&target.into(), &replica::Actor::Person, now())
        })
    }

    pub fn move_task(&self, reference: String, position: TaskPosition) -> Result<TaskInfo> {
        self.change(&reference, |store, task| store.reorder(task, &position.into()))
    }

    pub fn comment(&self, reference: String, text: String) -> Result<TaskInfo> {
        self.change(&reference, |_, task| task.comment(&text, &replica::Actor::Person, now()))
    }

    pub fn settings(&self) -> Result<TaskSettings> {
        Ok(self.store()?.project()?.settings().into())
    }

    /// Saves every field of `settings`.
    pub fn set_settings(&self, settings: TaskSettings) -> Result<TaskSettings> {
        let store = self.store()?;
        let mut doc = store.project()?;
        doc.set_settings(&settings.into())?;
        store.save_project(&doc)?;
        Ok(doc.settings().into())
    }

    /// What this replica holds, as JSON in the swarm's inventory form.
    pub fn inventory(&self) -> Result<String> {
        let (inventory, _) = self.store()?.inventory()?;
        serde_json::to_string(&inventory).map_err(|e| ReplicaError::Failed(e.to_string()))
    }

    /// A page of at most `limit` bytes of what the replica whose inventory is
    /// `theirs` lacks.
    pub fn updates_for(&self, theirs: String, limit: u64) -> Result<UpdatePage> {
        let theirs: replica::Inventory = serde_json::from_str(&theirs)
            .map_err(|e| ReplicaError::Refused(format!("invalid inventory: {e}")))?;
        let store = self.store()?;
        let (ours, _) = store.inventory()?;
        let (updates, more) = store.updates_for(&ours, &theirs, usize::try_from(limit).unwrap_or(usize::MAX))?;
        Ok(UpdatePage { updates: updates.into_iter().map(Into::into).collect(), more })
    }

    pub fn import_updates(&self, updates: Vec<DocumentUpdate>) -> Result<ImportResult> {
        let imported = self.store()?.import_updates(updates.into_iter().map(Into::into).collect())?;
        Ok(ImportResult {
            changed: u32::try_from(imported.changed).unwrap_or(u32::MAX),
            refused: imported.refused.into_iter().map(Into::into).collect(),
        })
    }
}
```

Field names must match `zerus_replica`'s public structs (`TaskView`, `LogEntry`, `Damaged`, `Update`, `Settings`, `Inventory`). If a field differs, follow the crate and ledger it. `replica::Result` must be public; it is re-exported in `lib.rs`.

- [ ] **Step 4: Run the crate tests**

Run: `cargo +1.99.1 test --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi && cargo +1.99.1 clippy --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi --all-targets -- -D warnings && cargo +1.99.1 fmt --check --manifest-path crates/replica-ffi/Cargo.toml`.

Expected: 5 passed and clean. Run `cargo fmt` first if needed.

Check that the bindings generate:

`cargo +1.99.1 build --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi && (cd crates/replica-ffi && cargo +1.99.1 run --locked --features bindgen --bin uniffi-bindgen --target-dir ../../target/replica-ffi-bindgen -- generate --library ../../target/replica-ffi/debug/libzerus_replica_ffi.so --language kotlin --no-format --out-dir ../../target/replica-ffi-kotlin) && ls target/replica-ffi-kotlin/app/zerus/mobile/replica`

Expected: `zerus_replica_ffi.kt`.

In `.github/dependabot.yml`, add `"/crates/replica-ffi"` to the Cargo `directories`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica-ffi/Cargo.toml crates/replica-ffi/Cargo.lock crates/replica-ffi/uniffi.toml crates/replica-ffi/src crates/replica-ffi/tests .github/dependabot.yml
git commit -m "Expose the task replica to the mobile apps through UniFFI"
```

---

### Task 3: The Android build compiles the library and generates the bindings

**Files:**
- Modify: `mobile/android/app/build.gradle.kts`
- Modify: `mobile/android/app/proguard-rules.pro`
- Create: `mobile/android/app/src/test/java/app/zerus/mobile/replica/TaskStoreBindingTest.kt`

**Interfaces:**
- Consumes (Task 2): the crate at `crates/replica-ffi` and the Kotlin package `app.zerus.mobile.replica`.
- Produces:
  - the Gradle tasks `rustHostLibrary`, `uniffiBindings` and `rustAndroidLibraries`;
  - `ndkVersion = "28.2.13676358"`;
  - the libraries in the APK as `lib/<abi>/libzerus_replica_ffi.so`.

- [ ] **Step 1: Write the failing binding test**

`mobile/android/app/src/test/java/app/zerus/mobile/replica/TaskStoreBindingTest.kt`:

```kotlin
package app.zerus.mobile.replica

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The task replica through its generated bindings, on the host library the build made. */
class TaskStoreBindingTest {
    private fun store() = TaskStore(Files.createTempDirectory("zerus-replica").toString(), "project-1", "orchard")

    @Test
    fun tasksAreFiledListedAndChanged() {
        val store = store()
        val later = store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val first = store.add("Measure first", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.Top)
        assertEquals(listOf("Measure first", "Add rate limiting"), store.list().tasks.map { it.text })
        store.moveTask(later.uuid, TaskPosition.Top)
        store.setStage(first.uuid, TaskStage.BACKLOG)
        val target = TaskTarget.Session(SessionRef("4b810143-c06b-48bc-b665-9625e51357ae", "codex/api"))
        val pinned = store.setTarget(later.uuid, target)
        assertEquals(target, pinned.target)
        val commented = store.comment(later.uuid, "Measured: 40 rps.")
        assertEquals("Measured: 40 rps.", commented.log.last { it.kind == "comment" }.text)
        assertEquals(TaskActor.Person, commented.log.last().actor)
        assertEquals(listOf(TaskStage.TODO, TaskStage.BACKLOG), store.list().tasks.map { it.stage })
    }

    @Test
    fun refusalsArriveAsRefusedWithTheirMessage() {
        val store = store()
        val task = store.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        try {
            store.editText(task.uuid, "  ")
            fail("an empty prompt was accepted")
        } catch (refused: ReplicaException.Refused) {
            assertTrue(refused.message.orEmpty(), refused.message.orEmpty().contains("empty"))
        }
        try {
            store.add("Nope", TaskStage.DONE, TaskTarget.AnySession, TaskPosition.End)
            fail("a task was filed in Done")
        } catch (refused: ReplicaException.Refused) {
            assertTrue(refused.message.orEmpty().contains("To do or Backlog"))
        }
    }

    @Test
    fun twoReplicasMeetThroughTheBindings() {
        val (a, b) = store() to store()
        a.add("Add rate limiting", TaskStage.TODO, TaskTarget.AnySession, TaskPosition.End)
        val page = a.updatesFor(b.inventory(), 1_048_576UL)
        assertFalse(page.more)
        assertTrue(b.importUpdates(page.updates).changed > 0u)
        assertEquals("Add rate limiting", b.list().tasks.single().text)
        assertEquals(a.inventory(), b.inventory())
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon testDebugUnitTest --tests app.zerus.mobile.replica.TaskStoreBindingTest`.

Expected: FAIL in `compileDebugUnitTestKotlin` with "Unresolved reference: TaskStore", because no bindings are generated yet.

- [ ] **Step 3: Gradle builds the library and the bindings**

In `mobile/android/app/build.gradle.kts`, add before `android {`:

```kotlin
// The task replica: zerus-replica through UniFFI (crates/replica-ffi), built with the
// pinned CI toolchain. The host library gives the Kotlin bindings and is what the JVM
// unit tests load; cargo-ndk builds the library of each packaged ABI.
val rustToolchain = "1.99.1"
val replicaCrate = rootProject.file("../../crates/replica-ffi")
val replicaSources = listOf(replicaCrate, rootProject.file("../../crates/replica")).flatMap {
    listOf(it.resolve("src"), it.resolve("Cargo.toml"), it.resolve("Cargo.lock"))
} + replicaCrate.resolve("uniffi.toml")
val rustBuild = layout.buildDirectory.dir("rust")
val hostLibraryName = if (System.getProperty("os.name").startsWith("Mac")) "libzerus_replica_ffi.dylib" else "libzerus_replica_ffi.so"
val hostLibrary = rustBuild.map { it.file("host/debug/$hostLibraryName") }
val uniffiKotlin = layout.buildDirectory.dir("generated/uniffi/kotlin")
val rustJniLibs = rustBuild.map { it.dir("jniLibs") }
val rustHostLibrary by tasks.registering(Exec::class) {
    inputs.files(replicaSources)
    outputs.file(hostLibrary)
    workingDir = replicaCrate
    commandLine("cargo", "+$rustToolchain", "build", "--locked", "--target-dir", rustBuild.get().dir("host").asFile.path)
}
val uniffiBindings by tasks.registering(Exec::class) {
    dependsOn(rustHostLibrary)
    inputs.file(hostLibrary)
    inputs.files(replicaSources)
    outputs.dir(uniffiKotlin)
    workingDir = replicaCrate
    doFirst { delete(uniffiKotlin) }
    commandLine("cargo", "+$rustToolchain", "run", "--locked", "--features", "bindgen", "--bin", "uniffi-bindgen",
        "--target-dir", rustBuild.get().dir("bindgen").asFile.path, "--", "generate",
        "--library", hostLibrary.get().asFile.path, "--language", "kotlin", "--no-format",
        "--out-dir", uniffiKotlin.get().asFile.path)
}
```

Inside `android {`:
- Add `ndkVersion = "28.2.13676358"` after `compileSdk = 36`.
- Add:

  ```kotlin
      sourceSets.getByName("main") {
          java.srcDir(uniffiKotlin)
          jniLibs.srcDir(rustJniLibs)
      }
  ```

After `android { … }`:

```kotlin
val rustAndroidLibraries by tasks.registering(Exec::class) {
    inputs.files(replicaSources)
    outputs.dir(rustJniLibs)
    workingDir = replicaCrate
    environment("ANDROID_NDK_HOME", android.ndkDirectory.path)
    commandLine("cargo", "+$rustToolchain", "ndk", "-t", "arm64-v8a", "-t", "armeabi-v7a", "-t", "x86_64",
        "--platform", "26", "-o", rustJniLibs.get().asFile.path, "build", "--release", "--locked",
        "--target-dir", rustBuild.get().dir("android").asFile.path)
}
tasks.named("preBuild") { dependsOn(uniffiBindings) }
tasks.matching { it.name.matches(Regex("merge.*JniLibFolders")) }.configureEach { dependsOn(rustAndroidLibraries) }
tasks.withType<Test>().configureEach {
    dependsOn(rustHostLibrary)
    systemProperty("uniffi.component.zerus_replica_ffi.libraryOverride", hostLibrary.get().asFile.path)
}
```

In `dependencies {`, add:

```kotlin
    // The task replica's bindings reach the Rust library through JNA.
    implementation("net.java.dev.jna:jna:5.17.0@aar")
    testImplementation("net.java.dev.jna:jna:5.17.0")
```

In `mobile/android/app/proguard-rules.pro`, append:

```
# The task replica's UniFFI bindings call the Rust library through JNA, which finds
# its classes and the bindings' structures by reflection.
-keep class com.sun.jna.** { *; }
-keep class * implements com.sun.jna.** { *; }
-keep class app.zerus.mobile.replica.** { *; }
-dontwarn java.awt.**
```

- [ ] **Step 4: Run the binding test and the app's tests**

Run: `cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon testDebugUnitTest lintRelease`.

Expected: BUILD SUCCESSFUL. `TaskStoreBindingTest` passes 3 tests, and the 111 existing test classes still pass. If lint reports issues in the generated sources, exclude `build/generated/uniffi` from lint (`lint { ignoreTestSources … }` does not cover it; use `lint { checkGeneratedSources = false }`) and ledger it.

- [ ] **Step 5: The APK carries the libraries; measure them**

Run:
- `cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon assembleDebug`
- `unzip -l app/build/outputs/apk/debug/app-debug.apk | grep libzerus_replica_ffi`
- for each ABI, `$ANDROID_HOME/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -lW app/build/rust/jniLibs/<abi>/libzerus_replica_ffi.so | grep LOAD`
- `ls -l app/build/rust/jniLibs/*/libzerus_replica_ffi.so`

Expected:
- three entries in the APK, `lib/arm64-v8a/`, `lib/armeabi-v7a/` and `lib/x86_64/`;
- LOAD alignment `0x4000` for arm64-v8a and x86_64.

Record the size of each library and of the APK compared with the baseline debug APK in "Measurements" below. The baseline is the debug APK built before Step 3. Before editing `build.gradle.kts`, run `./gradlew --no-daemon assembleDebug` once and note `ls -l app/build/outputs/apk/debug/app-debug.apk`.

Then run `./gradlew --no-daemon :app:minifyReleaseWithR8` (it needs no signing). Expected: BUILD SUCCESSFUL, no missing-class errors for JNA or the bindings. If R8 asks for signing first, ledger it and rely on the keep rules.

- [ ] **Step 6: Commit**

```bash
git add mobile/android/app/build.gradle.kts mobile/android/app/proguard-rules.pro mobile/android/app/src/test/java/app/zerus/mobile/replica/TaskStoreBindingTest.kt
git commit -m "Build the task replica for each Android ABI and generate its Kotlin bindings"
```

---

### Task 4: The licence notices of the library ship with the app

**Files:**
- Modify: `scripts/collect-licenses.py`. Add `--target TRIPLE`; the default is the host.
- Modify: `mobile/android/app/build.gradle.kts`. Add the `rustLicenses` task and the generated assets directory.
- Create: `mobile/android/app/src/main/assets/licenses/jna.txt`.
- Modify: `mobile/android/app/src/main/java/app/zerus/mobile/ThirdPartyUi.kt`.

**Interfaces:**
- Consumes (Tasks 2 and 3): the crate's `Cargo.lock` and the Gradle build.
- Produces: the assets `rust-licenses/inventory.json` and `rust-licenses/<crate>-<version>/…`.

- [ ] **Step 1: `--target` in the collector, test first**

Run the collector as it is: `python3 scripts/collect-licenses.py .ci-build/notices --manifest-path crates/replica-ffi/Cargo.toml --target aarch64-linux-android`.

Expected: FAIL with "unrecognized arguments: --target aarch64-linux-android".

In `scripts/collect-licenses.py`:
- add `parser.add_argument("--target", help="the Rust target the binaries are built for [default: this host]")`;
- replace the `target = next(…)` line with `target = args.target or next(…)`, keeping the host lookup as the fallback.

Run the command again. Expected: "Collected notices for N host-target dependencies." with N ≥ 20, and `.ci-build/notices/inventory.json` names `"target": "aarch64-linux-android"` and contains `loro` and `uniffi`. Remove `.ci-build/notices` afterwards.

- [ ] **Step 2: Gradle collects them into the app's assets**

In `mobile/android/app/build.gradle.kts`, next to the Rust tasks:

```kotlin
// The licence notices of every crate in the library, for Third-party notices; the
// collector fails the build when a crate ships none.
val rustLicenseAssets = layout.buildDirectory.dir("generated/rust-licenses")
val rustLicenses by tasks.registering(Exec::class) {
    inputs.file(replicaCrate.resolve("Cargo.lock"))
    outputs.dir(rustLicenseAssets)
    doFirst { delete(rustLicenseAssets) }
    commandLine("python3", rootProject.file("../../scripts/collect-licenses.py").path,
        rustLicenseAssets.get().dir("rust-licenses").asFile.path,
        "--manifest-path", replicaCrate.resolve("Cargo.toml").path, "--target", "aarch64-linux-android")
}
tasks.matching { it.name.matches(Regex("merge.*Assets")) }.configureEach { dependsOn(rustLicenses) }
```

In `android { sourceSets.getByName("main") { … } }`, add `assets.srcDir(rustLicenseAssets)`.

The collector runs `cargo metadata --offline`. In CI the crate's dependencies are downloaded when the library builds first, so `rustLicenses` must run after `rustHostLibrary`: add `dependsOn(rustHostLibrary)` to it.

- [ ] **Step 3: JNA's notice, and the Third-party notices section**

Copy JNA's Apache License 2.0 text from its jar into `mobile/android/app/src/main/assets/licenses/jna.txt`, with a first line `JNA 5.17.0 — Java Native Access, Apache License 2.0 (dual-licensed with LGPL 2.1; used under Apache 2.0).`:

`unzip -p $(find ~/.gradle/caches -name 'jna-5.17.0.jar' | head -1) META-INF/AL2.0`

If the jar has no `META-INF/AL2.0`, use its `META-INF/LICENSE` or the Apache 2.0 text of `licenses/camerax.txt`, and ledger that.

In `ThirdPartyUi.kt`, add a state and a section after the Markdown licences:

```kotlin
    val replicaLicenses by produceState("Loading task store licenses…",context) {
        value = withContext(Dispatchers.IO) { runCatching { rustNotices(context) }.getOrDefault("License texts are unavailable.") }
    }
```

```kotlin
        Text("Task store — Loro, UniFFI and the other Rust crates of zerus-replica; JNA 5.17.0 (Apache License 2.0).",style = MaterialTheme.typography.titleSmall)
        Text("Sources: github.com/loro-dev/loro; github.com/mozilla/uniffi-rs; github.com/java-native-access/jna",style = MaterialTheme.typography.bodySmall)
        Text(replicaLicenses,style = MaterialTheme.typography.bodySmall)
```

And at the end of the file:

```kotlin
/** Each crate of the task store's library with its licence and notice texts, then JNA's. */
private fun rustNotices(context: android.content.Context): String {
    val assets = context.assets
    val inventory = org.json.JSONObject(assets.open("rust-licenses/inventory.json").bufferedReader().use { it.readText() })
    val packages = inventory.getJSONArray("packages")
    val crates = (0 until packages.length()).joinToString("\n\n") { index ->
        val crate = packages.getJSONObject(index)
        val folder = "rust-licenses/${crate.getString("name")}-${crate.getString("version")}"
        val texts = assets.list(folder).orEmpty().sorted().joinToString("\n\n") { file ->
            assets.open("$folder/$file").bufferedReader().use { it.readText() }
        }
        "${crate.getString("name")} ${crate.getString("version")} (${crate.optString("license")})\n$texts"
    }
    return crates + "\n\n" + assets.open("licenses/jna.txt").bufferedReader().use { it.readText() }
}
```

`assets.list` returns only the files of one folder. A crate whose notices sit in subfolders shows only its top-level files. Check `find app/build/generated/rust-licenses -mindepth 3 -type f`; if anything is listed, walk the folders recursively instead, and ledger it.

- [ ] **Step 4: Build and check the assets**

Run: `cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon testDebugUnitTest lintRelease assembleDebug && unzip -l app/build/outputs/apk/debug/app-debug.apk | grep -c "assets/rust-licenses/" && unzip -p app/build/outputs/apk/debug/app-debug.apk assets/rust-licenses/inventory.json | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['target'], len(d['packages']), sorted(p['name'] for p in d['packages'])[:5])"`.

Expected: BUILD SUCCESSFUL; dozens of `assets/rust-licenses/` entries; target `aarch64-linux-android`, with `loro` and `uniffi` among the packages.

- [ ] **Step 5: Commit**

```bash
git add scripts/collect-licenses.py mobile/android/app/build.gradle.kts mobile/android/app/src/main/assets/licenses/jna.txt mobile/android/app/src/main/java/app/zerus/mobile/ThirdPartyUi.kt
git commit -m "Ship the licence notices of the task store's Rust crates and JNA in the Android app"
```

---

### Task 5: CI builds the library for the three ABIs and runs the binding tests; docs

**Files:**
- Modify: `.github/workflows/checks.yml`:
  - the quick Rust job, where `crates/replica` is tested (about lines 116-121);
  - the `android` job (about lines 413-470).
- Modify: `tests/test_ci_security.py`. It pins the exact command lines it finds in `checks.yml`.
- Modify: `mobile/android/README.md`. The "Build and install" prerequisites.
- Modify: `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`. Point "Android build" at the measured sizes in this plan.
- Modify: this plan's "Measurements".

- [ ] **Step 1: The CI test first**

In `tests/test_ci_security.py`, next to the existing assertion for `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --target-dir target/replica` (about line 476), add the FFI crate's lines to the expected list:
- `cargo +1.99.1 fmt --check --manifest-path crates/replica-ffi/Cargo.toml`
- `cargo +1.99.1 clippy --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi --all-targets -- -D warnings`
- `cargo +1.99.1 test --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi`

Add a test for the `android` job:

```python
    def test_android_job_builds_the_task_replica_for_every_abi_with_the_pinned_toolchain(self):
        job = self.workflow_job("checks.yml", "android")
        self.assertIn("rustup toolchain install 1.99.1 --profile minimal --target aarch64-linux-android,armv7-linux-androideabi,x86_64-linux-android", job)
        self.assertIn("cargo +1.99.1 install cargo-ndk --version 4.1.2 --locked", job)
        self.assertIn("'ndk;28.2.13676358'", job)
        self.assertIn("./gradlew --no-daemon testDebugUnitTest lintRelease assembleDebug -PzerusFirebase=true", job)
```

`workflow_job` stands for however `test_ci_security.py` reads one job's text. Use its existing helper (look for how it reads the `android` job, or the text between `  android:` and the next job) and ledger the name.

Run: `python3 -m unittest discover -s tests -p test_ci_security.py -v`. Expected: the new assertions FAIL.

- [ ] **Step 2: The workflow**

In `.github/workflows/checks.yml`:

(a) In the quick job, after the three `crates/replica` lines, add the three FFI crate lines from Step 1, with the same indentation.

(b) In the `android` job, before "Android platform and build tools", add:

```yaml
      - name: Rust and cargo-ndk for the task replica
        run: |
          rustup toolchain install 1.99.1 --profile minimal --target aarch64-linux-android,armv7-linux-androideabi,x86_64-linux-android
          cargo +1.99.1 install cargo-ndk --version 4.1.2 --locked
```

Then change the sdkmanager line to `"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" 'platforms;android-36' 'build-tools;36.0.0' 'ndk;28.2.13676358'`, and the Gradle line to `run: ./gradlew --no-daemon testDebugUnitTest lintRelease assembleDebug -PzerusFirebase=true`. Raise `timeout-minutes` from 30 to 45: three release builds with LTO are added.

Run: `python3 -m unittest discover -s tests -p test_ci_security.py -v`. Expected: pass.

- [ ] **Step 3: Docs**

In `mobile/android/README.md`, "Build and install", replace the first paragraph with:

```markdown
Install JDK 17 or 21, Android SDK platform 36 / build tools 36.0.0 and NDK
28.2.13676358, Rust 1.99.1 with the targets `aarch64-linux-android`,
`armv7-linux-androideabi` and `x86_64-linux-android`, and
`cargo install cargo-ndk --version 4.1.2 --locked`. Set `JAVA_HOME` and
`ANDROID_HOME` for those tools. The build compiles the task replica
(`crates/replica-ffi`) for each ABI, generates its Kotlin bindings with UniFFI
from the host library, which the unit tests load, and collects the Rust crates'
licence notices into Third-party notices. Then run:
```

In the replication spec's "Android build", append to the CI bullet: ` The measured sizes are in docs/superpowers/plans/2026-10-19-task-replica-android-bindings.md.`

Fill "Measurements" below with the numbers from Task 3 Step 5.

- [ ] **Step 4: The whole check**

Run:
- `cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml`
- `cargo +1.99.1 test --locked --manifest-path crates/replica-ffi/Cargo.toml --target-dir target/replica-ffi`
- `cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings && cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks tests.test_task_routing tests.test_swarm`
- `python3 -m unittest discover -s tests -p test_ci_security.py`
- `(cd mobile/android && env JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon testDebugUnitTest lintRelease assembleDebug)`
- `python3 scripts/ci/check-source.py`

Expected: everything passes.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/checks.yml tests/test_ci_security.py mobile/android/README.md docs/superpowers/specs/2026-10-18-task-replication-loro-design.md docs/superpowers/plans/2026-10-19-task-replica-android-bindings.md
git commit -m "Build the task replica for every Android ABI in CI and document the Android toolchain"
```

## Measurements

Measured on 2026-10-19 with Rust 1.99.1, NDK 28.2.13676358, cargo-ndk 4.1.2 and the release profile of `crates/replica-ffi` (`opt-level = "z"`, LTO, one codegen unit, stripped):

| ABI | `libzerus_replica_ffi.so` | LOAD alignment |
|---|---|---|
| arm64-v8a | 3 071 616 B | 0x4000 |
| armeabi-v7a | 1 861 652 B | 0x1000 |
| x86_64 | 3 585 176 B | 0x4000 |

The debug APK, packaged from scratch, went from 25 909 949 B to 35 650 702 B (+9 740 753 B). Of that:
- the three libraries are 8 518 444 B, stored uncompressed and page aligned;
- JNA's `libjnidispatch.so` for the same ABIs is about 0.4 MB;
- the Rust crates' licence notices (148 crates for arm64 then; 149 once all three ABIs' crates are collected, adding `spin` for x86_64) are about 620 KB compressed;
- the rest is the generated bindings.

An APK carries all three libraries; an App Bundle would deliver only the device's. With `opt-level = "s"` the arm64 library was 3 363 384 B.
