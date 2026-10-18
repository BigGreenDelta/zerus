//! Bounded private ledger. No native receipts or agent state live in this DB.
use super::model::*;
use anyhow::{ensure, Context, Result};
use chrono::DateTime;
use rusqlite::{
    params, Connection, OpenFlags, OptionalExtension, Transaction, TransactionBehavior,
};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{fs, path::Path, time::Duration};

const DAY: i64 = 86_400_000;
pub const EVENT_DAYS: i64 = 14;
pub const MAX_EVENTS: i64 = 200_000;
const MAX_SCOPES: i64 = 10_000;
const MAX_PAGES: i64 = 32_768; // 128 MiB, rollback journal bounded by one transaction.
pub const NIL_EPOCH: &str = "00000000-0000-0000-0000-000000000000";

pub struct Store {
    db: Connection,
}
fn date(ms: i64) -> Result<String> {
    Ok(DateTime::from_timestamp_millis(ms)
        .context("invalid analytics time")?
        .format("%Y-%m-%d")
        .to_string())
}

impl Store {
    pub fn open(root: &Path, create: bool, writable: bool) -> Result<Option<Self>> {
        Self::open_with_timeout(
            root,
            create,
            writable,
            Duration::from_millis(if create { 500 } else { 50 }),
        )
    }

    /// Best-effort observers never create storage or wait for a competing writer.
    pub(crate) fn open_observer(root: &Path) -> Result<Option<Self>> {
        Self::open_with_timeout(root, false, true, Duration::ZERO)
    }

