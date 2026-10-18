# Task replica, phase 1: the crate and local tasks — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `zerus-replica` crate (Loro, the task schema, its rules, storage and golden vectors) and the `hgs task` commands that keep, order, send, report and cancel tasks on one computer.

**Architecture:** `crates/replica` is a library and the only code that imports `loro` (pinned `=1.16.2`, default features off). A task is one Loro document (`meta`, `fields`, `text`, `log`); a project's settings are a second, small document; `ProjectStore` keeps them below `<HGS_STATE_DIR>/replica/<sha256(project ID)>/` under a per-project lock, as a snapshot plus appended, checksummed updates. hgs depends on the crate by path; `src/tasks.rs` implements `hgs task …` (JSON output) on top of it, resolves projects through the swarm catalog and pastes prompts through the existing `input::submit` path.

**Tech Stack:** Rust 1.99.1 (`rust-version = "1.99"`), `loro` 1.16.2, serde/serde_json, `fs2` file locks, `sha2`, `uuid`; Python `unittest` CLI tests with an isolated tmux socket.

**Spec:** `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md` (phase 1) and the task logic in `docs/superpowers/specs/2026-10-13-project-task-queue-design.md`.

## Global Constraints

- Work in the `task-tracker` worktree (a separate checkout of this repository on branch `task-tracker`); never switch branches in the shared main checkout. Every command below runs from the worktree root; commands for the crate alone run in a `(cd crates/replica && …)` subshell.
- Rust: `rust-version = "1.99"`; build, lint and test with `cargo +1.99.1`. Zerus tracks the newest stable Rust.
- `loro = "=1.16.2"` exactly, and only in `crates/replica/Cargo.toml`; no other crate or module imports `loro`.
- The crate is rustfmt-formatted and clippy-clean with `-D warnings`; hgs changes stay clippy-clean with `-D warnings`.
- Every `hgs task` command prints one JSON object on stdout; errors go to stderr with exit code 1.
- Task IDs are `T-` plus six base-36 characters; references are `PROJECT/T-xxxxxx` or `T-xxxxxx` with `--project`.
- Prompts, reports and comments are at most 64 KiB and carry no terminal control characters other than newline and tab; prompts never start with `/` or `!`; a rendered task prompt also fits 64 KiB; templates must start with `[Zerus task`.
- Phase 1 prompts name the CLI (`hgs task report …`, `hgs task get …`): `Tools::Cli`.
- Tests: synthetic names only (`orchard`, `team/web`, `codex/api`), never real project or session names. Tmux only through a shim in `$HOME/.local/bin` pointing at a private `-S` socket; pop `TMUX`, `TMUX_PANE`, `HGS_RUN_ID`, `HGS_SESSION`, `HGS_EXECUTABLE`, `HGS_SSH`, `HGS_PEERS`; never run a bare `tmux kill-server`.
- Repository text is English and passes `scripts/check-source-privacy.py` (no home or workspace paths). Commits carry no Claude attribution.
- Lock order: catalog lookups (project, machine identity) and stdin reads happen before a project store is opened; the hgs state lock (pasting) is taken only while a project lock is held, never the other way round.
- An agent is a caller with both `HGS_SESSION` and `HGS_RUN_ID`, as hooks see it; anyone else is the person.
- Licenses: Loro links `im`, `bitmaps` and `sized-chunks` under MPL-2.0 (accepted by the owner, used unmodified); every shipped crate needs a notice that `scripts/collect-licenses.py` can find.

## Review Focus

- Two `hgs task` processes changing one project at once (the GUI and an agent): the project lock serializes them and no task is lost — `test_concurrent_adds_all_land`, which hands all six processes their input before any of them finishes (Task 6).
- A crash in the middle of appending updates (a cut or zero-filled tail): the task still loads, the next save rewrites the snapshot, and later saves survive a reload — `a_cut_tail_is_repaired_and_later_saves_survive` (Task 4).
- A prompt that contains `{ref}` or other braces: it is pasted as written, never substituted again — `first_send_prompt` and `limits` (Task 3).
- A project whose name contains `/`: `team/web/T-xxxxxx` still resolves — `test_a_project_name_with_a_slash` (Task 6).
- hgs refuses the paste (the session does not exist or is not ready): the error is shown and the task stays where it was, with no `sent` entry — `test_a_refused_send_changes_nothing` (Task 7).

## File Structure

| Path | Responsibility |
|---|---|
| `crates/replica/Cargo.toml`, `.cargo/config.toml`, `Cargo.lock` | The crate manifest with the exact Loro pin and its own lockfile |
| `crates/replica/src/lib.rs` | Module list and public re-exports |
| `crates/replica/src/error.rs` | `Error`/`Result`, conversions from Loro, I/O and JSON errors |
| `crates/replica/src/ids.rs` | Short task IDs and UUIDs |
| `crates/replica/src/rank.rs` | Fractional ranks for the To do and Backlog order |
| `crates/replica/src/schema.rs` | `Stage`, `Target`, `Actor`, `SessionKey`, `LogEntry`, `TaskView`, limits |
| `crates/replica/src/task.rs` | `TaskDoc`: the task document and its rules |
| `crates/replica/src/project.rs` | `ProjectDoc` and `Settings` (templates, agent switch and limit) |
| `crates/replica/src/render.rs` | The task prompt and the cancel message |
| `crates/replica/src/store.rs` | `ProjectStore`: files, lock, append and fold, damaged files |
| `crates/replica/tests/vectors.rs`, `tests/vectors/*` | Golden vectors every build must read |
| `Cargo.toml`, `Cargo.lock` | hgs depends on the crate by path |
| `src/tasks.rs` | `hgs task …` |
| `src/main.rs`, `src/cli.rs` | The `task` command and its usage line |
| `src/swarm/mod.rs` | `projects(config)`: catalog projects as (ID, name) |
| `src/state/mod.rs`, `src/state/input.rs` | `submit_task_text`: paste text for a session's current run |
| `tests/test_tasks.py` | CLI tests: commands, and delivery on a private tmux socket |
| `scripts/collect-licenses.py`, `packaging/rust-licenses/*` | Notices for every shipped crate, including those that publish none |
| `THIRD_PARTY_NOTICES.md` | Loro and its MPL-2.0 dependencies |
| `mac-install.sh`, `tests/test_mac_install.py` | Mac installs carry the crate's sources |
| `.github/workflows/checks.yml`, `tests/test_ci_security.py`, `.github/dependabot.yml` | CI and dependency proposals for the crate |
| `docs/reference.md`, `docs/ci-and-aur.md`, `CONTRIBUTING.md`, `AGENTS.md`, `CLAUDE.md`, `docs/rust-dependencies.json` | Documentation and the dependency inventory |

**Known limits of this phase** (recorded, not fixed here): every command decodes all of the project's task documents, Done included, so a project with thousands of tasks gets slower — measure in phase 2 before adding an index; `hgs task` works on local sessions only, so a Windows control console cannot run it.

---

### Task 0: Record the design on the branch

**Files:**
- Add: `docs/superpowers/specs/2026-10-13-project-task-queue-design.md`, `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`, `design/task-tracker/index.html`, `docs/superpowers/plans/2026-10-18-task-replica-phase1.md`

- [ ] **Step 1: Check the worktree**

Run: `git branch --show-current && git status --porcelain`
Expected: `task-tracker`, and only the four paths above as untracked (`design/task-tracker/` as a directory).

- [ ] **Step 2: Check privacy**

Run: `python3 scripts/check-source-privacy.py`
Expected: `0 privacy guard findings`. The guard rejects home and workspace paths and personal data; fix anything it reports before committing.

- [ ] **Step 3: Commit them**

```bash
git add docs/superpowers/specs/2026-10-13-project-task-queue-design.md \
  docs/superpowers/specs/2026-10-18-task-replication-loro-design.md \
  design/task-tracker docs/superpowers/plans/2026-10-18-task-replica-phase1.md
git commit -m "Design the project task tracker and its Loro replication"
```

---

### Task 1: The crate skeleton, short IDs and ranks

**Files:**
- Create: `crates/replica/Cargo.toml`, `crates/replica/.cargo/config.toml`, `crates/replica/src/lib.rs`, `crates/replica/src/error.rs`, `crates/replica/src/ids.rs`, `crates/replica/src/rank.rs`
- Create (generated): `crates/replica/Cargo.lock`

**Interfaces:**
- Produces: `zerus_replica::{Error, Result}` (`Error::new(impl Into<String>)`, `Display`); `ids::{short_id() -> String, is_short_id(&str) -> bool, new_uuid() -> String, is_uuid(&str) -> bool}`; `rank::{between(Option<&str>, Option<&str>) -> Result<String>, is_rank(&str) -> bool}`.

- [ ] **Step 1: Write the manifest and resolver config**

`crates/replica/Cargo.toml`:

```toml
[package]
name = "zerus-replica"
version = "0.1.0"
edition = "2021"
rust-version = "1.99"
license = "MIT"
description = "Replicated Zerus documents on Loro: the task schema, its rules and storage"
publish = false

[dependencies]
# The only crate in Zerus that imports Loro. Raise this pin deliberately: the
# golden vectors in tests/vectors must still decode (see tests/vectors.rs).
loro = { version = "=1.16.2", default-features = false }
fs2 = "0.4"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
sha2 = "0.10"
uuid = { version = "1", features = ["v4"] }

[dev-dependencies]
tempfile = "3"
```

`crates/replica/.cargo/config.toml` (pick dependency versions that support `rust-version`, as the other crates do):

```toml
[resolver]
incompatible-rust-versions = "fallback"
```

- [ ] **Step 2: Write `lib.rs` and `error.rs`**

`crates/replica/src/lib.rs`:

```rust
//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
pub mod rank;

pub use error::{Error, Result};
```

`crates/replica/src/error.rs`:

```rust
use std::fmt;

/// A refusal or failure, with a message fit to show the person.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Error(String);

impl Error {
    pub fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

impl From<loro::LoroError> for Error {
    fn from(error: loro::LoroError) -> Self {
        Self(format!("replica: {error}"))
    }
}

impl From<loro::LoroEncodeError> for Error {
    fn from(error: loro::LoroEncodeError) -> Self {
        Self(format!("replica: {error}"))
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self(format!("replica storage: {error}"))
    }
}

impl From<serde_json::Error> for Error {
    fn from(error: serde_json::Error) -> Self {
        Self(format!("replica: {error}"))
    }
}

pub type Result<T> = std::result::Result<T, Error>;
```

- [ ] **Step 3: Write `ids.rs` and `rank.rs` with their tests**

`crates/replica/src/ids.rs`:

```rust
//! Task identities: a UUID names the document; `T-` and six base-36
//! characters name the task for people and agents, without coordination.
const ALPHABET: &[u8; 36] = b"0123456789abcdefghijklmnopqrstuvwxyz";

/// A random short ID. Callers redraw it when it collides with a local task.
pub fn short_id() -> String {
    let bytes = uuid::Uuid::new_v4().into_bytes();
    let mut value = u64::from_le_bytes(bytes[..8].try_into().expect("eight bytes"));
    let mut id = String::from("T-");
    for _ in 0..6 {
        id.push(ALPHABET[(value % 36) as usize] as char);
        value /= 36;
    }
    id
}

pub fn is_short_id(value: &str) -> bool {
    value.len() == 8 && value.starts_with("T-") && value[2..].bytes().all(|b| ALPHABET.contains(&b))
}

pub fn new_uuid() -> String {
    uuid::Uuid::new_v4().to_string()
}

pub fn is_uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| id.to_string() == value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_ids_are_well_formed_and_vary() {
        let ids: std::collections::BTreeSet<_> = (0..200).map(|_| short_id()).collect();
        assert!(ids.iter().all(|id| is_short_id(id)));
        assert!(ids.len() > 190);
        assert!(!is_short_id("T-ABCDEF"));
        assert!(!is_short_id("T-abc"));
        assert!(is_uuid(&new_uuid()));
        assert!(!is_uuid("not-a-uuid"));
    }
}
```

`crates/replica/src/rank.rs` — a rank never ends in `0`, so there is always room before and after it:

```rust
//! Fractional ranks: the To do order is a key on each task, so moving one task
//! rewrites one field of one document. Digits are 0-9A-Za-z in ASCII order; a
//! rank never ends in `0`, so there is always room before and after it.
use crate::{Error, Result};

const DIGITS: &[u8; 62] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

fn digit(c: u8) -> Option<usize> {
    DIGITS.iter().position(|&d| d == c)
}

pub fn is_rank(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 64
        && value.bytes().all(|c| digit(c).is_some())
        && !value.ends_with('0')
}

/// A rank strictly between `low` and `high`; `None` leaves that side open.
pub fn between(low: Option<&str>, high: Option<&str>) -> Result<String> {
    for rank in [low, high].into_iter().flatten() {
        if !is_rank(rank) {
            return Err(Error::new(format!("invalid rank {rank:?}")));
        }
    }
    if let (Some(l), Some(h)) = (low, high) {
        if l >= h {
            return Err(Error::new("rank bounds are out of order"));
        }
    }
    let low = low.unwrap_or("").as_bytes();
    let mut high = high.map(str::as_bytes);
    let mut out = Vec::new();
    let mut i = 0;
    loop {
        let l = low.get(i).map_or(0, |&c| digit(c).expect("checked"));
        let h = high.map_or(62, |h| h.get(i).map_or(62, |&c| digit(c).expect("checked")));
        if l == h {
            out.push(DIGITS[l]);
            i += 1;
            continue;
        }
        let mid = (l + h) / 2;
        if mid > l {
            out.push(DIGITS[mid]);
            break;
        }
        // Adjacent digits: keep the lower one; above it nothing bounds us.
        out.push(DIGITS[l]);
        i += 1;
        high = None;
    }
    let rank = String::from_utf8(out).expect("ASCII digits");
    if rank.len() > 64 {
        return Err(Error::new("ranks are exhausted here; reorder the list"));
    }
    Ok(rank)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn between_orders_strictly() {
        let first = between(None, None).unwrap();
        let after = between(Some(&first), None).unwrap();
        let before = between(None, Some(&first)).unwrap();
        let middle = between(Some(&first), Some(&after)).unwrap();
        assert!(before < first && first < middle && middle < after);
        for rank in [&first, &after, &before, &middle] {
            assert!(is_rank(rank), "{rank}");
        }
    }

    #[test]
    fn repeated_inserts_stay_ordered() {
        // Always inserting right after the same rank, and right before it.
        let mut low = between(None, None).unwrap();
        let high = between(Some(&low), None).unwrap();
        for _ in 0..40 {
            let next = between(Some(&low), Some(&high)).unwrap();
            assert!(low < next && next < high);
            low = next;
        }
        let mut high = between(None, None).unwrap();
        for _ in 0..40 {
            let next = between(None, Some(&high)).unwrap();
            assert!(next < high && is_rank(&next));
            high = next;
        }
    }

    #[test]
    fn adjacent_digits_and_bad_bounds() {
        let r = between(Some("V"), Some("W")).unwrap();
        assert!("V" < r.as_str() && r.as_str() < "W");
        assert!(between(Some("W"), Some("V")).is_err());
        assert!(between(Some("V0"), None).is_err());
        assert!(between(Some("a-b"), None).is_err());
    }
}
```

- [ ] **Step 4: Generate the lockfile, format, lint and test**

Run:
```bash
(cd crates/replica &&
  cargo +1.99.1 generate-lockfile &&
  cargo +1.99.1 fmt --check &&
  cargo +1.99.1 clippy --locked --all-targets -- -D warnings &&
  cargo +1.99.1 test --locked)
```
Expected: no format or lint output; `test result: ok. 4 passed`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica
git commit -m "Add the replica crate with short task IDs and fractional ranks"
```

---

### Task 2: The task document and its rules

**Files:**
- Create: `crates/replica/src/schema.rs`, `crates/replica/src/task.rs`
- Modify: `crates/replica/src/lib.rs`

**Interfaces:**
- Consumes: `ids`, `rank`, `Error`, `Result` from Task 1.
- Produces: `Stage { Backlog, Todo, Active, Review, Done }` with `as_str()`, `label()`, `parse(&str)`; `SessionKey { machine, session }`; `Target { Any, Session(SessionKey) }`; `Actor { Person, Agent(SessionKey) }`; `LogEntry { kind, actor, at, text, session }`; `TaskView { uuid, id, project, schema, writable, created_at, author, stage, target, rank, session, summary, cancelled_at, cancel_reason, text, log }`; `SCHEMA = 1`, `MAX_TEXT = 65536`, `MAX_DOCUMENT = 4 MiB`; `NewTask<'a> { project, id, text, stage, target, author, rank, now }`; `TaskDoc::{create(NewTask) -> Result<TaskDoc>, from_bytes(&[u8]) -> Result<TaskDoc>, uuid(), import(&[u8]), snapshot() -> Result<Vec<u8>>, version() -> Vec<u8>, export_since(&[u8]) -> Result<Vec<u8>>, id() -> String, schema() -> i64, stage() -> Result<Stage>, rank() -> String, view() -> Result<TaskView>, edit_text(&str, &Actor, i64), set_rank(&str), set_stage(Stage, &Actor, i64), set_target(&Target, &Actor, i64), record_send(&SessionKey, &str, &Actor, i64), already_running() -> Error, record_report(&str, &Actor, i64) -> Result<bool>, record_cancel(&str, &Actor, i64), comment(&str, &Actor, i64), note(&str, &Actor, i64)}`. Times are Unix milliseconds.

The rules come from the task spec: a new task goes to Backlog or To do; the prompt, target and rank change only in Backlog or To do; stages move Backlog ⇄ To do, In review → Done (Accept), In review or Done → To do; a send works from Backlog or To do and is refused for a task already In progress; a report moves In progress to In review and is otherwise only added to the history; Cancel moves In progress to In review, logging `cancelled` and then its reason as a `comment`; a document with a newer `schema` is read-only. Texts (prompts, reports, comments) are refused when empty, over 64 KiB, or carrying terminal control characters other than newline and tab — what hgs refuses to paste. A prompt edit is a character diff; a rewrite too large to diff within 200 ms replaces the text as a whole.

