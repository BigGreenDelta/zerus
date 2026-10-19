# Task replication, phase 2: computers — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every computer of the swarm holds every task: the swarm worker replicates the task documents after the catalog, and Send now and Cancel for a session on another computer run on that computer.

**Architecture:** `zerus-replica` gains the sync API the spec names — a project's inventory (each document's version), its digest, the updates another replica lacks in pages, and imports that refuse a bad document alone — with an inventory cache keyed by file stamps. In hgs, `swarm/replica.rs` advertises the capability and per-project digests in `hello`, answers `swarm replica-inventory` and `swarm replica-exchange`, and runs after each catalog sync; the worker also syncs at once when any task changes. `hgs task send|cancel` for a session on another computer replicates the project with that member, runs the command there over SSH and replicates back, so agents and the desktop share the routing; the desktop only stops blocking such sessions and names members that do not share tasks.

**Tech Stack:** Rust 1.99.1 (hgs and `crates/replica`, Loro `=1.16.2`), Python `unittest` through the real CLI with isolated nodes and a fake SSH client, Qt 6 / C++17 QtTest offscreen.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md` (Synchronization: inventory and digest, between computers; Actions with side effects; Compatibility and upgrades: mixed swarms; Limits; Failure cases; Testing: sync tests and side-effect routing; Phases: 2) and the Desktop interface of `docs/superpowers/specs/2026-10-13-project-task-queue-design.md`.

## Global Constraints

- Work on branch `task-tracker`, which the shared main checkout has checked out; never switch branches, merge, rebase or push. Other sessions may work in the same checkout: stage only this plan's files.
- Never rebuild binaries a running Zerus uses: `target/` belongs to the live CLI and `tray/build` to the running GUI. Build hgs with `cargo +1.99.1 build --target-dir .ci-build/cargo` and run the Python suites with `HGS_TEST_BIN=.ci-build/cargo/debug/hgs`; build the GUI in `.ci-build/gui` (configured as in the phase 3b plan). The crate's own tests run in `crates/replica` (its own `target/`).
- Rust: `cargo +1.99.1`; the replica crate stays rustfmt-formatted and clippy-clean with `-D warnings` (CI checks both); hgs stays clippy-clean with `-D warnings` and keeps its compact style (CI does not run rustfmt on it).
- `crates/replica` stays the only code that imports `loro`; hgs sees versions, inventories and updates, never a Loro type.
- Shared files an older hgs on the same computer reads keep their format: `swarm/catalog.json` peers gain no field; a new member list lives in its own file beside it, as `outdated-peers.json` does.
- Limits from the spec: a page carries at most 1 MiB of updates; a send or cancel for an unreachable computer is refused with the reason and changes nothing.
- Synthetic names only (`orchard`, `codex/api`, `codex/remote`, nodes `a`/`b`/`c`); never real project or session names.

## Decisions this plan takes

The spec leaves these open; each is the smallest reading that keeps its rules.

- **Capability.** `hello` gains `replica: {protocol: 1, schema, projects: {ID: digest}}`. A member without it, or with another protocol, is skipped and named (`tasks_outdated`). Documents of every schema are sent: the spec's schema rule has an older replica keep a newer document read-only, so "skip what they cannot read" applies to the exchange protocol, not to the schema.
- **Inventory.** `{project: version, tasks: {UUID: version}}`, a version being Loro peer (decimal text) → change count, sorted, so digests agree across replicas. The stage is left out until the phone (phase 5) needs it. A local cache `inventory.json` keeps each version with the length, modification time and inode of its files, so the worker's check every 3 s reads no unchanged task.
- **Pages.** At most 1 MiB of update bytes per page (before base64); a single document larger than that travels alone; at most 64 pages of one project per sync, the next sync continues. Each page is one `replica-inventory` and one `replica-exchange` call.
- **Refusals.** A document that cannot be imported (another project, another UUID, bytes that do not decode, a local copy that does not load) is refused alone, named in the worker's log, and never fails the sync. A damaged local file stays as it is (the Tasks tab names it); it is not replaced by a member's copy.
- **Routing in hgs.** `hgs task send|cancel` decides the session (`--session` now takes `--host`), and for another computer's session replicates the project with that member, runs `hgs task send|cancel` there through `~/.local/bin/hgs` over SSH (as the catalog does), and replicates back. The forwarded command names an agent caller with `--agent NODE/SESSION`; the computer that runs the session applies the project's agent rules and records the action. A Cancel for an unreachable computer is now refused, where phase 1 recorded it here with "message not sent".
- **Desktop.** A session on another computer takes Send now like a local one (its own blocker still applies: gone, not running, waiting for input); an Any session task sent to an open session on another computer passes `--host`. The Swarm settings' computer status adds "· Update Zerus there to share tasks".

## Review Focus

- A forwarded send cut off midway (the connection drops after that computer pasted and recorded, before the copy comes back): its record replicates with the next sync, and a repeated Send now is refused by that computer's copy ("already In progress") — `test_a_task_its_computer_shows_in_progress_is_refused` covers the refusal; check the interrupted path by reading `forwarded()`.
- Local writes during an exchange: the exchange holds the project lock only between network calls, so a GUI edit between the inventory and the import merges through Loro — check that no path imports into a stale in-memory document and saves over a newer file.
- The inventory cache on file systems with coarse timestamps: every write either appends (the length changes) or replaces the file atomically (the inode changes), so a missed change needs an in-place rewrite of equal length within one timestamp tick — confirm no write path rewrites in place.
- Cost on large swarms: the worker's 3 s fingerprint stats every task file of every project and opens each project store (its lock) — check it never loads unchanged documents and never waits on a long-held lock.
- Mixed members: an older member calling this hgs's `hello`, `inventory` and `exchange` sees only an extra `replica` field; a member with replica support but an older crate keeps newer documents read-only — `test_members_without_task_replication_are_skipped_and_named` covers the skip.

## File Structure

| Path | Responsibility |
|---|---|
| `crates/replica/src/sync.rs`, `lib.rs`, `store.rs` | Inventory, digest, paged updates, imports, the inventory cache, stored projects |
| `crates/replica/tests/sync.rs` | Two stores meeting, merging, paging, refusing |
| `src/swarm/replica.rs` | The capability, `replica-inventory` / `replica-exchange`, a member's sync, on-demand `replicate` |
| `src/swarm/transport.rs`, `mod.rs`, `store.rs` | `hello` with the capability, the replica sync after the catalog, the worker's trigger, `remote_hgs`, the `tasks_outdated` list |
| `src/tasks.rs` | `send --session S --host H`, routing send and cancel to the session's computer, `--agent` |
| `tests/test_swarm.py`, `tests/test_task_routing.py`, `tests/test_tasks.py` | Replication through the CLI with three nodes; routing with a scripted agent on node b |
| `tray/src/TasksView.cpp`, `tray/src/SwarmDialog.cpp`, `tray/tests/test_tasksview.cpp`, `tray/tests/test_swarmcontroller.cpp` | Sends to other computers' sessions; members that do not share tasks |
| `docs/reference.md` | Replication and routing in Project tasks |

**Known limits of this phase:** the desktop waits up to 30 s for a task command; a forwarded send over a slow link that takes longer is reported as timed out although it may complete, and a repeated Send now is then refused by that computer's copy. Windows keeps no verified bindings, so it neither pins to nor forwards to other computers (as before). A member is reached through `~/.local/bin/hgs`, like the catalog sync. The MCP tools (phase 4) and the phone (phase 5) are not part of this plan. Existing, outside this plan: `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7 of `test_sessionswindow` on some machines also without this work.

---

### Task 0: Record the plan

- [ ] **Step 1: Check the branch and privacy**

Run: `git branch --show-current && python3 scripts/check-source-privacy.py`
Expected: `task-tracker`; `0 privacy guard findings`.

- [ ] **Step 2: Commit the plan**

```bash
git add docs/superpowers/plans/2026-10-19-task-replication-phase2-computers.md
git commit -m "Plan task replication between computers"
```

---

### Task 1: The sync API in the replica crate

**Files:**
- Create: `crates/replica/tests/sync.rs`, `crates/replica/src/sync.rs`
- Modify: `crates/replica/src/lib.rs`, `crates/replica/src/store.rs`

**Interfaces:**
- Consumes: `ProjectStore` (`open`, `uuids`, `load`, `save`, `project`, `save_project`), `TaskDoc` (`current_vv`, `export_since_vv`, `import_block`, `snapshot`, `view`), `ProjectDoc` (`version`, `export_since`, `import`) from phase 1.
- Produces: in `zerus_replica`: `pub type Version = BTreeMap<String, i32>`; `pub struct Inventory { pub project: Version, pub tasks: BTreeMap<String, Version> }` (Serialize, Deserialize, Default, PartialEq) with `digest() -> String` (SHA-256 hex of its canonical JSON); `pub enum Document { Project, Task(String) }` (serde snake_case: `"project"` or `{"task": UUID}`); `pub struct Update { pub document: Document, pub bytes: Vec<u8> }`; `pub struct Imported { pub changed: usize, pub refused: Vec<Damaged> }`; on `ProjectStore`: `stored(root: &Path) -> Result<Vec<String>>` (sorted project IDs), `inventory() -> Result<(Inventory, Vec<Damaged>)>`, `updates_for(&Inventory, limit: usize) -> Result<(Vec<Update>, bool more)>`, `import_updates(Vec<Update>) -> Result<Imported>`. `write_atomic`, `dir()`, `snapshot_path`, `updates_path` become `pub(crate)`.

A replica that lacks a document receives its snapshot; one that holds part of it receives the Loro updates since its version. The cache `inventory.json` in the project's directory is only a speed-up: a missing or damaged one means reading the tasks again.

- [ ] **Step 1: Write the failing tests**

Create `crates/replica/tests/sync.rs`:

````rust
//! Replicating a project between two stores: inventories, digests, paged
//! updates and imports, as computers exchange them.
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;
use zerus_replica::{
    Actor, Document, Inventory, NewTask, ProjectStore, Settings, Stage, Target, TaskDoc, Update,
};

fn store(root: &Path) -> ProjectStore {
    ProjectStore::open(root, "project-1").unwrap()
}

fn add(store: &ProjectStore, text: &str) -> TaskDoc {
    let mut task = TaskDoc::create(NewTask {
        project: store.project_id(),
        id: &store.unused_short_id().unwrap(),
        text,
        stage: Stage::Todo,
        target: Target::Any,
        author: Actor::Person,
        rank: "V",
        now: 1,
    })
    .unwrap();
    store.save(&mut task).unwrap();
    task
}

/// Everything `from` holds that `to` lacks, page by page.
fn replicate(from: &ProjectStore, to: &ProjectStore, limit: usize) -> usize {
    let mut pages = 0;
    loop {
        let (theirs, _) = to.inventory().unwrap();
        let (updates, more) = from.updates_for(&theirs, limit).unwrap();
        if updates.is_empty() {
            return pages;
        }
        pages += 1;
        let imported = to.import_updates(updates).unwrap();
        assert!(imported.refused.is_empty(), "{:?}", imported.refused);
        if !more {
            return pages;
        }
    }
}

/// Text that does not compress: snapshots are compressed.
fn noise(seed: &str, length: usize) -> String {
    let mut out = String::new();
    while out.len() < length {
        out += &format!("{:x}", Sha256::digest(format!("{seed}-{}", out.len())));
    }
    out.truncate(length);
    out
}

fn texts(store: &ProjectStore) -> Vec<String> {
    let (tasks, _) = store.listing().unwrap();
    let mut out: Vec<String> = tasks.iter().map(|t| t.view().unwrap().text).collect();
    out.sort();
    out
}

#[test]
fn two_replicas_meet_and_then_agree() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    add(&a, "Add rate limiting");
    add(&a, "Document the setup");
    let mut doc = a.project().unwrap();
    doc.set_settings(&Settings {
        agent_limit: 3,
        ..doc.settings()
    })
    .unwrap();
    a.save_project(&doc).unwrap();
    assert_ne!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
    replicate(&a, &b, 1 << 20);
    assert_eq!(texts(&b), ["Add rate limiting", "Document the setup"]);
    assert_eq!(b.project().unwrap().settings().agent_limit, 3);
    // Equal versions, equal digests; nothing is left to send either way.
    assert_eq!(a.inventory().unwrap().0, b.inventory().unwrap().0);
    assert_eq!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
    assert!(a
        .updates_for(&b.inventory().unwrap().0, 1 << 20)
        .unwrap()
        .0
        .is_empty());
    assert!(b
        .updates_for(&a.inventory().unwrap().0, 1 << 20)
        .unwrap()
        .0
        .is_empty());
}

