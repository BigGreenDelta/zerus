#![cfg(unix)]
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
    for family in [
        "control",
        "conversation",
        "envelope",
        "launch",
        "project",
        "speech",
    ] {
        let rows = records(&format!("validation/{family}.jsonl"));
        let base = rows.iter().find(|row| row["kind"] == "context").unwrap()["context"].clone();
        for case in rows.iter().filter(|row| row["kind"] == "case") {
            let id = case["id"].as_str().unwrap();
            let raw = case.get("context").unwrap_or(&base);
            let context = validate::Context {
                paged: Default::default(),
                archive_supported: false,
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
                    // A TypeError in a validator is Python's generic receipt.
                    if let Some(raised) = expect.get("raised") {
                        assert_eq!(raised, "TypeError", "{id}");
                        assert_eq!(expect["error"], execute::GENERIC, "{id}");
                    }
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
    // The common Python vectors include both hook health and analytics reads.
    assert_eq!((checked, refused), (277, 2));
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
        if let Some(unavailable) = run.get("unavailable") {
            let mut flags = connector.flags.get();
            for flag in unavailable.as_array().unwrap() {
                match flag.as_str().unwrap() {
                    "attachment_read" => flags.attachment_read = false,
                    other => panic!("{id}: unknown unavailable capability {other}"),
                }
            }
            connector.flags.set(flags);
        }
        for native in run["native"].as_array().unwrap() {
            let argv: Vec<String> = native["argv"]
                .as_array()
                .unwrap()
                .iter()
                .map(|arg| arg.as_str().unwrap().into())
                .collect();
            // A list is the answers to consecutive calls, as in bb_operations.replay.
            let answer = filled(&native["answer"], native.get("fill"));
            for answer in answer.as_array().cloned().unwrap_or_else(|| vec![answer]) {
                connector.native.answer(argv.clone(), answer);
            }
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
            "attachment-read-chunk",
            "attachment-read-missing-file",
            "attachment-read-unavailable",
            "catalog-projection",
            "directory-create-route-mismatch",
            "dirs-projection",
            "file-read-snapshot",
            "gateway-route-invalid",
            "gateway-route-unavailable",
            "inspect-completed",
            "inspect-native-crash",
            "mark-read-not-found",
            "pause-native-failed",
            "process-output",
            "process-output-too-large",
            "project_assign-confirmed",
            "project_assignment-confirmed",
            "restore-archive",
            "send-completed",
            "send-native-crash",
            "send-native-timeout",
            "set-mobile-delivery",
            "speech-submit-drops-text",
            "terminal-input-expired",
            "terminal-input-submitted",
            "terminal-snapshot",
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

/// A heartbeat against scripted native answers keyed by their command line.
async fn heartbeat(vector: &Value) -> (Value, Value, Vec<Value>) {
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
    let presence = connector.state.borrow().presence.clone();
    let calls = connector.native.calls.borrow().clone();
    (snapshot, presence, calls)
}

#[tokio::test(flavor = "current_thread")]
async fn the_paged_heartbeat_snapshot_matches_the_python_connector() {
    // A CLI with the archive marker: live sessions only, and the archive
    // preview of `archives --json` with its descriptor.
    let vector = projections()
        .into_iter()
        .find(|row| row["kind"] == "snapshot")
        .unwrap();
    let (snapshot, presence, mut calls) = heartbeat(&vector).await;
    let expected = &vector["expect"]["heartbeat"];
    let mut wanted = expected["snapshot"].clone();
    // This build advertises only what it implements, and no peers yet (C6).
    wanted["mobile_capabilities"] =
        Flags::from_help(vector["native"]["--help"].as_str().unwrap()).advertised(false);
    assert_eq!(snapshot, wanted);
    assert_eq!(presence, expected["presence"]);
    calls.sort_by_key(|call| call["argv"].to_string());
    let mut wanted_calls = vector["expect"]["calls"].as_array().unwrap().clone();
    wanted_calls.sort_by_key(|call| call["argv"].to_string());
    assert!(py_eq(&json!(calls), &json!(wanted_calls)), "{calls:?}");
}

#[tokio::test(flavor = "current_thread")]
async fn the_heartbeat_snapshot_matches_the_python_connector() {
    // A CLI without the archive marker: the legacy listing, archives included,
    // and no archive preview.
    let vector = document("heartbeat-legacy.json");
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

/// Route identities phones and relays keep, equal to Python's uuid5.
#[test]
fn route_identities_match_the_python_connector() {
    let identities = document("identities.json");
    assert_eq!(
        identities["route_namespace"],
        zerus_mobile_connector::fleet::ROUTE_NAMESPACE
    );
    let routes = identities["route_ids"].as_array().unwrap();
    assert!(!routes.is_empty());
    for row in routes {
        assert_eq!(
            zerus_mobile_connector::fleet::route_id(
                row["via"].as_str().unwrap(),
                row["machine_id"].as_str().unwrap()
            ),
            row["route_id"].as_str().unwrap(),
            "{row}"
        );
    }
}

/// The black-box suites expect `unsupported operation` for exactly the known
/// operations this build does not implement, from `tests/ported.json`.
#[test]
fn ported_operations_name_what_this_build_implements() {
    let raw = std::fs::read(vectors().join("../tests/ported.json")).unwrap();
    let ported: Value = serde_json::from_slice(&raw).unwrap();
    let listed: Vec<&str> = ported["rust_operations"]
        .as_array()
        .unwrap()
        .iter()
        .map(|op| op.as_str().unwrap())
        .collect();
    let mut implemented: Vec<&str> = zerus_relay::protocol::OPERATIONS
        .iter()
        .copied()
        .filter(|operation| capabilities::implemented(operation))
        .collect();
    implemented.sort_unstable();
    assert_eq!(listed, implemented);
}

/// The connector-only table equals the shared one the relay tests and the
/// black-box suites read, and a gate named after a frozen marker is that
/// marker.
#[test]
fn connector_only_operations_match_the_shared_table() {
    let raw = std::fs::read(vectors().join("../tests/rust-only-operations.json")).unwrap();
    let shared: Value = serde_json::from_slice(&raw).unwrap();
    let rows: Vec<Value> = capabilities::CONNECTOR_ONLY
        .iter()
        .map(|row| {
            let class = match row.class {
                capabilities::Class::Read => "read",
                capabilities::Class::Mutation => "mutation",
            };
            json!({"operation": row.operation, "marker": row.gate.name(), "class": class})
        })
        .collect();
    assert_eq!(json!(rows), shared["operations"]);
    let operations = document("operations.json");
    let markers = operations["capabilities"]["markers"].as_array().unwrap();
    for row in capabilities::CONNECTOR_ONLY {
        let frozen = markers
            .iter()
            .find(|marker| marker["name"] == row.gate.name());
        match frozen {
            Some(marker) => assert_eq!(marker["help_line"], row.gate.help_line(), "{row:?}"),
            None => assert_eq!(
                shared["markers"][row.gate.name()],
                row.gate.help_line(),
                "{row:?}"
            ),
        }
    }
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
            let operation = operation.as_str().unwrap();
            assert!(capabilities::implemented(operation));
            assert!(
                wanted["operations"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .any(|wanted| wanted == operation)
                    || capabilities::connector_only(operation),
                "{name} {operation}"
            );
        }
        // A connector-only operation comes exactly with its own native gate.
        for row in capabilities::CONNECTOR_ONLY {
            assert_eq!(
                advertised["operations"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .any(|advertised| advertised == row.operation),
                row.gate.open(flags),
                "{name} {row:?}"
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
        zerus_mobile_connector::repositories::CLONE_TIMEOUT,
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

#[tokio::test(flavor = "current_thread")]
async fn video_voice_is_capability_gated_and_accepted_once_with_both_media() {
    use base64::{engine::general_purpose::STANDARD, Engine};
    let harness = harness("video-voice");
    let connector = &harness.connector;
    capable(connector);
    let mut mp4 = Vec::new();
    for (kind, data) in [
        (b"ftyp", b"isom0000".as_slice()),
        (b"moov", b"".as_slice()),
        (b"mdat", b"video".as_slice()),
    ] {
        mp4.extend_from_slice(&((data.len() + 8) as u32).to_be_bytes());
        mp4.extend_from_slice(kind);
        mp4.extend_from_slice(data);
    }
    let request = |id: &str| {
        json!({"request_id":id,"computer_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","operation":"voice_submit","session":"codex/example/session",
        "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation","speech":{"machine_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","profile_id":"fixture"},
        "audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":STANDARD.encode(vec![0u8;32000])},
        "video":{"format":"mp4","data_base64":STANDARD.encode(&mp4),"duration_ms":1000,"width":480,"height":480}}})
    };
    let refused = connector
        .execute(request("11111111-1111-4111-8111-111111111111"))
        .await
        .unwrap();
    assert_eq!(
        refused,
        Outcome::Finished(
            json!({"state":"failed","result":null,"error":"native video recording ABI is unavailable"})
        )
    );
    assert!(connector.native.calls.borrow().is_empty());
    let mut flags = connector.flags.get();
    flags.voice_video = true;
    connector.flags.set(flags);
    assert!(flags.advertised(false)["features"]
        .as_array()
        .unwrap()
        .contains(&json!("voice_video")));
    let id = "22222222-2222-4222-8222-222222222222";
    let upload = request(id);
    let result = json!({"schema":1,"message":{"id":id,"run_id":"run","conversation_id":"conversation","state":"queued","revision":1,
        "video":{"request_id":id,"index":1,"name":"video.mp4","mime":"video/mp4","bytes":mp4.len(),"duration_ms":1000,"width":480,"height":480}}});
    connector.native.answer(
        vec![
            "voice".into(),
            "codex/example/session".into(),
            "--json".into(),
        ],
        result.clone(),
    );
    let accepted = connector.execute(upload.clone()).await.unwrap();
    assert_eq!(
        accepted,
        Outcome::Finished(json!({"state":"completed","result":result,"error":null}))
    );
    assert_eq!(
        connector.execute(upload.clone()).await.unwrap(),
        Outcome::Replayed
    );
    assert_eq!(connector.native.calls.borrow().len(), 1);
    let mut wrong = result;
    wrong["message"]["video"]["bytes"] = json!(1);
    assert!(!zerus_mobile_connector::speech::voice_result_valid(
        "voice_submit",
        &wrong,
        &upload["payload"]
    )
    .unwrap());
}

#[tokio::test(flavor = "current_thread")]
async fn voice_refusal_and_explicit_retry_keep_separate_operation_and_native_receipts() {
    use base64::{engine::general_purpose::STANDARD, Engine};
    let harness = harness("voice-refusal");
    let connector = &harness.connector;
    capable(connector);
    let id = "11111111-1111-4111-8111-111111111111";
    let upload = json!({"request_id":id,"computer_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","operation":"voice_submit","session":"codex/example/session",
        "payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation",
        "speech":{"machine_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","profile_id":"fixture","profile_revision":7},
        "audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":STANDARD.encode(vec![0u8;3200])}}});
    let proof = json!({"schema":1,"state":"rejected_before_acceptance","request_id":id,"message_id":id,"run_id":"run","conversation_id":"conversation",
        "intent":zerus_relay::voice_rejection::intent(&upload["payload"]).unwrap(),"payload_sha256":"a".repeat(64),"generation":1,
        "retry_token":"33333333-3333-4333-8333-333333333333","code":"route_unavailable","detail":"The recording was not accepted."});
    let refused = json!({"schema":1,"rejection":proof});
    let argv = vec![
        "voice".into(),
        "codex/example/session".into(),
        "--json".into(),
    ];
    connector.native.answer(argv.clone(), refused.clone());
    assert_eq!(
        connector.execute(upload.clone()).await.unwrap(),
        Outcome::Finished(json!({"state":"completed","result":refused,"error":null}))
    );
    assert_eq!(
        connector.execute(upload.clone()).await.unwrap(),
        Outcome::Replayed
    );
    let mut retry = upload.clone();
    let operation = "22222222-2222-4222-8222-222222222222";
    retry["request_id"] = json!(operation);
    retry["payload"]["request_id"] = json!(operation);
    retry["payload"]["message_id"] = json!(id);
    retry["payload"]["rejection"] = proof.clone();
    zerus_relay::protocol::request(&retry).unwrap();
    let accepted = json!({"schema":1,"message":{"id":id,"run_id":"run","conversation_id":"conversation","state":"queued","revision":1}});
    connector.native.answers.borrow_mut().clear();
    connector.native.answer(argv, accepted.clone());
    assert_eq!(
        connector.execute(retry.clone()).await.unwrap(),
        Outcome::Finished(json!({"state":"completed","result":accepted,"error":null}))
    );
    assert_eq!(
        connector.execute(retry.clone()).await.unwrap(),
        Outcome::Replayed
    );
    assert_eq!(connector.native.calls.borrow().len(), 2);
    assert_eq!(
        connector.native.calls.borrow()[1]["payload"]["message_id"],
        id
    );
    assert_eq!(
        connector.native.calls.borrow()[1]["payload"]["request_id"],
        operation
    );
    for field in ["expected_run_id", "expected_conversation_id", "message_id"] {
        let mut wrong = retry.clone();
        wrong["payload"][field] = json!("44444444-4444-4444-8444-444444444444");
        assert!(zerus_relay::protocol::request(&wrong).is_err());
        assert!(zerus_mobile_connector::speech::validate_voice(
            "voice_submit",
            &wrong["payload"],
            operation
        )
        .is_err());
    }
    let mut wrong = retry.clone();
    wrong["payload"]["speech"]["profile_revision"] = json!(8);
    assert!(zerus_relay::protocol::request(&wrong).is_err());
    wrong = retry.clone();
    wrong["payload"]["audio"]["data_base64"] = json!(STANDARD.encode(vec![1u8; 3200]));
    assert!(zerus_relay::protocol::request(&wrong).is_err());
    for field in [
        "request_id",
        "message_id",
        "run_id",
        "conversation_id",
        "payload_sha256",
        "generation",
        "retry_token",
        "state",
    ] {
        let mut wrong = refused.clone();
        wrong["rejection"][field] = json!("wrong");
        assert!(!zerus_mobile_connector::speech::voice_result_valid(
            "voice_submit",
            &wrong,
            &upload["payload"]
        )
        .unwrap());
    }
    assert!(!zerus_mobile_connector::speech::voice_result_valid(
        "voice_submit",
        &json!({"schema":1,"error":"not found"}),
        &upload["payload"]
    )
    .unwrap());
}

#[tokio::test(flavor = "current_thread")]
async fn hook_health_runs_only_the_read_once_and_refuses_scope_or_write_extensions() {
    let harness = harness("hook-health-read");
    let connector = &harness.connector;
    capable(connector);
    let mut flags = connector.flags.get();
    flags.hook_health = true;
    connector.flags.set(flags);
    let id = "11111111-1111-4111-8111-111111111111";
    let mut request = json!({"request_id":id,"operation":"hooks_status","session":"codex/example/session","payload":{"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation"}});
    let response = json!({"schema":1,"provider":"codex","account_id":"native-codex","configuration":{"state":"missing","compatibility":{"state":"unknown"}},"effective":{"state":"unknown","reason":"Not verified"},"trust":{"state":"unknown","reason":"Not verified"},"observed":{"state":"unknown","events":[]},"running_agents_retrofitted":false,"request_id":id,"name":"codex/example/session","run_id":"run","conversation_id":"conversation"});
    connector.native.answer(
        vec![
            "hooks".into(),
            "codex/example/session".into(),
            "--json".into(),
        ],
        response.clone(),
    );
    let Outcome::Finished(receipt) = connector.execute(request.clone()).await.unwrap() else {
        panic!()
    };
    assert_eq!(receipt["state"], "completed");
    assert_eq!(receipt["result"], response);
    assert_eq!(
        connector.execute(request.clone()).await.unwrap(),
        Outcome::Replayed
    );
    assert_eq!(connector.native.calls.borrow().len(), 1);
    let call = connector.native.calls.borrow()[0].clone();
    assert_eq!(call["payload"], request["payload"]);
    assert_eq!(call["options"]["timeout"], 5.0);
    request["request_id"] = json!("22222222-2222-4222-8222-222222222222");
    request["payload"]["request_id"] = request["request_id"].clone();
    request["payload"]["action"] = json!("repair");
    let Outcome::Finished(receipt) = connector.execute(request).await.unwrap() else {
        panic!()
    };
    assert_eq!(receipt["state"], "failed");
    assert_eq!(connector.native.calls.borrow().len(), 1);
}
