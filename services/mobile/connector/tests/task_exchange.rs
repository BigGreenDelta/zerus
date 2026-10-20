//! The phone's task exchange, which only this connector offers: advertised
//! when hgs names `task exchange|digests --json`, run with its payload on
//! stdin, kept as a short read, and each project's tasks digest in the
//! heartbeat (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
mod common;

use common::*;
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use zerus_mobile_connector::capabilities::Flags;
use zerus_mobile_connector::execute::{Outcome, TOO_LARGE};
use zerus_mobile_connector::journal::{Journal, JOURNAL, TASK_READ_PRUNE};
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::runtime::{Connector, Settings};

const MARKER: &str =
    "       hgs task exchange|digests --json                 the phone's task exchange (mobile)\n";
const ACTIONS: &str =
    "       hgs task mobile-send|mobile-cancel|mobile-done --json   the phone's task actions (mobile)\n";
const SEND: [&str; 3] = ["task", "mobile-send", "--json"];
const CANCEL: [&str; 3] = ["task", "mobile-cancel", "--json"];
const DONE: [&str; 3] = ["task", "mobile-done", "--json"];
const TASK: &str = "0b9d6c55-2f5e-4c39-9a51-0d6f3c2a7e11";
const EXCHANGE: [&str; 3] = ["task", "exchange", "--json"];
const DIGESTS: [&str; 3] = ["task", "digests", "--json"];

#[derive(Default)]
struct Scripted {
    answers: RefCell<HashMap<Vec<String>, Value>>,
    calls: RefCell<Vec<Vec<String>>>,
    payloads: RefCell<Vec<Option<Value>>>,
    failure_json: RefCell<Vec<bool>>,
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.argv.clone());
        self.failure_json.borrow_mut().push(call.failure_json);
        self.payloads.borrow_mut().push(call.payload.clone());
        match self.answers.borrow().get(&call.argv) {
            Some(answer) => Ok(Output::Json(answer.clone())),
            None => Err(NativeError::Failed(native::FAILED)),
        }
    }
}

fn help() -> String {
    document("operations.json")["capabilities"]["help_text_all"]
        .as_str()
        .unwrap()
        .to_owned()
}

struct Harness {
    connector: Rc<Connector<Scripted>>,
    scratch: Scratch,
}

impl Harness {
    fn new(label: &str, marker: bool) -> Self {
        Self::with_help(label, if marker { help() + MARKER } else { help() })
    }