#[test]
fn edits_made_on_both_sides_merge() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    let task = add(&a, "Add rate limiting");
    replicate(&a, &b, 1 << 20);
    let mut here = a.load(task.uuid()).unwrap();
    here.comment("Use the config value.", &Actor::Person, 2)
        .unwrap();
    a.save(&mut here).unwrap();
    let mut there = b.load(task.uuid()).unwrap();
    there.set_stage(Stage::Backlog, &Actor::Person, 3).unwrap();
    b.save(&mut there).unwrap();
    replicate(&a, &b, 1 << 20);
    replicate(&b, &a, 1 << 20);
    for side in [&a, &b] {
        let view = side.load(task.uuid()).unwrap().view().unwrap();
        assert_eq!(view.stage, Stage::Backlog);
        assert!(view
            .log
            .iter()
            .any(|e| e.kind == "comment" && e.text == "Use the config value."));
    }
    assert_eq!(
        a.inventory().unwrap().0.digest(),
        b.inventory().unwrap().0.digest()
    );
}

#[test]
fn updates_come_in_pages_and_a_large_document_alone() {
    let (a, b) = (tempfile::tempdir().unwrap(), tempfile::tempdir().unwrap());
    let (a, b) = (store(a.path()), store(b.path()));
    for n in 0..6 {
        add(&a, &noise(&n.to_string(), 2000));
    }
    // A page stays within its limit (each snapshot here is about 5 KB) unless one
    // document alone exceeds it.
    let (theirs, _) = b.inventory().unwrap();
    let (page, more) = a.updates_for(&theirs, 12_000).unwrap();
    assert!(more);
    assert!(
        page.len() >= 2 && page.iter().map(|u| u.bytes.len()).sum::<usize>() <= 12_000,
        "{:?}",
        page.iter().map(|u| u.bytes.len()).collect::<Vec<_>>()
    );
    assert!(replicate(&a, &b, 12_000) >= 2);
    assert_eq!(texts(&b).len(), 6);
    let big = add(&a, &noise("big", 20_000));
    let (page, _) = a.updates_for(&b.inventory().unwrap().0, 1000).unwrap();
    assert_eq!(page.len(), 1);
    assert_eq!(page[0].document, Document::Task(big.uuid().to_owned()));
}

#[test]
fn a_document_that_cannot_be_imported_is_refused_alone() {
    let (here, there, other) = (
        tempfile::tempdir().unwrap(),
        tempfile::tempdir().unwrap(),
        tempfile::tempdir().unwrap(),
    );
    let (a, b) = (store(here.path()), store(there.path()));
    let good = add(&a, "Add rate limiting");
    let foreign = ProjectStore::open(other.path(), "project-2").unwrap();
    let stranger = add(&foreign, "Elsewhere");
    let updates = vec![
        Update {
            document: Document::Task(stranger.uuid().to_owned()),
            bytes: stranger.snapshot().unwrap(),
        },
        Update {
            document: Document::Task("not-a-uuid".into()),
            bytes: good.snapshot().unwrap(),
        },
        Update {
            document: Document::Task("8f14e45f-ceea-4e7a-9b1c-2f6d1a3b5c7d".into()),
            bytes: b"garbage".to_vec(),
        },
        Update {
            document: Document::Task("8f14e45f-ceea-4e7a-9b1c-2f6d1a3b5c7e".into()),
            bytes: good.snapshot().unwrap(),
        },
        Update {
            document: Document::Task(good.uuid().to_owned()),
            bytes: good.snapshot().unwrap(),
        },
    ];
    let imported = b.import_updates(updates).unwrap();
    assert_eq!(imported.changed, 1);
    let refused: Vec<&str> = imported.refused.iter().map(|d| d.error.as_str()).collect();
    assert_eq!(refused.len(), 4, "{refused:?}");
    assert!(refused[0].contains("another project"), "{}", refused[0]);
    assert!(refused[1].contains("not a task UUID"), "{}", refused[1]);
    assert!(refused[3].contains("another task"), "{}", refused[3]);
    assert_eq!(texts(&b), ["Add rate limiting"]);
    // A local task that fails to load is left out of the inventory, named, and
    // never stops the others.
    let broken = add(&b, "Broken here");
    fs::write(task_file(there.path(), broken.uuid()), b"damaged").unwrap();
    let (inventory, damaged) = b.inventory().unwrap();
    assert_eq!(damaged.len(), 1);
    assert!(!inventory.tasks.contains_key(broken.uuid()));
    add(&a, "Still replicated");
    replicate(&a, &b, 1 << 20);
    assert_eq!(texts(&b), ["Add rate limiting", "Still replicated"]);
}

fn task_file(root: &Path, uuid: &str) -> std::path::PathBuf {
    root.join(format!("{:x}", Sha256::digest(b"project-1")))
        .join("tasks")
        .join(format!("{uuid}.loro"))
}

#[test]
fn the_inventory_reads_a_task_again_only_when_its_files_change() {
    let root = tempfile::tempdir().unwrap();
    let a = store(root.path());
    let task = add(&a, "Add rate limiting");
    let (first, _) = a.inventory().unwrap();
    let cache = root
        .path()
        .join(format!("{:x}", Sha256::digest(b"project-1")))
        .join("inventory.json");
    // While the task's files stay as they were, the cached version is used as is.
    let text = fs::read_to_string(&cache).unwrap();
    let peer = first.tasks[task.uuid()].keys().next().unwrap().clone();
    fs::write(&cache, text.replace(&format!("\"{peer}\""), "\"7\"")).unwrap();
    assert!(a.inventory().unwrap().0.tasks[task.uuid()].contains_key("7"));
    // A change to them reads the task again.
    let mut task = a.load(task.uuid()).unwrap();
    task.comment("Later", &Actor::Person, 2).unwrap();
    a.save(&mut task).unwrap();
    let (later, _) = a.inventory().unwrap();
    assert!(later.tasks[task.uuid()].contains_key(&peer));
    assert_ne!(later, first);
}

#[test]
fn stored_projects_are_listed_by_id() {
    let root = tempfile::tempdir().unwrap();
    assert!(ProjectStore::stored(root.path()).unwrap().is_empty());
    drop(store(root.path()));
    drop(ProjectStore::open(root.path(), "team/web").unwrap());
    assert_eq!(
        ProjectStore::stored(root.path()).unwrap(),
        ["project-1", "team/web"]
    );
    assert!(ProjectStore::stored(&root.path().join("missing"))
        .unwrap()
        .is_empty());
    let _ = Inventory::default();
}
````

- [ ] **Step 2: Run them to see them fail**

Run: `(cd crates/replica && cargo +1.99.1 test --locked -q --test sync)`
Expected: FAIL to compile — `unresolved imports zerus_replica::Document, zerus_replica::Inventory, zerus_replica::Update`, and no `inventory`, `updates_for`, `import_updates` or `stored` on `ProjectStore`.

- [ ] **Step 3: Implement**

Apply this change to `crates/replica/src/lib.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/src/lib.rs b/crates/replica/src/lib.rs
index b763e3d7..449f1cbc 100644
--- a/crates/replica/src/lib.rs
+++ b/crates/replica/src/lib.rs
@@ -8,6 +8,7 @@ pub mod rank;
 mod render;
 mod schema;
 mod store;
+mod sync;
 mod task;
 
 pub use error::{Error, ErrorKind, Result};
@@ -17,4 +18,5 @@ pub use schema::{
     Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
 };
 pub use store::{Damaged, Position, ProjectStore};
+pub use sync::{Document, Imported, Inventory, Update, Version};
 pub use task::{NewTask, TaskDoc};
````

Apply this change to `crates/replica/src/store.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/src/store.rs b/crates/replica/src/store.rs
index e25bf1c1..a0c14ff0 100644
--- a/crates/replica/src/store.rs
+++ b/crates/replica/src/store.rs
@@ -64,7 +64,7 @@ fn private_dir(path: &Path) -> Result<()> {
     Ok(())
 }
 
-fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
+pub(crate) fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
     let temp = path.with_extension(format!("tmp-{}", ids::new_uuid()));
     let mut file = private_options().create_new(true).write(true).open(&temp)?;
     file.write_all(bytes)?;
@@ -160,6 +160,10 @@ impl ProjectStore {
         &self.project
     }
 
