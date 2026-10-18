//! Additive Git reads keep their own bounded history class and the original mutation identities.
mod common;

use common::Fixture;
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s},
    json::canonical,
    protocol,
};

fn command(computer: &str) -> Value {
    let id = Uuid::new_v4().to_string();
    json!({"request_id":id,"computer_id":computer,"operation":"git_status","session":"",
        "payload":{"request_id":id,"path":"/workspace/checkout with spaces;$(literal)"}})
}

#[test]
fn git_status_wire_is_an_exact_computer_scoped_read() {
    let good = command(&Uuid::new_v4().to_string());
    protocol::request(&good).unwrap();
    assert!(protocol::READS.contains(&"git_status"));
    assert!(protocol::OPERATIONS.contains(&"git_status"));
    assert!(!protocol::COMPUTER.contains(&"git_status"));
    assert!(!protocol::READ_SQL.contains("git_status"));
    for path in [
        "",
        "relative",
        "~/checkout",
        "/bad\npath",
        "/bad\0path",
        "/bad\u{7f}path",
        "/bad\u{85}path",
    ] {
        let mut bad = good.clone();
        bad["payload"]["path"] = json!(path);
        assert!(protocol::request(&bad).is_err(), "accepted path {path:?}");
    }
    for value in [json!(true), json!(1), Value::Null, json!([]), json!({})] {
        let mut bad = good.clone();
        bad["payload"]["path"] = value;
        assert!(protocol::request(&bad).is_err());
    }
    for key in [
        "expected_run_id",
        "expected_conversation_id",
        "agent_id",
        "archive_id",
        "refresh",
        "command",
    ] {
        let mut bad = good.clone();
        bad["payload"][key] = json!("unrequested");
        assert!(protocol::request(&bad).is_err(), "accepted extra {key}");
    }
    for key in ["request_id", "path"] {
        let mut bad = good.clone();
        bad["payload"].as_object_mut().unwrap().remove(key);
        assert!(protocol::request(&bad).is_err());
    }
    let mut bad = good.clone();
    bad["payload"]["request_id"] = json!(Uuid::new_v4().to_string());
    assert!(protocol::request(&bad).is_err());
    bad = good.clone();
    bad["session"] = json!("codex/example/tag");
    assert!(protocol::request(&bad).is_err());
    bad = good.clone();
    bad["archive_id"] = json!(Uuid::new_v4().to_string());
    assert!(protocol::request(&bad).is_err());
    let mut boundary = good;
    boundary["payload"]["path"] = json!(format!("/{}", "界".repeat(1365)));
    assert_eq!(boundary["payload"]["path"].as_str().unwrap().len(), 4096);
    protocol::request(&boundary).unwrap();
    boundary["payload"]["path"] = json!(format!("/{}x", "界".repeat(1365)));
    assert!(protocol::request(&boundary).is_err());
}

async fn beat(f: &Fixture, operations: &[&str]) {
    f.beat(&json!({"sessions":["codex/example/tag"],"mobile_capabilities":{"protocol_version":1,"operations":operations}})).await;
}

async fn complete(f: &Fixture, body: &Value, state: &str) {
    f.submit(body).await.unwrap();
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["request_id"], body["request_id"]);
    let receipt = json!({"state":state,"result":{"snapshot":"synthetic"},"error":null});
    f.store
        .result(
            &f.node,
            s(body, "request_id"),
            &receipt,
            Some(&canonical(&receipt["result"]).unwrap()),
        )
        .await
        .unwrap();
}

async fn age(f: &Fixture, body: &Value, seconds: f64) {
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - seconds, &body["request_id"]],
        )
        .await
        .unwrap();
}

