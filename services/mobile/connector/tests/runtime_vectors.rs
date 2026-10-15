//! The runtime against the golden vectors: `validation/*.jsonl`, the
//! `Connector.execute` runs, the heartbeat snapshot and the pure projections
//! of `projections.jsonl`, the bounded inspections and receipt limits of
//! `sizes.jsonl`, the capability markers of `operations.json` and the fixed
//! receipts of `receipts.json`. Native calls are scripted by argv; operations
//! this build does not implement must be refused as unsupported.
mod common;

use common::*;
use serde_json::{json, Map, Value};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;
use zerus_mobile_connector::capabilities::{self, Flags};
use zerus_mobile_connector::execute::{self, Outcome};
use zerus_mobile_connector::journal::Journal;
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::pyvalue::py_eq;
use zerus_mobile_connector::runtime::{Connector, Settings};
use zerus_mobile_connector::{accounts, inspection, projects, validate};

const BINDING: &str = "fixture-binding";

/// Native answers by argv, recording every call with Python's option names.
#[derive(Default)]
struct Scripted {
    answers: RefCell<HashMap<Vec<String>, Vec<Value>>>,
    calls: RefCell<Vec<Value>>,
}

impl Scripted {
    fn answer(&self, argv: Vec<String>, answer: Value) {
        self.answers
            .borrow_mut()
            .entry(argv)
            .or_default()
            .push(answer);
    }
}

fn options(call: &Call) -> Value {
    let mut options = Map::new();
    if let Some(timeout) = call.timeout {
        options.insert("timeout".into(), json!(timeout));
    }
    if !call.json_output {
        options.insert("json_output".into(), json!(false));
    }
    if call.failure_json {
        options.insert("failure_json".into(), json!(true));
    }
    if let Some(bytes) = call.max_output_bytes {
        options.insert("max_output_bytes".into(), json!(bytes));
    }
    Value::Object(options)
}

/// A ConnectorError text of the native layer, as a vector records it.
fn native_error(text: &str) -> NativeError {
    for known in [
        native::COULD_NOT_START,
        native::FAILED,
        native::INVALID_JSON,
        native::TIMED_OUT,
        native::OUTPUT_TOO_LARGE,
        native::REQUEST_TOO_LARGE,
    ] {
        if known == text {
            return NativeError::Failed(known);
        }
    }
    panic!("unknown native error {text}")
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        let mut record = json!({"argv": call.argv, "options": options(&call)});
        if let Some(payload) = &call.payload {
            record["payload"] = payload.clone();
        }
        self.calls.borrow_mut().push(record);
        let answer = {
            let mut answers = self.answers.borrow_mut();
            match answers.get_mut(&call.argv) {
                Some(queue) if queue.len() > 1 => Some(queue.remove(0)),
                Some(queue) => queue.first().cloned(),
                None => None,
            }
        };
        let Some(answer) = answer else {
            return Err(NativeError::Failed(native::FAILED));
        };
        match answer.get("raised").and_then(Value::as_str) {
            Some("ConnectorError") => Err(native_error(answer["error"].as_str().unwrap())),
            // Any other exception of a scripted Python native.
            Some(_) => Err(NativeError::Unicode),
            None if call.json_output => Ok(Output::Json(answer)),
            None => Ok(Output::Text(answer.as_str().unwrap().into())),
        }
    }
}

struct Harness {
    connector: Rc<Connector<Scripted>>,
    _scratch: Scratch,
}

fn harness(label: &str) -> Harness {
    umask();
    let scratch = Scratch::new(label);
    let journal = Journal::open(&scratch.path("state"), BINDING, EPOCH).unwrap();
    let settings = Settings {
        interval: 5.0,
        binding: BINDING.into(),
        default_timeout: native::DEFAULT_TIMEOUT,
        clock_scale: 1.0,
    };
    let connector = Connector::new(Scripted::default(), journal, settings, Box::new(|| EPOCH));
    Harness {
        connector: Rc::new(connector),
        _scratch: scratch,
    }
}

fn string_set(value: &Value) -> HashSet<String> {
    value
        .as_array()
        .unwrap()
        .iter()
        .map(|item| item.as_str().unwrap().to_owned())
        .collect()
}

