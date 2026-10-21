//! The golden vectors in `services/mobile/vectors` against this library:
//! `request_hash` through the codec at a claim batch's limits, the operation
//! tables, and the `relay` column of `validation/*.jsonl`, which
//! `protocol::request` decides. The files are read at run time by path, since
//! tests are never packaged and `src/` includes nothing outside itself. Only the
//! protocol library is used, so `cargo test --no-default-features --test
//! vectors` also checks the connector's build.
//!
//! `ZERUS_BLESS=1 cargo test --test vectors` rewrites only the `relay` column;
//! review its diff, then run `capture.py --check`.
//! New project outcomes are captured separately: run only
//! `project_expected_outcomes_and_scoped_relay_capture` with
//! `ZERUS_PROJECT_RELAY_CAPTURE` naming a new absolute private output file.
//! That output contains actual protocol decisions, after comparison with the
//! explicit expectations below; it never rewrites any vector source file.
use serde_json::{json, Value};
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use zerus_relay::{error::Error, json, protocol};

/// The connector parses a poll with these limits: a claim batch,
/// `{"requests":[request]}` with the request's `gateway_route` and
/// `expires_at`, nests a request two levels deeper and adds fewer than 64
/// values beyond what the relay admits.
const BATCH_DEPTH: usize = 34;
const BATCH_VALUES: usize = 50_064;
/// The largest `max_json_depth` and `max_json_values` relay configuration
/// admits (`config.rs`), with which `POST /v1/requests` parses a submission.
const RELAY_DEPTH: usize = 32;
const RELAY_VALUES: usize = 50_000;
/// The connector's own batch rule; the relay hands out one request per claim.
const BATCH_REQUESTS: usize = 100;

/// Rows of `request_hash.jsonl` and `request_envelopes.jsonl` that the relay
/// codec decides differently from Python's `json` module, and why. The
/// connector refuses the long integer itself, as Python does.
const CODEC_DIFFERENCES: &[(&str, &str)] = &[
    (
        "lone-high-surrogate",
        "serde_json refuses a lone surrogate escape, so the relay never stores one",
    ),
    (
        "lone-low-surrogate",
        "serde_json refuses a lone surrogate escape, so the relay never stores one",
    ),
    (
        "reversed-surrogates",
        "serde_json refuses a lone surrogate escape, so the relay never stores one",
    ),
    (
        "nan-literal",
        "NaN is not JSON: the relay never stores it, and the codec refuses the whole batch",
    ),
    (
        "infinity-literal",
        "Infinity is not JSON: the relay never stores it, and the codec refuses the whole batch",
    ),
    (
        "negative-infinity-literal",
        "-Infinity is not JSON: the relay never stores it, and the codec refuses the whole batch",
    ),
    (
        "request-id-before-nan",
        "NaN is not JSON: the codec refuses the batch before any request_id check",
    ),
    (
        "batch-nan-only-rejects-one",
        "NaN is not JSON: the codec refuses the whole batch, where Python drops one request",
    ),
    (
        "integer-4301-digits",
        "the codec keeps an integer of any length; Python's json refuses more than 4,300 digits",
    ),
];

/// Cases of `validation/*.jsonl` where the relay and the frozen relay-oracle
/// column of the Python reference relay decide differently, and why. Every
/// other case agrees on the decision and the status.
///
/// DEL is a control character in every identity (owner decision, zerus-3rqx.6):
/// `protocol::text` refuses it with the C0 controls, as the connector's
/// identity checks do; the reference relay's `text()` refused only C0 controls.
/// The Python connector itself accepts DEL in a send's `expected_run_id` and
/// never reads `computer_id`, which the relay removes before a claim.
const ORACLE_DIFFERENCES: &[(&str, &str)] = &[
    ("session/delete", "the relay refuses DEL in session"),
    ("computer-id/delete", "the relay refuses DEL in computer_id"),
    (
        "payload/run-delete",
        "the relay refuses DEL in expected_run_id",
    ),
];

