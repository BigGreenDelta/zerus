//! Table-driven presence alert rule: preference × presence × read × time,
//! plus the transitions that must reach the relay as events.
use serde_json::{json, Value};
use zerus_relay::{
    alerts::{away_since, decide, Candidate, Decision, Delivery, Presence, RECHECK},
    json::canonical,
    registry::event_changes,
};

const NOW: f64 = 1_000_000.0;

/// `locked`: `None` unlocked, `Some(None)` locked without a reported duration.
fn machine(age: f64, idle: f64, locked: Option<Option<f64>>) -> Presence {
    let presence =
        json!({"idle_seconds":idle,"locked":locked.is_some(),"locked_seconds":locked.flatten()});
    Presence::from_value(&presence, NOW - age).expect("valid presence")
}
fn candidate(kind: &str) -> Candidate {
    let row = json!({"name":"s","run_id":"r","conversation_id":"c","attention_signature":"s1","reply_id":"p1","phase":"input"});
    Candidate::capture(&json!({ "sessions": [row] }), "s", kind)
}
fn with(base: Value, changes: Value) -> Value {
    let mut row = base;
    for (key, value) in changes.as_object().unwrap() {
        row[key] = value.clone();
    }
    row
}
fn state(changes: Value) -> Value {
    with(
        json!({"name":"s","run_id":"r","conversation_id":"c","attention_signature":"s1","reply_id":"p1","phase":"input","activity":"idle","read":null}),
        changes,
    )
}
/// The store applies the same order: resolution first, then the delivery decision.
fn outcome(c: &Candidate, row: &Value, delivery: Delivery, machines: &[Presence]) -> &'static str {
    if c.resolved(Some(row)) {
        "drop"
    } else if decide(delivery, away_since(machines), NOW) == Decision::Deliver {
        "deliver"
    } else {
        "wait"
    }
}

#[test]
fn delivery_follows_preference_presence_and_time() {
    use Decision::{Deliver, Wait};
    use Delivery::{Away, Immediate};
    let active = machine(5.0, 1.0, None);
    let cases = [
        (
            "immediate ignores an active desktop",
            Immediate,
            vec![active],
            Deliver,
        ),
        (
            "away waits while a desktop is in use",
            Away(0),
            vec![active],
            Wait(NOW + RECHECK),
        ),
        (
            "away rechecks at the idle threshold",
            Away(0),
            vec![machine(0.0, 110.0, None)],
            Wait(NOW + 10.0),
        ),
        (
            "reaching the idle threshold is away",
            Away(0),
            vec![machine(0.0, 120.0, None)],
            Deliver,
        ),
        (
            "a locked desktop is away",
            Away(0),
            vec![machine(5.0, 1.0, Some(Some(1.0)))],
            Deliver,
        ),
        (
            "a lock without duration counts from the last input",
            Away(5),
            vec![machine(0.0, 400.0, Some(None))],
            Deliver,
        ),
        (
            "any active desktop holds alerts",
            Away(0),
            vec![machine(0.0, 1000.0, None), active],
            Wait(NOW + RECHECK),
        ),
        (
            "presence exactly 120 s old is still present",
            Away(30),
            vec![machine(120.0, 0.0, None)],
            Wait(NOW + RECHECK),
        ),
        (
            "stale presence cannot keep the owner active",
            Away(0),
            vec![machine(121.0, 0.0, None)],
            Deliver,
        ),
        (
            "stale presence keeps its last activity for away minutes",
            Away(30),
            vec![machine(121.0, 0.0, None)],
            Wait(NOW + RECHECK),
        ),
        (
            "lid closed and locked 5 min ago: away_10 still waits",
            Away(10),
            vec![machine(300.0, 0.0, Some(Some(0.0)))],
            Wait(NOW + RECHECK),
        ),
        (
            "lid closed and locked 10 min ago: away_10 delivers",
            Away(10),
            vec![machine(600.0, 0.0, Some(Some(0.0)))],
            Deliver,
        ),
        (
            "asleep unlocked: away counts from the idle threshold",
            Away(10),
            vec![machine(700.0, 0.0, None)],
            Wait(NOW + RECHECK),
        ),
        (
            "no presence ever received means away since forever",
            Away(30),
            vec![],
            Deliver,
        ),
        (
            "away_5 counts from the end of activity",
            Away(5),
            vec![machine(0.0, 410.0, None)],
            Wait(NOW + 10.0),
        ),
        (
            "away_5 delivers after five minutes",
            Away(5),
            vec![machine(0.0, 420.0, None)],
            Deliver,
        ),
        (
            "away_10 delivers after ten minutes",
            Away(10),
            vec![machine(0.0, 720.0, None)],
            Deliver,
        ),
        (
            "away_15 still waits after ten minutes",
            Away(15),
            vec![machine(0.0, 720.0, None)],
            Wait(NOW + RECHECK),
        ),
        (
            "the latest activity across machines wins",
            Away(5),
            vec![
                machine(0.0, 1000.0, None),
                machine(0.0, 300.0, Some(Some(100.0))),
            ],
            Wait(NOW + RECHECK),
        ),
    ];
    for (name, delivery, machines, expected) in cases {
        assert_eq!(
            decide(delivery, away_since(&machines), NOW),
            expected,
            "{name}"
        );
    }
}