    fn open_with_timeout(
        root: &Path,
        create: bool,
        writable: bool,
        timeout: Duration,
    ) -> Result<Option<Self>> {
        let path = root.join("analytics.sqlite3");
        if !path.exists() && !create {
            return Ok(None);
        }
        if create {
            crate::platform::private_dir(root)?;
        }
        ensure!(
            crate::platform::verify_private(root, crate::platform::Private::Directory)?,
            "Analytics directory must be private"
        );
        if create {
            let file = crate::platform::refuse_link(crate::platform::private_open(
                fs::OpenOptions::new()
                    .create(true)
                    .truncate(false)
                    .read(true)
                    .write(true),
            ))
            .open(&path)?;
            ensure!(
                crate::platform::verify_private_file(&file)?,
                "Analytics file must be private"
            );
        } else {
            let file =
                crate::platform::refuse_link(fs::OpenOptions::new().read(true)).open(&path)?;
            ensure!(
                crate::platform::verify_private_file(&file)?,
                "Analytics file must be private"
            );
        }
        let flags = if writable {
            OpenFlags::SQLITE_OPEN_READ_WRITE
        } else {
            OpenFlags::SQLITE_OPEN_READ_ONLY
        };
        let mut db = Connection::open_with_flags(path, flags | OpenFlags::SQLITE_OPEN_NO_MUTEX)?;
        db.busy_timeout(timeout)?;
        if writable {
            db.pragma_update(None, "secure_delete", true)?;
        }
        let version: i64 = db.query_row("PRAGMA user_version", [], |r| r.get(0))?;
        ensure!(
            version == 1 || (version == 0 && create),
            "Unsupported analytics schema"
        );
        if create && version == 0 {
            db.execute_batch(
                "PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
            )?;
            let tx = db.transaction_with_behavior(TransactionBehavior::Immediate)?;
            // Another creator may have initialized it while this connection waited.
            let locked_version: i64 = tx.query_row("PRAGMA user_version", [], |r| r.get(0))?;
            ensure!(
                locked_version == 0 || locked_version == 1,
                "Unsupported analytics schema"
            );
            if locked_version == 0 {
                tx.execute_batch("CREATE TABLE metadata(id INTEGER PRIMARY KEY CHECK(id=1),epoch TEXT NOT NULL,revision INTEGER NOT NULL DEFAULT 0,
                enabled INTEGER NOT NULL DEFAULT 0,retention INTEGER NOT NULL DEFAULT 90,boundary INTEGER NOT NULL DEFAULT 0,
                first_at INTEGER,last_at INTEGER,event_rows INTEGER NOT NULL DEFAULT 0,scope_rows INTEGER NOT NULL DEFAULT 0,usage_rows INTEGER NOT NULL DEFAULT 0,cursor_rows INTEGER NOT NULL DEFAULT 0);
              CREATE TABLE events(id BLOB PRIMARY KEY CHECK(length(id)=32),at INTEGER NOT NULL,fingerprint BLOB NOT NULL) WITHOUT ROWID;
              CREATE INDEX events_at ON events(at);
              CREATE TABLE daily(day TEXT NOT NULL,provider TEXT NOT NULL,producer TEXT NOT NULL,payload TEXT NOT NULL,
                PRIMARY KEY(day,provider,producer)) WITHOUT ROWID;
              CREATE TABLE intervals(scope BLOB NOT NULL,kind TEXT NOT NULL,generation BLOB NOT NULL,mono INTEGER NOT NULL,at INTEGER NOT NULL,owner BLOB NOT NULL,
                PRIMARY KEY(scope,kind)) WITHOUT ROWID;
              CREATE INDEX intervals_at ON intervals(at);
              CREATE INDEX intervals_owner ON intervals(owner,kind);
              CREATE TABLE usage(scope BLOB NOT NULL,epoch BLOB NOT NULL,at INTEGER NOT NULL,payload TEXT NOT NULL,
                active INTEGER NOT NULL CHECK(active IN (0,1)),PRIMARY KEY(scope,epoch)) WITHOUT ROWID;
              CREATE INDEX usage_at ON usage(at);
              CREATE UNIQUE INDEX usage_active ON usage(scope) WHERE active=1;
              CREATE TABLE cursors(source BLOB PRIMARY KEY CHECK(length(source)=32),fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),offset INTEGER NOT NULL CHECK(offset>=0),last_at INTEGER NOT NULL) WITHOUT ROWID;
              CREATE INDEX cursors_at ON cursors(last_at);
              CREATE TABLE gaps(code TEXT PRIMARY KEY,at INTEGER NOT NULL) WITHOUT ROWID;
              CREATE TABLE supported(provider TEXT NOT NULL,measure TEXT NOT NULL,PRIMARY KEY(provider,measure)) WITHOUT ROWID;
              CREATE TRIGGER event_add AFTER INSERT ON events BEGIN UPDATE metadata SET event_rows=event_rows+1 WHERE id=1; END;
              CREATE TRIGGER event_remove AFTER DELETE ON events BEGIN UPDATE metadata SET event_rows=event_rows-1 WHERE id=1; END;
              CREATE TRIGGER scope_add AFTER INSERT ON intervals BEGIN UPDATE metadata SET scope_rows=scope_rows+1 WHERE id=1; END;
              CREATE TRIGGER scope_remove AFTER DELETE ON intervals BEGIN UPDATE metadata SET scope_rows=scope_rows-1 WHERE id=1; END;
              CREATE TRIGGER usage_add AFTER INSERT ON usage BEGIN UPDATE metadata SET usage_rows=usage_rows+1 WHERE id=1; END;
              CREATE TRIGGER usage_remove AFTER DELETE ON usage BEGIN UPDATE metadata SET usage_rows=usage_rows-1 WHERE id=1; END;
              CREATE TRIGGER cursor_add AFTER INSERT ON cursors BEGIN UPDATE metadata SET cursor_rows=cursor_rows+1 WHERE id=1; END;
              CREATE TRIGGER cursor_remove AFTER DELETE ON cursors BEGIN UPDATE metadata SET cursor_rows=cursor_rows-1 WHERE id=1; END;
              PRAGMA user_version=1;")?;
                tx.execute(
                    "INSERT INTO metadata(id,epoch) VALUES(1,?)",
                    [uuid::Uuid::new_v4().to_string()],
                )?;
            }
            tx.commit()?;
        }
        if writable {
            db.pragma_update(None, "max_page_count", MAX_PAGES)?;
        }
        Ok(Some(Self { db }))
    }

    pub(crate) fn enabled(&self) -> Result<bool> {
        Ok(self
            .db
            .query_row("SELECT enabled FROM metadata WHERE id=1", [], |r| r.get(0))?)
    }

    /// Hook observers must not wait behind a configuration or maintenance writer.
    pub(crate) fn nonblocking_observer(&self) -> Result<()> {
        self.db.busy_timeout(Duration::ZERO)?;
        Ok(())
    }

    #[cfg(test)]
    pub(crate) fn cursor(&self, key: &Identity) -> Result<Option<(Identity, u64)>> {
        cursor(&self.db, key)
    }

    #[cfg(test)]
    pub(crate) fn save_cursor(
        &mut self,
        key: &Identity,
        fingerprint: &Identity,
        offset: u64,
        now: i64,
    ) -> Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let saved = save_cursor(&tx, key, fingerprint, offset, now)?;
        tx.commit()?;
        ensure!(saved, "Analytics cursor quota reached");
        Ok(())
    }

    /// One bounded native pass owns its accepted events and cursor atomically.
    /// The writer never waits. Preflight cursor capacity before accepting events.
    pub(crate) fn native_batch(
        &mut self,
        key: &Identity,
        now: i64,
    ) -> Result<Option<NativeBatch<'_>>> {
        self.nonblocking_observer()?;
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if !tx.query_row("SELECT enabled FROM metadata WHERE id=1", [], |r| {
            r.get::<_, bool>(0)
        })? {
            return Ok(None);
        }
        let exists: bool = tx.query_row(
            "SELECT EXISTS(SELECT 1 FROM cursors WHERE source=?)",
            [key.0.as_slice()],
            |r| r.get(0),
        )?;
        let count: i64 = tx.query_row("SELECT cursor_rows FROM metadata WHERE id=1", [], |r| {
            r.get(0)
        })?;
        if !exists && count >= MAX_SCOPES {
            gap(&tx, Gap::Quota, now)?;
            tx.commit()?;
            anyhow::bail!("Analytics cursor quota reached");
        }
        Ok(Some(NativeBatch { tx, usable: true }))
    }

    /// Called by the background recovery worker, including while collection is OFF.
    /// Summary reads never perform maintenance or mutate the ledger.
    pub fn maintain(&mut self, now: i64) -> Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        prune(&tx, now)?;
        tx.commit()?;
        Ok(())
    }

    pub fn configure(
        &mut self,
        expected: &str,
        enabled: bool,
        retention: u64,
        now: i64,
    ) -> Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let epoch: String =
            tx.query_row("SELECT epoch FROM metadata WHERE id=1", [], |r| r.get(0))?;
        ensure!(
            epoch == expected
                || (expected == NIL_EPOCH
                    && tx.query_row(
                        "SELECT revision=0 AND first_at IS NULL FROM metadata WHERE id=1",
                        [],
                        |r| r.get::<_, bool>(0)
                    )?),
            "Analytics configuration changed; refresh first"
        );
        ensure!((1..=90).contains(&retention), "Invalid analytics retention");
        let before: bool =
            tx.query_row("SELECT enabled FROM metadata WHERE id=1", [], |r| r.get(0))?;
        // Closing intervals/baselines at either boundary prevents counting OFF time.
        if before != enabled {
            tx.execute_batch("DELETE FROM intervals; DELETE FROM usage; DELETE FROM cursors;")?;
            gap(
                &tx,
                if enabled {
                    Gap::Observation
                } else {
                    Gap::Disabled
                },
                now,
            )?;
        }
        tx.execute("UPDATE metadata SET epoch=?,revision=revision+1,enabled=?,retention=?,boundary=max(boundary,?) WHERE id=1",
            params![uuid::Uuid::new_v4().to_string(),enabled,retention,if before!=enabled {now} else {0}])?;
        prune(&tx, now)?;
        tx.commit()?;
        Ok(())
    }

    pub fn purge(&mut self, expected: &str, now: i64) -> Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let epoch: String =
            tx.query_row("SELECT epoch FROM metadata WHERE id=1", [], |r| r.get(0))?;
        ensure!(
            epoch == expected,
            "Analytics history changed; refresh first"
        );
        tx.execute_batch("DELETE FROM events; DELETE FROM daily; DELETE FROM intervals; DELETE FROM usage; DELETE FROM cursors; DELETE FROM supported; DELETE FROM gaps;")?;
        tx.execute("UPDATE metadata SET epoch=?,revision=revision+1,boundary=?,first_at=NULL,last_at=NULL WHERE id=1",params![uuid::Uuid::new_v4().to_string(),now])?;
        gap(&tx, Gap::Purged, now)?;
        tx.commit()?;
        Ok(())
    }

    pub fn note_gap(&mut self, code: Gap, now: i64) -> Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.query_row("SELECT enabled FROM metadata WHERE id=1", [], |r| {
            r.get::<_, bool>(0)
        })? {
            gap(&tx, code, now)?;
        }
        tx.commit()?;
        Ok(())
    }

    pub fn record(&mut self, event: &Event, now: i64) -> Result<bool> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        match record(&tx, event, now)? {
            RecordOutcome::Skip => Ok(false),
            RecordOutcome::Commit(accepted) => {
                tx.commit()?;
                Ok(accepted)
            }
        }
    }

    pub fn summary(&self, machine: &str, from: &str, through: &str, now: i64) -> Result<Value> {
        // One read transaction pins metadata and every bucket to one revision.
        let tx = self.db.unchecked_transaction()?;
        let (epoch, revision, enabled, retention, first, last): (
            String,
            i64,
            bool,
            i64,
            Option<i64>,
            Option<i64>,
        ) = tx.query_row(
            "SELECT epoch,revision,enabled,retention,first_at,last_at FROM metadata WHERE id=1",
            [],
            |r| {
                Ok((
                    r.get(0)?,
                    r.get(1)?,
                    r.get(2)?,
                    r.get(3)?,
                    r.get(4)?,
                    r.get(5)?,
                ))
            },
        )?;
        let mut rows = Vec::new();
        let cutoff = date(now - (retention - 1) * DAY)?;
        let mut query=tx.prepare("SELECT day,provider,producer,payload FROM daily WHERE day>=? AND day<=? AND day>=? ORDER BY day,provider,producer LIMIT 1081")?;
        for row in query.query_map(params![from, through, cutoff], |r| {
            Ok((
                r.get::<_, String>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, String>(2)?,
                r.get::<_, String>(3)?,
            ))
        })? {
            let (day, provider, producer, payload) = row?;
            let mut value: Value = serde_json::from_str(&payload)?;
            value["date"] = json!(day);
            value["provider"] = json!(provider);
            value["producer"] = json!(producer);
            rows.push(value);
        }
        ensure!(rows.len() <= 1080, "Analytics bucket limit exceeded");
        let mut gaps: Vec<String> = tx
            .prepare("SELECT code FROM gaps ORDER BY code LIMIT 16")?
            .query_map([], |r| r.get(0))?
            .collect::<rusqlite::Result<_>>()?;
        if !enabled && !gaps.iter().any(|g| g == Gap::Disabled.name()) {
            gaps.push(Gap::Disabled.name().into());
        }
        if from < cutoff.as_str() && !gaps.iter().any(|g| g == Gap::Retention.name()) {
            gaps.push(Gap::Retention.name().into());
        }
        let mut providers = serde_json::Map::new();
        for provider in [
            Provider::Codex,
            Provider::Claude,
            Provider::Kimi,
            Provider::Dsh,
            Provider::Opencode,
            Provider::Other,
        ] {
            let supported: Vec<String> = tx
                .prepare(
                    "SELECT measure FROM supported WHERE provider=? ORDER BY measure LIMIT 16",
                )?
                .query_map([provider.name()], |r| r.get(0))?
                .collect::<rusqlite::Result<_>>()?;
            providers.insert(provider.name().into(),json!({"supported":supported,"partial":true,
                "unsupported":MEASURES.iter().filter(|m|!supported.iter().any(|s|s==**m)).collect::<Vec<_>>()}));
        }
        let unresolved: i64 = tx.query_row(
            "SELECT count(*) FROM intervals WHERE kind IN ('approval','input') AND at>=?",
            [now - DAY],
            |r| r.get(0),
        )?;
        Ok(
            json!({"schema":1,"machine_id":machine,"source":"agent","source_epoch":epoch,"revision":revision,"generated_at":now as f64/1000.0,
            "range":{"from":from,"through":through},"collection":{"enabled":enabled,"retention_days":retention,"event_retention_days":EVENT_DAYS},
            "coverage":{"start":first.map(|x|x as f64/1000.0),"end":last.map(|x|x as f64/1000.0),"gaps":gaps,"unsupported":["cost","background_jobs"],"partial":true,"providers":providers},
            "unresolved_waits":{"observed":unresolved,"complete":false},"buckets":rows}),
        )
    }
}

