# Overmind 1a — Local Tracker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A working Overmind task tracker on one machine: an append-only event
log, folding into task state under a data-driven workflow with gates, and the
`hgs overmind` CLI for people and for agents in Zerus sessions.

**Architecture:** A new top-level Rust module `src/overmind/` next to `src/swarm/`.
Events live in SQLite under `<state root>/overmind/`. Every command runs in one
IMMEDIATE transaction: replay a receipt, fold the current state, prepare events,
prove them by folding, append. The fold is pure and deterministic, so stage 1b
can replicate events between machines without changing it.

**Tech Stack:** Rust 2021 (`anyhow`, `rusqlite` bundled, `serde`, `serde_json`,
`sha2`, `base64`, `uuid` v4, `tempfile`), Python `unittest` for CLI integration.

**Spec:** `docs/superpowers/specs/2026-10-11-overmind-core-design.md`
(Parts 1–4 and 8–10). Replication, blobs between machines, sending into
sessions, MCP, GUI and mobile are later plans (1b, 1c, 1d).

## Global Constraints

- Minimum Rust 1.85, edition 2021: no let-chains, no std APIs newer than 1.85.
- No new crates and no new crate features: `cargo build --locked` must pass with the current `Cargo.lock`.
- Agent-facing text (CLI help for agents, `note` fields, refusal messages) is English.
- Journal markers are ASCII and frozen: `[created]`, `[edited]`, `[moved]`, `[claim]`, `[unclaim]`, `[spec]`, `[worklog]`, `[review]`, `[needs-human]`, `[answer]`, `[blocked]`, `[decompose]`, `[filed-by-agent]`, `[attach]`, `[comment]`, `[done]`, `[rejected]`, `[conflict]`.
- Limits: prompt ≤ 64 KiB (bytes) and non-empty; title ≤ 200 characters; ≤ 8 images, each ≤ 10 MiB, total ≤ 20 MiB, MIME `image/*`; request ≤ 29 MiB; other text fields ≤ 64 KiB.
- Store directory `<state root>/overmind/` (`config.state.join("overmind")`), mode 0700; files 0600.
- Done is reachable only by humans, enforced inside the fold for every write path.
- Task UUIDs are v4 (the spec's "UUIDv7" is corrected in Task 7: the `uuid` crate's v7 feature is not enabled).
- Tests never touch live state: Rust tests use `tempfile::tempdir()`; Python tests set `HOME`, `HGS_CONFIG_DIR`, `HGS_STATE_DIR` to a temporary directory, put a fake `tmux` first in `PATH`, and remove `HGS_SESSION`, `HGS_RUN_ID`, `TMUX`, `TMUX_PANE`, `HGS_PEERS`, `HGS_SELF`.
- Commit after each task on branch `overmind` with `git commit --only <own paths>`; never add Claude attribution; never write home paths or e-mail addresses into files (`python3 scripts/check-source-privacy.py` must pass).

## Review Focus

- Two processes writing at once (desktop and an agent CLI): no lost events, per-machine `seq` stays contiguous. Test in Task 1 (`concurrent_writers_keep_seq_contiguous`).
- Prompts with multi-byte UTF-8 at the exact limits: 64 KiB bytes accepted, one byte more refused; titles counted in characters. Test in Task 5 (`limits_count_bytes_for_prompts_and_characters_for_titles`).
- A damaged database or event row gives a clear error instead of a panic. Test in Task 1 (`damaged_rows_report_an_error`).
- Replaying a write by `request_id` after the task changed returns the original result, and reusing it for a different body is refused. Test in Task 5 (`receipts_replay_and_refuse_reuse`).
- Editing a workflow that removes a stage still holding tasks is refused without `moves`; the fold remaps stages deterministically. Tests in Task 3 (`workflow_change_remaps_removed_stage`) and Task 5 (`workflow_set_requires_moves_for_occupied_stages`).

---

### Task 1: Event model, SQLite store and blob store

**Files:**
- Create: `src/overmind/mod.rs`, `src/overmind/model.rs`, `src/overmind/store.rs`, `src/overmind/blobs.rs`
- Modify: `src/main.rs` (add `mod overmind;`)

**Interfaces:**
- Produces (used by every later task):
  - `model::{EVENT_VERSION, ActorKind, Actor, SessionRef, Event, Draft, Refusal, now}`; `Actor::human(&str)`, `Actor::agent(&str, &str)`, `Actor::session_ref() -> Option<SessionRef>`, `Event::order() -> (u64, &str, u64)`, `Refusal::new(code, message)`, `Refusal::field(code, field, state, message)`.
  - `store::{Filter, Store, Tx}`: `Store::open(&Path, &str) -> Result<Store>`, `machine()`, `dir()`, `load(&Filter) -> Result<Vec<Event>>` (sorted by `order()`), `read(FnOnce(&Tx) -> Result<T>) -> Result<T>` (snapshot, never bumps `rev`), `task_ids() -> Result<Vec<String>>`, `cursor() -> Result<i64>`, `touched_since(i64) -> Result<BTreeSet<String>>`, `version_vector() -> Result<BTreeMap<String,u64>>`, `missing(&BTreeMap<String,u64>, usize) -> Result<(Vec<Value>, bool)>`, `insert_raw(&[Value]) -> Result<usize>`, `write(FnOnce(&mut Tx) -> Result<T>) -> Result<T>`; `Tx::{load, task_ids, preview(&Draft) -> Result<Event>, insert(&Event), append(&Draft) -> Result<Event>, receipt(&str) -> Result<Option<(String, Value)>>, put_receipt(&str, &str, &Value)}`.
  - `blobs::{put(&Path, &[u8]) -> Result<String>, path(&Path, &str) -> Result<PathBuf>}`.

- [ ] **Step 1: Create the module skeleton**

`src/overmind/mod.rs`:

```rust
//! Overmind: tasks whose content is a ready prompt, moved through a gated,
//! data-driven workflow. See docs/superpowers/specs/2026-10-11-overmind-core-design.md.
#![allow(dead_code)] // Removed in Task 7 once the CLI uses every part.
mod blobs;
mod model;
mod store;
```