#[test]
fn preference_presence_read_matrix() {
    let held = candidate("attention");
    let unread = state(json!({}));
    let read = state(
        json!({"read":{"conversation_id":"c","reply_id":null,"attention_signature":"s1","at":1.0}}),
    );
    let active = vec![machine(5.0, 1.0, None)];
    let away_long = vec![machine(0.0, 4000.0, None)];
    let none: Vec<Presence> = vec![];
    let cases = [
        (Delivery::Immediate, &active, &unread, "deliver"),
        (Delivery::Immediate, &active, &read, "drop"),
        (Delivery::Away(0), &active, &unread, "wait"),
        (Delivery::Away(0), &away_long, &unread, "deliver"),
        (Delivery::Away(0), &none, &unread, "deliver"),
        (Delivery::Away(0), &away_long, &read, "drop"),
        (Delivery::Away(30), &away_long, &unread, "deliver"),
        (Delivery::Away(30), &active, &unread, "wait"),
        (Delivery::Away(30), &none, &read, "drop"),
    ];
    for (delivery, machines, row, expected) in cases {
        assert_eq!(
            outcome(&held, row, delivery, machines),
            expected,
            "{delivery:?} {row}"
        );
    }
}

#[test]
fn malformed_presence_is_missing() {
    for (name, presence) in [
        ("absent", Value::Null),
        ("not an object", json!("idle")),
        ("negative idle", json!({"idle_seconds":-1.0,"locked":false})),
        ("textual idle", json!({"idle_seconds":"5","locked":false})),
        ("missing lock state", json!({"idle_seconds":5.0})),
        (
            "textual lock duration",
            json!({"idle_seconds":5.0,"locked":true,"locked_seconds":"2"}),
        ),
        (
            "negative lock duration",
            json!({"idle_seconds":5.0,"locked":true,"locked_seconds":-2.0}),
        ),
    ] {
        assert_eq!(Presence::from_value(&presence, NOW), None, "{name}");
    }
    assert_eq!(
        Presence::from_value(
            &json!({"idle_seconds":5.0,"locked":false,"locked_seconds":null}),
            NOW
        ),
        Some(Presence {
            received: NOW,
            away_since: NOW + 115.0
        })
    );
}

