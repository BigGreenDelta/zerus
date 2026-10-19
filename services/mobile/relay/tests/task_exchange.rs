//! The phone's task exchange: a computer-scoped short read that carries Loro
//! updates both ways (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{args, db::s, json::canonical, protocol};

mod common;
use common::Fixture;

const TASK: &str = "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e11";

fn command(computer: &str) -> Value {
    json!({"request_id":Uuid::new_v4().to_string(),"computer_id":computer,"operation":"task_exchange",
        "session":"","payload":{"schema":1,"project":"project-1",
        "inventory":{"project":{"1234":3},"tasks":{TASK:{"1234":2,"98765":1}}},
        "updates":[{"task":null,"bytes":"bG9ybw=="},{"task":TASK,"bytes":""}],
        "continue":{"task":TASK,"sha256":"ab".repeat(32),"offset":524288}}})
}

/// A heartbeat advertising exactly these operations.
async fn beat(fixture: &Fixture, operations: &[&str]) {
    let snapshot =
        json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":operations}});
    fixture
        .store
        .heartbeat(
            &fixture.node,
            &json!({"snapshot":snapshot}),
            &canonical(&snapshot).unwrap(),
        )
        .await
        .unwrap();
}

#[test]
fn the_exchange_wire_is_a_strict_computer_scoped_read() {
    let value = command(&Uuid::new_v4().to_string());
    protocol::request(&value).unwrap();
    assert!(protocol::OPERATIONS.contains(&"task_exchange"));
    assert!(protocol::READS.contains(&"task_exchange"));
    assert!(!protocol::READ_SQL.contains("task_exchange"));
    assert!(!protocol::LAUNCH.contains(&"task_exchange"));
    // Only the fields hgs reads; updates and continue are optional.
    let mut minimal = value.clone();
    for key in ["updates", "continue"] {
        minimal["payload"].as_object_mut().unwrap().remove(key);
    }
    protocol::request(&minimal).unwrap();
    let mut bad = value.clone();
    bad["session"] = json!("codex/example");
    assert!(protocol::request(&bad).is_err());
    let cases: &[(&str, Value)] = &[
        ("/schema", json!(2)),
        ("/project", json!("")),
        ("/project", json!(7)),
        ("/inventory", json!([])),
        ("/inventory/extra", json!({})),
        ("/inventory/tasks", json!([])),
        ("/inventory/project/1234", json!(-1)),
        ("/inventory/project/1234", json!("3")),
        ("/inventory/project/peer", json!(1)),
        ("/inventory/tasks/not-a-uuid", json!({})),
        ("/updates", json!({})),
        ("/updates/0/task", json!("not-a-uuid")),
        ("/updates/0/bytes", json!("%%%")),
        ("/updates/0/extra", json!(1)),
        ("/continue/sha256", json!("AB")),
        ("/continue/offset", json!(-1)),
        ("/continue/extra", json!(1)),
        ("/extra", json!(1)),
    ];
    for (path, replacement) in cases {
        let mut bad = value.clone();
        let (parent, key) = path.rsplit_once('/').unwrap();
        let target = bad["payload"].pointer_mut(parent).unwrap();
        match target {
            Value::Array(items) => items[key.parse::<usize>().unwrap()] = replacement.clone(),
            _ => target[key] = replacement.clone(),
        }
        assert!(
            protocol::request(&bad).is_err(),
            "accepted {path} = {replacement}"
        );
    }
    // The relay's ordinary 1 MiB request bound.
    let mut large = value.clone();
    large["payload"]["updates"] = json!([{"task":null,"bytes":"A".repeat(1 << 20)}]);
    assert!(protocol::request(&large).is_err());
}

#[tokio::test]
async fn admission_needs_the_capability_and_counts_as_a_read() {
    let fixture = Fixture::new().await;
    let body = command(&fixture.node.id);
    beat(&fixture, &["catalog"]).await;
    assert_eq!(fixture.submit(&body).await.unwrap_err().0.as_u16(), 409);
    beat(&fixture, &["catalog", "task_exchange"]).await;
    assert_eq!(fixture.submit(&body).await.unwrap()["state"], "queued");
    let usage = fixture
        .store
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&fixture.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        (usage["reads"].clone(), usage["mutations"].clone()),
        (json!(1), json!(0))
    );
    let claim = fixture
        .store
        .claim(&fixture.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    let claimed = &claim["requests"][0];
    assert_eq!(claimed["operation"], "task_exchange");
    assert_eq!(claimed["payload"], body["payload"]);
    fixture.check_accounting().await;
    fixture.close().await;
}

#[tokio::test]
async fn a_delivered_exchange_leaves_the_relay_within_two_minutes() {
    let fixture = Fixture::new().await;
    beat(&fixture, &["task_exchange"]).await;
    let body = command(&fixture.node.id);
    let id = s(&body, "request_id");
    fixture.submit(&body).await.unwrap();
    fixture
        .store
        .claim(&fixture.node, false)
        .await
        .unwrap()
        .unwrap();
    let receipt = json!({"state":"completed","result":{"schema":1,"imported":1,"refused":[],
        "updates":[{"task":TASK,"bytes":"bG9ybw=="}],"more":false,"digest":"cd".repeat(32)},"error":null});
    fixture
        .store
        .result(
            &fixture.node,
            id,
            &receipt,
            Some(&canonical(&receipt["result"]).unwrap()),
        )
        .await
        .unwrap();
    assert_eq!(
        fixture.store.receipt(&fixture.phone, id).await.unwrap()["result"],
        receipt["result"]
    );
    let stored = |fixture: &Fixture| {
        let db = fixture.store.db.clone();
        async move {
            db.one("SELECT id FROM requests WHERE id=?", args![id])
                .await
                .unwrap()
                .is_some()
        }
    };
    let age = |seconds: f64| {
        let db = fixture.store.db.clone();
        async move {
            db.exec(
                "UPDATE requests SET updated=? WHERE id=?",
                args![zerus_relay::db::now() - seconds, id],
            )
            .await
            .unwrap();
        }
    };
    age(100.0).await;
    fixture.store.maintain(false).await.unwrap();
    assert!(stored(&fixture).await);
    age(121.0).await;
    fixture.store.maintain(false).await.unwrap();
    assert!(!stored(&fixture).await);
    let usage = fixture
        .store
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&fixture.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        (usage["reads"].clone(), usage["mutations"].clone()),
        (json!(0), json!(0))
    );
    fixture.check_accounting().await;
    fixture.close().await;
}