/// Private to the analytics native ingestion path. Drop rolls back the whole pass.
pub(crate) struct NativeBatch<'a> {
    tx: Transaction<'a>,
    usable: bool,
}
impl NativeBatch<'_> {
    pub(crate) fn cursor(&self, key: &Identity) -> Result<Option<(Identity, u64)>> {
        cursor(&self.tx, key)
    }
    pub(crate) fn record(&mut self, event: &Event, now: i64) -> Result<bool> {
        ensure!(
            self.usable,
            "Analytics batch cannot continue after a storage failure"
        );
        // Only a successfully released/rolled-back savepoint allows the pass to
        // commit. A cleanup failure must not retain a rejected event's counters.
        self.usable = false;
        let savepoint = self.tx.savepoint()?;
        let result = record(&savepoint, event, now);
        if matches!(&result, Ok(RecordOutcome::Commit(_))) {
            savepoint.commit()?;
        } else {
            savepoint.finish()?;
        }
        self.usable = true;
        result.map(|outcome| match outcome {
            RecordOutcome::Skip => false,
            RecordOutcome::Commit(accepted) => accepted,
        })
    }
    pub(crate) fn note_gap(&self, code: Gap, now: i64) -> Result<()> {
        gap(&self.tx, code, now)
    }
    pub(crate) fn save_cursor(
        &mut self,
        key: &Identity,
        fingerprint: &Identity,
        offset: u64,
        now: i64,
    ) -> Result<()> {
        ensure!(
            self.usable,
            "Analytics batch cannot continue after a storage failure"
        );
        self.usable = false;
        ensure!(
            save_cursor(&self.tx, key, fingerprint, offset, now)?,
            "Analytics cursor quota reached"
        );
        self.usable = true;
        Ok(())
    }
    pub(crate) fn commit(self) -> Result<()> {
        ensure!(
            self.usable,
            "Analytics batch cannot commit after a storage failure"
        );
        self.tx.commit()?;
        Ok(())
    }
}