#[tokio::test]
async fn git_status_gate_duplicate_and_shared_read_quota_leave_mutation_capacity() {
    let f = Fixture::new().await;
    let body = command(&f.node.id);
    beat(&f, &[]).await;
    assert_eq!(f.submit(&body).await.unwrap_err().0.as_u16(), 409);
    beat(&f, &["git_status"]).await;
    assert_eq!(f.submit(&body).await.unwrap()["state"], "queued");
    assert_eq!(f.submit(&body).await.unwrap()["state"], "queued");
    assert_eq!(f.count("requests").await, 1);
    let usage = f
        .store
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!((i(&usage, "reads"), i(&usage, "mutations")), (1, 0));
    let mut changed = body.clone();
    changed["payload"]["path"] = json!("/workspace/another");
    assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    f.store
        .db
        .exec(
            "UPDATE workspace_usage SET reads=5000 WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap();
    assert_eq!(
        f.submit(&command(&f.node.id)).await.unwrap_err().0.as_u16(),
        429
    );
    // Duplicate lookup still returns the original read at the retained limit.
    assert_eq!(f.submit(&body).await.unwrap()["state"], "queued");
    assert_eq!(
        f.submit(&f.command("send")).await.unwrap()["state"],
        "queued"
    );
    f.store
        .db
        .exec(
            "UPDATE workspace_usage SET reads=1 WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap();
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn git_status_routes_to_the_exact_peer_without_gateway_only_restriction() {
    let f = Fixture::new().await;
    let local = Uuid::new_v4().to_string();
    let remote = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["git_status"],"features":["gateway_one_hop"]}});
    let heartbeat = json!({"snapshot":snapshot,"machine_id":local,"peers":[{"route_id":route,"machine_id":remote,"name":"Peer","online":true}]});
    f.store
        .heartbeat(&f.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    f.store
        .peer_heartbeat(
            &f.node,
            &route,
            &json!({"machine_id":remote,"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
    let computers = f.store.computers(&f.phone).await.unwrap();
    let target = computers["computers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|row| row["machine_id"] == remote)
        .unwrap();
    let body = command(s(target, "id"));
    f.submit(&body).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    let claim = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    let request = &claim["requests"][0];
    assert_eq!(request["operation"], "git_status");
    assert_eq!(request["session"], "");
    assert_eq!(request["payload"], body["payload"]);
    assert_eq!(
        request["gateway_route"],
        json!({"schema":1,"route_id":route,"computer_id":body["computer_id"],"machine_id":remote})
    );
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn git_status_hourly_batched_retention_preserves_recent_active_and_mutation_tombstones() {
    let f = Fixture::config(Config {
        background: false,
        maintenance_batch: 2,
        ..Config::default()
    })
    .await;
    beat(&f, &["git_status", "remove_queued"]).await;
    let mut expired = vec![];
    for state in ["completed", "failed", "uncertain"] {
        let body = command(&f.node.id);
        complete(&f, &body, state).await;
        age(&f, &body, 8.0 * 86400.0).await;
        expired.push(body);
    }
    let recent = command(&f.node.id);
    complete(&f, &recent, "completed").await;
    age(&f, &recent, 3590.0).await;
    let mut mutation = f.command("remove_queued");
    mutation["payload"].as_object_mut().unwrap().remove("text");
    mutation["payload"]["message_id"] = json!("msg_Queued01");
    complete(&f, &mutation, "completed").await;
    age(&f, &mutation, 8.0 * 86400.0).await;
    let claimed = command(&f.node.id);
    f.submit(&claimed).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let queued = command(&f.node.id);
    f.submit(&queued).await.unwrap();
    for _ in 0..4 {
        f.store.maintain(false).await.unwrap();
    }
    for body in expired {
        assert!(f
            .store
            .db
            .one(
                "SELECT id FROM requests WHERE id=?",
                args![&body["request_id"]]
            )
            .await
            .unwrap()
            .is_none());
    }
    for (body, state) in [
        (&recent, "completed"),
        (&claimed, "claimed"),
        (&queued, "queued"),
    ] {
        let row = f
            .store
            .db
            .one(
                "SELECT state FROM requests WHERE id=?",
                args![&body["request_id"]],
            )
            .await
            .unwrap()
            .unwrap();
        assert_eq!(row["state"], state);
    }
    let tombstone = f.submit(&mutation).await.unwrap();
    assert_eq!(tombstone["state"], "completed");
    assert!(tombstone["result"].is_null());
    let row = f
        .store
        .db
        .one(
            "SELECT body,error FROM requests WHERE id=?",
            args![&mutation["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(row["body"], "");
    assert_eq!(
        row["error"],
        "request history expired; delivery must not be retried"
    );
    let usage = f
        .store
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!((i(&usage, "reads"), i(&usage, "mutations")), (3, 1));
    // Recent Git history gets its own one-hour class, rather than waiting seven days.
    age(&f, &recent, 3601.0).await;
    f.store.maintain(false).await.unwrap();
    assert!(f
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&recent["request_id"]]
        )
        .await
        .unwrap()
        .is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn git_status_honors_shorter_retention_and_an_old_schema_recreates_only_its_new_index() {
    let f = Fixture::config(Config {
        background: false,
        retention: 600,
        ..Config::default()
    })
    .await;
    beat(&f, &["git_status"]).await;
    let body = command(&f.node.id);
    complete(&f, &body, "completed").await;
    age(&f, &body, 601.0).await;
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("requests").await, 0);
    let old="CREATE INDEX IF NOT EXISTS read_history_v3 ON requests(updated,id) WHERE operation IN ('inspect','history','process_output','terminal_snapshot','catalog','dirs','worktrees','mark_read') AND state IN ('completed','failed','uncertain');";
    assert!(include_str!("../src/sqlite.sql").contains(old));
    assert!(include_str!("../src/postgres.sql").contains(old));
    let query="SELECT id,workspace_id FROM requests WHERE operation='git_status' AND state IN ('completed','failed','uncertain') AND updated<1 ORDER BY updated,id LIMIT 2";
    if f.backend.is_postgres() {
        let mut tx = f.store.db.begin().await.unwrap();
        tx.exec("SET LOCAL enable_seqscan=off", &[]).await.unwrap();
        let plan = tx.all(&format!("EXPLAIN {query}"), &[]).await.unwrap();
        assert!(serde_json::to_string(&plan)
            .unwrap()
            .contains("git_status_history_v1"));
        tx.commit().await.unwrap();
    } else {
        let mut tx = f.store.db.begin().await.unwrap();
        let plan = tx
            .all(
                &format!(
                    "EXPLAIN QUERY PLAN {}",
                    query.replace(
                        "FROM requests ",
                        "FROM requests INDEXED BY git_status_history_v1 "
                    )
                ),
                &[],
            )
            .await
            .unwrap();
        let plan = serde_json::to_string(&plan).unwrap();
        assert!(plan.contains("git_status_history_v1") && !plan.contains("TEMP B-TREE"));
        tx.commit().await.unwrap();
    }
    f.store
        .db
        .exec("DROP INDEX git_status_history_v1", &[])
        .await
        .unwrap();
    f.check_accounting().await;
    let Fixture {
        store,
        node,
        phone,
        backend,
        ..
    } = f;
    store.db.close().await;
    drop(store);
    let reopened = backend.store().await;
    let index = if backend.is_postgres() {
        reopened
            .db
            .one(
                "SELECT indexname::text AS name FROM pg_indexes WHERE indexname='git_status_history_v1'",
                &[],
            )
            .await
            .unwrap()
    } else {
        reopened.db.one("SELECT name FROM sqlite_master WHERE type='index' AND name='git_status_history_v1'",&[]).await.unwrap()
    };
    assert!(index.is_some());
    let mut next = command(&node.id);
    let (_, receipt) = reopened
        .submit(
            &phone,
            &zerus_relay::store::Submission::new(next.clone()).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(receipt["state"], "queued");
    assert!(reopened.claim(&node, false).await.unwrap().is_some());
    reopened
        .db
        .exec(
            "UPDATE requests SET claimed=? WHERE id=?",
            args![
                now() - backend.cfg.claim_ttl as f64 - 1.0,
                &next["request_id"]
            ],
        )
        .await
        .unwrap();
    reopened.maintain(true).await.unwrap();
    assert_eq!(
        reopened
            .receipt(&phone, s(&next, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    assert!(reopened.claim(&node, false).await.unwrap().is_none());
    next["payload"]["path"] = json!("/workspace/changed");
    assert_eq!(
        reopened
            .submit(&phone, &zerus_relay::store::Submission::new(next).unwrap())
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    reopened.db.close().await;
}
