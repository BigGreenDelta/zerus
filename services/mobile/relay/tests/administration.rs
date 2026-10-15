//! Offline administration through the `zerus-relay` command line.
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, s},
    json::canonical,
    protocol,
    store::{Role, Store},
};

mod common;
use common::Backend;

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
    let relay = Backend::new(Config {
        background: false,
        ..Config::default()
    })
    .await;
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
}
