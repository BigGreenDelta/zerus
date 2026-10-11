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
        let machine = self.machine.clone();
        let value = {
            let tx = self.conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
            let mut wrapped = Tx { tx, machine };
            let value = f(&mut wrapped)?;
            wrapped.tx.commit()?;
            value
        };
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