fn filled(value: &Value, fill: Option<&Value>) -> Value {
    let Some(fill) = fill else {
        return value.clone();
    };
    match value {
        Value::String(text) if fill.get(text).is_some() => {
            let mut out = String::new();
            for part in fill[text].as_array().unwrap() {
                out.push_str(
                    &part[0]
                        .as_str()
                        .unwrap()
                        .repeat(part[1].as_u64().unwrap() as usize),
                );
            }
            Value::String(out)
        }
        Value::Array(items) => {
            Value::Array(items.iter().map(|item| filled(item, Some(fill))).collect())
        }
        Value::Object(members) => Value::Object(
            members
                .iter()
                .map(|(key, item)| (key.clone(), filled(item, Some(fill))))
                .collect(),
        ),
        other => other.clone(),
    }
}

/// `projections.jsonl`. One hostile account status holds `Infinity`, which
/// the strict codec refuses in native output; `1e400` is the same float to
/// Python and reaches the projection as an unbounded number.
fn projections() -> Vec<Value> {
    std::fs::read_to_string(vectors().join("projections.jsonl"))
        .unwrap()
        .lines()
        .map(|line| parse(&line.replace(":Infinity", ":1e400")))
        .collect()
}

fn parse(text: &str) -> Value {
    zerus_relay::json::parse(text.as_bytes(), 64, 10_000_000).unwrap()
}

#[test]
fn validation_matches_the_python_connector_for_implemented_operations() {
    let mut checked = 0;
    let mut refused = 0;
    for family in ["control", "conversation", "envelope", "launch", "speech"] {
        let rows = records(&format!("validation/{family}.jsonl"));
        let base = rows.iter().find(|row| row["kind"] == "context").unwrap()["context"].clone();
        for case in rows.iter().filter(|row| row["kind"] == "case") {
            let id = case["id"].as_str().unwrap();
            let raw = case.get("context").unwrap_or(&base);
            let context = validate::Context {
                sessions: string_set(&raw["sessions"]),
                archives: raw["archives"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|pair| {
                        (
                            pair[0].as_str().unwrap().into(),
                            pair[1].as_str().unwrap().into(),
                        )
                    })
                    .collect(),
                snapshot_age: raw["snapshot_age"].as_f64().unwrap(),
            };
            let mut request = case["submission"].as_object().unwrap().clone();
            request.remove("computer_id");
            let expect = &case["connector"];
            let request_id = match validate::checked_uuid(request.get("request_id")) {
                Ok(request_id) => request_id,
                Err(error) => {
                    assert_eq!(expect["stage"], "before_journal", "{id}");
                    assert_eq!(expect["error"], json!(error), "{id}");
                    checked += 1;
                    continue;
                }
            };
            let operation = request
                .get("operation")
                .and_then(Value::as_str)
                .unwrap_or_default();
            let result = validate::validate(&request, &request_id, &context);
            if !capabilities::implemented(operation) {
                assert_eq!(result, Err(validate::UNSUPPORTED.into()), "{id}");
                refused += 1;
                continue;
            }
            match expect["decision"].as_str().unwrap() {
                "accept" => {
                    let validated = result.unwrap_or_else(|error| panic!("{id}: {error}"));
                    match expect["native_payload"].as_str().unwrap() {
                        "unchanged" => {
                            assert_eq!(validated.payload.as_ref(), request.get("payload"), "{id}")
                        }
                        "absent" => assert_eq!(validated.payload, None, "{id}"),
                        other => panic!("{id}: {other}"),
                    }
                }
                _ => {
                    assert_eq!(expect["stage"], "receipt", "{id}");
                    assert!(
                        expect.get("raised").is_none(),
                        "{id}: generic receipts are not validation"
                    );
                    assert_eq!(
                        result,
                        Err(expect["error"].as_str().unwrap().into()),
                        "{id}"
                    );
                }
            }
            checked += 1;
        }
    }
    assert_eq!((checked, refused), (52, 185));
}

