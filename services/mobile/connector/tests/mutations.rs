#![cfg(unix)]
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
use zerus_mobile_connector::mutations::validate_for_session;
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::runtime::{Connector, Settings};

const SESSION: &str = "codex/example/session";
const RUN: &str = "fixture-run";
const CONVERSATION: &str = "fixture-conversation";

#[derive(Default)]
struct Scripted {
    answers: RefCell<HashMap<Vec<String>, Value>>,
    calls: RefCell<Vec<Vec<String>>>,
    payloads: RefCell<Vec<Option<Value>>>,
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.argv.clone());
        self.payloads.borrow_mut().push(call.payload.clone());
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

#[tokio::test(flavor = "current_thread")]
async fn remove_queued_is_a_journaled_session_action_with_exact_identity() {
    let harness = Harness::new("mutation-remove-queued");
    let id = "f5a6b7c8-d9ea-5fb0-8c1d-2e3f40516273";
    let mut acknowledged = ack(id, json!("completed"));
    acknowledged["message_id"] = json!("msg_01");
    harness.answer(&["session-action", SESSION, "--json"], acknowledged);
    let mut payload = identity();
    payload["message_id"] = json!("msg_01");
    let result = harness.run("remove_queued", id, payload.clone()).await;
    assert_eq!(result["state"], "completed", "{result}");
    assert_eq!(result["result"]["message_id"], "msg_01");
    assert_eq!(harness.calls(), [["session-action", SESSION, "--json"]]);
    // An acknowledgement for another message is not the removal asked for.
    let other = "06b7c8d9-eafb-5c01-9d2e-3f4051627384";
    let mut wrong = ack(other, json!("completed"));
    wrong["message_id"] = json!("msg_02");
    harness.answer(&["session-action", SESSION, "--json"], wrong);
    let result = harness.run("remove_queued", other, payload).await;
    assert_eq!(result["state"], "uncertain", "{result}");
    // A malformed message identity never reaches native hgs.
    let before = harness.calls().len();
    let bad = "17c8d9ea-fb0c-5d12-8e3f-405162738495";
    let mut payload = identity();
    payload["message_id"] = json!("not-a-message");
    let result = harness.run("remove_queued", bad, payload).await;
    assert_eq!(result["state"], "failed", "{result}");
    assert_eq!(harness.calls().len(), before);
}

/// The `remove_queued` message ID shape every layer shares with hgs.
const QUEUED_MESSAGE_IDS: &str =
    include_str!("../../../../tests/fixtures/opencode-queued-message-ids.json");

#[test]
fn the_shared_fixture_decides_every_queued_message_id_shape() {
    let fixture: Value = serde_json::from_str(QUEUED_MESSAGE_IDS).unwrap();
    let accepts = |id: &Value| {
        let mut payload = identity();
        payload["request_id"] = json!("r");
        payload["message_id"] = id.clone();
        validate_for_session("remove_queued", "opencode/p/t", &payload, "r").is_ok()
    };
    for (kind, valid) in [("accept", true), ("reject", false)] {
        let ids = fixture[kind].as_array().unwrap();
        assert!(!ids.is_empty(), "{kind}");
        for id in ids {
            assert_eq!(accepts(id), valid, "{id}");
        }
    }
}

#[tokio::test(flavor = "current_thread")]
async fn an_opencode_settings_effort_must_be_offered_for_the_model() {
    let harness = Harness::new("mutation-opencode-settings");
    const OPENCODE: &str = "opencode/example/session";
    harness
        .connector
        .state
        .borrow_mut()
        .sessions
        .insert(OPENCODE.into());
    harness.answer(
        &["inspect", OPENCODE],
        json!({"name": OPENCODE, "run_id": RUN, "conversation_id": CONVERSATION,
               "settings_change_supported": true, "settings_model": "fixture/local",
               "model_options": [{"id": "fixture/local", "effort_options": ["low", "high"]}]}),
    );
    let run = |id: &'static str, effort: &'static str| {
        let harness = &harness;
        async move {
            let mut payload = identity();
            payload["effort"] = json!(effort);
            payload["request_id"] = json!(id);
            let request = json!({"request_id": id, "operation": "settings",
                                 "session": OPENCODE, "payload": payload});
            match harness.connector.execute(request).await.unwrap() {
                Outcome::Finished(receipt) => receipt,
                other => panic!("{other:?}"),
            }
        }
    };
    let refused = run("27d9eafb-0c1d-5e23-9f40-516273849506", "ultra").await;
    assert_eq!(
        refused,
        receipt("failed", "effort is not offered for this model")
    );
    assert_eq!(harness.calls().len(), 1);
    harness.answer(
        &["settings", OPENCODE, "--json"],
        json!({"request_id": "38eafb0c-1d2e-5f34-8051-627384950617", "name": OPENCODE,
               "run_id": RUN, "conversation_id": CONVERSATION, "status": "applied",
               "effort": "high"}),
    );
    let applied = run("38eafb0c-1d2e-5f34-8051-627384950617", "high").await;
    assert_eq!(applied["state"], "completed", "{applied}");
}

/// The OpenCode effort shape contract every layer shares with the native
/// catalog projection; `effort_options` membership stays authoritative.
const EFFORT_IDS: &str = include_str!("../../../../tests/fixtures/opencode-effort-ids.json");
const FIXED_EFFORTS: [&str; 11] = [
    "", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on",
];

fn effort_ids(kind: &str) -> Vec<String> {
    let fixture: Value = serde_json::from_str(EFFORT_IDS).unwrap();
    let ids: Vec<String> = fixture[kind]
        .as_array()
        .unwrap()
        .iter()
        .map(|id| id.as_str().unwrap().to_owned())
        .collect();
    assert!(!ids.is_empty(), "{kind}");
    ids
}

