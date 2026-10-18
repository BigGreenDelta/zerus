//! Optional prompt dismissal keeps exact context identities and existing receipts.
mod common;

use common::Fixture;
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{
    args,
    db::{i, now, s},
    json::{canonical, digest},
    protocol,
    store::{Role, Submission},
};

const FEATURE: &str = "clear_context_dismiss";

fn command(computer: &str, dismiss: Option<bool>) -> Value {
    let id = Uuid::new_v4().to_string();
    let mut body = json!({"request_id":id,"computer_id":computer,"operation":"clear_context",
        "session":"codex/example/tag","payload":{"request_id":id,
        "expected_run_id":"run-original","expected_conversation_id":"conversation-original"}});
    if let Some(dismiss) = dismiss {
        body["payload"]["dismiss_prompt"] = json!(dismiss);
    }
    body
}

fn snapshot(feature: bool) -> Value {
    json!({"sessions":["codex/example/tag"],"mobile_capabilities":{"protocol_version":1,
        "operations":["clear_context","compact_context"],"features":if feature {vec![FEATURE,"gateway_one_hop"]} else {vec!["gateway_one_hop"]}}})
}

#[test]
fn dismiss_prompt_is_an_optional_actual_boolean_on_clear_context_only() {
    let computer = Uuid::new_v4().to_string();
    let extension = command(&computer, Some(true));
    protocol::request(&extension).unwrap();
    protocol::request(&command(&computer, Some(false))).unwrap();
    protocol::request(&command(&computer, None)).unwrap();
    assert!(protocol::FEATURES.contains(&FEATURE));
    assert!(protocol::capabilities(vec![])["features"]
        .as_array()
        .unwrap()
        .contains(&json!(FEATURE)));
    for value in [
        Value::Null,
        json!(0),
        json!(1),
        json!(0.0),
        json!("false"),
        json!("true"),
        json!([]),
        json!({}),
    ] {
        let mut bad = extension.clone();
        bad["payload"]["dismiss_prompt"] = value;
        assert!(protocol::request(&bad).is_err(), "accepted {bad}");
    }
    for value in [json!(true), json!(false), Value::Null] {
        let mut bad = extension.clone();
        bad["operation"] = json!("compact_context");
        bad["payload"]["dismiss_prompt"] = value;
        assert!(protocol::request(&bad).is_err());
    }
    let mut compact = command(&computer, None);
    compact["operation"] = json!("compact_context");
    protocol::request(&compact).unwrap();
    for (key, value) in [
        ("archive_id", json!(Uuid::new_v4().to_string())),
        ("agent_id", json!("other-agent")),
        ("text", json!("/clear")),
        ("expected_run_id", json!("")),
        ("expected_conversation_id", json!("")),
    ] {
        let mut bad = extension.clone();
        bad["payload"][key] = value;
        assert!(protocol::request(&bad).is_err(), "accepted {key}");
    }
}