In `src/main.rs` add `mod overmind;` after `mod native_ui;` (keep the list's order otherwise).

- [ ] **Step 2: Write `model.rs`**

```rust
//! Overmind events. Stored events are parsed leniently so that fields added by
//! a newer machine survive on an older one; requests are parsed strictly.
use serde::{Deserialize, Serialize};
use serde_json::Value;

/// The newest event payload version this build understands.
pub const EVENT_VERSION: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ActorKind {
    Human,
    Agent,
    Autopilot,
    Manager,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Actor {
    pub kind: ActorKind,
    pub machine: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub session: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub device: String,
}

impl Actor {
    pub fn human(machine: &str) -> Self {
        Self { kind: ActorKind::Human, machine: machine.into(), session: String::new(), device: String::new() }
    }
    pub fn agent(machine: &str, session: &str) -> Self {
        Self { kind: ActorKind::Agent, machine: machine.into(), session: session.into(), device: String::new() }
    }
    /// The session an agent or manager acts for.
    pub fn session_ref(&self) -> Option<SessionRef> {
        if self.session.is_empty() {
            return None;
        }
        Some(SessionRef { machine: self.machine.clone(), name: self.session.clone() })
    }
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct SessionRef {
    pub machine: String,
    pub name: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Event {
    pub id: String,
    pub origin: String,
    pub seq: u64,
    pub lamport: u64,
    pub at: u64,
    pub v: u32,
    pub actor: Actor,
    #[serde(default)]
    pub task: String,
    #[serde(default)]
    pub project: String,
    pub kind: String,
    #[serde(default)]
    pub base: String,
    #[serde(default)]
    pub request_id: String,
    #[serde(default)]
    pub payload: Value,
}

impl Event {
    /// The total order in which every machine applies events.
    pub fn order(&self) -> (u64, &str, u64) {
        (self.lamport, self.origin.as_str(), self.seq)
    }
}

/// An event a command wants to append; the store assigns identity and clocks.
#[derive(Clone, Debug)]
pub struct Draft {
    pub actor: Actor,
    pub task: String,
    pub project: String,
    pub kind: String,
    pub base: String,
    pub request_id: String,
    pub payload: Value,
}

pub fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

/// A refused action. `state` says how a field arrived: missing, empty or invalid.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Refusal {
    pub code: String,
    pub message: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub field: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub state: String,
}

impl Refusal {
    pub fn new(code: &str, message: impl Into<String>) -> Self {
        Self { code: code.into(), message: message.into(), field: String::new(), state: String::new() }
    }
    pub fn field(code: &str, field: &str, state: &str, message: impl Into<String>) -> Self {
        Self { code: code.into(), message: message.into(), field: field.into(), state: state.into() }
    }
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.message.fmt(f)
    }
}

impl std::error::Error for Refusal {}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn stored_events_tolerate_unknown_fields() {
        let event: Event = serde_json::from_value(json!({
            "id": "e", "origin": "o", "seq": 1, "lamport": 1, "at": 0, "v": 2,
            "actor": {"kind": "human", "machine": "m", "future": true},
            "kind": "future_kind", "payload": {}, "future_field": 1
        }))
        .unwrap();
        assert_eq!(event.v, 2);
        assert_eq!(event.actor.kind, ActorKind::Human);
    }

    #[test]
    fn refusal_serializes_only_present_fields() {
        let plain = serde_json::to_value(Refusal::new("stale", "changed")).unwrap();
        assert_eq!(plain, json!({"code": "stale", "message": "changed"}));
        let field = serde_json::to_value(Refusal::field("missing_field", "spec", "missing", "m")).unwrap();
        assert_eq!(field["field"], "spec");
        assert_eq!(field["state"], "missing");
    }
}
```

- [ ] **Step 3: Write `blobs.rs`**

```rust
//! Attachment bytes by SHA-256: immutable, private, written atomically.
use anyhow::{ensure, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
use std::path::{Path, PathBuf};

pub fn path(dir: &Path, sha256: &str) -> Result<PathBuf> {
    ensure!(
        sha256.len() == 64 && sha256.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')),
        "invalid attachment digest"
    );
    Ok(dir.join("blobs").join(&sha256[..2]).join(sha256))
}

/// Stores `bytes` and returns their SHA-256. Existing blobs are left as they are.
pub fn put(dir: &Path, bytes: &[u8]) -> Result<String> {
    let sha = format!("{:x}", Sha256::digest(bytes));
    let target = path(dir, &sha)?;
    if target.exists() {
        return Ok(sha);
    }
    let parent = target.parent().expect("blob paths have a parent");
    fs::DirBuilder::new().recursive(true).mode(0o700).create(parent)?;
    let mut temp = tempfile::Builder::new().prefix(".blob-").tempfile_in(parent)?;
    temp.write_all(bytes)?;
    temp.as_file().set_permissions(fs::Permissions::from_mode(0o600))?;
    temp.as_file().sync_all()?;
    temp.persist(&target).map_err(|e| e.error)?;
    Ok(sha)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn blobs_are_private_and_deduplicated() {
        let dir = tempfile::tempdir().unwrap();
        let first = put(dir.path(), b"image").unwrap();
        let second = put(dir.path(), b"image").unwrap();
        assert_eq!(first, second);
        let file = path(dir.path(), &first).unwrap();
        assert_eq!(fs::read(&file).unwrap(), b"image");
        assert_eq!(fs::metadata(&file).unwrap().permissions().mode() & 0o777, 0o600);
        assert!(path(dir.path(), "../etc").is_err());
    }
}
```

- [ ] **Step 4: Write `store.rs` with its tests**

```rust
//! Append-only Overmind event log in SQLite. Each write runs in one IMMEDIATE
//! transaction, so a command's checks and appends never interleave with
//! another writer's. Every store holds a contiguous prefix of each machine's
//! events, so a version vector describes it exactly.
use super::model::{now, Draft, Event, EVENT_VERSION};
use anyhow::{ensure, Context, Result};
use rusqlite::{params, Connection, OptionalExtension, TransactionBehavior};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Write;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::time::Duration;

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS events(
  origin TEXT NOT NULL, seq INTEGER NOT NULL, id TEXT NOT NULL UNIQUE,
  lamport INTEGER NOT NULL, task TEXT NOT NULL, project TEXT NOT NULL,
  kind TEXT NOT NULL, raw TEXT NOT NULL, PRIMARY KEY(origin, seq));
CREATE INDEX IF NOT EXISTS events_task ON events(task);
CREATE INDEX IF NOT EXISTS events_project ON events(project);
CREATE INDEX IF NOT EXISTS events_lamport ON events(lamport);
CREATE TABLE IF NOT EXISTS receipts(
  request_id TEXT PRIMARY KEY, digest TEXT NOT NULL, result TEXT NOT NULL);
";

/// Which events to load. Shared events have no task: workflows, settings, renames.
#[derive(Clone, Debug)]
pub enum Filter {
    All,
    Task(String),
    /// Task-less events of this project plus global ones.
    Shared(String),
    /// Every event of a project, its tasks included, plus global ones.
    Project(String),
}

pub struct Store {
    conn: Connection,
    dir: PathBuf,
    machine: String,
}

pub struct Tx<'a> {
    tx: rusqlite::Transaction<'a>,
    machine: String,
}

fn query(conn: &Connection, filter: &Filter) -> Result<Vec<Event>> {
    let (sql, arg): (&str, Option<&str>) = match filter {
        Filter::All => ("SELECT raw FROM events", None),
        Filter::Task(task) => ("SELECT raw FROM events WHERE task = ?1", Some(task)),
        Filter::Shared(project) => (
            "SELECT raw FROM events WHERE task = '' AND (project = ?1 OR project = '')",
            Some(project),
        ),
        Filter::Project(project) => (
            "SELECT raw FROM events WHERE project = ?1 OR (task = '' AND project = '')",
            Some(project),
        ),
    };
    let mut statement = conn.prepare(sql)?;
    let rows: Vec<String> = match arg {
        Some(arg) => statement
            .query_map(params![arg], |row| row.get(0))?
            .collect::<rusqlite::Result<_>>()?,
        None => statement
            .query_map([], |row| row.get(0))?
            .collect::<rusqlite::Result<_>>()?,
    };
    let mut events = rows
        .iter()
        .map(|raw| serde_json::from_str::<Event>(raw).context("a stored Overmind event is damaged"))
        .collect::<Result<Vec<_>>>()?;
    events.sort_by(|a, b| a.order().cmp(&b.order()));
    Ok(events)
}

fn task_ids(conn: &Connection) -> Result<Vec<String>> {
    let mut statement = conn.prepare("SELECT DISTINCT task FROM events WHERE kind = 'task_created'")?;
    let ids = statement
        .query_map([], |row| row.get(0))?
        .collect::<rusqlite::Result<Vec<String>>>()?;
    Ok(ids)
}

fn insert(conn: &Connection, event: &Event, raw: &str) -> Result<()> {
    conn.execute(
        "INSERT INTO events(origin, seq, id, lamport, task, project, kind, raw) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            event.origin,
            event.seq as i64,
            event.id,
            event.lamport as i64,
            event.task,
            event.project,
            event.kind,
            raw
        ],
    )?;
    Ok(())
}

impl Store {
    pub fn open(dir: &Path, machine: &str) -> Result<Self> {
        ensure!(uuid::Uuid::parse_str(machine).is_ok(), "invalid machine identity");
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(dir)
            .with_context(|| format!("create {}", dir.display()))?;
        fs::set_permissions(dir, fs::Permissions::from_mode(0o700))?;
        let path = dir.join("overmind.sqlite3");
        if !path.exists() {
            fs::OpenOptions::new().create(true).append(true).mode(0o600).open(&path)?;
        }
        let conn = Connection::open(&path).with_context(|| format!("open {}", path.display()))?;
        conn.busy_timeout(Duration::from_secs(10))?;
        conn.query_row("PRAGMA journal_mode=WAL", [], |_| Ok(()))?;
        conn.execute_batch("PRAGMA synchronous=FULL;")?;
        conn.execute_batch(SCHEMA).context("the Overmind database is damaged")?;
        Ok(Self { conn, dir: dir.to_owned(), machine: machine.to_owned() })
    }

    pub fn machine(&self) -> &str {
        &self.machine
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    pub fn load(&self, filter: &Filter) -> Result<Vec<Event>> {
        query(&self.conn, filter)
    }

    pub fn task_ids(&self) -> Result<Vec<String>> {
        task_ids(&self.conn)
    }

    /// Local insertion position; increases with every stored event.
    pub fn cursor(&self) -> Result<i64> {
        Ok(self.conn.query_row("SELECT COALESCE(MAX(rowid), 0) FROM events", [], |row| row.get(0))?)
    }

    /// Tasks with events stored after `cursor`. The empty string stands for shared events.
    pub fn touched_since(&self, cursor: i64) -> Result<BTreeSet<String>> {
        let mut statement = self.conn.prepare("SELECT DISTINCT task FROM events WHERE rowid > ?1")?;
        let tasks = statement
            .query_map(params![cursor], |row| row.get(0))?
            .collect::<rusqlite::Result<BTreeSet<String>>>()?;
        Ok(tasks)
    }

    pub fn version_vector(&self) -> Result<BTreeMap<String, u64>> {
        let mut statement = self.conn.prepare("SELECT origin, MAX(seq) FROM events GROUP BY origin")?;
        let vector = statement
            .query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)? as u64)))?
            .collect::<rusqlite::Result<BTreeMap<_, _>>>()?;
        Ok(vector)
    }

    /// Raw events the peer lacks, per origin in `seq` order, up to about
    /// `max_bytes`. The flag says whether more remain.
    pub fn missing(&self, peer: &BTreeMap<String, u64>, max_bytes: usize) -> Result<(Vec<Value>, bool)> {
        let mut statement = self.conn.prepare("SELECT origin, seq, raw FROM events ORDER BY origin, seq")?;
        let rows = statement
            .query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)? as u64, row.get::<_, String>(2)?)))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        let mut out = Vec::new();
        let mut bytes = 0;
        for (origin, seq, raw) in rows {
            if seq <= peer.get(&origin).copied().unwrap_or(0) {
                continue;
            }
            if !out.is_empty() && bytes + raw.len() > max_bytes {
                return Ok((out, true));
            }
            bytes += raw.len();
            out.push(serde_json::from_str(&raw)?);
        }
        Ok((out, false))
    }

    /// Inserts events from another machine. Known events are skipped, and an
    /// origin's events after a gap are left for a later exchange.
    pub fn insert_raw(&mut self, raws: &[Value]) -> Result<usize> {
        let mut events = raws
            .iter()
            .map(|raw| Ok((serde_json::from_value::<Event>(raw.clone())?, raw.to_string())))
            .collect::<Result<Vec<_>>>()?;
        events.sort_by(|a, b| (a.0.origin.as_str(), a.0.seq).cmp(&(b.0.origin.as_str(), b.0.seq)));
        let tx = self.conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let mut inserted = 0;
        for (event, raw) in &events {
            ensure!(
                uuid::Uuid::parse_str(&event.id).is_ok()
                    && uuid::Uuid::parse_str(&event.origin).is_ok()
                    && event.seq > 0,
                "received an invalid Overmind event"
            );
            let max: i64 = tx.query_row(
                "SELECT COALESCE(MAX(seq), 0) FROM events WHERE origin = ?1",
                params![event.origin],
                |row| row.get(0),
            )?;
            if event.seq != max as u64 + 1 {
                continue;
            }
            insert(&tx, event, raw)?;
            inserted += 1;
        }
        tx.commit()?;
        if inserted > 0 {
            self.bump()?;
        }
        Ok(inserted)
    }

    /// Runs `f` against a consistent snapshot; nothing is written.
    pub fn read<T>(&mut self, f: impl FnOnce(&Tx<'_>) -> Result<T>) -> Result<T> {
        let tx = self.conn.transaction_with_behavior(TransactionBehavior::Deferred)?;
        let wrapped = Tx { tx, machine: self.machine.clone() };
        let value = f(&wrapped);
        drop(wrapped);
        value
    }

    /// Runs `f` in one IMMEDIATE transaction; an error rolls everything back.
    pub fn write<T>(&mut self, f: impl FnOnce(&mut Tx<'_>) -> Result<T>) -> Result<T> {
        let tx = self.conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        let mut wrapped = Tx { tx, machine: self.machine.clone() };
        let value = f(&mut wrapped)?;
        wrapped.tx.commit()?;
        self.bump()?;
        Ok(value)
    }

    /// Publishes the cursor in `rev` so that watchers notice changes.
    fn bump(&self) -> Result<()> {
        let mut temp = tempfile::Builder::new().prefix(".rev-").tempfile_in(&self.dir)?;
        temp.write_all(self.cursor()?.to_string().as_bytes())?;
        temp.as_file().set_permissions(fs::Permissions::from_mode(0o600))?;
        temp.as_file().sync_all()?;
        temp.persist(self.dir.join("rev")).map_err(|e| e.error)?;
        Ok(())
    }
}

impl Tx<'_> {
    pub fn load(&self, filter: &Filter) -> Result<Vec<Event>> {
        query(&self.tx, filter)
    }

    pub fn task_ids(&self) -> Result<Vec<String>> {
        task_ids(&self.tx)
    }

    /// The event `append` would store, without storing it.
    pub fn preview(&self, draft: &Draft) -> Result<Event> {
        let seq: i64 = self.tx.query_row(
            "SELECT COALESCE(MAX(seq), 0) + 1 FROM events WHERE origin = ?1",
            params![self.machine],
            |row| row.get(0),
        )?;
        let lamport: i64 =
            self.tx.query_row("SELECT COALESCE(MAX(lamport), 0) + 1 FROM events", [], |row| row.get(0))?;
        Ok(Event {
            id: uuid::Uuid::new_v4().to_string(),
            origin: self.machine.clone(),
            seq: seq as u64,
            lamport: lamport as u64,
            at: now(),
            v: EVENT_VERSION,
            actor: draft.actor.clone(),
            task: draft.task.clone(),
            project: draft.project.clone(),
            kind: draft.kind.clone(),
            base: draft.base.clone(),
            request_id: draft.request_id.clone(),
            payload: draft.payload.clone(),
        })
    }

    pub fn insert(&mut self, event: &Event) -> Result<()> {
        insert(&self.tx, event, &serde_json::to_string(event)?)
    }

    pub fn append(&mut self, draft: &Draft) -> Result<Event> {
        let event = self.preview(draft)?;
        self.insert(&event)?;
        Ok(event)
    }

    pub fn receipt(&self, request_id: &str) -> Result<Option<(String, Value)>> {
        let row = self
            .tx
            .query_row(
                "SELECT digest, result FROM receipts WHERE request_id = ?1",
                params![request_id],
                |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?)),
            )
            .optional()?;
        match row {
            Some((digest, result)) => Ok(Some((digest, serde_json::from_str(&result)?))),
            None => Ok(None),
        }
    }

    pub fn put_receipt(&mut self, request_id: &str, digest: &str, result: &Value) -> Result<()> {
        self.tx.execute(
            "INSERT INTO receipts(request_id, digest, result) VALUES (?1, ?2, ?3)",
            params![request_id, digest, result.to_string()],
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::model::Actor;
    use serde_json::json;

    pub const MACHINE: &str = "11111111-1111-4111-8111-111111111111";
    const OTHER: &str = "22222222-2222-4222-8222-222222222222";

    fn draft(task: &str) -> Draft {
        Draft {
            actor: Actor::human(MACHINE),
            task: task.into(),
            project: "p".into(),
            kind: "journal".into(),
            base: String::new(),
            request_id: String::new(),
            payload: json!({"marker": "[comment]", "text": "x"}),
        }
    }

    #[test]
    fn open_creates_private_files() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("overmind");
        let mut store = Store::open(&root, MACHINE).unwrap();
        store.write(|tx| tx.append(&draft("t"))).unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&root), 0o700);
        assert_eq!(mode(&root.join("overmind.sqlite3")), 0o600);
        assert_eq!(mode(&root.join("rev")), 0o600);
        assert_eq!(fs::read_to_string(root.join("rev")).unwrap(), "1");
    }

    #[test]
    fn appends_assign_contiguous_seq_and_increasing_lamport() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let (a, b) = store.write(|tx| Ok((tx.append(&draft("t"))?, tx.append(&draft("t"))?))).unwrap();
        assert_eq!((a.seq, b.seq), (1, 2));
        assert!(b.lamport > a.lamport);
        assert_eq!(store.load(&Filter::Task("t".into())).unwrap(), vec![a, b]);
        assert_eq!(store.version_vector().unwrap().get(MACHINE), Some(&2));
    }

    #[test]
    fn failed_writes_roll_back() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let result: Result<()> = store.write(|tx| {
            tx.append(&draft("t"))?;
            anyhow::bail!("refused")
        });
        assert!(result.is_err());
        assert!(store.load(&Filter::All).unwrap().is_empty());
    }

    #[test]
    fn remote_events_are_idempotent_contiguous_and_advance_lamport() {
        let dir = tempfile::tempdir().unwrap();
        let source_dir = tempfile::tempdir().unwrap();
        let mut source = Store::open(source_dir.path(), OTHER).unwrap();
        for _ in 0..3 {
            source.write(|tx| tx.append(&draft("t"))).unwrap();
        }
        let (all, more) = source.missing(&BTreeMap::new(), usize::MAX).unwrap();
        assert!(!more);
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        // A gap (seq 1 missing) inserts nothing for that origin.
        assert_eq!(store.insert_raw(&all[1..]).unwrap(), 0);
        assert_eq!(store.insert_raw(&all).unwrap(), 3);
        assert_eq!(store.insert_raw(&all).unwrap(), 0);
        let local = store.write(|tx| tx.append(&draft("t"))).unwrap();
        assert!(local.lamport > 3);
        let (rest, _) = store.missing(&source.version_vector().unwrap(), usize::MAX).unwrap();
        assert_eq!(rest.len(), 1);
    }

    #[test]
    fn missing_respects_the_byte_budget() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        for _ in 0..5 {
            store.write(|tx| tx.append(&draft("t"))).unwrap();
        }
        let (first, more) = store.missing(&BTreeMap::new(), 1).unwrap();
        assert_eq!((first.len(), more), (1, true));
    }

    #[test]
    fn receipts_roundtrip() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        store.write(|tx| tx.put_receipt("r", "d", &json!({"ok": true}))).unwrap();
        let found = store.write(|tx| tx.receipt("r")).unwrap();
        assert_eq!(found, Some(("d".into(), json!({"ok": true}))));
    }

    #[test]
    fn concurrent_writers_keep_seq_contiguous() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        Store::open(&path, MACHINE).unwrap();
        let threads: Vec<_> = (0..2)
            .map(|_| {
                let path = path.clone();
                std::thread::spawn(move || {
                    let mut store = Store::open(&path, MACHINE).unwrap();
                    for _ in 0..50 {
                        store.write(|tx| tx.append(&draft("t"))).unwrap();
                    }
                })
            })
            .collect();
        for thread in threads {
            thread.join().unwrap();
        }
        let store = Store::open(&path, MACHINE).unwrap();
        let mut seqs: Vec<u64> = store.load(&Filter::All).unwrap().iter().map(|e| e.seq).collect();
        seqs.sort();
        assert_eq!(seqs, (1..=100).collect::<Vec<_>>());
    }

    #[test]
    fn damaged_rows_report_an_error() {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::open(dir.path(), MACHINE).unwrap();
        store
            .conn
            .execute(
                "INSERT INTO events(origin, seq, id, lamport, task, project, kind, raw) VALUES ('o', 1, 'i', 1, 't', '', 'k', '{')",
                [],
            )
            .unwrap();
        let error = store.load(&Filter::All).unwrap_err();
        assert!(format!("{error:#}").contains("damaged"));
        let broken = tempfile::tempdir().unwrap();
        fs::write(broken.path().join("overmind.sqlite3"), b"not a database at all, definitely").unwrap();
        assert!(Store::open(broken.path(), MACHINE).is_err());
    }
}
```

- [ ] **Step 5: Run the tests**

Run: `cargo test --locked overmind:: 2>&1 | tail -20`
Expected: all `overmind::model`, `overmind::blobs`, `overmind::store` tests PASS. (Write the tests first if you prefer; they are given together because the store's API is fixed by this plan.)

- [ ] **Step 6: Commit**

```bash
git add src/overmind/mod.rs src/overmind/model.rs src/overmind/store.rs src/overmind/blobs.rs src/main.rs
git commit --only src/overmind/mod.rs src/overmind/model.rs src/overmind/store.rs src/overmind/blobs.rs src/main.rs \
  -m "Store Overmind events in a private append-only SQLite log"
```

---

### Task 2: Workflow engine

**Files:**
- Create: `src/overmind/workflow.rs`
- Modify: `src/overmind/mod.rs` (add `mod workflow;`)

**Interfaces:**
- Consumes: `model::{ActorKind, Refusal}`.
- Produces: `workflow::{Role, Action, Evidence, Stage, Rule, Workflow, RESUME, FIELDS}`; `Workflow::default_v1()`, `validate() -> anyhow::Result<()>`, `stage(&str) -> Option<&Stage>`, `role(&str) -> Option<Role>`, `first(Role) -> Option<&Stage>`, `allow(Action, Option<&str>, &str, ActorKind, &BTreeMap<String, String>) -> Result<&Rule, Refusal>`.

- [ ] **Step 1: Write the failing tests** (at the bottom of the new file)

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use ActorKind::*;

    fn fields(pairs: &[(&str, &str)]) -> BTreeMap<String, String> {
        pairs.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect()
    }

    #[test]
    fn default_workflow_is_valid() {
        Workflow::default_v1().validate().unwrap();
    }

    #[test]
    fn advance_needs_a_spec_and_names_how_it_arrived() {
        let wf = Workflow::default_v1();
        let missing = wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[])).unwrap_err();
        assert_eq!((missing.code.as_str(), missing.field.as_str(), missing.state.as_str()), ("missing_field", "spec", "missing"));
        let empty = wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[("spec", "  ")])).unwrap_err();
        assert_eq!(empty.state, "empty");
        assert!(wf.allow(Action::Advance, Some("Design"), "Build", Agent, &fields(&[("spec", "plan")])).is_ok());
    }

    #[test]
    fn creation_rules_depend_on_the_actor() {
        let wf = Workflow::default_v1();
        assert!(wf.allow(Action::Create, None, "Queue", Human, &fields(&[])).is_ok());
        assert_eq!(wf.allow(Action::Create, None, "Queue", Agent, &fields(&[])).unwrap_err().field, "decomposed_from");
        assert_eq!(wf.allow(Action::Create, None, "Done", Human, &fields(&[])).unwrap_err().code, "transition_not_allowed");
        assert_eq!(wf.allow(Action::Create, None, "Backlog", Agent, &fields(&[])).unwrap_err().field, "breaks");
        assert!(wf.allow(Action::Create, None, "Backlog", Agent, &fields(&[("breaks", "x")])).is_ok());
        assert!(wf.allow(Action::Create, None, "Queue", Agent, &fields(&[("decomposed_from", "u")])).is_ok());
    }

    #[test]
    fn agents_never_reach_done() {
        let wf = Workflow::default_v1();
        for action in Action::ALL {
            assert!(wf.allow(action, Some("Review"), "Done", Agent, &fields(&[("report", "r")])).is_err());
        }
    }

    #[test]
    fn invalid_workflows_are_refused() {
        let mut duplicate = Workflow::default_v1();
        duplicate.stages.push(Stage { name: "Queue".into(), role: Role::Ready });
        assert!(duplicate.validate().is_err());
        let mut no_done = Workflow::default_v1();
        no_done.stages.retain(|s| s.role != Role::Done);
        no_done.rules.retain(|r| r.to != "Done" && !r.from.contains(&"Done".to_string()));
        assert!(no_done.validate().is_err());
        let mut unreachable = Workflow::default_v1();
        unreachable.stages.push(Stage { name: "Limbo".into(), role: Role::Parked });
        assert!(unreachable.validate().is_err());
        let mut agent_done = Workflow::default_v1();
        agent_done.rules.push(Rule { action: Action::Move, from: vec!["Build".into()], to: "Done".into(), actors: vec![Agent], requires: vec![], own: false });
        assert!(agent_done.validate().is_err());
        let mut bad_field = Workflow::default_v1();
        bad_field.rules[0].requires.push("password".into());
        assert!(bad_field.validate().is_err());
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo test --locked overmind::workflow 2>&1 | tail -5`
Expected: compile errors (`Workflow` not defined).

- [ ] **Step 3: Implement `workflow.rs`** (above the tests)

```rust
//! Workflows are data. The engine understands stage roles, never stage names.
use super::model::{ActorKind, Refusal};
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Role {
    Backlog,
    Ready,
    Active,
    Review,
    NeedsHuman,
    Done,
    Parked,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Action {
    Create,
    Move,
    Claim,
    Unclaim,
    Advance,
    CallHuman,
    Answer,
    Return,
    Decompose,
    Approve,
    NeedsWork,
    Done,
    Reopen,
}

impl Action {
    pub const ALL: [Action; 13] = [
        Action::Create, Action::Move, Action::Claim, Action::Unclaim, Action::Advance, Action::CallHuman,
        Action::Answer, Action::Return, Action::Decompose, Action::Approve, Action::NeedsWork, Action::Done,
        Action::Reopen,
    ];
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Evidence {
    /// A commit that exists in the session's repository; free text outside Git.
    GitShaOrText,
    Text,
    None,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Stage {
    pub name: String,
    pub role: Role,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Rule {
    pub action: Action,
    #[serde(default)]
    pub from: Vec<String>,
    pub to: String,
    pub actors: Vec<ActorKind>,
    #[serde(default)]
    pub requires: Vec<String>,
    /// The acting agent's session must be the task's assignee.
    #[serde(default)]
    pub own: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Workflow {
    pub version: u32,
    pub stages: Vec<Stage>,
    pub rules: Vec<Rule>,
    pub evidence: Evidence,
    pub session_wip: u32,
}

/// Target of `answer`: the active stage the task left for Your Call.
pub const RESUME: &str = "@resume";
pub const FIELDS: &[&str] = &[
    "spec", "worklog", "evidence", "root_cause", "question", "answer", "reason", "report", "breaks",
    "decomposed_from",
];

fn rule(action: Action, from: &[&str], to: &str, actors: &[ActorKind], requires: &[&str], own: bool) -> Rule {
    Rule {
        action,
        from: from.iter().map(|s| s.to_string()).collect(),
        to: to.into(),
        actors: actors.to_vec(),
        requires: requires.iter().map(|s| s.to_string()).collect(),
        own,
    }
}

fn blank(value: Option<&String>) -> Option<&'static str> {
    match value {
        None => Some("missing"),
        Some(v) if v.trim().is_empty() => Some("empty"),
        Some(_) => None,
    }
}

impl Workflow {
    /// The vikunja-mcp process: Backlog → Queue → Design → Build → Review → Done.
    pub fn default_v1() -> Self {
        use Action as A;
        use ActorKind::*;
        let stage = |name: &str, role| Stage { name: name.into(), role };
        let working = ["Backlog", "Queue", "Design", "Build", "Review", "Your Call"];
        Self {
            version: 1,
            stages: vec![
                stage("Backlog", Role::Backlog),
                stage("Queue", Role::Ready),
                stage("Design", Role::Active),
                stage("Build", Role::Active),
                stage("Review", Role::Review),
                stage("Your Call", Role::NeedsHuman),
                stage("Done", Role::Done),
                stage("Icebox", Role::Parked),
            ],
            rules: vec![
                rule(A::Create, &[], "Backlog", &[Human, Manager], &[], false),
                rule(A::Create, &[], "Backlog", &[Agent], &["breaks"], false),
                rule(A::Create, &[], "Queue", &[Human, Manager], &[], false),
                rule(A::Create, &[], "Queue", &[Agent], &["decomposed_from"], false),
                rule(A::Move, &["Backlog"], "Queue", &[Human], &[], false),
                rule(A::Move, &["Queue"], "Backlog", &[Human], &[], false),
                rule(A::Claim, &["Queue"], "Design", &[Human, Autopilot], &[], false),
                rule(A::Advance, &["Design"], "Build", &[Agent], &["spec"], true),
                rule(A::Advance, &["Build"], "Review", &[Agent], &["worklog", "evidence"], true),
                rule(A::CallHuman, &["Design", "Build"], "Your Call", &[Agent], &["question"], true),
                rule(A::Answer, &["Your Call"], RESUME, &[Human], &["answer"], false),
                rule(A::Return, &["Backlog", "Queue", "Design", "Build", "Your Call"], "Backlog", &[Agent], &["reason"], true),
                rule(A::Decompose, &["Design", "Build"], "Backlog", &[Agent], &[], true),
                rule(A::Approve, &["Review"], "Review", &[Agent, Human], &["report"], false),
                rule(A::NeedsWork, &["Review"], "Build", &[Agent, Human], &["report"], false),
                rule(A::Done, &["Review"], "Done", &[Human], &[], false),
                rule(A::Unclaim, &["Design", "Build"], "Queue", &[Human, Autopilot], &[], false),
                rule(A::Move, &working, "Icebox", &[Human], &[], false),
                rule(A::Move, &["Icebox"], "Backlog", &[Human], &[], false),
                rule(A::Move, &["Icebox"], "Queue", &[Human], &[], false),
                rule(A::Reopen, &["Done"], "Queue", &[Human], &[], false),
            ],
            evidence: Evidence::GitShaOrText,
            session_wip: 1,
        }
    }

    pub fn stage(&self, name: &str) -> Option<&Stage> {
        self.stages.iter().find(|s| s.name == name)
    }

    pub fn role(&self, name: &str) -> Option<Role> {
        self.stage(name).map(|s| s.role)
    }

    pub fn first(&self, role: Role) -> Option<&Stage> {
        self.stages.iter().find(|s| s.role == role)
    }

    pub fn validate(&self) -> Result<()> {
        ensure!(self.version == 1, "workflow version must be 1");
        ensure!(!self.stages.is_empty() && self.stages.len() <= 32, "a workflow has 1 to 32 stages");
        let mut names = BTreeSet::new();
        for stage in &self.stages {
            let length = stage.name.chars().count();
            ensure!(
                (1..=40).contains(&length) && !stage.name.chars().any(char::is_control) && !stage.name.starts_with('@'),
                "stage names are 1 to 40 printable characters and do not start with @"
            );
            ensure!(names.insert(stage.name.as_str()), "duplicate stage {}", stage.name);
        }
        let count = |role: Role| self.stages.iter().filter(|s| s.role == role).count();
        ensure!(count(Role::Done) == 1, "a workflow needs exactly one done stage");
        for role in [Role::Backlog, Role::Ready, Role::Active, Role::Review, Role::NeedsHuman] {
            ensure!(count(role) >= 1, "a workflow needs a {role:?} stage");
        }
        ensure!(self.rules.len() <= 200, "a workflow has at most 200 rules");
        for rule in &self.rules {
            ensure!(!rule.actors.is_empty(), "every rule names its actors");
            ensure!((rule.action == Action::Create) == rule.from.is_empty(), "only create rules have no source stage");
            for name in &rule.from {
                ensure!(names.contains(name.as_str()), "a rule refers to unknown stage {name}");
            }
            let resume = rule.action == Action::Answer && rule.to == RESUME;
            ensure!(resume || names.contains(rule.to.as_str()), "a rule refers to unknown stage {}", rule.to);
            for field in &rule.requires {
                ensure!(FIELDS.contains(&field.as_str()), "unknown required field {field}");
            }
            if self.role(&rule.to) == Some(Role::Done) {
                ensure!(
                    rule.actors.iter().all(|a| *a == ActorKind::Human),
                    "only people may move tasks to the done stage"
                );
            }
        }
        let mut reached: BTreeSet<&str> =
            self.rules.iter().filter(|r| r.action == Action::Create).map(|r| r.to.as_str()).collect();
        loop {
            let before = reached.len();
            for rule in &self.rules {
                if rule.to != RESUME && rule.from.iter().any(|f| reached.contains(f.as_str())) {
                    reached.insert(rule.to.as_str());
                }
            }
            if reached.len() == before {
                break;
            }
        }
        for stage in &self.stages {
            ensure!(reached.contains(stage.name.as_str()), "stage {} cannot be reached", stage.name);
        }
        Ok(())
    }

    /// The rule allowing `action` from `from` (None when creating) to `to`.
    /// A rule that matches but lacks a field refuses with that field.
    pub fn allow(
        &self,
        action: Action,
        from: Option<&str>,
        to: &str,
        actor: ActorKind,
        fields: &BTreeMap<String, String>,
    ) -> Result<&Rule, Refusal> {
        let candidates: Vec<&Rule> = self
            .rules
            .iter()
            .filter(|r| {
                r.action == action
                    && r.to == to
                    && r.actors.contains(&actor)
                    && match from {
                        None => r.from.is_empty(),
                        Some(stage) => r.from.iter().any(|f| f == stage),
                    }
            })
            .collect();
        let mut first_missing = None;
        for rule in &candidates {
            let missing = rule.requires.iter().find_map(|f| blank(fields.get(f)).map(|state| (f.clone(), state)));
            match missing {
                None => return Ok(rule),
                Some(found) => {
                    if first_missing.is_none() {
                        first_missing = Some(found);
                    }
                }
            }
        }
        if let Some((field, state)) = first_missing {
            return Err(Refusal::field(
                "missing_field",
                &field,
                state,
                format!("this action needs `{field}`, which arrived {state}"),
            ));
        }
        Err(Refusal::new(
            "transition_not_allowed",
            format!(
                "{} from {} to {to} is not allowed for {}",
                serde_json::to_value(action).unwrap().as_str().unwrap_or("action"),
                from.unwrap_or("nothing"),
                serde_json::to_value(actor).unwrap().as_str().unwrap_or("this actor")
            ),
        ))
    }
}
```

- [ ] **Step 4: Run the tests**

Add `mod workflow;` to `src/overmind/mod.rs`.
Run: `cargo test --locked overmind::workflow 2>&1 | tail -5`
Expected: 5 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/overmind/workflow.rs src/overmind/mod.rs
git commit --only src/overmind/workflow.rs src/overmind/mod.rs -m "Define Overmind workflows as validated data with the vikunja-mcp default"
```

---

### Task 3: Folding events into task state

**Files:**
- Create: `src/overmind/fold.rs`
- Modify: `src/overmind/mod.rs` (add `mod fold;`)

**Interfaces:**
- Consumes: `model::*`, `workflow::{Action, Role, Workflow, RESUME}`.
- Produces: `fold::{Attachment, JournalEntry, Task, VERDICT_LABELS, fold, fold_many, project_workflow}`:
  - `fold(uuid: &str, events: &[&Event]) -> Option<Task>` — `events` holds the task's events plus shared events (any order).
  - `fold_many(events: &[Event]) -> BTreeMap<String, Task>` — every task found in `events`, deleted ones included.
  - `project_workflow(events: &[&Event], project: &str) -> Workflow` — last valid `workflow_set` for the project, else the default.
  - `Task` fields as listed in the spec plus `head: String`, `rejections: BTreeMap<String, Refusal>` (event id → reason, not serialized).
- Event kinds and payloads (v1), the contract for all later tasks:
  - `task_created` `{title, prompt, stage, priority, rank, labels[], pinned?, parent, follows[], attachments[], fields{}}`
  - `field_set` `{field: "title"|"prompt"|"priority"|"attachments", value}`
  - `rank_set` `{rank}`; `label_set` `{label, present}`; `relation_set` `{relation: "parent"|"follows"|"related", target, present}`
  - `pin_set` `{session: SessionRef|null}`; `reviewer_set` `{session: SessionRef|null}`; `flag_set` `{flag, value}`
  - `attachment_added` `{attachment}`; `journal` `{marker, text, transition?}`; `task_deleted` `{}`
  - `transition` `{action, to, fields{}, session?, children[]}` with `base` = the task's `head`
  - shared: `workflow_set` `{workflow, moves{}}` (task "", project P); `session_renamed` `{machine, old, new}` (task "", project "")

- [ ] **Step 1: Write the failing tests** (bottom of `fold.rs`)

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const A: &str = "11111111-1111-4111-8111-111111111111";
    const B: &str = "22222222-2222-4222-8222-222222222222";
    const T: &str = "33333333-3333-4333-8333-333333333333";

    fn ev(origin: &str, seq: u64, lamport: u64, actor: Actor, kind: &str, base: &str, payload: Value) -> Event {
        Event {
            id: format!("{origin}-{seq}"), origin: origin.into(), seq, lamport, at: lamport, v: 1, actor,
            task: T.into(), project: "p".into(), kind: kind.into(), base: base.into(),
            request_id: String::new(), payload,
        }
    }
    fn human() -> Actor { Actor::human(A) }
    fn agent(name: &str) -> Actor { Actor::agent(A, name) }
    fn session(name: &str) -> Value { json!({"machine": A, "name": name}) }
    fn created(stage: &str) -> Event {
        ev(A, 1, 1, human(), "task_created", "", json!({"title": "Title", "prompt": "Do it", "stage": stage}))
    }
    fn transition(origin: &str, seq: u64, lamport: u64, actor: Actor, base: &str, action: &str, to: &str, fields: Value, session: Value) -> Event {
        ev(origin, seq, lamport, actor, "transition", base, json!({"action": action, "to": to, "fields": fields, "session": session}))
    }
    fn run(events: &[Event]) -> Task {
        let refs: Vec<&Event> = events.iter().collect();
        fold(T, &refs).unwrap()
    }

    #[test]
    fn creation_sets_the_initial_state() {
        let task = run(&[created("Queue")]);
        assert_eq!((task.stage.as_str(), task.title.as_str(), task.prompt.as_str(), task.priority), ("Queue", "Title", "Do it", 2));
        assert_eq!(task.journal[0].marker, "[created]");
        assert_eq!(task.head, format!("{A}-1"));
    }

    #[test]
    fn claim_and_advance_follow_the_gates() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let no_spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({}), Value::Null);
        let spec = transition(A, 4, 4, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "plan"}), Value::Null);
        let task = run(&[c, claim, no_spec.clone(), spec]);
        assert_eq!(task.stage, "Build");
        assert_eq!(task.assignee.as_ref().unwrap().name, "w1");
        assert_eq!(task.rejections[&no_spec.id].field, "spec");
        assert!(task.journal.iter().any(|j| j.marker == "[spec]" && j.text == "plan"));
        assert!(task.journal.iter().any(|j| j.marker == "[rejected]"));
    }

    #[test]
    fn other_sessions_cannot_act_on_the_task() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let intruder = transition(A, 3, 3, agent("w2"), &claim.id, "advance", "Build", json!({"spec": "x"}), Value::Null);
        let task = run(&[c, claim, intruder.clone()]);
        assert_eq!(task.stage, "Design");
        assert_eq!(task.rejections[&intruder.id].code, "not_assigned");
    }

    #[test]
    fn agents_never_reach_done_by_any_action() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let review = transition(A, 4, 4, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        assert_eq!(run(&[c.clone(), claim.clone(), spec.clone(), review.clone()]).stage, "Review");
        for action in Action::ALL {
            let name = serde_json::to_value(action).unwrap();
            let attempt = transition(A, 5, 5, agent("w1"), &review.id, name.as_str().unwrap(), "Done", json!({"report": "r"}), Value::Null);
            let task = run(&[c.clone(), claim.clone(), spec.clone(), review.clone(), attempt]);
            assert_eq!(task.stage, "Review", "{action:?}");
        }
    }

    #[test]
    fn concurrent_claims_resolve_identically_in_any_delivery_order() {
        let c = created("Queue");
        let first = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let second = transition(B, 1, 2, Actor::human(B), &c.id, "claim", "Design", json!({}), json!({"machine": B, "name": "w9"}));
        let label = ev(B, 2, 3, Actor::human(B), "label_set", "", json!({"label": "ui", "present": true}));
        let mut events = vec![c, first, second.clone(), label];
        let expected = serde_json::to_value(run(&events)).unwrap();
        assert_eq!(expected["assignee"]["name"], "w1");
        let mut seed = 7u64;
        for _ in 0..200 {
            for i in (1..events.len()).rev() {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                events.swap(i, (seed % (i as u64 + 1)) as usize);
            }
            assert_eq!(serde_json::to_value(run(&events)).unwrap(), expected);
        }
        assert_eq!(run(&events).rejections[&second.id].code, "stale");
    }

    #[test]
    fn a_sent_prompt_on_a_rejected_claim_is_a_conflict() {
        let c = created("Queue");
        let first = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let second = transition(B, 1, 2, Actor::human(B), &c.id, "claim", "Design", json!({}), json!({"machine": B, "name": "w9"}));
        let sent = ev(B, 2, 3, Actor::human(B), "journal", "", json!({"marker": "[sent]", "text": "w9", "transition": second.id}));
        let task = run(&[c, first, second, sent]);
        assert!(task.flags.contains("conflict"));
        assert!(task.journal.iter().any(|j| j.marker == "[conflict]"));
    }

    #[test]
    fn your_call_returns_to_the_stage_it_left() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let ask = transition(A, 4, 4, agent("w1"), &spec.id, "call_human", "Your Call", json!({"question": "Which API?"}), Value::Null);
        let answer = transition(A, 5, 5, human(), &ask.id, "answer", "", json!({"answer": "REST"}), Value::Null);
        let task = run(&[c, claim, spec, ask, answer]);
        assert_eq!(task.stage, "Build");
        assert_eq!(task.assignee.as_ref().unwrap().name, "w1");
    }

    #[test]
    fn bugs_need_a_root_cause_and_reviews_need_independence() {
        let c = created("Queue");
        let bug = ev(A, 2, 2, human(), "label_set", "", json!({"label": "bug", "present": true}));
        let claim = transition(A, 3, 3, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 4, 4, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let no_cause = transition(A, 5, 5, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let done = transition(A, 6, 6, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e", "root_cause": "r"}), Value::Null);
        let self_review = transition(A, 7, 7, agent("w1"), &done.id, "approve", "Review", json!({"report": "ok"}), Value::Null);
        let reviewer = ev(A, 8, 8, human(), "reviewer_set", "", json!({"session": session("w2")}));
        let review = transition(A, 9, 9, agent("w2"), &done.id, "approve", "Review", json!({"report": "ok"}), Value::Null);
        let task = run(&[c, bug, claim, spec, no_cause.clone(), done, self_review.clone(), reviewer, review]);
        assert_eq!(task.rejections[&no_cause.id].field, "root_cause");
        assert_eq!(task.rejections[&self_review.id].code, "not_reviewer");
        assert!(task.labels.contains("reviewed"));
    }

    #[test]
    fn workflow_change_remaps_removed_stage() {
        let c = created("Queue");
        let mut wf = Workflow::default_v1();
        wf.stages[1].name = "Ready".into();
        for rule in &mut wf.rules {
            for f in &mut rule.from { if f == "Queue" { *f = "Ready".into(); } }
            if rule.to == "Queue" { rule.to = "Ready".into(); }
        }
        let mut set = ev(A, 2, 2, human(), "workflow_set", "", json!({"workflow": wf, "moves": {}}));
        set.task = String::new();
        let task = run(&[c, set]);
        assert_eq!(task.stage, "Ready");
    }

    #[test]
    fn renames_follow_sessions_and_newer_events_make_tasks_read_only() {
        let c = created("Queue");
        let pin = ev(A, 2, 2, human(), "pin_set", "", json!({"session": session("old")}));
        let mut rename = ev(A, 3, 3, human(), "session_renamed", "", json!({"machine": A, "old": "old", "new": "new"}));
        rename.task = String::new();
        rename.project = String::new();
        let mut future = ev(A, 4, 4, human(), "field_set", "", json!({"field": "title", "value": "x"}));
        future.v = 2;
        let task = run(&[c, pin, rename, future]);
        assert_eq!(task.pinned.unwrap().name, "new");
        assert!(task.read_only);
        assert_eq!(task.title, "Title");
    }

    #[test]
    fn fold_many_finds_every_task() {
        let c = created("Queue");
        let mut other = created("Backlog");
        other.task = "44444444-4444-4444-8444-444444444444".into();
        other.id = "other".into();
        other.seq = 2;
        let tasks = fold_many(&[c, other]);
        assert_eq!(tasks.len(), 2);
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo test --locked overmind::fold 2>&1 | tail -5`
Expected: compile errors (`fold` not defined).

- [ ] **Step 3: Implement `fold.rs`** (above the tests)

```rust
//! Folding events into task state. Every machine applies the same events in
//! the same total order and therefore reaches the same state. Transitions are
//! compare-and-set against the task's head and are checked against the
//! workflow in effect at that point; a refused transition stays visible as
//! `[rejected]`.
use super::model::*;
use super::workflow::{Action, Role, Workflow, RESUME};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

pub const VERDICT_LABELS: [&str; 2] = ["reviewed", "review-failed"];

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Attachment {
    pub name: String,
    pub mime: String,
    pub bytes: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct JournalEntry {
    pub marker: String,
    pub text: String,
    pub at: u64,
    pub actor: Actor,
    pub event: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Task {
    pub uuid: String,
    pub project: String,
    pub title: String,
    pub prompt: String,
    pub attachments: Vec<Attachment>,
    pub stage: String,
    pub priority: u8,
    pub rank: f64,
    pub labels: BTreeSet<String>,
    pub pinned: Option<SessionRef>,
    pub assignee: Option<SessionRef>,
    pub reviewer: Option<SessionRef>,
    pub resume_stage: String,
    pub parent: String,
    pub follows: BTreeSet<String>,
    pub related: BTreeSet<String>,
    pub journal: Vec<JournalEntry>,
    pub flags: BTreeSet<String>,
    pub head: String,
    pub deleted: bool,
    pub read_only: bool,
    pub created_at: u64,
    pub updated_at: u64,
    pub created_by: Actor,
    #[serde(skip)]
    pub rejections: BTreeMap<String, Refusal>,
}

#[derive(Deserialize)]
struct Created {
    title: String,
    prompt: String,
    stage: String,
    #[serde(default = "default_priority")]
    priority: u8,
    #[serde(default)]
    rank: f64,
    #[serde(default)]
    labels: BTreeSet<String>,
    #[serde(default)]
    pinned: Option<SessionRef>,
    #[serde(default)]
    parent: String,
    #[serde(default)]
    follows: BTreeSet<String>,
    #[serde(default)]
    attachments: Vec<Attachment>,
    #[serde(default)]
    fields: BTreeMap<String, String>,
}

fn default_priority() -> u8 {
    2
}

#[derive(Deserialize)]
struct Transition {
    action: Action,
    #[serde(default)]
    to: String,
    #[serde(default)]
    fields: BTreeMap<String, String>,
    #[serde(default)]
    session: Option<SessionRef>,
    #[serde(default)]
    children: Vec<String>,
}

#[derive(Deserialize)]
struct WorkflowSet {
    workflow: Workflow,
    #[serde(default)]
    moves: BTreeMap<String, String>,
}

fn parse_workflow(event: &Event) -> Option<WorkflowSet> {
    let set: WorkflowSet = serde_json::from_value(event.payload.clone()).ok()?;
    set.workflow.validate().ok()?;
    Some(set)
}

/// The project's workflow after all `workflow_set` events in `events`.
pub fn project_workflow(events: &[&Event], project: &str) -> Workflow {
    let mut sorted: Vec<&&Event> = events
        .iter()
        .filter(|e| e.task.is_empty() && e.kind == "workflow_set" && e.project == project && e.v <= EVENT_VERSION)
        .collect();
    sorted.sort_by(|a, b| a.order().cmp(&b.order()));
    sorted
        .iter()
        .rev()
        .find_map(|e| parse_workflow(e))
        .map(|set| set.workflow)
        .unwrap_or_else(Workflow::default_v1)
}

impl Task {
    fn journal(&mut self, marker: &str, text: impl Into<String>, event: &Event) {
        self.journal.push(JournalEntry {
            marker: marker.into(),
            text: text.into(),
            at: event.at,
            actor: event.actor.clone(),
            event: event.id.clone(),
        });
    }

    fn clear_verdicts(&mut self) {
        for label in VERDICT_LABELS {
            self.labels.remove(label);
        }
    }
}

fn field(fields: &BTreeMap<String, String>, name: &str) -> String {
    fields.get(name).map(|v| v.trim().to_owned()).unwrap_or_default()
}

fn create(uuid: &str, event: &Event, wf: &Workflow) -> Option<Task> {
    let payload: Created = serde_json::from_value(event.payload.clone()).ok()?;
    let mut task = Task {
        uuid: uuid.into(),
        project: event.project.clone(),
        title: payload.title,
        prompt: payload.prompt,
        attachments: payload.attachments,
        stage: payload.stage.clone(),
        priority: payload.priority.min(4),
        rank: payload.rank,
        labels: payload.labels,
        pinned: payload.pinned,
        assignee: None,
        reviewer: None,
        resume_stage: String::new(),
        parent: payload.parent,
        follows: payload.follows,
        related: BTreeSet::new(),
        journal: Vec::new(),
        flags: BTreeSet::new(),
        head: event.id.clone(),
        deleted: false,
        read_only: false,
        created_at: event.at,
        updated_at: event.at,
        created_by: event.actor.clone(),
        rejections: BTreeMap::new(),
    };
    match wf.allow(Action::Create, None, &payload.stage, event.actor.kind, &payload.fields) {
        Ok(_) => {
            let marker = if event.actor.kind == ActorKind::Agent && !payload.fields.contains_key("decomposed_from") {
                "[filed-by-agent]"
            } else {
                "[created]"
            };
            let text = payload.fields.get("breaks").cloned().unwrap_or_default();
            task.journal(marker, text, event);
        }
        Err(refusal) => {
            task.stage = wf.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
            task.journal("[rejected]", refusal.message.clone(), event);
            task.rejections.insert(event.id.clone(), refusal);
        }
    }
    Some(task)
}

fn transition(task: &mut Task, event: &Event, wf: &Workflow) -> Result<(), Refusal> {
    let p: Transition = serde_json::from_value(event.payload.clone())
        .map_err(|e| Refusal::new("invalid_event", e.to_string()))?;
    if event.base != task.head {
        return Err(Refusal::new("stale", "the task changed since this action was prepared"));
    }
    let actor = event.actor.kind;
    let from = task.stage.clone();
    let from_role = wf.role(&from).unwrap_or(Role::Backlog);
    if matches!(from_role, Role::Done | Role::Parked) && actor != ActorKind::Human {
        return Err(Refusal::new("task_closed", "this task is done or parked; only a person can change it"));
    }
    let target = if p.action == Action::Answer { RESUME.to_owned() } else { p.to.clone() };
    let rule = wf.allow(p.action, Some(&from), &target, actor, &p.fields)?;
    let own = rule.own;
    let to = if p.action == Action::Answer { task.resume_stage.clone() } else { p.to.clone() };
    let to_role = wf
        .role(&to)
        .ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {to}")))?;
    if to_role == Role::Done && actor != ActorKind::Human {
        return Err(Refusal::new("done_is_human_only", "only a person can move a task to Done"));
    }
    if own && event.actor.session_ref() != task.assignee {
        return Err(Refusal::new("not_assigned", "this task is not assigned to your session"));
    }
    let text = |name: &str| field(&p.fields, name);
    match p.action {
        Action::Create => return Err(Refusal::new("invalid_event", "tasks are created by task_created")),
        Action::Claim => {
            if task.labels.contains("epic") {
                return Err(Refusal::new("epic", "an epic is a container and is never sent to a session"));
            }
            let session = p
                .session
                .clone()
                .ok_or_else(|| Refusal::field("missing_field", "session", "missing", "choose a session"))?;
            task.assignee = Some(session.clone());
            task.clear_verdicts();
            task.journal("[claim]", session.name, event);
        }
        Action::Unclaim => {
            task.assignee = None;
            task.journal("[unclaim]", text("reason"), event);
        }
        Action::Advance => {
            if to_role == Role::Review {
                if task.labels.contains("bug") && text("root_cause").is_empty() {
                    return Err(Refusal::field(
                        "missing_field",
                        "root_cause",
                        "missing",
                        "a bug needs `root_cause` before review",
                    ));
                }
                let mut body = format!("Worklog: {}\nEvidence: {}", text("worklog"), text("evidence"));
                if !text("root_cause").is_empty() {
                    body = format!("Root cause: {}\n{body}", text("root_cause"));
                }
                task.journal("[worklog]", body, event);
            } else {
                task.clear_verdicts();
                task.journal("[spec]", text("spec"), event);
            }
        }
        Action::CallHuman => {
            task.resume_stage = from.clone();
            task.journal("[needs-human]", text("question"), event);
        }
        Action::Answer => {
            if task.resume_stage.is_empty() {
                return Err(Refusal::new("no_question", "this task is not waiting for an answer"));
            }
            task.resume_stage.clear();
            task.journal("[answer]", text("answer"), event);
        }
        Action::Return => {
            task.assignee = None;
            task.labels.insert("blocked".into());
            task.clear_verdicts();
            task.journal("[blocked]", text("reason"), event);
        }
        Action::Decompose => {
            if p.children.len() < 2 {
                return Err(Refusal::new("too_few_subtasks", "decompose into at least two subtasks"));
            }
            task.labels.insert("epic".into());
            task.assignee = None;
            task.clear_verdicts();
            task.journal("[decompose]", p.children.join(" "), event);
        }
        Action::Approve | Action::NeedsWork => {
            if actor == ActorKind::Agent {
                let me = event.actor.session_ref();
                if me.is_none() || me != task.reviewer || me == task.assignee {
                    return Err(Refusal::new("not_reviewer", "only the session this task was sent to for review can review it"));
                }
            }
            task.clear_verdicts();
            let (label, verdict) = if p.action == Action::Approve {
                ("reviewed", "APPROVE")
            } else {
                ("review-failed", "NEEDS WORK")
            };
            task.labels.insert(label.into());
            task.journal("[review]", format!("{verdict}\n{}", text("report")), event);
        }
        Action::Done => task.journal("[done]", "", event),
        Action::Reopen => {
            task.clear_verdicts();
            task.journal("[moved]", format!("{from} -> {to}"), event);
        }
        Action::Move => task.journal("[moved]", format!("{from} -> {to}"), event),
    }
    task.stage = to;
    task.head = event.id.clone();
    Ok(())
}

fn rename(slot: &mut Option<SessionRef>, machine: &str, old: &str, new: &str) {
    if let Some(session) = slot {
        if session.machine == machine && session.name == old {
            session.name = new.into();
        }
    }
}

fn remap(stage: &str, old: &Workflow, new: &Workflow, moves: &BTreeMap<String, String>) -> String {
    if new.stage(stage).is_some() {
        return stage.into();
    }
    if let Some(target) = moves.get(stage).filter(|t| new.stage(t).is_some()) {
        return target.clone();
    }
    old.role(stage)
        .and_then(|role| new.first(role))
        .or_else(|| new.first(Role::Backlog))
        .map(|s| s.name.clone())
        .unwrap_or_default()
}

fn apply(task: &mut Task, event: &Event, wf: &mut Workflow) {
    if event.v > EVENT_VERSION {
        task.read_only = true;
        return;
    }
    let p = &event.payload;
    if event.task.is_empty() {
        match event.kind.as_str() {
            "workflow_set" if event.project == task.project => {
                if let Some(set) = parse_workflow(event) {
                    task.stage = remap(&task.stage, wf, &set.workflow, &set.moves);
                    if !task.resume_stage.is_empty() {
                        task.resume_stage = remap(&task.resume_stage, wf, &set.workflow, &set.moves);
                    }
                    *wf = set.workflow;
                }
            }
            "session_renamed" => {
                let s = |k: &str| p.get(k).and_then(Value::as_str).unwrap_or_default().to_owned();
                let (machine, old, new) = (s("machine"), s("old"), s("new"));
                rename(&mut task.pinned, &machine, &old, &new);
                rename(&mut task.assignee, &machine, &old, &new);
                rename(&mut task.reviewer, &machine, &old, &new);
            }
            _ => {}
        }
        return;
    }
    task.updated_at = task.updated_at.max(event.at);
    let text = |k: &str| p.get(k).and_then(Value::as_str).unwrap_or_default().to_owned();
    let session = |k: &str| serde_json::from_value::<Option<SessionRef>>(p.get(k).cloned().unwrap_or(Value::Null)).ok().flatten();
    match event.kind.as_str() {
        "task_created" => {}
        "field_set" => {
            let value = p.get("value").cloned().unwrap_or(Value::Null);
            match text("field").as_str() {
                "title" => {
                    if let Some(v) = value.as_str() {
                        task.title = v.into();
                    }
                }
                "prompt" => {
                    if let Some(v) = value.as_str() {
                        task.prompt = v.into();
                    }
                }
                "priority" => {
                    if let Some(v) = value.as_u64() {
                        task.priority = v.min(4) as u8;
                    }
                }
                "attachments" => {
                    if let Ok(v) = serde_json::from_value(value) {
                        task.attachments = v;
                    }
                }
                _ => task.read_only = true,
            }
            task.journal("[edited]", text("field"), event);
        }
        "rank_set" => {
            if let Some(rank) = p.get("rank").and_then(Value::as_f64) {
                task.rank = rank;
            }
        }
        "label_set" => {
            let label = text("label");
            if p.get("present").and_then(Value::as_bool).unwrap_or(false) {
                task.labels.insert(label);
            } else {
                task.labels.remove(&label);
            }
        }
        "relation_set" => {
            let target = text("target");
            let present = p.get("present").and_then(Value::as_bool).unwrap_or(false);
            match text("relation").as_str() {
                "parent" => task.parent = if present { target } else { String::new() },
                "follows" => {
                    if present {
                        task.follows.insert(target);
                    } else {
                        task.follows.remove(&target);
                    }
                }
                "related" => {
                    if present {
                        task.related.insert(target);
                    } else {
                        task.related.remove(&target);
                    }
                }
                _ => task.read_only = true,
            }
        }
        "pin_set" => task.pinned = session("session"),
        "reviewer_set" => task.reviewer = session("session"),
        "flag_set" => {
            let flag = text("flag");
            if p.get("value").and_then(Value::as_bool).unwrap_or(false) {
                task.flags.insert(flag);
            } else {
                task.flags.remove(&flag);
            }
        }
        "attachment_added" => {
            if let Ok(attachment) = serde_json::from_value::<Attachment>(p.get("attachment").cloned().unwrap_or(Value::Null)) {
                task.attachments.push(attachment);
            }
        }
        "journal" => {
            let mut entry_text = text("text");
            if let Some(transition) = p.get("transition").and_then(Value::as_str) {
                if !transition.is_empty() {
                    entry_text = format!("{entry_text}\ntransition: {transition}");
                }
            }
            task.journal(&text("marker"), entry_text, event);
        }
        "task_deleted" => task.deleted = true,
        "transition" => {
            if let Err(refusal) = transition(task, event, wf) {
                task.journal("[rejected]", refusal.message.clone(), event);
                task.rejections.insert(event.id.clone(), refusal);
            }
        }
        _ => task.read_only = true,
    }
}

/// Folds one task from its events plus shared events, in any order.
pub fn fold(uuid: &str, events: &[&Event]) -> Option<Task> {
    let mut sorted: Vec<&Event> = events
        .iter()
        .copied()
        .filter(|e| e.task == uuid || (e.task.is_empty() && matches!(e.kind.as_str(), "workflow_set" | "session_renamed")))
        .collect();
    sorted.sort_by(|a, b| a.order().cmp(&b.order()));
    let created_at = sorted.iter().position(|e| e.task == uuid && e.kind == "task_created")?;
    let created = sorted[created_at];
    let project = created.project.clone();
    let earlier: Vec<&Event> = sorted[..created_at].to_vec();
    let mut wf = project_workflow(&earlier, &project);
    let mut task = create(uuid, created, &wf)?;
    for event in &sorted[created_at + 1..] {
        if event.task.is_empty() && event.kind == "workflow_set" && event.project != project {
            continue;
        }
        apply(&mut task, event, &mut wf);
    }
    // A prompt delivered on a claim that lost the race cannot be recalled.
    let sent_on_rejected: Vec<(String, Event)> = sorted
        .iter()
        .filter(|e| e.task == uuid && e.kind == "journal")
        .filter(|e| matches!(e.payload.get("marker").and_then(Value::as_str), Some("[sent]") | Some("[sent?]")))
        .filter_map(|e| {
            let claim = e.payload.get("transition").and_then(Value::as_str)?;
            task.rejections.contains_key(claim).then(|| (claim.to_owned(), (*e).clone()))
        })
        .collect();
    for (claim, event) in sent_on_rejected {
        task.flags.insert("conflict".into());
        task.journal("[conflict]", format!("a prompt was sent on claim {claim}, which lost to another claim"), &event);
    }
    Some(task)
}

/// Every task found in `events`, keyed by UUID; deleted tasks are included.
pub fn fold_many(events: &[Event]) -> BTreeMap<String, Task> {
    let shared: Vec<&Event> = events.iter().filter(|e| e.task.is_empty()).collect();
    let mut by_task: BTreeMap<&str, Vec<&Event>> = BTreeMap::new();
    for event in events.iter().filter(|e| !e.task.is_empty()) {
        by_task.entry(event.task.as_str()).or_default().push(event);
    }
    by_task
        .into_iter()
        .filter_map(|(uuid, mut own)| {
            own.extend(shared.iter().copied());
            fold(uuid, &own).map(|task| (uuid.to_owned(), task))
        })
        .collect()
}
```

- [ ] **Step 4: Run the tests**

Add `mod fold;` to `src/overmind/mod.rs`.
Run: `cargo test --locked overmind::fold 2>&1 | tail -5`
Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/overmind/fold.rs src/overmind/mod.rs
git commit --only src/overmind/fold.rs src/overmind/mod.rs -m "Fold Overmind events into task state deterministically with gated transitions"
```

---

### Task 4: Short task references

**Files:**
- Create: `src/overmind/refs.rs`
- Modify: `src/overmind/mod.rs` (add `mod refs;`)

**Interfaces:**
- Consumes: `model::Refusal`.
- Produces: `refs::{MIN, digest(&str) -> String, refs(&[String]) -> BTreeMap<String, String>, resolve(&str, &[String]) -> Result<String, Refusal>}`.

- [ ] **Step 1: Write the failing tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    fn ids(n: usize) -> Vec<String> {
        (0..n).map(|i| format!("00000000-0000-4000-8000-{i:012}")).collect()
    }

    #[test]
    fn digests_are_stable_base32() {
        let d = digest("x");
        assert_eq!(d.len(), 52);
        assert!(d.bytes().all(|b| ALPHABET.contains(&b)));
        assert_eq!(d, digest("x"));
    }

    #[test]
    fn refs_are_unique_and_at_least_five_characters() {
        let all = ids(2000);
        let map = refs(&all);
        let values: std::collections::BTreeSet<_> = map.values().collect();
        assert_eq!(values.len(), all.len());
        assert!(map.values().all(|r| r.starts_with("om-") && r.len() >= 3 + MIN));
    }

    #[test]
    fn prefixes_grow_only_where_needed() {
        let lengths = shortest(&["aaaaab".into(), "aaaaac".into(), "zzzzzz".into()]);
        assert_eq!(lengths, vec![6, 6, 5]);
    }

    #[test]
    fn resolve_accepts_uuids_and_unambiguous_prefixes() {
        let all = ids(100);
        let map = refs(&all);
        assert_eq!(resolve(&all[3], &all).unwrap(), all[3]);
        assert_eq!(resolve(&map[&all[3]], &all).unwrap(), all[3]);
        assert_eq!(resolve(&format!(" {} ", map[&all[3]]), &all).unwrap(), all[3]);
        let first = &digest(&all[0])[..1];
        let error = resolve(&format!("om-{first}"), &all).unwrap_err();
        assert_eq!(error.code, "ambiguous");
        assert_eq!(resolve("om-zzzzzzzz9", &all).unwrap_err().code, "unknown_task");
        assert_eq!(resolve("nonsense", &all).unwrap_err().code, "unknown_task");
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo test --locked overmind::refs 2>&1 | tail -5`
Expected: compile errors.

- [ ] **Step 3: Implement `refs.rs`**

```rust
//! Short task references: `om-` plus the shortest unambiguous prefix, at least
//! five characters, of a base32 digest of the task UUID. A reference can grow
//! as tasks arrive; an older, shorter one then resolves as ambiguous.
use super::model::Refusal;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

const ALPHABET: &[u8; 32] = b"abcdefghijklmnopqrstuvwxyz234567";
pub const MIN: usize = 5;

pub fn digest(uuid: &str) -> String {
    let mut out = String::new();
    let mut buffer: u32 = 0;
    let mut bits = 0;
    for byte in Sha256::digest(uuid.as_bytes()) {
        buffer = (buffer << 8) | byte as u32;
        bits += 8;
        while bits >= 5 {
            bits -= 5;
            out.push(ALPHABET[((buffer >> bits) & 31) as usize] as char);
        }
    }
    if bits > 0 {
        out.push(ALPHABET[((buffer << (5 - bits)) & 31) as usize] as char);
    }
    out
}

fn common(a: &str, b: &str) -> usize {
    a.bytes().zip(b.bytes()).take_while(|(x, y)| x == y).count()
}

/// Prefix lengths that keep each digest distinct from all others.
fn shortest(digests: &[String]) -> Vec<usize> {
    let mut sorted: Vec<&String> = digests.iter().collect();
    sorted.sort();
    digests
        .iter()
        .map(|d| {
            let i = sorted.binary_search(&d).expect("digest is present");
            let mut need = MIN;
            if i > 0 {
                need = need.max(common(d, sorted[i - 1]) + 1);
            }
            if i + 1 < sorted.len() {
                need = need.max(common(d, sorted[i + 1]) + 1);
            }
            need.min(d.len())
        })
        .collect()
}

pub fn refs(uuids: &[String]) -> BTreeMap<String, String> {
    let digests: Vec<String> = uuids.iter().map(|u| digest(u)).collect();
    shortest(&digests)
        .into_iter()
        .zip(uuids.iter().zip(digests.iter()))
        .map(|(n, (uuid, d))| (uuid.clone(), format!("om-{}", &d[..n])))
        .collect()
}

pub fn resolve(input: &str, uuids: &[String]) -> Result<String, Refusal> {
    let input = input.trim();
    if let Some(found) = uuids.iter().find(|u| u.as_str() == input) {
        return Ok(found.clone());
    }
    let unknown = || Refusal::new("unknown_task", format!("no task {input}"));
    let prefix = input.strip_prefix("om-").filter(|p| !p.is_empty()).ok_or_else(unknown)?;
    let matches: Vec<&String> = uuids.iter().filter(|u| digest(u).starts_with(prefix)).collect();
    match matches.len() {
        0 => Err(unknown()),
        1 => Ok(matches[0].clone()),
        _ => {
            let all = refs(uuids);
            let names: Vec<&str> = matches.iter().take(10).map(|u| all[*u].as_str()).collect();
            Err(Refusal::new("ambiguous", format!("{input} matches several tasks: {}", names.join(", "))))
        }
    }
}
```

- [ ] **Step 4: Run the tests**

Add `mod refs;` to `src/overmind/mod.rs`.
Run: `cargo test --locked overmind::refs 2>&1 | tail -5`
Expected: 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/overmind/refs.rs src/overmind/mod.rs
git commit --only src/overmind/refs.rs src/overmind/mod.rs -m "Give Overmind tasks short unambiguous references"
```

---

### Task 5: Human commands and the `hgs overmind` CLI

**Files:**
- Create: `src/overmind/commands.rs`, `src/overmind/cli.rs`, `tests/test_overmind.py`
- Modify: `src/overmind/mod.rs` (add `mod commands; mod cli; pub use cli::dispatch;`), `src/cli.rs` (route `overmind`, one usage line)

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces:
  - `commands::Ctx { store: &mut Store, actor: Actor, cwd: Option<PathBuf> }`
  - `commands::{rev, board, show, create, edit, move_task, done, reopen, reorder, pin, label, link, comment, delete, workflow_get, workflow_set}`, each `(&mut Ctx, Value) -> anyhow::Result<Value>` (`rev` takes only `&mut Ctx`).
  - Shared helpers used by Task 6: `parse<T>(&Value)`, `write(&mut Ctx, &Value, FnOnce(&mut Tx, &Actor) -> Result<Value>)`, `load(&Tx, &str) -> Result<Task>`, `resolve(&Tx, &str) -> Result<String>`, `workflow(&Tx, &str) -> Result<Workflow>`, `prove(&mut Tx, Draft) -> Result<Event>`, `transition(&mut Tx, &Actor, &Task, Action, &str, BTreeMap<String,String>, Option<SessionRef>, Vec<String>, &str) -> Result<Event>`, `view(&Task, &str, bool) -> Value`, `reference(&Tx, &str) -> Result<String>`, `store_attachments(&Store, &[NewAttachment]) -> Result<Vec<Attachment>>`, `check_text(&str, &str, usize, bool) -> Result<(), Refusal>`, `created_payload(...)`, `next_rank(&Tx, &str, &str) -> Result<f64>`, `NewAttachment`, `human_only(&Actor) -> Result<()>`, constants `MAX_PROMPT`, `MAX_TITLE`, `MAX_FIELD`.
  - `overmind::dispatch(&Config, &[String], bool) -> cli::Result<i32>`.
- CLI contract: `hgs overmind <command> [args] [--json]`; with `--json` the request is JSON on stdin. Success prints `{"ok": true, ...}` and exits 0. A refusal prints `{"ok": false, "error": {code, message, field?, state?}}` on stdout, `hgs: <message>` on stderr, exits 1.

- [ ] **Step 1: Write the failing Rust tests** (bottom of `commands.rs`)

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn store() -> (tempfile::TempDir, Store) {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::open(dir.path(), MACHINE).unwrap();
        (dir, store)
    }
    fn human(store: &mut Store) -> Ctx<'_> {
        Ctx { store, actor: Actor::human(MACHINE), cwd: None }
    }
    fn new_task(store: &mut Store, stage: &str) -> String {
        let result = create(&mut human(store), json!({"project": "p", "prompt": "Fix the drafts\nDetails", "stage": stage})).unwrap();
        result["task"]["uuid"].as_str().unwrap().to_owned()
    }
    fn refusal(error: anyhow::Error) -> Refusal {
        error.downcast::<Refusal>().unwrap()
    }

    #[test]
    fn create_defaults_title_stage_priority_and_rank() {
        let (_dir, mut store) = store();
        let first = create(&mut human(&mut store), json!({"project": "p", "prompt": "\n  Fix the drafts  \nmore"})).unwrap();
        assert_eq!(first["task"]["title"], "Fix the drafts");
        assert_eq!(first["task"]["stage"], "Backlog");
        assert_eq!(first["task"]["priority"], 2);
        assert!(first["task"]["ref"].as_str().unwrap().starts_with("om-"));
        let second = create(&mut human(&mut store), json!({"project": "p", "prompt": "Second"})).unwrap();
        assert!(second["task"]["rank"].as_f64().unwrap() > first["task"]["rank"].as_f64().unwrap());
    }

    #[test]
    fn create_requires_a_project_or_a_pin() {
        let (_dir, mut store) = store();
        let error = refusal(create(&mut human(&mut store), json!({"prompt": "x"})).unwrap_err());
        assert_eq!(error.field, "project");
        let pinned = create(&mut human(&mut store), json!({"prompt": "x", "stage": "Queue", "pinned": {"machine": "", "name": "claude/zerus/w1"}})).unwrap();
        assert_eq!(pinned["task"]["pinned"]["machine"], MACHINE);
    }

    #[test]
    fn limits_count_bytes_for_prompts_and_characters_for_titles() {
        let (_dir, mut store) = store();
        let exact = "ж".repeat(MAX_PROMPT / 2);
        assert_eq!(exact.len(), MAX_PROMPT);
        assert!(create(&mut human(&mut store), json!({"project": "p", "prompt": exact})).is_ok());
        let over = format!("{exact}x");
        assert_eq!(refusal(create(&mut human(&mut store), json!({"project": "p", "prompt": over})).unwrap_err()).field, "prompt");
        let title = "я".repeat(MAX_TITLE);
        assert!(create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "title": title})).is_ok());
        let long = "я".repeat(MAX_TITLE + 1);
        assert_eq!(refusal(create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "title": long})).unwrap_err()).field, "title");
        let control = create(&mut human(&mut store), json!({"project": "p", "prompt": "a\u{0007}b"})).unwrap_err();
        assert_eq!(refusal(control).state, "invalid");
    }

    #[test]
    fn receipts_replay_and_refuse_reuse() {
        let (_dir, mut store) = store();
        let id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        let request = json!({"request_id": id, "project": "p", "prompt": "x"});
        let first = create(&mut human(&mut store), request.clone()).unwrap();
        assert_eq!(first["task"]["uuid"], id, "a request UUID becomes the task UUID");
        edit(&mut human(&mut store), json!({"task": id, "title": "Changed"})).unwrap();
        assert_eq!(create(&mut human(&mut store), request).unwrap(), first);
        let reused = create(&mut human(&mut store), json!({"request_id": id, "project": "p", "prompt": "y"})).unwrap_err();
        assert_eq!(refusal(reused).code, "request_reused");
        assert_eq!(store.task_ids().unwrap().len(), 1);
    }

    #[test]
    fn moves_choose_the_action_and_respect_gates() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Backlog");
        let moved = move_task(&mut human(&mut store), json!({"task": task, "to": "Queue"})).unwrap();
        assert_eq!(moved["task"]["stage"], "Queue");
        let start = refusal(move_task(&mut human(&mut store), json!({"task": task, "to": "Design"})).unwrap_err());
        assert_eq!(start.code, "use_send");
        let skip = refusal(done(&mut human(&mut store), json!({"task": task})).unwrap_err());
        assert_eq!(skip.code, "transition_not_allowed");
        let iced = move_task(&mut human(&mut store), json!({"task": task, "to": "Icebox"})).unwrap();
        assert_eq!(iced["task"]["stage"], "Icebox");
    }

    #[test]
    fn agents_cannot_run_human_commands() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        let mut ctx = Ctx { store: &mut store, actor: Actor::agent(MACHINE, "w1"), cwd: None };
        for result in [
            create(&mut ctx, json!({"project": "p", "prompt": "x"})),
            edit(&mut ctx, json!({"task": task, "title": "x"})),
            move_task(&mut ctx, json!({"task": task, "to": "Backlog"})),
            done(&mut ctx, json!({"task": task})),
            delete(&mut ctx, json!({"task": task})),
        ] {
            assert_eq!(refusal(result.unwrap_err()).code, "human_only");
        }
    }

    #[test]
    fn board_lists_tasks_in_stage_and_rank_order_and_since_a_cursor() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        reorder(&mut human(&mut store), json!({"task": b, "before": a})).unwrap();
        let board_all = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        let order: Vec<&str> = board_all["tasks"].as_array().unwrap().iter().map(|t| t["uuid"].as_str().unwrap()).collect();
        assert_eq!(order, vec![b.as_str(), a.as_str()]);
        let cursor = board_all["cursor"].as_i64().unwrap();
        label(&mut human(&mut store), json!({"task": a, "label": "ui", "present": true})).unwrap();
        let delta = board(&mut human(&mut store), json!({"project": "p", "since": cursor})).unwrap();
        assert_eq!(delta["tasks"].as_array().unwrap().len(), 1);
        assert_eq!(delta["tasks"][0]["labels"], json!(["ui"]));
    }

    #[test]
    fn show_comment_pin_link_and_delete() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        comment(&mut human(&mut store), json!({"task": a, "text": "note"})).unwrap();
        pin(&mut human(&mut store), json!({"task": a, "session": {"machine": "", "name": "w1"}})).unwrap();
        link(&mut human(&mut store), json!({"task": a, "relation": "follows", "target": b, "present": true})).unwrap();
        let shown = show(&mut human(&mut store), json!({"task": a})).unwrap();
        assert_eq!(shown["task"]["pinned"]["name"], "w1");
        assert_eq!(shown["task"]["follows"], json!([b]));
        assert!(shown["task"]["journal"].as_array().unwrap().iter().any(|j| j["marker"] == "[comment]"));
        assert_eq!(refusal(link(&mut human(&mut store), json!({"task": a, "relation": "follows", "target": a, "present": true})).unwrap_err()).code, "invalid_field");
        assert_eq!(refusal(label(&mut human(&mut store), json!({"task": a, "label": "reviewed", "present": true})).unwrap_err()).code, "reserved_label");
        delete(&mut human(&mut store), json!({"task": a})).unwrap();
        assert_eq!(refusal(show(&mut human(&mut store), json!({"task": a})).unwrap_err()).code, "unknown_task");
    }

    #[test]
    fn attachments_are_stored_by_digest_within_limits() {
        let (_dir, mut store) = store();
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let result = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.png", "mime": "image/png", "data_base64": png}]})).unwrap();
        let sha = result["task"]["attachments"][0]["sha256"].as_str().unwrap().to_owned();
        assert!(blobs::path(store.dir(), &sha).unwrap().exists());
        let text = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.txt", "mime": "text/plain", "data_base64": png}]})).unwrap_err();
        assert_eq!(refusal(text).field, "attachments");
    }

    #[test]
    fn workflow_set_requires_moves_for_occupied_stages() {
        let (_dir, mut store) = store();
        new_task(&mut store, "Queue");
        let mut wf = Workflow::default_v1();
        wf.stages[1].name = "Ready".into();
        for rule in &mut wf.rules {
            for f in &mut rule.from { if f == "Queue" { *f = "Ready".into(); } }
            if rule.to == "Queue" { rule.to = "Ready".into(); }
        }
        let refused = workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": wf})).unwrap_err();
        assert_eq!(refusal(refused).code, "moves_required");
        workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": wf, "moves": {"Queue": "Ready"}})).unwrap();
        let board_now = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert_eq!(board_now["tasks"][0]["stage"], "Ready");
        assert_eq!(workflow_get(&mut human(&mut store), json!({"project": "p"})).unwrap()["workflow"]["stages"][1]["name"], "Ready");
        let invalid = workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": {"version": 2}})).unwrap_err();
        assert_eq!(refusal(invalid).code, "invalid_workflow");
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo test --locked overmind::commands 2>&1 | tail -5`
Expected: compile errors.

- [ ] **Step 3: Implement `commands.rs`** (above the tests)

```rust
//! Overmind commands. A write runs in one transaction: replay its receipt,
//! fold the current state, prepare events, prove each by folding, append.
use super::blobs;
use super::fold::{fold, fold_many, project_workflow, Attachment, Task};
use super::model::*;
use super::refs;
use super::store::{Filter, Store, Tx};
use super::workflow::{Action, Role, Workflow};
use anyhow::Result;
use base64::Engine;
use serde::de::DeserializeOwned;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

pub const MAX_PROMPT: usize = 64 * 1024;
pub const MAX_TITLE: usize = 200;
pub const MAX_FIELD: usize = 64 * 1024;
const MAX_ATTACHMENTS: usize = 8;
const MAX_ATTACHMENT: usize = 10 * 1024 * 1024;
const MAX_ATTACHMENTS_TOTAL: usize = 20 * 1024 * 1024;
const RESERVED_LABELS: [&str; 4] = ["reviewed", "review-failed", "epic", "epic-ready"];

pub struct Ctx<'a> {
    pub store: &'a mut Store,
    pub actor: Actor,
    /// Working directory of the calling agent session, for evidence checks.
    pub cwd: Option<PathBuf>,
}