fn cursor(db: &Connection, key: &Identity) -> Result<Option<(Identity, u64)>> {
    let row:Option<(Vec<u8>,u64)>=db.query_row(
            "SELECT fingerprint,offset FROM cursors WHERE source=? AND (SELECT enabled FROM metadata WHERE id=1)=1",
            [key.0.as_slice()],|r|Ok((r.get(0)?,r.get(1)?))).optional()?;
    row.map(|(fingerprint, offset)| {
        Ok((
            Identity(
                fingerprint
                    .try_into()
                    .map_err(|_| anyhow::anyhow!("Invalid analytics cursor fingerprint"))?,
            ),
            offset,
        ))
    })
    .transpose()
}
fn save_cursor(
    tx: &Connection,
    key: &Identity,
    fingerprint: &Identity,
    offset: u64,
    now: i64,
) -> Result<bool> {
    ensure!(
        offset <= i64::MAX as u64,
        "Analytics cursor offset exceeds storage bound"
    );
    if !tx.query_row("SELECT enabled FROM metadata WHERE id=1", [], |r| {
        r.get::<_, bool>(0)
    })? {
        return Ok(true);
    }
    let exists: bool = tx.query_row(
        "SELECT EXISTS(SELECT 1 FROM cursors WHERE source=?)",
        [key.0.as_slice()],
        |r| r.get(0),
    )?;
    let count: i64 = tx.query_row("SELECT cursor_rows FROM metadata WHERE id=1", [], |r| {
        r.get(0)
    })?;
    if !exists && count >= MAX_SCOPES {
        gap(tx, Gap::Quota, now)?;
        return Ok(false);
    }
    tx.execute("INSERT INTO cursors VALUES(?,?,?,?) ON CONFLICT(source) DO UPDATE SET fingerprint=excluded.fingerprint,offset=excluded.offset,last_at=excluded.last_at",
            params![key.0.as_slice(),fingerprint.0.as_slice(),offset,now])?;
    Ok(true)
}
enum RecordOutcome {
    Skip,
    Commit(bool),
}
fn record(tx: &Connection, event: &Event, now: i64) -> Result<RecordOutcome> {
    let (enabled, boundary): (bool, i64) = tx.query_row(
        "SELECT enabled,boundary FROM metadata WHERE id=1",
        [],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    if !enabled {
        return Ok(RecordOutcome::Skip);
    }
    // Dedup tombstones may expire only after events cease being admissible.
    if event.observed_ms < now - EVENT_DAYS * DAY
        || event.observed_ms < boundary
        || event.observed_ms > now + 300_000
    {
        return Ok(RecordOutcome::Skip);
    }
    prune(tx, now)?;
    // Observation clocks can change when native events are replayed by another
    // process. Only intrinsic source metadata establishes conflicting identity.
    let fingerprint = Sha256::digest(format!(
        "{:?}",
        (
            &event.scope,
            event.provider,
            event.producer,
            &event.correlation,
            event.observed_ms,
            &event.kind
        )
    ));
    let old: Option<Vec<u8>> = tx
        .query_row(
            "SELECT fingerprint FROM events WHERE id=?",
            [event.id.0.as_slice()],
            |r| r.get(0),
        )
        .optional()?;
    if let Some(old) = old {
        ensure!(
            old == fingerprint.as_slice(),
            "Conflicting analytics event identity"
        );
        return Ok(RecordOutcome::Skip);
    }
    let rows: i64 = tx.query_row("SELECT event_rows FROM metadata WHERE id=1", [], |r| {
        r.get(0)
    })?;
    if rows >= MAX_EVENTS {
        gap(tx, Gap::Quota, now)?;
        return Ok(RecordOutcome::Commit(false));
    }
    tx.execute(
        "INSERT INTO events(id,at,fingerprint) VALUES(?,?,?)",
        params![
            event.id.0.as_slice(),
            event.observed_ms,
            fingerprint.as_slice()
        ],
    )?;
    let day = date(event.observed_ms)?;
    if !matches!(event.kind, Kind::WaitResolved(_)) {
        if let Some(counter) = event.kind.counter() {
            add(tx, &day, event, counter, 1, None)?;
        }
    }
    match &event.kind {
        Kind::TurnStarted => {
            support(tx, event, "turns")?;
            start(tx, event, Interval::Work)?;
        }
        Kind::TurnCompleted | Kind::TurnInterrupted => {
            support(tx, event, "turns")?;
            finish(tx, event, Interval::Work)?;
        }
        Kind::WaitStarted(kind) => {
            // A turn spanning native approval/input is not all working time.
            // Until native work-resumption proof is available, omit that
            // work interval instead of counting the wait twice.
            if tx.execute(
                "DELETE FROM intervals WHERE owner=? AND kind='work'",
                [event.scope.0.as_slice()],
            )? > 0
            {
                gap(tx, Gap::Observation, event.observed_ms)?;
            }
            start(tx, event, *kind)?;
        }
        Kind::WaitResolved(kind) => {
            // Tool completion alone does not prove that permission was requested.
            // A consumed wait cannot be resolved twice through different hook names.
            let pending = match &event.correlation {
                Some(id) => tx.query_row(
                    "SELECT EXISTS(SELECT 1 FROM intervals WHERE scope=? AND kind=? AND owner=?)",
                    params![id.0.as_slice(), kind.key(), event.scope.0.as_slice()],
                    |r| r.get::<_, bool>(0),
                )?,
                None => false,
            };
            if pending {
                if let Some(counter) = event.kind.counter() {
                    add(tx, &day, event, counter, 1, None)?;
                }
            }
            finish(tx, event, *kind)?;
        }
        Kind::DeliverySubmitted | Kind::DeliveryFailed | Kind::DeliveryUncertain => {
            support(tx, event, "delivery")?
        }
        Kind::RecoveryAttempt | Kind::RecoverySucceeded | Kind::RecoveryExhausted => {
            support(tx, event, "recovery")?
        }
        Kind::Failure(_) => {
            support(tx, event, "failures")?;
            // A later completion cannot establish continuous work through a
            // failed/recovering interval. Never invalidate another owner/turn.
            let removed = if let Some(turn) = &event.correlation {
                tx.execute(
                    "DELETE FROM intervals WHERE owner=? AND kind='work' AND scope=? AND at<=?",
                    params![
                        event.scope.0.as_slice(),
                        turn.0.as_slice(),
                        event.observed_ms
                    ],
                )?
            } else {
                tx.execute(
                    "DELETE FROM intervals WHERE owner=? AND kind='work' AND at<=?",
                    params![event.scope.0.as_slice(), event.observed_ms],
                )?
            };
            if removed > 0 {
                gap(tx, Gap::Observation, event.observed_ms)?;
            }
        }
        Kind::Usage { epoch, counters } => usage(tx, event, epoch, counters, true)?,
        Kind::RequestUsage { counters } => usage(
            tx,
            event,
            &Identity::new(&["native-request-v1"]),
            counters,
            false,
        )?,
    }
    tx.execute("UPDATE metadata SET revision=revision+1,first_at=min(coalesce(first_at,?),?),last_at=max(coalesce(last_at,?),?) WHERE id=1",
            params![event.observed_ms,event.observed_ms,event.observed_ms,event.observed_ms])?;
    Ok(RecordOutcome::Commit(true))
}

pub fn empty(machine: &str, from: &str, through: &str, now: i64) -> Value {
    json!({"schema":1,"machine_id":machine,"source":"agent","source_epoch":NIL_EPOCH,"revision":0,"generated_at":now as f64/1000.0,
        "range":{"from":from,"through":through},"collection":{"enabled":false,"retention_days":90,"event_retention_days":EVENT_DAYS},
        "coverage":{"start":null,"end":null,"gaps":["collection_disabled"],"unsupported":MEASURES,"partial":true},"buckets":[]})
}
fn gap(tx: &Connection, code: Gap, at: i64) -> Result<()> {
    if tx.execute(
        "INSERT OR IGNORE INTO gaps(code,at) VALUES(?,?)",
        params![code.name(), at],
    )? > 0
    {
        tx.execute("UPDATE metadata SET revision=revision+1 WHERE id=1", [])?;
    }
    Ok(())
}
fn support(tx: &Connection, event: &Event, measure: &str) -> Result<()> {
    tx.execute(
        "INSERT OR IGNORE INTO supported VALUES(?,?)",
        params![event.provider.name(), measure],
    )?;
    Ok(())
}
fn prune(tx: &Connection, now: i64) -> Result<()> {
    // Indexed, capped work per callback even after a long offline interval.
    let removed = tx.execute(
        "DELETE FROM events WHERE id IN (SELECT id FROM events WHERE at<? ORDER BY at LIMIT 1000)",
        [now - EVENT_DAYS * DAY],
    )?;
    let expired=tx.execute("DELETE FROM intervals WHERE (scope,kind) IN (SELECT scope,kind FROM intervals WHERE at<? ORDER BY at LIMIT 1000)",[now-DAY])?;
    let usage=tx.execute("DELETE FROM usage WHERE (scope,epoch) IN (SELECT scope,epoch FROM usage WHERE at<? ORDER BY at LIMIT 1000)",[now-EVENT_DAYS*DAY])?;
    let cursors=tx.execute("DELETE FROM cursors WHERE source IN (SELECT source FROM cursors WHERE last_at<? ORDER BY last_at LIMIT 1000)",[now-EVENT_DAYS*DAY])?;
    let retention: i64 = tx.query_row("SELECT retention FROM metadata WHERE id=1", [], |r| {
        r.get(0)
    })?;
    let buckets = tx.execute(
        "DELETE FROM daily WHERE day<?",
        [date(now - (retention - 1) * DAY)?],
    )?;
    if removed > 0 || buckets > 0 {
        gap(tx, Gap::Retention, now)?;
    }
    if expired > 0 {
        gap(tx, Gap::Observation, now)?;
    }
    if removed + buckets + expired + usage + cursors > 0 {
        tx.execute("UPDATE metadata SET revision=revision+1 WHERE id=1", [])?;
    }
    Ok(())
}
fn add(
    tx: &Connection,
    day: &str,
    event: &Event,
    key: &str,
    delta: u64,
    histogram: Option<u64>,
) -> Result<()> {
    let old: Option<String> = tx
        .query_row(
            "SELECT payload FROM daily WHERE day=? AND provider=? AND producer=?",
            params![day, event.provider.name(), event.producer.name()],
            |r| r.get(0),
        )
        .optional()?;
    let mut value: Value = old
        .map(|v| serde_json::from_str(&v))
        .transpose()?
        .unwrap_or_else(|| json!({"counters":{},"histograms":{}}));
    let total = value["counters"][key]
        .as_u64()
        .unwrap_or(0)
        .checked_add(delta)
        .filter(|v| *v <= MAX_VALUE)
        .context("Analytics counter overflow")?;
    value["counters"][key] = json!(total);
    if let Some(duration) = histogram {
        let h = &mut value["histograms"][key];
        if !h.is_object() {
            *h = json!({"version":1,"bounds":HISTOGRAM_BOUNDS,"counts":[0,0,0,0,0,0],"count":0,"sum":0});
        }
        let index = HISTOGRAM_BOUNDS
            .iter()
            .position(|b| duration <= *b)
            .unwrap_or(5);
        h["counts"][index] = json!(h["counts"][index].as_u64().unwrap_or(0) + 1);
        h["count"] = json!(h["count"].as_u64().unwrap_or(0) + 1);
        h["sum"] = json!(h["sum"]
            .as_u64()
            .unwrap_or(0)
            .checked_add(duration)
            .filter(|v| *v <= MAX_VALUE)
            .context("Analytics histogram overflow")?);
    }
    tx.execute("INSERT INTO daily VALUES(?,?,?,?) ON CONFLICT(day,provider,producer) DO UPDATE SET payload=excluded.payload",
        params![day,event.provider.name(),event.producer.name(),value.to_string()])?;
    Ok(())
}
fn start(tx: &Connection, event: &Event, kind: Interval) -> Result<()> {
    let Some(correlation) = &event.correlation else {
        return gap(tx, Gap::Identity, event.observed_ms);
    };
    let Some(clock) = &event.clock else {
        return gap(tx, Gap::Clock, event.observed_ms);
    };
    let existing: bool = tx.query_row(
        "SELECT EXISTS(SELECT 1 FROM intervals WHERE scope=? AND kind=?)",
        params![correlation.0.as_slice(), kind.key()],
        |r| r.get(0),
    )?;
    if existing {
        gap(tx, Gap::Observation, event.observed_ms)?;
    }
    let rows: i64 = tx.query_row("SELECT scope_rows FROM metadata WHERE id=1", [], |r| {
        r.get(0)
    })?;
    if !existing && rows >= MAX_SCOPES {
        return gap(tx, Gap::Quota, event.observed_ms);
    };
    ensure!(clock.monotonic_ms <= MAX_VALUE, "Invalid monotonic clock");
    tx.execute("INSERT INTO intervals VALUES(?,?,?,?,?,?) ON CONFLICT(scope,kind) DO UPDATE SET generation=excluded.generation,mono=excluded.mono,at=excluded.at,owner=excluded.owner",
        params![correlation.0.as_slice(),kind.key(),clock.generation.0.as_slice(),clock.monotonic_ms,event.observed_ms,event.scope.0.as_slice()])?;
    Ok(())
}
fn finish(tx: &Connection, event: &Event, kind: Interval) -> Result<()> {
    let Some(correlation) = &event.correlation else {
        return gap(tx, Gap::Identity, event.observed_ms);
    };
    let row: Option<(Vec<u8>, u64, i64)> = tx
        .query_row(
            "SELECT generation,mono,at FROM intervals WHERE scope=? AND kind=? AND owner=?",
            params![
                correlation.0.as_slice(),
                kind.key(),
                event.scope.0.as_slice()
            ],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    tx.execute(
        "DELETE FROM intervals WHERE scope=? AND kind=? AND owner=?",
        params![
            correlation.0.as_slice(),
            kind.key(),
            event.scope.0.as_slice()
        ],
    )?;
    let (Some((generation, mono, at)), Some(clock)) = (row, &event.clock) else {
        return gap(tx, Gap::Observation, event.observed_ms);
    };
    let Some(duration) = clock.monotonic_ms.checked_sub(mono) else {
        return gap(tx, Gap::Clock, event.observed_ms);
    };
    let wall = event.observed_ms - at;
    if generation != clock.generation.0
        || duration > DAY as u64
        || wall < 0
        || (wall - duration as i64).abs() > 5_000
    {
        return gap(tx, Gap::Clock, event.observed_ms);
    };
    support(tx, event, kind.metric())?;
    // Split exact monotonic duration proportionally at UTC midnight. Histogram
    // observes the entire interval once, on its resolution day.
    let mut cursor = at;
    let mut assigned = 0u64;
    while cursor < event.observed_ms {
        let end = ((cursor.div_euclid(DAY) + 1) * DAY).min(event.observed_ms);
        let cumulative = if end == event.observed_ms {
            duration
        } else {
            ((end - at) as u128 * duration as u128 / wall as u128) as u64
        };
        add(
            tx,
            &date(cursor)?,
            event,
            kind.metric(),
            cumulative - assigned,
            None,
        )?;
        assigned = cumulative;
        cursor = end;
    }
    add(
        tx,
        &date(event.observed_ms)?,
        event,
        kind.metric(),
        duration - assigned,
        Some(duration),
    )?;
    Ok(())
}
fn usage(
    tx: &Connection,
    event: &Event,
    epoch: &Identity,
    counters: &Tokens,
    baseline_first: bool,
) -> Result<()> {
    ensure!(
        counters
            .values()
            .iter()
            .all(|(_, n)| n.is_none_or(|n| n <= MAX_VALUE)),
        "Invalid token counter"
    );
    if counters.values().iter().all(|(_, n)| n.is_none()) {
        return gap(tx, Gap::Observation, event.observed_ms);
    };
    let previous: Option<(Vec<u8>, i64, String)> = tx
        .query_row(
            "SELECT epoch,at,payload FROM usage WHERE scope=? AND active=1",
            [event.scope.0.as_slice()],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    if previous
        .as_ref()
        .is_some_and(|(_, at, _)| event.observed_ms < *at)
    {
        return gap(tx, Gap::Observation, event.observed_ms);
    };
    let same_epoch = previous.as_ref().is_some_and(|(old, _, _)| *old == epoch.0);
    if !same_epoch {
        // Preserve bounded tombstones for retired epochs. Delayed old streams
        // must not reset their baselines and recount already observed usage.
        let retired: bool = tx.query_row(
            "SELECT EXISTS(SELECT 1 FROM usage WHERE scope=? AND epoch=?)",
            params![event.scope.0.as_slice(), epoch.0.as_slice()],
            |r| r.get(0),
        )?;
        if retired {
            return gap(tx, Gap::Observation, event.observed_ms);
        }
        let count: i64 = tx.query_row("SELECT usage_rows FROM metadata WHERE id=1", [], |r| {
            r.get(0)
        })?;
        if count >= MAX_SCOPES {
            return gap(tx, Gap::Quota, event.observed_ms);
        }
    }
    let mut saved = counters.clone();
    match previous {
        Some((old_epoch, _, raw)) if old_epoch == epoch.0 => {
            let old: Tokens = serde_json::from_str(&raw)?;
            if counters.values().iter().zip(old.values()).any(|((_,value),(_,before))|matches!((value,before),(Some(value),Some(before)) if *value<before)) {
                return gap(tx,Gap::Observation,event.observed_ms);
            }
            let mut comparable = false;
            for ((key, value), (_, before)) in counters.values().iter().zip(old.values()) {
                if let (Some(value), Some(before)) = (value, before) {
                    comparable = true;
                    let Some(delta) = value.checked_sub(before) else {
                        return gap(tx, Gap::Observation, event.observed_ms);
                    };
                    add(tx, &date(event.observed_ms)?, event, key, delta, None)?;
                } else if let Some(value) = value {
                    if baseline_first {
                        gap(tx, Gap::Observation, event.observed_ms)?;
                    } else {
                        comparable = true;
                        add(tx, &date(event.observed_ms)?, event, key, *value, None)?;
                    }
                }
            }
            saved.input_tokens = saved.input_tokens.or(old.input_tokens);
            saved.output_tokens = saved.output_tokens.or(old.output_tokens);
            saved.cache_tokens = saved.cache_tokens.or(old.cache_tokens);
            saved.reasoning_tokens = saved.reasoning_tokens.or(old.reasoning_tokens);
            if comparable {
                support(tx, event, "tokens")?;
            }
        }
        Some(_) => gap(tx, Gap::Observation, event.observed_ms)?,
        None => {
            if baseline_first {
                gap(tx, Gap::Observation, event.observed_ms)?;
            } else {
                for (key, value) in counters.values() {
                    if let Some(value) = value {
                        add(tx, &date(event.observed_ms)?, event, key, value, None)?;
                    }
                }
                support(tx, event, "tokens")?;
            }
        }
    }
    if !same_epoch {
        tx.execute(
            "UPDATE usage SET active=0 WHERE scope=? AND active=1",
            [event.scope.0.as_slice()],
        )?;
    }
    tx.execute("INSERT INTO usage VALUES(?,?,?,?,1) ON CONFLICT(scope,epoch) DO UPDATE SET at=excluded.at,payload=excluded.payload",
        params![event.scope.0.as_slice(),epoch.0.as_slice(),event.observed_ms,serde_json::to_string(&saved)?])?;
    Ok(())
}