#[test]
fn delivery_names_are_exact_and_default_to_away() {
    for name in [
        "immediate",
        "away",
        "away_5",
        "away_10",
        "away_15",
        "away_30",
    ] {
        assert_eq!(Delivery::parse(name).map(Delivery::name), Some(name));
    }
    for name in ["", "AWAY", "away_45", "away_0", "never"] {
        assert_eq!(Delivery::parse(name), None, "{name}");
    }
    assert_eq!(Delivery::DEFAULT, Delivery::Away(0));
    assert_eq!(Delivery::from_snapshot(&json!({"sessions":[]})), None);
    assert_eq!(
        Delivery::from_snapshot(&json!({"preferences":{"mobile_delivery":"immediate"}})),
        Some(Delivery::Immediate)
    );
    assert_eq!(
        Delivery::from_snapshot(&json!({"preferences":{"mobile_delivery":5}})),
        None
    );
}

#[test]
fn attention_and_error_candidates_drop_when_handled_read_or_replaced() {
    let read = |signature: &str, conversation: &str| json!({"conversation_id":conversation,"reply_id":null,"attention_signature":signature,"at":1.0});
    for kind in ["attention", "error"] {
        let held = candidate(kind);
        assert_eq!(
            (
                held.attention_signature.as_deref(),
                held.reply_id.as_deref()
            ),
            (Some("s1"), None)
        );
        let cases = [
            ("unchanged and unread", state(json!({})), false),
            ("run changed", state(json!({"run_id":"r2"})), true),
            (
                "conversation changed",
                state(json!({"conversation_id":"c2"})),
                true,
            ),
            (
                "replaced by a new need",
                state(json!({"attention_signature":"s2"})),
                true,
            ),
            ("resolved", state(json!({"attention_signature":null})), true),
            (
                "read on any device",
                state(json!({ "read": read("s1", "c") })),
                true,
            ),
            (
                "an older read mark",
                state(json!({ "read": read("s0", "c") })),
                false,
            ),
            (
                "a mark for another conversation",
                state(json!({ "read": read("s1", "c0") })),
                false,
            ),
        ];
        for (name, row, dropped) in cases {
            assert_eq!(held.resolved(Some(&row)), dropped, "{kind}: {name}");
        }
        assert!(held.resolved(None), "{kind}: session gone");
    }
}

#[test]
fn an_older_read_mark_never_drops_a_new_codex_question() {
    // hgs derives the signature from the same Codex question index the connector publishes.
    let row = json!({"name":"s","run_id":"r","conversation_id":"c","phase":"working","attention_signature":"s2","mobile_attention":[{"question_id":"q2","question_hash":"h2"}]});
    let held = Candidate::capture(&json!({ "sessions": [row.clone()] }), "s", "attention");
    let older = with(
        row.clone(),
        json!({"read":{"conversation_id":"c","reply_id":null,"attention_signature":"s1","at":1.0}}),
    );
    assert!(!held.resolved(Some(&older)));
    let current = with(
        row,
        json!({"read":{"conversation_id":"c","reply_id":null,"attention_signature":"s2","at":2.0}}),
    );
    assert!(held.resolved(Some(&current)));
}

#[test]
fn candidates_without_signature_resolve_only_by_native_state() {
    let unsigned = |kind: &str| {
        Candidate::capture(
            &json!({"sessions":[{"name":"s","run_id":"r","conversation_id":"c","phase":"working"}]}),
            "s",
            kind,
        )
    };
    let row = |fields: &Value| {
        with(
            json!({"name":"s","run_id":"r","conversation_id":"c","phase":"working","read":{"conversation_id":"c","reply_id":null,"attention_signature":null,"at":1.0}}),
            fields.clone(),
        )
    };
    let cases = [
        ("attention", json!({"phase":"approval"}), false),
        (
            "attention",
            json!({"mobile_attention":[{"question_id":"q"}]}),
            false,
        ),
        ("attention", json!({"pending_question_count":2}), false),
        ("attention", json!({}), true),
        (
            "attention",
            json!({"phase":"input","attention_signature":"s9","read":{"conversation_id":"c","reply_id":null,"attention_signature":"s9","at":2.0}}),
            false,
        ),
        ("error", json!({"phase":"error"}), false),
        (
            "error",
            json!({"recovery":{"state":"exhausted","request_id":"x1"}}),
            false,
        ),
        (
            "error",
            json!({"phase":"error","recovery":{"state":"waiting","request_id":"x1"}}),
            true,
        ),
        ("error", json!({"phase":"idle"}), true),
    ];
    for (kind, fields, dropped) in cases {
        assert_eq!(
            unsigned(kind).resolved(Some(&row(&fields))),
            dropped,
            "{kind}: {fields}"
        );
    }
}