    fn with_help(label: &str, help: String) -> Self {
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
        connector.flags.set(Flags::from_help(&help));
        connector.state.borrow_mut().snapshot_seen_at = Some(connector.clock());
        Harness {
            connector: Rc::new(connector),
            scratch,
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

    fn calls(&self, argv: &[&str]) -> Vec<Option<Value>> {
        let native = &self.connector.native;
        native
            .calls
            .borrow()
            .iter()
            .zip(native.payloads.borrow().iter())
            .filter(|(call, _)| call.as_slice() == argv)
            .map(|(_, payload)| payload.clone())
            .collect()
    }

    async fn exchange(&self, request_id: &str, session: &str, payload: Value) -> Value {
        match self
            .execute("task_exchange", request_id, session, payload)
            .await
        {
            Outcome::Finished(receipt) => receipt,
            other => panic!("{other:?}"),
        }
    }

    async fn execute(
        &self,
        operation: &str,
        request_id: &str,
        session: &str,
        payload: Value,
    ) -> Outcome {
        let request = json!({"request_id": request_id, "operation": operation,
                             "session": session, "payload": payload});
        self.connector.execute(request).await.unwrap()
    }

    async fn finished(&self, operation: &str, request_id: &str, payload: Value) -> Value {
        match self.execute(operation, request_id, "", payload).await {
            Outcome::Finished(receipt) => receipt,
            other => panic!("{other:?}"),
        }
    }

    fn journal(&self) -> Vec<Value> {
        query(
            &read_only(&self.scratch.path("state").join(JOURNAL)),
            "SELECT request_id,state FROM requests ORDER BY request_id",
        )
    }
}

fn request() -> Value {
    json!({"schema": 1, "project": "project-1",
           "inventory": {"project": {"1234": 3}, "tasks": {TASK: {"1234": 2}}},
           "updates": [{"task": TASK, "bytes": "bG9ybw=="}]})
}

fn answer() -> Value {
    json!({"schema": 1, "imported": 1, "refused": [], "updates": [{"task": null, "bytes": "bG9ybw=="}],
           "more": false, "digest": "cd".repeat(32)})
}

fn failed(error: &str) -> Value {
    json!({"state": "failed", "result": null, "error": error})
}

#[test]
fn task_exchange_is_advertised_only_when_hgs_names_it() {
    let with = Flags::from_help(&(help() + MARKER));
    let without = Flags::from_help(&help());
    assert!(with.task_exchange && !without.task_exchange);
    for (flags, fleet) in [(&with, true), (&with, false)] {
        let operations = flags.advertised_for(fleet, true)["operations"].clone();
        assert!(operations
            .as_array()
            .unwrap()
            .contains(&json!("task_exchange")));
    }
    let operations = without.advertised(true)["operations"].clone();
    assert!(!operations
        .as_array()
        .unwrap()
        .contains(&json!("task_exchange")));
    // Python never offers it, whatever its help says.
    assert!(!with.python_operations(true).contains("task_exchange"));
    // The sets stay sorted for the relay's capability shape.
    let advertised = with.advertised(true)["operations"].clone();
    let mut sorted = advertised.as_array().unwrap().clone();
    sorted.sort_by(|a, b| a.as_str().cmp(&b.as_str()));
    assert_eq!(advertised, Value::Array(sorted));
}

#[tokio::test(flavor = "current_thread")]
async fn task_exchange_runs_hgs_with_its_payload() {
    let harness = Harness::new("task-exchange-run", true);
    harness.answer(&EXCHANGE, answer());
    let id = "7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c4d5e";
    assert_eq!(
        harness.exchange(id, "", request()).await,
        json!({"state": "completed", "result": answer(), "error": null})
    );
    assert_eq!(harness.calls(&EXCHANGE), [Some(request())]);
    assert_eq!(harness.journal(), [json!([id, "completed"])]);
}

#[tokio::test(flavor = "current_thread")]
async fn a_refused_or_failing_exchange_fails_without_or_after_hgs() {
    let id = |n: u8| format!("7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c4d{n:02x}");
    // Without the marker nothing runs.
    let old = Harness::new("task-exchange-old", false);
    old.answer(&EXCHANGE, answer());
    assert_eq!(
        old.exchange(&id(1), "", request()).await,
        failed("native task exchange ABI is unavailable")
    );
    assert!(old.calls(&EXCHANGE).is_empty());
    let harness = Harness::new("task-exchange-failures", true);
    // A session target or a payload hgs would refuse never reaches it.
    let refused = harness.exchange(&id(2), "codex/example", request()).await;
    assert_eq!(refused["state"], "failed");
    let mut bad = request();
    bad["schema"] = json!(2);
    assert_eq!(harness.exchange(&id(3), "", bad).await["state"], "failed");
    assert!(harness.calls(&EXCHANGE).is_empty());
    // A failing hgs, and an answer beyond the relay's bound, fail as reads do.
    assert_eq!(
        harness.exchange(&id(4), "", request()).await,
        failed(native::FAILED)
    );
    let mut large = answer();
    large["updates"] = json!([{"task": null, "bytes": "A".repeat(1 << 20)}]);
    harness.answer(&EXCHANGE, large);
    assert_eq!(
        harness.exchange(&id(5), "", request()).await,
        failed(TOO_LARGE)
    );
    harness.answer(&EXCHANGE, json!(["not", "an", "answer"]));
    assert_eq!(
        harness.exchange(&id(6), "", request()).await["state"],
        "failed"
    );
}

#[tokio::test(flavor = "current_thread")]
async fn a_refusal_from_hgs_reaches_the_phone_as_its_text() {
    let harness = Harness::new("task-exchange-refusal", true);
    let busy = "the project's tasks are busy; try again";
    harness.answer(&EXCHANGE, json!({"schema": 1, "ok": false, "error": busy}));
    let id = |n: u8| format!("7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c4e{n:02x}");
    assert_eq!(harness.exchange(&id(1), "", request()).await, failed(busy));
    // hgs answers its refusal on stdout with a failing exit status.
    assert_eq!(*harness.connector.native.failure_json.borrow(), [true]);
    harness.answer(
        &EXCHANGE,
        json!({"schema": 1, "ok": false, "error": ["not", "text"]}),
    );
    assert_eq!(
        harness.exchange(&id(2), "", request()).await,
        failed("native task exchange answer is invalid")
    );
}

#[test]
fn delivered_exchanges_are_found_by_their_operation_not_by_age() {
    umask();
    let scratch = Scratch::new("task-exchange-plan");
    drop(Journal::open(&scratch.path("state"), "fixture-binding", EPOCH).unwrap());
    let db = read_only(&scratch.path("state").join(JOURNAL));
    let plan = query(
        &db,
        &format!("EXPLAIN QUERY PLAN {}", TASK_READ_PRUNE.replace('?', "0")),
    );
    // Thousands of delivered mutation tombstones are never walked for it.
    assert!(
        plan.iter()
            .any(|row| row.to_string().contains("journal_operation")),
        "{plan:?}"
    );
}

#[test]
fn a_delivered_exchange_leaves_the_journal_after_two_minutes() {
    umask();
    let scratch = Scratch::new("task-exchange-journal");
    let journal = Journal::open(&scratch.path("state"), "fixture-binding", EPOCH).unwrap();
    let rows = || {
        query(
            &read_only(&scratch.path("state").join(JOURNAL)),
            "SELECT request_id FROM requests ORDER BY request_id",
        )
    };
    let receipt = json!({"state": "completed", "result": answer(), "error": null});
    for (id, operation) in [("a", "task_exchange"), ("b", "set_mobile_delivery")] {
        assert!(journal.claim(id, operation, Some("hash"), EPOCH).unwrap());
        journal.finish(id, &receipt, EPOCH).unwrap();
        journal.delivered(id, EPOCH).unwrap();
    }
    journal.prune(EPOCH + 100.0).unwrap();
    assert_eq!(rows().len(), 2);
    journal.prune(EPOCH + 121.0).unwrap();
    // The mutation stays; the exchange is gone and could run again.
    assert_eq!(rows(), [json!(["b"])]);
    // An exchange still undelivered is never pruned.
    assert!(journal
        .claim("c", "task_exchange", Some("hash"), EPOCH)
        .unwrap());
    journal.finish("c", &receipt, EPOCH).unwrap();
    journal.prune(EPOCH + 1000.0).unwrap();
    assert_eq!(rows().len(), 2);
}

fn swarm() -> Value {
    json!({"schema": 1, "initialized": true, "node_id": "native-local", "swarm_id": "native-shared",
           "machines": [{"id": "native-local", "connection": "Synthetic computer", "local": true}],
           "organization": {"projects": [
               {"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []},
               {"id": "project-2", "name": "meadow", "color": "#654321", "folders": [], "sessions": []}]}})
}

async fn heartbeat(harness: &Harness) -> Value {
    heartbeat_with(harness, swarm()).await
}

async fn heartbeat_with(harness: &Harness, catalog: Value) -> Value {
    for listing in [
        vec!["ls", "--json", "--local"],
        vec!["ls", "--json", "--local", "--no-archives"],
    ] {
        harness.answer(
            &listing,
            json!({"host": "Synthetic computer", "sessions": []}),
        );
    }
    harness.answer(&["swarm", "get"], catalog);
    let local = tokio::task::LocalSet::new();
    local.run_until(harness.connector.snapshot()).await.unwrap()
}

fn digests(snapshot: &Value) -> Vec<Value> {
    snapshot["mobile_projects"]["projects"]
        .as_array()
        .unwrap()
        .iter()
        .map(|project| project.get("tasks_digest").cloned().unwrap_or(Value::Null))
        .collect()
}

#[tokio::test(flavor = "current_thread")]
async fn heartbeat_projects_carry_their_tasks_digest() {
    let harness = Harness::new("task-exchange-heartbeat", true);
    // A project hgs leaves out, and a digest that is not one, carry none.
    harness.answer(
        &DIGESTS,
        json!({"schema": 1, "projects": {"project-1": {"digest": "ab".repeat(32)},
                                         "project-2": {"digest": "not a digest"},
                                         "elsewhere": {"digest": "ef".repeat(32)}}}),
    );
    let snapshot = heartbeat(&harness).await;
    assert_eq!(digests(&snapshot), [json!("ab".repeat(32)), Value::Null]);
    assert_eq!(harness.calls(&DIGESTS), [None]);
    // Without the marker hgs is not asked and no project carries one.
    let old = Harness::new("task-exchange-heartbeat-old", false);
    old.answer(
        &DIGESTS,
        json!({"schema": 1, "projects": {"project-1": {"digest": "ab".repeat(32)}}}),
    );
    assert_eq!(digests(&heartbeat(&old).await), [Value::Null, Value::Null]);
    assert!(old.calls(&DIGESTS).is_empty());
    // A computer outside any swarm has no projects to ask about.
    let alone = Harness::new("task-exchange-heartbeat-alone", true);
    let uninitialized =
        json!({"schema": 1, "initialized": false, "organization": {"projects": []}});
    heartbeat_with(&alone, uninitialized).await;
    assert!(alone.calls(&DIGESTS).is_empty());
    // A failed digest read leaves the projects as they are.
    let failing = Harness::new("task-exchange-heartbeat-failing", true);
    let snapshot = heartbeat(&failing).await;
    assert_eq!(snapshot["mobile_projects"]["available"], true);
    assert_eq!(digests(&snapshot), [Value::Null, Value::Null]);
}

fn send_payload() -> Value {
    json!({"schema": 1, "project": "project-1", "task": TASK, "session": "codex/api"})
}

fn actions() -> Harness {
    Harness::with_help("task-actions", help() + MARKER + ACTIONS)
}

#[test]
fn the_task_actions_are_advertised_only_with_their_marker() {
    let ops = |help: String| -> Vec<String> {
        Flags::from_help(&help).advertised(true)["operations"]
            .as_array()
            .unwrap()
            .iter()
            .map(|op| op.as_str().unwrap().to_owned())
            .filter(|op| op.starts_with("task_"))
            .collect()
    };
    assert_eq!(
        ops(help() + MARKER + ACTIONS),
        ["task_cancel", "task_done", "task_exchange", "task_send"]
    );
    assert_eq!(ops(help() + MARKER), ["task_exchange"]);
    assert!(ops(help()).is_empty());
    for op in ["task_send", "task_cancel", "task_done"] {
        assert!(!Flags::from_help(&(help() + ACTIONS))
            .python_operations(true)
            .contains(op));
    }
}

#[tokio::test(flavor = "current_thread")]
async fn a_send_runs_hgs_once_and_replays() {
    let harness = actions();
    let answer = json!({"schema": 1, "ok": true, "task": TASK, "stage": "active",
                        "session": {"machine": "m", "session": "codex/api"}, "receipt": {"status": "submitted"}});
    harness.answer(&SEND, answer.clone());
    let id = "7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c5d01";
    assert_eq!(
        harness.finished("task_send", id, send_payload()).await,
        json!({"state": "completed", "result": answer, "error": null})
    );
    assert_eq!(harness.calls(&SEND), [Some(send_payload())]);
    // The same identity again replays its receipt; hgs runs once.
    assert_eq!(
        harness.execute("task_send", id, "", send_payload()).await,
        Outcome::Replayed
    );
    assert_eq!(harness.calls(&SEND).len(), 1);
    let cancel =
        json!({"schema": 1, "project": "project-1", "task": TASK, "reason": "Wrong approach"});
    let cancelled =
        json!({"schema": 1, "ok": true, "task": TASK, "stage": "review", "cancel_message": null});
    harness.answer(&CANCEL, cancelled.clone());
    assert_eq!(
        harness
            .finished(
                "task_cancel",
                "7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c5d02",
                cancel.clone()
            )
            .await,
        json!({"state": "completed", "result": cancelled, "error": null})
    );
    assert_eq!(harness.calls(&CANCEL), [Some(cancel)]);
}

#[tokio::test(flavor = "current_thread")]
async fn a_send_that_may_have_pasted_is_uncertain() {
    let id = |n: u8| format!("7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c6d{n:02x}");
    let state = |receipt: &Value| (receipt["state"].clone(), receipt["error"].clone());
    // Without the marker nothing runs, and nothing happened.
    let old = Harness::new("task-actions-old", true);
    old.answer(&SEND, json!({"schema": 1, "ok": true}));
    assert_eq!(
        state(&old.finished("task_send", &id(1), send_payload()).await),
        (
            json!("failed"),
            json!("native task actions ABI is unavailable")
        )
    );
    assert!(old.calls(&SEND).is_empty());
    let harness = actions();
    // A native failure once the send began: it may have pasted.
    assert_eq!(
        state(&harness.finished("task_send", &id(2), send_payload()).await).0,
        "uncertain"
    );
    let unrecorded = "the prompt was sent, but the task could not record it: disk full";
    harness.answer(
        &SEND,
        json!({"schema": 1, "ok": false, "error": unrecorded, "uncertain": true}),
    );
    assert_eq!(
        state(&harness.finished("task_send", &id(3), send_payload()).await),
        (json!("uncertain"), json!(unrecorded))
    );
    // A refusal before the paste: failed, with hgs's reason.
    let refused = "codex/api cannot take it now: a question is open";
    harness.answer(&SEND, json!({"schema": 1, "ok": false, "error": refused}));
    assert_eq!(
        state(&harness.finished("task_send", &id(4), send_payload()).await),
        (json!("failed"), json!(refused))
    );
    // A session target or a payload the relay refuses never reaches hgs.
    let calls = harness.calls(&SEND).len();
    let mut bad = send_payload();
    bad["task"] = json!("T-a1b2c3");
    assert_eq!(
        state(&harness.finished("task_send", &id(5), bad).await).0,
        "failed"
    );
    match harness
        .execute("task_send", &id(6), "codex/api", send_payload())
        .await
    {
        Outcome::Finished(receipt) => assert_eq!(receipt["state"], "failed"),
        other => panic!("{other:?}"),
    }
    assert_eq!(harness.calls(&SEND).len(), calls);
}

#[tokio::test(flavor = "current_thread")]
async fn a_done_page_is_a_short_read() {
    let harness = actions();
    let page = json!({"schema": 1, "tasks": [], "next": null, "total": 0, "recent": 0});
    harness.answer(&DONE, page.clone());
    let request = json!({"schema": 1, "project": "project-1", "limit": 50});
    let id = "7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c7d01";
    assert_eq!(
        harness.finished("task_done", id, request.clone()).await,
        json!({"state": "completed", "result": page, "error": null})
    );
    harness.answer(
        &DONE,
        json!({"schema": 1, "ok": false, "error": "the project's tasks are busy; try again"}),
    );
    assert_eq!(
        harness
            .finished("task_done", "7d2c4e8a-1b3f-4a5c-9d6e-0f1a2b3c7d02", request)
            .await["error"],
        "the project's tasks are busy; try again"
    );
    let journal = Journal::open(&harness.scratch.path("state2"), "fixture-binding", EPOCH).unwrap();
    let receipt = json!({"state": "completed", "result": page, "error": null});
    assert!(journal
        .claim("d", "task_done", Some("hash"), EPOCH)
        .unwrap());
    journal.finish("d", &receipt, EPOCH).unwrap();
    journal.delivered("d", EPOCH).unwrap();
    journal.prune(EPOCH + 121.0).unwrap();
    assert!(journal
        .claim("d", "task_done", Some("hash"), EPOCH + 121.0)
        .unwrap());
}