- [ ] **Step 1: Write `schema.rs`**

`crates/replica/src/schema.rs`:

```rust
//! The task schema shared by every replica.
use crate::{Error, Result};
use serde::{Deserialize, Serialize};

/// The layout version of task and project documents this build writes.
pub const SCHEMA: i64 = 1;
/// The longest prompt or comment: the input limit of hgs.
pub const MAX_TEXT: usize = 64 * 1024;
/// A task document this large accepts no further comments.
pub const MAX_DOCUMENT: usize = 4 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Stage {
    Backlog,
    Todo,
    Active,
    Review,
    Done,
}

impl Stage {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Backlog => "backlog",
            Self::Todo => "todo",
            Self::Active => "active",
            Self::Review => "review",
            Self::Done => "done",
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::Backlog => "Backlog",
            Self::Todo => "To do",
            Self::Active => "In progress",
            Self::Review => "In review",
            Self::Done => "Done",
        }
    }

    pub fn parse(value: &str) -> Result<Self> {
        Ok(match value {
            "backlog" => Self::Backlog,
            "todo" => Self::Todo,
            "active" => Self::Active,
            "review" => Self::Review,
            "done" => Self::Done,
            _ => return Err(Error::new(format!("unknown stage {value:?}"))),
        })
    }
}

/// A session on a computer: the computer's swarm identity and the session name.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionKey {
    pub machine: String,
    pub session: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Target {
    Any,
    Session(SessionKey),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Actor {
    Person,
    Agent(SessionKey),
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LogEntry {
    pub kind: String,
    pub actor: Actor,
    pub at: i64,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub text: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub session: Option<SessionKey>,
}

/// A task as callers read it.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct TaskView {
    pub uuid: String,
    pub id: String,
    pub project: String,
    pub schema: i64,
    pub writable: bool,
    pub created_at: i64,
    pub author: Actor,
    pub stage: Stage,
    pub target: Target,
    pub rank: String,
    pub session: Option<SessionKey>,
    pub summary: String,
    pub cancelled_at: Option<i64>,
    pub cancel_reason: String,
    pub text: String,
    pub log: Vec<LogEntry>,
}

pub(crate) fn check_text(text: &str, what: &str) -> Result<()> {
    if text.trim().is_empty() {
        return Err(Error::new(format!("the {what} is empty")));
    }
    if text.len() > MAX_TEXT {
        return Err(Error::new(format!("the {what} is longer than 64 KiB")));
    }
    // hgs pastes only text: terminal control characters never reach an agent.
    if text
        .chars()
        .any(|c| c.is_control() && c != '\n' && c != '\t')
    {
        return Err(Error::new(format!(
            "the {what} contains terminal control characters"
        )));
    }
    Ok(())
}
```

- [ ] **Step 2: Write `task.rs`, tests included**

`crates/replica/src/task.rs`:

```rust
//! One task as one Loro document: `meta` (identity, set once), `fields`
//! (last writer wins), `text` (the prompt, merged by character) and `log`
//! (history, append only).
use crate::schema::{
    check_text, Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, SCHEMA,
};
use crate::{ids, rank, Error, Result};
use loro::{ExportMode, LoroDoc, LoroMap, LoroValue, UpdateOptions, VersionVector};

pub struct NewTask<'a> {
    pub project: &'a str,
    pub id: &'a str,
    pub text: &'a str,
    pub stage: Stage,
    pub target: Target,
    pub author: Actor,
    pub rank: &'a str,
    pub now: i64,
}

pub struct TaskDoc {
    uuid: String,
    doc: LoroDoc,
}

fn string(map: &LoroMap, key: &str) -> Option<String> {
    match map.get(key)?.into_value().ok()? {
        LoroValue::String(value) => Some(value.to_string()),
        _ => None,
    }
}

fn integer(map: &LoroMap, key: &str) -> Option<i64> {
    match map.get(key)?.into_value().ok()? {
        LoroValue::I64(value) => Some(value),
        _ => None,
    }
}

fn decode<T: serde::de::DeserializeOwned>(map: &LoroMap, key: &str) -> Result<T> {
    let text =
        string(map, key).ok_or_else(|| Error::new(format!("task field {key} is missing")))?;
    Ok(serde_json::from_str(&text)?)
}

impl TaskDoc {
    pub fn create(task: NewTask<'_>) -> Result<Self> {
        if !matches!(task.stage, Stage::Backlog | Stage::Todo) {
            return Err(Error::new("a new task goes to Backlog or To do"));
        }
        check_text(task.text, "prompt")?;
        if !ids::is_short_id(task.id) {
            return Err(Error::new(format!("invalid task ID {:?}", task.id)));
        }
        if !rank::is_rank(task.rank) {
            return Err(Error::new("invalid rank"));
        }
        if task.project.is_empty() || task.project.len() > 128 {
            return Err(Error::new("invalid project ID"));
        }
        let uuid = ids::new_uuid();
        let doc = LoroDoc::new();
        let meta = doc.get_map("meta");
        meta.insert("schema", SCHEMA)?;
        meta.insert("uuid", uuid.as_str())?;
        meta.insert("id", task.id)?;
        meta.insert("project", task.project)?;
        meta.insert("created_at", task.now)?;
        meta.insert("author", serde_json::to_string(&task.author)?)?;
        let fields = doc.get_map("fields");
        fields.insert("stage", task.stage.as_str())?;
        fields.insert("target", serde_json::to_string(&task.target)?)?;
        fields.insert("rank", task.rank)?;
        fields.insert("summary", "")?;
        doc.get_text("text").insert(0, task.text)?;
        let this = Self { uuid, doc };
        this.log("created", &task.author, task.now, "", None)?;
        this.doc.commit();
        Ok(this)
    }

    /// A document from a snapshot, possibly followed by updates.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        let doc = LoroDoc::new();
        doc.import(bytes)?;
        let uuid = string(&doc.get_map("meta"), "uuid")
            .ok_or_else(|| Error::new("not a task document"))?;
        if !ids::is_uuid(&uuid) {
            return Err(Error::new("not a task document"));
        }
        Ok(Self { uuid, doc })
    }

    pub fn uuid(&self) -> &str {
        &self.uuid
    }

    pub fn import(&mut self, bytes: &[u8]) -> Result<()> {
        self.doc.import(bytes)?;
        Ok(())
    }

    pub fn snapshot(&self) -> Result<Vec<u8>> {
        Ok(self.doc.export(ExportMode::Snapshot)?)
    }

    pub fn version(&self) -> Vec<u8> {
        self.doc.oplog_vv().encode()
    }

    pub fn export_since(&self, version: &[u8]) -> Result<Vec<u8>> {
        let version = VersionVector::decode(version)?;
        Ok(self.doc.export(ExportMode::updates(&version))?)
    }

    fn meta(&self) -> LoroMap {
        self.doc.get_map("meta")
    }

    fn fields(&self) -> LoroMap {
        self.doc.get_map("fields")
    }

    pub fn id(&self) -> String {
        string(&self.meta(), "id").unwrap_or_default()
    }

    pub fn schema(&self) -> i64 {
        integer(&self.meta(), "schema").unwrap_or(0)
    }

    pub fn stage(&self) -> Result<Stage> {
        Stage::parse(&string(&self.fields(), "stage").unwrap_or_default())
    }

    pub fn rank(&self) -> String {
        string(&self.fields(), "rank").unwrap_or_default()
    }

    pub fn view(&self) -> Result<TaskView> {
        let meta = self.meta();
        let fields = self.fields();
        let mut log = Vec::new();
        let list = self.doc.get_list("log");
        for index in 0..list.len() {
            let Some(item) = list.get(index) else {
                continue;
            };
            let Ok(container) = item.into_container() else {
                continue;
            };
            let Ok(entry) = container.into_map() else {
                continue;
            };
            log.push(LogEntry {
                kind: string(&entry, "kind").unwrap_or_default(),
                actor: decode(&entry, "actor").unwrap_or(Actor::Person),
                at: integer(&entry, "at").unwrap_or(0),
                text: string(&entry, "text").unwrap_or_default(),
                session: string(&entry, "session").and_then(|s| serde_json::from_str(&s).ok()),
            });
        }
        Ok(TaskView {
            uuid: self.uuid.clone(),
            id: self.id(),
            project: string(&meta, "project").unwrap_or_default(),
            schema: self.schema(),
            writable: self.schema() <= SCHEMA,
            created_at: integer(&meta, "created_at").unwrap_or(0),
            author: decode(&meta, "author")?,
            stage: self.stage()?,
            target: decode(&fields, "target")?,
            rank: self.rank(),
            session: string(&fields, "session").and_then(|s| serde_json::from_str(&s).ok()),
            summary: string(&fields, "summary").unwrap_or_default(),
            cancelled_at: integer(&fields, "cancelled_at"),
            cancel_reason: string(&fields, "cancel_reason").unwrap_or_default(),
            text: self.doc.get_text("text").to_string(),
            log,
        })
    }

    fn log(
        &self,
        kind: &str,
        actor: &Actor,
        at: i64,
        text: &str,
        session: Option<&SessionKey>,
    ) -> Result<()> {
        let list = self.doc.get_list("log");
        let entry = list.insert_container(list.len(), LoroMap::new())?;
        entry.insert("kind", kind)?;
        entry.insert("actor", serde_json::to_string(actor)?)?;
        entry.insert("at", at)?;
        if !text.is_empty() {
            entry.insert("text", text)?;
        }
        if let Some(session) = session {
            entry.insert("session", serde_json::to_string(session)?)?;
        }
        Ok(())
    }

    fn writable(&self) -> Result<()> {
        if self.schema() > SCHEMA {
            return Err(Error::new("Update Zerus to edit this task"));
        }
        Ok(())
    }

    fn require(&self, stages: &[Stage], message: &str) -> Result<Stage> {
        self.writable()?;
        let stage = self.stage()?;
        if !stages.contains(&stage) {
            return Err(Error::new(message));
        }
        Ok(stage)
    }

    pub fn edit_text(&mut self, text: &str, actor: &Actor, now: i64) -> Result<()> {
        self.require(
            &[Stage::Backlog, Stage::Todo],
            "the prompt can be edited only in Backlog or To do",
        )?;
        check_text(text, "prompt")?;
        let current = self.doc.get_text("text");
        let quick = UpdateOptions {
            timeout_ms: Some(200.0),
            ..UpdateOptions::default()
        };
        // A character diff merges best; a rewrite too large to diff in time is
        // replaced as a whole, with the same result.
        if current.update(text, quick).is_err() {
            current.delete(0, current.len_unicode())?;
            current.insert(0, text)?;
        }
        self.log("edited", actor, now, "", None)?;
        self.doc.commit();
        Ok(())
    }

    pub fn set_rank(&mut self, value: &str) -> Result<()> {
        self.require(
            &[Stage::Backlog, Stage::Todo],
            "only tasks in Backlog or To do are ordered",
        )?;
        if !rank::is_rank(value) {
            return Err(Error::new("invalid rank"));
        }
        self.fields().insert("rank", value)?;
        self.doc.commit();
        Ok(())
    }

    /// Backlog ⇄ To do, Accept (In review → Done), Back to To do (from In review or Done).
    pub fn set_stage(&mut self, to: Stage, actor: &Actor, now: i64) -> Result<()> {
        self.writable()?;
        let from = self.stage()?;
        let allowed = matches!(
            (from, to),
            (Stage::Backlog, Stage::Todo)
                | (Stage::Todo, Stage::Backlog)
                | (Stage::Review, Stage::Done)
                | (Stage::Review, Stage::Todo)
                | (Stage::Done, Stage::Todo)
        );
        if !allowed {
            return Err(Error::new(format!(
                "a task cannot go from {} to {}",
                from.label(),
                to.label()
            )));
        }
        self.fields().insert("stage", to.as_str())?;
        self.log("stage", actor, now, to.as_str(), None)?;
        self.doc.commit();
        Ok(())
    }

    pub fn set_target(&mut self, target: &Target, actor: &Actor, now: i64) -> Result<()> {
        self.require(
            &[Stage::Backlog, Stage::Todo],
            "the session can be changed only in Backlog or To do",
        )?;
        self.fields()
            .insert("target", serde_json::to_string(target)?)?;
        let label = match target {
            Target::Any => "any".to_owned(),
            Target::Session(key) => key.session.clone(),
        };
        self.log("target", actor, now, &label, None)?;
        self.doc.commit();
        Ok(())
    }

    /// The send that Zerus pasted into `session`; the task becomes In progress.
    pub fn record_send(
        &mut self,
        session: &SessionKey,
        request_id: &str,
        actor: &Actor,
        now: i64,
    ) -> Result<()> {
        self.writable()?;
        match self.stage()? {
            Stage::Backlog | Stage::Todo => {}
            Stage::Active => return Err(self.already_running()),
            stage => {
                return Err(Error::new(format!(
                    "a task in {} is not sent; move it back to To do first",
                    stage.label()
                )))
            }
        }
        let fields = self.fields();
        fields.insert("stage", Stage::Active.as_str())?;
        fields.insert("session", serde_json::to_string(session)?)?;
        fields.delete("cancelled_at")?;
        fields.delete("cancel_reason")?;
        self.log("sent", actor, now, request_id, Some(session))?;
        self.doc.commit();
        Ok(())
    }

    /// The refusal for a second send of a task this copy shows In progress.
    pub fn already_running(&self) -> Error {
        let session = string(&self.fields(), "session")
            .and_then(|s| serde_json::from_str::<SessionKey>(&s).ok())
            .map_or_else(|| "a session".to_owned(), |key| key.session);
        Error::new(format!("{} is already In progress in {session}", self.id()))
    }

    /// Returns whether the report moved the task to In review; otherwise it
    /// is only added to the history.
    pub fn record_report(&mut self, summary: &str, actor: &Actor, now: i64) -> Result<bool> {
        self.writable()?;
        check_text(summary, "report")?;
        let moved = self.stage()? == Stage::Active;
        if moved {
            let fields = self.fields();
            fields.insert("stage", Stage::Review.as_str())?;
            fields.insert("summary", summary)?;
        }
        self.log("report", actor, now, summary, None)?;
        self.doc.commit();
        Ok(moved)
    }

    pub fn record_cancel(&mut self, reason: &str, actor: &Actor, now: i64) -> Result<()> {
        self.require(&[Stage::Active], "only a task In progress can be cancelled")?;
        let fields = self.fields();
        fields.insert("stage", Stage::Review.as_str())?;
        fields.insert("cancelled_at", now)?;
        fields.insert("cancel_reason", reason.trim())?;
        self.log("cancelled", actor, now, "", None)?;
        // The reason is a comment for people and agents; it is never sent.
        if !reason.trim().is_empty() {
            self.log("comment", actor, now, reason.trim(), None)?;
        }
        self.doc.commit();
        Ok(())
    }

    pub fn comment(&mut self, text: &str, actor: &Actor, now: i64) -> Result<()> {
        self.writable()?;
        check_text(text, "comment")?;
        if self.snapshot()?.len() >= MAX_DOCUMENT {
            return Err(Error::new(
                "this task's history is full; it accepts no more comments",
            ));
        }
        self.log("comment", actor, now, text, None)?;
        self.doc.commit();
        Ok(())
    }

    /// A line Zerus adds to the history, such as a refused cancel message.
    pub fn note(&mut self, text: &str, actor: &Actor, now: i64) -> Result<()> {
        self.log("note", actor, now, text, None)?;
        self.doc.commit();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn agent(session: &str) -> Actor {
        Actor::Agent(SessionKey {
            machine: "m1".into(),
            session: session.into(),
        })
    }

    fn task(stage: Stage) -> TaskDoc {
        TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "Add rate limiting",
            stage,
            target: Target::Any,
            author: Actor::Person,
            rank: "V",
            now: 1,
        })
        .unwrap()
    }

    fn api() -> SessionKey {
        SessionKey {
            machine: "m1".into(),
            session: "codex/api".into(),
        }
    }

    #[test]
    fn a_new_task_has_its_fields_and_a_history() {
        let view = task(Stage::Todo).view().unwrap();
        assert_eq!(
            (view.id.as_str(), view.stage, view.text.as_str()),
            ("T-a1b2c3", Stage::Todo, "Add rate limiting")
        );
        assert_eq!(
            (
                view.author,
                view.target,
                view.rank.as_str(),
                view.schema,
                view.writable
            ),
            (Actor::Person, Target::Any, "V", SCHEMA, true)
        );
        assert_eq!(view.log.len(), 1);
        assert_eq!(view.log[0].kind, "created");
        assert!(TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "x",
            stage: Stage::Active,
            target: Target::Any,
            author: Actor::Person,
            rank: "V",
            now: 1
        })
        .is_err());
        assert!(TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: " ",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Person,
            rank: "V",
            now: 1
        })
        .is_err());
    }

    #[test]
    fn the_lifecycle_follows_the_task_spec() {
        let mut t = task(Stage::Backlog);
        t.set_stage(Stage::Todo, &Actor::Person, 2).unwrap();
        assert!(t.set_stage(Stage::Done, &Actor::Person, 3).is_err());
        t.record_send(&api(), "r1", &Actor::Person, 4).unwrap();
        assert_eq!(t.stage().unwrap(), Stage::Active);
        assert_eq!(
            t.record_send(&api(), "r2", &Actor::Person, 5)
                .unwrap_err()
                .to_string(),
            "T-a1b2c3 is already In progress in codex/api"
        );
        assert!(t.edit_text("other", &Actor::Person, 5).is_err());
        assert!(t
            .record_report("Done; 6 tests.", &agent("codex/api"), 6)
            .unwrap());
        let view = t.view().unwrap();
        assert_eq!(
            (view.stage, view.summary.as_str()),
            (Stage::Review, "Done; 6 tests.")
        );
        // A further report is history only.
        assert!(!t
            .record_report("Also updated the README.", &agent("codex/api"), 7)
            .unwrap());
        assert_eq!(t.view().unwrap().summary, "Done; 6 tests.");
        t.set_stage(Stage::Done, &Actor::Person, 8).unwrap();
        t.set_stage(Stage::Todo, &Actor::Person, 9).unwrap();
        assert_eq!(t.view().unwrap().session, Some(api()));
    }

    #[test]
    fn cancel_moves_to_review_with_the_reason() {
        let mut t = task(Stage::Todo);
        assert!(t.record_cancel("", &Actor::Person, 2).is_err());
        t.record_send(&api(), "r1", &Actor::Person, 3).unwrap();
        t.record_cancel(" Wrong approach ", &Actor::Person, 4)
            .unwrap();
        let view = t.view().unwrap();
        assert_eq!(
            (view.stage, view.cancelled_at, view.cancel_reason.as_str()),
            (Stage::Review, Some(4), "Wrong approach")
        );
        let kinds: Vec<_> = view.log.iter().map(|e| e.kind.as_str()).collect();
        assert_eq!(kinds, ["created", "sent", "cancelled", "comment"]);
        assert_eq!(view.log[3].text, "Wrong approach");
        // A late report after the cancel stays in the history.
        assert!(!t
            .record_report("Stopped; changed one file.", &agent("codex/api"), 5)
            .unwrap());
        t.set_stage(Stage::Todo, &Actor::Person, 6).unwrap();
        t.record_send(&api(), "r2", &Actor::Person, 7).unwrap();
        assert_eq!(t.view().unwrap().cancelled_at, None);
    }

    #[test]
    fn edits_targets_ranks_and_comments() {
        let mut t = task(Stage::Todo);
        t.edit_text("Add strict rate limiting", &Actor::Person, 2)
            .unwrap();
        t.set_target(&Target::Session(api()), &Actor::Person, 3)
            .unwrap();
        t.set_rank("W").unwrap();
        t.comment("Use Redis", &agent("codex/web"), 4).unwrap();
        assert!(t
            .comment(&"x".repeat(64 * 1024 + 1), &Actor::Person, 5)
            .is_err());
        assert!(t.set_rank("W0").is_err());
        let view = t.view().unwrap();
        assert_eq!(
            (view.text.as_str(), view.target, view.rank.as_str()),
            ("Add strict rate limiting", Target::Session(api()), "W")
        );
        assert_eq!(
            view.log.iter().map(|e| e.kind.as_str()).collect::<Vec<_>>(),
            ["created", "edited", "target", "comment"]
        );
    }

    #[test]
    fn a_large_rewrite_is_replaced_quickly() {
        let mut t = task(Stage::Todo);
        let long = "x".repeat(60_000);
        let started = std::time::Instant::now();
        t.edit_text(&long, &Actor::Person, 2).unwrap();
        t.edit_text("Short again", &Actor::Person, 3).unwrap();
        assert_eq!(t.view().unwrap().text, "Short again");
        assert!(started.elapsed().as_secs() < 10, "{:?}", started.elapsed());
    }

    #[test]
    fn concurrent_edits_merge_the_same_way_everywhere() {
        let mut a = task(Stage::Todo);
        a.record_send(&api(), "r1", &Actor::Person, 1).unwrap();
        a.record_report("Done.", &agent("codex/api"), 1).unwrap();
        let mut b = TaskDoc::from_bytes(&a.snapshot().unwrap()).unwrap();
        let (va, vb) = (a.version(), b.version());
        a.set_stage(Stage::Done, &Actor::Person, 2).unwrap();
        b.set_stage(Stage::Todo, &agent("codex/api"), 2).unwrap();
        b.edit_text("Add strict rate limiting", &Actor::Person, 3)
            .unwrap();
        a.comment("Looks good", &Actor::Person, 3).unwrap();
        let from_a = a.export_since(&vb).unwrap();
        let from_b = b.export_since(&va).unwrap();
        a.import(&from_b).unwrap();
        b.import(&from_a).unwrap();
        let (va, vb) = (a.view().unwrap(), b.view().unwrap());
        assert_eq!(va, vb);
        // Both stage changes stay in the history, whichever won.
        assert_eq!(va.log.iter().filter(|e| e.kind == "stage").count(), 2);
        assert_eq!(va.text, "Add strict rate limiting");
    }

    #[test]
    fn a_newer_schema_is_read_only() {
        let t = task(Stage::Todo);
        t.meta().insert("schema", SCHEMA + 1).unwrap();
        t.doc.commit();
        let mut t = TaskDoc::from_bytes(&t.snapshot().unwrap()).unwrap();
        assert!(!t.view().unwrap().writable);
        assert_eq!(
            t.comment("x", &Actor::Person, 2).unwrap_err().to_string(),
            "Update Zerus to edit this task"
        );
    }
}
```

