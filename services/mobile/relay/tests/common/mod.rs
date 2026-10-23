//! Private relay backends for the storage contracts.
//!
//! With `ZERUS_RELAY_TEST_DATABASE_URL` naming a disposable PostgreSQL server,
//! every backend is a new `relay_test_<uuid>` database on it, dropped with the
//! backend even after a failed assertion. Otherwise it is a private SQLite file
//! (until the relay is PostgreSQL-only); `ZERUS_REQUIRE_RELAY_E2E=1` refuses that
//! fallback. The URL's credentials are never printed.
#![allow(dead_code)]

use serde_json::{json, Value};
use std::{
    fs::{OpenOptions, Permissions},
    io::Write,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::PathBuf,
    process::{Command, Output},
    sync::Arc,
};
use tempfile::TempDir;
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, s, Db},
    error::Result,
    json::canonical,
    store::{Credential, Role, Store, Submission},
};

pub const DATABASE_URL: &str = "ZERUS_RELAY_TEST_DATABASE_URL";

/// One test's database and the private configuration that selects it.
pub struct Backend {
    dir: TempDir,
    pub cfg: Arc<Config>,
    postgres: Option<Postgres>,
}

impl Backend {
    pub async fn new(mut cfg: Config) -> Self {
        let dir = tempfile::tempdir().unwrap();
        std::fs::set_permissions(dir.path(), Permissions::from_mode(0o700)).unwrap();
        let postgres = match std::env::var(DATABASE_URL) {
            Ok(server) => {
                let postgres = Postgres::create(server).await;
                cfg.database_url = Some(postgres.url());
                Some(postgres)
            }
            Err(_) => {
                assert_ne!(
                    std::env::var("ZERUS_REQUIRE_RELAY_E2E").as_deref(),
                    Ok("1"),
                    "ZERUS_REQUIRE_RELAY_E2E=1 needs {DATABASE_URL}"
                );
                None
            }
        };
        // The command line reads the same backend and limits from this file.
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(dir.path().join("relay.json"))
            .unwrap()
            .write_all(&serde_json::to_vec(&cfg).unwrap())
            .unwrap();
        Self {
            dir,
            cfg: Arc::new(cfg),
            postgres,
        }
    }
    /// For contracts that only exist across PostgreSQL workers.
    pub async fn postgres(cfg: Config) -> Self {
        assert!(
            std::env::var_os(DATABASE_URL).is_some(),
            "isolated PostgreSQL required: set {DATABASE_URL}"
        );
        Self::new(cfg).await
    }
    /// A PostgreSQL database that first holds `sql`, as an older relay left
    /// it; the next `store()` upgrades it in place. None without a server,
    /// which `ZERUS_REQUIRE_RELAY_E2E=1` refuses.
    pub async fn restored(cfg: Config, sql: &str) -> Option<Self> {
        use sqlx::Connection;
        if std::env::var_os(DATABASE_URL).is_none() {
            assert_ne!(
                std::env::var("ZERUS_REQUIRE_RELAY_E2E").as_deref(),
                Ok("1"),
                "ZERUS_REQUIRE_RELAY_E2E=1 needs {DATABASE_URL}"
            );
            return None;
        }
        let backend = Self::new(cfg).await;
        let url = backend.cfg.database_url.as_deref().unwrap();
        let mut connection = sqlx::PgConnection::connect(url).await.unwrap();
        sqlx::raw_sql(sql).execute(&mut connection).await.unwrap();
        connection.close().await.unwrap();
        Some(backend)
    }
    pub fn is_postgres(&self) -> bool {
        self.postgres.is_some()
    }
    /// This test's PostgreSQL database name.
    pub fn database(&self) -> &str {
        &self.postgres.as_ref().expect("PostgreSQL backend").name
    }
    /// An administrative connection to the server holding this test's database.
    pub async fn server(&self) -> sqlx::PgConnection {
        use sqlx::Connection;
        let server = &self.postgres.as_ref().expect("PostgreSQL backend").server;
        sqlx::PgConnection::connect(server).await.unwrap()
    }
    fn sqlite(&self) -> PathBuf {
        self.dir.path().join("relay.sqlite3")
    }
    /// Another worker (or, after a close, a restart) on the same database.
    pub async fn store(&self) -> Store {
        Store::new(Db::open(&self.sqlite(), self.cfg.clone()).await.unwrap())
    }
    /// Offline administration through the real `zerus-relay` command line.
    #[allow(clippy::option_env_unwrap)] // Also reused by connector tests without a relay binary target.
    pub fn cli(&self, arguments: &[&str]) -> Output {
        Command::new(
            option_env!("CARGO_BIN_EXE_zerus-relay")
                .expect("relay CLI required for administration contracts"),
        )
        // Never fall back to the default database under HOME.
        .arg("--database")
        .arg(self.sqlite())
        .arg("--config")
        .arg(self.dir.path().join("relay.json"))
        .args(arguments)
        .output()
        .unwrap()
    }
}

struct Postgres {
    server: String,
    name: String,
}

impl Postgres {
    async fn create(server: String) -> Self {
        let name = format!("relay_test_{}", Uuid::new_v4().simple());
        admin(&server, &format!("CREATE DATABASE {name}"))
            .await
            .unwrap_or_else(|e| panic!("could not create a database on {DATABASE_URL}: {e}"));
        Self { server, name }
    }
    fn url(&self) -> String {
        let mut url = url::Url::parse(&self.server).unwrap();
        url.set_path(&self.name);
        url.into()
    }
}

