//! Wake delivery contracts that only the Python reference relay's tests
//! covered: generic payloads, bounded retries, revocation and unregistered
//! tokens, and the data-only FCM message. `ORACLE.md` maps each Python test
//! to its contract here.
use serde_json::{json, Value};
use zerus_relay::{
    args,
    db::{f, i, now, s},
    push::{fcm_message, fcm_unregistered},
    store::Role,
};

mod common;
use common::Fixture;

/// Without presence the owner counts as away, so a new question alerts and
/// queues one wake per registered phone at once.
async fn question(fx: &Fixture, attention: &str) {
    for (phase, id) in [("working", ""), ("input", attention)] {
        fx.beat(&json!({"sessions":[{"name":"private/session","run_id":"r","conversation_id":"c","phase":phase,"activity":"busy","attention_id":id}]}))
            .await;
    }
}
async fn registered(fx: &Fixture) -> Value {
    fx.store
        .push_registration(&fx.phone.id)
        .await
        .unwrap()
        .expect("a registration")
}
async fn job(fx: &Fixture) -> Value {
    fx.store
        .db
        .one("SELECT * FROM push_jobs", &[])
        .await
        .unwrap()
        .expect("a wake job")
}

#[tokio::test]
async fn wakes_are_generic_back_off_and_leave_with_the_device() {
    let fx = Fixture::new().await;
    fx.store
        .register_push(&fx.phone, "fcm", "opaque")
        .await
        .unwrap();
    question(&fx, "q1").await;
    let queued = job(&fx).await;
    let payload: Value = serde_json::from_str(s(&queued, "payload")).unwrap();
    assert_eq!(
        payload.as_object().unwrap().keys().collect::<Vec<_>>(),
        ["event_id", "kind"]
    );
    assert_eq!(payload["kind"], "wake");
    let claimed = fx.store.claim_push_jobs().await.unwrap();
    assert_eq!(claimed.len(), 1);
    let reg = registered(&fx).await;
    fx.store
        .finish_push(&claimed[0], false, false, Some(&reg))
        .await
        .unwrap();
    let retry = job(&fx).await;
    assert_eq!(i(&retry, "attempts"), 1);
    assert!(f(&retry, "next_at") > now());
    fx.store.revoke(Role::Device, &fx.phone.id).await.unwrap();
    assert_eq!(fx.count("push_jobs").await, 0);
    assert_eq!(fx.count("pushes").await, 0);
    let usage = fx
        .store
        .db
        .one("SELECT push_jobs FROM global_usage WHERE id=1", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&usage, "push_jobs"), 0);
    fx.close().await;
}

#[tokio::test]
async fn retries_are_bounded_and_an_unregistered_token_is_removed() {
    let fx = Fixture::new().await;
    fx.store
        .register_push(&fx.phone, "fcm", "opaque")
        .await
        .unwrap();
    question(&fx, "q1").await;
    let reg = registered(&fx).await;
    for attempt in 0..5 {
        fx.store
            .db
            .exec("UPDATE push_jobs SET next_at=0", &[])
            .await
            .unwrap();
        let claimed = fx.store.claim_push_jobs().await.unwrap();
        assert_eq!(claimed.len(), 1, "attempt {attempt}");
        fx.store
            .finish_push(&claimed[0], false, false, Some(&reg))
            .await
            .unwrap();
    }
    // The fifth failure gives up; the registration itself stays.
    assert_eq!(fx.count("push_jobs").await, 0);
    assert!(fx.store.claim_push_jobs().await.unwrap().is_empty());
    assert_eq!(registered(&fx).await, reg);
    question(&fx, "q2").await;
    let claimed = fx.store.claim_push_jobs().await.unwrap();
    fx.store
        .finish_push(&claimed[0], false, true, Some(&reg))
        .await
        .unwrap();
    assert!(fx
        .store
        .push_registration(&fx.phone.id)
        .await
        .unwrap()
        .is_none());
    assert_eq!(fx.count("push_jobs").await, 0);
    fx.close().await;
}

#[test]
fn fcm_messages_carry_only_generic_string_data() {
    let message = fcm_message("example-token", &json!({"event_id":1,"kind":"wake"})).unwrap();
    assert_eq!(
        message,
        json!({"message":{"token":"example-token","data":{"event_id":"1","kind":"wake"},
            "android":{"priority":"high","ttl":"300s"}}})
    );
    assert!(message["message"].get("notification").is_none());
    assert!(message["message"]["android"].get("notification").is_none());
    assert!(fcm_message("example-token", &json!(["wake"])).is_err());
    let unregistered = json!({"error":{"code":404,"details":[
        {"@type":"type.googleapis.com/google.firebase.fcm.v1.FcmError","errorCode":"UNREGISTERED"}]}});
    assert!(fcm_unregistered(&unregistered));
    for other in [
        json!({"error":{"code":503,"details":[
            {"@type":"type.googleapis.com/google.firebase.fcm.v1.FcmError","errorCode":"UNAVAILABLE"}]}}),
        json!({"error":{"code":404,"details":[{"@type":"other","errorCode":"UNREGISTERED"}]}}),
        json!({"error":"UNREGISTERED"}),
    ] {
        assert!(!fcm_unregistered(&other), "{other}");
    }
}

#[tokio::test]
async fn a_revoked_phone_gets_no_new_wakes() {
    let fx = Fixture::new().await;
    fx.store
        .register_push(&fx.phone, "fcm", "opaque")
        .await
        .unwrap();
    fx.store.revoke(Role::Device, &fx.phone.id).await.unwrap();
    question(&fx, "q1").await;
    assert_eq!(fx.count("push_jobs").await, 0);
    let alerts = fx
        .store
        .db
        .one(
            "SELECT count(*) AS n FROM events WHERE workspace_id=? AND kind='alert'",
            args![&fx.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(i(&alerts, "n"), 1);
    fx.close().await;
}