pub fn refuse<T>(code: &str, message: impl Into<String>) -> Result<T> {
    Err(Refusal::new(code, message).into())
}

pub fn parse<T: DeserializeOwned>(request: &Value) -> Result<T> {
    serde_json::from_value(request.clone())
        .map_err(|e| Refusal::new("invalid_request", format!("invalid request: {e}")).into())
}

pub fn human_only(actor: &Actor) -> Result<()> {
    if actor.kind != ActorKind::Human {
        return refuse("human_only", "only a person can do this");
    }
    Ok(())
}

pub fn check_text(name: &str, value: &str, max: usize, multiline: bool) -> Result<(), Refusal> {
    if value.len() > max {
        return Err(Refusal::field("too_long", name, "invalid", format!("`{name}` is longer than {max} bytes")));
    }
    let allowed = |c: char| !c.is_control() || (multiline && (c == '\n' || c == '\t'));
    if !value.chars().all(allowed) {
        return Err(Refusal::field("invalid_field", name, "invalid", format!("`{name}` contains control characters")));
    }
    Ok(())
}

fn check_label(label: &str) -> Result<(), Refusal> {
    let mut chars = label.chars();
    let first = chars.next().is_some_and(|c| c.is_ascii_lowercase() || c.is_ascii_digit());
    let rest = chars.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || ":_./-".contains(c));
    if !first || !rest || label.len() > 64 {
        return Err(Refusal::field("invalid_field", "labels", "invalid", format!("invalid label {label:?}")));
    }
    Ok(())
}