/// Independently reviewable schema expectations, not executed/captured relay columns.
/// Zero denotes acceptance; 400 denotes the exact BAD request status. In particular
/// an empty conversation is valid for a first-message run, and only the read accepts
/// a cursor. Launch/directory/whole-organization authority is never accepted here.
const PROJECT_EXPECTED: &[(&str, u16)] = &[
    ("valid/project_assign", 0),
    ("valid/project_assignment", 0),
    ("project_assign/forbidden-directory", 400),
    ("project_assign/forbidden-add_folder", 400),
    ("project_assign/forbidden-project_folder_id", 400),
    ("project_assign/forbidden-archive_id", 400),
    ("project_assign/forbidden-agent_id", 400),
    ("project_assign/forbidden-organization", 400),
    ("project_assign/machine-nil", 400),
    ("project_assign/empty-conversation", 0),
    ("project_assignment/forbidden-directory", 400),
    ("project_assignment/forbidden-add_folder", 400),
    ("project_assignment/forbidden-project_folder_id", 400),
    ("project_assignment/forbidden-archive_id", 400),
    ("project_assignment/forbidden-agent_id", 400),
    ("project_assignment/forbidden-organization", 400),
    ("project_assignment/machine-nil", 400),
    ("project_assignment/empty-conversation", 0),
    ("project_assignment/cursor", 0),
    ("project_assignment/mutation-field", 400),
    ("project_assign/short-proof", 400),
    ("project_assign/cursor", 400),
];

fn vectors() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../vectors")
}

/// One vector record. The files hold the serde magic key, so the relay codec
/// reads them.
fn record(line: &str) -> Value {
    json::parse(line.as_bytes(), 64, 1_000_000).unwrap()
}

fn records(name: &str) -> Vec<Value> {
    std::fs::read_to_string(vectors().join(name))
        .unwrap()
        .lines()
        .map(record)
        .collect()
}

fn raw(row: &Value) -> Vec<u8> {
    match row.get("raw_parts") {
        Some(parts) => parts
            .as_array()
            .unwrap()
            .iter()
            .flat_map(|part| {
                part[0]
                    .as_str()
                    .unwrap()
                    .repeat(part[1].as_u64().unwrap() as usize)
                    .into_bytes()
            })
            .collect(),
        None => row["raw"].as_str().unwrap().as_bytes().to_vec(),
    }
}