- [ ] **Step 3: Register the modules**

Replace `crates/replica/src/lib.rs` with:

```rust
//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
pub mod rank;
mod schema;
mod task;

pub use error::{Error, Result};
pub use schema::{
    Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
};
pub use task::{NewTask, TaskDoc};
```

- [ ] **Step 4: Format, lint and test**

Run: `(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked)`
Expected: `test result: ok. 11 passed`, including `concurrent_edits_merge_the_same_way_everywhere`, `a_large_rewrite_is_replaced_quickly` and `a_newer_schema_is_read_only`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica
git commit -m "Keep each task as a Loro document with the task rules"
```

---

### Task 3: Project settings and the prompts

**Files:**
- Create: `crates/replica/src/project.rs`, `crates/replica/src/render.rs`
- Modify: `crates/replica/src/lib.rs`

**Interfaces:**
- Consumes: `TaskView`, `Actor`, `SessionKey`, `MAX_TEXT`, `SCHEMA` (Task 2).
- Produces: `Settings { task_template, cancel_template, agents_other: bool, agent_limit: i64 }` (serde, `Default`) with `check_agent(&self, &Actor, session: &SessionKey, starting: bool, starts_last_hour: usize) -> Result<()>` (people always pass; an agent outside its own session needs `agents_other`; a start needs fewer than `agent_limit` agent starts in the last hour when the limit is above 0); `ProjectDoc::{new(), from_bytes(&[u8]), import(&[u8]), snapshot(), version(), export_since(&[u8]), settings() -> Settings, set_settings(&Settings) -> Result<()>}`; `Tools { Mcp, Cli }`; `task_prompt(template, &TaskView, project_name, Tools) -> Result<String>`; `cancel_message(template, &TaskView, project_name, Tools) -> Result<String>`.

Placeholders are `{ref}`, `{author}`, `{prompt}`, `{history}` and `{tools}`; `{tools}.task_report` and `{tools}.task_get` become `zerus.task_report` with MCP, or `` `hgs task report orchard/T-xxxxxx` `` without it. Substitution is one pass, so text inside the prompt is never substituted again.

- [ ] **Step 1: Write `project.rs`**

`crates/replica/src/project.rs`:

```rust
//! A project's task settings as one small Loro document.
use crate::schema::{Actor, SessionKey, SCHEMA};
use crate::{Error, Result};
use loro::{ExportMode, LoroDoc, LoroValue, VersionVector};
use serde::{Deserialize, Serialize};

pub const DEFAULT_TASK_TEMPLATE: &str = "[Zerus task {ref}]\nWritten by {author}.\n{prompt}\n{history}\nWhen you finish, or find that it cannot be done, report with\n{tools}.task_report: one to three sentences on what you changed, how you\nchecked it, and what is left undone or why it cannot be done.";
pub const DEFAULT_CANCEL_TEMPLATE: &str = "[Zerus task {ref} cancelled]\nThe person cancelled this task. Stop working on it. Do not continue and do\nnot revert anything; list what you changed and wait for the person.";
const MAX_TEMPLATE: usize = 16 * 1024;

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Settings {
    pub task_template: String,
    pub cancel_template: String,
    /// Agents may start and cancel tasks in other sessions.
    pub agents_other: bool,
    /// Tasks agents may start per hour in the project; 0 is no limit.
    pub agent_limit: i64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            task_template: DEFAULT_TASK_TEMPLATE.into(),
            cancel_template: DEFAULT_CANCEL_TEMPLATE.into(),
            agents_other: true,
            agent_limit: 0,
        }
    }
}

impl Settings {
    /// Refuses an agent's start or cancel that the project's settings forbid.
    /// `starts_last_hour` counts the tasks agents started in the past hour.
    pub fn check_agent(
        &self,
        actor: &Actor,
        session: &SessionKey,
        starting: bool,
        starts_last_hour: usize,
    ) -> Result<()> {
        let Actor::Agent(own) = actor else {
            return Ok(());
        };
        if !self.agents_other && own != session {
            return Err(Error::new(
                "this project lets agents start and cancel tasks only in their own session",
            ));
        }
        if starting && self.agent_limit > 0 && starts_last_hour as i64 >= self.agent_limit {
            return Err(Error::new(format!(
                "this project allows {} tasks started by agents per hour; try again later",
                self.agent_limit
            )));
        }
        Ok(())
    }
}

pub struct ProjectDoc {
    doc: LoroDoc,
}

impl Default for ProjectDoc {
    fn default() -> Self {
        Self::new()
    }
}

impl ProjectDoc {
    pub fn new() -> Self {
        Self {
            doc: LoroDoc::new(),
        }
    }

    pub fn from_bytes(bytes: &[u8]) -> Result<Self> {
        let doc = LoroDoc::new();
        doc.import(bytes)?;
        Ok(Self { doc })
    }

    pub fn import(&mut self, bytes: &[u8]) -> Result<()> {
        self.doc.import(bytes)?;
        Ok(())
    }

    pub fn snapshot(&self) -> Result<Vec<u8>> {
        Ok(self.doc.export(ExportMode::Snapshot)?)
    }

    pub fn version(&self) -> Vec<u8> {
        self.doc.oplog_vv().encode()
    }

    pub fn export_since(&self, version: &[u8]) -> Result<Vec<u8>> {
        Ok(self
            .doc
            .export(ExportMode::updates(&VersionVector::decode(version)?))?)
    }

    pub fn settings(&self) -> Settings {
        let map = self.doc.get_map("settings");
        let mut settings = Settings::default();
        let value = |key: &str| map.get(key).and_then(|v| v.into_value().ok());
        if let Some(LoroValue::String(s)) = value("task_template") {
            settings.task_template = s.to_string();
        }
        if let Some(LoroValue::String(s)) = value("cancel_template") {
            settings.cancel_template = s.to_string();
        }
        if let Some(LoroValue::Bool(b)) = value("agents_other") {
            settings.agents_other = b;
        }
        if let Some(LoroValue::I64(n)) = value("agent_limit") {
            settings.agent_limit = n;
        }
        settings
    }

    pub fn set_settings(&mut self, settings: &Settings) -> Result<()> {
        for (name, template) in [
            ("task prompt", &settings.task_template),
            ("cancel message", &settings.cancel_template),
        ] {
            if !template.starts_with("[Zerus task") {
                return Err(Error::new(format!(
                    "the {name} template must start with its [Zerus task …] line"
                )));
            }
            if template.len() > MAX_TEMPLATE {
                return Err(Error::new(format!(
                    "the {name} template is longer than 16 KiB"
                )));
            }
        }
        if settings.agent_limit < 0 {
            return Err(Error::new("the agent limit cannot be negative"));
        }
        let map = self.doc.get_map("settings");
        map.insert("schema", SCHEMA)?;
        map.insert("task_template", settings.task_template.as_str())?;
        map.insert("cancel_template", settings.cancel_template.as_str())?;
        map.insert("agents_other", settings.agents_other)?;
        map.insert("agent_limit", settings.agent_limit)?;
        self.doc.commit();
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_then_validated_changes() {
        let mut project = ProjectDoc::new();
        assert_eq!(project.settings(), Settings::default());
        let mut changed = Settings {
            agents_other: false,
            agent_limit: 5,
            ..Settings::default()
        };
        project.set_settings(&changed).unwrap();
        let reloaded = ProjectDoc::from_bytes(&project.snapshot().unwrap()).unwrap();
        assert_eq!(reloaded.settings(), changed);
        changed.task_template = "Do {prompt}".into();
        assert!(project.set_settings(&changed).is_err());
        changed.task_template = DEFAULT_TASK_TEMPLATE.into();
        changed.agent_limit = -1;
        assert!(project.set_settings(&changed).is_err());
    }

    #[test]
    fn agent_rules() {
        let own = SessionKey {
            machine: "m".into(),
            session: "codex/api".into(),
        };
        let other = SessionKey {
            machine: "m".into(),
            session: "codex/web".into(),
        };
        let agent = Actor::Agent(own.clone());
        let mut settings = Settings::default();
        assert!(settings.check_agent(&agent, &other, true, 100).is_ok());
        settings.agents_other = false;
        assert!(settings.check_agent(&agent, &other, true, 0).is_err());
        assert!(settings.check_agent(&agent, &own, true, 0).is_ok());
        assert!(settings
            .check_agent(&Actor::Person, &other, true, 0)
            .is_ok());
        settings.agent_limit = 2;
        assert!(settings.check_agent(&agent, &own, true, 1).is_ok());
        assert!(settings.check_agent(&agent, &own, true, 2).is_err());
        assert!(settings.check_agent(&agent, &own, false, 2).is_ok());
    }
}
```

- [ ] **Step 2: Write `render.rs`**

`crates/replica/src/render.rs`:

```rust
//! The prompts Zerus pastes: the task prompt and the cancel message.
use crate::schema::{Actor, TaskView, MAX_TEXT};
use crate::{Error, Result};

/// How the prompt names the task tools: the MCP server, or `hgs task`
/// commands for an agent without it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tools {
    Mcp,
    Cli,
}

fn tool(tools: Tools, name: &str, reference: &str) -> String {
    match tools {
        Tools::Mcp => format!("zerus.{name}"),
        Tools::Cli => format!(
            "`hgs task {} {reference}`",
            name.trim_start_matches("task_")
        ),
    }
}

/// Substitutes `{name}` placeholders once; substituted text is not scanned
/// again, so a prompt that contains `{ref}` stays as written.
fn fill(
    template: &str,
    tools: Tools,
    reference: &str,
    value: impl Fn(&str) -> Option<String>,
) -> String {
    let mut out = String::with_capacity(template.len());
    let mut rest = template;
    while let Some(start) = rest.find('{') {
        out.push_str(&rest[..start]);
        let after = &rest[start + 1..];
        let Some(end) = after.find('}') else {
            out.push_str(&rest[start..]);
            return out;
        };
        let key = &after[..end];
        let tail = &after[end + 1..];
        if key == "tools" {
            if let Some(next) = ["task_report", "task_get"]
                .iter()
                .find(|name| tail.starts_with(&format!(".{name}")))
            {
                out.push_str(&tool(tools, next, reference));
                rest = &tail[next.len() + 1..];
                continue;
            }
            out.push_str(match tools {
                Tools::Mcp => "zerus",
                Tools::Cli => "`hgs task`",
            });
            rest = tail;
            continue;
        }
        match value(key) {
            Some(text) => out.push_str(&text),
            None => out.push_str(&rest[start..start + end + 2]),
        }
        rest = tail;
    }
    out.push_str(rest);
    out
}

fn checked(text: String) -> Result<String> {
    if text.len() > MAX_TEXT {
        return Err(Error::new(
            "the rendered prompt is longer than 64 KiB; shorten the task text",
        ));
    }
    if text.starts_with('/') || text.starts_with('!') {
        return Err(Error::new("a prompt cannot start with / or !"));
    }
    Ok(text)
}

fn author(task: &TaskView) -> String {
    match &task.author {
        Actor::Person => "the person".into(),
        Actor::Agent(key) => format!("the agent of {}", key.session),
    }
}

/// `project` is the project's current name, shown in the reference.
pub fn task_prompt(template: &str, task: &TaskView, project: &str, tools: Tools) -> Result<String> {
    let reference = format!("{project}/{}", task.id);
    let sent_before = task.log.iter().any(|entry| entry.kind == "sent");
    let history = if sent_before {
        format!(
            "This task was sent before; read its history with {} before you start.",
            tool(tools, "task_get", &reference)
        )
    } else {
        String::new()
    };
    checked(fill(template, tools, &reference, |key| match key {
        "ref" => Some(reference.clone()),
        "author" => Some(author(task)),
        "prompt" => Some(task.text.clone()),
        "history" => Some(history.clone()),
        _ => None,
    }))
}

pub fn cancel_message(
    template: &str,
    task: &TaskView,
    project: &str,
    tools: Tools,
) -> Result<String> {
    let reference = format!("{project}/{}", task.id);
    checked(fill(template, tools, &reference, |key| match key {
        "ref" => Some(reference.clone()),
        "author" => Some(author(task)),
        _ => None,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::project::{DEFAULT_CANCEL_TEMPLATE, DEFAULT_TASK_TEMPLATE};
    use crate::schema::{SessionKey, Stage, Target};
    use crate::task::{NewTask, TaskDoc};

    fn view() -> TaskView {
        TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "Fix {ref} handling",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Person,
            rank: "V",
            now: 1,
        })
        .unwrap()
        .view()
        .unwrap()
    }

    #[test]
    fn first_send_prompt() {
        let text = task_prompt(DEFAULT_TASK_TEMPLATE, &view(), "orchard", Tools::Mcp).unwrap();
        assert!(text.starts_with("[Zerus task orchard/T-a1b2c3]\nWritten by the person.\nFix {ref} handling\n\nWhen you finish"), "{text}");
        assert!(text.contains("report with\nzerus.task_report: one to three"));
    }

    #[test]
    fn later_send_points_to_the_history_and_cli_names_commands() {
        let mut doc = TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "Fix it",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Agent(SessionKey {
                machine: "m".into(),
                session: "codex/api".into(),
            }),
            rank: "V",
            now: 1,
        })
        .unwrap();
        doc.record_send(
            &SessionKey {
                machine: "m".into(),
                session: "codex/api".into(),
            },
            "r",
            &Actor::Person,
            2,
        )
        .unwrap();
        let text = task_prompt(
            DEFAULT_TASK_TEMPLATE,
            &doc.view().unwrap(),
            "orchard",
            Tools::Cli,
        )
        .unwrap();
        assert!(text.contains("Written by the agent of codex/api."));
        assert!(text.contains("This task was sent before; read its history with `hgs task get orchard/T-a1b2c3` before you start."));
        assert!(text.contains("report with\n`hgs task report orchard/T-a1b2c3`: one to three"));
        let cancel = cancel_message(
            DEFAULT_CANCEL_TEMPLATE,
            &doc.view().unwrap(),
            "orchard",
            Tools::Cli,
        )
        .unwrap();
        assert!(cancel.starts_with("[Zerus task orchard/T-a1b2c3 cancelled]\nThe person cancelled"));
    }

    #[test]
    fn limits() {
        let mut big = view();
        big.text = "x".repeat(64 * 1024);
        assert!(task_prompt(DEFAULT_TASK_TEMPLATE, &big, "orchard", Tools::Mcp).is_err());
        assert!(task_prompt("/{prompt}", &view(), "orchard", Tools::Mcp).is_err());
        assert_eq!(
            task_prompt("[Zerus task {unknown} {ref", &view(), "o", Tools::Mcp).unwrap(),
            "[Zerus task {unknown} {ref"
        );
    }
}
```

- [ ] **Step 3: Register the modules**

Replace `crates/replica/src/lib.rs` with:

```rust
//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
mod project;
pub mod rank;
mod render;
mod schema;
mod task;

