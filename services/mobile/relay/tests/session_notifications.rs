use serde_json::{json, Value};
use zerus_relay::{
    alerts::Candidate,
    json::canonical,
    protocol,
    registry::{event_changes, notification_revisions_current},
};
mod common;
use common::Fixture;

fn row() -> Value {
    json!({"name":"s","run_id":"r","conversation_id":"c","phase":"input","activity":"idle","attention_signature":"a","reply_id":"p","notification_preferences":{"conversation_id":"c","muted":false,"revision":0,"suppressed":{"reply_id":null,"attention_signature":null,"optional_question_ids":[],"main_attention":null,"child_attention":{}}}})
}

#[test]
fn payload_is_a_strict_mutation_not_a_read_or_acknowledgement() {
    let request = json!({"request_id":"00000000-0000-4000-8000-000000000001","computer_id":"00000000-0000-4000-8000-000000000002","operation":"session_notifications","session":"codex/example/session","payload":{"run_id":"r","conversation_id":"c","muted":true,"expected_revision":0}});
    protocol::request(&request).unwrap();
    assert!(!protocol::READS.contains(&"session_notifications"));
    for (key, value) in [
        ("muted", json!(1)),
        ("expected_revision", json!(true)),
        ("expected_revision", json!(0.0)),
        ("expected_revision", json!(-1)),
        ("expected_revision", json!(u64::MAX)),
        ("run_id", json!("")),
        ("run_id", json!("é".repeat(65))),
        ("conversation_id", json!("é".repeat(129))),
    ] {
        let mut bad = request.clone();
        bad["payload"][key] = value;
        assert!(protocol::request(&bad).is_err());
    }
    let mut bad = request.clone();
    bad["payload"]["reply_id"] = json!("p");
    assert!(protocol::request(&bad).is_err());
}

#[test]
fn muting_drops_existing_and_new_alert_candidates_without_marking_work_read() {
    let original = row();
    let mut muted = original.clone();
    muted["notification_preferences"]["muted"] = json!(true);
    muted["notification_preferences"]["revision"] = json!(1);
    for kind in ["attention", "error", "completed"] {
        let candidate = Candidate::capture(&json!({"sessions":[original.clone()]}), "s", kind);
        assert!(candidate.resolved(Some(&muted)));
        assert!(!candidate.resolved(Some(&original)));
    }
    assert_eq!(muted["phase"], "input");
    assert_eq!(muted["reply_id"], "p");
    assert!(muted["read"].is_null());
    let mut wrong = muted.clone();
    wrong["notification_preferences"]["conversation_id"] = json!("other");
    let candidate = Candidate::capture(&json!({"sessions":[original]}), "s", "attention");
    assert!(!candidate.resolved(Some(&wrong)));
}

#[test]
fn unmute_baseline_suppresses_old_work_even_when_phone_missed_the_whole_muted_interval() {
    let original = row();
    let mut unmuted = original.clone();
    unmuted["notification_preferences"]["revision"] = json!(2);
    unmuted["notification_preferences"]["suppressed"]["reply_id"] = json!("p");
    unmuted["notification_preferences"]["suppressed"]["attention_signature"] = json!("a");
    for kind in ["attention", "completed"] {
        let candidate = Candidate::capture(&json!({"sessions":[original.clone()]}), "s", kind);
        assert!(candidate.resolved(Some(&unmuted)));
    }
    let mut fresh = unmuted.clone();
    fresh["attention_signature"] = json!("new");
    fresh["reply_id"] = json!("new-p");
    for kind in ["attention", "completed"] {
        let candidate = Candidate::capture(&json!({"sessions":[fresh.clone()]}), "s", kind);
        assert!(!candidate.resolved(Some(&fresh)));
    }
    assert!(unmuted["read"].is_null());
}

#[test]
fn preference_only_changes_emit_plain_refresh_not_an_invented_attention_edge() {
    let before = row();
    let mut after = before.clone();
    after["notification_preferences"]["muted"] = json!(true);
    after["notification_preferences"]["revision"] = json!(1);
    let old = canonical(&json!({"sessions":[before]})).unwrap();
    assert_eq!(
        event_changes(Some(&old), &json!({"sessions":[after]})).unwrap(),
        vec![("s".into(), "notification_preferences".into())]
    );
}