fn listed(table: &[(&'static str, &str)]) -> BTreeSet<&'static str> {
    table.iter().map(|(id, _)| *id).collect()
}

/// One request's expected outcome; returns whether it was hashed.
fn check(id: &str, request: &Value, expect: &Value) -> bool {
    if let Some(hash) = expect["request_hash"].as_str() {
        let canonical = json::canonical(request).unwrap_or_else(|error| panic!("{id}: {error}"));
        if let Some(text) = expect["canonical"].as_str() {
            assert_eq!(canonical, text, "{id}");
        }
        assert_eq!(
            canonical.len() as u64,
            expect["canonical_bytes"].as_u64().unwrap(),
            "{id}"
        );
        assert_eq!(json::digest(&canonical), hash, "{id}");
        return true;
    }
    match expect["error"].as_str().unwrap() {
        // Python reads 1e400 as infinity and cannot hash it; neither can the codec.
        "invalid native request JSON" => assert!(json::canonical(request).is_err(), "{id}"),
        // The connector's own checks before the hash; the relay's agree.
        "request_id must be a canonical UUID" => {
            assert!(
                protocol::uuid(&request["request_id"], false).is_err(),
                "{id}"
            )
        }
        "invalid request" => assert!(!request.is_object(), "{id}"),
        error => panic!("{id}: unreviewed rejection {error:?}"),
    }
    false
}

#[test]
fn request_hashes_match_python_through_the_batch_codec() {
    let differences = listed(CODEC_DIFFERENCES);
    let mut seen = BTreeSet::new();
    let mut hashed = 0;
    for name in ["request_hash.jsonl", "request_envelopes.jsonl"] {
        for row in records(name) {
            let id = row["id"].as_str().unwrap().to_owned();
            assert!(seen.insert(id.clone()), "{id}: duplicate row");
            let raw = raw(&row);
            // A lone request is delivered as the only member of its batch.
            let body = if row["envelope"] == "batch" {
                raw
            } else {
                [&b"{\"requests\":["[..], &raw, b"]}"].concat()
            };
            let parsed = json::parse(&body, BATCH_DEPTH, BATCH_VALUES);
            let expect = &row["expect"];
            if differences.contains(id.as_str()) {
                if id == "integer-4301-digits" {
                    let batch = parsed.unwrap();
                    let canonical = json::canonical(&batch["requests"][0]).unwrap();
                    assert!(canonical.contains(&format!("9{}7", "8".repeat(4299))));
                } else {
                    assert_eq!(parsed.unwrap_err().0, Error::BAD.0, "{id}");
                }
                continue;
            }
            if id == "relay-maximum-in-batch" {
                // A submission at the relay's limits is two levels and one value
                // beyond them in its batch.
                json::parse(&body, BATCH_DEPTH, 50_001).unwrap();
                for (depth, values) in [(BATCH_DEPTH - 1, BATCH_VALUES), (BATCH_DEPTH, 50_000)] {
                    assert_eq!(
                        json::parse(&body, depth, values).unwrap_err().0,
                        Error::LARGE.0
                    );
                }
                assert_eq!(
                    json::parse(&body, RELAY_DEPTH, RELAY_VALUES).unwrap_err().0,
                    Error::LARGE.0
                );
            }
            match (expect["rejected"].as_str(), expect["error"].as_str()) {
                (Some("parse"), _) => {
                    assert!(parsed.is_err(), "{id}");
                    continue;
                }
                (Some("batch"), Some(error)) => {
                    let requests = &parsed.unwrap()["requests"];
                    match error {
                        "relay returned invalid request list" => {
                            assert!(!requests.is_array(), "{id}")
                        }
                        "relay request list exceeds size limit" => {
                            assert!(requests.as_array().unwrap().len() > BATCH_REQUESTS, "{id}")
                        }
                        error => panic!("{id}: unreviewed rejection {error:?}"),
                    }
                    continue;
                }
                _ => {}
            }
            let batch = parsed.unwrap_or_else(|error| panic!("{id}: {error}"));
            let requests = batch["requests"].as_array().unwrap();
            let outcomes: Vec<&Value> = match expect.get("requests") {
                Some(outcomes) => outcomes.as_array().unwrap().iter().collect(),
                None => vec![expect],
            };
            assert_eq!(requests.len(), outcomes.len(), "{id}");
            for (request, expect) in requests.iter().zip(outcomes) {
                hashed += usize::from(check(&id, request, expect));
            }
        }
    }
    let missing: Vec<_> = differences
        .iter()
        .filter(|id| !seen.contains(**id))
        .collect();
    assert!(
        missing.is_empty(),
        "listed rows without a vector: {missing:?}"
    );
    // 56 requests of request_hash.jsonl and 63 of request_envelopes.jsonl.
    assert_eq!(hashed, 119); // Includes the shared session notification envelope.
}

/// `services/mobile/tests/rust-only-operations.json`: each operation only
/// the Rust connector executes, and whether it is a read.
fn rust_only() -> Vec<(String, bool)> {
    let table = json::parse(
        &std::fs::read(vectors().join("../tests/rust-only-operations.json")).unwrap(),
        64,
        1_000_000,
    )
    .unwrap();
    let rows: Vec<(String, bool)> = table["operations"]
        .as_array()
        .unwrap()
        .iter()
        .map(|row| {
            assert!(
                row["class"] == "read" || row["class"] == "mutation",
                "{row}"
            );
            (
                row["operation"].as_str().unwrap().to_owned(),
                row["class"] == "read",
            )
        })
        .collect();
    assert!(!rows.is_empty());
    rows
}

fn sorted(items: &[&str]) -> Vec<String> {
    let mut items: Vec<String> = items.iter().map(|item| (*item).to_owned()).collect();
    items.sort();
    items
}

/// The operations an SQL `IN` list names, in order.
fn sql_list(text: &str) -> Vec<String> {
    text.trim_matches(['(', ')'])
        .split(',')
        .map(|item| item.trim_matches('\'').to_owned())
        .collect()
}

#[test]
fn operation_tables_match_the_python_connector() {
    let operations = json::parse(
        &std::fs::read(vectors().join("operations.json")).unwrap(),
        64,
        1_000_000,
    )
    .unwrap();
    let set = |names: &[&str]| -> Vec<String> {
        let mut items: Vec<String> = names
            .iter()
            .flat_map(|name| operations["sets"][*name].as_array().unwrap())
            .map(|item| item.as_str().unwrap().to_owned())
            .collect();
        items.sort();
        items
    };
    let sql = |names: &[&str]| -> Vec<String> {
        let mut items: Vec<String> = names
            .iter()
            .flat_map(|name| sql_list(operations["sql"][*name].as_str().unwrap()))
            .collect();
        items.sort();
        items
    };
    // Operations only the Rust connector executes: the Python connector,
    // whose tables these vectors freeze, never lists them. Each is a read or
    // a mutation here as its row says.
    let rust_only = rust_only();
    let python: Vec<&str> = protocol::OPERATIONS
        .iter()
        .copied()
        .filter(|operation| !rust_only.iter().any(|(name, _)| name == operation))
        .collect();
    assert_eq!(python.len() + rust_only.len(), protocol::OPERATIONS.len());
    assert_eq!(sorted(&python), set(&["OPERATIONS"]));
    for (operation, read) in &rust_only {
        assert_eq!(protocol::READS.contains(&operation.as_str()), *read);
        for name in ["OPERATIONS", "READ_OPERATIONS", "MUTATIONS"] {
            assert!(!set(&[name]).contains(operation), "{name} {operation}");
        }
    }
    let python_reads: Vec<&str> = protocol::READS
        .iter()
        .copied()
        .filter(|operation| !rust_only.iter().any(|(name, _)| name == operation))
        .collect();
    assert_eq!(
        python_reads.len() + rust_only.iter().filter(|(_, read)| *read).count(),
        protocol::READS.len()
    );
    assert_eq!(sorted(&python_reads), set(&["READ_OPERATIONS"]));
    assert_eq!(sorted(protocol::LAUNCH), set(&["LAUNCH_OPERATIONS"]));
    assert_eq!(sorted(protocol::COMPUTER), set(&["COMPUTER_OPERATIONS"]));
    assert_eq!(
        sorted(protocol::SPEECH),
        set(&["speech.OPERATIONS", "speech_config.OPERATIONS"])
    );
    for operation in ["file_read", "attachment_read"] {
        assert!(protocol::OPERATIONS.contains(&operation));
        assert!(protocol::READS.contains(&operation));
    }
    assert_eq!(
        json!(protocol::MOBILE_DELIVERY),
        operations["sequences"]["MOBILE_DELIVERY"]
    );
    // Baseline vectors predate optional recording video (zerus-eo7j). Video is
    // advertised only by native-capable connectors; presence alerts are relay-owned.
    // The scoped project feature is additive; the legacy FEATURES vector stays frozen.
    assert!(protocol::FEATURES.contains(&"session_project_assignment_v1"));
    assert!(!set(&["FEATURES"])
        .iter()
        .any(|feature| feature == "session_project_assignment_v1"));
    let features: Vec<&str> = protocol::FEATURES
        .iter()
        .copied()
        .filter(|feature| {
            ![
                "opencode_launch_effort",
                "presence_alerts",
                "voice_video",
                "clear_context_dismiss",
                "session_project_assignment_v1",
            ]
            .contains(feature)
        })
        .collect();
    assert_eq!(sorted(&features), set(&["FEATURES"]));
    // The relay's index predicates are its own text over the same operations.
    let mut reads = sql_list(protocol::READ_SQL);
    reads.sort();
    assert_eq!(reads, sql(&["READ_SQL"]));
    let mut speech_reads = sql_list(protocol::SPEECH_READ_SQL);
    speech_reads.sort();
    assert_eq!(speech_reads, sql(&["speech.READ_SQL", "voice.READ_SQL"]));
    let constants = &operations["constants"];
    let limits = protocol::capabilities(Vec::new());
    assert_eq!(limits["attachment_limits"], constants["attachments.LIMITS"]);
    assert_eq!(json!(protocol::REQUEST_MAX), constants["MAX_PAYLOAD"]);
    assert_eq!(
        json!(protocol::ATTACHMENT_FILE_MAX),
        constants["attachments.LIMITS"]["max_file_bytes"]
    );
    assert_eq!(
        json!(protocol::ATTACHMENT_CHUNK_MAX),
        constants["attachments.MAX_READ_BYTES"]
    );
    assert_eq!(
        json!(protocol::SPEECH_AUDIO_MAX),
        constants["speech.MAX_AUDIO_BYTES"]
    );
    for (value, name) in [
        (
            protocol::ATTACHMENT_RESULT_TTL,
            "context.ATTACHMENT_RESULT_TTL",
        ),
        (
            protocol::ATTACHMENT_READ_GRACE,
            "context.ATTACHMENT_READ_GRACE",
        ),
    ] {
        assert_eq!(Some(value), constants[name].as_f64(), "{name}");
    }
}

/// The relay's decision on one submission, as `POST /v1/requests` makes it:
/// the codec at the largest configurable limits, then `protocol::request`.
fn relay_decision(submission: &Value) -> Value {
    let raw = json::canonical(submission).unwrap();
    match json::parse(raw.as_bytes(), RELAY_DEPTH, RELAY_VALUES)
        .and_then(|value| protocol::request(&value))
    {
        Ok(()) => json!({"decision": "accept"}),
        Err(Error(status, _)) => json!({"decision": "reject", "status": status.as_u16()}),
    }
}

fn verdict(decision: &Value) -> (&Value, &Value) {
    (&decision["decision"], &decision["status"])
}

#[test]
fn project_expected_outcomes_and_scoped_relay_capture() {
    let input = std::fs::read_to_string(vectors().join("validation/project.jsonl")).unwrap();
    let expected_ids: BTreeSet<_> = PROJECT_EXPECTED.iter().map(|(id, _)| *id).collect();
    assert_eq!(expected_ids.len(), 22);
    assert_eq!(PROJECT_EXPECTED.len(), expected_ids.len());
    assert_eq!(
        PROJECT_EXPECTED
            .iter()
            .filter(|(_, status)| *status == 0)
            .count(),
        5
    );
    assert!(PROJECT_EXPECTED
        .iter()
        .all(|(_, status)| [0, 400].contains(status)));
    let mut ids = BTreeSet::new();
    let mut contexts = 0;
    let mut captured = String::with_capacity(input.len() + 4096);
    for (index, line) in input.lines().enumerate() {
        let mut row = record(line);
        assert_eq!(json::canonical(&row).unwrap(), line, "project line {index}");
        if row["kind"] == "context" {
            assert_eq!(index, 0, "only the first row may be a context");
            contexts += 1;
        } else {
            assert_eq!(row["kind"], "case");
            let id = row["id"].as_str().unwrap().to_owned();
            assert!(ids.insert(id.clone()), "duplicate project case {id}");
            let status = PROJECT_EXPECTED
                .iter()
                .find(|(key, _)| *key == id)
                .unwrap_or_else(|| panic!("unreviewed project case {id}"))
                .1;
            let expected = if status == 0 {
                json!({"decision":"accept"})
            } else {
                json!({"decision":"reject","status":status})
            };
            let actual = relay_decision(&row["submission"]);
            assert_eq!(actual, expected, "project case {id}");
            assert_eq!(
                verdict(&row["oracle"]),
                verdict(&expected),
                "project oracle {id}"
            );
            // This is filled from the real Rust handler, never from PROJECT_EXPECTED.
            row["relay"] = actual;
        }
        captured.push_str(&json::canonical(&row).unwrap());
        captured.push('\n');
    }
    assert_eq!(contexts, 1);
    assert_eq!(
        ids.iter().map(String::as_str).collect::<BTreeSet<_>>(),
        expected_ids
    );
    if let Some(output) = std::env::var_os("ZERUS_PROJECT_RELAY_CAPTURE") {
        use std::io::Write;
        let output = PathBuf::from(output);
        assert!(
            output.is_absolute(),
            "capture output must be an absolute private path"
        );
        // Never replace an old capture or a checked-in vector, including a symlink.
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(output)
            .unwrap();
        file.write_all(captured.as_bytes()).unwrap();
        file.flush().unwrap();
        file.sync_all().unwrap();
    }
}

#[test]
fn validation_relay_column_is_protocol_request() {
    let bless = std::env::var_os("ZERUS_BLESS").is_some_and(|value| value == "1");
    let mut paths: Vec<PathBuf> = std::fs::read_dir(vectors().join("validation"))
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "jsonl")
        })
        .collect();
    paths.sort();
    assert_eq!(
        paths
            .iter()
            .map(|path| path.file_name().unwrap().to_str().unwrap())
            .collect::<Vec<_>>(),
        [
            "control.jsonl",
            "conversation.jsonl",
            "envelope.jsonl",
            "launch.jsonl",
            "project.jsonl",
            "speech.jsonl"
        ]
    );
    let mut stale = Vec::new();
    let mut differing = BTreeSet::new();
    let mut ids = BTreeSet::new();
    for path in paths {
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        let text = std::fs::read_to_string(&path).unwrap();
        let mut blessed = String::with_capacity(text.len() + 4096);
        for line in text.lines() {
            let mut row = record(line);
            // Every line is canonical, so a rewrite changes only the relay column.
            assert_eq!(json::canonical(&row).unwrap(), line, "{name}");
            if row["kind"] == "case" {
                let id = row["id"].as_str().unwrap().to_owned();
                assert!(ids.insert(id.clone()), "{id}: duplicate case");
                let relay = relay_decision(&row["submission"]);
                if row.get("relay") != Some(&relay) {
                    stale.push(format!("{name} {id}: protocol::request gives {relay}"));
                }
                if verdict(&row["oracle"]) != verdict(&relay) {
                    differing.insert(id);
                }
                row["relay"] = relay;
            }
            blessed.push_str(&json::canonical(&row).unwrap());
            blessed.push('\n');
        }
        if bless && blessed != text {
            std::fs::write(&path, blessed).unwrap();
        }
    }
    assert!(ids.len() > 200);
    assert!(
        bless || stale.is_empty(),
        "the relay column is stale; regenerate it with ZERUS_BLESS=1 cargo test --test vectors \
         and review the diff:\n{}",
        stale.join("\n")
    );
    let listed = listed(ORACLE_DIFFERENCES);
    let missing: Vec<_> = listed.iter().filter(|id| !ids.contains(**id)).collect();
    assert!(
        missing.is_empty(),
        "listed cases without a vector: {missing:?}"
    );
    let unlisted: Vec<_> = differing
        .iter()
        .filter(|id| !listed.contains(id.as_str()))
        .collect();
    let agreeing: Vec<_> = listed
        .iter()
        .filter(|id| !differing.contains(**id))
        .collect();
    assert!(
        unlisted.is_empty() && agreeing.is_empty(),
        "relay and frozen oracle decisions: unlisted differences {unlisted:?}, listed but agreeing {agreeing:?}"
    );
}