#[test]
fn the_shared_fixture_decides_every_settings_effort_shape() {
    let accepts = |session: &str, effort: &str| {
        let mut payload = identity();
        payload["request_id"] = json!("r");
        payload["effort"] = json!(effort);
        validate_for_session("settings", session, &payload, "r").is_ok()
    };
    for id in effort_ids("accept") {
        assert!(accepts("opencode/p/t", &id), "{id:?}");
    }
    for id in effort_ids("reject") {
        // An empty effort selects the model's default.
        assert_eq!(accepts("opencode/p/t", &id), id.is_empty(), "{id:?}");
    }
    // Every other session keeps the fixed list.
    for session in ["codex/p/t", "claude/p/t", "kimi/p/t", "dsh/p/t"] {
        for fixed in FIXED_EFFORTS {
            assert!(accepts(session, fixed), "{session} {fixed}");
        }
        for id in effort_ids("accept")
            .into_iter()
            .chain(effort_ids("reject"))
            .filter(|id| !FIXED_EFFORTS.contains(&id.as_str()))
        {
            assert!(!accepts(session, &id), "{session} {id:?}");
        }
    }
}

#[tokio::test(flavor = "current_thread")]
async fn only_an_effort_the_session_model_offers_reaches_the_settings_payload() {
    let harness = Harness::new("mutation-opencode-effort-ids");
    const OPENCODE: &str = "opencode/example/session";
    harness
        .connector
        .state
        .borrow_mut()
        .sessions
        .insert(OPENCODE.into());
    harness.answer(
        &["inspect", OPENCODE],
        json!({"name": OPENCODE, "run_id": RUN, "conversation_id": CONVERSATION,
               "settings_change_supported": true, "settings_model": "fixture/local",
               "model_options": [{"id": "fixture/local", "effort_options": ["think:deep", "caf\u{e9}"]},
                                 {"id": "fixture/other", "effort_options": ["low"]}]}),
    );
    let run = |id: &'static str, extra: Value| {
        let harness = &harness;
        async move {
            let mut payload = identity();
            for (key, value) in extra.as_object().unwrap() {
                payload[key] = value.clone();
            }
            payload["request_id"] = json!(id);
            let request = json!({"request_id": id, "operation": "settings",
                                 "session": OPENCODE, "payload": payload});
            match harness.connector.execute(request).await.unwrap() {
                Outcome::Finished(receipt) => receipt,
                other => panic!("{other:?}"),
            }
        }
    };
    // Well formed but not offered by the current or the requested model:
    // refused from the session's own inspection before any native write.
    let refusals = [
        (
            "49fb0c1d-2e3f-5045-9162-738495061728",
            json!({"effort": "low\u{2014}deep"}),
        ),
        (
            "5a0c1d2e-3f40-5156-a273-849506172839",
            json!({"effort": "low"}),
        ),
        (
            "6b1d2e3f-4051-5267-b384-95061728394a",
            json!({"model": "fixture/other", "effort": "think:deep"}),
        ),
    ];
    for (id, extra) in refusals {
        let before = harness.calls().len();
        let refused = run(id, extra.clone()).await;
        let expected = receipt("failed", "effort is not offered for this model");
        assert_eq!(refused, expected, "{extra}");
        assert_eq!(
            harness.calls()[before..],
            [["inspect", OPENCODE]],
            "{extra}"
        );
        let stored: Value =
            serde_json::from_str(&harness.connector.journal.replay(id).unwrap()).unwrap();
        assert_eq!(stored, expected, "{extra}");
    }
    // An offered colon id reaches native hgs unchanged, in the JSON payload only.
    let id = "7c2e3f40-5162-5378-8495-061728394a5b";
    harness.answer(
        &["settings", OPENCODE, "--json"],
        json!({"request_id": id, "name": OPENCODE, "run_id": RUN,
               "conversation_id": CONVERSATION, "status": "applied", "effort": "think:deep"}),
    );
    let applied = run(id, json!({"effort": "think:deep"})).await;
    assert_eq!(applied["state"], "completed", "{applied}");
    assert_eq!(
        harness.calls().last().unwrap(),
        &["settings", OPENCODE, "--json"]
    );
    let written = harness.connector.native.payloads.borrow().last().cloned();
    assert_eq!(written.unwrap().unwrap()["effort"], "think:deep");
}

#[test]
fn an_in_flight_remove_queued_recovers_as_uncertain_after_a_restart() {
    umask();
    let scratch = Scratch::new("mutation-remove-queued-restart");
    let state = scratch.path("state");
    let journal = Journal::open(&state, "fixture-binding", EPOCH).unwrap();
    let removal = "28d9eafb-0c1d-5e23-9f40-516273849507";
    let read = "39eafb0c-1d2e-5f34-8051-627384950618";
    assert!(journal
        .claim(removal, "remove_queued", None, EPOCH)
        .unwrap());
    assert!(journal.claim(read, "no_such_read", None, EPOCH).unwrap());
    journal.close();
    // The connector restarted while both were running.
    let journal = Journal::open(&state, "fixture-binding", EPOCH + 1.0).unwrap();
    let stored =
        |id: &str| -> Value { serde_json::from_str(&journal.replay(id).unwrap()).unwrap() };
    assert_eq!(stored(removal), receipt("uncertain", RESTART_ERROR));
    assert_eq!(stored(read), receipt("failed", RESTART_ERROR));
    assert!(zerus_mobile_connector::journal::is_mutation(
        "remove_queued"
    ));
}