#[test]
fn completed_candidates_drop_when_read_superseded_or_busy() {
    let held = candidate("completed");
    assert_eq!(
        (
            held.reply_id.as_deref(),
            held.attention_signature.as_deref()
        ),
        (Some("p1"), None)
    );
    let read = |reply: &str| json!({"conversation_id":"c","reply_id":reply,"attention_signature":null,"at":1.0});
    let cases = [
        ("an unread idle turn", state(json!({})), false),
        ("busy again", state(json!({"activity":"busy"})), true),
        ("a newer reply", state(json!({"reply_id":"p2"})), true),
        (
            "the reply was read",
            state(json!({ "read": read("p1") })),
            true,
        ),
        (
            "an older reply was read",
            state(json!({ "read": read("p0") })),
            false,
        ),
        (
            "new attention does not drop a finished turn",
            state(json!({"attention_signature":"s9"})),
            false,
        ),
    ];
    for (name, row, dropped) in cases {
        assert_eq!(held.resolved(Some(&row)), dropped, "{name}");
    }
}

#[test]
fn capture_uses_the_live_row_not_an_archive_with_the_same_name() {
    let snapshot = json!({"sessions":[
        {"name":"s","state":"archived","run_id":"old","conversation_id":"old","attention_signature":"x"},
        {"name":"s","run_id":"r","conversation_id":"c","attention_signature":"s1"}]});
    let c = Candidate::capture(&snapshot, "s", "attention");
    assert_eq!(
        (
            c.run_id.as_deref(),
            c.conversation_id.as_deref(),
            c.attention_signature.as_deref()
        ),
        (Some("r"), Some("c"), Some("s1"))
    );
    assert_eq!(
        Candidate::capture(&snapshot, "missing", "attention").run_id,
        None
    );
}

#[test]
fn every_phone_alert_condition_produces_a_relay_event() {
    let listing = |fields: Value| json!({ "sessions": [with(json!({"name":"s","run_id":"r","conversation_id":"c","phase":"working","activity":"busy"}), fields)] });
    let cases = [
        (
            "recovery gives up",
            json!({}),
            json!({"recovery":{"state":"exhausted","request_id":"x1"}}),
            vec!["error"],
        ),
        (
            "the same failed recovery",
            json!({"recovery":{"state":"exhausted","request_id":"x1"}}),
            json!({"recovery":{"state":"exhausted","request_id":"x1"}}),
            vec![],
        ),
        (
            "another failed recovery",
            json!({"recovery":{"state":"blocked","request_id":"x1"}}),
            json!({"recovery":{"state":"blocked","request_id":"x2"}}),
            vec!["error"],
        ),
        (
            "a recovery still retrying",
            json!({}),
            json!({"recovery":{"state":"retrying","request_id":"x1"}}),
            vec![],
        ),
        (
            "a counted question appears",
            json!({}),
            json!({"pending_question_count":1}),
            vec!["attention"],
        ),
        (
            "the same counted question",
            json!({"pending_question_count":1}),
            json!({"pending_question_count":1}),
            vec![],
        ),
        (
            "another counted question",
            json!({"pending_question_count":1}),
            json!({"pending_question_count":2}),
            vec!["attention"],
        ),
    ];
    for (name, before, after, expected) in cases {
        let previous = canonical(&listing(before)).unwrap();
        let kinds: Vec<String> = event_changes(Some(previous.as_str()), &listing(after))
            .unwrap()
            .into_iter()
            .map(|(_, kind)| kind)
            .collect();
        assert_eq!(kinds, expected, "{name}");
    }
}
