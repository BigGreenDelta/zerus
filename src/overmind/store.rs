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
  n INTEGER PRIMARY KEY AUTOINCREMENT,
  origin TEXT NOT NULL, seq INTEGER NOT NULL, id TEXT NOT NULL UNIQUE,
  lamport INTEGER NOT NULL, task TEXT NOT NULL, project TEXT NOT NULL,
  kind TEXT NOT NULL, raw TEXT NOT NULL, UNIQUE(origin, seq));
CREATE INDEX IF NOT EXISTS events_task ON events(task);
CREATE INDEX IF NOT EXISTS events_project ON events(project);
CREATE INDEX IF NOT EXISTS events_lamport ON events(lamport);
CREATE TABLE IF NOT EXISTS receipts(
  request_id TEXT PRIMARY KEY, digest TEXT NOT NULL, result TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS peers(
  machine TEXT PRIMARY KEY, alias TEXT NOT NULL, vector TEXT,
  last_ok INTEGER NOT NULL, last_error TEXT NOT NULL, updated INTEGER NOT NULL);
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

/// What `insert_raw` did with a batch from another machine.
#[derive(Debug, Default, PartialEq)]
pub struct Received {
    pub inserted: usize,
    /// Damaged events, out-of-range positions and ids stored at another position.
    pub refused: usize,
}

/// The largest `seq` and `lamport`: exact in every JSON reader, including the
/// desktop and the phone. Real logs never come near it.
pub const MAX_POSITION: u64 = (1 << 53) - 1;

/// Identities are UUIDs, and positions are at most `MAX_POSITION` on every
/// machine alike.
fn storable(event: &Event) -> bool {
    uuid::Uuid::parse_str(&event.id).is_ok()
        && uuid::Uuid::parse_str(&event.origin).is_ok()
        && (1..=MAX_POSITION).contains(&event.seq)
        && event.lamport <= MAX_POSITION
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
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
        let conn = Connection::open(&path).with_context(|| format!("open {}", path.display()))?;
        conn.busy_timeout(Duration::from_secs(10))?;
        conn.query_row("PRAGMA journal_mode=WAL", [], |_| Ok(()))
            .context("the Overmind database is damaged")?;
        conn.execute_batch("PRAGMA synchronous=FULL;")?;
        conn.execute_batch(SCHEMA).context("the Overmind database is damaged")?;
        Ok(Self { conn, dir: dir.to_owned(), machine: machine.to_owned() })
    }

    pub fn machine(&self) -> &str {
        &self.machine
    }

    /// How long a write waits for other writers (10 seconds after `open`).
    pub fn wait_for_writers(&self, timeout: Duration) -> Result<()> {
        self.conn.busy_timeout(timeout)?;
        Ok(())
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

    /// Local insertion position (`n`); increases with every stored event.
    pub fn cursor(&self) -> Result<i64> {
        Ok(self.conn.query_row("SELECT COALESCE(MAX(n), 0) FROM events", [], |row| row.get(0))?)
    }

    /// Tasks with events stored after `cursor`. The empty string stands for shared events.
    pub fn touched_since(&self, cursor: i64) -> Result<BTreeSet<String>> {
        let mut statement = self.conn.prepare("SELECT DISTINCT task FROM events WHERE n > ?1")?;
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

    /// Raw events the peer lacks, up to about `max_bytes`, taking one event of
    /// each origin in turn (each origin in `seq` order), so an origin the peer
    /// cannot accept never holds back the others. The flag says whether more remain.
    pub fn missing(&self, peer: &BTreeMap<String, u64>, max_bytes: usize) -> Result<(Vec<Value>, bool)> {
        let mut pending: Vec<(String, i64)> = self
            .conn
            .prepare("SELECT DISTINCT origin FROM events ORDER BY origin")?
            .query_map([], |row| row.get::<_, String>(0))?
            .collect::<rusqlite::Result<Vec<_>>>()?
            .into_iter()
            .map(|origin| {
                let known = peer.get(&origin).map_or(0, |seq| i64::try_from(*seq).unwrap_or(i64::MAX));
                (origin, known)
            })
            .collect();
        let mut statement =
            self.conn.prepare("SELECT seq, raw FROM events WHERE origin = ?1 AND seq > ?2 ORDER BY seq LIMIT 1")?;
        let mut out = Vec::new();
        let mut bytes = 0;
        while !pending.is_empty() {
            let mut next = Vec::new();
            for (origin, after) in pending {
                let row = statement
                    .query_row(params![origin, after], |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?)))
                    .optional()?;
                let Some((seq, raw)) = row else { continue };
                if !out.is_empty() && bytes + raw.len() > max_bytes {
                    return Ok((out, true));
                }
                bytes += raw.len();
                out.push(serde_json::from_str(&raw)?);
                next.push((origin, seq));
            }
            pending = next;
        }
        Ok((out, false))
    }

    /// Inserts events from another machine. Known events are skipped, and an
    /// origin's events after a gap are left for a later exchange. A refused
    /// event is never stored, so its origin stops there; the rest of the batch
    /// is still inserted.
    pub fn insert_raw(&mut self, raws: &[Value]) -> Result<Received> {
        let mut received = Received::default();
        let mut events = Vec::new();
        for raw in raws {
            match serde_json::from_value::<Event>(raw.clone()) {
                Ok(event) if storable(&event) => events.push((event, raw.to_string())),
                _ => received.refused += 1,
            }
        }
        events.sort_by(|a, b| (a.0.origin.as_str(), a.0.seq).cmp(&(b.0.origin.as_str(), b.0.seq)));
        let tx = self.conn.transaction_with_behavior(TransactionBehavior::Immediate)?;
        for (event, raw) in &events {
            let max: i64 = tx.query_row(
                "SELECT COALESCE(MAX(seq), 0) FROM events WHERE origin = ?1",
                params![event.origin],
                |row| row.get(0),
            )?;
            if event.seq != max as u64 + 1 {
                continue;
            }
            // The same id at another position is a damaged or forged copy.
            let reused: bool =
                tx.query_row("SELECT EXISTS(SELECT 1 FROM events WHERE id = ?1)", params![event.id], |row| row.get(0))?;
            if reused {
                received.refused += 1;
                continue;
            }
            insert(&tx, event, raw)?;
            received.inserted += 1;
        }
        tx.commit()?;
        if received.inserted > 0 {
            self.bump_quietly();
        }
        Ok(received)
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
        self.bump_quietly();
        Ok(value)
    }

    /// Publishes the cursor in `rev` so that watchers notice changes.
    fn bump(&self) -> Result<()> {
        use fs2::FileExt;
        let lock = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .mode(0o600)
            .open(self.dir.join("rev.lock"))?;
        lock.lock_exclusive()?;
        let mut temp = tempfile::Builder::new().prefix(".rev-").tempfile_in(&self.dir)?;
        temp.write_all(self.cursor()?.to_string().as_bytes())?;
        temp.as_file().set_permissions(fs::Permissions::from_mode(0o600))?;
        temp.as_file().sync_all()?;
        temp.persist(self.dir.join("rev")).map_err(|e| e.error)?;
        Ok(())
    }

    /// The write is already committed, so a failed publication is only reported.
    fn bump_quietly(&self) {
        if let Err(error) = self.bump() {
            eprintln!("hgs overmind: could not update rev: {error:#}");
        }
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
        ensure!(
            seq as u64 <= MAX_POSITION && lamport as u64 <= MAX_POSITION,
            "the Overmind event clock is exhausted: an event from another machine set it to its limit"
        );
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
        assert_eq!(store.insert_raw(&all[1..]).unwrap(), received(0, 0));
        assert_eq!(store.insert_raw(&all).unwrap(), received(3, 0));
        assert_eq!(store.insert_raw(&all).unwrap(), received(0, 0));
        let local = store.write(|tx| tx.append(&draft("t"))).unwrap();
        assert!(local.lamport > 3);
        let (rest, _) = store.missing(&source.version_vector().unwrap(), usize::MAX).unwrap();
        assert_eq!(rest.len(), 1);
    }

    fn received(inserted: usize, refused: usize) -> Received {
        Received { inserted, refused }
    }

    /// Events of OTHER, as a peer would send them.
    fn remote(count: usize) -> Vec<Value> {
        let dir = tempfile::tempdir().unwrap();
        let mut source = Store::open(dir.path(), OTHER).unwrap();
        for _ in 0..count {
            source.write(|tx| tx.append(&draft("t"))).unwrap();
        }
        source.missing(&BTreeMap::new(), usize::MAX).unwrap().0
    }

    #[test]
    fn missing_skips_what_the_peer_has_per_origin() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        for _ in 0..3 {
            store.write(|tx| tx.append(&draft("t"))).unwrap();
        }
        store.insert_raw(&remote(2)).unwrap();
        let positions = |peer: BTreeMap<String, u64>| -> Vec<(String, u64)> {
            let (events, more) = store.missing(&peer, usize::MAX).unwrap();
            assert!(!more);
            events.iter().map(|e| (e["origin"].as_str().unwrap().to_owned(), e["seq"].as_u64().unwrap())).collect()
        };
        let peer = BTreeMap::from([(MACHINE.to_owned(), 2), (OTHER.to_owned(), 2)]);
        assert_eq!(positions(peer), vec![(MACHINE.to_owned(), 3)]);
        let beyond = BTreeMap::from([(MACHINE.to_owned(), u64::MAX)]);
        assert_eq!(positions(beyond), vec![(OTHER.to_owned(), 1), (OTHER.to_owned(), 2)]);
        // Origins take turns, so a long backlog of one never crowds out the others.
        let (machine, other) = (MACHINE.to_owned(), OTHER.to_owned());
        assert_eq!(
            positions(BTreeMap::new()),
            vec![(machine.clone(), 1), (other.clone(), 1), (machine.clone(), 2), (other, 2), (machine, 3)]
        );
    }

    #[test]
    fn refused_events_never_stop_the_rest_of_a_batch() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let local = store.write(|tx| tx.append(&draft("t"))).unwrap();
        let theirs = remote(2);
        let copy = |origin: &str, seq: u64, lamport: u64, id: &str| {
            let mut event = theirs[0].clone();
            event["origin"] = json!(origin);
            event["seq"] = json!(seq);
            event["lamport"] = json!(lamport);
            event["id"] = json!(id);
            event
        };
        let fresh = || uuid::Uuid::new_v4().to_string();
        let origins: Vec<String> = (0..3).map(|_| fresh()).collect();
        let mut batch = vec![
            copy(&origins[0], 1, 1, &local.id),
            copy(&origins[1], MAX_POSITION + 1, 1, &fresh()),
            copy(&origins[2], 1, MAX_POSITION + 1, &fresh()),
            json!({"id": "damaged"}),
        ];
        batch.extend(theirs.iter().cloned());
        assert_eq!(store.insert_raw(&batch).unwrap(), received(2, 4));
        let vector = store.version_vector().unwrap();
        assert_eq!(vector, BTreeMap::from([(MACHINE.to_owned(), 1), (OTHER.to_owned(), 2)]));
        // A clock pushed to the limit fails local writes with a clear error, never an overflow.
        let edge = copy(&origins[0], 1, MAX_POSITION - 1, &fresh());
        assert_eq!(store.insert_raw(&[edge]).unwrap(), received(1, 0));
        assert_eq!(store.write(|tx| tx.append(&draft("t"))).unwrap().lamport, MAX_POSITION);
        let error = store.write(|tx| tx.append(&draft("t"))).unwrap_err();
        assert!(format!("{error:#}").contains("exhausted"), "{error:#}");
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
    fn the_cursor_counts_local_insertions() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        assert_eq!(store.cursor().unwrap(), 0);
        store.write(|tx| tx.append(&draft("a"))).unwrap();
        let cursor = store.cursor().unwrap();
        store.write(|tx| tx.append(&draft("b"))).unwrap();
        assert_eq!(store.cursor().unwrap(), cursor + 1);
        assert_eq!(store.touched_since(cursor).unwrap(), BTreeSet::from(["b".to_string()]));
        let columns: Vec<String> = store
            .conn
            .prepare("SELECT name FROM pragma_table_info('events')")
            .unwrap()
            .query_map([], |row| row.get(0))
            .unwrap()
            .collect::<rusqlite::Result<_>>()
            .unwrap();
        assert_eq!(columns[0], "n");
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
        let error = Store::open(broken.path(), MACHINE).err().expect("garbage file must fail");
        assert!(format!("{error:#}").contains("damaged"));
    }

    #[test]
    fn a_failed_rev_publication_does_not_fail_a_committed_write() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        fs::create_dir(dir.path().join("rev")).unwrap();
        store.write(|tx| tx.append(&draft("t"))).unwrap();
        assert_eq!(store.load(&Filter::All).unwrap().len(), 1);
    }
}

