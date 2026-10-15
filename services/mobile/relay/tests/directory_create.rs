//! Additive computer-scoped mutations retain exact route and permanent identity.
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{args, db::s, json::canonical, protocol};

mod common;
use common::Fixture;

fn command(computer: &str) -> Value {
    let id = Uuid::new_v4().to_string();
    json!({"request_id":id,"computer_id":computer,"operation":"directory_create","session":"",
        "payload":{"request_id":id,"path":"/workspace/new project;$(literal)"}})
}

#[test]
fn strict_directory_wire_agrees_with_connector_and_is_never_a_read() {
    let value = command(&Uuid::new_v4().to_string());
    protocol::request(&value).unwrap();
    assert!(protocol::LAUNCH.contains(&"directory_create"));
    assert!(!protocol::READS.contains(&"directory_create"));
    assert!(!protocol::READ_SQL.contains("directory_create"));
    for path in [
        "~",
        "~/new",
        "relative",
        "/",
        "/new/",
        "/new/.",
        "/new/..",
        "/new\nname",
        "/new\u{7f}name",
    ] {
        let mut bad = value.clone();
        bad["payload"]["path"] = json!(path);
        assert!(protocol::request(&bad).is_err(), "accepted {path:?}");
    }
    for (key, replacement) in [
        ("command", json!("mkdir")),
        ("parents", json!(true)),
        ("request_id", json!(Uuid::new_v4().to_string())),
    ] {
        let mut bad = value.clone();
        bad["payload"][key] = replacement;
        assert!(protocol::request(&bad).is_err());
    }
    let mut bad = value.clone();
    bad["session"] = json!("codex/example");
    assert!(protocol::request(&bad).is_err());
    let mut bad = value.clone();
    bad["request_id"] = json!("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA");
    bad["payload"]["request_id"] = bad["request_id"].clone();
    assert!(protocol::request(&bad).is_err());
    let mut boundary = value;
    boundary["payload"]["path"] = json!(format!("/{}", "界".repeat(1365)));
    protocol::request(&boundary).unwrap();
    boundary["payload"]["path"] = json!(format!("/{}", "界".repeat(1366)));
    assert!(protocol::request(&boundary).is_err());
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

#[tokio::test]
async fn admission_capability_duplicate_claim_receipt_and_retention_preserve_mutation() {
    let fixture = Fixture::new().await;
    let body = command(&fixture.node.id);
    beat(&fixture, &["dirs", "launch"]).await;
    assert_eq!(fixture.submit(&body).await.unwrap_err().0.as_u16(), 409);
    beat(&fixture, &["dirs", "directory_create"]).await;
    assert_eq!(fixture.submit(&body).await.unwrap()["state"], "queued");
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
    assert_eq!(usage["reads"], 0);
    assert_eq!(usage["mutations"], 1);
    let (a, b) = tokio::join!(
        fixture.store.claim(&fixture.node, false),
        fixture.store.claim(&fixture.node, false)
    );
    assert_eq!(
        usize::from(a.unwrap().is_some()) + usize::from(b.unwrap().is_some()),
        1
    );
    let receipt = json!({"state":"completed","result":{"status":"created","request_id":body["request_id"],
        "requested_path":body["payload"]["path"],"path":body["payload"]["path"],"parent":"/workspace"},"error":null});
    fixture
        .store
        .result(
            &fixture.node,
            s(&body, "request_id"),
            &receipt,
            Some(&canonical(&receipt["result"]).unwrap()),
        )
        .await
        .unwrap();
    assert_eq!(
        fixture.submit(&body).await.unwrap()["result"],
        receipt["result"]
    );
    assert!(fixture
        .store
        .claim(&fixture.node, false)
        .await
        .unwrap()
        .is_none());
    // One-hour read expiration must not remove directory mutation receipts.
    fixture
        .store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![zerus_relay::db::now() - 7200.0, &body["request_id"]],
        )
        .await
        .unwrap();
    fixture.store.maintain(false).await.unwrap();
    assert_eq!(
        fixture.submit(&body).await.unwrap()["result"],
        receipt["result"]
    );
    // Ordinary payload expiration leaves a permanent no-replay tombstone.
    fixture
        .store
        .db
        .exec(
            "UPDATE requests SET updated=1 WHERE id=?",
            args![&body["request_id"]],
        )
        .await
        .unwrap();
    fixture.store.maintain(false).await.unwrap();
    assert!(fixture
        .store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&body["request_id"]]
        )
        .await
        .unwrap()
        .is_some());
    assert!(fixture
        .store
        .claim(&fixture.node, false)
        .await
        .unwrap()
        .is_none());
    let mut changed = body.clone();
    changed["payload"]["path"] = json!("/workspace/other");
    assert_eq!(fixture.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    fixture.close().await;
}

#[tokio::test]
async fn direct_peer_directory_request_is_bound_to_enrolled_gateway_and_exact_machine() {
    let fixture = Fixture::new().await;
    let root = Uuid::new_v4().to_string();
    let machine = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    let snapshot = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["directory_create"],"features":["gateway_one_hop"]}});
    let mut heartbeat = json!({"snapshot":snapshot,"machine_id":root,"peers":[{"route_id":route,"machine_id":machine,"name":"Peer","online":true}]});
    fixture
        .store
        .heartbeat(&fixture.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let peer = json!({"machine_id":machine,"snapshot":snapshot});
    fixture
        .store
        .peer_heartbeat(&fixture.node, &route, &peer, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let computers = fixture.store.computers(&fixture.phone).await.unwrap();
    let computer = computers["computers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|row| row["machine_id"] == machine)
        .unwrap();
    let body = command(s(computer, "id"));
    fixture.submit(&body).await.unwrap();
    assert!(fixture
        .store
        .claim(&fixture.node, false)
        .await
        .unwrap()
        .is_none());
    let claim = fixture
        .store
        .claim(&fixture.node, true)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    let claimed = &claim["requests"][0];
    assert_eq!(claimed["operation"], "directory_create");
    assert_eq!(claimed["payload"], body["payload"]);
    assert_eq!(
        claimed["gateway_route"],
        json!({"schema":1,"route_id":route,"computer_id":body["computer_id"],"machine_id":machine})
    );
    heartbeat["peers"] = json!([]);
    fixture
        .store
        .heartbeat(&fixture.node, &heartbeat, &canonical(&snapshot).unwrap())
        .await
        .unwrap();
    let receipt = fixture
        .store
        .receipt(&fixture.phone, s(&body, "request_id"))
        .await
        .unwrap();
    assert_eq!(receipt["state"], "uncertain");
    assert!(fixture
        .store
        .claim(&fixture.node, true)
        .await
        .unwrap()
        .is_none());
    assert_eq!(fixture.submit(&body).await.unwrap()["state"], "uncertain");
    fixture.close().await;
}