/// Python's `capable(node)`: every flag, the vectors' sessions and archive.
fn capable(connector: &Connector<Scripted>) {
    let all = projections()
        .into_iter()
        .find(|row| row["kind"] == "snapshot")
        .unwrap()["native"]["--help"]
        .as_str()
        .unwrap()
        .to_owned();
    connector.flags.set(Flags::from_help(&all));
    let mut state = connector.state.borrow_mut();
    state.sessions = ["codex/example/session", "dsh/example/parent"]
        .map(String::from)
        .into();
    state.archives = [(
        "codex/example/session".to_owned(),
        "81b191c0-bdae-543a-ba1c-0fbcbb2f638d".to_owned(),
    )]
    .into();
    state.snapshot_seen_at = Some(connector.clock());
}

#[tokio::test(flavor = "current_thread")]
async fn executions_match_the_python_connector() {
    let mut checked = Vec::new();
    for run in projections()
        .into_iter()
        .filter(|row| row["kind"] == "execute")
    {
        let id = run["id"].as_str().unwrap().to_owned();
        let operation = run["request"]["operation"].as_str().unwrap();
        let routed = run["request"].get("gateway_route").is_some();
        if !capabilities::implemented(operation) && operation != "bogus_operation" && !routed {
            continue;
        }
        let harness = harness(&format!("execute-{id}"));
        let connector = &harness.connector;
        capable(connector);
        for native in run["native"].as_array().unwrap() {
            let argv: Vec<String> = native["argv"]
                .as_array()
                .unwrap()
                .iter()
                .map(|arg| arg.as_str().unwrap().into())
                .collect();
            connector
                .native
                .answer(argv, filled(&native["answer"], native.get("fill")));
        }
        let outcome = connector.execute(run["request"].clone()).await.unwrap();
        let expect = &run["expect"];
        let Outcome::Finished(response) = outcome else {
            panic!("{id}: {outcome:?}");
        };
        let stored = parse(expect["stored"].as_str().unwrap());
        assert!(py_eq(&response, &stored), "{id}: {response} != {stored}");
        assert_eq!(response["state"], expect["state"], "{id}");
        let calls = Value::Array(connector.native.calls.borrow().clone());
        assert!(
            py_eq(&calls, &expect["calls"]),
            "{id}: {calls} != {}",
            expect["calls"]
        );
        checked.push(id);
    }
    checked.sort();
    assert_eq!(
        checked,
        [
            "catalog-projection",
            "directory-create-route-mismatch",
            "dirs-projection",
            "gateway-route-invalid",
            "gateway-route-unavailable",
            "inspect-completed",
            "inspect-native-crash",
            "mark-read-not-found",
            "process-output",
            "process-output-too-large",
            "unsupported-operation",
            "worktrees-state-prefix",
        ]
    );
}

#[tokio::test(flavor = "current_thread")]
async fn later_operations_are_refused_without_native_calls() {
    for run in projections()
        .into_iter()
        .filter(|row| row["kind"] == "execute")
    {
        let operation = run["request"]["operation"].as_str().unwrap();
        if capabilities::implemented(operation)
            || operation == "bogus_operation"
            || operation == "directory_create"
            || run["request"].get("gateway_route").is_some()
        {
            continue;
        }
        let harness = harness(&format!("refused-{}", run["id"].as_str().unwrap()));
        capable(&harness.connector);
        let outcome = harness
            .connector
            .execute(run["request"].clone())
            .await
            .unwrap();
        assert_eq!(
            outcome,
            Outcome::Finished(
                json!({"state": "failed", "result": null, "error": "unsupported operation"})
            ),
            "{}",
            run["id"]
        );
        assert!(harness.connector.native.calls.borrow().is_empty());
    }
}