/// What this machine learned about a peer in exchanges. Local only.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PeerState {
    pub machine: String,
    pub alias: String,
    /// The peer's version vector after the last exchange; None before the first.
    pub vector: Option<BTreeMap<String, u64>>,
    pub last_ok: u64,
    pub last_error: String,
    pub updated: u64,
}

fn peer_row(row: &rusqlite::Row) -> rusqlite::Result<PeerState> {
    let vector: Option<String> = row.get(2)?;
    Ok(PeerState {
        machine: row.get(0)?,
        alias: row.get(1)?,
        vector: vector.and_then(|v| serde_json::from_str(&v).ok()),
        last_ok: row.get::<_, i64>(3)? as u64,
        last_error: row.get(4)?,
        updated: row.get::<_, i64>(5)? as u64,
    })
}

impl Store {
    pub fn peer(&self, machine: &str) -> Result<Option<PeerState>> {
        Ok(self
            .conn
            .query_row("SELECT machine, alias, vector, last_ok, last_error, updated FROM peers WHERE machine = ?1", params![machine], peer_row)
            .optional()?)
    }

    pub fn peers(&self) -> Result<Vec<PeerState>> {
        let mut statement = self.conn.prepare("SELECT machine, alias, vector, last_ok, last_error, updated FROM peers ORDER BY machine")?;
        let peers = statement.query_map([], peer_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(peers)
    }

    pub fn put_peer(&self, peer: &PeerState) -> Result<()> {
        let vector = peer.vector.as_ref().map(serde_json::to_string).transpose()?;
        self.conn.execute(
            "INSERT OR REPLACE INTO peers(machine, alias, vector, last_ok, last_error, updated) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![peer.machine, peer.alias, vector, peer.last_ok as i64, peer.last_error, peer.updated as i64],
        )?;
        Ok(())
    }
}