#[tokio::test]
async fn any_boolean_presence_is_gated_after_durable_duplicate_lookup() {
    let f = Fixture::new().await;
    f.beat(&snapshot(false)).await;
    let ordinary = command(&f.node.id, None);
    assert_eq!(f.submit(&ordinary).await.unwrap()["state"], "queued");
    let row = f
        .store
        .db
        .one(
            "SELECT body,body_hash FROM requests WHERE id=?",
            args![&ordinary["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    let encoded = canonical(&ordinary).unwrap();
    assert_eq!(row["body"], encoded);
    assert_eq!(row["body_hash"], digest(&encoded));
    assert!(!encoded.contains("dismiss_prompt"));
    for dismiss in [false, true] {
        assert_eq!(
            f.submit(&command(&f.node.id, Some(dismiss)))
                .await
                .unwrap_err()
                .0
                .as_u16(),
            409
        );
    }
    assert_eq!(f.count("requests").await, 1);
    f.beat(&snapshot(true)).await;
    let mut admitted = vec![];
    for dismiss in [false, true] {
        let body = command(&f.node.id, Some(dismiss));
        let (status, receipt) = f
            .store
            .submit(&f.phone, &Submission::new(body.clone()).unwrap())
            .await
            .unwrap();
        assert_eq!(status.as_u16(), 202);
        assert_eq!(receipt["state"], "queued");
        admitted.push(body);
    }
    f.beat(&snapshot(false)).await;
    for body in admitted {
        let (status, receipt) = f
            .store
            .submit(&f.phone, &Submission::new(body.clone()).unwrap())
            .await
            .unwrap();
        assert_eq!(status.as_u16(), 202);
        assert_eq!(receipt["state"], "queued");
        let mut changed = body.clone();
        changed["payload"]["dismiss_prompt"] =
            json!(!body["payload"]["dismiss_prompt"].as_bool().unwrap());
        assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
        for key in ["expected_run_id", "expected_conversation_id"] {
            changed = body.clone();
            changed["payload"][key] = json!("another-identity");
            assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
        }
    }
    for dismiss in [false, true] {
        assert_eq!(
            f.submit(&command(&f.node.id, Some(dismiss)))
                .await
                .unwrap_err()
                .0
                .as_u16(),
            409
        );
    }
    assert_eq!(
        f.submit(&command(&f.node.id, None)).await.unwrap()["state"],
        "queued"
    );
    assert_eq!(f.count("requests").await, 4);
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
    assert_eq!((i(&usage, "reads"), i(&usage, "mutations")), (0, 4));
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn dismissal_feature_comes_from_the_exact_peer_and_preserves_route_and_workspace() {
    let f = Fixture::new().await;
    let local = Uuid::new_v4().to_string();
    let remote = Uuid::new_v4().to_string();
    let route = Uuid::new_v4().to_string();
    for (gateway_feature, peer_feature) in [(true, false), (false, true)] {
        let gateway = snapshot(gateway_feature);
        f.store
            .heartbeat(
                &f.node,
                &json!({"snapshot":gateway,"machine_id":local,"peers":[{"route_id":route,"machine_id":remote,"name":"Peer","online":true}]}),
                &canonical(&gateway).unwrap(),
            )
            .await
            .unwrap();
        let peer = snapshot(peer_feature);
        f.store
            .peer_heartbeat(
                &f.node,
                &route,
                &json!({"machine_id":remote,"snapshot":peer}),
                &canonical(&peer).unwrap(),
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
        let body = command(s(target, "id"), Some(true));
        if !peer_feature {
            assert_eq!(f.submit(&body).await.unwrap_err().0.as_u16(), 409);
            continue;
        }
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
        assert_eq!(request["operation"], "clear_context");
        assert_eq!(request["session"], body["session"]);
        assert_eq!(request["payload"], body["payload"]);
        assert_eq!(
            request["gateway_route"],
            json!({"schema":1,"route_id":route,"computer_id":body["computer_id"],"machine_id":remote})
        );
        assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
        let other = f
            .store
            .provision("Other workspace", "Foreign")
            .await
            .unwrap();
        let foreign = command(s(&other, "node_id"), Some(true));
        assert_eq!(f.submit(&foreign).await.unwrap_err().0.as_u16(), 404);
        let paired = f
            .store
            .pair(s(&other, "pair_code"), "Other phone")
            .await
            .unwrap();
        let other_phone = f
            .store
            .authenticate(s(&paired, "device_token"), Role::Device)
            .await
            .unwrap();
        assert_eq!(
            f.store
                .receipt(&other_phone, s(&body, "request_id"))
                .await
                .unwrap_err()
                .0
                .as_u16(),
            404
        );
        let fresh = command(s(target, "id"), Some(true));
        assert_eq!(
            f.store
                .submit(&other_phone, &Submission::new(fresh).unwrap())
                .await
                .unwrap_err()
                .0
                .as_u16(),
            404
        );
    }
    assert_eq!(f.count("requests").await, 1);
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn dismissed_context_claim_stays_uncertain_after_restart_without_requeue() {
    let f = Fixture::new().await;
    f.beat(&snapshot(true)).await;
    let body = command(&f.node.id, Some(true));
    f.submit(&body).await.unwrap();
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["payload"], body["payload"]);
    f.store
        .db
        .exec(
            "UPDATE requests SET claimed=? WHERE id=?",
            args![
                now() - f.backend.cfg.claim_ttl as f64 - 1.0,
                &body["request_id"]
            ],
        )
        .await
        .unwrap();
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
    reopened.maintain(true).await.unwrap();
    assert_eq!(
        reopened
            .receipt(&phone, s(&body, "request_id"))
            .await
            .unwrap()["state"],
        "uncertain"
    );
    assert!(reopened.claim(&node, false).await.unwrap().is_none());
    let unavailable = snapshot(false);
    reopened
        .heartbeat(
            &node,
            &json!({"snapshot":unavailable}),
            &canonical(&unavailable).unwrap(),
        )
        .await
        .unwrap();
    let (status, receipt) = reopened
        .submit(&phone, &Submission::new(body.clone()).unwrap())
        .await
        .unwrap();
    assert_eq!(status.as_u16(), 202);
    assert_eq!(receipt["state"], "uncertain");
    let mut changed = body.clone();
    changed["payload"]["dismiss_prompt"] = json!(false);
    assert_eq!(
        reopened
            .submit(&phone, &Submission::new(changed).unwrap())
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert_eq!(
        reopened
            .submit(
                &phone,
                &Submission::new(command(&node.id, Some(true))).unwrap()
            )
            .await
            .unwrap_err()
            .0
            .as_u16(),
        409
    );
    assert!(reopened.claim(&node, false).await.unwrap().is_none());
    let usage = reopened
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!((i(&usage, "reads"), i(&usage, "mutations")), (0, 1));
    reopened.db.close().await;
}