#[tokio::test(flavor = "current_thread")]
async fn the_heartbeat_snapshot_matches_the_python_connector() {
    let vector = projections()
        .into_iter()
        .find(|row| row["kind"] == "snapshot")
        .unwrap();
    let harness = harness("snapshot");
    let connector = &harness.connector;
    for (command, answer) in vector["native"].as_object().unwrap() {
        let argv: Vec<String> = match command.as_str() {
            "inspect codex/example/session" => {
                vec!["inspect".into(), "codex/example/session".into()]
            }
            command => command.split(' ').map(String::from).collect(),
        };
        connector.native.answer(argv, answer.clone());
    }
    let local = tokio::task::LocalSet::new();
    let snapshot = local.run_until(connector.snapshot()).await.unwrap();
    let expected = &vector["expect"]["heartbeat"];
    let mut wanted = expected["snapshot"].clone();
    // This build advertises only what it implements (capabilities.rs).
    wanted["mobile_capabilities"] =
        Flags::from_help(vector["native"]["--help"].as_str().unwrap()).advertised(false);
    assert_eq!(snapshot, wanted);
    assert_eq!(connector.state.borrow().presence, expected["presence"]);
    let mut calls = connector.native.calls.borrow().clone();
    calls.sort_by_key(|call| call["argv"].to_string());
    let mut wanted_calls = vector["expect"]["calls"].as_array().unwrap().clone();
    wanted_calls.sort_by_key(|call| call["argv"].to_string());
    assert!(py_eq(&json!(calls), &json!(wanted_calls)), "{calls:?}");
    for key in ["peers", "ended_launches", "desktop_presence"] {
        assert!(snapshot.get(key).is_none(), "{key}");
    }
}

fn call_records(function: &str) -> Vec<Value> {
    projections()
        .into_iter()
        .filter(|row| row["kind"] == "call" && row["function"] == function)
        .collect()
}

fn outcome(result: Result<Value, zerus_mobile_connector::pyvalue::PyError>) -> Value {
    match result {
        Ok(value) => json!({"returns": value}),
        Err(zerus_mobile_connector::pyvalue::PyError::Value(message)) => {
            json!({"raises": {"message": message, "type": "ValueError"}})
        }
        Err(error) => panic!("{error:?}"),
    }
}

fn expected(record: &Value) -> Value {
    match record.get("returns") {
        Some(value) => json!({"returns": value}),
        None => json!({"raises": record["raises"]}),
    }
}

#[test]
fn projections_match_the_python_functions() {
    let mut checked = 0;
    for record in call_records("projects.normalize") {
        let args = &record["args"];
        assert_eq!(
            outcome(projects::normalize(
                &args[0],
                &args[1],
                args[2].as_str().unwrap()
            )),
            expected(&record),
            "{}",
            record["id"]
        );
        checked += 1;
    }
    for record in call_records("projects.unavailable") {
        assert_eq!(
            projects::unavailable(record["args"][0].as_str().unwrap()),
            record["returns"]
        );
        checked += 1;
    }
    let normalized = call_records("projects.normalize")
        .into_iter()
        .find(|record| record["id"] == "projects-normalize")
        .unwrap()["returns"]
        .clone();
    for record in call_records("projects.apply_memberships") {
        let mut snapshot = record["args"][0].clone();
        let mut sessions = snapshot["sessions"].as_array().unwrap().clone();
        assert_eq!(record["args"][1], "$normalized");
        projects::apply_memberships(&mut sessions, &normalized);
        snapshot["sessions"] = Value::Array(sessions);
        assert_eq!(snapshot, record["returns"], "{}", record["id"]);
        checked += 1;
    }
    for record in call_records("accounts.usage") {
        assert_eq!(
            accounts::usage(&record["args"][0]),
            record["returns"],
            "{}",
            record["id"]
        );
        checked += 1;
    }
    for record in call_records("accounts.model_options") {
        assert_eq!(
            accounts::model_options(record["args"][0].as_object().unwrap()),
            record["returns"]
        );
        checked += 1;
    }
    for record in call_records("launch.catalog") {
        assert_eq!(
            outcome(accounts::launch_catalog(&record["args"][0])),
            expected(&record),
            "{}",
            record["id"]
        );
        checked += 1;
    }
    for record in call_records("accounts.catalog+view") {
        let result = accounts::catalog(&record["args"][0]).map(|fresh| {
            let mut cache = accounts::AccountCache::default();
            cache.apply_catalog(Ok(fresh), EPOCH);
            cache.view(EPOCH)
        });
        assert_eq!(outcome(result), expected(&record), "{}", record["id"]);
        checked += 1;
    }
    for record in call_records("worktrees.catalog") {
        let args = &record["args"];
        let result =
            execute::worktree_catalog(&args[0], args[1].as_str().unwrap()).map(Value::Object);
        assert_eq!(outcome(result), expected(&record), "{}", record["id"]);
        checked += 1;
    }
    for record in call_records("context.mark_read_result") {
        let args = &record["args"];
        assert_eq!(
            json!(execute::mark_read_result(&args[0], &args[1])),
            record["returns"],
            "{}",
            record["id"]
        );
        checked += 1;
    }
    assert_eq!(checked, 19);
}

