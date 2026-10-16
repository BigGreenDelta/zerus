//! Session mutations where no golden vector or black-box test reaches:
//! Python's KeyError and TypeError paths, which become the generic receipt,
//! the stop rule of divergence h for a write that never started, and the
//! sessions an authoritative action target makes inspectable. Native calls
//! are scripted by argv.
mod common;

use common::*;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::HashMap;
use zerus_mobile_connector::capabilities::Flags;
use zerus_mobile_connector::execute::{Outcome, GENERIC};
use zerus_mobile_connector::journal::{Journal, RESTART_ERROR};
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::runtime::{Connector, Settings};

const SESSION: &str = "codex/example/session";
const RUN: &str = "fixture-run";
const CONVERSATION: &str = "fixture-conversation";

#[derive(Default)]
struct Scripted {
    answers: RefCell<HashMap<Vec<String>, Value>>,
    calls: RefCell<Vec<Vec<String>>>,
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.argv.clone());
        match self.answers.borrow().get(&call.argv) {
            Some(answer) => Ok(Output::Json(answer.clone())),
            None => Err(NativeError::Failed(native::FAILED)),
        }
    }
}

struct Harness {
    connector: Connector<Scripted>,
    _scratch: Scratch,
}

impl Harness {
    fn new(label: &str) -> Self {
        umask();
        let scratch = Scratch::new(label);
        let journal = Journal::open(&scratch.path("state"), "fixture-binding", EPOCH).unwrap();
        let settings = Settings {
            interval: 5.0,
            binding: "fixture-binding".into(),
            default_timeout: native::DEFAULT_TIMEOUT,
            clock_scale: 1.0,
        };
        let connector = Connector::new(Scripted::default(), journal, settings, Box::new(|| EPOCH));
        let help = document("operations.json")["capabilities"]["help_text_all"]
            .as_str()
            .unwrap()
            .to_owned();
        connector.flags.set(Flags::from_help(&help));
        {
            let mut state = connector.state.borrow_mut();
            state.sessions.insert(SESSION.into());
            state.snapshot_seen_at = Some(connector.clock());
        }
        Harness {
            connector,
            _scratch: scratch,
        }
    }

    fn answer(&self, argv: &[&str], answer: Value) {
        let argv = argv.iter().map(|arg| arg.to_string()).collect();
        self.connector
            .native
            .answers
            .borrow_mut()
            .insert(argv, answer);
    }

    fn calls(&self) -> Vec<Vec<String>> {
        self.connector.native.calls.borrow().clone()
    }

    async fn run(&self, operation: &str, request_id: &str, payload: Value) -> Value {
        let mut payload = payload;
        payload["request_id"] = json!(request_id);
        let request = json!({"request_id": request_id, "operation": operation,
                             "session": SESSION, "payload": payload});
        match self.connector.execute(request).await.unwrap() {
            Outcome::Finished(receipt) => receipt,
            other => panic!("{other:?}"),
        }
    }
}

fn identity() -> Value {
    json!({"expected_run_id": RUN, "expected_conversation_id": CONVERSATION})
}

fn ack(request_id: &str, status: Value) -> Value {
    json!({"request_id": request_id, "name": SESSION, "run_id": RUN,
           "conversation_id": CONVERSATION, "status": status})
}

fn receipt(state: &str, error: &str) -> Value {
    json!({"state": state, "result": null, "error": error})
}

#[tokio::test(flavor = "current_thread")]
async fn no_write_starts_once_the_connector_stops() {
    let harness = Harness::new("mutation-stopping");
    harness.answer(
        &["send", SESSION, "--json"],
        ack("unused", json!("submitted")),
    );
    harness.connector.stopping.set();
    let mut payload = identity();
    payload["text"] = json!("never written");
    let id = "5b0a8c1e-4f3d-5e6a-9b7c-1d2e3f405162";
    // Nothing reached native hgs, so the receipt is failed, not uncertain.
    assert_eq!(
        harness.run("send", id, payload).await,
        receipt("failed", RESTART_ERROR)
    );
    assert!(harness.calls().is_empty());
}