pub use error::{Error, Result};
pub use project::{ProjectDoc, Settings};
pub use render::{cancel_message, task_prompt, Tools};
pub use schema::{
    Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
};
pub use task::{NewTask, TaskDoc};
```

- [ ] **Step 4: Format, lint and test**

Run: `(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked)`
Expected: `test result: ok. 16 passed`, including `agent_rules`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica
git commit -m "Add project task settings and render task prompts"
```

---

### Task 4: Storage

**Files:**
- Create: `crates/replica/src/store.rs`
- Modify: `crates/replica/src/task.rs` (what the store last saw), `crates/replica/src/lib.rs`

**Interfaces:**
- Consumes: `TaskDoc`, `ProjectDoc`, `Stage`, `Actor`, `ids`, `rank`.
- Produces: `ProjectStore::{open(root: &Path, project_id: &str) -> Result<ProjectStore>, project_id(), project() -> Result<ProjectDoc>, save_project(&ProjectDoc), uuids() -> Result<Vec<String>>, load(uuid) -> Result<TaskDoc>, save(&mut TaskDoc) -> Result<()>, tasks() -> Result<(Vec<TaskDoc>, Vec<Damaged>)>, find(reference) -> Result<TaskDoc>, unused_short_id() -> Result<String>, ordered(Stage) -> Result<Vec<TaskDoc>>, rank_for(Stage, &Position, moving: Option<&str>) -> Result<String>, agent_starts_since(i64) -> Result<usize>}`; `Position { Top, End, Index(usize), Before(String), After(String) }`; `Damaged { uuid, error }` (serde).

`open` creates `<root>/<sha256(project ID)>/tasks`, writes `project-id`, and holds an exclusive `flock` on `.lock` until the store is dropped. Each appended block is `[u32 little-endian length][first 4 bytes of the payload's SHA-256][Loro updates]`. `load` stops at the first short, empty or mismatched block, or one Loro cannot apply completely, and marks the task for a fold. `save` folds — writes a fresh snapshot, then removes `.updates` — the first time, after such a stop, when the file is longer than the valid blocks last read or written (a cut append), when appended updates reach 256 KiB, and once when the task becomes Done (its compaction); otherwise it appends one block. Atomic writes sync the file and, on Unix, its directory. Files are 0600 and directories 0700 on Unix. `ordered` sorts by rank, then UUID; `rank_for` leaves the moving task out, and between equal neighbours aims below the first strictly higher rank.

- [ ] **Step 1: Let `TaskDoc` carry what the store last saw**

In `crates/replica/src/task.rs`, replace the struct:
```rust
pub struct TaskDoc {
    uuid: String,
    doc: LoroDoc,
}
```
with:
```rust
pub struct TaskDoc {
    uuid: String,
    doc: LoroDoc,
    /// The version the store last read or wrote, for appending updates.
    pub(crate) saved: Option<VersionVector>,
    /// The length of the valid updates the store read or wrote after the snapshot.
    pub(crate) saved_len: u64,
    /// The stored updates were cut short or could not be applied: rewrite the snapshot.
    pub(crate) needs_fold: bool,
    /// The stage when the store last read or wrote the task.
    pub(crate) saved_stage: Option<Stage>,
}
```
In `create`, replace `let this = Self { uuid, doc };` with:
```rust
        let this = Self {
            uuid,
            doc,
            saved: None,
            saved_len: 0,
            needs_fold: false,
            saved_stage: None,
        };
```
In `from_bytes`, replace `Ok(Self { uuid, doc })` with:
```rust
        Ok(Self {
            uuid,
            doc,
            saved: None,
            saved_len: 0,
            needs_fold: false,
            saved_stage: None,
        })
```
Directly after the `export_since` method, add:
```rust

    pub(crate) fn export_since_vv(&self, version: &VersionVector) -> Result<Vec<u8>> {
        Ok(self.doc.export(ExportMode::updates(version))?)
    }

    pub(crate) fn current_vv(&self) -> VersionVector {
        self.doc.oplog_vv()
    }

    /// Applies stored updates; false when they cannot be applied completely.
    pub(crate) fn import_block(&mut self, bytes: &[u8]) -> bool {
        self.doc
            .import(bytes)
            .is_ok_and(|status| status.pending.is_none())
    }
```

- [ ] **Step 2: Write `store.rs`, tests included**

`crates/replica/src/store.rs`:

```rust
//! Documents on disk, one directory per project:
//! `<root>/<sha256(project ID)>/project.loro` and `tasks/<uuid>.loro`, plus
//! `tasks/<uuid>.updates` with the updates appended since that snapshot. Each
//! appended block carries its length and a checksum; a block cut short or
//! damaged by a crash ends the list, and the next save rewrites the snapshot.
//! Updates are folded into a fresh snapshot when they grow, and once when a
//! task is accepted (its Done compaction). One process at a time holds a project.
use crate::project::ProjectDoc;
use crate::schema::{Actor, Stage};
use crate::task::TaskDoc;
use crate::{ids, rank, Error, Result};
use fs2::FileExt;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Write};
use std::path::{Path, PathBuf};

/// Appended updates beyond this size are folded into the snapshot.
const FOLD_BYTES: u64 = 256 * 1024;
/// A block: little-endian `u32` length, the first four bytes of the SHA-256
/// of the payload, then the payload (Loro updates).
const HEADER: usize = 8;

/// A task file that failed to load. It stays on disk and never blocks the others.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Damaged {
    pub uuid: String,
    pub error: String,
}

/// Where a task goes in the order of its stage.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Position {
    Top,
    End,
    /// Zero-based; beyond the end means the end.
    Index(usize),
    /// Before or after the task with this short ID or UUID.
    Before(String),
    After(String),
}

pub struct ProjectStore {
    dir: PathBuf,
    project: String,
    _lock: File,
}

fn private_options() -> OpenOptions {
    let mut options = OpenOptions::new();
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    options
}

fn private_dir(path: &Path) -> Result<()> {
    fs::create_dir_all(path)?;
    #[cfg(unix)]
    fs::set_permissions(path, std::os::unix::fs::PermissionsExt::from_mode(0o700))?;
    Ok(())
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let temp = path.with_extension(format!("tmp-{}", ids::new_uuid()));
    let mut file = private_options().create_new(true).write(true).open(&temp)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temp, path)?;
    // The rename itself survives a power loss only once the directory is synced.
    #[cfg(unix)]
    if let Some(parent) = path.parent() {
        File::open(parent)?.sync_all()?;
    }
    Ok(())
}

fn checksum(payload: &[u8]) -> [u8; 4] {
    Sha256::digest(payload)[..4].try_into().expect("four bytes")
}

fn block(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER + payload.len());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(&checksum(payload));
    out.extend_from_slice(payload);
    out
}

/// The intact blocks with their end offsets, and whether the whole input was
/// intact. A short, empty or mismatched block ends the list.
fn blocks(bytes: &[u8]) -> (Vec<(&[u8], usize)>, bool) {
    let mut out = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let rest = &bytes[at..];
        if rest.len() < HEADER {
            return (out, false);
        }
        let length = u32::from_le_bytes(rest[..4].try_into().expect("four bytes")) as usize;
        if length == 0 || rest.len() - HEADER < length {
            return (out, false);
        }
        let payload = &rest[HEADER..HEADER + length];
        if checksum(payload) != rest[4..8] {
            return (out, false);
        }
        at += HEADER + length;
        out.push((payload, at));
    }
    (out, true)
}

impl ProjectStore {
    /// Opens the project's directory below `root` and waits for its lock.
    pub fn open(root: &Path, project: &str) -> Result<Self> {
        if project.is_empty() || project.len() > 128 || project.chars().any(char::is_control) {
            return Err(Error::new("invalid project ID"));
        }
        private_dir(root)?;
        let dir = root.join(format!("{:x}", Sha256::digest(project.as_bytes())));
        private_dir(&dir)?;
        private_dir(&dir.join("tasks"))?;
        let lock = private_options()
            .create(true)
            .truncate(false)
            .write(true)
            .open(dir.join(".lock"))?;
        lock.lock_exclusive()?;
        let name = dir.join("project-id");
        if !name.exists() {
            write_atomic(&name, project.as_bytes())?;
        }
        Ok(Self {
            dir,
            project: project.into(),
            _lock: lock,
        })
    }

    pub fn project_id(&self) -> &str {
        &self.project
    }

    pub fn project(&self) -> Result<ProjectDoc> {
        match fs::read(self.dir.join("project.loro")) {
            Ok(bytes) => ProjectDoc::from_bytes(&bytes),
            Err(error) if error.kind() == ErrorKind::NotFound => Ok(ProjectDoc::new()),
            Err(error) => Err(error.into()),
        }
    }

    pub fn save_project(&self, doc: &ProjectDoc) -> Result<()> {
        write_atomic(&self.dir.join("project.loro"), &doc.snapshot()?)
    }

    fn snapshot_path(&self, uuid: &str) -> PathBuf {
        self.dir.join("tasks").join(format!("{uuid}.loro"))
    }

    fn updates_path(&self, uuid: &str) -> PathBuf {
        self.dir.join("tasks").join(format!("{uuid}.updates"))
    }

    pub fn uuids(&self) -> Result<Vec<String>> {
        let mut uuids = BTreeSet::new();
        for entry in fs::read_dir(self.dir.join("tasks"))? {
            let path = entry?.path();
            let extension = path.extension().and_then(|e| e.to_str());
            if matches!(extension, Some("loro" | "updates")) {
                if let Some(stem) = path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .filter(|s| ids::is_uuid(s))
                {
                    uuids.insert(stem.to_owned());
                }
            }
        }
        Ok(uuids.into_iter().collect())
    }

    pub fn load(&self, uuid: &str) -> Result<TaskDoc> {
        let mut task = TaskDoc::from_bytes(&fs::read(self.snapshot_path(uuid))?)?;
        if task.uuid() != uuid {
            return Err(Error::new("the task file does not match its name"));
        }
        match fs::read(self.updates_path(uuid)) {
            Ok(bytes) => {
                let (found, intact) = blocks(&bytes);
                task.needs_fold = !intact;
                for (payload, end) in found {
                    if !task.import_block(payload) {
                        task.needs_fold = true;
                        break;
                    }
                    task.saved_len = end as u64;
                }
            }
            Err(error) if error.kind() == ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        task.saved = Some(task.current_vv());
        task.saved_stage = Some(task.stage()?);
        Ok(task)
    }

    pub fn save(&self, task: &mut TaskDoc) -> Result<()> {
        let snapshot = self.snapshot_path(task.uuid());
        let updates = self.updates_path(task.uuid());
        let current = task.current_vv();
        let stage = task.stage()?;
        let on_disk = match fs::metadata(&updates) {
            Ok(meta) => meta.len(),
            Err(error) if error.kind() == ErrorKind::NotFound => 0,
            Err(error) => return Err(error.into()),
        };
        let fold = task.saved.is_none()
            || !snapshot.exists()
            || task.needs_fold
            // Bytes past the valid blocks: a cut append, or another writer.
            || on_disk != task.saved_len
            || on_disk >= FOLD_BYTES
            || (stage == Stage::Done && task.saved_stage != Some(Stage::Done));
        if !fold && task.saved.as_ref() == Some(&current) {
            return Ok(());
        }
        if fold {
            // The snapshot first: a crash before the removal leaves updates
            // the snapshot already contains, and importing them again is harmless.
            write_atomic(&snapshot, &task.snapshot()?)?;
            match fs::remove_file(&updates) {
                Ok(()) => {}
                Err(error) if error.kind() == ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
            task.saved_len = 0;
            task.needs_fold = false;
        } else {
            let saved = task.saved.clone().expect("checked above");
            let bytes = block(&task.export_since_vv(&saved)?);
            let mut file = private_options().create(true).append(true).open(&updates)?;
            file.write_all(&bytes)?;
            file.sync_all()?;
            task.saved_len += bytes.len() as u64;
        }
        task.saved = Some(current);
        task.saved_stage = Some(stage);
        Ok(())
    }

    /// Every task that loads, and the ones that do not.
    pub fn tasks(&self) -> Result<(Vec<TaskDoc>, Vec<Damaged>)> {
        let mut tasks = Vec::new();
        let mut damaged = Vec::new();
        for uuid in self.uuids()? {
            match self.load(&uuid) {
                Ok(task) => tasks.push(task),
                Err(error) => damaged.push(Damaged {
                    uuid,
                    error: error.to_string(),
                }),
            }
        }
        Ok((tasks, damaged))
    }

    /// A task by its UUID or its short ID.
    pub fn find(&self, reference: &str) -> Result<TaskDoc> {
        if ids::is_uuid(reference) {
            return self.load(reference);
        }
        if !ids::is_short_id(reference) {
            return Err(Error::new(format!("{reference:?} is not a task ID")));
        }
        let (tasks, _) = self.tasks()?;
        let mut found: Vec<_> = tasks.into_iter().filter(|t| t.id() == reference).collect();
        match found.len() {
            0 => Err(Error::new(format!("no task {reference} in this project"))),
            1 => Ok(found.pop().expect("one")),
            _ => Err(Error::new(format!(
                "{reference} names more than one task; use its UUID"
            ))),
        }
    }

    /// A short ID no local task uses yet.
    pub fn unused_short_id(&self) -> Result<String> {
        let (tasks, _) = self.tasks()?;
        let used: BTreeSet<String> = tasks.iter().map(TaskDoc::id).collect();
        loop {
            let id = ids::short_id();
            if !used.contains(&id) {
                return Ok(id);
            }
        }
    }

    /// The tasks of `stage` in their order: rank, then UUID for equal ranks.
    pub fn ordered(&self, stage: Stage) -> Result<Vec<TaskDoc>> {
        let (tasks, _) = self.tasks()?;
        let mut list: Vec<TaskDoc> = tasks
            .into_iter()
            .filter(|t| t.stage().ok() == Some(stage))
            .collect();
        list.sort_by(|a, b| (a.rank(), a.uuid()).cmp(&(b.rank(), b.uuid())));
        Ok(list)
    }

    /// The rank that puts a task at `position` in `stage`, leaving out the
    /// task being moved (its UUID).
    pub fn rank_for(
        &self,
        stage: Stage,
        position: &Position,
        moving: Option<&str>,
    ) -> Result<String> {
        let list: Vec<TaskDoc> = self
            .ordered(stage)?
            .into_iter()
            .filter(|t| Some(t.uuid()) != moving)
            .collect();
        let at = |reference: &str| {
            list.iter()
                .position(|t| t.id() == reference || t.uuid() == reference)
                .ok_or_else(|| Error::new(format!("{reference} is not in {}", stage.label())))
        };
        let index = match position {
            Position::Top => 0,
            Position::End => list.len(),
            Position::Index(index) => (*index).min(list.len()),
            Position::Before(reference) => at(reference)?,
            Position::After(reference) => at(reference)? + 1,
        };
        let low = index.checked_sub(1).map(|i| list[i].rank());
        // Equal ranks can meet after concurrent moves: aim below the first higher one.
        let high = list[index..]
            .iter()
            .map(TaskDoc::rank)
            .find(|r| low.as_ref().is_none_or(|l| r > l));
        rank::between(low.as_deref(), high.as_deref())
    }

    /// How many tasks agents started since `since` (Unix milliseconds).
    pub fn agent_starts_since(&self, since: i64) -> Result<usize> {
        let (tasks, _) = self.tasks()?;
        let mut count = 0;
        for task in tasks {
            count += task
                .view()?
                .log
                .iter()
                .filter(|e| e.kind == "sent" && e.at >= since && matches!(e.actor, Actor::Agent(_)))
                .count();
        }
        Ok(count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{SessionKey, Target};
    use crate::task::NewTask;

    fn new_task(store: &ProjectStore, rank: &str) -> TaskDoc {
        TaskDoc::create(NewTask {
            project: store.project_id(),
            id: &store.unused_short_id().unwrap(),
            text: "Add rate limiting",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Person,
            rank,
            now: 1,
        })
        .unwrap()
    }

    fn api() -> SessionKey {
        SessionKey {
            machine: "m".into(),
            session: "codex/api".into(),
        }
    }

    fn comments(task: &TaskDoc) -> Vec<String> {
        task.view()
            .unwrap()
            .log
            .into_iter()
            .filter(|e| e.kind == "comment")
            .map(|e| e.text)
            .collect()
    }

    fn append(store: &ProjectStore, task: &TaskDoc, bytes: &[u8]) {
        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(store.updates_path(task.uuid()))
            .unwrap();
        file.write_all(bytes).unwrap();
    }

    #[test]
    fn saves_append_updates_and_reload() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        task.comment("first", &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        task.comment("second", &Actor::Person, 3).unwrap();
        store.save(&mut task).unwrap();
        let updates = fs::read(store.updates_path(task.uuid())).unwrap();
        let (found, intact) = blocks(&updates);
        assert_eq!((found.len(), intact), (2, true));
        let loaded = store.find(&task.id()).unwrap();
        assert_eq!(loaded.view().unwrap(), task.view().unwrap());
        assert_eq!(
            store.find(task.uuid()).unwrap().view().unwrap(),
            task.view().unwrap()
        );
    }

    #[test]
    fn a_cut_tail_is_repaired_and_later_saves_survive() {
        for junk in [&[200, 0, 0, 0, 1, 2][..], &[0; 64][..]] {
            let root = tempfile::tempdir().unwrap();
            let store = ProjectStore::open(root.path(), "p1").unwrap();
            let mut task = new_task(&store, "V");
            store.save(&mut task).unwrap();
            task.comment("first", &Actor::Person, 2).unwrap();
            store.save(&mut task).unwrap();
            append(&store, &task, junk);
            // A later process: the cut tail is skipped, and the next save rewrites the snapshot.
            let mut again = store.load(task.uuid()).unwrap();
            assert!(again.needs_fold);
            again.comment("after the crash", &Actor::Person, 3).unwrap();
            store.save(&mut again).unwrap();
            assert!(!store.updates_path(task.uuid()).exists());
            again.comment("then appended", &Actor::Person, 4).unwrap();
            store.save(&mut again).unwrap();
            let last = store.load(task.uuid()).unwrap();
            assert_eq!(
                comments(&last),
                ["first", "after the crash", "then appended"]
            );
            assert!(!last.needs_fold);
        }
    }

    #[test]
    fn a_damaged_block_ends_the_updates() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        task.comment("first", &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        let path = store.updates_path(task.uuid());
        let mut bytes = fs::read(&path).unwrap();
        let last = bytes.len() - 1;
        bytes[last] ^= 0xff;
        fs::write(&path, bytes).unwrap();
        let loaded = store.load(task.uuid()).unwrap();
        assert!(loaded.needs_fold);
        assert!(comments(&loaded).is_empty());
    }

    #[test]
    fn done_compacts_once() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        task.record_send(&api(), "r", &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        task.record_report("Done.", &Actor::Person, 3).unwrap();
        task.set_stage(Stage::Done, &Actor::Person, 4).unwrap();
        store.save(&mut task).unwrap();
        assert!(!store.updates_path(task.uuid()).exists());
        // Comments on a Done task are appended again.
        task.comment("thanks", &Actor::Person, 5).unwrap();
        store.save(&mut task).unwrap();
        assert!(store.updates_path(task.uuid()).exists());
        assert_eq!(
            store.load(task.uuid()).unwrap().view().unwrap(),
            task.view().unwrap()
        );
    }

    #[test]
    fn a_damaged_file_is_set_aside_and_projects_are_separate() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        let bad = ids::new_uuid();
        fs::write(store.snapshot_path(&bad), b"not loro").unwrap();
        let (tasks, damaged) = store.tasks().unwrap();
        assert_eq!(tasks.len(), 1);
        assert_eq!(damaged.len(), 1);
        assert_eq!(damaged[0].uuid, bad);
        assert!(store.snapshot_path(&bad).exists());
        let other = ProjectStore::open(root.path(), "p2").unwrap();
        assert!(other.tasks().unwrap().0.is_empty());
        assert!(store.find("T-zzzzzz").is_err());
        assert!(store.find("nonsense").is_err());
    }

    #[test]
    fn ranks_place_tasks_even_between_equal_ranks() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        assert_eq!(
            store.rank_for(Stage::Todo, &Position::End, None).unwrap(),
            "V"
        );
        for rank in ["V", "V", "X"] {
            let mut task = new_task(&store, rank);
            store.save(&mut task).unwrap();
        }
        let list = store.ordered(Stage::Todo).unwrap();
        let last = list[2].id();
        let between = store
            .rank_for(Stage::Todo, &Position::Index(1), None)
            .unwrap();
        assert!(
            "V" < between.as_str() && between.as_str() < "X",
            "{between}"
        );
        let before = store
            .rank_for(Stage::Todo, &Position::Before(last.clone()), None)
            .unwrap();
        assert!("V" < before.as_str() && before.as_str() < "X", "{before}");
        assert!(
            store
                .rank_for(Stage::Todo, &Position::Top, None)
                .unwrap()
                .as_str()
                < "V"
        );
        assert!(
            store
                .rank_for(Stage::Todo, &Position::End, None)
                .unwrap()
                .as_str()
                > "X"
        );
        let after_last = store
            .rank_for(
                Stage::Todo,
                &Position::After(last.clone()),
                Some(list[2].uuid()),
            )
            .is_err();
        assert!(after_last, "the moving task is not its own anchor");
        assert!(store
            .rank_for(Stage::Backlog, &Position::Before(last), None)
            .is_err());
    }

    #[test]
    fn agent_starts_are_counted() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut by_agent = new_task(&store, "V");
        by_agent
            .record_send(&api(), "r1", &Actor::Agent(api()), 1_000)
            .unwrap();
        store.save(&mut by_agent).unwrap();
        let mut by_person = new_task(&store, "W");
        by_person
            .record_send(&api(), "r2", &Actor::Person, 1_000)
            .unwrap();
        store.save(&mut by_person).unwrap();
        assert_eq!(store.agent_starts_since(500).unwrap(), 1);
        assert_eq!(store.agent_starts_since(2_000).unwrap(), 0);
    }

    #[test]
    fn the_project_lock_is_exclusive() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let other = File::open(store.dir.join(".lock")).unwrap();
        assert!(other.try_lock_exclusive().is_err());
        drop(store);
        assert!(other.try_lock_exclusive().is_ok());
    }
}
```