#[test]
fn bounded_inspections_match_the_python_function() {
    let mut checked = 0;
    for record in records("sizes.jsonl")
        .into_iter()
        .filter(|row| row["kind"] == "bound_inspection")
    {
        let id = &record["id"];
        let value = filled(&record["value"], record.get("fill"));
        let maximum = record["maximum"].as_u64().unwrap() as usize;
        let result =
            inspection::bound_inspection(value.as_object().unwrap().clone(), maximum).unwrap();
        let expect = &record["expect"];
        let at = |stream: &str| -> Value {
            result[stream]
                .as_array()
                .unwrap()
                .iter()
                .map(|row| row["at"].clone())
                .collect()
        };
        assert_eq!(at("events"), expect["kept_events_at"], "{id}");
        assert_eq!(
            at("message_events"),
            expect["kept_message_events_at"],
            "{id}"
        );
        assert_eq!(
            json!(result.contains_key("mobile_history_truncated")),
            expect["truncated"],
            "{id}"
        );
        assert_eq!(
            result
                .get("mobile_history_omitted")
                .cloned()
                .unwrap_or(Value::Null),
            expect["omitted"],
            "{id}"
        );
        assert_eq!(
            inspection::completed_size(&result).unwrap() as u64,
            expect["completed_size"].as_u64().unwrap(),
            "{id}"
        );
        let canonical = zerus_relay::json::canonical(&Value::Object(result)).unwrap();
        assert_eq!(
            zerus_relay::json::digest(canonical),
            expect["sha256"].as_str().unwrap(),
            "{id}"
        );
        checked += 1;
    }
    assert_eq!(checked, 2);
}

#[test]
fn receipts_fit_python_and_the_relay() {
    let request_id = "fcabfe83-02cd-59f6-aa77-6f2d1fbd66f1";
    for record in records("sizes.jsonl")
        .into_iter()
        .filter(|row| row["kind"] == "limit" && row["measure"] == "default_ascii")
    {
        let id = record["id"].as_str().unwrap();
        let receipt = {
            let mut receipt = filled(&record["value"], record.get("fill"));
            receipt["state"] = json!("completed");
            receipt
        };
        let plain = zerus_relay::json::python_default_len(&receipt, true).unwrap();
        assert_eq!(
            plain as u64,
            record["expect"]["bytes"].as_u64().unwrap(),
            "{id}"
        );
        assert_eq!(
            plain > execute::MAX_RESULT,
            record["expect"]["exceeds"] == true,
            "{id}"
        );
        // Each fits Python's plain bound by at most one byte; with its
        // request_id the relay's canonical form is larger, so the receipt
        // becomes the fixed failure (divergence b).
        let sanitized = execute::sanitize(receipt.clone(), request_id, false);
        assert_eq!(
            sanitized,
            json!({"state": "failed", "result": null, "error": execute::TOO_LARGE}),
            "{id}"
        );
        assert_eq!(
            execute::sanitize(receipt, request_id, true)["state"],
            "uncertain",
            "{id}"
        );
    }
    let long = "\u{e9}".repeat(4097);
    let cut = execute::sanitize(
        json!({"state": "failed", "result": null, "error": long}),
        request_id,
        true,
    );
    assert_eq!(cut["error"].as_str().unwrap().chars().count(), 4096);
    let typed = execute::sanitize(
        json!({"state": "failed", "result": null, "error": {"detail": "x"}}),
        request_id,
        true,
    );
    assert_eq!(typed["error"], execute::ERROR_NOT_TEXT);
    let kept = json!({"state": "completed", "result": {"ok": true}, "error": null});
    assert_eq!(execute::sanitize(kept.clone(), request_id, true), kept);
}