fn check_project(project: &str) -> Result<(), Refusal> {
    if project.len() > 128 || project.chars().any(char::is_control) {
        return Err(Refusal::field("invalid_field", "project", "invalid", "invalid project id"));
    }
    Ok(())
}

fn request_digest(request: &Value) -> String {
    format!("{:x}", Sha256::digest(request.to_string().as_bytes()))
}

/// Runs a write with receipt replay when the request carries a `request_id`.
pub fn write(ctx: &mut Ctx, request: &Value, f: impl FnOnce(&mut Tx, &Actor) -> Result<Value>) -> Result<Value> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default().to_owned();
    if !request_id.is_empty() && uuid::Uuid::parse_str(&request_id).is_err() {
        return refuse("invalid_request", "request_id must be a UUID");
    }
    let digest = request_digest(request);
    let actor = ctx.actor.clone();
    ctx.store.write(|tx| {
        if !request_id.is_empty() {
            if let Some((stored, result)) = tx.receipt(&request_id)? {
                if stored != digest {
                    return refuse("request_reused", "this request_id was already used for a different request");
                }
                return Ok(result);
            }
        }
        let result = f(tx, &actor)?;
        if !request_id.is_empty() {
            tx.put_receipt(&request_id, &digest, &result)?;
        }
        Ok(result)
    })
}

