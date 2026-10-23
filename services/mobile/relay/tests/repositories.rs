use serde_json::{json, Value};
use zerus_relay::{protocol, repositories};
mod common;
const ID: &str = "11111111-1111-4111-8111-111111111111";
fn request(op: &str) -> Value {
    let mut p = json!({"request_id":ID,"expected_machine_id":ID,"url":"git@forge:owner/source.git","parent":"/fixture/parent","name":"Source ; $ literal"});
    if op == "repository_status" {
        p["clone_request_id"] = json!(ID);
    }
    json!({"request_id":ID,"computer_id":ID,"session":"","operation":op,"payload":p})
}
#[test]
fn exact_additive_envelopes_and_read_classes() {
    for op in repositories::OPERATIONS {
        let r = request(op);
        protocol::request(&r).unwrap();
        assert_eq!(protocol::READS.contains(op), *op != "repository_clone");
        assert!(!protocol::READ_SQL.contains(op));
        for key in ["request_id", "expected_machine_id", "url", "parent", "name"] {
            let mut bad = r.clone();
            bad["payload"].as_object_mut().unwrap().remove(key);
            assert!(protocol::request(&bad).is_err(), "{op} missing {key}");
        }
        for (key, value) in [
            ("request_id", json!("22222222-2222-4222-8222-222222222222")),
            ("expected_machine_id", json!("unknown")),
            (
                "expected_machine_id",
                json!("00000000-0000-0000-0000-000000000000"),
            ),
            ("agent", json!("codex")),
            ("command", json!([])),
        ] {
            let mut bad = r.clone();
            bad["payload"][key] = value;
            assert!(protocol::request(&bad).is_err());
        }
        let mut bad = r.clone();
        bad["session"] = json!("codex/fixture/existing");
        assert!(protocol::request(&bad).is_err());
    }
}
#[test]
fn urls_and_destinations_refuse_local_paths_credentials_and_devices() {
    for good in [
        "https://example.test/owner/repo.git",
        "ssh://git@example.test/owner/repo",
        "git@forge:owner/repo",
        "forge:repo",
    ] {
        assert!(repositories::valid_url(good), "{good}");
    }
    for bad in [
        "C:/source",
        "C:\\source",
        "//server/share",
        "\\\\server\\share",
        "\\\\?\\C:\\source",
        "/source",
        "../source",
        "file:///source",
        "ext::command",
        "https://token@example.test/repo",
        "ssh://user:password@example.test/repo",
        "https://example.test/repo?token=x",
        "https://example.test/repo#token",
        "git@forge:",
        "https://example.test/repo\n",
    ] {
        assert!(!repositories::valid_url(bad), "{bad}");
    }
    for bad in [
        ".",
        "..",
        "CON",
        "com¹.log",
        "LPT9.foo",
        "NUL .log",
        "nested/name",
        "trailing.",
        " name",
        "bad\nname",
    ] {
        let mut r = request("repository_clone");
        r["payload"]["name"] = json!(bad);
        assert!(protocol::request(&r).is_err(), "{bad}");
    }
    let mut r = request("repository_check");
    r["payload"]["parent"] = json!("~");
    protocol::request(&r).unwrap();
    r["operation"] = json!("repository_clone");
    assert!(protocol::request(&r).is_err());
}

#[tokio::test]
async fn durable_clone_and_bounded_read_retention_remain_separate() {
    use zerus_relay::{
        args,
        db::{now, s},
        json::canonical,
    };
    let f = common::Fixture::new().await;
    let mut clone = request("repository_clone");
    clone["computer_id"] = json!(f.node.id);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":[]}}))
        .await;
    assert_eq!(f.submit(&clone).await.unwrap_err().0.as_u16(), 409);
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":repositories::OPERATIONS}})).await;
    let mut bodies = vec![clone.clone()];
    for op in repositories::READS {
        let mut body = request(op);
        let id = uuid::Uuid::new_v4().to_string();
        body["request_id"] = json!(id);
        body["payload"]["request_id"] = json!(id);
        body["computer_id"] = json!(f.node.id);
        bodies.push(body);
    }
    for body in &bodies {
        f.submit(body).await.unwrap();
        f.submit(body).await.unwrap();
        let mut changed = body.clone();
        changed["payload"]["name"] = json!("different");
        assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
        let claimed = f
            .store
            .claim(&f.node, false)
            .await
            .unwrap()
            .unwrap()
            .decode()
            .unwrap();
        assert_eq!(claimed["requests"][0]["request_id"], body["request_id"]);
        let receipt = json!({"state":"completed","result":{"status":"cloned"},"error":null});
        f.store
            .result(
                &f.node,
                s(body, "request_id"),
                &receipt,
                Some(&canonical(&receipt["result"]).unwrap()),
            )
            .await
            .unwrap();
        f.store
            .db
            .exec(
                "UPDATE requests SET updated=? WHERE id=?",
                args![now() - 3601.0, &body["request_id"]],
            )
            .await
            .unwrap();
    }
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("requests").await, 1);
    assert_eq!(f.submit(&clone).await.unwrap()["state"], "completed");
    f.check_accounting().await;
    f.close().await;
}