- [ ] **Step 3: Register the module**

Replace `crates/replica/src/lib.rs` with:

```rust
//! Replicated Zerus documents on Loro. This crate is the only code in Zerus
//! that imports `loro`: hgs and the mobile apps use its typed operations, so
//! a Loro upgrade or a schema change happens here once.
mod error;
pub mod ids;
mod project;
pub mod rank;
mod render;
mod schema;
mod store;
mod task;

pub use error::{Error, Result};
pub use project::{ProjectDoc, Settings};
pub use render::{cancel_message, task_prompt, Tools};
pub use schema::{
    Actor, LogEntry, SessionKey, Stage, Target, TaskView, MAX_DOCUMENT, MAX_TEXT, SCHEMA,
};
pub use store::{Damaged, Position, ProjectStore};
pub use task::{NewTask, TaskDoc};
```

- [ ] **Step 4: Format, lint and test**

Run: `(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked)`
Expected: `test result: ok. 24 passed`, including `a_cut_tail_is_repaired_and_later_saves_survive`, `a_damaged_block_ends_the_updates`, `done_compacts_once`, `ranks_place_tasks_even_between_equal_ranks`, `agent_starts_are_counted` and `the_project_lock_is_exclusive`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica
git commit -m "Store task documents as snapshots plus checksummed updates"
```

---

### Task 5: Golden vectors

**Files:**
- Create: `crates/replica/tests/vectors.rs`
- Create (generated once): in `crates/replica/tests/vectors/`, `task-schema1-loro1.16.loro`, `task-schema1-loro1.16.version`, `task-schema1-loro1.16-a.update`, `task-schema1-loro1.16-b.update`, `task-schema1-loro1.16-merged.json`, `project-schema1-loro1.16.loro`, `project-schema1-loro1.16.json`

**Interfaces:**
- Consumes: the public API of Tasks 1–4.

The vectors are a task snapshot, its version vector, two updates made at the same time on two replicas of it (a send and report on one, a comment on the other), their merged view, and a project snapshot with its settings. Every later build must read them, merge the two updates in either order to the same view, and carry both to a fresh copy through `export_since`; a Loro upgrade that cannot fails CI. `ZERUS_BLESS=1` writes them; use it only to add a vector for a new schema or Loro version, never to make a failing build pass.

- [ ] **Step 1: Write the test**

`crates/replica/tests/vectors.rs`:

```rust
//! Golden vectors: documents and updates written by earlier versions of this
//! crate and of Loro. Every build must still read and merge them. Running with
//! `ZERUS_BLESS=1` writes them anew; do that only to add a vector for a new
//! schema or Loro version, never to make a failing build pass.
use std::fs;
use std::path::PathBuf;
use zerus_replica::{
    Actor, NewTask, ProjectDoc, SessionKey, Settings, Stage, Target, TaskDoc, TaskView,
};

const TASK: &str = "task-schema1-loro1.16";

fn path(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name)
}

fn read(name: &str) -> Vec<u8> {
    fs::read(path(name)).unwrap_or_else(|e| panic!("{name}: {e}"))
}

fn blessing() -> bool {
    std::env::var("ZERUS_BLESS").as_deref() == Ok("1")
}

fn api() -> SessionKey {
    SessionKey {
        machine: "00000000-0000-4000-8000-000000000001".into(),
        session: "codex/api".into(),
    }
}

/// The base document, then two replicas that change it at the same time.
fn bless_task() {
    let mut base = TaskDoc::create(NewTask {
        project: "project-1",
        id: "T-a1b2c3",
        text: "Add rate limiting to POST /v1/orders.",
        stage: Stage::Todo,
        target: Target::Session(api()),
        author: Actor::Agent(api()),
        rank: "V",
        now: 1_760_000_000_000,
    })
    .unwrap();
    base.comment("Use the config value.", &Actor::Person, 1_760_000_001_000)
        .unwrap();
    let snapshot = base.snapshot().unwrap();
    let version = base.version();
    let mut a = TaskDoc::from_bytes(&snapshot).unwrap();
    a.record_send(
        &api(),
        "00000000-0000-4000-8000-000000000002",
        &Actor::Person,
        1_760_000_002_000,
    )
    .unwrap();
    a.record_report(
        "Token bucket in Redis; 6 tests.",
        &Actor::Agent(api()),
        1_760_000_003_000,
    )
    .unwrap();
    let mut b = TaskDoc::from_bytes(&snapshot).unwrap();
    b.comment("Limit per API key.", &Actor::Person, 1_760_000_002_500)
        .unwrap();
    let (update_a, update_b) = (
        a.export_since(&version).unwrap(),
        b.export_since(&version).unwrap(),
    );
    a.import(&update_b).unwrap();
    for (name, bytes) in [
        (format!("{TASK}.loro"), snapshot),
        (format!("{TASK}.version"), version),
        (format!("{TASK}-a.update"), update_a),
        (format!("{TASK}-b.update"), update_b),
    ] {
        fs::write(path(&name), bytes).unwrap();
    }
    let merged = serde_json::to_string_pretty(&a.view().unwrap()).unwrap() + "\n";
    fs::write(path(&format!("{TASK}-merged.json")), merged).unwrap();
}

fn merged(first: &str, second: &str) -> TaskView {
    let mut task = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    task.import(&read(&format!("{TASK}-{first}.update")))
        .unwrap();
    task.import(&read(&format!("{TASK}-{second}.update")))
        .unwrap();
    task.view().unwrap()
}

#[test]
fn task_schema1_loro_1_16() {
    if blessing() {
        bless_task();
    }
    let expected: serde_json::Value =
        serde_json::from_slice(&read(&format!("{TASK}-merged.json"))).unwrap();
    // Concurrent updates merge to the same task in either order.
    for (first, second) in [("a", "b"), ("b", "a")] {
        let view = merged(first, second);
        assert_eq!(serde_json::to_value(&view).unwrap(), expected);
    }
    // An export since the stored version carries both updates to a fresh copy.
    let mut both = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    both.import(&read(&format!("{TASK}-a.update"))).unwrap();
    both.import(&read(&format!("{TASK}-b.update"))).unwrap();
    let delta = both
        .export_since(&read(&format!("{TASK}.version")))
        .unwrap();
    let mut fresh = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
    fresh.import(&delta).unwrap();
    assert_eq!(
        serde_json::to_value(fresh.view().unwrap()).unwrap(),
        expected
    );
    // The stored document still accepts this build's edits.
    both.set_stage(Stage::Done, &Actor::Person, 1_760_000_004_000)
        .unwrap();
    assert_eq!(both.view().unwrap().stage, Stage::Done);
}

#[test]
fn project_schema1_loro_1_16() {
    let (bytes, expected) = (
        "project-schema1-loro1.16.loro",
        "project-schema1-loro1.16.json",
    );
    if blessing() {
        let mut project = ProjectDoc::new();
        project
            .set_settings(&Settings {
                agents_other: false,
                agent_limit: 3,
                ..Settings::default()
            })
            .unwrap();
        fs::write(path(bytes), project.snapshot().unwrap()).unwrap();
        let settings = serde_json::to_string_pretty(&project.settings()).unwrap() + "\n";
        fs::write(path(expected), settings).unwrap();
    }
    let project = ProjectDoc::from_bytes(&read(bytes)).unwrap();
    let expected: Settings = serde_json::from_slice(&read(expected)).unwrap();
    assert_eq!(project.settings(), expected);
}
```

- [ ] **Step 2: Run it before the vectors exist**

Run: `(cd crates/replica && cargo +1.99.1 test --locked --test vectors)`
Expected: FAIL — both tests panic reading a missing file in `tests/vectors`.

- [ ] **Step 3: Write the vectors once**

Run: `(cd crates/replica && mkdir -p tests/vectors && ZERUS_BLESS=1 cargo +1.99.1 test --locked --test vectors)`
Expected: `test result: ok. 2 passed`; the seven files above in `crates/replica/tests/vectors`. Check that `task-schema1-loro1.16-merged.json` shows `"stage": "review"`, `"summary": "Token bucket in Redis; 6 tests."` and a log of `created`, `comment` ("Use the config value."), `comment` ("Limit per API key."), `sent`, `report`.

- [ ] **Step 4: Run the whole crate without blessing**

Run: `(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -- -D warnings && cargo +1.99.1 test --locked)`
Expected: `24 passed` and `2 passed`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/tests
git commit -m "Pin replica documents with golden vectors"
```

---

### Task 6: `hgs task` on one computer, without sending

**Files:**
- Modify: `Cargo.toml`, `Cargo.lock`, `src/main.rs`, `src/cli.rs`, `src/swarm/mod.rs`
- Create: `src/tasks.rs`, `tests/test_tasks.py`

**Interfaces:**
- Consumes: `zerus_replica` (Tasks 1–5); `crate::swarm::local_node_id(&Config) -> anyhow::Result<String>`; `crate::cli::{Error, Result}`; `Config::state` (the hgs state directory, `HGS_STATE_DIR`).
- Produces: `crate::swarm::projects(&Config) -> anyhow::Result<Vec<(String, String)>>`; `crate::tasks::dispatch(&Config, &[String], bool) -> cli::Result<i32>` with `list`, `get`, `add`, `edit`, `move`, `stage`, `target`, `comment`, `report`, `settings`. Send and cancel come in Task 7.

`HGS_SESSION` together with `HGS_RUN_ID` makes the caller an agent: its `add` is pinned to its own session, and it is the author of reports and comments. `add` and `edit` refuse a prompt the task prompt could not carry, rendered with the project's current template. Ordering goes through `ProjectStore::rank_for`; a task that changes list (Backlog ⇄ To do, or back to To do from In review or Done) goes to the top of its new list, as the task spec says; new tasks go to the end unless `--position` is given. Stdin is read and catalog lookups are done before the project store is opened.

- [ ] **Step 1: Write the CLI tests**

`tests/test_tasks.py`:

```python
#!/usr/bin/env python3
"""`hgs task` through the real CLI: an isolated catalog and state; sends go to
a scripted agent on a private tmux socket."""
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

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
TMUX = shutil.which("tmux")

# Records every byte the pane receives; never echoes, so the composer stays empty.
AGENT = r'''
import os, pathlib, sys, termios, tty
root = pathlib.Path(os.environ['TASK_FIXTURE'])
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, b'\x1b[?2004h\x1b[2J\x1b[H' + '› '.encode())
(root / 'agent-pid').write_text(str(os.getpid()))
while True:
    value = os.read(fd, 65536)
    if not value: break
    with (root / 'received').open('ab') as file: file.write(value)
'''


class Fixture(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-tasks-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bin = self.root / ".local/bin"
        self.bin.mkdir(parents=True)
        self.state = self.root / "state"
        self.state.mkdir()
        self.socket = self.root / "socket"
        self.env = dict(os.environ, HOME=str(self.root), TASK_FIXTURE=str(self.root),
                        HGS_CONFIG_DIR=str(self.root / "config"), HGS_STATE_DIR=str(self.state),
                        HGS_SELF="test", HGS_TAB="0", PATH=str(self.bin) + ":" + os.environ["PATH"])
        for variable in ("TMUX", "TMUX_PANE", "HGS_RUN_ID", "HGS_SESSION", "HGS_EXECUTABLE", "HGS_SSH", "HGS_PEERS"):
            self.env.pop(variable, None)
        # hgs puts $HOME/.local/bin first on PATH: the shim there keeps tmux private.
        # Each test stops its own private server, never another one.
        if TMUX:
            self.addCleanup(lambda: subprocess.run([TMUX, "-S", str(self.socket), "kill-server"], capture_output=True))
            self.script("tmux", '#!/bin/sh\nexec ' + TMUX + ' -S "' + str(self.socket) + '" -f /dev/null "$@"\n')
        else:
            self.script("tmux", "#!/bin/sh\nexit 1\n")
        catalog = {"projects": [{"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []},
                                {"id": "project-2", "name": "team/web", "color": "#654321", "folders": [], "sessions": []}]}
        result = subprocess.run([str(HGS), "swarm", "initialize"], input=json.dumps(catalog), env=self.env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def script(self, name, value):
        path = self.bin / name
        path.write_text(value)
        path.chmod(0o755)

    def hgs(self, *args, text=None, ok=True, session=None):
        env = dict(self.env)
        if session:
            # An agent, as hooks know it: its session and its run.
            env.update(HGS_SESSION=session, HGS_RUN_ID="run-test")
        result = subprocess.run([str(HGS), "task", *args], input=text, env=env, text=True,
                                capture_output=True, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr

    def add(self, text, *args, session=None):
        return self.hgs("add", "--project", "orchard", *args, text=text, session=session)["task"]

    def ids(self, stage):
        listing = self.hgs("list", "--project", "orchard", "--stage", stage)
        return [task["id"] for task in listing["tasks"]]


class TaskCommands(Fixture):
    def test_add_list_and_get(self):
        task = self.add("Add rate limiting")
        self.assertRegex(task["id"], r"^T-[0-9a-z]{6}$")
        self.assertEqual((task["stage"], task["target"], task["author"]), ("todo", {"kind": "any"}, {"kind": "person"}))
        self.assertEqual(task["ref"], "orchard/" + task["id"])
        backlog = self.add("Later", "--backlog")
        self.assertEqual(self.ids("todo"), [task["id"]])
        self.assertEqual(self.ids("backlog"), [backlog["id"]])
        got = self.hgs("get", task["ref"])["task"]
        self.assertEqual(got["text"], "Add rate limiting")
        self.assertEqual([entry["kind"] for entry in got["log"]], ["created"])
        self.assertEqual(self.hgs("get", task["id"], "--project", "project-1")["task"]["uuid"], got["uuid"])
        self.assertIn("no project", self.hgs("list", "--project", "missing", ok=False))
        self.assertIn("no task", self.hgs("get", "orchard/T-zzzzzz", ok=False))

    def test_order_and_positions(self):
        first, second, third = (self.add(t)["id"] for t in ("one", "two", "three"))
        self.assertEqual(self.ids("todo"), [first, second, third])
        self.hgs("move", "orchard/" + third, "--top")
        self.assertEqual(self.ids("todo"), [third, first, second])
        self.hgs("move", "orchard/" + third, "--after", first)
        self.assertEqual(self.ids("todo"), [first, third, second])
        fourth = self.add("four", "--position", "1")["id"]
        self.assertEqual(self.ids("todo"), [fourth, first, third, second])

    def test_stages_edits_targets_and_comments(self):
        ref = self.add("Draft")["ref"]
        self.hgs("edit", ref, text="Final text")
        self.hgs("stage", ref, "backlog")
        self.hgs("stage", ref, "todo")
        self.assertIn("cannot go from To do to Done", self.hgs("stage", ref, "done", ok=False))
        self.hgs("target", ref, "codex/api")
        task = self.hgs("comment", ref, text="Use Redis")["task"]
        self.assertEqual(task["text"], "Final text")
        self.assertEqual(task["target"]["session"], "codex/api")
        self.assertEqual([e["kind"] for e in task["log"]], ["created", "edited", "stage", "stage", "target", "comment"])
        self.assertIn("longer than 64 KiB", self.hgs("comment", ref, text="x" * (64 * 1024 + 1), ok=False))

    def test_an_agent_files_tasks_for_its_own_session(self):
        task = self.add("Document the setup", session="codex/api")
        self.assertEqual(task["author"]["kind"], "agent")
        self.assertEqual(task["target"]["session"], "codex/api")
        self.assertIn("pinned to its own session", self.hgs("add", "--project", "orchard", "--target", "any", text="x", session="codex/api", ok=False))

    def test_settings(self):
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        self.assertTrue(settings["task_template"].startswith("[Zerus task {ref}]"))
        settings.update(agents_other=False, agent_limit=4)
        self.assertEqual(self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings))["settings"]["agent_limit"], 4)
        settings["task_template"] = "{prompt}"
        self.assertIn("must start with", self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings), ok=False))

    def test_concurrent_adds_all_land(self):
        processes = [subprocess.Popen([str(HGS), "task", "add", "--project", "orchard"], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, text=True)
                     for _ in range(6)]
        # Every process has its text before any of them finishes: all six race for the project.
        for index, process in enumerate(processes):
            process.stdin.write("task %d" % index)
            process.stdin.close()
        for process in processes:
            self.assertEqual(process.wait(timeout=30), 0, process.stderr.read())
        tasks = self.hgs("list", "--project", "orchard")["tasks"]
        self.assertEqual(sorted(t["text"] for t in tasks), ["task %d" % i for i in range(6)])
        self.assertEqual(len({t["id"] for t in tasks}), 6)
        self.assertEqual(len({t["rank"] for t in tasks}), 6)

    def test_texts_that_cannot_be_sent_are_refused(self):
        self.assertIn("control characters", self.hgs("add", "--project", "orchard", text="\x1b[31mred", ok=False))
        self.assertIn("control characters", self.hgs("add", "--project", "orchard", text="line one\r\n", ok=False))
        # Fits 64 KiB alone, but not inside the task prompt.
        self.assertIn("rendered prompt", self.hgs("add", "--project", "orchard", text="x" * 65450, ok=False))
        ref = self.add("Short")["ref"]
        self.assertIn("rendered prompt", self.hgs("edit", ref, text="x" * 65450, ok=False))
        self.assertEqual(self.hgs("get", ref)["task"]["text"], "Short")
        self.assertEqual(self.ids("todo"), [ref.split("/")[-1]])

    def test_a_task_that_changes_list_goes_to_its_top(self):
        first, second = self.add("one")["id"], self.add("two")["id"]
        later, other = self.add("later", "--backlog")["id"], self.add("other", "--backlog")["id"]
        self.hgs("stage", "orchard/" + later, "todo")
        self.assertEqual((self.ids("todo"), self.ids("backlog")), ([later, first, second], [other]))
        self.hgs("stage", "orchard/" + second, "backlog")
        self.assertEqual((self.ids("todo"), self.ids("backlog")), ([later, first], [second, other]))

    def test_a_project_name_with_a_slash(self):
        task = self.hgs("add", "--project", "team/web", text="Ship it")["task"]
        self.assertEqual(task["ref"], "team/web/" + task["id"])
        self.assertEqual(self.hgs("get", task["ref"])["task"]["uuid"], task["uuid"])

    def test_a_damaged_task_is_reported_not_fatal(self):
        self.add("Fine")
        project = next(p for p in (self.state / "replica").iterdir() if p.is_dir())
        (project / "tasks" / (str(uuid.uuid4()) + ".loro")).write_bytes(b"broken")
        listing = self.hgs("list", "--project", "orchard")
        self.assertEqual(len(listing["tasks"]), 1)
        self.assertEqual(len(listing["damaged"]), 1)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo +1.99.1 build && python3 -m unittest tests.test_tasks -v`
Expected: FAIL — `hgs task` is not a command yet. An unknown command is treated as an agent launch; the fixture's tmux shim keeps that on the test's private socket, and each test stops that server with `-S` when it ends.

- [ ] **Step 3: Depend on the crate**

In `Cargo.toml`, after `uuid = { version = "1", features = ["v4"] }` in `[dependencies]`, add:
```toml
zerus-replica = { path = "crates/replica" }
```
Run: `cargo +1.99.1 build` (without `--locked`, once) to add the crate and Loro to `Cargo.lock`.

- [ ] **Step 4: List catalog projects**

In `src/swarm/mod.rs`, directly above `/// Persist and return the machine identity without retaining the catalog lock.`, add:
```rust
/// Live catalog projects as (ID, name), in the shared order.
pub(crate) fn projects(config: &Config) -> Result<Vec<(String, String)>> {
    let snapshot = LockedStore::load(config)?.snapshot(config);
    Ok(snapshot["organization"]["projects"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|p| Some((p["id"].as_str()?.to_owned(), p["name"].as_str().unwrap_or_default().to_owned())))
        .collect())
}
```

- [ ] **Step 5: Write `src/tasks.rs`**

`src/tasks.rs`:

```rust
//! `hgs task`: the project task tracker on this computer. Tasks are Loro
//! documents kept by `zerus-replica` below `<state dir>/replica`; every
//! command prints JSON. See docs/superpowers/specs/2026-10-13-project-task-queue-design.md
//! and docs/superpowers/specs/2026-10-18-task-replication-loro-design.md.
//!
//! Lock order: catalog lookups (project, machine identity) happen before the
//! project store is opened, and stdin is read before it too.
use crate::cli::{Error, Result};
use crate::config::Config;
use serde_json::{json, Value};
use std::io::Read;
use std::time::{SystemTime, UNIX_EPOCH};
use zerus_replica::{
    task_prompt, Actor, NewTask, Position, ProjectStore, SessionKey, Settings, Stage, Target,
    TaskDoc, Tools, MAX_TEXT,
};

const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any | send REF [--session SESSION] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
  REF is T-xxxxxx with --project P, or PROJECT/T-xxxxxx; texts and --set settings are read from stdin";

fn fail(message: impl std::fmt::Display) -> Error {
    Error::new(1, message.to_string())
}

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

fn stdin_text(what: &str) -> Result<String> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_TEXT as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() > MAX_TEXT {
        return Err(fail(format!("the {what} is longer than 64 KiB")));
    }
    String::from_utf8(bytes).map_err(|_| fail(format!("the {what} is not UTF-8")))
}

fn option<'a>(args: &'a [String], name: &str) -> Result<Option<&'a str>> {
    match args.iter().position(|a| a == name) {
        Some(i) => args
            .get(i + 1)
            .map(|v| Some(v.as_str()))
            .ok_or_else(|| fail(format!("{name} needs a value"))),
        None => Ok(None),
    }
}

fn flag(args: &[String], name: &str) -> bool {
    args.iter().any(|a| a == name)
}

/// The project's catalog ID and current name, by ID or name.
fn project(config: &Config, wanted: &str) -> Result<(String, String)> {
    let projects = crate::swarm::projects(config).map_err(fail)?;
    if let Some(found) = projects.iter().find(|(id, _)| id == wanted) {
        return Ok(found.clone());
    }
    let named: Vec<_> = projects.iter().filter(|(_, name)| name == wanted).collect();
    match named.as_slice() {
        [one] => Ok((*one).clone()),
        [] => Err(fail(format!("no project {wanted:?} in the catalog"))),
        _ => Err(fail(format!(
            "{wanted:?} names more than one project; use its ID"
        ))),
    }
}

/// A task reference: `PROJECT/T-xxxxxx`, or `T-xxxxxx` (or a UUID) with `--project`.
struct Reference {
    project: (String, String),
    task: String,
}

fn reference(config: &Config, args: &[String]) -> Result<Reference> {
    let value = args.first().ok_or_else(|| fail(USAGE))?;
    let (project_name, task) = match value.rsplit_once('/') {
        Some((project, task)) => (Some(project), task),
        None => (option(args, "--project")?, value.as_str()),
    };
    let project_name =
        project_name.ok_or_else(|| fail("name the project: PROJECT/T-xxxxxx or --project P"))?;
    Ok(Reference {
        project: project(config, project_name)?,
        task: task.to_owned(),
    })
}

fn store(config: &Config, project_id: &str) -> Result<ProjectStore> {
    ProjectStore::open(&config.state.join("replica"), project_id).map_err(fail)
}

fn machine(config: &Config) -> Result<String> {
    crate::swarm::local_node_id(config).map_err(fail)
}

/// The agent of the session that runs this command, as hooks know it (both
/// `HGS_SESSION` and `HGS_RUN_ID`), or the person.
fn actor(here: &str) -> Actor {
    let var = |name| std::env::var(name).ok().filter(|v| !v.is_empty());
    match (var("HGS_SESSION"), var("HGS_RUN_ID")) {
        (Some(session), Some(_)) => Actor::Agent(SessionKey {
            machine: here.to_owned(),
            session,
        }),
        _ => Actor::Person,
    }
}

fn view(task: &TaskDoc, project_name: &str, with_log: bool) -> Result<Value> {
    let mut value = serde_json::to_value(task.view().map_err(fail)?).map_err(fail)?;
    value["ref"] = json!(format!("{project_name}/{}", task.id()));
    if !with_log {
        value.as_object_mut().expect("object").remove("log");
    }
    Ok(value)
}

fn print(value: Value) -> Result<i32> {
    println!("{value}");
    Ok(0)
}

/// Refuses a prompt that the task prompt could not carry when it is sent.
fn check_prompt(store: &ProjectStore, task: &TaskDoc, project_name: &str) -> Result<()> {
    let settings = store.project().map_err(fail)?.settings();
    let view = task.view().map_err(fail)?;
    task_prompt(&settings.task_template, &view, project_name, Tools::Cli)
        .map(|_| ())
        .map_err(fail)
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    if dry {
        return Err(fail("task commands do not support --dry-run"));
    }
    let Some((action, args)) = args.split_first() else {
        return Err(fail(USAGE));
    };
    match action.as_str() {
        "list" => list(config, args),
        "get" => {
            let r = reference(config, args)?;
            let task = store(config, &r.project.0)?
                .find(&r.task)
                .map_err(fail)?;
            print(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true)?}))
        }
        "add" => add(config, args),
        "edit" | "move" | "stage" | "target" | "comment" | "report" => {
            change(config, action, args)
        }
        "settings" => settings(config, args),
        _ => Err(fail(USAGE)),
    }
}

fn list(config: &Config, args: &[String]) -> Result<i32> {
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let only = option(args, "--stage")?
        .map(Stage::parse)
        .transpose()
        .map_err(fail)?;
    let store = store(config, &id)?;
    let (_, damaged) = store.tasks().map_err(fail)?;
    let mut out = Vec::new();
    for stage in [
        Stage::Review,
        Stage::Active,
        Stage::Todo,
        Stage::Backlog,
        Stage::Done,
    ] {
        if only.is_some_and(|s| s != stage) {
            continue;
        }
        for task in store.ordered(stage).map_err(fail)? {
            out.push(view(&task, &name, false)?);
        }
    }
    print(json!({"project": {"id": id, "name": name}, "tasks": out, "damaged": damaged}))
}

fn add(config: &Config, args: &[String]) -> Result<i32> {
    let text = stdin_text("prompt")?;
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let here = machine(config)?;
    let author = actor(&here);
    let stage = if flag(args, "--backlog") {
        Stage::Backlog
    } else {
        Stage::Todo
    };
    // A task an agent files is always pinned to that agent's session.
    let target = match (&author, option(args, "--target")?) {
        (Actor::Agent(_), Some(_)) => {
            return Err(fail(
                "a task an agent files is pinned to its own session; --target is for people",
            ))
        }
        (Actor::Agent(own), None) => Target::Session(own.clone()),
        (Actor::Person, None | Some("any")) => Target::Any,
        (Actor::Person, Some(session)) => Target::Session(SessionKey {
            machine: here.clone(),
            session: session.to_owned(),
        }),
    };
    let position = match option(args, "--position")? {
        Some(p) => Position::Index(
            p.parse::<usize>()
                .ok()
                .filter(|p| *p >= 1)
                .ok_or_else(|| fail("--position is a number from 1"))?
                - 1,
        ),
        None => Position::End,
    };
    let store = store(config, &id)?;
    let rank = store.rank_for(stage, &position, None).map_err(fail)?;
    let short = store.unused_short_id().map_err(fail)?;
    let mut task = TaskDoc::create(NewTask {
        project: &id,
        id: &short,
        text: &text,
        stage,
        target,
        author,
        rank: &rank,
        now: now(),
    })
    .map_err(fail)?;
    check_prompt(&store, &task, &name)?;
    store.save(&mut task).map_err(fail)?;
    print(json!({"task": view(&task, &name, true)?}))
}

fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
    let text = match action {
        "edit" => Some(stdin_text("prompt")?),
        "comment" => Some(stdin_text("comment")?),
        "report" => Some(stdin_text("report")?),
        _ => None,
    };
    let r = reference(config, args)?;
    let (project_id, project_name) = (&r.project.0, &r.project.1);
    let here = machine(config)?;
    let who = actor(&here);
    let store = store(config, project_id)?;
    let mut task = store.find(&r.task).map_err(fail)?;
    let text = text.unwrap_or_default();
    let mut extra = json!({});
    match action {
        "edit" => {
            task.edit_text(&text, &who, now()).map_err(fail)?;
            check_prompt(&store, &task, project_name)?;
        }
        "comment" => task.comment(&text, &who, now()).map_err(fail)?,
        "report" => {
            let moved = task.record_report(&text, &who, now()).map_err(fail)?;
            extra = json!({"moved_to_review": moved});
        }
        "move" => {
            let position = if flag(args, "--top") {
                Position::Top
            } else if let Some(anchor) = option(args, "--before")? {
                Position::Before(anchor.rsplit('/').next().unwrap_or(anchor).to_owned())
            } else if let Some(anchor) = option(args, "--after")? {
                Position::After(anchor.rsplit('/').next().unwrap_or(anchor).to_owned())
            } else {
                return Err(fail(USAGE));
            };
            let stage = task.stage().map_err(fail)?;
            let rank = store
                .rank_for(stage, &position, Some(task.uuid()))
                .map_err(fail)?;
            task.set_rank(&rank).map_err(fail)?;
        }
        "stage" => {
            let to = Stage::parse(args.get(1).ok_or_else(|| fail(USAGE))?).map_err(fail)?;
            let from = task.stage().map_err(fail)?;
            task.set_stage(to, &who, now()).map_err(fail)?;
            // A task that changes list goes to the top of its new list.
            if from != to && matches!(to, Stage::Todo | Stage::Backlog) {
                let rank = store
                    .rank_for(to, &Position::Top, Some(task.uuid()))
                    .map_err(fail)?;
                task.set_rank(&rank).map_err(fail)?;
            }
        }
        "target" => {
            let value = args.get(1).ok_or_else(|| fail(USAGE))?;
            let target = if value == "any" {
                Target::Any
            } else {
                Target::Session(SessionKey {
                    machine: here.clone(),
                    session: value.clone(),
                })
            };
            task.set_target(&target, &who, now()).map_err(fail)?;
        }
        _ => return Err(fail(USAGE)),
    }
    store.save(&mut task).map_err(fail)?;
    let mut out = json!({"task": view(&task, project_name, true)?});
    for (key, value) in extra.as_object().expect("object") {
        out[key] = value.clone();
    }
    print(out)
}

fn settings(config: &Config, args: &[String]) -> Result<i32> {
    let wanted = if flag(args, "--set") {
        Some(
            serde_json::from_str::<Settings>(&stdin_text("settings")?)
                .map_err(|e| fail(format!("invalid settings: {e}")))?,
        )
    } else {
        None
    };
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let store = store(config, &id)?;
    let mut doc = store.project().map_err(fail)?;
    if let Some(wanted) = wanted {
        doc.set_settings(&wanted).map_err(fail)?;
        store.save_project(&doc).map_err(fail)?;
    }
    print(json!({"project": {"id": id, "name": name}, "settings": doc.settings()}))
}
```