pub fn resolve(tx: &Tx, input: &str) -> Result<String> {
    Ok(refs::resolve(input, &tx.task_ids()?)?)
}

pub fn reference(tx: &Tx, uuid: &str) -> Result<String> {
    Ok(refs::refs(&tx.task_ids()?).remove(uuid).unwrap_or_default())
}

pub fn workflow(tx: &Tx, project: &str) -> Result<Workflow> {
    let shared = tx.load(&Filter::Shared(project.into()))?;
    let refs: Vec<&Event> = shared.iter().collect();
    Ok(project_workflow(&refs, project))
}

fn context(tx: &Tx, uuid: &str, extra: Option<&Event>) -> Result<Option<Task>> {
    let mut events = tx.load(&Filter::Task(uuid.into()))?;
    let project = match events.iter().find(|e| e.kind == "task_created").or(extra) {
        Some(created) => created.project.clone(),
        None => return Ok(None),
    };
    events.extend(tx.load(&Filter::Shared(project))?);
    if let Some(extra) = extra {
        events.push(extra.clone());
    }
    let refs: Vec<&Event> = events.iter().collect();
    Ok(fold(uuid, &refs))
}

pub fn load(tx: &Tx, uuid: &str) -> Result<Task> {
    match context(tx, uuid, None)? {
        Some(task) if !task.deleted => Ok(task),
        _ => refuse("unknown_task", "no such task"),
    }
}

/// Appends `draft` only if folding accepts it.
pub fn prove(tx: &mut Tx, draft: Draft) -> Result<Event> {
    let event = tx.preview(&draft)?;
    let task = context(tx, &draft.task, Some(&event))?;
    if let Some(refusal) = task.as_ref().and_then(|t| t.rejections.get(&event.id)) {
        return Err(refusal.clone().into());
    }
    tx.insert(&event)?;
    Ok(event)
}

#[allow(clippy::too_many_arguments)]
pub fn transition(
    tx: &mut Tx,
    actor: &Actor,
    task: &Task,
    action: Action,
    to: &str,
    fields: BTreeMap<String, String>,
    session: Option<SessionRef>,
    children: Vec<String>,
    request_id: &str,
) -> Result<Event> {
    for (name, value) in &fields {
        check_text(name, value, MAX_FIELD, true)?;
    }
    prove(
        tx,
        Draft {
            actor: actor.clone(),
            task: task.uuid.clone(),
            project: task.project.clone(),
            kind: "transition".into(),
            base: task.head.clone(),
            request_id: request_id.into(),
            payload: json!({"action": action, "to": to, "fields": fields, "session": session, "children": children}),
        },
    )
}

