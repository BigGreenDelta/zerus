//! Offline administration through the `zerus-relay` command line.
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
    json::canonical,
    protocol,
    store::{Role, Store},
};

/// A private relay database: a new PostgreSQL database when
/// `ZERUS_RELAY_TEST_DATABASE_URL` names a disposable server, otherwise SQLite.
struct Relay {
    dir: TempDir,
    cfg: Arc<Config>,
    postgres: Option<(String, String)>,
}

impl Relay {
    async fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        std::fs::set_permissions(dir.path(), Permissions::from_mode(0o700)).unwrap();
        let mut cfg = Config {
            background: false,
            ..Config::default()
        };
        let mut postgres = None;
        if let Ok(base) = std::env::var("ZERUS_RELAY_TEST_DATABASE_URL") {
            use sqlx::Connection;
            let mut admin = sqlx::PgConnection::connect(&base).await.unwrap();
            let name = format!("relay_test_{}", Uuid::new_v4().simple());
            sqlx::query(&format!("CREATE DATABASE {name}"))
                .execute(&mut admin)
                .await
                .unwrap();
            admin.close().await.unwrap();
            let mut url = url::Url::parse(&base).unwrap();
            url.set_path(&name);
            // The command line reads the same backend from private configuration.
            OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(dir.path().join("relay.json"))
                .unwrap()
                .write_all(
                    json!({ "database_url": url.as_str() })
                        .to_string()
                        .as_bytes(),
                )
                .unwrap();
            cfg.database_url = Some(url.into());
            postgres = Some((base, name));
        }
        Self {
            dir,
            cfg: Arc::new(cfg),
            postgres,
        }
    }
    fn sqlite(&self) -> PathBuf {
        self.dir.path().join("relay.sqlite3")
    }
    async fn store(&self) -> Store {
        Store::new(Db::open(&self.sqlite(), self.cfg.clone()).await.unwrap())
    }
    fn cli(&self, arguments: &[&str]) -> Output {
        let mut command = Command::new(env!("CARGO_BIN_EXE_zerus-relay"));
        // Never fall back to the default database under HOME.
        command.arg("--database").arg(self.sqlite());
        if self.postgres.is_some() {
            command
                .arg("--config")
                .arg(self.dir.path().join("relay.json"));
        }
        command.args(arguments).output().unwrap()
    }
    async fn finish(self) {
        if let Some((base, name)) = &self.postgres {
            use sqlx::Connection;
            let mut admin = sqlx::PgConnection::connect(base).await.unwrap();
            sqlx::query(&format!("DROP DATABASE {name}"))
                .execute(&mut admin)
                .await
                .unwrap();
            admin.close().await.unwrap();
        }
    }
}

async fn count(store: &Store, sql: &str, value: &str) -> i64 {
    i(
        &store.db.one(sql, args![value]).await.unwrap().unwrap(),
        "n",
    )
}

async fn lookup(store: &Store, rows: &str, value: &str) -> String {
    let sql = format!("SELECT computer_id FROM {rows}=?");
    let row = store.db.one(&sql, args![value]).await.unwrap().unwrap();
    s(&row, "computer_id").to_string()
}

#[tokio::test]
async fn revoke_computer_by_alias() {
    let relay = Relay::new().await;
    let [local, peer, route] = std::array::from_fn(|_| Uuid::new_v4().to_string());
    let snapshot = json!({"sessions":[{"name":"codex/example"}],"mobile_capabilities":{"protocol_version":1,"operations":protocol::OPERATIONS,"features":["gateway_one_hop"]}});
    let encoded = canonical(&snapshot).unwrap();
    let store = relay.store().await;
    let gateway = store
        .provision("Synthetic workspace", "Arch")
        .await
        .unwrap();
    let workspace = s(&gateway, "workspace_id").to_string();
    let node = store
        .authenticate(s(&gateway, "node_token"), Role::Node)
        .await
        .unwrap();
    let direct = store.node(&workspace, "Mac direct").await.unwrap();
    let alias = s(&direct, "node_id").to_string();
    let credential = store
        .authenticate(s(&direct, "node_token"), Role::Node)
        .await
        .unwrap();
    store
        .heartbeat(
            &node,
            &json!({"snapshot":snapshot,"machine_id":local,"peers":[
                {"route_id":route,"machine_id":peer,"name":"Mac","online":true}]}),
            &encoded,
        )
        .await
        .unwrap();
    store
        .peer_heartbeat(
            &node,
            &route,
            &json!({"machine_id":peer,"snapshot":snapshot}),
            &encoded,
        )
        .await
        .unwrap();
    let target = lookup(&store, "native_computers WHERE machine_id", &peer).await;
    // The peer's own direct enrollment binds to its native computer. No phone
    // has listed it, so its enrollment ID becomes an alias of the target.
    store
        .heartbeat(
            &credential,
            &json!({"snapshot":snapshot,"machine_id":peer,"peers":[]}),
            &encoded,
        )
        .await
        .unwrap();
    assert_eq!(
        lookup(&store, "computer_aliases WHERE alias", &alias).await,
        target
    );
    // The superseded enrollment computer keeps that ID and was created first.
    assert_eq!(
        count(
            &store,
            "SELECT count(*) AS n FROM computers WHERE id=?",
            &alias
        )
        .await,
        1
    );
    store.db.close().await;
    drop(store);

    let completed = relay.cli(&["revoke-computer", "--id", &alias]);
    assert!(completed.status.success(), "{completed:?}");
    assert_eq!(
        serde_json::from_slice::<Value>(&completed.stdout).unwrap(),
        json!({"revoked":true})
    );
    let store = relay.store().await;
    assert_eq!(
        lookup(&store, "computer_aliases WHERE alias", &alias).await,
        target
    );
    assert_eq!(
        count(
            &store,
            "SELECT revoked AS n FROM computers WHERE id=?",
            &target
        )
        .await,
        1
    );
    // Both the gateway's peer route and the direct local route are withdrawn.
    assert_eq!(
        count(
            &store,
            "SELECT count(*) AS n FROM computer_routes WHERE computer_id=? AND active=1",
            &target
        )
        .await,
        0
    );
    assert_eq!(
        count(
            &store,
            "SELECT count(*) AS n FROM nodes WHERE workspace_id=? AND revoked=1",
            &workspace
        )
        .await,
        0
    );
    store.db.close().await;
    drop(store);

    let missing = relay.cli(&["revoke-computer", "--id", &Uuid::new_v4().to_string()]);
    assert_eq!(missing.status.code(), Some(2), "{missing:?}");
    assert!(String::from_utf8_lossy(&missing.stderr).contains("computer not found"));
    relay.finish().await;
}