- [ ] **Step 6: Wire the command**

In `src/main.rs`, after `mod swarm;`, add `mod tasks;`.

In `src/cli.rs`, in the final `match command.as_str()` of `dispatch`, after `"project" => project(&config, args, dry),` add:
```rust
        "task" => crate::tasks::dispatch(&config, args, dry),
```
In the `USAGE` text, after the line `       hgs project ls [--json] | add <name> <dir> | set <name> <dir> | rm <name>`, add:
```text
       hgs task list|get|add|edit|move|stage|target|send|cancel|report|comment|settings …   project tasks (JSON)
```

- [ ] **Step 7: Run the tests**

Run:
```bash
cargo +1.99.1 build
cargo +1.99.1 clippy --all-targets -- -D warnings
python3 -m unittest tests.test_tasks -v
```
Expected: no lint output; `Ran 10 tests … OK`, including `test_concurrent_adds_all_land`, `test_texts_that_cannot_be_sent_are_refused`, `test_a_task_that_changes_list_goes_to_its_top` and `test_a_project_name_with_a_slash`.

- [ ] **Step 8: Run the existing suites**

Run: `cargo +1.99.1 test --locked && python3 -m unittest tests.test_swarm tests.test_input`
Expected: all pass; nothing else changed behaviour.

- [ ] **Step 9: Record the lock order for phase 2**

The phase-2 swarm worker will open project stores after the catalog; it must keep this order. Beads lives in the main checkout (`.beads/embeddeddolt` is not part of a worktree), so run there:
```bash
bd remember --key replica-lock-order "hgs task: resolve catalog project, machine identity and stdin before opening a replica ProjectStore; take the hgs state lock only while holding the project lock. Never take the catalog lock while holding a project lock."
```
Expected: the memory is stored; `bd recall replica-lock-order` prints it.

- [ ] **Step 10: Commit**

```bash
git add Cargo.toml Cargo.lock src/main.rs src/cli.rs src/swarm/mod.rs src/tasks.rs tests/test_tasks.py
git commit -m "Add hgs task commands over the replica store"
```

---

### Task 7: Send, report and cancel

**Files:**
- Modify: `src/state/mod.rs`, `src/state/input.rs`, `src/tasks.rs`, `tests/test_tasks.py`

**Interfaces:**
- Consumes: `input::submit(name, json bytes, None) -> Result<Value, String>` (the `hgs send` path); `read(name)` and `string(&Value, key)` inside `state`; `task_prompt`, `cancel_message`, `Tools::Cli`.
- Produces: `crate::state::submit_task_text(name: &str, text: &str) -> Result<Value, String>`; `hgs task send REF [--session S]` and `hgs task cancel REF [--reason TEXT]`.

Send pastes into a session on this computer only: the pinned session, `--session`, or an agent's own session for an Any task. A person's `--session` must match the pin (or the task is Any, and stays Any); an agent's `--session` also pins the task to that session. Send refuses a task this copy shows In progress, applies the project's agent rules (`check_agent` with `agent_starts_since` over the last hour) to agents, and records the send only after `input::submit` accepted the paste; a refused paste leaves the task unchanged. Cancel applies the same agent rule, records the cancellation (its reason as a comment, never pasted), then pastes the cancel message without stopping the turn; a refused message is noted in the history and the task stays cancelled. An agent cannot cancel the task it was sent.

- [ ] **Step 1: Write the delivery tests**

Append to `tests/test_tasks.py`, before `if __name__ == "__main__":` (the class stops its own tmux server and waits for the pane processes to exit before the fixture home is removed):

```python
@unittest.skipUnless(TMUX, "tmux is required for delivery")
class TaskDelivery(Fixture):
    def setUp(self):
        super().setUp()
        (self.root / "agent.py").write_text(AGENT)
        self.name = "codex/task-test"
        self.run_id = str(uuid.uuid4())
        self.tmux("new-session", "-d", "-s", self.name, "-x", "100", "-y", "30", "python3", str(self.root / "agent.py"))
        self.addCleanup(self.stop)
        for _ in range(200):
            pid_file = self.root / "agent-pid"
            if pid_file.exists() and pid_file.read_text().strip():
                break
            time.sleep(.02)
        pid = int((self.root / "agent-pid").read_text())
        start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="],
                                        env=dict(self.env, LC_ALL="C", TZ="UTC"), text=True).strip()
        pane = self.tmux("display-message", "-p", "-t", "=" + self.name + ":", "#{pane_id}").strip()
        self.tmux("set-option", "-t", "=" + self.name + ":", "@hgs_run", self.run_id)
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest() + ".json")
        self.record_path.write_text(json.dumps(dict(
            version=1, name=self.name, agent="codex", run_id=self.run_id, conversation_id=str(uuid.uuid4()),
            pane=pane, pid=pid, process_start=start, activity="idle", phase="idle", active_tools={},
            subagents={}, last_event_at=time.time(), created=time.time())))

    def stop(self):
        # kill-server acknowledges before its panes exit; wait for them before the
        # fixture home is removed, so no pane writes into a deleted directory.
        panes = subprocess.run([str(self.bin / "tmux"), "list-panes", "-a", "-F", "#{pane_pid}"], env=self.env,
                               capture_output=True, text=True, timeout=5).stdout.split()
        pids = {pid for pid in panes if pid.isdigit()}
        subprocess.run([TMUX, "-S", str(self.socket), "kill-server"], capture_output=True)
        deadline = time.monotonic() + 2
        while pids and time.monotonic() < deadline:
            status = subprocess.run(["ps", "-p", ",".join(sorted(pids)), "-o", "pid=,stat="],
                                    capture_output=True, text=True, timeout=5)
            pids = {parts[0] for line in status.stdout.splitlines()
                    if len(parts := line.split()) >= 2 and not parts[1].startswith(("Z", "X"))}
            if pids:
                time.sleep(.02)
        self.assertFalse(pids, "owned fixture panes survived tmux shutdown")

    def tmux(self, *args):
        result = subprocess.run([str(self.bin / "tmux"), *args], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def received(self):
        path = self.root / "received"
        return path.read_bytes().decode(errors="replace") if path.exists() else ""

    def wait_received(self, text):
        for _ in range(250):
            if text in self.received():
                return
            time.sleep(.02)
        self.fail("the agent never received " + repr(text) + "; got " + repr(self.received()))

    def confirm(self):
        # The agent's next hook event confirms the pasted input.
        record = json.loads(self.record_path.read_text())
        record["last_event_at"] = record.get("input_pending_at", time.time()) + 1
        self.record_path.write_text(json.dumps(record))

    def settings(self, **changes):
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        settings.update(changes)
        self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings))

    def test_send_report_and_a_second_send(self):
        task = self.add("Add rate limiting", "--target", self.name)
        sent = self.hgs("send", task["ref"])
        self.assertEqual(sent["task"]["stage"], "active")
        self.assertEqual(sent["task"]["session"]["session"], self.name)
        self.assertEqual(sent["receipt"]["status"], "submitted")
        self.wait_received("[Zerus task orchard/" + task["id"] + "]")
        self.wait_received("Add rate limiting")
        self.wait_received("`hgs task report orchard/" + task["id"] + "`")
        self.assertIn("already In progress in " + self.name, self.hgs("send", task["ref"], ok=False))
        done = self.hgs("report", task["ref"], text="Token bucket; 6 tests.", session=self.name)
        self.assertTrue(done["moved_to_review"])
        self.assertEqual((done["task"]["stage"], done["task"]["summary"]), ("review", "Token bucket; 6 tests."))
        again = self.hgs("report", task["ref"], text="Also updated the README.", session=self.name)
        self.assertFalse(again["moved_to_review"])

    def test_an_any_session_task_needs_a_session(self):
        task = self.add("Anywhere")
        self.assertIn("--session", self.hgs("send", task["ref"], ok=False))
        sent = self.hgs("send", task["ref"], "--session", self.name)["task"]
        self.assertEqual((sent["stage"], sent["target"]), ("active", {"kind": "any"}))
        self.wait_received("Anywhere")

    def test_session_rules(self):
        pinned = self.add("Pinned", "--target", self.name)
        self.assertIn("pinned to " + self.name, self.hgs("send", pinned["ref"], "--session", "codex/other", ok=False))
        anywhere = self.add("Anywhere")
        # An agent that names the session also pins the task there.
        sent = self.hgs("send", anywhere["ref"], "--session", self.name, session="codex/planner")["task"]
        self.assertEqual(sent["target"]["session"], self.name)

    def test_a_refused_send_changes_nothing(self):
        task = self.add("Nowhere", "--target", "codex/missing")
        self.assertIn("codex/missing", self.hgs("send", task["ref"], ok=False))
        after = self.hgs("get", task["ref"])["task"]
        self.assertEqual((after["stage"], [e["kind"] for e in after["log"]]), ("todo", ["created"]))

    def test_project_settings_bound_agents(self):
        self.settings(agents_other=False)
        other = self.add("Elsewhere", "--target", "codex/other")
        self.assertIn("only in their own session", self.hgs("send", other["ref"], session=self.name, ok=False))
        self.assertEqual(self.received(), "")
        own = self.add("Mine")
        self.hgs("send", own["ref"], session=self.name)
        self.wait_received("Mine")
        self.confirm()
        self.settings(agents_other=True, agent_limit=1)
        another = self.add("One more")
        self.assertIn("1 tasks started by agents per hour", self.hgs("send", another["ref"], session=self.name, ok=False))
        # People are not limited.
        self.assertEqual(self.hgs("send", another["ref"], "--session", self.name)["task"]["stage"], "active")

    def test_cancel_moves_to_review_and_tells_the_agent(self):
        task = self.add("Refactor the parser", "--target", self.name)
        self.hgs("send", task["ref"])
        self.confirm()
        self.assertIn("a task you were sent", self.hgs("cancel", task["ref"], session=self.name, ok=False))
        out = self.hgs("cancel", task["ref"], "--reason", "Wrong approach")
        self.assertEqual((out["task"]["stage"], out["task"]["cancel_reason"]), ("review", "Wrong approach"))
        self.assertEqual([(e["kind"], e.get("text", "")) for e in out["task"]["log"][-2:]],
                         [("cancelled", ""), ("comment", "Wrong approach")])
        self.assertTrue(out["cancel_message"]["sent"])
        self.wait_received("[Zerus task orchard/" + task["id"] + " cancelled]")
        self.assertNotIn("Wrong approach", self.received())

    def test_a_refused_cancel_message_still_cancels(self):
        task = self.add("Refactor the parser", "--target", self.name)
        self.hgs("send", task["ref"])
        # No hook event since the paste: hgs refuses further input.
        out = self.hgs("cancel", task["ref"])
        self.assertEqual(out["task"]["stage"], "review")
        self.assertFalse(out["cancel_message"]["sent"])
        self.assertIn("not been confirmed", out["cancel_message"]["error"])
        self.assertEqual(out["task"]["log"][-1]["kind"], "note")
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo +1.99.1 build && python3 -m unittest tests.test_tasks.TaskDelivery -v`
Expected: FAIL — `send` and `cancel` print the usage error.

- [ ] **Step 3: Paste task text through the `hgs send` path**

In `src/state/input.rs`, directly above `pub(super) fn submit(name: &str, input: &[u8], recovery_id: Option<&str>) -> Result<Value> {`, add:
```rust
/// Plain text for the session's current run and conversation (task prompts).
pub(super) fn submit_text(name: &str, text: &str) -> Result<Value> {
    let record = read(name)?;
    let request = json!({"request_id": uuid::Uuid::new_v4().to_string(), "text": text,
        "expected_run_id": string(&record, "run_id"),
        "expected_conversation_id": string(&record, "conversation_id"), "attachments": []});
    submit(name, request.to_string().as_bytes(), None)
}
```
In `src/state/mod.rs`, directly above `pub fn dispatch(args: &[String]) -> Result<i32> {`, add:
```rust
/// Pastes a task prompt or cancel message into `name` for its current run and
/// conversation, the way `hgs send` does, and returns the durable receipt.
pub(crate) fn submit_task_text(name: &str, text: &str) -> Result<Value> {
    local_sessions()?;
    input::submit_text(name, text)
}
```

- [ ] **Step 4: Add send and cancel to `src/tasks.rs`**

Replace the `use zerus_replica::{…};` block with:
```rust
use zerus_replica::{
    cancel_message, task_prompt, Actor, NewTask, Position, ProjectStore, SessionKey, Settings,
    Stage, Target, TaskDoc, Tools, MAX_TEXT,
};
```
In the module comment, replace the line `//! project store is opened, and stdin is read before it too.` with:
```rust
//! project store is opened, and stdin is read before it too; the hgs state lock
//! (taken when pasting) is only ever taken while holding the project lock.
```
In `dispatch`, replace `"edit" | "move" | "stage" | "target" | "comment" | "report" => {` with:
```rust
        "edit" | "move" | "stage" | "target" | "comment" | "report" | "send" | "cancel" => {
```
In `change`, after the `"target" => { … }` arm, add:
```rust
        "send" => extra = send(&here, &store, &mut task, project_name, args, &who)?,
        "cancel" => extra = cancel(&here, &store, &mut task, project_name, args, &who)?,
```
Directly above `fn settings(config: &Config, args: &[String]) -> Result<i32> {`, add:

```rust
/// Pastes the task prompt into its session on this computer.
fn send(
    here: &str,
    store: &ProjectStore,
    task: &mut TaskDoc,
    project_name: &str,
    args: &[String],
    who: &Actor,
) -> Result<Value> {
    match task.stage().map_err(fail)? {
        Stage::Backlog | Stage::Todo => {}
        Stage::Active => return Err(fail(task.already_running())),
        stage => {
            return Err(fail(format!(
                "a task in {} is not sent; move it back to To do first",
                stage.label()
            )))
        }
    }
    let view = task.view().map_err(fail)?;
    let local = |session: &str| SessionKey {
        machine: here.to_owned(),
        session: session.to_owned(),
    };
    // The session: --session (an agent's choice also pins the task), the
    // pinned session, or for an Any task the agent's own session.
    let (session, pin) = match (option(args, "--session")?, &view.target, who) {
        (Some(name), Target::Session(pinned), Actor::Person) if *pinned != local(name) => {
            return Err(fail(format!(
                "{} is pinned to {}; change its session first",
                task.id(),
                pinned.session
            )))
        }
        (Some(name), _, Actor::Person) => (local(name), false),
        (Some(name), _, Actor::Agent(_)) => (local(name), true),
        (None, Target::Session(pinned), _) if pinned.machine == here => (pinned.clone(), false),
        (None, Target::Session(pinned), _) => {
            return Err(fail(format!(
                "{} is for {} on another computer; sending there comes with replication",
                task.id(),
                pinned.session
            )))
        }
        (None, Target::Any, Actor::Agent(own)) => (own.clone(), false),
        (None, Target::Any, Actor::Person) => {
            return Err(fail("choose the session with --session"))
        }
    };
    let settings = store.project().map_err(fail)?.settings();
    let starts = store
        .agent_starts_since(now() - 3_600_000)
        .map_err(fail)?;
    settings
        .check_agent(who, &session, true, starts)
        .map_err(fail)?;
    if pin && view.target != Target::Session(session.clone()) {
        task.set_target(&Target::Session(session.clone()), who, now())
            .map_err(fail)?;
    }
    let prompt = task_prompt(
        &settings.task_template,
        &task.view().map_err(fail)?,
        project_name,
        Tools::Cli,
    )
    .map_err(fail)?;
    let receipt = crate::state::submit_task_text(&session.session, &prompt).map_err(fail)?;
    let request_id = receipt["request_id"].as_str().unwrap_or_default().to_owned();
    task.record_send(&session, &request_id, who, now())
        .map_err(|e| {
            fail(format!(
                "the prompt was sent, but the task could not record it: {e}"
            ))
        })?;
    Ok(json!({"receipt": receipt}))
}

/// Moves the task to In review and pastes the cancel message; the turn is not stopped.
fn cancel(
    here: &str,
    store: &ProjectStore,
    task: &mut TaskDoc,
    project_name: &str,
    args: &[String],
    who: &Actor,
) -> Result<Value> {
    let session = task
        .view()
        .map_err(fail)?
        .session
        .ok_or_else(|| fail("only a task In progress can be cancelled"))?;
    if let Actor::Agent(own) = who {
        if *own == session {
            return Err(fail(format!(
                "You cannot cancel {project_name}/{}, a task you were sent. If it cannot be done, report what is left undone.",
                task.id()
            )));
        }
    }
    let settings = store.project().map_err(fail)?.settings();
    settings
        .check_agent(who, &session, false, 0)
        .map_err(fail)?;
    let reason = option(args, "--reason")?.unwrap_or_default();
    task.record_cancel(reason, who, now()).map_err(fail)?;
    store.save(task).map_err(fail)?;
    let message = cancel_message(
        &settings.cancel_template,
        &task.view().map_err(fail)?,
        project_name,
        Tools::Cli,
    )
    .map_err(fail)?;
    let pasted = if session.machine == here {
        crate::state::submit_task_text(&session.session, &message).map_err(|e| e.to_string())
    } else {
        Err(format!("{} is on another computer", session.session))
    };
    Ok(match pasted {
        Ok(receipt) => json!({"cancel_message": {"sent": true, "receipt": receipt}}),
        Err(error) => {
            task.note(&format!("Cancel message not sent: {error}"), who, now())
                .map_err(fail)?;
            json!({"cancel_message": {"sent": false, "error": error}})
        }
    })
}
```