fn simple(tx: &mut Tx, actor: &Actor, task: &Task, kind: &str, payload: Value, request_id: &str) -> Result<Event> {
    prove(
        tx,
        Draft {
            actor: actor.clone(),
            task: task.uuid.clone(),
            project: task.project.clone(),
            kind: kind.into(),
            base: String::new(),
            request_id: request_id.into(),
            payload,
        },
    )
}

pub fn view(task: &Task, reference: &str, full: bool) -> Value {
    let mut out = json!({
        "uuid": task.uuid, "ref": reference, "project": task.project, "title": task.title,
        "stage": task.stage, "priority": task.priority, "rank": task.rank, "labels": task.labels,
        "pinned": task.pinned, "assignee": task.assignee, "reviewer": task.reviewer,
        "flags": task.flags, "parent": task.parent, "follows": task.follows,
        "created_at": task.created_at, "updated_at": task.updated_at,
        "read_only": task.read_only, "deleted": task.deleted, "journal_count": task.journal.len(),
    });
    if full {
        out["prompt"] = json!(task.prompt);
        out["attachments"] = json!(task.attachments);
        out["journal"] = json!(task.journal);
        out["related"] = json!(task.related);
        out["resume_stage"] = json!(task.resume_stage);
        out["created_by"] = json!(task.created_by);
    }
    out
}

fn task_result(tx: &Tx, uuid: &str) -> Result<Value> {
    let task = load(tx, uuid)?;
    Ok(json!({"ok": true, "task": view(&task, &reference(tx, uuid)?, true)}))
}

fn default_title(prompt: &str) -> String {
    prompt
        .lines()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .unwrap_or_default()
        .chars()
        .take(MAX_TITLE)
        .collect()
}

fn check_title(title: &str) -> Result<(), Refusal> {
    if title.chars().count() > MAX_TITLE || title.chars().any(char::is_control) {
        return Err(Refusal::field("invalid_field", "title", "invalid", format!("a title has at most {MAX_TITLE} characters on one line")));
    }
    Ok(())
}

fn check_prompt(prompt: &str) -> Result<(), Refusal> {
    check_text("prompt", prompt, MAX_PROMPT, true)?;
    if prompt.trim().is_empty() {
        return Err(Refusal::field("missing_field", "prompt", "empty", "write the task's prompt"));
    }
    Ok(())
}

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NewAttachment {
    pub name: String,
    pub mime: String,
    pub data_base64: String,
}

pub fn store_attachments(store: &Store, items: &[NewAttachment]) -> Result<Vec<Attachment>> {
    let bad = |message: String| -> anyhow::Error { Refusal::field("invalid_field", "attachments", "invalid", message).into() };
    if items.len() > MAX_ATTACHMENTS {
        return Err(bad(format!("at most {MAX_ATTACHMENTS} images per task")));
    }
    let mut total = 0;
    let mut out = Vec::new();
    for item in items {
        if item.name.is_empty() || item.name.len() > 255 || item.name.contains('/') || item.name.chars().any(char::is_control) {
            return Err(bad("invalid attachment name".into()));
        }
        if !item.mime.starts_with("image/") || item.mime.len() > 100 {
            return Err(bad("attachments must be images".into()));
        }
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(item.data_base64.as_bytes())
            .map_err(|_| bad("attachment data is not base64".into()))?;
        total += bytes.len();
        if bytes.len() > MAX_ATTACHMENT || total > MAX_ATTACHMENTS_TOTAL {
            return Err(bad("images are limited to 10 MiB each and 20 MiB in total".into()));
        }
        let sha256 = blobs::put(store.dir(), &bytes)?;
        out.push(Attachment { name: item.name.clone(), mime: item.mime.clone(), bytes: bytes.len() as u64, sha256 });
    }
    Ok(out)
}

fn board_tasks(tx: &Tx, project: &str) -> Result<Vec<Task>> {
    let events = tx.load(&Filter::Project(project.into()))?;
    Ok(fold_many(&events).into_values().filter(|t| t.project == project).collect())
}

pub fn next_rank(tx: &Tx, project: &str, stage: &str) -> Result<f64> {
    let max = board_tasks(tx, project)?
        .iter()
        .filter(|t| !t.deleted && t.stage == stage)
        .map(|t| t.rank)
        .fold(0.0_f64, f64::max);
    Ok(max + 1.0)
}

#[allow(clippy::too_many_arguments)]
pub fn created_payload(
    title: &str,
    prompt: &str,
    stage: &str,
    priority: Option<u8>,
    labels: &[String],
    pinned: Option<SessionRef>,
    parent: &str,
    follows: BTreeSet<String>,
    attachments: Vec<Attachment>,
    fields: BTreeMap<String, String>,
    rank: f64,
) -> Result<Value> {
    check_prompt(prompt)?;
    let title = if title.trim().is_empty() { default_title(prompt) } else { title.trim().to_owned() };
    check_title(&title)?;
    let priority = priority.unwrap_or(2);
    if priority > 4 {
        return Err(Refusal::field("invalid_field", "priority", "invalid", "priority is 0 (highest) to 4").into());
    }
    if labels.len() > 32 {
        return Err(Refusal::field("invalid_field", "labels", "invalid", "at most 32 labels").into());
    }
    for label in labels {
        check_label(label)?;
    }
    for (name, value) in &fields {
        check_text(name, value, MAX_FIELD, true)?;
    }
    Ok(json!({
        "title": title, "prompt": prompt, "stage": stage, "priority": priority, "rank": rank,
        "labels": labels, "pinned": pinned, "parent": parent, "follows": follows,
        "attachments": attachments, "fields": fields,
    }))
}

fn task_uuid(request_id: &str) -> String {
    uuid::Uuid::parse_str(request_id)
        .map(|u| u.hyphenated().to_string())
        .unwrap_or_else(|_| uuid::Uuid::new_v4().to_string())
}

fn local_session(session: Option<SessionRef>, machine: &str) -> Option<SessionRef> {
    session.map(|mut s| {
        if s.machine.is_empty() {
            s.machine = machine.into();
        }
        s
    })
}

