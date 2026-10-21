use serde_json::{json, Value};
use zerus_relay::{
    alerts::Candidate,
    json::canonical,
    protocol,
    registry::{event_changes, notification_revisions_current},
};
mod common;
use common::Fixture;

#[tokio::test]
async fn projection_growth_then_shrink_fits_in_both_row_orders() {
    use zerus_relay::notification_highwater;
    let f = Fixture::new().await;
    let mut grow = row();
    grow["conversation_id"] = json!("grow");
    grow["notification_preferences"]["conversation_id"] = json!("grow");
    grow["notification_preferences"]["revision"] = json!(1);
    for index in 0..5 {
        grow["notification_preferences"]["suppressed"]["child_attention"]
            [format!("child-{index}")] =
            json!({"phase":"input","request_id":"x".repeat(65536),"started_at":null});
    }
    let mut shrink = row();
    shrink["conversation_id"] = json!("shrink");
    shrink["notification_preferences"]["conversation_id"] = json!("shrink");
    shrink["notification_preferences"]["revision"] = json!(1);
    let mut t = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut t, &f.node.workspace)
        .await
        .unwrap();
    notification_highwater::accept(
        &f.store.db,
        &mut t,
        &f.node.workspace,
        &f.node.id,
        &json!({"sessions":[grow.clone(),shrink.clone()]}),
    )
    .await
    .unwrap();
    t.commit().await.unwrap();
    let mut old_grow = row();
    old_grow["conversation_id"] = json!("grow");
    old_grow["notification_preferences"]["conversation_id"] = json!("grow");
    let mut old_shrink = grow.clone();
    old_shrink["conversation_id"] = json!("shrink");
    old_shrink["notification_preferences"]["conversation_id"] = json!("shrink");
    old_shrink["notification_preferences"]["revision"] = json!(0);
    for rows in [
        vec![old_grow.clone(), old_shrink.clone()],
        vec![old_shrink, old_grow],
    ] {
        let mut snapshot = json!({"sessions":rows,"padding":"p".repeat(500000)});
        let mut t = f.store.db.begin().await.unwrap();
        notification_highwater::overlay(&mut t, &f.node.id, &mut snapshot)
            .await
            .unwrap();
        assert!(canonical(&snapshot).unwrap().len() <= 1024 * 1024);
        for row in snapshot["sessions"].as_array().unwrap() {
            assert_eq!(
                row["notification_preferences"],
                if row["conversation_id"] == "grow" {
                    grow["notification_preferences"].clone()
                } else {
                    shrink["notification_preferences"].clone()
                }
            );
        }
    }
    f.close().await;
}

#[tokio::test]
async fn repeated_scopes_refuse_projection_before_any_preference_is_cloned_into_rows() {
    use zerus_relay::notification_highwater;
    let f = Fixture::new().await;
    let mut large = row();
    large["notification_preferences"]["revision"] = json!(1);
    for index in 0..5 {
        large["notification_preferences"]["suppressed"]["child_attention"]
            [format!("child-{index}")] =
            json!({"phase":"input","request_id":"x".repeat(65536),"started_at":null});
    }
    let mut t = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut t, &f.node.workspace)
        .await
        .unwrap();
    notification_highwater::accept(
        &f.store.db,
        &mut t,
        &f.node.workspace,
        &f.node.id,
        &json!({"sessions":[large]}),
    )
    .await
    .unwrap();
    t.commit().await.unwrap();
    for absent in [false, true] {
        let mut original = row();
        if absent {
            original
                .as_object_mut()
                .unwrap()
                .remove("notification_preferences");
        }
        let mut snapshot = json!({"sessions":vec![original;10]});
        let before = snapshot.clone();
        let mut t = f.store.db.begin().await.unwrap();
        assert!(
            notification_highwater::overlay(&mut t, &f.node.id, &mut snapshot)
                .await
                .is_err()
        );
        assert_eq!(snapshot, before); // Refusal precedes every replacement, not only final serialization.
    }
    f.close().await;
}

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
fn unmute_suppresses_a_held_candidate_when_its_current_native_signature_was_captured() {
    let original = row();
    let held = Candidate::capture(&json!({"sessions":[original.clone()]}), "s", "attention");
    let mut changed = original;
    changed["attention_signature"] = json!("child-change");
    assert!(!held.resolved(Some(&changed))); // A change alone is not an acknowledgement.
    changed["notification_preferences"]["revision"] = json!(2);
    changed["notification_preferences"]["suppressed"]["attention_signature"] =
        json!("child-change");
    assert!(held.resolved(Some(&changed)));
    changed["notification_preferences"]["suppressed"]["attention_signature"] = Value::Null;
    assert!(!held.resolved(Some(&changed)));
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
        let mut unknown = muted.clone();
        unknown["notification_preferences"] = value;
        assert!(
            !notification_revisions_current(Some(&known), &json!({"sessions":[unknown]})).unwrap()
        );
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

#[tokio::test]
async fn physical_highwater_projects_a_stale_route_without_touching_native_state() {
    use zerus_relay::{args, notification_highwater};
    let f = Fixture::new().await;
    let original = row();
    let mut muted = original.clone();
    muted["notification_preferences"]["muted"] = json!(true);
    muted["notification_preferences"]["revision"] = json!(1);
    let mut t = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut t, &f.node.workspace)
        .await
        .unwrap();
    notification_highwater::accept(
        &f.store.db,
        &mut t,
        &f.node.workspace,
        &f.node.id,
        &json!({"sessions":[muted.clone()]}),
    )
    .await
    .unwrap();
    t.commit().await.unwrap();
    let mut t = f.store.db.begin().await.unwrap();
    let mut stale = json!({"sessions":[original.clone()]});
    notification_highwater::overlay(&mut t, &f.node.id, &mut stale)
        .await
        .unwrap();
    assert_eq!(stale["sessions"][0], muted);
    assert!(
        Candidate::capture(&json!({"sessions":[original.clone()]}), "s", "attention")
            .resolved(Some(&stale["sessions"][0]))
    );
    let charge = t
        .one(
            "SELECT payload_bytes FROM computer_notification_preferences WHERE computer_id=?",
            args![&f.node.id],
        )
        .await
        .unwrap()
        .unwrap();
    let usage = t
        .one(
            "SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?",
            args![&f.node.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!(charge["payload_bytes"], usage["payload_bytes"]);
    t.commit().await.unwrap();
    let mut t = f.store.db.begin().await.unwrap();
    f.store
        .db
        .payload_usage(&mut t, &f.node.workspace)
        .await
        .unwrap();
    assert!(notification_highwater::accept(
        &f.store.db,
        &mut t,
        &f.node.workspace,
        &f.node.id,
        &json!({"sessions":[original]})
    )
    .await
    .is_err());
    drop(t);
    let mut t = f.store.db.begin().await.unwrap();
    let mut resumed = muted.clone();
    resumed["name"] = json!("s/project/renamed");
    resumed["run_id"] = json!("resumed");
    notification_highwater::overlay(
        &mut t,
        &f.node.id,
        &mut json!({"sessions":[resumed.clone()]}),
    )
    .await
    .unwrap();
    let mut separate = resumed;
    separate["name"] = json!("other/session");
    separate["notification_preferences"]["muted"] = json!(false);
    separate["notification_preferences"]["revision"] = json!(0);
    let mut snapshot = json!({"sessions":[separate.clone()]});
    notification_highwater::overlay(&mut t, &f.node.id, &mut snapshot)
        .await
        .unwrap();
    assert_eq!(snapshot["sessions"][0], separate);
    t.commit().await.unwrap();
    f.store.db.close().await;
}