#[test]
fn capability_markers_match_the_python_connector() {
    let operations = document("operations.json");
    let capabilities = &operations["capabilities"];
    let python = |flags: &Flags, one_hop: bool| {
        json!({
            "operations": flags.python_operations(true).into_iter().collect::<Vec<_>>(),
            "features": flags.python_features(one_hop).into_iter().collect::<Vec<_>>(),
        })
    };
    let all = Flags::from_help(capabilities["help_text_all"].as_str().unwrap());
    let none = Flags::default();
    for (name, flags, one_hop) in [
        ("all", &all, true),
        ("without_peer_discovery", &all, false),
        ("none", &none, false),
        ("probe_failed", &none, false),
    ] {
        let wanted = &capabilities[name];
        assert_eq!(
            python(flags, one_hop),
            json!({"operations": wanted["operations"], "features": wanted["features"]}),
            "{name}"
        );
        let advertised = flags.advertised(one_hop);
        assert_eq!(advertised["reasons"], wanted["reasons"], "{name}");
        for operation in advertised["operations"].as_array().unwrap() {
            assert!(capabilities::implemented(operation.as_str().unwrap()));
            assert!(
                wanted["operations"].as_array().unwrap().contains(operation),
                "{name}"
            );
        }
    }
    assert!(all.peers && all.read && all.file_read);
    // Each marker, left out of the full help text, takes away exactly its
    // operations and features and adds its reasons.
    let help = capabilities["help_text_all"].as_str().unwrap();
    for marker in capabilities["markers"].as_array().unwrap() {
        let line = format!("{}\n", marker["help_line"].as_str().unwrap());
        let without = Flags::from_help(&help.replacen(&line, "", 1));
        let one_hop = |flags: &Flags| flags.peers;
        let lost: Vec<&str> = all
            .python_operations(true)
            .difference(&without.python_operations(true))
            .copied()
            .collect();
        let lost_features: Vec<&str> = all
            .python_features(one_hop(&all))
            .difference(&without.python_features(one_hop(&without)))
            .copied()
            .collect();
        assert_eq!(json!(lost), marker["operations"], "{}", marker["name"]);
        assert_eq!(
            json!(lost_features),
            marker["features"],
            "{}",
            marker["name"]
        );
        assert_eq!(
            without.advertised(false)["reasons"],
            marker["reasons"],
            "{}",
            marker["name"]
        );
    }
}

#[test]
fn fixed_receipts_keep_python_texts() {
    let receipts = document("receipts.json");
    let receipts = &receipts["receipts"];
    for (name, text) in [
        (
            "cancelled_running_read",
            zerus_mobile_connector::journal::RESTART_ERROR,
        ),
        ("execute:unsupported-operation", validate::UNSUPPORTED),
        ("execute:inspect-native-crash", execute::GENERIC),
        ("execute:process-output-too-large", execute::TOO_LARGE),
    ] {
        assert_eq!(receipts[name]["error"], text, "{name}");
    }
}