pub fn rev(ctx: &mut Ctx) -> Result<Value> {
    Ok(json!({"ok": true, "cursor": ctx.store.cursor()?}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct BoardRequest {
    #[serde(default)]
    project: String,
    #[serde(default)]
    since: Option<i64>,
}

pub fn board(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: BoardRequest = parse(&request)?;
    let cursor = ctx.store.cursor()?;
    let touched = match req.since {
        Some(since) => Some(ctx.store.touched_since(since)?),
        None => None,
    };
    let events = ctx.store.load(&Filter::Project(req.project.clone()))?;
    let refs: Vec<&Event> = events.iter().collect();
    let wf = project_workflow(&refs, &req.project);
    let names = refs::refs(&ctx.store.task_ids()?);
    let shared_changed = touched.as_ref().is_some_and(|t| t.contains(""));
    let mut tasks: Vec<Task> = fold_many(&events)
        .into_values()
        .filter(|t| t.project == req.project)
        .filter(|t| shared_changed || touched.as_ref().map_or(true, |set| set.contains(&t.uuid)))
        .filter(|t| touched.is_some() || !t.deleted)
        .collect();
    let position = |stage: &str| wf.stages.iter().position(|s| s.name == stage).unwrap_or(usize::MAX);
    tasks.sort_by(|a, b| {
        position(&a.stage)
            .cmp(&position(&b.stage))
            .then(a.rank.partial_cmp(&b.rank).unwrap_or(std::cmp::Ordering::Equal))
            .then(a.uuid.cmp(&b.uuid))
    });
    let views: Vec<Value> = tasks
        .iter()
        .map(|t| view(t, names.get(&t.uuid).map(String::as_str).unwrap_or_default(), false))
        .collect();
    Ok(json!({"ok": true, "project": req.project, "cursor": cursor, "workflow": wf, "tasks": views}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct TaskRequest {
    #[serde(default)]
    request_id: String,
    task: String,
}

pub fn show(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: TaskRequest = parse(&request)?;
    ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CreateRequest {
    #[serde(default)]
    request_id: String,
    #[serde(default)]
    project: String,
    #[serde(default)]
    title: String,
    prompt: String,
    #[serde(default)]
    stage: String,
    #[serde(default)]
    priority: Option<u8>,
    #[serde(default)]
    labels: Vec<String>,
    #[serde(default)]
    pinned: Option<SessionRef>,
    #[serde(default)]
    attachments: Vec<NewAttachment>,
    #[serde(default)]
    parent: String,
    #[serde(default)]
    follows: Vec<String>,
}

pub fn create(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: CreateRequest = parse(&request)?;
    check_project(&req.project)?;
    let pinned = local_session(req.pinned.clone(), ctx.store.machine());
    if pinned.is_none() && req.project.is_empty() {
        return Err(Refusal::field("missing_field", "project", "missing", "choose a project or pin the task to a session").into());
    }
    check_prompt(&req.prompt)?;
    let attachments = store_attachments(ctx.store, &req.attachments)?;
    write(ctx, &request, |tx, actor| {
        let wf = workflow(tx, &req.project)?;
        let stage = if req.stage.is_empty() {
            wf.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default()
        } else {
            req.stage.clone()
        };
        let follows = req.follows.iter().map(|r| resolve(tx, r)).collect::<Result<BTreeSet<_>>>()?;
        let parent = if req.parent.is_empty() { String::new() } else { resolve(tx, &req.parent)? };
        let rank = next_rank(tx, &req.project, &stage)?;
        let payload = created_payload(&req.title, &req.prompt, &stage, req.priority, &req.labels, pinned.clone(), &parent, follows, attachments.clone(), BTreeMap::new(), rank)?;
        let uuid = task_uuid(&req.request_id);
        if tx.task_ids()?.contains(&uuid) {
            return refuse("request_reused", "a task with this request_id already exists");
        }
        prove(tx, Draft { actor: actor.clone(), task: uuid.clone(), project: req.project.clone(), kind: "task_created".into(), base: String::new(), request_id: req.request_id.clone(), payload })?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct EditRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    prompt: Option<String>,
    #[serde(default)]
    priority: Option<u8>,
    #[serde(default)]
    attachments: Option<Vec<NewAttachment>>,
}

pub fn edit(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: EditRequest = parse(&request)?;
    let attachments = match &req.attachments {
        Some(items) => Some(store_attachments(ctx.store, items)?),
        None => None,
    };
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let mut changes: Vec<(&str, Value)> = Vec::new();
        if let Some(title) = &req.title {
            let title = title.trim();
            if title.is_empty() {
                return Err(Refusal::field("missing_field", "title", "empty", "a title cannot be empty").into());
            }
            check_title(title)?;
            changes.push(("title", json!(title)));
        }
        if let Some(prompt) = &req.prompt {
            check_prompt(prompt)?;
            changes.push(("prompt", json!(prompt)));
        }
        if let Some(priority) = req.priority {
            if priority > 4 {
                return Err(Refusal::field("invalid_field", "priority", "invalid", "priority is 0 (highest) to 4").into());
            }
            changes.push(("priority", json!(priority)));
        }
        if let Some(attachments) = &attachments {
            changes.push(("attachments", json!(attachments)));
        }
        for (field, value) in changes {
            let task = load(tx, &uuid)?;
            simple(tx, actor, &task, "field_set", json!({"field": field, "value": value}), &req.request_id)?;
        }
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MoveRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    to: String,
}

fn move_to(ctx: &mut Ctx, request: &Value, task_ref: &str, target: impl Fn(&Workflow) -> Option<String>) -> Result<Value> {
    human_only(&ctx.actor)?;
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default().to_owned();
    write(ctx, request, |tx, actor| {
        let uuid = resolve(tx, task_ref)?;
        let task = load(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to = target(&wf).ok_or_else(|| Refusal::new("unknown_stage", "unknown stage"))?;
        let from_role = wf.role(&task.stage).unwrap_or(Role::Backlog);
        let to_role = wf.role(&to).ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {to}")))?;
        let action = match (from_role, to_role) {
            (Role::Active, Role::Ready) => Action::Unclaim,
            (Role::Review, Role::Done) => Action::Done,
            (Role::Done, Role::Ready) => Action::Reopen,
            (_, Role::Active) if from_role != Role::Active => return refuse("use_send", "send the task to a session to start it"),
            (Role::NeedsHuman, _) if to_role != Role::Parked => return refuse("use_answer", "answer the question to resume the task"),
            _ => Action::Move,
        };
        transition(tx, actor, &task, action, &to, BTreeMap::new(), None, Vec::new(), &request_id)?;
        task_result(tx, &uuid)
    })
}

pub fn move_task(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: MoveRequest = parse(&request)?;
    let to = req.to.clone();
    move_to(ctx, &request, &req.task, move |_| Some(to.clone()))
}

pub fn done(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    move_to(ctx, &request, &req.task, |wf| wf.first(Role::Done).map(|s| s.name.clone()))
}

pub fn reopen(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    move_to(ctx, &request, &req.task, |wf| wf.first(Role::Ready).map(|s| s.name.clone()))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReorderRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    before: Option<String>,
}

pub fn reorder(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReorderRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let column: Vec<Task> = board_tasks(tx, &task.project)?
            .into_iter()
            .filter(|t| !t.deleted && t.stage == task.stage && t.uuid != uuid)
            .collect();
        let rank = match &req.before {
            None => column.iter().map(|t| t.rank).fold(0.0_f64, f64::max) + 1.0,
            Some(before) => {
                let before = load(tx, &resolve(tx, before)?)?;
                let previous = column.iter().map(|t| t.rank).filter(|r| *r < before.rank).fold(f64::MIN, f64::max);
                if previous == f64::MIN { before.rank - 1.0 } else { (previous + before.rank) / 2.0 }
            }
        };
        simple(tx, actor, &task, "rank_set", json!({"rank": rank}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PinRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    session: Option<SessionRef>,
}

pub fn pin(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: PinRequest = parse(&request)?;
    let session = local_session(req.session.clone(), ctx.store.machine());
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        simple(tx, actor, &task, "pin_set", json!({"session": session}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LabelRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    label: String,
    present: bool,
}

pub fn label(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: LabelRequest = parse(&request)?;
    check_label(&req.label)?;
    if RESERVED_LABELS.contains(&req.label.as_str()) {
        return refuse("reserved_label", format!("`{}` is set by workflow actions", req.label));
    }
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        simple(tx, actor, &task, "label_set", json!({"label": req.label, "present": req.present}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LinkRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    relation: String,
    target: String,
    present: bool,
}

pub fn link(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: LinkRequest = parse(&request)?;
    if !matches!(req.relation.as_str(), "parent" | "follows" | "related") {
        return Err(Refusal::field("invalid_field", "relation", "invalid", "relation is parent, follows or related").into());
    }
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let target = resolve(tx, &req.target)?;
        if target == uuid {
            return Err(Refusal::field("invalid_field", "target", "invalid", "a task cannot refer to itself").into());
        }
        let task = load(tx, &uuid)?;
        simple(tx, actor, &task, "relation_set", json!({"relation": req.relation, "target": target, "present": req.present}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CommentRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    text: String,
}

pub fn comment(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: CommentRequest = parse(&request)?;
    check_text("text", &req.text, MAX_FIELD, true)?;
    if req.text.trim().is_empty() {
        return Err(Refusal::field("missing_field", "text", "empty", "write the comment").into());
    }
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        if actor.kind == ActorKind::Agent {
            let me = actor.session_ref();
            if me.is_none() || (me != task.assignee && me != task.reviewer) {
                return refuse("not_assigned", "this task is not assigned to your session");
            }
        }
        simple(tx, actor, &task, "journal", json!({"marker": "[comment]", "text": req.text}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

pub fn delete(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        simple(tx, actor, &task, "task_deleted", json!({}), &req.request_id)?;
        Ok(json!({"ok": true, "deleted": uuid}))
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ProjectRequest {
    #[serde(default)]
    project: String,
}

pub fn workflow_get(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: ProjectRequest = parse(&request)?;
    let wf = ctx.store.read(|tx| workflow(tx, &req.project))?;
    Ok(json!({"ok": true, "project": req.project, "workflow": wf}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkflowSetRequest {
    #[serde(default)]
    request_id: String,
    #[serde(default)]
    project: String,
    workflow: Value,
    #[serde(default)]
    moves: BTreeMap<String, String>,
}

pub fn workflow_set(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: WorkflowSetRequest = parse(&request)?;
    check_project(&req.project)?;
    let wf: Workflow = serde_json::from_value(req.workflow.clone())
        .map_err(|e| Refusal::new("invalid_workflow", format!("invalid workflow: {e}")))?;
    wf.validate().map_err(|e| Refusal::new("invalid_workflow", format!("{e:#}")))?;
    write(ctx, &request, |tx, actor| {
        for task in board_tasks(tx, &req.project)?.iter().filter(|t| !t.deleted) {
            if wf.stage(&task.stage).is_none() && !req.moves.get(&task.stage).is_some_and(|to| wf.stage(to).is_some()) {
                return refuse("moves_required", format!("stage {} still holds tasks; say where to move them in `moves`", task.stage));
            }
        }
        tx.append(&Draft {
            actor: actor.clone(),
            task: String::new(),
            project: req.project.clone(),
            kind: "workflow_set".into(),
            base: String::new(),
            request_id: req.request_id.clone(),
            payload: json!({"workflow": wf, "moves": req.moves}),
        })?;
        Ok(json!({"ok": true, "project": req.project, "workflow": wf}))
    })
}
```

- [ ] **Step 4: Run the Rust tests**

Add `mod commands;` to `src/overmind/mod.rs`.
Run: `cargo test --locked overmind::commands 2>&1 | tail -5`
Expected: 10 tests PASS.

- [ ] **Step 5: Write `cli.rs` and route the command**

`src/overmind/cli.rs`:

```rust
//! `hgs overmind`: JSON in, JSON out. Refusals are printed as JSON on stdout
//! so that the desktop, the phone and MCP can show their fields.
use super::commands::{self, Ctx};
use super::model::{Actor, ActorKind, Refusal};
use super::store::Store;
use crate::{cli, config::Config};
use anyhow::{bail, ensure, Result};
use serde_json::{json, Value};
use std::io::Read;

pub const MAX_REQUEST: u64 = 29 * 1024 * 1024;

const HUMAN_USAGE: &str = "usage: hgs [@host] overmind <command> [--json]   (JSON request on stdin)
  board --json {project, since?}           tasks of a project, optionally changed since a cursor
  show <ref> | show --json {task}          one task with its prompt and journal
  create --json {project, prompt, title?, stage?, priority?, labels?, pinned?, attachments?, parent?, follows?}
  edit --json {task, title?, prompt?, priority?, attachments?}
  move --json {task, to}  |  done --json {task}  |  reopen --json {task}
  reorder --json {task, before?}  |  pin --json {task, session?}
  label --json {task, label, present}  |  link --json {task, relation, target, present}
  comment --json {task, text}  |  delete --json {task}
  workflow get --json {project}  |  workflow set --json {project, workflow, moves?}
  rev                                      change cursor for watchers
Write requests may carry a request_id (UUID); repeating it replays the result.
";

const AGENT_USAGE: &str = "usage: hgs overmind <command> --json   (JSON request on stdin)
  mine                                     tasks assigned to this session or sent to it for review
  show <ref>                               the task, its prompt and journal
  comment --json {task, text}
  advance --json {task, to, spec}          Design -> Build: your plan
  advance --json {task, to, worklog, evidence, root_cause?}   Build -> Review: what you did and its commit
  call-human --json {task, question}       park the task until a person decides
  return --json {task, reason}             give back a task blocked by something external
  decompose --json {task, subtasks: [{title?, prompt}], ordered?}
  file --json {prompt, breaks, title?, related?, project?}   record an out-of-scope finding in Backlog
  attach --json {task, name, mime, data_base64, note?}
  review --json {task, verdict: approve|needs_work, report}
You can never move a task to Done: a person signs it off.
";

fn usage(actor: &Actor) -> &'static str {
    if actor.kind == ActorKind::Agent {
        AGENT_USAGE
    } else {
        HUMAN_USAGE
    }
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> cli::Result<i32> {
    if dry {
        return Err(cli::Error::new(1, "overmind commands do not support --dry-run"));
    }
    match run(config, args) {
        Ok(Some(value)) => {
            println!("{value}");
            Ok(0)
        }
        Ok(None) => Ok(0),
        Err(error) => {
            if let Some(refusal) = error.downcast_ref::<Refusal>() {
                println!("{}", json!({"ok": false, "error": refusal}));
                return Err(cli::Error::new(1, refusal.message.clone()));
            }
            Err(cli::Error::new(1, format!("{error:#}")))
        }
    }
}

fn read_stdin() -> Result<Value> {
    let mut bytes = Vec::new();
    std::io::stdin().take(MAX_REQUEST + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() as u64 <= MAX_REQUEST, "the overmind request exceeds 29 MiB");
    Ok(serde_json::from_slice(&bytes)?)
}

fn run(config: &Config, args: &[String]) -> Result<Option<Value>> {
    let machine = crate::swarm::local_node_id(config)?;
    let actor = Actor::human(&machine);
    let cwd = None;
    let command = args.first().map(String::as_str).unwrap_or("--help");
    if matches!(command, "-h" | "--help" | "help") {
        print!("{}", usage(&actor));
        return Ok(None);
    }
    let rest: Vec<&str> = args[1..].iter().map(String::as_str).collect();
    let json_flag = rest.last() == Some(&"--json");
    let positional: Vec<&str> = rest.iter().copied().filter(|a| *a != "--json").collect();
    let request = if json_flag { read_stdin()? } else { json!({}) };
    let mut store = Store::open(&config.state.join("overmind"), &machine)?;
    let mut ctx = Ctx { store: &mut store, actor, cwd };
    let value = match (command, positional.as_slice()) {
        ("rev", []) => commands::rev(&mut ctx)?,
        ("board", []) => commands::board(&mut ctx, request)?,
        ("show", [reference]) => commands::show(&mut ctx, json!({"task": reference}))?,
        ("show", []) => commands::show(&mut ctx, request)?,
        ("create", []) => commands::create(&mut ctx, request)?,
        ("edit", []) => commands::edit(&mut ctx, request)?,
        ("move", []) => commands::move_task(&mut ctx, request)?,
        ("done", []) => commands::done(&mut ctx, request)?,
        ("reopen", []) => commands::reopen(&mut ctx, request)?,
        ("reorder", []) => commands::reorder(&mut ctx, request)?,
        ("pin", []) => commands::pin(&mut ctx, request)?,
        ("label", []) => commands::label(&mut ctx, request)?,
        ("link", []) => commands::link(&mut ctx, request)?,
        ("comment", []) => commands::comment(&mut ctx, request)?,
        ("delete", []) => commands::delete(&mut ctx, request)?,
        ("workflow", ["get"]) => commands::workflow_get(&mut ctx, request)?,
        ("workflow", ["set"]) => commands::workflow_set(&mut ctx, request)?,
        _ => bail!("unknown overmind command; see hgs overmind --help"),
    };
    Ok(Some(value))
}
```

In `src/overmind/mod.rs` add `mod cli; mod commands;` (keep alphabetical order) and `pub use cli::dispatch;`.

In `src/cli.rs`:
- add to `USAGE`, after the `hgs [@host] search …` line:
  `       hgs [@host] overmind <command> [--json]   Overmind tasks (see hgs overmind --help)`
- add a match arm next to `"swarm" => …`:
  `"overmind" => crate::overmind::dispatch(&config, args, dry),`

- [ ] **Step 6: Write the Python integration test** `tests/test_overmind.py`

```python
#!/usr/bin/env python3
"""Overmind tasks through the real CLI with isolated state."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()


class Overmind(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-overmind-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        (self.root / 'config').mkdir()
        tmux = self.root / 'bin/tmux'
        tmux.write_text('#!/bin/sh\nif [ "$1" = -V ]; then echo "tmux 3.7"; fi\n')
        tmux.chmod(0o755)
        self.state = self.root / 'state'
        self.env = dict(os.environ, HOME=str(self.root), HGS_CONFIG_DIR=str(self.root / 'config'),
                        HGS_STATE_DIR=str(self.state), HGS_TAB='0',
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        for key in ('HGS_PEERS', 'HGS_SELF', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION', 'HGS_EXECUTABLE'):
            self.env.pop(key, None)

    def run_hgs(self, *args, data=None, env=None):
        return subprocess.run([str(HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                              env=env or self.env, text=True, capture_output=True, timeout=20)

    def ok(self, *args, data=None):
        result = self.run_hgs(*args, data=data)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def refused(self, *args, data=None):
        result = self.run_hgs(*args, data=data)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertTrue(result.stderr.startswith('hgs: '), result.stderr)
        body = json.loads(result.stdout)
        self.assertFalse(body['ok'])
        return body['error']

    def test_create_board_show_and_move(self):
        created = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'Fix drafts\nmore', 'stage': 'Queue'})
        ref = created['task']['ref']
        self.assertTrue(ref.startswith('om-'))
        board = self.ok('board', '--json', data={'project': 'demo'})
        self.assertEqual([t['ref'] for t in board['tasks']], [ref])
        self.assertEqual(self.ok('show', ref)['task']['prompt'], 'Fix drafts\nmore')
        self.assertEqual(self.ok('move', '--json', data={'task': ref, 'to': 'Backlog'})['task']['stage'], 'Backlog')
        self.assertEqual(self.refused('done', '--json', data={'task': ref})['code'], 'transition_not_allowed')

    def test_store_is_private(self):
        self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x'})
        directory = self.state / 'overmind'
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual((directory / 'overmind.sqlite3').stat().st_mode & 0o777, 0o600)
        self.assertEqual(int((directory / 'rev').read_text()), self.ok('rev')['cursor'])

    def test_receipts_replay(self):
        request = {'request_id': '0b9c5c1e-34c4-4d5e-9a64-1d2a3b4c5d6e', 'project': 'demo', 'prompt': 'x'}
        first = self.ok('create', '--json', data=request)
        self.assertEqual(self.ok('create', '--json', data=request), first)
        self.assertEqual(self.refused('create', '--json', data={**request, 'prompt': 'y'})['code'], 'request_reused')

    def test_invalid_requests_and_help(self):
        self.assertEqual(self.refused('create', '--json', data={'project': 'demo', 'prompt': 'x', 'unknown': 1})['code'],
                         'invalid_request')
        self.assertEqual(self.refused('create', '--json', data={'project': 'demo', 'prompt': '   '})['field'], 'prompt')
        result = self.run_hgs('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('workflow set', result.stdout)
        unknown = self.run_hgs('frobnicate')
        self.assertEqual(unknown.returncode, 1)
        self.assertIn('unknown overmind command', unknown.stderr)


if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 7: Run all checks for this task**

Run:
```bash
cargo test --locked overmind:: 2>&1 | tail -5
cargo build --locked 2>&1 | tail -2
HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind -v 2>&1 | tail -8
```
Expected: Rust tests PASS; 4 Python tests OK.

- [ ] **Step 8: Commit**

```bash
git add src/overmind/commands.rs src/overmind/cli.rs src/overmind/mod.rs src/cli.rs tests/test_overmind.py
git commit --only src/overmind/commands.rs src/overmind/cli.rs src/overmind/mod.rs src/cli.rs tests/test_overmind.py \
  -m "Add hgs overmind commands for people: create, board, show, move and edit tasks"
```

---

### Task 6: Agent identity and agent commands

**Files:**
- Create: `src/overmind/identity.rs`, `src/overmind/agent.rs`
- Modify: `src/state/mod.rs` (add `CallerSession` and `caller_session()`), `src/overmind/mod.rs`, `src/overmind/cli.rs`, `tests/test_overmind.py`

**Interfaces:**
- Consumes: Task 5 helpers (`write`, `load`, `resolve`, `workflow`, `prove`, `transition`, `view`, `reference`, `parse`, `refuse`, `check_text`, `created_payload`, `next_rank`, `store_attachments`, `NewAttachment`, `MAX_FIELD`).
- Produces:
  - `crate::state::{CallerSession { name: String, run_id: String, cwd: String }, caller_session() -> Result<Option<CallerSession>, String>}`.
  - `identity::caller(machine: &str) -> anyhow::Result<(Actor, Option<PathBuf>)>`.
  - `agent::{mine, advance, call_human, return_task, decompose, file, attach, review}`, each `(&mut Ctx, Value) -> anyhow::Result<Value>` (`mine` takes only `&mut Ctx`). Results carry `note` (English) with the next expected step.

- [ ] **Step 1: Write the failing Rust tests** (bottom of `agent.rs`)

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands;
    use crate::overmind::store::Store;
    use base64::Engine;
    use serde_json::json;
    use std::path::PathBuf;
    use std::process::Command;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn setup() -> (tempfile::TempDir, Store, String) {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let task = commands::create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": "Queue"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        // Stage 1b sends tasks; here the claim is written directly.
        store.write(|tx| {
            let current = commands::load(tx, &task)?;
            commands::transition(tx, &Actor::human(MACHINE), &current, Action::Claim, "Design", BTreeMap::new(), Some(SessionRef { machine: MACHINE.into(), name: "w1".into() }), vec![], "")?;
            Ok(())
        }).unwrap();
        (dir, store, task)
    }
    fn agent<'a>(store: &'a mut Store, name: &str, cwd: Option<PathBuf>) -> Ctx<'a> {
        Ctx { store, actor: Actor::agent(MACHINE, name), cwd }
    }
    fn refusal(error: anyhow::Error) -> Refusal {
        error.downcast::<Refusal>().unwrap()
    }

    #[test]
    fn mine_lists_assigned_tasks_with_a_note() {
        let (_dir, mut store, task) = setup();
        let mine_now = mine(&mut agent(&mut store, "w1", None)).unwrap();
        assert_eq!(mine_now["tasks"][0]["uuid"], task);
        assert!(mine_now["note"].as_str().unwrap().contains("one task"));
        assert!(mine(&mut agent(&mut store, "w2", None)).unwrap()["tasks"].as_array().unwrap().is_empty());
    }

    #[test]
    fn advance_through_build_to_review_outside_git() {
        let (_dir, mut store, task) = setup();
        let outside = tempfile::tempdir().unwrap();
        let built = advance(&mut agent(&mut store, "w1", Some(outside.path().into())), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        assert_eq!(built["task"]["stage"], "Build");
        assert!(built["note"].as_str().unwrap().contains("Review"));
        let reviewed = advance(&mut agent(&mut store, "w1", Some(outside.path().into())), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "manual check"})).unwrap();
        assert_eq!(reviewed["task"]["stage"], "Review");
    }

    #[test]
    fn evidence_must_be_an_existing_commit_in_git() {
        let (_dir, mut store, task) = setup();
        let repo = tempfile::tempdir().unwrap();
        let git = |args: &[&str]| assert!(Command::new("git").arg("-C").arg(repo.path()).args(args).output().unwrap().status.success());
        git(&["init", "-q"]);
        git(&["-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "--allow-empty", "-m", "x"]);
        let head = String::from_utf8(Command::new("git").arg("-C").arg(repo.path()).args(["rev-parse", "HEAD"]).output().unwrap().stdout).unwrap().trim().to_owned();
        let cwd = Some(repo.path().to_path_buf());
        advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        let text = advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "trust me"})).unwrap_err();
        assert_eq!((refusal(text).field.as_str()), "evidence");
        let missing = advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "deadbeefdeadbeef"})).unwrap_err();
        assert_eq!(refusal(missing).state, "invalid");
        advance(&mut agent(&mut store, "w1", cwd), json!({"task": task, "to": "Review", "worklog": "did", "evidence": head})).unwrap();
    }

    #[test]
    fn predecessors_block_review() {
        let (_dir, mut store, task) = setup();
        let mut human = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let before = commands::create(&mut human, json!({"project": "p", "prompt": "First"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        commands::link(&mut human, json!({"task": task, "relation": "follows", "target": before, "present": true})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        let blocked = advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap_err();
        assert_eq!(refusal(blocked).code, "predecessor_open");
    }

    #[test]
    fn call_human_return_and_file() {
        let (_dir, mut store, task) = setup();
        let parked = call_human(&mut agent(&mut store, "w1", None), json!({"task": task, "question": "Which API?"})).unwrap();
        assert_eq!(parked["task"]["stage"], "Your Call");
        assert!(parked["note"].as_str().unwrap().contains("answer"));
        let returned = return_task(&mut agent(&mut store, "w1", None), json!({"task": task, "reason": "no access"})).unwrap();
        assert_eq!(returned["task"]["stage"], "Backlog");
        assert!(returned["task"]["labels"].as_array().unwrap().contains(&json!("blocked")));
        let filed = file(&mut agent(&mut store, "w1", None), json!({"prompt": "Flaky test", "breaks": "CI fails randomly", "related": task})).unwrap();
        assert_eq!(filed["task"]["stage"], "Backlog");
        assert_eq!(filed["task"]["journal"][0]["marker"], "[filed-by-agent]");
        let no_breaks = file(&mut agent(&mut store, "w1", None), json!({"prompt": "x", "related": task})).unwrap_err();
        assert_eq!(refusal(no_breaks).field, "breaks");
    }

    #[test]
    fn decompose_creates_ordered_children_and_an_epic() {
        let (_dir, mut store, task) = setup();
        let result = decompose(&mut agent(&mut store, "w1", None), json!({"task": task, "ordered": true, "subtasks": [{"prompt": "Part one"}, {"prompt": "Part two"}]})).unwrap();
        assert_eq!(result["task"]["stage"], "Backlog");
        assert!(result["task"]["labels"].as_array().unwrap().contains(&json!("epic")));
        let children = result["children"].as_array().unwrap();
        assert_eq!(children.len(), 2);
        assert_eq!(children[0]["stage"], "Queue");
        assert_eq!(children[1]["follows"], json!([children[0]["uuid"]]));
        let one = decompose(&mut agent(&mut store, "w2", None), json!({"task": task, "subtasks": [{"prompt": "a"}, {"prompt": "b"}]})).unwrap_err();
        assert_eq!(refusal(one).code, "not_assigned");
    }

    #[test]
    fn attach_and_review_need_the_right_session() {
        let (_dir, mut store, task) = setup();
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let attached = attach(&mut agent(&mut store, "w1", None), json!({"task": task, "name": "shot.png", "mime": "image/png", "data_base64": png, "note": "after"})).unwrap();
        assert_eq!(attached["task"]["attachments"][0]["name"], "shot.png");
        assert_eq!(refusal(attach(&mut agent(&mut store, "w2", None), json!({"task": task, "name": "x.png", "mime": "image/png", "data_base64": png})).unwrap_err()).code, "not_assigned");
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "s"})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap();
        let own = review(&mut agent(&mut store, "w1", None), json!({"task": task, "verdict": "approve", "report": "fine"})).unwrap_err();
        assert_eq!(refusal(own).code, "not_reviewer");
        store.write(|tx| {
            let current = commands::load(tx, &task)?;
            commands::prove(tx, Draft { actor: Actor::human(MACHINE), task: task.clone(), project: current.project.clone(), kind: "reviewer_set".into(), base: String::new(), request_id: String::new(), payload: json!({"session": {"machine": MACHINE, "name": "w2"}}) })?;
            Ok(())
        }).unwrap();
        let failed = review(&mut agent(&mut store, "w2", None), json!({"task": task, "verdict": "needs_work", "report": "tests missing"})).unwrap();
        assert_eq!(failed["task"]["stage"], "Build");
        assert!(failed["task"]["labels"].as_array().unwrap().contains(&json!("review-failed")));
        let bad = review(&mut agent(&mut store, "w2", None), json!({"task": task, "verdict": "maybe", "report": "?"})).unwrap_err();
        assert_eq!(refusal(bad).field, "verdict");
    }
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cargo test --locked overmind::agent 2>&1 | tail -5`
Expected: compile errors.

- [ ] **Step 3: Add the caller check to `src/state/mod.rs`**

Add after `account_binding` (it reuses the hook gate: environment run token plus a live, matching pane):

```rust
/// The Zerus session a CLI call comes from, verified like hook events.
pub(crate) struct CallerSession {
    pub name: String,
    pub run_id: String,
    pub cwd: String,
}

/// None outside a session. A session environment that does not match a live
/// session is an error, never a person.
pub(crate) fn caller_session() -> Result<Option<CallerSession>> {
    let (Some(name), Some(run_id)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID")) else {
        return Ok(None);
    };
    let mismatch = || "this session environment does not match a live Zerus session".to_string();
    let _guard = lock(None)?;
    let record = read_run(&name, &run_id)?.ok_or_else(mismatch)?;
    if !matches(&record, live()?.get(string(&record, "name"))) {
        return Err(mismatch());
    }
    Ok(Some(CallerSession { name: string(&record, "name").to_owned(), run_id, cwd: string(&record, "cwd").to_owned() }))
}
```

- [ ] **Step 4: Write `identity.rs`**

```rust
//! Who is calling: a person, or the agent of a verified Zerus session. This
//! guards against agent mistakes; it is not a security boundary.
use super::model::Actor;
use anyhow::{anyhow, Result};
use std::path::PathBuf;

pub fn caller(machine: &str) -> Result<(Actor, Option<PathBuf>)> {
    match crate::state::caller_session().map_err(|e| anyhow!(e))? {
        None => Ok((Actor::human(machine), None)),
        Some(session) => Ok((Actor::agent(machine, &session.name), Some(PathBuf::from(session.cwd)))),
    }
}
```

- [ ] **Step 5: Implement `agent.rs`** (above the tests)

```rust
//! Commands for agents in Zerus sessions. Each one is narrow and refuses moves
//! that would break the process; results carry a `note` with the next step.
use super::commands::{
    check_text, created_payload, load, next_rank, parse, prove, reference, refuse, resolve,
    store_attachments, transition, view, workflow, write, Ctx, NewAttachment, MAX_FIELD,
};
use super::fold::{fold_many, Task};
use super::model::*;
use super::store::{Filter, Tx};
use super::workflow::{Action, Evidence, Role};
use anyhow::Result;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::process::Command;

fn agent_only(actor: &Actor) -> Result<SessionRef> {
    match (actor.kind, actor.session_ref()) {
        (ActorKind::Agent, Some(me)) => Ok(me),
        _ => refuse("agent_only", "this command is for agents working in a Zerus session"),
    }
}

fn result(tx: &Tx, uuid: &str, note: &str) -> Result<Value> {
    let task = load(tx, uuid)?;
    Ok(json!({"ok": true, "task": view(&task, &reference(tx, uuid)?, true), "note": note}))
}

fn fields(pairs: &[(&str, &Option<String>)]) -> BTreeMap<String, String> {
    pairs
        .iter()
        .filter_map(|(k, v)| v.as_ref().map(|v| (k.to_string(), v.clone())))
        .collect()
}

pub fn mine(ctx: &mut Ctx) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let events = ctx.store.load(&Filter::All)?;
    let names = super::refs::refs(&ctx.store.task_ids()?);
    let tasks: Vec<Value> = fold_many(&events)
        .into_values()
        .filter(|t| !t.deleted && (t.assignee.as_ref() == Some(&me) || t.reviewer.as_ref() == Some(&me)))
        .map(|t| {
            let mut v = view(&t, names.get(&t.uuid).map(String::as_str).unwrap_or_default(), false);
            v["role"] = json!(if t.reviewer.as_ref() == Some(&me) { "reviewer" } else { "assignee" });
            v
        })
        .collect();
    Ok(json!({"ok": true, "tasks": tasks,
        "note": "Work on one task at a time. Read it with `show`, then advance it; call_human when a decision is not yours to make."}))
}

fn verify_evidence(cwd: &Path, evidence: &str) -> Result<()> {
    let inside = Command::new("git")
        .arg("-C").arg(cwd)
        .args(["rev-parse", "--is-inside-work-tree"])
        .output()
        .map(|o| o.status.success() && String::from_utf8_lossy(&o.stdout).trim() == "true")
        .unwrap_or(false);
    if !inside {
        return Ok(());
    }
    let sha = evidence.trim();
    let hex = (7..=64).contains(&sha.len()) && sha.bytes().all(|b| b.is_ascii_hexdigit());
    if !hex {
        return Err(Refusal::field("invalid_field", "evidence", "invalid", "in a Git repository, evidence is the commit SHA of the finished work").into());
    }
    let exists = Command::new("git")
        .arg("-C").arg(cwd)
        .args(["cat-file", "-e", &format!("{sha}^{{commit}}")])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if !exists {
        return Err(Refusal::field("invalid_field", "evidence", "invalid", format!("commit {sha} does not exist in this repository")).into());
    }
    Ok(())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AdvanceRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    to: String,
    #[serde(default)]
    spec: Option<String>,
    #[serde(default)]
    worklog: Option<String>,
    #[serde(default)]
    evidence: Option<String>,
    #[serde(default)]
    root_cause: Option<String>,
}

pub fn advance(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: AdvanceRequest = parse(&request)?;
    let cwd = ctx.cwd.clone();
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to_role = wf.role(&req.to).ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {}", req.to)))?;
        if to_role == Role::Review {
            for predecessor in &task.follows {
                let before = load(tx, predecessor)?;
                let role = workflow(tx, &before.project)?.role(&before.stage);
                if !matches!(role, Some(Role::Review) | Some(Role::Done)) {
                    return refuse("predecessor_open", format!("{} must reach review first", reference(tx, predecessor)?));
                }
            }
            if wf.evidence == Evidence::GitShaOrText {
                if let (Some(cwd), Some(evidence)) = (&cwd, &req.evidence) {
                    verify_evidence(cwd, evidence)?;
                }
            }
        }
        let fields = fields(&[("spec", &req.spec), ("worklog", &req.worklog), ("evidence", &req.evidence), ("root_cause", &req.root_cause)]);
        transition(tx, actor, &task, Action::Advance, &req.to, fields, None, vec![], &req.request_id)?;
        let note = if to_role == Role::Review {
            "Done from your side: the task waits for review. Take your next task."
        } else {
            "Implement the spec, then advance to Review with a worklog and evidence (the commit SHA in a Git repository)."
        };
        result(tx, &uuid, note)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct QuestionRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    question: String,
}

pub fn call_human(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: QuestionRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let to = workflow(tx, &task.project)?.first(Role::NeedsHuman).map(|s| s.name.clone()).unwrap_or_default();
        let fields = BTreeMap::from([("question".to_string(), req.question.clone())]);
        transition(tx, actor, &task, Action::CallHuman, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "The task waits for a person's answer; stop working on it until the answer arrives.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReasonRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    reason: String,
}

pub fn return_task(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: ReasonRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let to = workflow(tx, &task.project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        let fields = BTreeMap::from([("reason".to_string(), req.reason.clone())]);
        transition(tx, actor, &task, Action::Return, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "The task went back to Backlog as blocked; a person will triage it.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Subtask {
    #[serde(default)]
    title: String,
    prompt: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct DecomposeRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    subtasks: Vec<Subtask>,
    #[serde(default)]
    ordered: bool,
}

pub fn decompose(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: DecomposeRequest = parse(&request)?;
    if req.subtasks.len() < 2 || req.subtasks.len() > 20 {
        return refuse("too_few_subtasks", "decompose into 2 to 20 subtasks");
    }
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        if task.assignee.as_ref() != Some(&me) {
            return refuse("not_assigned", "this task is not assigned to your session");
        }
        let ready = workflow(tx, &task.project)?.first(Role::Ready).map(|s| s.name.clone()).unwrap_or_default();
        let mut children: Vec<String> = Vec::new();
        for sub in &req.subtasks {
            let follows: BTreeSet<String> = match (req.ordered, children.last()) {
                (true, Some(previous)) => BTreeSet::from([previous.clone()]),
                _ => BTreeSet::new(),
            };
            let fields = BTreeMap::from([("decomposed_from".to_string(), uuid.clone())]);
            let rank = next_rank(tx, &task.project, &ready)?;
            let payload = created_payload(&sub.title, &sub.prompt, &ready, Some(task.priority), &[], None, &uuid, follows, vec![], fields, rank)?;
            let child = uuid::Uuid::new_v4().to_string();
            prove(tx, Draft { actor: actor.clone(), task: child.clone(), project: task.project.clone(), kind: "task_created".into(), base: String::new(), request_id: String::new(), payload })?;
            children.push(child);
        }
        let backlog = workflow(tx, &task.project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        transition(tx, actor, &task, Action::Decompose, &backlog, BTreeMap::new(), None, children.clone(), &req.request_id)?;
        let views = children
            .iter()
            .map(|c| Ok(view(&load(tx, c)?, &reference(tx, c)?, false)))
            .collect::<Result<Vec<_>>>()?;
        let mut out = result(tx, &uuid, "The subtasks are in Queue; the original task is now an epic container.")?;
        out["children"] = json!(views);
        Ok(out)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FileRequest {
    #[serde(default)]
    request_id: String,
    prompt: String,
    #[serde(default)]
    breaks: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    related: String,
    #[serde(default)]
    project: Option<String>,
}

fn my_tasks(tx: &Tx, me: &SessionRef) -> Result<Vec<Task>> {
    let events = tx.load(&Filter::All)?;
    Ok(fold_many(&events).into_values().filter(|t| !t.deleted && t.assignee.as_ref() == Some(me)).collect())
}

pub fn file(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: FileRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let related = if req.related.is_empty() { String::new() } else { resolve(tx, &req.related)? };
        let project = match (&req.project, related.is_empty()) {
            (Some(project), _) => project.clone(),
            (None, false) => load(tx, &related)?.project,
            (None, true) => {
                let mut own = my_tasks(tx, &me)?;
                if own.len() != 1 {
                    return Err(Refusal::field("missing_field", "project", "missing", "name the project or the related task").into());
                }
                own.remove(0).project
            }
        };
        let backlog = workflow(tx, &project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        let mut fields = BTreeMap::new();
        if !req.breaks.trim().is_empty() {
            fields.insert("breaks".to_string(), req.breaks.clone());
        }
        let rank = next_rank(tx, &project, &backlog)?;
        let payload = created_payload(&req.title, &req.prompt, &backlog, None, &[], None, "", BTreeSet::new(), vec![], fields, rank)?;
        let uuid = uuid::Uuid::new_v4().to_string();
        prove(tx, Draft { actor: actor.clone(), task: uuid.clone(), project: project.clone(), kind: "task_created".into(), base: String::new(), request_id: req.request_id.clone(), payload })?;
        if !related.is_empty() {
            let created = load(tx, &uuid)?;
            prove(tx, Draft { actor: actor.clone(), task: uuid.clone(), project: created.project.clone(), kind: "relation_set".into(), base: String::new(), request_id: String::new(), payload: json!({"relation": "related", "target": related, "present": true}) })?;
        }
        result(tx, &uuid, "Filed in Backlog for a person to triage. Continue with your current task.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AttachRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    name: String,
    mime: String,
    data_base64: String,
    #[serde(default)]
    note: String,
}

pub fn attach(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: AttachRequest = parse(&request)?;
    check_text("note", &req.note, MAX_FIELD, true)?;
    let stored = store_attachments(ctx.store, &[NewAttachment { name: req.name.clone(), mime: req.mime.clone(), data_base64: req.data_base64.clone() }])?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        if task.assignee.as_ref() != Some(&me) && task.reviewer.as_ref() != Some(&me) {
            return refuse("not_assigned", "this task is not assigned to your session");
        }
        if task.attachments.len() >= 8 {
            return Err(Refusal::field("invalid_field", "attachments", "invalid", "at most 8 images per task").into());
        }
        let attachment = &stored[0];
        let draft = |kind: &str, payload: Value| Draft { actor: actor.clone(), task: uuid.clone(), project: task.project.clone(), kind: kind.into(), base: String::new(), request_id: String::new(), payload };
        prove(tx, draft("attachment_added", json!({"attachment": attachment})))?;
        let text = format!("{} ({}, {} bytes){}", attachment.name, attachment.mime, attachment.bytes, if req.note.is_empty() { String::new() } else { format!("\n{}", req.note) });
        prove(tx, draft("journal", json!({"marker": "[attach]", "text": text})))?;
        result(tx, &uuid, "Attached; reviewers can open it from the task.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReviewRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    verdict: String,
    report: String,
}

pub fn review(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: ReviewRequest = parse(&request)?;
    let action = match req.verdict.as_str() {
        "approve" => Action::Approve,
        "needs_work" => Action::NeedsWork,
        _ => return Err(Refusal::field("invalid_field", "verdict", "invalid", "verdict is approve or needs_work").into()),
    };
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to = if action == Action::Approve {
            task.stage.clone()
        } else {
            wf.rules.iter().find(|r| r.action == Action::NeedsWork && r.from.contains(&task.stage)).map(|r| r.to.clone()).unwrap_or_default()
        };
        let fields = BTreeMap::from([("report".to_string(), req.report.clone())]);
        transition(tx, actor, &task, action, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "Verdict recorded. A person signs the task off.")
    })
}
```

(If the compiler reports an unused import, remove it; the test module imports what only the tests use.)

- [ ] **Step 6: Use identity in the CLI and route agent commands**

In `src/overmind/cli.rs` replace

```rust
    let actor = Actor::human(&machine);
    let cwd = None;
```

with

```rust
    let (actor, cwd) = super::identity::caller(&machine)?;
```

and add these arms before the catch-all `_ =>` arm:

```rust
        ("mine", []) => agent::mine(&mut ctx)?,
        ("advance", []) => agent::advance(&mut ctx, request)?,
        ("call-human", []) => agent::call_human(&mut ctx, request)?,
        ("return", []) => agent::return_task(&mut ctx, request)?,
        ("decompose", []) => agent::decompose(&mut ctx, request)?,
        ("file", []) => agent::file(&mut ctx, request)?,
        ("attach", []) => agent::attach(&mut ctx, request)?,
        ("review", []) => agent::review(&mut ctx, request)?,
```

with `use super::agent;` at the top. Add `mod agent; mod identity;` to `src/overmind/mod.rs`.

- [ ] **Step 7: Extend the Python test**

Add to class `Overmind` in `tests/test_overmind.py`:

```python
    def test_session_environment_must_match_a_live_session(self):
        env = dict(self.env, HGS_SESSION='claude/demo/w1', HGS_RUN_ID='run-that-does-not-exist')
        result = self.run_hgs('mine', env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn('does not match a live Zerus session', result.stderr)

    def test_people_cannot_use_agent_commands(self):
        created = self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})
        error = self.refused('advance', '--json', data={'task': created['task']['ref'], 'to': 'Build', 'spec': 's'})
        self.assertEqual(error['code'], 'agent_only')
```

- [ ] **Step 8: Run all checks for this task**

```bash
cargo test --locked overmind:: 2>&1 | tail -5
cargo build --locked 2>&1 | tail -2
HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind -v 2>&1 | tail -8
```
Expected: all Rust tests PASS; 6 Python tests OK.

- [ ] **Step 9: Commit**

```bash
git add src/overmind/identity.rs src/overmind/agent.rs src/overmind/mod.rs src/overmind/cli.rs src/state/mod.rs tests/test_overmind.py
git commit --only src/overmind/identity.rs src/overmind/agent.rs src/overmind/mod.rs src/overmind/cli.rs src/state/mod.rs tests/test_overmind.py \
  -m "Let agents in verified Zerus sessions advance, park, return, decompose, file and review Overmind tasks"
```

---

### Task 7: Documentation, spec corrections and full checks

**Files:**
- Modify: `docs/reference.md` (new section), `docs/superpowers/specs/2026-10-11-overmind-core-design.md`, `src/overmind/mod.rs` (drop `#![allow(dead_code)]`)

- [ ] **Step 1: Remove the dead-code allowance**

Delete the `#![allow(dead_code)]` line from `src/overmind/mod.rs`. Run `cargo build --locked 2>&1 | grep -A3 warning | head -40`. Items still unused are parts of the 1b contract (`Store::missing`, `insert_raw`, `version_vector`, `touched_since` users, `Tx::append`, `ActorKind::Autopilot`/`Manager`, `Actor::device`). For each remaining warning add a narrow `#[allow(dead_code)] // Used by replication (plan 1b).` on that item rather than a module-wide allowance. No other warnings may remain.

- [ ] **Step 2: Correct the spec**

In `docs/superpowers/specs/2026-10-11-overmind-core-design.md`:
- Part 1, `uuid` row: replace "UUIDv7, or the client `request_id` when created from the phone." with "UUID v4, or the client `request_id` of the creating request (the phone and any idempotent client)."
- Part 1, actors table, `agent:<session>` row: replace "and the caller descends from the session pane, the same check hooks use." with "and that run has a live, matching tmux pane: the same check hook events pass."
- Part 2, "Local store": replace the path line with "`<state root>/overmind/` (`$HGS_STATE_DIR/overmind`, by default `~/.local/state/hgs/sessions/overmind`), mode 0700:"

- [ ] **Step 3: Document the CLI in `docs/reference.md`**

Insert before `## JSON and external clients`:

```markdown
## Overmind tasks

Overmind keeps tasks whose content is a ready prompt. Tasks belong to a project
(or are pinned to a session), move through the project's workflow, and keep a
journal of every step. The default workflow is Backlog → Queue → Design → Build →
Review → Done, with Your Call for questions that need a person and Icebox for
parked work. Only a person can move a task to Done.

`hgs overmind --help` lists the commands. Requests are JSON on stdin with
`--json`; replies are JSON. A refusal exits 1 and prints
`{"ok": false, "error": {"code", "message", "field", "state"}}`, where `state`
says whether a field was missing, empty or invalid. A write that carries a
`request_id` UUID can be repeated safely: the same request returns the first
result, a different request with the same ID is refused.

Tasks are referenced by `om-` plus at least five characters, or by UUID. A
reference grows when needed to stay unambiguous.

Agents in Zerus sessions use their own commands (`mine`, `show`, `comment`,
`advance`, `call-human`, `return`, `decompose`, `file`, `attach`, `review`).
Their session is verified like hook events: `HGS_SESSION` and `HGS_RUN_ID` must
name a live session. An agent may act only on tasks assigned to its session;
in a Git repository, the evidence for review is a commit that exists there.

Data lives in `$HGS_STATE_DIR/overmind/` (an SQLite event log, attachment
blobs by SHA-256 and a `rev` file that changes with every write). Replication
between machines, sending tasks into sessions and the desktop and phone
interfaces are described in `docs/superpowers/specs/2026-10-11-overmind-core-design.md`.
```

- [ ] **Step 4: Run the complete checks**

```bash
cargo test --locked 2>&1 | tail -3
cargo build --locked 2>&1 | grep -c warning
HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind 2>&1 | tail -3
python3 scripts/check-source-privacy.py
python3 scripts/ci/check-source.py 2>&1 | tail -5
```
Expected: all Rust tests pass (the whole crate, not only Overmind); `0` warnings; Python OK; privacy guard 0 findings; source check passes. If `check-source.py` needs tools that are missing locally, record which check could not run.

- [ ] **Step 5: Commit**

```bash
git add docs/reference.md docs/superpowers/specs/2026-10-11-overmind-core-design.md src/overmind/
git commit --only docs/reference.md docs/superpowers/specs/2026-10-11-overmind-core-design.md src/overmind/ \
  -m "Document hgs overmind and align the Overmind spec with the local tracker"
```