- [ ] **Step 5: Run the tests**

Run:
```bash
cargo +1.99.1 build
cargo +1.99.1 clippy --all-targets -- -D warnings
python3 -m unittest tests.test_tasks -v
```
Expected: no lint output; `Ran 17 tests … OK`, including `test_session_rules`, `test_project_settings_bound_agents`, `test_a_refused_send_changes_nothing` and `test_a_refused_cancel_message_still_cancels`.

- [ ] **Step 6: Run the input suite**

Run: `python3 -m unittest tests.test_input`
Expected: OK; `hgs send` behaves as before.

- [ ] **Step 7: Commit**

```bash
git add src/state/mod.rs src/state/input.rs src/tasks.rs tests/test_tasks.py
git commit -m "Send, report and cancel tasks through the hgs input path"
```

---

### Task 8: License notices and Mac installs

**Files:**
- Modify: `scripts/collect-licenses.py`, `THIRD_PARTY_NOTICES.md`, `mac-install.sh`, `tests/test_mac_install.py`
- Create: `packaging/rust-licenses/README.md`, and `packaging/rust-licenses/<crate>/LICENSE` for `append-only-bytes`, `arref`, `generic-btree`, `serde_columnar`, `serde_columnar_derive`

Loro brings crates whose crates.io packages carry no license file, and three (`im`, `bitmaps`, `sized-chunks`, all MPL-2.0) whose notice is named `LICENCE.md`. `scripts/collect-licenses.py`, which the AUR, macOS and Windows packaging run, aborts on both. `mac-install.sh` sends a fixed list of sources to the Mac, which must now include the crate.

- [ ] **Step 1: See the license collection fail**

Run: `python3 scripts/collect-licenses.py "$(mktemp -d)"`
Expected: FAIL — `No license text found for append-only-bytes 0.1.12; review before distribution.`

- [ ] **Step 2: Accept `LICENCE` and reviewed fallbacks**

In `scripts/collect-licenses.py`, replace
```python
import argparse
import json
from pathlib import Path
import shutil
import subprocess
```
with
```python
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# Reviewed notice texts for crates that publish none; see the README there.
FALLBACK = ROOT / "packaging/rust-licenses"
```
and replace
```python
        notices = [path for path in source.rglob("*") if path.is_file()
                   and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "UNLICENSE"))]
        if package.get("license_file"):
            notices.append(source / package["license_file"])
        notices = sorted(set(notices))
        if not notices:
```
with
```python
        notices = [path for path in source.rglob("*") if path.is_file()
                   and path.name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "UNLICENSE"))]
        if package.get("license_file"):
            notices.append(source / package["license_file"])
        notices = sorted(set(notices))
        if not notices and (FALLBACK / package["name"]).is_dir():
            source = FALLBACK / package["name"]
            notices = sorted(path for path in source.iterdir() if path.is_file())
        if not notices:
```

- [ ] **Step 3: Add the upstream notices, pinned to a commit and a checksum**

Run:
```bash
fetch() {  # crate, GitHub repository, commit, SHA-256 of the file
  mkdir -p "packaging/rust-licenses/$1"
  curl -fsSL "https://raw.githubusercontent.com/$2/$3/LICENSE" -o "packaging/rust-licenses/$1/LICENSE"
  echo "$4  packaging/rust-licenses/$1/LICENSE" | sha256sum --check
}
fetch append-only-bytes zxch3n/append-only-bytes 5ee04bd3e3249c6bfeb9c3d31644ada87bb90cbe a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf
fetch arref zxch3n/arref c09551ed17509719e8f9a46d2814c4378f8786fa a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf
fetch generic-btree loro-dev/generic-btree 0ae02fad39fbaf6b4836e1e578e3da7759e321ed 732d8393fd664a8126a46f0b7a4c23c577fdfc9658e9ad1620d5354050fb9f51
fetch serde_columnar loro-dev/columnar 06663cfc569c6c770b28688f015f8a87fc04e156 a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b
fetch serde_columnar_derive loro-dev/columnar 06663cfc569c6c770b28688f015f8a87fc04e156 a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b
```
Expected: five `OK` lines; each file is an MIT license text with a copyright line.

`packaging/rust-licenses/README.md`:

```markdown
# Rust license notices for crates that publish none

`scripts/collect-licenses.py` takes a crate's notices from its crates.io
package. These crates ship no license file there, so their notices come from
the upstream repository instead, copied unchanged at the commit below. Before
upgrading one of them, compare the new upstream file and update the copy, the
URL and the checksum.

| Crate | Source | SHA-256 |
|---|---|---|
| `append-only-bytes` | https://github.com/zxch3n/append-only-bytes/blob/5ee04bd3e3249c6bfeb9c3d31644ada87bb90cbe/LICENSE | `a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf` |
| `arref` | https://github.com/zxch3n/arref/blob/c09551ed17509719e8f9a46d2814c4378f8786fa/LICENSE | `a9ec532f4f11817f427e4f3ca0432b176138721e0866f152864b17ebdeac6bbf` |
| `generic-btree` | https://github.com/loro-dev/generic-btree/blob/0ae02fad39fbaf6b4836e1e578e3da7759e321ed/LICENSE | `732d8393fd664a8126a46f0b7a4c23c577fdfc9658e9ad1620d5354050fb9f51` |
| `serde_columnar` | https://github.com/loro-dev/columnar/blob/06663cfc569c6c770b28688f015f8a87fc04e156/LICENSE | `a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b` |
| `serde_columnar_derive` | https://github.com/loro-dev/columnar/blob/06663cfc569c6c770b28688f015f8a87fc04e156/LICENSE | `a9af0e3a3ded007f9914a1125b736ec526d469105c6577e367e2a9ca4de39d6b` |

`serde_columnar` and `serde_columnar_derive` are offered under "MIT OR
Apache-2.0"; Zerus uses them under MIT, the license their repository carries.
```

- [ ] **Step 4: Record the MPL-2.0 dependencies**

In `THIRD_PARTY_NOTICES.md`, replace
```markdown
Rust dependencies and versions are locked in `Cargo.lock` and, for the mobile
connector that Linux packages also ship, `services/mobile/connector/Cargo.lock`.
```
with
```markdown
Rust dependencies and versions are locked in `Cargo.lock` and, for the mobile
connector that Linux packages also ship, `services/mobile/connector/Cargo.lock`.
hgs links Loro (MIT) through `crates/replica`; Loro in turn links `im`,
`bitmaps` and `sized-chunks` under MPL-2.0, unmodified from crates.io, where
their source code is available. Packages carry their MPL-2.0 license texts.
Crates that publish no license file get the upstream notices kept in
`packaging/rust-licenses`.
```

- [ ] **Step 5: Carry the crate in Mac installs**

In `tests/test_mac_install.py`, in `test_mobile_connector_sources_travel_and_its_build_stays_opt_in`, replace
```python
                             'services/mobile/relay/Cargo.toml', 'services/mobile/relay/src'):
```
with
```python
                             'services/mobile/relay/Cargo.toml', 'services/mobile/relay/src',
                             'crates/replica/Cargo.toml', 'crates/replica/Cargo.lock',
                             'crates/replica/.cargo', 'crates/replica/src'):
```
Run: `python3 -m unittest tests.test_mac_install`
Expected: FAIL — `crates/replica/Cargo.toml` is not among the transferred paths.

In `mac-install.sh`, replace
```bash
# The mobile connector and the relay library it links are named file by file,
# so no Linux target/ of theirs travels either.
sources=(Cargo.toml Cargo.lock build.rs .cargo src scripts install.sh hgs hgs_state.py tests README.md LICENSE THIRD_PARTY_NOTICES.md docs mac-install.sh tmux.conf .gitignore
  services/mobile/connector/Cargo.toml services/mobile/connector/Cargo.lock services/mobile/connector/.cargo
  services/mobile/connector/src services/mobile/relay/Cargo.toml services/mobile/relay/src)
```
with
```bash
# The mobile connector, the relay library it links and the replica crate are
# named file by file, so no Linux target/ of theirs travels either.
sources=(Cargo.toml Cargo.lock build.rs .cargo src scripts install.sh hgs hgs_state.py tests README.md LICENSE THIRD_PARTY_NOTICES.md docs mac-install.sh tmux.conf .gitignore
  services/mobile/connector/Cargo.toml services/mobile/connector/Cargo.lock services/mobile/connector/.cargo
  services/mobile/connector/src services/mobile/relay/Cargo.toml services/mobile/relay/src
  crates/replica/Cargo.toml crates/replica/Cargo.lock crates/replica/.cargo crates/replica/src)
```
and, in the `copy="…"` list that publishes the checkout's entry points, add `crates` after `src`, since hgs builds against it:
```bash
copy="Cargo.toml Cargo.lock build.rs .cargo src crates scripts install.sh tests README.md LICENSE THIRD_PARTY_NOTICES.md docs mac-install.sh tmux.conf .gitignore"
```

- [ ] **Step 6: Verify**

Run:
```bash
python3 scripts/collect-licenses.py "$(mktemp -d)"
python3 -m unittest tests.test_mac_install
python3 scripts/check-source-privacy.py
```
Expected: `Collected notices for … host-target dependencies.` (the output directory then holds `im-15.1.0/LICENCE.md` and `append-only-bytes-0.1.12/LICENSE`); the Mac install tests pass; `0 privacy guard findings`.

- [ ] **Step 7: Commit**

```bash
git add scripts/collect-licenses.py packaging/rust-licenses THIRD_PARTY_NOTICES.md mac-install.sh tests/test_mac_install.py
git commit -m "Ship notices for Loro's dependencies and carry the crate to Macs"
```

---

### Task 9: CI, dependency proposals and documentation

**Files:**
- Modify: `.github/workflows/checks.yml`, `tests/test_ci_security.py`, `.github/dependabot.yml`, `docs/reference.md`, `docs/ci-and-aur.md`, `CONTRIBUTING.md`, `AGENTS.md`, `CLAUDE.md`, `docs/rust-dependencies.json`

- [ ] **Step 1: Pin the new checks in the CI security test**

In `tests/test_ci_security.py`, in `test_source_only_resolves_and_every_suite_starts_from_it`, replace
```python
                     "run: cargo +1.99.1 test --locked"):
```
with
```python
                     "run: cargo +1.99.1 test --locked",
                     "cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --target-dir target/replica",
                     'run: python3 scripts/collect-licenses.py "$RUNNER_TEMP/rust-licenses"'):
```
Run: `python3 -m unittest tests.test_ci_security`
Expected: FAIL — the quick job runs neither yet.

- [ ] **Step 2: Run the crate and the license collection in the quick job**

In `.github/workflows/checks.yml`, job `quick`, replace
```yaml
      - run: cargo +1.99.1 test --locked
      - name: Save Rust 1.99 cache
```
with
```yaml
      - run: cargo +1.99.1 test --locked
      - name: Replica crate (format, lints, tests, golden vectors)
        run: |
          rustup component add --toolchain 1.99.1 rustfmt clippy
          cargo +1.99.1 fmt --check --manifest-path crates/replica/Cargo.toml
          cargo +1.99.1 clippy --locked --manifest-path crates/replica/Cargo.toml --target-dir target/replica --all-targets -- -D warnings
          cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --target-dir target/replica
      # Packages fail without a notice for every shipped crate; find out here.
      - name: Rust license notices
        run: python3 scripts/collect-licenses.py "$RUNNER_TEMP/rust-licenses"
      - name: Save Rust 1.99 cache
```
`target/replica` lies inside the job's cached `target`; the license collection reads the registry the tests already filled.

In `.github/dependabot.yml`, in the `cargo` entry, change
`directories: ["/", "/services/mobile/relay", "/services/mobile/connector"]` to
`directories: ["/", "/services/mobile/relay", "/services/mobile/connector", "/crates/replica"]`.

In `docs/ci-and-aur.md`, in the jobs table, replace the row
```markdown
| Quick checks | Always | Privacy, Python/Bash syntax, version consistency, actionlint, stable/nightly publication contracts, locked Rust 1.99 unit tests |
```
with
```markdown
| Quick checks | Always | Privacy, Python/Bash syntax, version consistency, actionlint, stable/nightly publication contracts, locked Rust 1.99 unit tests, the replica crate (format, clippy, tests, golden vectors), Rust license notices |
```
and in the cache table, replace the row
```markdown
| `msrv-`, `cli-`, `windows-cli-` | Quick checks, CLI, Windows | OS/arch, hash of `rustc -Vv`, month, `Cargo.lock` hash | Exact key missed; Rust build passed | Monthly restart; about 200–250 MB each |
```
with
```markdown
| `msrv-`, `cli-`, `windows-cli-` | Quick checks, CLI, Windows | OS/arch, hash of `rustc -Vv`, month, `Cargo.lock` hash | Exact key missed; Rust build passed | Monthly restart; about 200–250 MB each; the quick checks' `target/replica` (the replica crate's own lockfile) rides under the same key |
```

- [ ] **Step 3: Validate the workflow locally**

Run:
```bash
python3 -m unittest tests.test_ci_security
python3 scripts/ci/check-source.py
lint_dir=$(mktemp -d) && (cd "$lint_dir" && \
  curl -fsSLO https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_linux_amd64.tar.gz && \
  curl -fsSLO https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_checksums.txt && \
  sha256sum --check --ignore-missing actionlint_1.7.12_checksums.txt && \
  tar xzf actionlint_1.7.12_linux_amd64.tar.gz actionlint) && \
  "$lint_dir/actionlint" -color -ignore 'unexpected key "cache-mode" for "(workflow|job)" section'
```
Expected: all pass; actionlint prints nothing.

- [ ] **Step 4: Document `hgs task`**

In `docs/reference.md`, directly above `## Machines and accounts`, add:
````markdown
## Project tasks

`hgs task` keeps a project's task list on this computer: prompts that wait in
Backlog or To do until someone sends them to a session. Every command prints
JSON. Tasks belong to a catalog project, named by its ID or name; a task is
`T-` plus six characters, and `PROJECT/T-xxxxxx` names it anywhere.

```sh
hgs task add --project orchard [--backlog] [--target codex/api|any] [--position N] < prompt.txt
hgs task list --project orchard [--stage todo]
hgs task get orchard/T-a1b2c3
hgs task send orchard/T-a1b2c3 [--session codex/api]
hgs task report orchard/T-a1b2c3 < summary.txt
hgs task cancel orchard/T-a1b2c3 [--reason "Wrong approach"]
hgs task stage orchard/T-a1b2c3 backlog|todo|done
hgs task move orchard/T-a1b2c3 --top|--before REF|--after REF
hgs task edit orchard/T-a1b2c3 < prompt.txt
hgs task comment orchard/T-a1b2c3 < comment.txt
hgs task target orchard/T-a1b2c3 codex/api|any
hgs task settings --project orchard [--set < settings.json]
```

Send pastes the task prompt into a session on this computer through the same
input path as `hgs send`; the task becomes In progress, and Zerus does not
check that the agent received it. The prompt asks the agent to report with
`hgs task report`, which moves the task to In review. Cancel moves a task In
progress to In review and pastes the cancel message without stopping the turn;
its reason is kept as a comment and never pasted. A task this computer already
shows In progress is not sent again. Run inside a session (`HGS_SESSION` and
`HGS_RUN_ID`), `hgs task` acts as that session's agent: `add` files the task
for that session, and the project settings can keep agents to their own
session or limit how many tasks they start per hour.

Tasks are Loro documents below `HGS_STATE_DIR/replica`, one directory per
project; a task file that fails to load is listed under `damaged` and left in
place. Replication between computers, the Tasks view and the MCP tools come in
later releases.
````

In `CONTRIBUTING.md`, after the sentence ending with ``for that pinned toolchain.``, add:
```markdown
The replica crate has its own lockfile: run
`cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml`; its
golden vectors must keep decoding, and `ZERUS_BLESS=1` only adds new ones.
```

In both `AGENTS.md` and `CLAUDE.md`, in the first paragraph, replace
```text
The CLI lives in `src/`; the Qt GUI lives in `tray/src/`.
```
with
```text
The CLI lives in `src/`; the Qt GUI lives in `tray/src/`;
replicated documents (Loro) live in `crates/replica/`, the only code that imports Loro.
```
and reflow the paragraph to the file's line width.

- [ ] **Step 5: Refresh the Rust dependency inventory**

Run from the worktree root:
```bash
cargo +1.99.1 metadata --format-version 1 --locked | python3 -c '
import json, sys
packages = [p for p in json.load(sys.stdin)["packages"] if p["source"]]
rows = sorted(({"name": p["name"], "version": p["version"], "license": p["license"],
                "repository": p["repository"]} for p in packages), key=lambda r: (r["name"], r["version"]))
json.dump(rows, open("docs/rust-dependencies.json", "w"), indent=2)
open("docs/rust-dependencies.json", "a").write("\n")'
git diff --stat docs/rust-dependencies.json
```
Expected: entries for `loro` 1.16.2 and its dependencies appear (the file was also behind `Cargo.lock`; this refreshes those entries too). The only new terms that are not permissive (MIT, Apache-2.0, BSD and the like) are MPL-2.0 for `im`, `bitmaps` and `sized-chunks`, recorded in Task 8; stop and report anything else copyleft.

- [ ] **Step 6: Full local checks**

Run:
```bash
python3 scripts/ci/check-source.py
cargo +1.99.1 test --locked
cargo +1.99.1 test --locked --manifest-path crates/replica/Cargo.toml --target-dir target/replica
python3 scripts/collect-licenses.py "$(mktemp -d)"
bash scripts/ci/cli.sh
```
Expected: all pass (`scripts/ci/cli.sh` runs the Python suites, `tests/test_tasks.py` included).

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/checks.yml tests/test_ci_security.py .github/dependabot.yml \
  docs/reference.md docs/ci-and-aur.md CONTRIBUTING.md AGENTS.md CLAUDE.md docs/rust-dependencies.json
git commit -m "Check the replica crate and license notices in CI and document hgs task"
```

