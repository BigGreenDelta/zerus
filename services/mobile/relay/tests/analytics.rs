//! Read-only analytics admission and short-lived transport retention.
mod common;
use common::Fixture;
use serde_json::{json, Value};
use zerus_relay::{args, db::now, json::canonical, protocol};

fn request() -> Value {
    json!({"request_id":"00000000-0000-4000-8000-000000000001",
        "computer_id":"00000000-0000-4000-8000-000000000002",
        "operation":"analytics_read","session":"",
        "payload":{"schema":1,"from":"2026-01-01","through":"2026-03-31"}})
}

#[test]
fn analytics_payload_is_calendar_valid_bounded_and_read_only() {
    let good = request();
    protocol::request(&good).unwrap();
    assert!(protocol::READS.contains(&"analytics_read"));
    for (key, value) in [
        ("schema", json!(true)),
        ("schema", json!(1.0)),
        ("schema", json!(2)),
        ("from", json!("0000-01-01")),
        ("from", json!("2026-1-01")),
        ("through", json!("2026-02-29")),
        ("through", json!("２０２６-03-31")),
        ("through", json!("2026-04-01")),
        ("through", json!("2025-12-31")),
        ("action", json!("purge")),
        ("enabled", json!(true)),
        ("path", json!("/fixture/private")),
    ] {
        let mut bad = good.clone();
        bad["payload"][key] = value;
        assert!(protocol::request(&bad).is_err(), "accepted {bad}");
    }
    let mut bad = good.clone();
    bad["session"] = json!("codex/fixture");
    assert!(protocol::request(&bad).is_err());
    let mut leap = good;
    leap["payload"]["from"] = json!("2024-02-29");
    leap["payload"]["through"] = json!("2024-02-29");
    protocol::request(&leap).unwrap();
}

#[tokio::test]
async fn analytics_is_capability_gated_and_completed_read_is_pruned_with_accounting() {
    let f = Fixture::new().await;
    let mut command = request();
    command["computer_id"] = json!(f.node.id);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":[]}}))
        .await;
    assert_eq!(f.submit(&command).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["analytics_read"]}})).await;
    f.submit(&command).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let receipt = json!({"state":"completed","result":{"schema":1,"buckets":[]},"error":null});
    f.store
        .result(
            &f.node,
            command["request_id"].as_str().unwrap(),
            &receipt,
            Some(&canonical(&receipt["result"]).unwrap()),
        )
        .await
        .unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE id=?",
            args![now() - 121.0, &command["request_id"]],
        )
        .await
        .unwrap();
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("requests").await, 0);
    f.check_accounting().await;
    f.close().await;
}
