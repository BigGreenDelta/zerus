//! Stored effects behind the released HTTP contracts of
//! `services/mobile/tests/test_rust_relay.py` that HTTP cannot show: elapsed
//! time beyond the relay's settings and the rows themselves.
use serde_json::{json, Value};
use zerus_relay::{
    args,
    db::{i, now, s},
    json::{canonical, digest},
    store::Role,
};

mod common;
use common::Fixture;

async fn count(f: &Fixture, column: &str, value: &str) -> i64 {
    let sql = format!("SELECT count(*) AS n FROM {column}=?");
    i(
        &f.store.db.one(&sql, args![value]).await.unwrap().unwrap(),
        "n",
    )
}

async fn complete(f: &Fixture, b: &Value) {
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    let e = canonical(&result["result"]).unwrap();
    f.store
        .result(&f.node, s(b, "request_id"), &result, Some(&e))
        .await
        .unwrap();
}

#[tokio::test]
async fn invitations_expire_and_credentials_are_stored_only_as_hashes() {
    let f = Fixture::new().await;
    for (column, secret) in [
        ("devices WHERE token_hash", s(&f.paired, "device_token")),
        ("nodes WHERE token_hash", s(&f.credentials, "node_token")),
    ] {
        assert_eq!(count(&f, column, secret).await, 0);
        assert_eq!(count(&f, column, &digest(secret)).await, 1);
    }
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let code = s(&invitation, "pair_code");
    assert_eq!(count(&f, "invitations WHERE code_hash", code).await, 0);
    assert_eq!(
        count(&f, "invitations WHERE code_hash", &digest(code)).await,
        1
    );
    f.store
        .db
        .exec(
            "UPDATE invitations SET expires=? WHERE code_hash=?",
            args![now() - 1.0, &digest(code)],
        )
        .await
        .unwrap();
    assert_eq!(
        f.store.pair(code, "Late").await.unwrap_err().0.as_u16(),
        401
    );
    // Pairing consumed the provisioned invitation; maintenance removes this one.
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count("invitations").await, 0);
    f.close().await;
}

#[tokio::test]
async fn device_logout_fails_its_queued_requests() {
    let f = Fixture::new().await;
    let b = f.command("send");
    assert_eq!(f.submit(&b).await.unwrap()["state"], "queued");
    // What DELETE /v1/device does for the calling device.
    f.store.revoke(Role::Device, &f.phone.id).await.unwrap();
    let row = f
        .store
        .db
        .one(
            "SELECT state,error FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        (&row["state"], &row["error"]),
        (
            &json!("failed"),
            &json!("credential revoked before delivery")
        )
    );
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn read_history_expires_without_consuming_mutation_tombstones() {
    let f = Fixture::new().await;
    let (read, mutation) = (f.command("inspect"), f.command("send"));
    for b in [&read, &mutation] {
        f.submit(b).await.unwrap();
        complete(&f, b).await;
    }
    let age = |b: &Value, seconds: f64| {
        let id = b["request_id"].clone();
        let db = f.store.db.clone();
        async move {
            db.exec(
                "UPDATE requests SET updated=? WHERE id=?",
                args![now() - seconds, &id],
            )
            .await
            .unwrap();
        }
    };
    // An hour later the read is gone and its identity is new again; the
    // mutation keeps its whole receipt.
    age(&read, 3601.0).await;
    age(&mutation, 3601.0).await;
    f.store.maintain(false).await.unwrap();
    let missing = f.store.receipt(&f.phone, s(&read, "request_id")).await;
    assert_eq!(missing.unwrap_err().0.as_u16(), 404);
    let receipt = f.submit(&mutation).await.unwrap();
    assert_eq!(
        (&receipt["state"], &receipt["result"]),
        (&json!("completed"), &json!({"ok":true}))
    );
    assert_eq!(f.submit(&read).await.unwrap()["state"], "queued");
    // After the retention the mutation keeps only its identity and state.
    age(&mutation, 8.0 * 86400.0).await;
    f.store.maintain(false).await.unwrap();
    let receipt = f.submit(&mutation).await.unwrap();
    assert_eq!(
        (&receipt["state"], &receipt["result"], &receipt["error"]),
        (
            &json!("completed"),
            &Value::Null,
            &json!("request history expired; delivery must not be retried")
        )
    );
    let mut changed = mutation.clone();
    changed["session"] = json!("changed");
    assert_eq!(f.submit(&changed).await.unwrap_err().0.as_u16(), 409);
    f.check_accounting().await;
    f.close().await;
}
