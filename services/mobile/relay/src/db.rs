//! A small dialect boundary, with identical transactions in both backends.
//! No SQL transaction holds a connection across a network delivery or idle poll.
use crate::{
    config::{self, Config},
    error::{Error, Result},
    json::digest,
};
use serde_json::{json, Value};
use sqlx::{
    any::{AnyConnectOptions, AnyPoolOptions, AnyTypeInfoKind},
    Any, AnyPool, Column, ConnectOptions, Connection, Row, Transaction, ValueRef,
};
use std::{path::Path, str::FromStr, sync::Arc, time::Duration};
use tokio::sync::{broadcast, OwnedSemaphorePermit, Semaphore};
use tokio_util::{sync::CancellationToken, task::TaskTracker};

pub type RowValue = Value;
#[derive(Clone)]
pub struct Db {
    pub pool: AnyPool,
    pub postgres: bool,
    pub cfg: Arc<Config>,
    pub wake: broadcast::Sender<String>,
    pub stop: CancellationToken,
    pub tasks: TaskTracker,
    slots: Arc<Semaphore>,
    cleanup: Arc<Semaphore>,
    _lock: Option<Arc<std::fs::File>>,
}
pub struct Tx {
    pub inner: Transaction<'static, Any>,
    pub postgres: bool,
    _permit: OwnedSemaphorePermit,
}
#[derive(Clone)]
pub enum Arg<'a> {
    Text(&'a str),
    Int(i64),
    Float(f64),
    Null,
}
impl<'a> From<&'a str> for Arg<'a> {
    fn from(x: &'a str) -> Self {
        Self::Text(x)
    }
}
impl<'a> From<&'a String> for Arg<'a> {
    fn from(x: &'a String) -> Self {
        Self::Text(x)
    }
}
impl From<i64> for Arg<'_> {
    fn from(x: i64) -> Self {
        Self::Int(x)
    }
}
impl From<usize> for Arg<'_> {
    fn from(x: usize) -> Self {
        Self::Int(x as i64)
    }
}
impl From<f64> for Arg<'_> {
    fn from(x: f64) -> Self {
        Self::Float(x)
    }
}
impl From<bool> for Arg<'_> {
    fn from(x: bool) -> Self {
        Self::Int(i64::from(x))
    }
}
impl<'a> From<&'a Value> for Arg<'a> {
    fn from(v: &'a Value) -> Self {
        match v {
            Value::String(x) => Self::Text(x),
            Value::Number(x) => {
                if let Some(i) = x.as_i64() {
                    Self::Int(i)
                } else {
                    Self::Float(x.as_f64().unwrap_or(0.0))
                }
            }
            Value::Bool(x) => Self::Int(i64::from(*x)),
            _ => Self::Null,
        }
    }
}
#[macro_export]
macro_rules! args {($($x:expr),* $(,)?)=>{&[$($crate::db::Arg::from($x)),*]};}