/// Every native budget, plus the drain after a kill, ends before systemd's
/// default 90 s `TimeoutStopSec`: a stop waits for one call at most, since
/// no call starts once the connector is stopping (divergence h).
#[test]
fn every_native_budget_ends_before_systemd_stops_the_unit() {
    let budgets = [
        native::DEFAULT_TIMEOUT,
        capabilities::PROBE_TIMEOUT,
        accounts::CATALOG_TIMEOUT,
        accounts::INSPECT_TIMEOUT,
        zerus_mobile_connector::runtime::SWARM_TIMEOUT,
        native::MOBILE_CREATE_DRAIN.as_secs_f64(),
        2.0,
        5.0,
    ];
    let longest = budgets.iter().copied().fold(0.0, f64::max);
    assert!(longest + native::DRAIN.as_secs_f64() < 90.0, "{longest}");
    assert!(zerus_mobile_connector::http::CALL_TIMEOUT.as_secs_f64() < 90.0);
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn a_failed_probe_keeps_the_previous_flags() {
    let harness = harness("probe");
    let connector = &harness.connector;
    let help = vec!["--help".to_owned()];
    connector
        .native
        .answer(help.clone(), json!("hgs read <session> --json\n"));
    connector.probe_capabilities().await;
    assert!(connector.flags.get().read && connector.probed.is_set());
    // Within 300 s nothing is probed again.
    connector.probe_capabilities().await;
    assert_eq!(connector.native.calls.borrow().len(), 1);
    connector.native.answers.borrow_mut().clear();
    tokio::time::advance(std::time::Duration::from_secs(301)).await;
    connector.probe_capabilities().await;
    assert_eq!(connector.native.calls.borrow().len(), 2);
    assert!(
        connector.flags.get().read,
        "a failed probe disabled the previous flags"
    );
    connector.native.answer(help, json!("Usage: hgs\n"));
    tokio::time::advance(std::time::Duration::from_secs(301)).await;
    connector.probe_capabilities().await;
    assert!(!connector.flags.get().read);
}

#[tokio::test(flavor = "current_thread")]
async fn no_native_call_starts_once_the_connector_stops() {
    let harness = harness("stopping");
    let connector = &harness.connector;
    connector.stopping.set();
    assert_eq!(
        connector
            .native(Call::new(["inspect", "codex/example/session"]))
            .await,
        Err(NativeError::Stopping)
    );
    assert!(connector.native.calls.borrow().is_empty());
    capable(connector);
    let request = json!({"request_id": "470a1b9f-d243-5102-8685-6f97bc3f295d", "operation": "inspect",
                         "session": "codex/example/session", "payload": {}});
    assert_eq!(
        connector.execute(request).await.unwrap(),
        Outcome::Finished(json!({"state": "failed", "result": null,
                                 "error": zerus_mobile_connector::journal::RESTART_ERROR}))
    );
}

#[test]
fn stored_rows_upload_as_stored_or_as_a_fixed_substitute() {
    let id = "470a1b9f-d243-5102-8685-6f97bc3f295d";
    let body =
        |text: &str, operation: &str| Connector::<Scripted>::upload_body(text, id, operation);
    let upload =
        |text: &str, operation: &str| parse(std::str::from_utf8(&body(text, operation)).unwrap());
    let stored =
        "{\"state\": \"completed\", \"result\": {\"b\": 1, \"a\": [2.50]}, \"error\": null}";
    assert_eq!(body(stored, "inspect"), stored.as_bytes());
    let substitute = |state: &str| json!({"state": state, "result": null, "error": zerus_mobile_connector::runtime::STORED_INVALID});
    let nan = "{\"state\": \"completed\", \"result\": NaN, \"error\": null}";
    assert_eq!(upload(nan, "inspect"), substitute("failed"));
    assert_eq!(upload(nan, "send"), substitute("uncertain"));
    assert_eq!(upload(nan, "unknown_operation"), substitute("failed"));
    let long = format!(
        "{{\"state\": \"failed\", \"result\": null, \"error\": \"{}\"}}",
        "e".repeat(4097)
    );
    assert_eq!(upload(&long, "send"), substitute("uncertain"));
    let deep = format!(
        "{{\"state\": \"completed\", \"result\": {}1{}, \"error\": null}}",
        "[".repeat(32),
        "]".repeat(32)
    );
    assert_eq!(upload(&deep, "inspect"), substitute("failed"));
    let large = format!(
        "{{\"state\": \"completed\", \"result\": \"{}\", \"error\": null}}",
        "a".repeat(1024 * 1024)
    );
    assert_eq!(upload(&large, "inspect"), substitute("failed"));
    let unknown = "{\"state\": \"done\", \"result\": null, \"error\": null}";
    assert_eq!(upload(unknown, "inspect"), substitute("failed"));
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn a_refused_result_shortens_the_poll_until_its_next_attempt() {
    let harness = harness("poll-wait");
    let connector = &harness.connector;
    assert_eq!(connector.poll_wait(), 25);
    let now = connector.clock();
    connector
        .state
        .borrow_mut()
        .refused
        .insert("a".into(), (1, now));
    assert_eq!(connector.poll_wait(), 25);
    tokio::time::advance(std::time::Duration::from_secs(12)).await;
    assert_eq!(connector.poll_wait(), 18);
    tokio::time::advance(std::time::Duration::from_secs(30)).await;
    assert_eq!(connector.poll_wait(), 1);
}