#[test]
fn late_poll_cannot_roll_back_a_confirmed_revision_or_change_its_meaning() {
    let old = row();
    let mut muted = old.clone();
    muted["notification_preferences"]["muted"] = json!(true);
    muted["notification_preferences"]["revision"] = json!(1);
    let known = canonical(&json!({"sessions":[muted.clone()]})).unwrap();
    assert!(!notification_revisions_current(Some(&known), &json!({"sessions":[old]})).unwrap());
    let mut conflict = muted.clone();
    conflict["notification_preferences"]["muted"] = json!(false);
    assert!(
        !notification_revisions_current(Some(&known), &json!({"sessions":[conflict]})).unwrap()
    );
    for value in [Value::Null, json!({}), json!({"conversation_id":"other"})] {
        let mut unknown = muted.clone(); unknown["notification_preferences"] = value;
        assert!(!notification_revisions_current(Some(&known), &json!({"sessions":[unknown]})).unwrap());
    }
    let mut resumed = muted.clone();
    resumed["run_id"] = json!("resumed");
    resumed["name"] = json!("s/renamed");
    assert!(notification_revisions_current(Some(&known), &json!({"sessions":[resumed]})).unwrap());
    let mut next = muted;
    next["conversation_id"] = json!("new");
    next["notification_preferences"]["conversation_id"] = json!("new");
    next["notification_preferences"]["revision"] = json!(0);
    assert!(notification_revisions_current(Some(&known), &json!({"sessions":[next]})).unwrap());
}

#[tokio::test]
async fn shared_mute_retires_held_alerts_and_only_wakes_phones_to_refresh_the_preference() {
    let f = Fixture::new().await;
    f.store
        .register_push(&f.phone, "fcm", "synthetic-token")
        .await
        .unwrap();
    let active = json!({"idle_seconds":1.0,"locked":false});
    let listing = |v: Value| json!({"sessions":[v]});
    let mut busy = row();
    busy["phase"] = json!("tool");
    busy["activity"] = json!("busy");
    busy["attention_signature"] = Value::Null;
    f.beat_with(&listing(busy), &active).await;
    let mut waiting = row();
    waiting["attention_id"] = json!("q1");
    f.beat_with(&listing(waiting.clone()), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    assert_eq!(f.count("push_jobs").await, 0);
    waiting["notification_preferences"]["muted"] = json!(true);
    waiting["notification_preferences"]["revision"] = json!(1);
    f.beat_with(&listing(waiting.clone()), &active).await;
    assert_eq!(f.count("alert_candidates").await, 0);
    assert_eq!(f.alert_count().await, 0);
    let refresh = f.events_of("notification_preferences").await;
    assert_eq!(refresh.len(), 1);
    let jobs = f.store.claim_push_jobs().await.unwrap();
    assert_eq!(jobs.len(), 1);
    let payload = jobs[0]["payload"].clone();
    waiting["attention_id"] = json!("q2");
    waiting["attention_signature"] = json!("b");
    f.beat_with(&listing(waiting.clone()), &active).await;
    assert_eq!(f.count("alert_candidates").await, 0);
    assert_eq!(f.alert_count().await, 0);
    let job = f
        .store
        .db
        .one("SELECT payload FROM push_jobs", &[])
        .await
        .unwrap()
        .unwrap();
    assert_eq!(job["payload"], payload);
    waiting["notification_preferences"]["muted"] = json!(false);
    waiting["notification_preferences"]["revision"] = json!(2);
    waiting["notification_preferences"]["suppressed"]["attention_signature"] = json!("b");
    f.beat_with(&listing(waiting.clone()), &active).await;
    assert_eq!(f.count("alert_candidates").await, 0);
    assert_eq!(f.alert_count().await, 0);
    waiting["attention_id"] = json!("q3");
    waiting["attention_signature"] = json!("c");
    f.beat_with(&listing(waiting.clone()), &active).await;
    assert_eq!(f.count("alert_candidates").await, 1);
    f.beat_with(
        &listing(waiting),
        &json!({"idle_seconds":4.0,"locked":true,"locked_seconds":2.0}),
    )
    .await;
    assert_eq!(f.alert_count().await, 1);
    assert_eq!(f.count("alert_candidates").await, 0);
    f.close().await;
}