impl Drop for Postgres {
    /// Its own thread and runtime, so a failed or panicking test still drops
    /// the database; FORCE closes connections a test left open.
    fn drop(&mut self) {
        let (server, name) = (self.server.clone(), self.name.clone());
        let dropped = std::thread::spawn(move || {
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .ok()?;
            runtime
                .block_on(admin(
                    &server,
                    &format!("DROP DATABASE IF EXISTS {name} WITH (FORCE)"),
                ))
                .ok()
        })
        .join();
        if !std::thread::panicking() {
            assert!(
                matches!(dropped, Ok(Some(()))),
                "could not drop {}",
                self.name
            );
        }
    }
}

async fn admin(server: &str, statement: &str) -> std::result::Result<(), sqlx::Error> {
    use sqlx::Connection;
    let mut connection = sqlx::PgConnection::connect(server).await?;
    sqlx::query(statement).execute(&mut connection).await?;
    connection.close().await
}

/// A provisioned workspace with one computer and one paired phone.
pub struct Fixture {
    pub store: Store,
    pub node: Credential,
    pub phone: Credential,
    pub credentials: Value,
    pub paired: Value,
    pub backend: Backend,
}

impl Fixture {
    pub async fn new() -> Self {
        Self::config(Config {
            background: false,
            ..Config::default()
        })
        .await
    }
    pub async fn config(cfg: Config) -> Self {
        Self::provision(Backend::new(cfg).await).await
    }
    pub async fn provision(backend: Backend) -> Self {
        let store = backend.store().await;
        let c = store
            .provision("Example workspace", "Example computer")
            .await
            .unwrap();
        let p = store.pair(s(&c, "pair_code"), "Phone").await.unwrap();
        let node = store
            .authenticate(s(&c, "node_token"), Role::Node)
            .await
            .unwrap();
        let phone = store
            .authenticate(s(&p, "device_token"), Role::Device)
            .await
            .unwrap();
        Self {
            store,
            node,
            phone,
            credentials: c,
            paired: p,
            backend,
        }
    }
    pub fn command(&self, op: &str) -> Value {
        let id = Uuid::new_v4().to_string();
        json!({"request_id":id,"computer_id":self.node.id,"operation":op,"session":"codex/example/tag","payload":if op=="inspect"{json!({})}else{json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation","text":"hello"})}})
    }
    pub async fn submit(&self, b: &Value) -> Result<Value> {
        self.store
            .submit(&self.phone, &Submission::new(b.clone())?)
            .await
            .map(|(_, b)| b)
    }
    pub async fn check_accounting(&self) {
        let mut t = self.store.db.begin().await.unwrap();
        let row=t.one("SELECT CAST((SELECT COALESCE(sum(body_bytes+result_bytes+reserved_bytes),0) FROM requests WHERE workspace_id=?)+(SELECT COALESCE(sum(payload_bytes),0) FROM computer_notification_preferences WHERE workspace_id=?) AS BIGINT) AS bytes",args![&self.phone.workspace,&self.phone.workspace]).await.unwrap().unwrap();
        let usage = t
            .one(
                "SELECT * FROM workspace_usage WHERE workspace_id=?",
                args![&self.phone.workspace],
            )
            .await
            .unwrap()
            .unwrap();
        assert_eq!(row["bytes"], usage["payload_bytes"]);
        let active=t.one("SELECT count(*) AS n FROM requests WHERE workspace_id=? AND state IN ('queued','claimed')",args![&self.phone.workspace]).await.unwrap().unwrap();
        assert_eq!(active["n"], usage["active"]);
        t.commit().await.unwrap();
    }
    /// A heartbeat from a connector that never sends presence.
    pub async fn beat(&self, snapshot: &Value) {
        self.store
            .heartbeat(
                &self.node,
                &json!({ "snapshot": snapshot }),
                &canonical(snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    pub async fn beat_with(&self, snapshot: &Value, presence: &Value) {
        self.store
            .heartbeat(
                &self.node,
                &json!({"snapshot":snapshot,"presence":presence}),
                &canonical(snapshot).unwrap(),
            )
            .await
            .unwrap();
    }
    pub async fn node_row(&self, columns: &str) -> Value {
        self.store
            .db
            .one(
                &format!("SELECT {columns} FROM nodes WHERE id=?"),
                args![&self.node.id],
            )
            .await
            .unwrap()
            .unwrap()
    }
    /// Counts the rows of a FROM clause, for example `alert_candidates WHERE session='s0'`.
    pub async fn count(&self, from: &str) -> i64 {
        i(
            &self
                .store
                .db
                .one(&format!("SELECT count(*) AS n FROM {from}"), &[])
                .await
                .unwrap()
                .unwrap(),
            "n",
        )
    }
    pub async fn alert_count(&self) -> i64 {
        self.count("events WHERE kind='alert'").await
    }
    pub async fn events_of(&self, kind: &str) -> Vec<Value> {
        self.store.events(&self.phone, 0).await.unwrap()["events"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|e| e["kind"] == kind)
            .cloned()
            .collect()
    }
    /// Closes the pool, then drops the backend and its database.
    pub async fn close(self) {
        self.store.db.close().await;
    }
}