fn dialect(sql: &str, pg: bool) -> String {
    let mut s = if pg {
        sql.replace("BYTES(", "octet_length(")
            .replace(" AS_BYTES)", ")")
    } else {
        sql.replace("BYTES(", "length(CAST(")
            .replace(" AS_BYTES)", " AS BLOB))")
            .replace(" FOR UPDATE SKIP LOCKED", "")
            .replace(" FOR UPDATE", "")
            .replace(" FOR SHARE", "")
    };
    if pg {
        let mut n = 0;
        s = s
            .chars()
            .map(|c| {
                if c == '?' {
                    n += 1;
                    format!("${n}")
                } else {
                    c.to_string()
                }
            })
            .collect();
    }
    s
}
fn bind<'q>(
    s: &'q str,
    args: &'q [Arg<'q>],
) -> sqlx::query::Query<'q, Any, sqlx::any::AnyArguments<'q>> {
    let mut q = sqlx::query(s).persistent(false);
    for a in args {
        q = match a {
            Arg::Text(x) => q.bind(*x),
            Arg::Int(x) => q.bind(*x),
            Arg::Float(x) => q.bind(*x),
            Arg::Null => q.bind(None::<String>),
        };
    }
    q
}
fn row(r: sqlx::any::AnyRow) -> Result<RowValue> {
    let mut o = serde_json::Map::new();
    for c in r.columns() {
        let i = c.ordinal();
        let raw = r.try_get_raw(i)?;
        let v = if raw.is_null() {
            Value::Null
        } else {
            match raw.type_info().kind() {
                AnyTypeInfoKind::Text => json!(r.try_get::<String, _>(i)?),
                AnyTypeInfoKind::SmallInt | AnyTypeInfoKind::Integer | AnyTypeInfoKind::BigInt => {
                    json!(r.try_get::<i64, _>(i)?)
                }
                AnyTypeInfoKind::Real | AnyTypeInfoKind::Double => json!(r.try_get::<f64, _>(i)?),
                AnyTypeInfoKind::Bool => json!(r.try_get::<bool, _>(i)?),
                _ => {
                    return Err(Error::UNAVAILABLE);
                }
            }
        };
        o.insert(c.name().to_string(), v);
    }
    Ok(Value::Object(o))
}
impl Tx {
    pub async fn all(&mut self, s: &str, a: &[Arg<'_>]) -> Result<Vec<RowValue>> {
        let s = dialect(s, self.postgres);
        bind(&s, a)
            .fetch_all(&mut *self.inner)
            .await?
            .into_iter()
            .map(row)
            .collect()
    }
    pub async fn one(&mut self, s: &str, a: &[Arg<'_>]) -> Result<Option<RowValue>> {
        let s = dialect(s, self.postgres);
        bind(&s, a)
            .fetch_optional(&mut *self.inner)
            .await?
            .map(row)
            .transpose()
    }
    pub async fn exec(&mut self, s: &str, a: &[Arg<'_>]) -> Result<u64> {
        let s = dialect(s, self.postgres);
        Ok(bind(&s, a).execute(&mut *self.inner).await?.rows_affected())
    }
    pub async fn commit(self) -> Result<()> {
        Ok(self.inner.commit().await?)
    }
    pub async fn notify(&mut self, topic: &str) -> Result<()> {
        if self.postgres {
            self.exec("SELECT pg_notify('zerus_relay',?)", args![topic])
                .await?;
        }
        Ok(())
    }
}
impl Db {
    pub async fn open(path: &Path, cfg: Arc<Config>) -> Result<Self> {
        sqlx::any::install_default_drivers();
        let pg = cfg.database_url.is_some();
        let lock = if pg {
            None
        } else {
            Some(Arc::new(config::sqlite_file(path)?))
        };
        let url = cfg
            .database_url
            .clone()
            .unwrap_or_else(|| format!("sqlite://{}?mode=rw", path.display()));
        let options = AnyConnectOptions::from_str(&url)?.disable_statement_logging();
        let pool = AnyPoolOptions::new()
            .min_connections(if pg { cfg.pool_min } else { 1 })
            .max_connections(if pg { cfg.pool_max } else { 1 })
            .acquire_timeout(Duration::from_secs(5))
            .after_release(|connection, _| {
                // PostgreSQL otherwise retains the largest read/write buffer
                // on every pooled socket after a maximum attachment transfer.
                connection.shrink_buffers();
                Box::pin(async { Ok(true) })
            })
            .after_connect(move |c, _| {
                Box::pin(async move {
                    if pg {
                        for statement in [
                            "SET statement_timeout='15s'",
                            "SET lock_timeout='5s'",
                            "SET idle_in_transaction_session_timeout='20s'",
                            "SET application_name='zerus-mobile:rust'",
                        ] {
                            sqlx::query(statement).execute(&mut *c).await?;
                        }
                    } else {
                        for statement in [
                            "PRAGMA foreign_keys=ON",
                            "PRAGMA journal_mode=DELETE",
                            "PRAGMA synchronous=FULL",
                            "PRAGMA secure_delete=ON",
                            "PRAGMA busy_timeout=5000",
                            "PRAGMA cache_size=-2048",
                        ] {
                            sqlx::query(statement).execute(&mut *c).await?;
                        }
                    }
                    Ok(())
                })
            })
            .connect_with(options)
            .await?;
        let (wake, _) = broadcast::channel(1024);
        let db = Self {
            pool,
            postgres: pg,
            cfg: cfg.clone(),
            wake,
            stop: CancellationToken::new(),
            tasks: TaskTracker::new(),
            slots: Arc::new(Semaphore::new(cfg.pool_max as usize + 64)),
            cleanup: Arc::new(Semaphore::new(
                cfg.max_polls_global + cfg.maintenance_batch as usize,
            )),
            _lock: lock,
        };
        db.initialize().await?;
        if pg {
            db.start_listener().await?;
        }
        Ok(db)
    }
    pub async fn begin(&self) -> Result<Tx> {
        self.transaction(false).await
    }
    pub async fn cleanup(&self) -> Result<Tx> {
        self.transaction(true).await
    }
    async fn transaction(&self, cleanup: bool) -> Result<Tx> {
        let permit = if cleanup { &self.cleanup } else { &self.slots }
            .clone()
            .try_acquire_owned()
            .map_err(|_| Error::BUSY)?;
        let inner = self
            .pool
            .begin_with(if self.postgres {
                "BEGIN"
            } else {
                "BEGIN IMMEDIATE"
            })
            .await?;
        Ok(Tx {
            inner,
            postgres: self.postgres,
            _permit: permit,
        })
    }
    pub async fn one(&self, s: &str, a: &[Arg<'_>]) -> Result<Option<RowValue>> {
        let mut t = self.begin().await?;
        let r = t.one(s, a).await?;
        t.commit().await?;
        Ok(r)
    }
    pub async fn exec(&self, s: &str, a: &[Arg<'_>]) -> Result<u64> {
        let mut t = self.begin().await?;
        let r = t.exec(s, a).await?;
        t.commit().await?;
        Ok(r)
    }
    pub fn shard(&self, w: &str) -> i64 {
        let h = digest(w);
        let n = u64::from_str_radix(&h[..16], 16).expect("SHA256 hex");
        (n % self.cfg.quota_shards as u64) as i64
    }
    pub async fn usage(&self, t: &mut Tx, w: &str) -> Result<Value> {
        t.one(
            "SELECT * FROM workspace_usage WHERE workspace_id=? FOR UPDATE",
            args![w],
        )
        .await?
        .ok_or(Error::NOT_FOUND)
    }
    pub async fn payload_usage(&self, t: &mut Tx, w: &str) -> Result<Value> {
        t.one(
            "SELECT payload_bytes FROM payload_shards WHERE id=? FOR UPDATE",
            args![self.shard(w)],
        )
        .await?;
        self.usage(t, w).await
    }
    pub async fn bytes(&self, t: &mut Tx, w: &str, delta: i64) -> Result<()> {
        t.exec(
            "UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=?",
            args![delta, self.shard(w)],
        )
        .await?;
        t.exec(
            "UPDATE workspace_usage SET payload_bytes=payload_bytes+? WHERE workspace_id=?",
            args![delta, w],
        )
        .await?;
        Ok(())
    }
    pub fn signal(&self, topic: String) {
        let _ = self.wake.send(topic);
    }
    async fn start_listener(&self) -> Result<()> {
        let dsn = self.cfg.database_url.as_deref().ok_or(Error::BAD)?;
        let mut listener = sqlx::postgres::PgListener::connect(dsn).await?;
        listener.listen("zerus_relay").await?;
        let db = self.clone();
        self.tasks.spawn(async move {
            loop {tokio::select! {biased; _=db.stop.cancelled()=>break, r=listener.try_recv()=>match r {
                Ok(Some(n))=>{db.signal(n.payload().to_string());},
                Ok(None)=>{db.signal(String::new());},
                Err(_)=>{db.signal(String::new());tokio::select!{_=db.stop.cancelled()=>break,_=tokio::time::sleep(Duration::from_secs(1))=>{}}}
            }}}
        });
        Ok(())
    }
    async fn initialize(&self) -> Result<()> {
        let mut t = self.begin().await?;
        if self.postgres {
            t.exec("SELECT pg_advisory_xact_lock(735628109)", &[])
                .await?;
        } else {
            // Upgrade current Python SQLite databases in place without changing identities.
            for (table, columns) in [
                (
                    "nodes",
                    vec![
                        ("computer_id", "TEXT"),
                        ("machine_id", "TEXT"),
                        ("manifest_hash", "TEXT"),
                        ("snapshot_hash", "TEXT"),
                        ("presence_at", "DOUBLE PRECISION"),
                        ("presence_until", "DOUBLE PRECISION"),
                        ("mobile_delivery", "TEXT"),
                    ],
                ),
                (
                    "requests",
                    vec![
                        ("target_computer_id", "TEXT"),
                        ("target_machine_id", "TEXT"),
                        ("route_id", "TEXT"),
                        ("expires_at", "DOUBLE PRECISION"),
                        ("result_read", "DOUBLE PRECISION"),
                        ("result_bytes", "INTEGER NOT NULL DEFAULT 0"),
                        ("reserved_bytes", "INTEGER NOT NULL DEFAULT 0"),
                    ],
                ),
                (
                    "workspace_usage",
                    vec![
                        ("events", "INTEGER NOT NULL DEFAULT 0"),
                        ("push_jobs", "INTEGER NOT NULL DEFAULT 0"),
                        ("active_polls", "INTEGER NOT NULL DEFAULT 0"),
                    ],
                ),
                (
                    "push_jobs",
                    vec![("lease_token", "TEXT"), ("lease_until", "DOUBLE PRECISION")],
                ),
                ("events", vec![("alert_kind", "TEXT")]),
                (
                    "computer_routes",
                    vec![
                        ("presence_at", "DOUBLE PRECISION"),
                        ("presence_until", "DOUBLE PRECISION"),
                    ],
                ),
            ] {
                let existing = t.all(&format!("PRAGMA table_info({table})"), &[]).await?;
                if !existing.is_empty() {
                    for (name, typ) in columns {
                        if !existing.iter().any(|r| r["name"] == name) {
                            t.exec(&format!("ALTER TABLE {table} ADD COLUMN {name} {typ}"), &[])
                                .await?;
                        }
                    }
                }
            }
        }
        let schema = if self.postgres {
            include_str!("postgres.sql")
        } else {
            include_str!("sqlite.sql")
        };
        // SQLite new tables need route columns before the common registry backfill.
        let schema = if !self.postgres {
            schema.replace("snapshot_hash text)","snapshot_hash text,computer_id TEXT,machine_id TEXT,manifest_hash TEXT,presence_at DOUBLE PRECISION,presence_until DOUBLE PRECISION,mobile_delivery TEXT)").replace("reserved_bytes INTEGER NOT NULL DEFAULT 0)","reserved_bytes INTEGER NOT NULL DEFAULT 0,target_computer_id TEXT,target_machine_id TEXT,route_id TEXT)")
        } else {
            schema.into()
        };
        sqlx::raw_sql(&schema).execute(&mut *t.inner).await?;
        if t.one("SELECT max(version) AS version FROM relay_schema", &[])
            .await?
            .is_none_or(|r| r["version"] != 1)
        {
            return Err(Error::BAD);
        }
        t.exec(
            "INSERT INTO relay_allocation VALUES(1,?,?) ON CONFLICT DO NOTHING",
            args![self.cfg.global_max_bytes, self.cfg.quota_shards],
        )
        .await?;
        let a = t
            .one("SELECT * FROM relay_allocation WHERE id=1", &[])
            .await?
            .ok_or(Error::BAD)?;
        if a["global_max_bytes"] != self.cfg.global_max_bytes
            || a["quota_shards"] != self.cfg.quota_shards
        {
            return Err(Error::CONFLICT);
        }
        for id in 0..self.cfg.quota_shards {
            t.exec(
                "INSERT INTO payload_shards(id) VALUES(?) ON CONFLICT DO NOTHING",
                args![id],
            )
            .await?;
        }
        if !self.postgres {
            // Exclusive SQLite startup can reconcile old counters; PostgreSQL never
            // scans or retires other workers' claims on startup.
            t.exec("INSERT INTO workspace_usage(workspace_id) SELECT id FROM workspaces WHERE 1=1 ON CONFLICT DO NOTHING",&[]).await?;
            t.exec("UPDATE workspace_usage SET events=(SELECT count(*) FROM events WHERE workspace_id=workspace_usage.workspace_id),push_jobs=(SELECT count(*) FROM push_jobs j JOIN devices d ON d.id=j.device_id WHERE d.workspace_id=workspace_usage.workspace_id),active_polls=0",&[]).await?;
            t.exec("UPDATE global_usage SET events=(SELECT count(*) FROM events),push_jobs=(SELECT count(*) FROM push_jobs) WHERE id=1",&[]).await?;
            t.exec("UPDATE payload_shards SET payload_bytes=0", &[])
                .await?;
            for row in t
                .all(
                    "SELECT workspace_id,payload_bytes FROM workspace_usage",
                    &[],
                )
                .await?
            {
                t.exec(
                    "UPDATE payload_shards SET payload_bytes=payload_bytes+? WHERE id=?",
                    args![&row["payload_bytes"], self.shard(s(&row, "workspace_id"))],
                )
                .await?;
            }
            t.exec("DELETE FROM poll_leases", &[]).await?;
            t.exec("DELETE FROM poll_credentials", &[]).await?;
        }
        t.commit().await
    }
    pub async fn close(&self) {
        self.stop.cancel();
        self.tasks.close();
        // A partitioned database cannot acknowledge rollback or a maintenance
        // query. Final shutdown must still finish; durable leases can expire.
        let _ = tokio::time::timeout(Duration::from_secs(5), async {
            self.tasks.wait().await;
            self.pool.close().await;
        })
        .await;
    }
}
pub fn s<'a>(r: &'a Value, k: &str) -> &'a str {
    r[k].as_str().unwrap_or("")
}
pub fn i(r: &Value, k: &str) -> i64 {
    r[k].as_i64().unwrap_or(0)
}
pub fn f(r: &Value, k: &str) -> f64 {
    r[k].as_f64().unwrap_or(0.0)
}
pub fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}
