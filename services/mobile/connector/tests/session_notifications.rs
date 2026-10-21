#![cfg(unix)]
//! Exact shared-Mute mutation and durable receipts, with no real native process.
mod common;
use common::{Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::RefCell;
use zerus_mobile_connector::{
    capabilities::Flags,
    execute::Outcome,
    journal::{is_mutation, Journal},
    native::{Call, Native, NativeResult, Output},
    runtime::{Connector, Settings},
    session_notifications::{self, HELP_MARKER},
};

const NAME: &str = "codex/example/notifications";
fn payload() -> Value {
    json!({"run_id":"fixture-run","conversation_id":"fixture-conversation","muted":true,"expected_revision":0})
}
fn request(n: usize) -> Value {
    json!({"request_id":format!("00000000-0000-4000-8000-{n:012}"),"operation":"session_notifications","session":NAME,"payload":payload()})
}
fn receipt() -> Value {
    json!({"ok":true,"applied":true,"run_id":"fixture-run","conversation_id":"fixture-conversation","preferences":{"conversation_id":"fixture-conversation","muted":true,"revision":1,"suppressed":{"reply_id":null,"attention_signature":null,"optional_question_ids":[],"main_attention":null,"child_attention":{}}}})
}
struct Fake {
    calls: RefCell<Vec<Call>>,
    result: Value,
}
impl Native for Fake {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call);
        Ok(Output::Json(self.result.clone()))
    }
}
fn connector(scratch: &Scratch, result: Value) -> Connector<Fake> {
    let node = Connector::new(
        Fake {
            calls: RefCell::new(vec![]),
            result,
        },
        Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap(),
        Settings {
            interval: 5.0,
            binding: "fixture".into(),
            default_timeout: 45.0,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    );
    node.flags.set(Flags::from_help(HELP_MARKER));
    node.state.borrow_mut().sessions.insert(NAME.into());
    node.state.borrow_mut().snapshot_seen_at = Some(node.clock());
    node
}

#[test]
fn exact_marker_and_mutation_class_preserve_read_contracts() {
    assert!(is_mutation("session_notifications"));
    for (help, expected) in [
        ("", false),
        ("notifications <session> --json", false),
        ("session-notifications-v10", false),
        ("session-notifications-v1 suffix", false),
        (HELP_MARKER, true),
    ] {
        let flags = Flags::from_help(help);
        assert_eq!(
            flags
                .python_operations(true)
                .contains("session_notifications"),
            expected
        );
        assert!(!flags.read);
    }
}

#[test]
fn strict_payload_and_receipt_types_bind_revision_and_exact_target() {
    session_notifications::validate(&payload()).unwrap();
    for (key, value) in [
        ("muted", json!(1)),
        ("muted", json!("true")),
        ("expected_revision", json!(true)),
        ("expected_revision", json!(-1)),
        ("expected_revision", json!(0.0)),
        ("expected_revision", json!(u64::MAX)),
        ("run_id", json!("")),
        ("run_id", json!("é".repeat(65))),
        ("conversation_id", json!("é".repeat(129))),
        ("run_id", json!("line\nbreak")),
        ("conversation_id", json!("x".repeat(257))),
    ] {
        let mut bad = payload();
        bad[key] = value;
        assert!(session_notifications::validate(&bad).is_err(), "{bad}");
    }
    let mut extra = payload();
    extra["reply_id"] = json!("old");
    assert!(session_notifications::validate(&extra).is_err());
    assert!(session_notifications::result_valid(&receipt(), &payload()));
    for (key, value) in [
        ("muted", json!(false)),
        ("muted", json!(1)),
        ("revision", json!(true)),
        ("revision", json!(2)),
        ("conversation_id", json!("other")),
        ("suppressed", Value::Null),
    ] {
        let mut bad = receipt();
        bad["preferences"][key] = value;
        assert!(
            !session_notifications::result_valid(&bad, &payload()),
            "{bad}"
        );
    }
    let mut noop = receipt();
    noop["applied"] = json!(false);
    noop["preferences"]["revision"] = json!(0);
    assert!(session_notifications::result_valid(&noop, &payload()));
    for applied in [json!(true), json!(false), Value::Null] {
        assert!(!session_notifications::result_valid(
            &json!({"ok":false,"error":"conflict","applied":applied}),
            &payload()
        ));
    }
}

#[tokio::test(flavor = "current_thread")]
async fn native_receipt_uses_exact_payload_and_never_repeats_a_durable_mutation() {
    let scratch = Scratch::new("shared-notifications-native");
    let node = connector(&scratch, receipt());
    let Outcome::Finished(first) = node.execute(request(1)).await.unwrap() else {
        panic!()
    };
    assert_eq!(first["state"], "completed");
    assert_eq!(first["result"], receipt());
    assert_eq!(node.execute(request(1)).await.unwrap(), Outcome::Replayed);
    assert_eq!(
        node.native.calls.borrow().as_slice(),
        &[Call::new(["notifications", NAME, "--json"])
            .payload(payload())
            .failure_json()]
    );
}

#[tokio::test(flavor = "current_thread")]
async fn native_conflict_is_retained_and_never_retried() {
    let scratch = Scratch::new("shared-notifications-conflict");
    let native = json!({"ok":false,"error":"conflict","run_id":"fixture-run","conversation_id":"fixture-conversation","preferences":receipt()["preferences"]});
    let node = connector(&scratch, native.clone());
    let Outcome::Finished(first) = node.execute(request(1)).await.unwrap() else {
        panic!()
    };
    assert_eq!(first["result"], native);
    assert_eq!(node.execute(request(1)).await.unwrap(), Outcome::Replayed);
    assert_eq!(node.native.calls.borrow().len(), 1);
}

#[tokio::test(flavor = "current_thread")]
async fn unsupported_or_unadmitted_target_never_reaches_native() {
    let scratch = Scratch::new("shared-notifications-refused");
    let node = connector(&scratch, receipt());
    node.flags.set(Flags::default());
    let Outcome::Finished(first) = node.execute(request(1)).await.unwrap() else {
        panic!()
    };
    assert_eq!(first["state"], "failed");
    node.flags.set(Flags::from_help(HELP_MARKER));
    let mut bad = request(2);
    bad["session"] = json!("codex/other");
    let Outcome::Finished(second) = node.execute(bad).await.unwrap() else {
        panic!()
    };
    assert_eq!(second["state"], "failed");
    assert!(node.native.calls.borrow().is_empty());
}