+    pub(crate) fn dir(&self) -> &Path {
+        &self.dir
+    }
+
     pub fn project(&self) -> Result<ProjectDoc> {
         match fs::read(self.dir.join("project.loro")) {
             Ok(bytes) => ProjectDoc::from_bytes(&bytes),
@@ -172,11 +176,11 @@ impl ProjectStore {
         write_atomic(&self.dir.join("project.loro"), &doc.snapshot()?)
     }
 
-    fn snapshot_path(&self, uuid: &str) -> PathBuf {
+    pub(crate) fn snapshot_path(&self, uuid: &str) -> PathBuf {
         self.dir.join("tasks").join(format!("{uuid}.loro"))
     }
 
-    fn updates_path(&self, uuid: &str) -> PathBuf {
+    pub(crate) fn updates_path(&self, uuid: &str) -> PathBuf {
         self.dir.join("tasks").join(format!("{uuid}.updates"))
     }
 
````

Create `crates/replica/src/sync.rs`:

````rust
//! Replicating a project between replicas: what each holds (its inventory, the
//! version of every document), the updates the other lacks, and importing them.
//! The inventory is bookkeeping, not a log, so nothing here grows without bound.
use crate::store::{write_atomic, Damaged, ProjectStore};
use crate::task::TaskDoc;
use crate::{ids, Error, Result};
use loro::VersionVector;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::time::UNIX_EPOCH;

/// A document's version: each Loro peer that changed it (its ID as decimal
/// text) and how many changes it made. Sorted, so equal versions compare and
/// hash alike on every replica.
pub type Version = BTreeMap<String, i32>;

/// What a replica holds of one project.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Inventory {
    /// The project document; empty while it was never saved.
    pub project: Version,
    /// Each task document by UUID.
    pub tasks: BTreeMap<String, Version>,
}

impl Inventory {
    /// Equal for replicas that hold the same versions of the same documents.
    pub fn digest(&self) -> String {
        let canonical = serde_json::to_vec(self).expect("an inventory serializes");
        format!("{:x}", Sha256::digest(canonical))
    }
}

/// The document an update belongs to.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Document {
    Project,
    Task(String),
}

/// Loro updates for one document: what the other replica lacks of it, or the
/// whole document when it has none.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub document: Document,
    pub bytes: Vec<u8>,
}

/// What an import changed, and the documents it refused.
#[derive(Debug, Default)]
pub struct Imported {
    pub changed: usize,
    pub refused: Vec<Damaged>,
}

const CACHE: &str = "inventory.json";

/// What tells a file changed without reading it: its length, modification
/// time and, on Unix, its inode (an atomic rewrite gets a new one).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
struct Stamp(u64, u64, u32, u64);

fn stamp(path: &Path) -> Option<Stamp> {
    let meta = fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?.duration_since(UNIX_EPOCH).ok()?;
    #[cfg(unix)]
    let inode = std::os::unix::fs::MetadataExt::ino(&meta);
    #[cfg(not(unix))]
    let inode = 0;
    Some(Stamp(
        meta.len(),
        modified.as_secs(),
        modified.subsec_nanos(),
        inode,
    ))
}

/// The versions read last time, by the stamps of the files they came from.
#[derive(Default, PartialEq, Serialize, Deserialize)]
struct Cache {
    project: Option<Cached>,
    tasks: BTreeMap<String, Cached>,
}

#[derive(Clone, PartialEq, Serialize, Deserialize)]
struct Cached {
    files: Vec<Option<Stamp>>,
    version: Version,
}

fn version(vv: &VersionVector) -> Version {
    vv.iter()
        .map(|(peer, count)| (peer.to_string(), *count))
        .collect()
}

fn vector(version: &Version) -> Result<VersionVector> {
    version
        .iter()
        .map(|(peer, count)| {
            let peer = peer
                .parse::<u64>()
                .map_err(|_| Error::new(format!("invalid peer {peer:?} in a version")))?;
            Ok((peer, *count))
        })
        .collect()
}

/// Whether `theirs` holds every change of `ours`.
fn includes(theirs: &Version, ours: &Version) -> bool {
    ours.iter()
        .all(|(peer, count)| theirs.get(peer).is_some_and(|known| known >= count))
}

impl ProjectStore {
    /// The project IDs stored below `root`.
    pub fn stored(root: &Path) -> Result<Vec<String>> {
        let entries = match fs::read_dir(root) {
            Ok(entries) => entries,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(vec![]),
            Err(error) => return Err(error.into()),
        };
        let mut out = Vec::new();
        for entry in entries {
            if let Ok(id) = fs::read_to_string(entry?.path().join("project-id")) {
                out.push(id);
            }
        }
        out.sort();
        Ok(out)
    }

    /// The version of every document of the project, and the tasks that do not
    /// load. A task is read again only when its files changed since last time.
    pub fn inventory(&self) -> Result<(Inventory, Vec<Damaged>)> {
        let path = self.dir().join(CACHE);
        let old: Cache = fs::read(&path)
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        let mut cache = Cache::default();
        let mut inventory = Inventory::default();
        let mut damaged = Vec::new();
        let project = vec![stamp(&self.dir().join("project.loro"))];
        if project[0].is_some() {
            let cached = match old.project.as_ref().filter(|c| c.files == project) {
                Some(cached) => cached.clone(),
                None => Cached {
                    files: project,
                    version: version(&VersionVector::decode(&self.project()?.version())?),
                },
            };
            inventory.project = cached.version.clone();
            cache.project = Some(cached);
        }
        for uuid in self.uuids()? {
            let files = vec![
                stamp(&self.snapshot_path(&uuid)),
                stamp(&self.updates_path(&uuid)),
            ];
            let cached = match old.tasks.get(&uuid).filter(|c| c.files == files) {
                Some(cached) => cached.clone(),
                None => match self.load(&uuid) {
                    Ok(task) => Cached {
                        files,
                        version: version(&task.current_vv()),
                    },
                    Err(error) => {
                        damaged.push(Damaged {
                            uuid,
                            error: error.to_string(),
                        });
                        continue;
                    }
                },
            };
            inventory.tasks.insert(uuid.clone(), cached.version.clone());
            cache.tasks.insert(uuid, cached);
        }
        if cache != old {
            // Only a speed-up: an unwritten cache means reading the tasks again.
            let _ = write_atomic(&path, &serde_json::to_vec(&cache)?);
        }
        Ok((inventory, damaged))
    }

    /// The updates a replica holding `theirs` lacks, in pages of at most
    /// `limit` bytes; a document larger than that comes alone. True when more
    /// remain after this page.
    pub fn updates_for(&self, theirs: &Inventory, limit: usize) -> Result<(Vec<Update>, bool)> {
        let (ours, _) = self.inventory()?;
        let mut page = Vec::new();
        let mut size = 0;
        let mut add = |update: Update| {
            if !page.is_empty() && size + update.bytes.len() > limit {
                return false;
            }
            size += update.bytes.len();
            page.push(update);
            true
        };
        if !ours.project.is_empty() && !includes(&theirs.project, &ours.project) {
            let bytes = self
                .project()?
                .export_since(&vector(&theirs.project)?.encode())?;
            if !add(Update {
                document: Document::Project,
                bytes,
            }) {
                return Ok((page, true));
            }
        }
        for (uuid, mine) in &ours.tasks {
            let known = theirs.tasks.get(uuid);
            if known.is_some_and(|known| includes(known, mine)) {
                continue;
            }
            let task = self.load(uuid)?;
            let bytes = match known {
                Some(known) => task.export_since_vv(&vector(known)?)?,
                None => task.snapshot()?,
            };
            if !add(Update {
                document: Document::Task(uuid.clone()),
                bytes,
            }) {
                return Ok((page, true));
            }
        }
        Ok((page, false))
    }

    /// Imports another replica's updates. A document that cannot take them is
    /// refused and named; it never stops the others.
    pub fn import_updates(&self, updates: Vec<Update>) -> Result<Imported> {
        let mut imported = Imported::default();
        for update in updates {
            let (name, result) = match &update.document {
                Document::Project => ("project".to_owned(), self.import_project(&update.bytes)),
                Document::Task(uuid) => (uuid.clone(), self.import_task(uuid, &update.bytes)),
            };
            match result {
                Ok(changed) => imported.changed += usize::from(changed),
                Err(error) => imported.refused.push(Damaged {
                    uuid: name,
                    error: error.to_string(),
                }),
            }
        }
        Ok(imported)
    }

    fn import_project(&self, bytes: &[u8]) -> Result<bool> {
        let mut doc = self.project()?;
        let before = VersionVector::decode(&doc.version())?;
        doc.import(bytes)?;
        if VersionVector::decode(&doc.version())? == before {
            return Ok(false);
        }
        self.save_project(&doc)?;
        Ok(true)
    }

    fn import_task(&self, uuid: &str, bytes: &[u8]) -> Result<bool> {
        if !ids::is_uuid(uuid) {
            return Err(Error::new(format!("{uuid:?} is not a task UUID")));
        }
        let stored = self.snapshot_path(uuid).exists() || self.updates_path(uuid).exists();
        let mut task = if stored {
            let mut task = self.load(uuid)?;
            let before = task.current_vv();
            if !task.import_block(bytes) {
                return Err(Error::new("the updates could not be applied"));
            }
            if task.current_vv() == before {
                return Ok(false);
            }
            task
        } else {
            TaskDoc::from_bytes(bytes)?
        };
        if task.uuid() != uuid {
            return Err(Error::new("the update is for another task"));
        }
        let view = task.view()?;
        if view.project != self.project_id() {
            return Err(Error::new("the task belongs to another project"));
        }
        self.save(&mut task)?;
        Ok(true)
    }
}
````

- [ ] **Step 4: Run them to see them pass**

Run: `(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -q -- -D warnings && cargo +1.99.1 test --locked -q)`
Expected: no format or lint output; the unit tests `40 passed`, `sync` `6 passed`, `vectors` `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/src/lib.rs \
  crates/replica/src/store.rs \
  crates/replica/src/sync.rs \
  crates/replica/tests/sync.rs
git commit -m "Inventory, page and import task replicas"
```

---

### Task 2: Task replicas in the swarm

**Files:**
- Create: `src/swarm/replica.rs`
- Modify: `tests/test_swarm.py`, `src/swarm/mod.rs`, `src/swarm/store.rs`, `src/swarm/transport.rs`

**Interfaces:**
- Consumes: the sync API (Task 1); `transport::remote`, `sync_peer`, the worker loop, `LockedStore` (phase 0 swarm).
- Produces: `src/swarm/replica.rs` with `PROTOCOL = 1`, `hello(config) -> Result<Value>` (`{protocol, schema, projects}`), `fingerprint(config) -> Result<String>`, `inventory(config, Request)` and `exchange(config, Request)` for `hgs swarm replica-inventory` / `replica-exchange` (stdin `{project, inventory?, updates?: [{document, bytes: base64}]}`; the exchange answers `{imported, refused, updates, more}`), `sync_peer(config, alias, identity)`. `hgs swarm hello` and `inventory` gain `replica`; `transport::sync_peer` runs the replica sync after the catalog; the worker treats a changed task like a changed catalog. `LockedStore.replica_outdated` (file `replica-outdated-peers.json`) and `peers[alias].tasks_outdated` in the snapshot. The fake SSH of `tests/test_swarm.py` gains `noreplica-<alias>` (a member from before task replication).

The project lock is taken only between network calls. A project known to one side only exchanges too (the other side creates its directory), so empty projects converge after one exchange.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tests/test_swarm.py` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tests/test_swarm.py b/tests/test_swarm.py
index ab2bbfb7..660cb31d 100644
--- a/tests/test_swarm.py
+++ b/tests/test_swarm.py
@@ -2,6 +2,7 @@
 """Catalog replication through the real CLI, with isolated nodes and fake SSH."""
 import copy
 import fcntl
+import hashlib
 import json
 import os
 from pathlib import Path
@@ -21,6 +22,16 @@ if (root / ('offline-' + alias)).exists():
 env = dict(os.environ, HGS_CONFIG_DIR=str(root / alias), HGS_STATE_DIR=str(root / alias / 'state'))
 args = shlex.split(sys.argv[-1])[1:]
 with (root / 'calls').open('a') as file: file.write(alias + ' ' + ' '.join(args) + '\\n')
+if (root / ('noreplica-' + alias)).exists():
+    # A member from before task replication: its hello has no replica field and
+    # it knows no replica commands.
+    if args[1:2] in (['replica-inventory'], ['replica-exchange']):
+        print('usage: hgs swarm get | initialize', file=sys.stderr); sys.exit(1)
+    result = subprocess.run([os.environ['SWARM_TEST_HGS'], *args], input=sys.stdin.read(), env=env, text=True, capture_output=True)
+    sys.stderr.write(result.stderr)
+    if result.returncode: sys.exit(result.returncode)
+    output = json.loads(result.stdout); output.pop('replica', None)
+    print(json.dumps(output)); sys.exit(0)
 if (root / ('preference-' + alias)).exists():
     # A member from before the shared project order: it lists the kinds it
     # accepts, without "order", and rejects that kind.
@@ -51,6 +62,14 @@ print(json.dumps(output))
 '''
 
 
+def noise(seed, length):
+    """Text that does not compress, as task documents are compressed."""
+    out = ''
+    while len(out) < length:
+        out += hashlib.sha256(f'{seed}-{len(out)}'.encode()).hexdigest()
+    return out[:length]
+
+
 def project(id, name, machine, path):
     return dict(id=id, name=name, color='#123456', vivid=False,
                 folders=[dict(id='folder-' + id, machine=machine, path=path, name='')], sessions=[])
@@ -83,6 +102,19 @@ class Swarm(unittest.TestCase):
         self.assertNotEqual(result.returncode, 0)
         return result.stderr
 
+    def task(self, node, *args, text=None):
+        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / node), HGS_STATE_DIR=str(self.root / node / 'state'))
+        result = subprocess.run([str(HGS), 'task', *args], input=text, env=env, text=True, capture_output=True, timeout=40)
+        self.assertEqual(result.returncode, 0, result.stderr)
+        return json.loads(result.stdout)
+
+    def tasks(self, node):
+        return sorted(t['text'] for t in self.task(node, 'list', '--project', 'a')['tasks'])
+
+    def exchanges(self):
+        calls = self.root / 'calls'
+        return sum('replica-exchange' in line for line in calls.read_text().splitlines()) if calls.exists() else 0
+
     def join(self, node, peer, mapping=None, prefer_peer=False):
         preview = self.call(node, 'preview', peer)
         return self.call(node, 'join', peer, data=dict(node_id=preview['node_id'], swarm_id=preview['swarm_id'], project_map=mapping or {}, prefer_peer_memberships=prefer_peer))
@@ -397,6 +429,75 @@ class Swarm(unittest.TestCase):
         self.assertEqual(self.call('a', 'get')['worker'], {'running': False, 'current': False})
         self.assertFalse((self.root / 'service-calls').exists())
 
+    def test_tasks_replicate_after_the_catalog(self):
+        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
+        self.converge()
+        first = self.task('a', 'add', '--project', 'a', text='Add rate limiting')['task']
+        self.task('a', 'add', '--project', 'a', text='Document the setup')
+        # b fetches them from a, and c from b.
+        self.call('b', 'sync'); self.call('c', 'sync')
+        for node in 'bc':
+            self.assertEqual(self.tasks(node), ['Add rate limiting', 'Document the setup'], node)
+        # Changes made on two members at once merge everywhere.
+        self.task('c', 'comment', first['id'], '--project', 'a', text='Use the config value.')
+        self.task('a', 'stage', first['id'], 'backlog', '--project', 'a')
+        self.converge()
+        for node in 'abc':
+            view = self.task(node, 'get', first['id'], '--project', 'a')['task']
+            self.assertEqual(view['stage'], 'backlog', node)
+            self.assertIn('Use the config value.', [entry.get('text') for entry in view['log']], node)
+        hellos = [self.call(node, 'hello')['replica'] for node in 'abc']
+        self.assertEqual(len({json.dumps(h['projects'], sort_keys=True) for h in hellos}), 1)
+        self.assertEqual({(h['protocol'], h['schema']) for h in hellos}, {(1, 1)})
+        # Equal digests: a sync exchanges no project.
+        before = self.exchanges(); self.call('a', 'sync'); self.call('b', 'sync')
+        self.assertEqual(self.exchanges(), before)
+
+    def test_a_large_project_travels_in_pages(self):
+        self.join('a', 'b'); self.join('b', 'a')
+        self.call('a', 'sync'); self.call('b', 'sync')
+        for n in range(20):
+            self.task('a', 'add', '--project', 'a', text=noise(n, 60000))
+        before = self.exchanges()
+        self.assertTrue(self.call('b', 'sync')['a']['ok'])
+        self.assertEqual(len(self.tasks('b')), 20)
+        self.assertGreaterEqual(self.exchanges() - before, 3)
+
+    def test_members_without_task_replication_are_skipped_and_named(self):
+        self.join('a', 'b'); self.join('b', 'a'); self.join('c', 'b'); self.join('b', 'c')
+        self.converge()
+        self.task('b', 'add', '--project', 'a', text='Add rate limiting')
+        (self.root / 'noreplica-c').touch()  # c now answers like a release without task replication
+        self.assertTrue(self.call('b', 'sync')['c']['ok'])
+        self.assertEqual(self.tasks('c'), [])
+        peers = self.call('b', 'get')['peers']
+        self.assertTrue(peers['c']['tasks_outdated'])
+        self.assertNotIn('tasks_outdated', peers['a'])
+        # After the update it receives the tasks and the notice clears.
+        (self.root / 'noreplica-c').unlink()
+        self.assertTrue(self.call('b', 'sync')['c']['ok'])
+        self.assertEqual(self.tasks('c'), ['Add rate limiting'])
+        self.assertNotIn('tasks_outdated', self.call('b', 'get')['peers']['c'])
+
+    def test_the_worker_sends_a_new_task_within_seconds(self):
+        self.join('a', 'b'); self.join('b', 'a')
+        self.call('a', 'sync'); self.call('b', 'sync')
+        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / 'a'), HGS_STATE_DIR=str(self.root / 'a/state'))
+        worker = subprocess.Popen([str(HGS), 'swarm', 'worker'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
+        try:
+            # Its first round is over; the next scheduled one is 30 s away.
+            synced = self.call('a', 'get')['peers']['b']['last_sync']
+            deadline = time.monotonic() + 10
+            while self.call('a', 'get')['peers']['b']['last_sync'] == synced and time.monotonic() < deadline:
+                time.sleep(.1)
+            self.task('a', 'add', '--project', 'a', text='Add rate limiting')
+            deadline = time.monotonic() + 10
+            while self.tasks('b') != ['Add rate limiting'] and time.monotonic() < deadline:
+                time.sleep(.2)
+            self.assertEqual(self.tasks('b'), ['Add rate limiting'])
+        finally:
+            worker.terminate(); worker.communicate(timeout=5)
+
     def test_initial_join_can_keep_existing_swarm_session_placement(self):
         for node in ['a', 'b']:
             before = self.call(node, 'get'); desired = copy.deepcopy(before['organization'])
````

- [ ] **Step 2: Run them to see them fail**

Run: `cargo +1.99.1 build --target-dir .ci-build/cargo -q && HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_swarm -k replicate -k pages -k without_task -k within_seconds`
Expected: `test_tasks_replicate_after_the_catalog` (`Lists differ: [] != ['Add rate limiting', 'Document the setup']`), `test_a_large_project_travels_in_pages` (`0 != 20`), `test_the_worker_sends_a_new_task_within_seconds` (`Lists differ: [] != ['Add rate limiting']`) fail and `test_members_without_task_replication_are_skipped_and_named` errors with `KeyError: 'tasks_outdated'`; `FAILED (failures=3, errors=1)`.

- [ ] **Step 3: Implement**

Apply this change to `src/swarm/mod.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/mod.rs b/src/swarm/mod.rs
index 89ca96f1..0056f38a 100644
--- a/src/swarm/mod.rs
+++ b/src/swarm/mod.rs
@@ -4,6 +4,7 @@
 mod catalog;
 mod launch_project;
 mod model;
+mod replica;
 mod service;
 mod store;
 mod transport;
@@ -164,6 +165,8 @@ fn execute(config: &Config, args: &[String]) -> Result<Value> {
         "bind" => transport::bind(config,alias()?),
         "join" => transport::join(config,alias()?,serde_json::from_value(input()?)?),
         "exchange" => transport::exchange(config,serde_json::from_value(input()?)?),
+        "replica-inventory" => replica::inventory(config,serde_json::from_value(input()?)?),
+        "replica-exchange" => replica::exchange(config,serde_json::from_value(input()?)?),
         "sync" => transport::sync(config),
         "get" | "initialize" | "apply" | "export" | "resolve" | "disconnect" => {
             // Read stdin before taking the lock: slow clients cannot monopolize it.
@@ -196,6 +199,6 @@ fn execute(config: &Config, args: &[String]) -> Result<Value> {
             snapshot["written"] = json!(written);
             Ok(snapshot)
         }
-        _ => bail!("usage: hgs swarm get | initialize | apply | preview PEER | join PEER | sync | worker | resolve | disconnect PEER | preference get|set"),
+        _ => bail!("usage: hgs swarm get | initialize | apply | preview PEER | join PEER | sync | worker | resolve | disconnect PEER | preference get|set | replica-inventory | replica-exchange"),
     }
 }
````

Create `src/swarm/replica.rs`:

````rust
//! Task replicas between members. After the catalog, every project whose
//! digest differs exchanges inventories and the updates each side lacks, in
//! pages; members whose hgs has no replica support are skipped and named in
//! Settings (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md).
use super::{store::LockedStore, transport};
use crate::config::Config;
use anyhow::{bail, ensure, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;
use zerus_replica::{Document, Inventory, ProjectStore, Update, SCHEMA};

/// The replica exchange this hgs speaks; members with another one are skipped.
pub const PROTOCOL: u64 = 1;
/// The most update bytes one page carries; a larger document travels alone.
const PAGE: usize = 1 << 20;
/// Pages of one project in one sync; the next sync continues.
const MAX_PAGES: usize = 64;

fn root(config: &Config) -> PathBuf {
    config.state.join("replica")
}

/// Each stored project's inventory digest.
fn digests(config: &Config) -> Result<BTreeMap<String, String>> {
    let root = root(config);
    let mut out = BTreeMap::new();
    for id in ProjectStore::stored(&root)? {
        let store = ProjectStore::open(&root, &id)?;
        out.insert(id, store.inventory()?.0.digest());
    }
    Ok(out)
}

/// The replica capability `hello` advertises: the protocol, the highest task
/// schema this hgs reads and every stored project's digest.
pub fn hello(config: &Config) -> Result<Value> {
    Ok(json!({"protocol": PROTOCOL, "schema": SCHEMA, "projects": digests(config)?}))
}

/// Changes whenever any stored project changes; the worker exchanges at once.
pub fn fingerprint(config: &Config) -> Result<String> {
    Ok(format!("{:x}", Sha256::digest(serde_json::to_vec(&digests(config)?)?)))
}

/// One update on the wire: the document and its Loro updates in base64.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Wire {
    document: Document,
    bytes: String,
}

fn encode(updates: Vec<Update>) -> Vec<Wire> {
    updates
        .into_iter()
        .map(|u| Wire {
            document: u.document,
            bytes: STANDARD.encode(u.bytes),
        })
        .collect()
}

fn decode(wire: Vec<Wire>) -> Result<Vec<Update>> {
    wire.into_iter()
        .map(|w| {
            Ok(Update {
                document: w.document,
                bytes: STANDARD.decode(w.bytes)?,
            })
        })
        .collect()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    project: String,
    #[serde(default)]
    inventory: Option<Inventory>,
    #[serde(default)]
    updates: Vec<Wire>,
}

/// `swarm replica-inventory`: what this member holds of the project.
pub fn inventory(config: &Config, request: Request) -> Result<Value> {
    let store = ProjectStore::open(&root(config), &request.project)?;
    Ok(json!({"inventory": store.inventory()?.0}))
}

/// `swarm replica-exchange`: imports the requester's updates and answers with
/// a page of what its inventory lacks.
pub fn exchange(config: &Config, request: Request) -> Result<Value> {
    let theirs = request
        .inventory
        .ok_or_else(|| anyhow::anyhow!("a replica exchange needs the requester's inventory"))?;
    let store = ProjectStore::open(&root(config), &request.project)?;
    let imported = store.import_updates(decode(request.updates)?)?;
    let (updates, more) = store.updates_for(&theirs, PAGE)?;
    Ok(json!({"imported": imported.changed, "refused": imported.refused, "updates": encode(updates), "more": more}))
}

/// Exchanges every project whose digest differs with the member behind
/// `alias`, whose `hello` answered `identity`.
pub fn sync_peer(config: &Config, alias: &str, identity: &Value) -> Result<()> {
    let remote = &identity["replica"];
    let supported = remote["protocol"] == PROTOCOL;
    let mut store = LockedStore::load(config)?;
    if supported {
        store.replica_outdated.remove(alias);
    } else {
        let node = identity["node_id"].as_str().unwrap_or_default().to_owned();
        store.replica_outdated.insert(alias.into(), node);
    }
    store.save()?;
    drop(store);
    if !supported {
        return Ok(());
    }
    let local = digests(config)?;
    let theirs: BTreeMap<String, String> =
        serde_json::from_value(remote["projects"].clone()).unwrap_or_default();
    let projects: BTreeSet<&String> = local.keys().chain(theirs.keys()).collect();
    for project in projects {
        if local.get(project) != theirs.get(project) {
            exchange_project(config, alias, project)?;
        }
    }
    Ok(())
}

fn exchange_project(config: &Config, alias: &str, project: &str) -> Result<()> {
    let root = root(config);
    for _ in 0..MAX_PAGES {
        let reply = transport::remote(config, alias, "replica-inventory", Some(&json!({"project": project})))?;
        let theirs: Inventory = serde_json::from_value(reply["inventory"].clone())?;
        // The project's lock is held only between network calls.
        let (mine, page, more_mine) = {
            let store = ProjectStore::open(&root, project)?;
            let (mine, _) = store.inventory()?;
            let (page, more) = store.updates_for(&theirs, PAGE)?;
            (mine, page, more)
        };
        let response = transport::remote(
            config,
            alias,
            "replica-exchange",
            Some(&json!({"project": project, "inventory": mine, "updates": encode(page)})),
        )?;
        let updates = decode(serde_json::from_value(response["updates"].clone())?)?;
        ensure!(response["more"].is_boolean(), "{alias} sent an invalid replica exchange");
        if !updates.is_empty() {
            let imported = ProjectStore::open(&root, project)?.import_updates(updates)?;
            // A refused document is named in the worker's log; the others are kept.
            for refused in imported.refused {
                eprintln!("hgs swarm: {alias}: task {} of {project} was refused: {}", refused.uuid, refused.error);
            }
        }
        if !more_mine && response["more"] != true {
            return Ok(());
        }
    }
    bail!("the tasks of {project} did not finish in {MAX_PAGES} pages; the next sync continues")
}
````

Apply this change to `src/swarm/store.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/store.rs b/src/swarm/store.rs
index 7cdb5223..7a7a870a 100644
--- a/src/swarm/store.rs
+++ b/src/swarm/store.rs
@@ -13,6 +13,8 @@ use std::{
 /// Members whose hgs predates shared preferences. Kept beside catalog.json,
 /// which an older hgs on this machine must still be able to read.
 const OUTDATED: &str = "outdated-peers.json";
+/// Members whose hgs does not replicate tasks, kept the same way.
+const REPLICA_OUTDATED: &str = "replica-outdated-peers.json";
 
 #[derive(Clone, Debug, Serialize, Deserialize)]
 #[serde(deny_unknown_fields)]
@@ -42,15 +44,42 @@ pub struct Store {
     pub peers: BTreeMap<String, Peer>,
 }
 
+/// Keeps the members still enrolled with the same node and writes the list
+/// when it changed; an empty list removes its file.
+fn save_members(
+    path: &Path,
+    members: &mut BTreeMap<String, String>,
+    peers: &BTreeMap<String, Peer>,
+    original: &mut Vec<u8>,
+) -> Result<()> {
+    members.retain(|alias, node| peers.get(alias).is_some_and(|p| &p.node == node));
+    let bytes = if members.is_empty() { vec![] } else { serde_json::to_vec(members)? };
+    if bytes != *original {
+        if bytes.is_empty() {
+            match fs::remove_file(path) {
+                Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e.into()),
+                _ => {}
+            }
+        } else {
+            replace(path, &bytes)?;
+        }
+        *original = bytes;
+    }
+    Ok(())
+}
+
 pub struct LockedStore {
     pub data: Store,
     /// Alias -> node of members that keep syncing projects and folders but do
     /// not receive the delivery choice. A missing or damaged file means none.
     pub outdated: BTreeMap<String, String>,
+    /// Alias -> node of members that keep syncing the catalog but not tasks.
+    pub replica_outdated: BTreeMap<String, String>,
     pub path: PathBuf,
     _lock: platform::FileLock,
     original: Vec<u8>,
     outdated_original: Vec<u8>,
+    replica_outdated_original: Vec<u8>,
 }
 
 impl LockedStore {
@@ -87,13 +116,16 @@ impl LockedStore {
         };
         data.catalog.validate()?;
         let outdated_original = fs::read(dir.join(OUTDATED)).unwrap_or_default();
+        let replica_outdated_original = fs::read(dir.join(REPLICA_OUTDATED)).unwrap_or_default();
         let mut loaded = Self {
             data,
             outdated: serde_json::from_slice(&outdated_original).unwrap_or_default(),
+            replica_outdated: serde_json::from_slice(&replica_outdated_original).unwrap_or_default(),
             path,
             _lock: lock,
             original,
             outdated_original,
+            replica_outdated_original,
         };
         let node = loaded.data.node_id.clone();
         for alias in &config.selves {
@@ -119,26 +151,13 @@ impl LockedStore {
             self.original = bytes;
         }
         let peers = &self.data.peers;
-        self.outdated
-            .retain(|alias, node| peers.get(alias).is_some_and(|p| &p.node == node));
-        let outdated = if self.outdated.is_empty() {
-            vec![]
-        } else {
-            serde_json::to_vec(&self.outdated)?
-        };
-        if outdated != self.outdated_original {
-            let path = self.path.with_file_name(OUTDATED);
-            if outdated.is_empty() {
-                match fs::remove_file(path) {
-                    Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e.into()),
-                    _ => {}
-                }
-            } else {
-                replace(&path, &outdated)?;
-            }
-            self.outdated_original = outdated;
-        }
-        Ok(())
+        save_members(&self.path.with_file_name(OUTDATED), &mut self.outdated, peers, &mut self.outdated_original)?;
+        save_members(
+            &self.path.with_file_name(REPLICA_OUTDATED),
+            &mut self.replica_outdated,
+            peers,
+            &mut self.replica_outdated_original,
+        )
     }
     /// The catalog snapshot, naming members that need an hgs update.
     pub fn snapshot(&self, config: &Config) -> Value {
@@ -147,6 +166,9 @@ impl LockedStore {
             if self.outdated.get(alias) == Some(&peer.node) {
                 snapshot["peers"][alias]["catalog_outdated"] = json!(true);
             }
+            if self.replica_outdated.get(alias) == Some(&peer.node) {
+                snapshot["peers"][alias]["tasks_outdated"] = json!(true);
+            }
         }
         snapshot
     }
````

Apply this change to `src/swarm/transport.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/transport.rs b/src/swarm/transport.rs
index d11549ce..f7b004c5 100644
--- a/src/swarm/transport.rs
+++ b/src/swarm/transport.rs
@@ -1,6 +1,6 @@
 use super::{
     catalog::{self, Catalog, Key, Operation},
-    service,
+    replica, service,
     store::{LockedStore, Peer},
 };
 use crate::{config::Config, platform};
@@ -128,6 +128,9 @@ pub fn hello(config: &Config, inventory: bool) -> Result<Value> {
     if inventory {
         result["known"] = json!(store.data.catalog.operations.keys().collect::<Vec<_>>());
     }
+    // Project stores are opened after the catalog lock is released.
+    drop(store);
+    result["replica"] = replica::hello(config)?;
     Ok(result)
 }
 
@@ -241,6 +244,7 @@ pub fn sync_peer(config: &Config, alias: &str) -> Result<()> {
     } else {
         drop(local);
     }
+    replica::sync_peer(config, alias, &identity)?;
     let mut local = LockedStore::load(config)?;
     if let Some(peer) = local.data.peers.get_mut(alias) {
         peer.last_sync = now();
@@ -487,10 +491,12 @@ pub fn worker() -> Result<()> {
             .and_then(|config| {
                 let mut store = LockedStore::load(&config)?;
                 store.save()?;
-                let digest = store.data.catalog.digest();
-                let changed = digest != last_digest;
+                let catalog = store.data.catalog.digest();
                 let peers: Vec<_> = store.data.peers.keys().cloned().collect();
                 drop(store);
+                // A local change to the catalog or to any task syncs at once.
+                let digest = catalog + &replica::fingerprint(&config)?;
+                let changed = digest != last_digest;
                 for alias in peers {
                     let (due, failures) = schedule.get(&alias).copied().unwrap_or_default();
                     if now() < due && !(changed && failures == 0) {
@@ -512,7 +518,7 @@ pub fn worker() -> Result<()> {
                         }
                     }
                 }
-                last_digest = LockedStore::load(&config)?.data.catalog.digest();
+                last_digest = LockedStore::load(&config)?.data.catalog.digest() + &replica::fingerprint(&config)?;
                 Ok(())
             }) {
             Ok(()) => {}
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cargo +1.99.1 clippy --target-dir .ci-build/cargo --all-targets -q -- -D warnings
cargo +1.99.1 build --target-dir .ci-build/cargo -q
HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_swarm tests.test_tasks
```
Expected: no lint output; `Ran 44 tests … OK` (25 swarm, 19 task).

- [ ] **Step 5: Commit**

```bash
git add src/swarm/mod.rs \
  src/swarm/replica.rs \
  src/swarm/store.rs \
  src/swarm/transport.rs \
  tests/test_swarm.py
git commit -m "Replicate tasks after the catalog in the swarm worker"
```

---

### Task 3: Send now and Cancel on the session's computer

**Files:**
- Create: `tests/test_task_routing.py`
- Modify: `tests/test_tasks.py`, `docs/reference.md`, `src/swarm/mod.rs`, `src/swarm/replica.rs`, `src/swarm/transport.rs`, `src/tasks.rs`

**Interfaces:**
- Consumes: `replica::exchange_project` and `PROTOCOL` (Task 2), `transport::remote`, `verified_bindings` through `Machines`.
- Produces: `transport::run(config, alias, words, input) -> Result<Value>` (`hgs WORDS…` on the member; `remote` now calls it with `["swarm", action]`); `swarm::remote_hgs(config, alias, args)` and `swarm::replicate(config, alias, project)` (refuses with "ALIAS cannot be reached: …", "update Zerus on ALIAS to send tasks there", or a changed identity); in `src/tasks.rs`: `Machines::alias(node)`, `actor(here, args) -> Result<Actor>` (reads `--agent NODE/SESSION`), `send_session(task, args, who, machines) -> Result<(SessionKey, bool)>`, `forwarded(…)`, and `send(store, task, project_name, session, pin, who)`. Usage: `send REF [--session SESSION [--host ALIAS]]`. A new suite `tests/test_task_routing.py` (two nodes, fake SSH through `HGS_SSH`, a scripted agent on node b).

The decision of where a send goes happens before any paste, so the project lock is dropped before the network: the forwarded path opens no store until the copy came back. The forwarded answer keeps that computer's `receipt` or `cancel_message`; the printed task is this computer's copy after the exchange, so hosts read from here.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_task_routing.py`:

````python
#!/usr/bin/env python3
"""Send now and Cancel for a session on another computer of the swarm: that
computer gets the project's tasks, pastes and records, and its copy comes back.
Isolated nodes behind a fake SSH client; node b runs a scripted agent on its
own private tmux socket."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import uuid

from tests import test_tasks

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
TMUX = shutil.which("tmux")
SSH = '''#!/usr/bin/env python3
import json, os, pathlib, shlex, subprocess, sys
root = pathlib.Path(os.environ['ROUTING_ROOT'])
alias = sys.argv[-2]
if (root / ('offline-' + alias)).exists():
    print('ssh: connect to host ' + alias + ': Connection timed out', file=sys.stderr); sys.exit(255)
node = root / alias
env = dict(os.environ, HOME=str(node / 'profile'), HGS_CONFIG_DIR=str(node / 'config'),
           HGS_STATE_DIR=str(node / 'state'), TASK_FIXTURE=str(node))
for name in ('HGS_SESSION', 'HGS_RUN_ID'):
    env.pop(name, None)  # an SSH login is no agent
args = shlex.split(sys.argv[-1])[1:]
with (root / 'calls').open('a') as file: file.write(alias + ' ' + ' '.join(args) + '\\n')
if (root / ('noreplica-' + alias)).exists():
    # hgs from before task replication: no replica field in hello.
    result = subprocess.run([os.environ['ROUTING_HGS'], *args], input=sys.stdin.read(), env=env, text=True, capture_output=True)
    sys.stderr.write(result.stderr)
    if result.returncode: sys.exit(result.returncode)
    output = json.loads(result.stdout); output.pop('replica', None); print(json.dumps(output)); sys.exit(0)
os.execve(os.environ['ROUTING_HGS'], [os.environ['ROUTING_HGS'], *args], env)
'''


@unittest.skipUnless(TMUX, "tmux is required for delivery")
class TaskRouting(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-routing-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        ssh = self.root / "ssh"; ssh.write_text(SSH); ssh.chmod(0o755)
        self.base = dict(os.environ, ROUTING_ROOT=str(self.root), ROUTING_HGS=str(HGS), HGS_SSH=str(ssh), HGS_TAB="0")
        for name in ("TMUX", "TMUX_PANE", "HGS_RUN_ID", "HGS_SESSION", "HGS_EXECUTABLE", "HGS_SELF", "HGS_PEERS"):
            self.base.pop(name, None)
        for node, peer in (("a", "b"), ("b", "a")):
            home = self.root / node / "profile/.local/bin"; home.mkdir(parents=True)
            (self.root / node / "state").mkdir()
            socket = self.root / node / "socket"
            # hgs puts $HOME/.local/bin first on PATH: each node's tmux stays private.
            shim = home / "tmux"; shim.write_text(f'#!/bin/sh\nexec {TMUX} -S "{socket}" -f /dev/null "$@"\n'); shim.chmod(0o755)
            self.addCleanup(lambda socket=socket: subprocess.run([TMUX, "-S", str(socket), "kill-server"], capture_output=True))
            config = self.root / node / "config"; config.mkdir()
            (config / "config").write_text(f'HGS_SELF="{node}"\nHGS_PEERS="{peer}"\n')
            (config / "machines.local.json").write_text(json.dumps({"machines": {peer: dict(hostname="host.invalid", enabled=True)}}))
        projects = [{"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []}]
        self.swarm("a", "initialize", data={"projects": projects})
        self.swarm("b", "initialize", data={"projects": []})
        for node, peer in (("b", "a"), ("a", "b")):
            preview = self.swarm(node, "preview", peer)
            self.swarm(node, "join", peer, data=dict(node_id=preview["node_id"], swarm_id=preview["swarm_id"], project_map={}, prefer_peer_memberships=False))
        self.start_agent()

    def env(self, node, session=None):
        env = dict(self.base, HOME=str(self.root / node / "profile"), HGS_CONFIG_DIR=str(self.root / node / "config"),
                   HGS_STATE_DIR=str(self.root / node / "state"), TASK_FIXTURE=str(self.root / node))
        if session:
            env.update(HGS_SESSION=session, HGS_RUN_ID="run-test")
        return env

    def swarm(self, node, *args, data=None):
        result = subprocess.run([str(HGS), "swarm", *args], input=json.dumps(data) if data is not None else None,
                                env=self.env(node), text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def task(self, node, *args, text=None, ok=True, session=None):
        result = subprocess.run([str(HGS), "task", *args], input=text, env=self.env(node, session), text=True,
                                capture_output=True, timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr

    def tmux(self, *args):
        result = subprocess.run([str(self.root / "b/profile/.local/bin/tmux"), *args], env=self.env("b"),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def start_agent(self):
        """codex/remote on b: a scripted agent that records what it receives."""
        node = self.root / "b"
        (node / "agent.py").write_text(test_tasks.AGENT)
        self.tmux("new-session", "-d", "-s", "codex/remote", "-x", "100", "-y", "30", "python3", str(node / "agent.py"))
        for _ in range(200):
            if (node / "agent-pid").exists() and (node / "agent-pid").read_text().strip():
                break
            time.sleep(.02)
        pid = int((node / "agent-pid").read_text())
        start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="], env=dict(os.environ, LC_ALL="C", TZ="UTC"), text=True).strip()
        pane = self.tmux("display-message", "-p", "-t", "=codex/remote:", "#{pane_id}").strip()
        run_id = str(uuid.uuid4())
        self.tmux("set-option", "-t", "=codex/remote:", "@hgs_run", run_id)
        record = node / "state" / (hashlib.sha256(b"codex/remote").hexdigest() + ".json")
        record.write_text(json.dumps(dict(
            version=1, name="codex/remote", agent="codex", run_id=run_id, conversation_id=str(uuid.uuid4()),
            pane=pane, pid=pid, process_start=start, activity="idle", phase="idle", active_tools={},
            subagents={}, last_event_at=time.time(), created=time.time())))
        self.record = record

    def confirm(self):
        # The agent's next hook event confirms the pasted input.
        record = json.loads(self.record.read_text())
        record["last_event_at"] = record.get("input_pending_at", time.time()) + 1
        self.record.write_text(json.dumps(record))

    def received(self):
        path = self.root / "b/received"
        return path.read_bytes().decode(errors="replace") if path.exists() else ""

    def wait_received(self, text):
        for _ in range(250):
            if text in self.received():
                return
            time.sleep(.02)
        self.fail("the agent on b never received " + repr(text) + "; got " + repr(self.received()))

    def add(self, text, *args):
        return self.task("a", "add", "--project", "project-1", *args, text=text)["task"]

    def get(self, node, task):
        return self.task(node, "get", task["id"], "--project", "project-1")["task"]

    def test_send_now_runs_on_the_sessions_computer(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        sent = self.task("a", "send", task["id"], "--project", "project-1")
        self.assertEqual(sent["receipt"]["status"], "submitted")
        self.wait_received("[Zerus task orchard/" + task["id"] + "]")
        self.wait_received("Add rate limiting")
        # Both copies show it In progress in b's session, sent by the person.
        here, there = sent["task"], self.get("b", task)
        self.assertEqual((here["stage"], here["session"]["session"], here["session"]["host"]), ("active", "codex/remote", "b"))
        self.assertEqual((there["stage"], there["session"]["host"]), ("active", ""))
        entry = next(e for e in there["log"] if e["kind"] == "sent")
        self.assertEqual(entry["actor"]["kind"], "person")
        calls = (self.root / "calls").read_text()
        self.assertIn("b task send " + task["uuid"] + " --project project-1", calls)

    def test_an_any_session_task_goes_to_the_session_named_there(self):
        task = self.add("Document the setup")
        self.task("a", "send", task["id"], "--project", "project-1", "--session", "codex/remote", "--host", "b")
        self.wait_received("Document the setup")
        view = self.get("a", task)
        self.assertEqual((view["stage"], view["session"]["host"], view["target"]["kind"]), ("active", "b", "any"))

    def test_cancel_runs_on_the_sessions_computer(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        self.task("a", "send", task["id"], "--project", "project-1")
        self.wait_received("Add rate limiting")
        self.confirm()
        cancelled = self.task("a", "cancel", task["id"], "--project", "project-1", "--reason", "Wrong approach")
        self.assertTrue(cancelled["cancel_message"]["sent"], cancelled)
        self.wait_received("[Zerus task orchard/" + task["id"] + " cancelled]")
        for node in "ab":
            view = self.get(node, task)
            self.assertEqual(view["stage"], "review", node)
            self.assertIn("Wrong approach", [e.get("text") for e in view["log"]], node)

    def test_an_unreachable_computer_refuses_and_records_nothing(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        (self.root / "offline-b").touch()
        refused = self.task("a", "send", task["id"], "--project", "project-1", ok=False)
        self.assertIn("b cannot be reached", refused)
        self.assertEqual(self.get("a", task)["stage"], "todo")
        (self.root / "offline-b").unlink()
        self.task("a", "send", task["id"], "--project", "project-1")
        self.wait_received("Add rate limiting")
        (self.root / "offline-b").touch()
        refused = self.task("a", "cancel", task["id"], "--project", "project-1", ok=False)
        self.assertIn("b cannot be reached", refused)
        self.assertEqual(self.get("a", task)["stage"], "active")

    def test_a_task_its_computer_shows_in_progress_is_refused(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        self.swarm("b", "sync")
        # b's person sends it there; a's copy has not heard of that yet.
        self.task("b", "send", task["id"], "--project", "project-1")
        self.wait_received("Add rate limiting")
        self.assertEqual(self.get("a", task)["stage"], "todo")
        refused = self.task("a", "send", task["id"], "--project", "project-1", ok=False)
        self.assertIn("already In progress in codex/remote", refused)
        self.assertEqual(self.received().count("Add rate limiting"), 1)

    def test_an_agent_sends_as_itself(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        self.task("a", "send", task["id"], "--project", "project-1", session="codex/local")
        self.wait_received("Add rate limiting")
        entry = next(e for e in self.get("b", task)["log"] if e["kind"] == "sent")
        self.assertEqual((entry["actor"]["kind"], entry["actor"]["session"], entry["actor"]["host"]), ("agent", "codex/local", "a"))

    def test_a_computer_without_task_replication_is_named(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        (self.root / "noreplica-b").touch()
        refused = self.task("a", "send", task["id"], "--project", "project-1", ok=False)
        self.assertIn("update Zerus on b", refused)
        self.assertEqual(self.get("a", task)["stage"], "todo")


if __name__ == "__main__":
    unittest.main()
````

Apply this change to `tests/test_tasks.py` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tests/test_tasks.py b/tests/test_tasks.py
index c23f212a..d90632a4 100644
--- a/tests/test_tasks.py
+++ b/tests/test_tasks.py
@@ -216,7 +216,8 @@ class TaskCommands(Fixture):
         self.assertEqual(moved["target"]["host"], "orchard-mac")
         listed = {t["id"]: t for t in self.hgs("list", "--project", "orchard")["tasks"]}
         self.assertEqual(listed[there["id"]]["target"]["host"], "orchard-mac")
-        self.assertIn("on another computer", self.hgs("send", there["ref"], ok=False))
+        # A send for it runs on that computer, which this fixture cannot reach.
+        self.assertIn("orchard-mac cannot be reached", self.hgs("send", there["ref"], ok=False))
 
     def test_a_project_name_with_a_slash(self):
         task = self.hgs("add", "--project", "team/web", text="Ship it")["task"]
````

- [ ] **Step 2: Run them to see them fail**

Run: `cargo +1.99.1 build --target-dir .ci-build/cargo -q && HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_routing tests.test_tasks`
Expected: all seven routing tests fail with `… is for codex/remote on another computer; sending there comes with replication` (the Any session test with `codex/remote: no saved binding`), and `test_tasks_name_the_computer_of_their_session` fails: `'orchard-mac cannot be reached' not found`.

- [ ] **Step 3: Implement**

Apply this change to `docs/reference.md` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/docs/reference.md b/docs/reference.md
index d0973694..5adc1059 100644
--- a/docs/reference.md
+++ b/docs/reference.md
@@ -1854,8 +1854,9 @@ older `hgs` must be updated before cleanup is available there.
 
 ## Project tasks
 
-`hgs task` keeps a project's task list on this computer: prompts that wait in
-Backlog or To do until someone sends them to a session. Every command prints
+`hgs task` keeps a project's task list: prompts that wait in Backlog or To do
+until someone sends them to a session. Every computer of the swarm holds every
+task and works with them offline. Every command prints
 JSON. Tasks belong to a catalog project, named by its ID or name; a task is
 `T-` plus six characters, and `PROJECT/T-xxxxxx` names it anywhere.
 
@@ -1863,7 +1864,7 @@ JSON. Tasks belong to a catalog project, named by its ID or name; a task is
 hgs task add --project orchard [--backlog] [--target codex/api|any [--host ALIAS]] [--position N] < prompt.txt
 hgs task list --project orchard [--stage todo]
 hgs task get orchard/T-a1b2c3
-hgs task send orchard/T-a1b2c3 [--session codex/api]
+hgs task send orchard/T-a1b2c3 [--session codex/api [--host ALIAS]]
 hgs task report orchard/T-a1b2c3 < summary.txt
 hgs task cancel orchard/T-a1b2c3 [--reason "Wrong approach"]
 hgs task stage orchard/T-a1b2c3 backlog|todo|done
@@ -1874,8 +1875,8 @@ hgs task target orchard/T-a1b2c3 codex/api|any [--host ALIAS]
 hgs task settings --project orchard [--set < settings.json]
 ```
 
-Send pastes the task prompt into a session on this computer through the same
-input path as `hgs send`; the task becomes In progress, and Zerus does not
+Send pastes the task prompt into its session through the same input path as
+`hgs send`; the task becomes In progress, and Zerus does not
 check that the agent received it. The prompt asks the agent to report with
 `hgs task report`, which moves the task to In review. Cancel moves a task In
 progress to In review and pastes the cancel message without stopping the turn;
@@ -1886,8 +1887,12 @@ for that session, and the project settings can keep agents to their own
 session or limit how many tasks they start per hour. `settings --set` reads a
 JSON object and changes only the fields it names.
 
-`--host ALIAS` pins a task to a session on another computer of the swarm that
-this computer verified itself; sending there comes with replication. Listings
+`--host ALIAS` names a session on another computer of the swarm that this
+computer verified itself. A send or cancel for such a session runs on that
+computer: it gets the project's tasks first, refuses a task its own copy shows
+In progress, pastes and records there, and its copy comes back. When it cannot
+be reached, or its Zerus is too old to share tasks, the send or cancel is
+refused with the reason and nothing changes. Listings
 name the computer of every session reference (the target, the session, the
 author and each history entry's session and actor) as `host`: empty for this
 one, the alias of a verified member, or null. Windows keeps no verified
@@ -1895,7 +1900,12 @@ bindings yet, so there `--host` is refused and other computers read null.
 
 Tasks are Loro documents below `HGS_STATE_DIR/replica`, one directory per
 project; a task file that fails to load is listed under `damaged` and left in
-place. Replication between computers and the MCP tools come in later releases.
+place. The swarm worker replicates them with the catalog: after each catalog
+sync it compares every project's digest with the member's and exchanges only
+the changes each side lacks, in pages of at most 1 MiB, and a task changed
+here goes out within seconds. Changes made on several computers at once merge.
+A member whose Zerus does not share tasks keeps syncing the catalog and is
+named in the Swarm settings. The MCP tools come in a later release.
 
 In the desktop, the **Tasks** tab beside Activity and Terminal shows the open
 session's project. Stage tiles (In review, In progress, To do, Backlog, Done)
````

Apply this change to `src/swarm/mod.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/mod.rs b/src/swarm/mod.rs
index 0056f38a..01745335 100644
--- a/src/swarm/mod.rs
+++ b/src/swarm/mod.rs
@@ -17,6 +17,7 @@ use store::LockedStore;
 
 #[cfg(unix)]
 pub(crate) use catalog::MOBILE_DELIVERY;
+pub(crate) use replica::replicate;
 #[cfg(unix)]
 pub(crate) use store::replace;
 
@@ -54,6 +55,12 @@ pub(crate) fn verified_bindings(
         .unwrap_or_default()
 }
 
+/// `hgs ARGS…` on the swarm member behind `alias`; its JSON answer.
+pub(crate) fn remote_hgs(config: &Config, alias: &str, args: &[String]) -> Result<Value> {
+    let words: Vec<&str> = args.iter().map(String::as_str).collect();
+    transport::run(config, alias, &words, None)
+}
+
 /// Shared preferences for `hgs ls --json --local`. Lock-free: the store is
 /// replaced atomically and listings never wait for a sync. A missing or
 /// unreadable catalog reports the defaults.
````

Apply this change to `src/swarm/replica.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/replica.rs b/src/swarm/replica.rs
index c4c8f211..9f246239 100644
--- a/src/swarm/replica.rs
+++ b/src/swarm/replica.rs
@@ -132,6 +132,23 @@ pub fn sync_peer(config: &Config, alias: &str, identity: &Value) -> Result<()> {
     Ok(())
 }
 
+/// Brings this project's tasks here and on the member behind `alias` to the
+/// same versions now, for a send or cancel that runs there.
+pub fn replicate(config: &Config, alias: &str, project: &str) -> Result<()> {
+    let identity = transport::remote(config, alias, "hello", None)
+        .map_err(|error| anyhow::anyhow!("{alias} cannot be reached: {error:#}"))?;
+    let store = LockedStore::load(config)?;
+    let peer = store
+        .data
+        .peers
+        .get(alias)
+        .ok_or_else(|| anyhow::anyhow!("{alias} is not a member of this swarm"))?;
+    ensure!(identity["node_id"] == peer.node, "the computer behind {alias} changed identity");
+    drop(store);
+    ensure!(identity["replica"]["protocol"] == PROTOCOL, "update Zerus on {alias} to send tasks there");
+    exchange_project(config, alias, project)
+}
+
 fn exchange_project(config: &Config, alias: &str, project: &str) -> Result<()> {
     let root = root(config);
     for _ in 0..MAX_PAGES {
````

Apply this change to `src/swarm/transport.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/swarm/transport.rs b/src/swarm/transport.rs
index f7b004c5..2399ed8e 100644
--- a/src/swarm/transport.rs
+++ b/src/swarm/transport.rs
@@ -35,6 +35,12 @@ pub struct Exchange {
 }
 
 pub fn remote(config: &Config, alias: &str, action: &str, input: Option<&Value>) -> Result<Value> {
+    run(config, alias, &["swarm", action], input)
+}
+
+/// `hgs WORDS…` on the member behind `alias`, with `input` as JSON on its
+/// standard input; its JSON answer.
+pub fn run(config: &Config, alias: &str, words: &[&str], input: Option<&Value>) -> Result<Value> {
     ensure!(
         config.peers.iter().any(|p| p == alias) && !config.is_self(alias),
         "choose a locally configured remote machine"
@@ -63,8 +69,8 @@ pub fn remote(config: &Config, alias: &str, action: &str, input: Option<&Value>)
         .args(crate::machines::ssh_options(config, alias))
         .arg(alias)
         .arg(format!(
-            "~/.local/bin/hgs swarm {}",
-            platform::quote(action)
+            "~/.local/bin/hgs {}",
+            words.iter().map(|word| platform::quote(word)).collect::<Vec<_>>().join(" ")
         ));
     let ssh = platform::spawn_ssh(&mut command, true, true, true).context("start swarm connection")?;
     let mut child = ssh.child;
````

Apply this change to `src/tasks.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/tasks.rs b/src/tasks.rs
index ca0e9937..39928d18 100644
--- a/src/tasks.rs
+++ b/src/tasks.rs
@@ -17,7 +17,7 @@ use zerus_replica::{
     Stage, Target, TaskDoc, Tools, MAX_TEXT,
 };
 
-const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any [--host ALIAS]] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any [--host ALIAS] | send REF [--session SESSION] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
+const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any [--host ALIAS]] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any [--host ALIAS] | send REF [--session SESSION [--host ALIAS]] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
   REF is T-xxxxxx with --project P, or PROJECT/T-xxxxxx; texts and --set settings are read from stdin";
 
 fn fail(message: impl std::fmt::Display) -> Error {
@@ -122,6 +122,11 @@ impl Machines {
         }
     }
 
+    /// The alias of a verified member by its node.
+    fn alias(&self, node: &str) -> Option<String> {
+        self.peers.iter().find(|(_, id)| id.as_str() == node).map(|(alias, _)| alias.clone())
+    }
+
     /// How callers name a computer: "" for this one, a verified member's
     /// alias, or null when this computer does not know it.
     fn host(&self, node: &str) -> Value {
@@ -136,16 +141,27 @@ impl Machines {
 }
 
 /// The agent of the session that runs this command, as hooks know it (both
-/// `HGS_SESSION` and `HGS_RUN_ID`), or the person.
-fn actor(here: &str) -> Actor {
+/// `HGS_SESSION` and `HGS_RUN_ID`), or the person. A send or cancel another
+/// computer forwards names its agent with `--agent NODE/SESSION`.
+fn actor(here: &str, args: &[String]) -> Result<Actor> {
+    if let Some(agent) = option(args, "--agent")? {
+        let (machine, session) = agent
+            .split_once('/')
+            .filter(|(machine, session)| !machine.is_empty() && !session.is_empty())
+            .ok_or_else(|| fail("--agent is NODE/SESSION"))?;
+        return Ok(Actor::Agent(SessionKey {
+            machine: machine.to_owned(),
+            session: session.to_owned(),
+        }));
+    }
     let var = |name| std::env::var(name).ok().filter(|v| !v.is_empty());
-    match (var("HGS_SESSION"), var("HGS_RUN_ID")) {
+    Ok(match (var("HGS_SESSION"), var("HGS_RUN_ID")) {
         (Some(session), Some(_)) => Actor::Agent(SessionKey {
             machine: here.to_owned(),
             session,
         }),
         _ => Actor::Person,
-    }
+    })
 }
 
 fn view(task: &TaskDoc, project_name: &str, with_log: bool, machines: &Machines) -> Result<Value> {
@@ -250,7 +266,7 @@ fn add(config: &Config, args: &[String]) -> Result<i32> {
         option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
     )?;
     let machines = machines(config)?;
-    let author = actor(&machines.here);
+    let author = actor(&machines.here, args)?;
     let stage = if flag(args, "--backlog") {
         Stage::Backlog
     } else {
@@ -313,9 +329,20 @@ fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
     let (project_id, project_name) = (&r.project.0, &r.project.1);
     let machines = machines(config)?;
     let here = machines.here.as_str();
-    let who = actor(here);
+    let who = actor(here, args)?;
     let store = store(config, project_id)?;
     let mut task = store.find(&r.task).map_err(fail)?;
+    // A send or cancel for a session another computer runs happens there.
+    let session = match action {
+        "send" => Some(send_session(&task, args, &who, &machines)?),
+        "cancel" => task.view().map_err(fail)?.session.map(|session| (session, false)),
+        _ => None,
+    };
+    if let Some((session, _)) = session.as_ref().filter(|(session, _)| session.machine != here) {
+        let uuid = task.uuid().to_owned();
+        drop(store);
+        return forwarded(config, &machines, &r.project, &uuid, action, args, session, &who);
+    }
     let text = text.unwrap_or_default();
     let mut extra = json!({});
     match action {
@@ -371,8 +398,11 @@ fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
             };
             task.set_target(&target, &who, now()).map_err(fail)?;
         }
-        "send" => extra = send(here, &store, &mut task, project_name, args, &who)?,
-        "cancel" => extra = cancel(here, &store, &mut task, project_name, args, &who)?,
+        "send" => {
+            let (session, pin) = session.expect("a send has its session");
+            extra = send(&store, &mut task, project_name, session, pin, &who)?;
+        }
+        "cancel" => extra = cancel(&store, &mut task, project_name, args, &who)?,
         _ => return Err(fail(USAGE)),
     }
     store.save(&mut task).map_err(fail)?;
@@ -383,48 +413,92 @@ fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
     print(out)
 }
 
+/// The session a send goes to, and whether it pins the task there: --session
+/// (with --host on another computer; an agent's choice also pins the task),
+/// the pinned session, or for an Any task the agent's own session.
+fn send_session(task: &TaskDoc, args: &[String], who: &Actor, machines: &Machines) -> Result<(SessionKey, bool)> {
+    let view = task.view().map_err(fail)?;
+    let host = option(args, "--host")?;
+    let named = |session: &str| -> Result<SessionKey> {
+        Ok(SessionKey {
+            machine: machines.node(host)?,
+            session: session.to_owned(),
+        })
+    };
+    match (option(args, "--session")?, &view.target, who) {
+        (None, ..) if host.is_some() => Err(fail("--host needs a --session")),
+        (Some(name), Target::Session(pinned), Actor::Person) if *pinned != named(name)? => Err(fail(format!(
+            "{} is pinned to {}; change its session first",
+            task.id(),
+            pinned.session
+        ))),
+        (Some(name), _, Actor::Person) => Ok((named(name)?, false)),
+        (Some(name), _, Actor::Agent(_)) => Ok((named(name)?, true)),
+        (None, Target::Session(pinned), _) => Ok((pinned.clone(), false)),
+        (None, Target::Any, Actor::Agent(own)) => Ok((own.clone(), false)),
+        (None, Target::Any, Actor::Person) => Err(fail("choose the session with --session")),
+    }
+}
+
+/// Runs a send or cancel on the computer that runs `session`. That computer
+/// gets this project's tasks first, so it checks and records the action in its
+/// own copy and pastes in its session; its copy then comes back. Nothing
+/// happens when it cannot be reached.
+#[allow(clippy::too_many_arguments)]
+fn forwarded(
+    config: &Config,
+    machines: &Machines,
+    project: &(String, String),
+    uuid: &str,
+    action: &str,
+    args: &[String],
+    session: &SessionKey,
+    who: &Actor,
+) -> Result<i32> {
+    let alias = machines
+        .alias(&session.machine)
+        .ok_or_else(|| fail(format!("{} is on a computer this one has not verified", session.session)))?;
+    crate::swarm::replicate(config, &alias, &project.0).map_err(|e| fail(format!("{e:#}")))?;
+    let mut remote: Vec<String> = vec!["task".into(), action.into(), uuid.into(), "--project".into(), project.0.clone()];
+    if action == "send" && option(args, "--session")?.is_some() {
+        remote.extend(["--session".into(), session.session.clone()]);
+    }
+    if let Some(reason) = option(args, "--reason")?.filter(|_| action == "cancel") {
+        remote.extend(["--reason".into(), reason.into()]);
+    }
+    if let Actor::Agent(agent) = who {
+        remote.extend(["--agent".into(), format!("{}/{}", agent.machine, agent.session)]);
+    }
+    let reply = crate::swarm::remote_hgs(config, &alias, &remote).map_err(|e| {
+        fail(format!("{e:#}").replacen(&format!("{alias}: hgs: "), &format!("{alias}: "), 1))
+    })?;
+    // Its copy comes back now; when that fails the next sync brings it.
+    if let Err(error) = crate::swarm::replicate(config, &alias, &project.0) {
+        eprintln!("hgs: {error:#}");
+    }
+    let task = store(config, &project.0)?.find(uuid).map_err(fail)?;
+    let mut out = json!({"task": view(&task, &project.1, true, machines)?});
+    for key in ["receipt", "cancel_message"] {
+        if let Some(value) = reply.get(key) {
+            out[key] = value.clone();
+        }
+    }
+    print(out)
+}
+
 /// Pastes the task prompt into its session on this computer.
 fn send(
-    here: &str,
     store: &ProjectStore,
     task: &mut TaskDoc,
     project_name: &str,
-    args: &[String],
+    session: SessionKey,
+    pin: bool,
     who: &Actor,
 ) -> Result<Value> {
     // Everything that could stop the send from being recorded is checked
     // before the paste; only I/O can fail after it.
     task.check_send().map_err(fail)?;
     let view = task.view().map_err(fail)?;
-    let local = |session: &str| SessionKey {
-        machine: here.to_owned(),
-        session: session.to_owned(),
-    };
-    // The session: --session (an agent's choice also pins the task), the
-    // pinned session, or for an Any task the agent's own session.
-    let (session, pin) = match (option(args, "--session")?, &view.target, who) {
-        (Some(name), Target::Session(pinned), Actor::Person) if *pinned != local(name) => {
-            return Err(fail(format!(
-                "{} is pinned to {}; change its session first",
-                task.id(),
-                pinned.session
-            )))
-        }
-        (Some(name), _, Actor::Person) => (local(name), false),
-        (Some(name), _, Actor::Agent(_)) => (local(name), true),
-        (None, Target::Session(pinned), _) if pinned.machine == here => (pinned.clone(), false),
-        (None, Target::Session(pinned), _) => {
-            return Err(fail(format!(
-                "{} is for {} on another computer; sending there comes with replication",
-                task.id(),
-                pinned.session
-            )))
-        }
-        (None, Target::Any, Actor::Agent(own)) => (own.clone(), false),
-        (None, Target::Any, Actor::Person) => {
-            return Err(fail("choose the session with --session"))
-        }
-    };
     let settings = store.project().map_err(fail)?.settings();
     let starts = store
         .agent_starts_since(now() - 3_600_000)
@@ -456,7 +530,6 @@ fn send(
 
 /// Moves the task to In review and pastes the cancel message; the turn is not stopped.
 fn cancel(
-    here: &str,
     store: &ProjectStore,
     task: &mut TaskDoc,
     project_name: &str,
@@ -490,11 +563,7 @@ fn cancel(
         Tools::Cli,
     )
     .map_err(fail)?;
-    let pasted = if session.machine == here {
-        crate::state::submit_task_text(&session.session, &message).map_err(|e| e.to_string())
-    } else {
-        Err(format!("{} is on another computer", session.session))
-    };
+    let pasted = crate::state::submit_task_text(&session.session, &message).map_err(|e| e.to_string());
     Ok(match pasted {
         Ok(receipt) => json!({"cancel_message": {"sent": true, "receipt": receipt}}),
         Err(error) => {
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cargo +1.99.1 clippy --target-dir .ci-build/cargo --all-targets -q -- -D warnings
cargo +1.99.1 build --target-dir .ci-build/cargo -q
HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_task_routing tests.test_tasks tests.test_swarm
python3 scripts/ci/check-source.py
```
Expected: no lint output; `Ran 51 tests … OK`; `Source syntax, privacy, child process and release version checks passed.`

- [ ] **Step 5: Commit**

```bash
git add docs/reference.md \
  src/swarm/mod.rs \
  src/swarm/replica.rs \
  src/swarm/transport.rs \
  src/tasks.rs \
  tests/test_task_routing.py \
  tests/test_tasks.py
git commit -m "Run Send now and Cancel on the session's computer"
```

---

### Task 4: The desktop

**Files:**
- Modify: `tray/tests/test_swarmcontroller.cpp`, `tray/tests/test_tasksview.cpp`, `tray/src/SwarmDialog.cpp`, `tray/src/TasksView.cpp`

**Interfaces:**
- Consumes: `hgs task send … --session S --host H` and the routing (Task 3); `peers[alias].tasks_outdated` (Task 2).
- Produces: `TasksView::sendBlocker` no longer blocks a session on another computer; `steps(SendNow)` adds `--host <open session's computer>` for an Any session task; `showSwarmDialog`'s computer status ends with " · Update Zerus there to share tasks" for a member flagged `tasks_outdated`.

A refusal for an unreachable computer reaches the person as any refused action does (the window's notice), with hgs's reason.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/test_swarmcontroller.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_swarmcontroller.cpp b/tray/tests/test_swarmcontroller.cpp
index a95f38c5..36a5f89b 100644
--- a/tray/tests/test_swarmcontroller.cpp
+++ b/tray/tests/test_swarmcontroller.cpp
@@ -542,6 +542,25 @@ print(json.dumps(result))
         QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(10000);
         showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
     }
+    void computersThatDoNotShareTasksAreNamed() {
+        HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
+        auto snapshot=controller.snapshot();
+        snapshot["machines"]=QJsonArray{QJsonObject{{"id","node-mac"},{"name","Mac"},{"connection","mac"}},QJsonObject{{"id","node-pi"},{"name","Pi"},{"connection","pi"}}};
+        snapshot["peers"]=QJsonObject{{"mac",QJsonObject{{"last_sync",12345},{"tasks_outdated",true}}},{"pi",QJsonObject{{"last_sync",12345}}}};
+        controller.acceptExternal(snapshot);FleetState fleet;bool checked=false;
+        QTimer::singleShot(0,this,[&]{
+            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);
+            QTimer::singleShot(50,dialog,[&,dialog]{
+                auto *table=dialog->findChild<QTableWidget*>("swarmMachines");
+                const auto status=[&](const QString &name){for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->text()==name)return table->item(row,2)->text();return QString();};
+                QVERIFY2(status("Mac").endsWith(" · Update Zerus there to share tasks"),qPrintable(status("Mac")));
+                QVERIFY2(status("Pi").startsWith("Synced ")&&!status("Pi").contains("tasks"),qPrintable(status("Pi")));
+                checked=true;dialog->reject();
+            });
+        });
+        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,this,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(10000);
+        showSwarmDialog(&client,&controller,fleet,nullptr,[]{});QVERIFY(checked);
+    }
     void largeFleetKeepsScrollAndSelection() {
         HgsClient client(FakeCli::program(m_program));SwarmController controller(&client);controller.start(organization("Original"));QTRY_VERIFY(controller.settled());
         auto snapshot=controller.snapshot();QJsonArray machines;
````

Apply this change to `tray/tests/test_tasksview.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_tasksview.cpp b/tray/tests/test_tasksview.cpp
index 5f956049..3427bdff 100644
--- a/tray/tests/test_tasksview.cpp
+++ b/tray/tests/test_tasksview.cpp
@@ -137,15 +137,25 @@ void TestTasksView::sendNowNamesWhyASessionCannotTakeIt() {
         FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}}),
         FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/stopped")}}),
         FakeTasksCli::task(3, "todo", {{"target", FakeTasksCli::pinned("codex/ios", "orchard-mac")}}),
-        FakeTasksCli::task(4, "todo", {{"target", FakeTasksCli::pinned("codex/web")}})}));
+        FakeTasksCli::task(4, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}),
+        FakeTasksCli::task(5, "todo")}));
     HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
     view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
-    QTRY_VERIFY(row(view, 4)); tile(view, "todo")->click();
+    QTRY_VERIFY(row(view, 5)); tile(view, "todo")->click();
     const auto blocked = [&](int n) { auto *b = action(row(view, n), "sendNow"); return b->isEnabled() ? QString() : b->toolTip(); };
     QCOMPARE(blocked(1), QString("codex/gone no longer exists — choose another session"));
     QCOMPARE(blocked(2), QString("The agent is not running. Resume it before sending."));
-    QCOMPARE(blocked(3), QString("codex/ios runs on another computer; sending there comes with replication"));
     QCOMPARE(blocked(4), QString());
+    // A session on another computer takes it there; hgs runs the send on that computer.
+    QCOMPARE(blocked(3), QString());
+    action(row(view, 3), "sendNow")->click();
+    QTRY_VERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "send", "uuid-3", "--project", "project-1"})));
+    // An Any session task goes to the open session, there too.
+    view.setContext("project-1", "orchard", "orchard-mac", "codex/ios", "orchard-mac\ncodex/ios", sessions());
+    QTRY_VERIFY(row(view, 5) && action(row(view, 5), "sendNow")->isEnabled());
+    action(row(view, 5), "sendNow")->click();
+    QTRY_VERIFY(FakeTasksCli::calls(dir.path()).contains(
+        QStringList({"task", "send", "uuid-5", "--session", "codex/ios", "--host", "orchard-mac", "--project", "project-1"})));
 }
 
 void TestTasksView::theInputFilesATaskForTheOpenSession() {
````

- [ ] **Step 2: Run them to see them fail**

Run:
```bash
cmake --build .ci-build/gui --target test_tasksview test_swarmcontroller -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview sendNowNamesWhyASessionCannotTakeIt
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_swarmcontroller computersThatDoNotShareTasksAreNamed
```
Expected: `Actual (blocked(3)): "codex/ios runs on another computer; sending there comes with replication"`, `Expected (QString()): ""`; and `'status("Mac").endsWith(" · Update Zerus there to share tasks")' returned FALSE. (Synced …)`.

- [ ] **Step 3: Implement**

Apply this change to `tray/src/SwarmDialog.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SwarmDialog.cpp b/tray/src/SwarmDialog.cpp
index 26a6365e..6b997610 100644
--- a/tray/src/SwarmDialog.cpp
+++ b/tray/src/SwarmDialog.cpp
@@ -128,6 +128,8 @@ void showSwarmDialog(HgsClient *client,SwarmController *controller,const FleetSt
             auto *access=machines->item(row,1);access->setText(machine.value("local").toBool()?QObject::tr("This computer"):alias.isEmpty()?QObject::tr("Not configured"):alias);access->setToolTip(access->text());
             const auto state=peers.value(alias).toObject();QString text=machine.value("local").toBool()?QObject::tr("Local catalog"):QObject::tr("Shared through swarm");
             if(!state.isEmpty()){text=state.value("error").toString();if(text.isEmpty()){const auto timestamp=qint64(state.value("last_sync").toDouble());text=timestamp?QObject::tr("Synced %1").arg(QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().toString("HH:mm:ss")):QObject::tr("Waiting to sync");}}
+            // Its Zerus syncs the catalog but not tasks.
+            if(state.value("tasks_outdated").toBool())text+=QObject::tr(" · Update Zerus there to share tasks");
             auto *item=machines->item(row,2);item->setText(text);item->setToolTip(text);
         }
         machines->setSortingEnabled(true);filterMachines();
````

Apply this change to `tray/src/TasksView.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.cpp b/tray/src/TasksView.cpp
index d1bba747..cf39c48b 100644
--- a/tray/src/TasksView.cpp
+++ b/tray/src/TasksView.cpp
@@ -256,9 +256,9 @@ QString TasksView::sendBlocker(const TaskItem &task) const {
     const QString host = any ? m_host : task.targetHost, name = any ? m_session : task.targetSession;
     if (name.isEmpty()) return tr("Open the session to send this task to");
     if (!any && !task.targetKnown) return tr("%1 is on a computer this one has not verified").arg(name);
+    // A session on another computer takes the task there; hgs refuses when it cannot be reached.
     const auto *found = session(host, name);
     if (!found) return tr("%1 no longer exists — choose another session").arg(name);
-    if (!host.isEmpty()) return tr("%1 runs on another computer; sending there comes with replication").arg(name);
     return found->blocker;
 }
 
@@ -340,9 +340,10 @@ QList<TasksView::Step> TasksView::steps(const TaskItem &task, TaskAction action)
     case TaskAction::ToTodo: case TaskAction::BackToTodo: return {{withProject({"stage", task.uuid, "todo"}), {}}};
     case TaskAction::Accept: return {{withProject({"stage", task.uuid, "done"}), {}}};
     case TaskAction::SendNow: {
-        // An Any session task goes to the open session.
+        // An Any session task goes to the open session, on whichever computer runs it.
         QStringList arguments{"send", task.uuid};
         if (task.targetSession.isEmpty()) arguments << "--session" << m_session;
+        if (task.targetSession.isEmpty() && !m_host.isEmpty()) arguments << "--host" << m_host;
         return {{withProject(arguments), {}}};
     }
     case TaskAction::Edit: case TaskAction::Cancel: break;
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake --build .ci-build/gui --target test_tasksview test_swarmcontroller -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_swarmcontroller
```
Expected: `test_tasksview` `Totals: 17 passed`; `test_swarmcontroller` `Totals: 31 passed`.

- [ ] **Step 5: Run everything**

Run:
```bash
cmake --build .ci-build/gui -j 8
ctest --test-dir .ci-build/gui --parallel 8 --timeout 600 --output-on-failure
python3 scripts/ci/check-source.py
```
Expected: every suite passes. If `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7, it fails on the branch's previous commit too on affected machines (Known limits). Then `Source syntax, privacy, child process and release version checks passed.`

- [ ] **Step 6: Commit**

```bash
git add tray/src/SwarmDialog.cpp \
  tray/src/TasksView.cpp \
  tray/tests/test_swarmcontroller.cpp \
  tray/tests/test_tasksview.cpp
git commit -m "Send tasks to sessions on other computers from the desktop"
```

---