#[tokio::test(flavor = "current_thread")]
async fn an_unhashable_native_status_is_the_generic_uncertain_receipt() {
    let harness = Harness::new("mutation-unhashable");
    let pause = "6c1b9d2f-5a4e-5f7b-8c8d-2e3f40516273";
    harness.answer(
        &["session-action", SESSION, "--json"],
        ack(pause, json!(["completed"])),
    );
    assert_eq!(
        harness.run("pause", pause, identity()).await,
        receipt("uncertain", GENERIC)
    );
    let clear = "7d2c0e3a-6b5f-5a8c-9d9e-3f4051627384";
    harness.answer(
        &["clear-context", SESSION, "--json"],
        ack(clear, json!({"status": "confirmed"})),
    );
    harness.answer(
        &["inspect", SESSION],
        json!({"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
               "clear_context_supported": true}),
    );
    assert_eq!(
        harness.run("clear_context", clear, identity()).await,
        receipt("uncertain", GENERIC)
    );
    // Another identity is refused before the status is ever compared.
    let other = "8e3d1f4b-7c6a-5b9d-8eaf-405162738495";
    let wrong = ack("ffffffff-ffff-5fff-bfff-ffffffffffff", json!([]));
    harness.answer(&["session-action", SESSION, "--json"], wrong);
    assert_eq!(
        harness.run("resume", other, identity()).await,
        receipt(
            "uncertain",
            "native lifecycle acknowledgement identity or status did not match"
        )
    );
}

#[tokio::test(flavor = "current_thread")]
async fn a_continuation_without_its_conversation_is_the_generic_failure() {
    let harness = Harness::new("mutation-keyerror");
    let compaction = "9f4e205c-8d7b-5cae-9fb0-5162738495a6";
    harness.answer(
        &["inspect", SESSION],
        json!({"name": SESSION, "run_id": RUN, "phase": "idle", "activity": "idle",
               "compact_context_request": {"request_id": compaction, "run_id": RUN,
                                           "conversation_id": "", "status": "completed"}}),
    );
    harness.answer(&["send", SESSION, "--json"], json!({"status": "submitted"}));
    // `payload["expected_conversation_id"]` raises KeyError in Python.
    let missing = "a0513f6d-9e8c-5dbf-a0c1-62738495a6b7";
    let payload = json!({"expected_run_id": RUN, "text": "kept draft",
                         "expected_compaction_id": compaction});
    assert_eq!(
        harness.run("send", missing, payload).await,
        receipt("failed", GENERIC)
    );
    assert_eq!(harness.calls(), [["inspect", SESSION]]);
    // A listing without a conversation matches the empty one.
    let empty = "b162407e-af9d-5ec0-b1d2-738495a6b7c8";
    let payload = json!({"expected_run_id": RUN, "expected_conversation_id": "",
                         "text": "kept draft", "expected_compaction_id": compaction});
    let sent = harness.run("send", empty, payload).await;
    assert_eq!(sent["state"], "completed", "{sent}");
    assert_eq!(
        harness.calls().last().unwrap(),
        &["send", SESSION, "--json"]
    );
}

#[tokio::test(flavor = "current_thread")]
async fn only_a_live_action_target_becomes_inspectable_at_once() {
    let harness = Harness::new("mutation-targets");
    let sessions = || harness.connector.state.borrow().sessions.clone();
    let rename = "c273518f-b0ae-5fd1-82e3-8495a6b7c8d9";
    let mut renamed = ack(rename, json!("completed"));
    renamed["result_target"] =
        json!({"name": "codex/example/renamed", "run_id": RUN, "conversation_id": CONVERSATION});
    harness.answer(&["session-action", SESSION, "--json"], renamed);
    let mut payload = identity();
    payload["new_name"] = json!("codex/example/renamed");
    let result = harness.run("rename", rename, payload).await;
    assert_eq!(result["state"], "completed", "{result}");
    assert_eq!(sessions(), ["codex/example/renamed".to_owned()].into());
    // A failed action and an archived target leave the sessions as they are.
    harness.connector.state.borrow_mut().sessions = [SESSION.to_owned()].into();
    for (status, archive) in [
        ("failed", ""),
        ("completed", "81b191c0-bdae-543a-ba1c-0fbcbb2f638d"),
    ] {
        let fork = if status == "failed" {
            "d38462a0-c1bf-5ae2-93f4-95a6b7c8d9ea"
        } else {
            "e49573b1-d2c0-5bf3-a405-a6b7c8d9eafb"
        };
        let mut forked = ack(fork, json!(status));
        forked["result_target"] = json!({"name": "codex/example/fork", "run_id": "fork-run",
                                         "conversation_id": "", "archive_id": archive});
        harness.answer(&["session-action", SESSION, "--json"], forked);
        let result = harness.run("fork", fork, identity()).await;
        assert_eq!(result["state"], status, "{result}");
        assert_eq!(sessions(), [SESSION.to_owned()].into(), "{status}");
    }
}
