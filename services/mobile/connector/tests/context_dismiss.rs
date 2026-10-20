#![cfg(unix)]
//! Optional prompt dismissal preserves ordinary clear bytes and durable no-replay identities.
mod common;

use common::{Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::RefCell;
use zerus_mobile_connector::{
    capabilities::Flags,
    execute::Outcome,
    journal::Journal,
    mutations::validate_for_session,
    native::{self, Call, Native, NativeError, NativeResult, Output},
    runtime::{Connector, Settings},
};

const SESSION: &str = "codex/fixture/context";
const RUN: &str = "fixture-run";
const CONVERSATION: &str = "fixture-conversation";
const MARKER: &str = "Mobile clear-context ABI: dismiss-prompt-v1";
const FEATURE: &str = "clear_context_dismiss";
const UNAVAILABLE: &str = "native clear-context dismiss ABI is unavailable";
const CONTEXT_UNAVAILABLE: &str = "native context operation is unavailable";
const TYPE_ERROR: &str = "dismiss_prompt must be a boolean";

fn id(n: usize) -> String {
    format!("00000000-0000-4000-8000-{n:012}")
}
fn command(n: usize, operation: &str, dismiss: Option<Value>) -> Value {
    let mut payload =
        json!({"request_id":id(n),"expected_run_id":RUN,"expected_conversation_id":CONVERSATION});
    if let Some(value) = dismiss {
        payload["dismiss_prompt"] = value;
    }
    json!({"request_id":id(n),"operation":operation,"session":SESSION,"payload":payload})
}
fn detail() -> Value {
    json!({"name":SESSION,"run_id":RUN,"conversation_id":CONVERSATION,"archive_id":"",
        "phase":"waiting","activity":"attention","clear_context_supported":false,"clear_context_dismiss":true})
}
struct Fake {
    calls: RefCell<Vec<Call>>,
    detail: RefCell<Result<Value, NativeError>>,
    write: RefCell<Option<Result<Value, NativeError>>>,
}
impl Native for Fake {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.clone());
        if call.argv == ["inspect", SESSION] {
            return self.detail.borrow().clone().map(Output::Json);
        }
        if let Some(result) = self.write.borrow().clone() {
            return result.map(Output::Json);
        }
        assert!(
            call.argv == ["clear-context", SESSION, "--json"]
                || call.argv == ["compact-context", SESSION, "--json"]
        );
        let payload = call.payload.expect("context payload");
        Ok(Output::Json(
            json!({"request_id":payload["request_id"],"name":SESSION,"run_id":RUN,
            "conversation_id":CONVERSATION,"status":"submitted"}),
        ))
    }
}
struct Harness {
    node: Connector<Fake>,
    _scratch: Scratch,
}
impl Harness {
    fn new(label: &str, help: &str) -> Self {
        let scratch = Scratch::new(label);
        let node = Connector::new(
            Fake {
                calls: RefCell::new(Vec::new()),
                detail: RefCell::new(Ok(detail())),
                write: RefCell::new(None),
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
        node.flags.set(Flags::from_help(help));
        node.state.borrow_mut().sessions.insert(SESSION.into());
        node.state.borrow_mut().snapshot_seen_at = Some(node.clock());
        Self {
            node,
            _scratch: scratch,
        }
    }
    async fn execute(&self, request: Value) -> Value {
        match self.node.execute(request).await.unwrap() {
            Outcome::Finished(receipt) => receipt,
            other => panic!("{other:?}"),
        }
    }
    fn calls(&self) -> Vec<Call> {
        self.node.native.calls.borrow().clone()
    }
}
fn failed(error: &str) -> Value {
    json!({"state":"failed","result":null,"error":error})
}

#[tokio::test(flavor = "current_thread")]
async fn omitted_clear_and_compact_keep_the_original_payload_and_native_calls() {
    for (n, operation, command_name) in [
        (1, "clear_context", "clear-context"),
        (2, "compact_context", "compact-context"),
    ] {
        let h = Harness::new("context-dismiss-old-controls", "");
        let mut ordinary = detail();
        ordinary
            .as_object_mut()
            .unwrap()
            .remove("clear_context_dismiss");
        ordinary[format!("{operation}_supported")] = json!(true);
        ordinary["phase"] = json!("idle");
        ordinary["activity"] = json!("idle");
        *h.node.native.detail.borrow_mut() = Ok(ordinary);
        let request = command(n, operation, None);
        let result = h.execute(request.clone()).await;
        assert_eq!(result["state"], "completed", "{result}");
        assert_eq!(
            request["payload"],
            json!({"request_id":id(n),"expected_run_id":RUN,"expected_conversation_id":CONVERSATION})
        );
        assert_eq!(
            h.calls(),
            vec![
                Call::new(["inspect", SESSION]),
                Call::new([command_name, SESSION, "--json"]).payload(request["payload"].clone())
            ]
        );
    }
}

#[test]
fn only_the_exact_marker_adds_the_feature_and_never_changes_operation_sets() {
    let ordinary = Flags::default().advertised(false);
    for (help, expected) in [
        ("", false),
        ("clear-context <session> --json", false),
        ("Mobile clear-context ABI: dismiss-prompt-v10", false),
        (
            "Mobile clear-context ABI: dismiss-prompt-v1 unavailable",
            false,
        ),
        ("prefix Mobile clear-context ABI: dismiss-prompt-v1", false),
        ("  Mobile clear-context ABI: dismiss-prompt-v1  \n", true),
        (MARKER, true),
    ] {
        let flags = Flags::from_help(help);
        let offered = flags.advertised(false);
        assert_eq!(
            offered["features"]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| v == FEATURE),
            expected,
            "{help}"
        );
        assert_eq!(offered["operations"], ordinary["operations"]);
        assert_eq!(flags.python_features(false).contains(FEATURE), expected);
    }
}

#[test]
fn only_clear_accepts_an_optional_actual_boolean_and_original_identity_rules_stay_strict() {
    for value in [None, Some(json!(true)), Some(json!(false))] {
        let request = command(1, "clear_context", value);
        assert!(
            validate_for_session("clear_context", SESSION, &request["payload"], &id(1)).is_ok()
        );
    }
    assert!(validate_for_session(
        "compact_context",
        SESSION,
        &command(1, "compact_context", None)["payload"],
        &id(1)
    )
    .is_ok());
    for value in [json!(true), json!(false)] {
        assert!(validate_for_session(
            "compact_context",
            SESSION,
            &command(1, "compact_context", Some(value))["payload"],
            &id(1)
        )
        .is_err());
    }
    for value in [
        Value::Null,
        json!(0),
        json!(1),
        json!(0.0),
        json!("true"),
        json!([]),
        json!({}),
    ] {
        let request = command(1, "clear_context", Some(value));
        assert_eq!(
            validate_for_session("clear_context", SESSION, &request["payload"], &id(1))
                .unwrap_err(),
            TYPE_ERROR
        );
    }
    for (key, value) in [
        ("request_id", json!(id(2))),
        ("expected_run_id", json!("")),
        ("expected_conversation_id", json!(false)),
        ("archive_id", json!("archive")),
        ("agent_id", json!("child")),
        ("extra", json!(true)),
    ] {
        let mut request = command(1, "clear_context", Some(json!(true)));
        request["payload"][key] = value;
        assert!(
            validate_for_session("clear_context", SESSION, &request["payload"], &id(1)).is_err(),
            "{key}"
        );
    }
}

#[tokio::test(flavor = "current_thread")]
async fn true_uses_fresh_exact_native_eligibility_and_forwards_original_payload() {
    let h = Harness::new("context-dismiss-true", MARKER);
    let request = command(1, "clear_context", Some(json!(true)));
    let result = h.execute(request.clone()).await;
    assert_eq!(result["state"], "completed", "{result}");
    let calls = h.calls();
    assert_eq!(calls.len(), 2);
    assert_eq!(calls[0], Call::new(["inspect", SESSION]));
    assert_eq!(
        calls[1],
        Call::new(["clear-context", SESSION, "--json"]).payload(request["payload"].clone())
    );
    h.node.flags.set(Flags::default());
    assert_eq!(h.node.execute(request).await.unwrap(), Outcome::Replayed);
    assert_eq!(h.calls().len(), 2);
    assert_eq!(
        h.execute(command(2, "clear_context", Some(json!(true))))
            .await,
        failed(UNAVAILABLE)
    );
    assert_eq!(h.calls().len(), 2);
}

#[tokio::test(flavor = "current_thread")]
async fn false_completion_replays_after_marker_withdrawal_but_new_false_identity_is_gated() {
    let h = Harness::new("context-dismiss-false-replay", MARKER);
    let mut ordinary = detail();
    ordinary["clear_context_supported"] = json!(true);
    *h.node.native.detail.borrow_mut() = Ok(ordinary);
    let request = command(1, "clear_context", Some(json!(false)));
    let receipt = h.execute(request.clone()).await;
    assert_eq!(receipt["state"], "completed");
    h.node.flags.set(Flags::default());
    assert_eq!(h.node.execute(request).await.unwrap(), Outcome::Replayed);
    let stored: Value = serde_json::from_str(&h.node.journal.replay(&id(1)).unwrap()).unwrap();
    assert_eq!(stored, receipt);
    assert_eq!(
        h.execute(command(2, "clear_context", Some(json!(false))))
            .await,
        failed(UNAVAILABLE)
    );
    assert_eq!(h.calls().len(), 2);
}

#[tokio::test(flavor = "current_thread")]
async fn a_new_dismiss_identity_rechecks_current_native_eligibility_without_reusing_an_old_inspection(
) {
    let h = Harness::new("context-dismiss-fresh", MARKER);
    assert_eq!(
        h.execute(command(1, "clear_context", Some(json!(true))))
            .await["state"],
        "completed"
    );
    let mut changed = detail();
    changed["clear_context_dismiss"] = json!(false);
    *h.node.native.detail.borrow_mut() = Ok(changed);
    assert_eq!(
        h.execute(command(2, "clear_context", Some(json!(true))))
            .await,
        failed(CONTEXT_UNAVAILABLE)
    );
    assert_eq!(
        h.calls()
            .iter()
            .map(|call| call.argv.clone())
            .collect::<Vec<_>>(),
        vec![
            vec!["inspect", SESSION],
            vec!["clear-context", SESSION, "--json"],
            vec!["inspect", SESSION]
        ]
    );
}

#[tokio::test(flavor = "current_thread")]
async fn presence_gate_rejects_both_boolean_values_before_any_inspection_and_replays_that_failure()
{
    for (n, value) in [(1, true), (2, false)] {
        let h = Harness::new("context-dismiss-presence", "");
        let request = command(n, "clear_context", Some(json!(value)));
        assert_eq!(h.execute(request.clone()).await, failed(UNAVAILABLE));
        assert!(h.calls().is_empty());
        h.node.flags.set(Flags::from_help(MARKER));
        assert_eq!(h.node.execute(request).await.unwrap(), Outcome::Replayed);
        assert!(h.calls().is_empty());
        let stored: Value = serde_json::from_str(&h.node.journal.replay(&id(n)).unwrap()).unwrap();
        assert_eq!(stored, failed(UNAVAILABLE));
    }
}

#[tokio::test(flavor = "current_thread")]
async fn absent_and_false_keep_the_original_supported_gate_and_exact_call_bytes() {
    for (n, value, help) in [(1, None, ""), (2, Some(json!(false)), MARKER)] {
        let h = Harness::new("context-dismiss-ordinary", help);
        let mut ordinary = detail();
        ordinary["clear_context_supported"] = json!(true);
        ordinary
            .as_object_mut()
            .unwrap()
            .remove("clear_context_dismiss");
        *h.node.native.detail.borrow_mut() = Ok(ordinary);
        let request = command(n, "clear_context", value);
        let result = h.execute(request.clone()).await;
        assert_eq!(result["state"], "completed", "{result}");
        assert_eq!(
            h.calls(),
            vec![
                Call::new(["inspect", SESSION]),
                Call::new(["clear-context", SESSION, "--json"]).payload(request["payload"].clone())
            ]
        );
        if n == 1 {
            assert_eq!(request["payload"].as_object().unwrap().len(), 3);
        }
    }
    for value in [None, Some(json!(false))] {
        let h = Harness::new("context-dismiss-no-implicit-bypass", MARKER);
        assert_eq!(
            h.execute(command(3, "clear_context", value)).await,
            failed(CONTEXT_UNAVAILABLE)
        );
        assert_eq!(h.calls(), vec![Call::new(["inspect", SESSION])]);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn true_requires_actual_boolean_eligibility_even_when_ordinary_clear_is_supported_or_idle() {
    for value in [
        None,
        Some(Value::Null),
        Some(json!(false)),
        Some(json!(1)),
        Some(json!("true")),
        Some(json!([])),
    ] {
        let h = Harness::new("context-dismiss-no-inference", MARKER);
        let mut inspected = detail();
        inspected["clear_context_supported"] = json!(true);
        inspected["phase"] = json!("idle");
        inspected["activity"] = json!("idle");
        inspected["source"] = json!("claude_terminal_choice");
        inspected
            .as_object_mut()
            .unwrap()
            .remove("clear_context_dismiss");
        if let Some(value) = value {
            inspected["clear_context_dismiss"] = value;
        }
        *h.node.native.detail.borrow_mut() = Ok(inspected);
        assert_eq!(
            h.execute(command(1, "clear_context", Some(json!(true))))
                .await,
            failed(CONTEXT_UNAVAILABLE)
        );
        assert_eq!(h.calls(), vec![Call::new(["inspect", SESSION])]);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn exact_identity_and_archive_guards_precede_dismiss_eligibility() {
    for (key, value) in [
        ("name", json!("codex/other/session")),
        ("run_id", json!("other-run")),
        ("conversation_id", json!("other-conversation")),
        ("archive_id", json!("fixture-archive")),
        ("state", json!("archived")),
    ] {
        let h = Harness::new("context-dismiss-target", MARKER);
        let mut inspected = detail();
        inspected[key] = value;
        inspected["clear_context_dismiss"] = json!(false);
        *h.node.native.detail.borrow_mut() = Ok(inspected);
        let result = h
            .execute(command(1, "clear_context", Some(json!(true))))
            .await;
        assert_eq!(
            result,
            failed("fresh native context identity did not match"),
            "{key}"
        );
        assert_eq!(h.calls(), vec![Call::new(["inspect", SESSION])]);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn malformed_fields_and_compact_dismiss_never_inspect_or_write() {
    let h = Harness::new("context-dismiss-validation", MARKER);
    for (n, value) in [Value::Null, json!(0), json!("false"), json!([])]
        .into_iter()
        .enumerate()
    {
        assert_eq!(
            h.execute(command(n + 1, "clear_context", Some(value)))
                .await,
            failed(TYPE_ERROR)
        );
    }
    for (n, value) in [true, false].into_iter().enumerate() {
        assert_eq!(
            h.execute(command(n + 10, "compact_context", Some(json!(value))))
                .await,
            failed("context control requires exact request/run/conversation identity")
        );
    }
    assert!(h.calls().is_empty());
}

#[tokio::test(flavor = "current_thread")]
async fn preflight_timeout_is_failed_and_post_write_timeout_uncertain_with_no_replay() {
    for preflight in [true, false] {
        let h = Harness::new("context-dismiss-timeout", MARKER);
        let timeout = Err(NativeError::Failed(native::TIMED_OUT));
        if preflight {
            *h.node.native.detail.borrow_mut() = timeout;
        } else {
            *h.node.native.write.borrow_mut() = Some(timeout);
        }
        let request = command(1, "clear_context", Some(json!(true)));
        let result = h.execute(request.clone()).await;
        assert_eq!(
            result,
            json!({"state":if preflight {"failed"}else{"uncertain"},"result":null,"error":native::TIMED_OUT})
        );
        let before = h.calls().len();
        assert_eq!(before, if preflight { 1 } else { 2 });
        assert_eq!(h.node.execute(request).await.unwrap(), Outcome::Replayed);
        assert_eq!(h.calls().len(), before);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn wrong_native_ack_is_sanitized_uncertain_and_durable_hash_conflict_never_reexecutes() {
    let h = Harness::new("context-dismiss-ack", MARKER);
    *h.node.native.write.borrow_mut() =
        Some(Ok(json!({"request_id":id(99),"name":SESSION,"run_id":RUN,
        "conversation_id":CONVERSATION,"status":"submitted","secret":"PRIVATE-SYNTHETIC"})));
    let request = command(1, "clear_context", Some(json!(true)));
    let result = h.execute(request.clone()).await;
    assert_eq!(result["state"], "uncertain");
    assert!(result["result"].is_null());
    assert!(!result.to_string().contains("PRIVATE-SYNTHETIC"));
    assert_eq!(
        h.node.execute(request.clone()).await.unwrap(),
        Outcome::Replayed
    );
    let mut changed = request;
    changed["payload"]["dismiss_prompt"] = json!(false);
    assert!(matches!(
        h.node.execute(changed).await.unwrap(),
        Outcome::Rejected(_)
    ));
    assert_eq!(h.calls().len(), 2);
}
