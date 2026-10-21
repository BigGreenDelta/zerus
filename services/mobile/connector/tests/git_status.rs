#![cfg(unix)]
//! Synthetic checkout reads: native projection, exact argv and durable read identity.
mod common;

use common::{Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::RefCell;
use zerus_mobile_connector::{
    capabilities::Flags,
    execute::Outcome,
    journal::{is_mutation, Journal, JOURNAL, MUTATION_SECONDS, READ_CAP, READ_SECONDS},
    native::{Call, Native, NativeError, NativeResult, Output},
    runtime::{Connector, Settings},
};

const REQUEST: &str = "00000000-0000-4000-8000-000000000002";
const MARKER: &str = "Mobile Git status ABI: git-status-v1";
const PATH: &str = "/fixture/link with spaces/$(literal);--flag";

fn request() -> Value {
    json!({"request_id":REQUEST,"computer_id":"fixture-computer","operation":"git_status",
        "session":"","payload":{"request_id":REQUEST,"path":PATH}})
}

fn snapshot() -> Value {
    json!({"state":"ok","path":"/fixture/canonical","remote_state":"verified",
        "head":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","branch":"main","upstream":"origin/main",
        "root":"/fixture/canonical","common_dir":"/fixture/canonical/.git","remote":"origin",
        "remote_ref":"refs/heads/main","unborn":false,"detached":false,"changed_files":2,
        "conflicts":0,"ahead":null,"behind":3,"sampled_at":EPOCH,"remote_checked_at":EPOCH-1.0,
        "remote_age_seconds":1.0})
}

struct Fake {
    calls: RefCell<Vec<Call>>,
    reply: Value,
}

impl Native for Fake {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call);
        Ok(Output::Json(self.reply.clone()))
    }
}

fn connector(scratch: &Scratch, reply: Value) -> Connector<Fake> {
    Connector::new(
        Fake {
            calls: RefCell::new(vec![]),
            reply,
        },
        Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap(),
        Settings {
            interval: 5.0,
            binding: "fixture".into(),
            default_timeout: 45.0,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    )
}

#[test]
fn exact_marker_offers_rust_only_read_without_changing_python_operations() {
    for (help, expected) in [
        ("", false),
        ("git-status --path PATH --json", false),
        ("Mobile Git status ABI: git-status-v10", false),
        (
            "Mobile Git status ABI: git-status-v1 explanatory suffix",
            false,
        ),
        ("prefix Mobile Git status ABI: git-status-v1", false),
        ("  Mobile Git status ABI: git-status-v1  \n", true),
        (MARKER, true),
    ] {
        let flags = Flags::from_help(help);
        assert_eq!(
            flags.advertised(false)["operations"]
                .as_array()
                .unwrap()
                .iter()
                .any(|v| v == "git_status"),
            expected
        );
        assert!(!flags.python_operations(true).contains("git_status"));
    }
    assert!(!is_mutation("git_status"));
}

#[tokio::test(flavor = "current_thread")]
async fn native_projection_uses_exact_original_path_no_stdin_and_deduplicates() {
    let scratch = Scratch::new("git-status-native");
    let mut native = snapshot();
    native["files"] = json!(["private-file"]);
    native["remote_url"] = json!("https://private.example.test/repo");
    let node = connector(&scratch, native);
    node.flags.set(Flags::from_help(MARKER));
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "completed", "{response}");
    assert_eq!(
        response["result"],
        json!({"schema":1,"request_id":REQUEST,"requested_path":PATH,"snapshot":snapshot()})
    );
    assert_eq!(node.execute(request()).await.unwrap(), Outcome::Replayed);
    let calls = node.native.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].argv, ["git-status", "--path", PATH, "--json"]);
    assert_eq!(calls[0].payload, None);
    assert_eq!(calls[0].timeout, Some(5.0));
    assert_eq!(calls[0].max_output_bytes, Some(32 * 1024));
}

#[tokio::test(flavor = "current_thread")]
async fn missing_marker_or_invalid_target_and_payload_never_call_native() {
    let scratch = Scratch::new("git-status-rejected");
    let node = connector(&scratch, snapshot());
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(node.native.calls.borrow().is_empty());
    node.flags.set(Flags::from_help(MARKER));
    let mut cases = Vec::new();
    for session in [json!("codex/fixture"), json!(null), json!(false)] {
        let mut bad = request();
        bad["session"] = session;
        cases.push(bad);
    }
    for path in [
        json!("relative"),
        json!("~"),
        json!(""),
        json!(false),
        json!(5),
        json!("/bad\npath"),
        json!("/bad\u{007f}path"),
        json!(format!("/{}", "é".repeat(2048))),
    ] {
        let mut bad = request();
        bad["payload"]["path"] = path;
        cases.push(bad);
    }
    for key in [
        "expected_run_id",
        "expected_conversation_id",
        "agent_id",
        "archive_id",
        "extra",
    ] {
        let mut bad = request();
        bad["payload"][key] = json!("unexpected");
        cases.push(bad);
    }
    let mut mismatch = request();
    mismatch["payload"]["request_id"] = json!("wrong");
    cases.push(mismatch);
    let mut missing = request();
    missing["payload"]
        .as_object_mut()
        .unwrap()
        .remove("request_id");
    cases.push(missing);
    for (index, mut bad) in cases.into_iter().enumerate() {
        let id = format!("00000000-0000-4000-8000-{:012}", index + 10);
        bad["request_id"] = json!(id);
        if bad["payload"]["request_id"] == REQUEST {
            bad["payload"]["request_id"] = json!(id);
        }
        let Outcome::Finished(response) = node.execute(bad).await.unwrap() else {
            panic!()
        };
        assert_eq!(response["state"], "failed", "case {index}: {response}");
    }
    assert!(node.native.calls.borrow().is_empty());
}

#[test]
fn partial_snapshots_remain_partial_and_known_native_states_are_preserved() {
    use zerus_mobile_connector::git_status::project;
    for state in [
        "ok",
        "unavailable",
        "folder_unavailable",
        "timeout",
        "git_unavailable",
        "metadata_error",
        "too_large",
        "not_repo",
        "changed_during_check",
    ] {
        for remote in [
            "unknown",
            "detached",
            "no_upstream",
            "local_upstream",
            "unavailable",
            "verified",
            "changed",
            "missing",
        ] {
            let native = json!({"state":state,"path":"/canonical/elsewhere","remote_state":remote});
            let result = project(&native, REQUEST, PATH).unwrap();
            assert_eq!(result["snapshot"], native);
            assert_eq!(result["requested_path"], PATH);
            assert!(result["snapshot"].get("changed_files").is_none());
        }
    }
}

#[test]
fn path_and_projected_utf8_byte_limits_are_exact() {
    use zerus_mobile_connector::git_status::{project, validate, MAX_BYTES};
    for path in [
        format!("/{}", "x".repeat(4095)),
        format!("/{}x", "é".repeat(2047)),
    ] {
        assert_eq!(path.len(), 4096);
        assert!(validate(&json!({"request_id":REQUEST,"path":path}), REQUEST).is_ok());
    }
    for path in [
        format!("/{}", "x".repeat(4096)),
        "/bad\u{0085}control".into(),
    ] {
        assert!(validate(&json!({"request_id":REQUEST,"path":path}), REQUEST).is_err());
    }
    let mut native = json!({"state":"ok","path":"/canonical","remote_state":"unknown","branch":""});
    let size = zerus_relay::json::canonical(&project(&native, REQUEST, PATH).unwrap())
        .unwrap()
        .len();
    native["branch"] = json!("x".repeat(MAX_BYTES - size));
    assert_eq!(
        zerus_relay::json::canonical(&project(&native, REQUEST, PATH).unwrap())
            .unwrap()
            .len(),
        MAX_BYTES
    );
    native["branch"] = json!("x".repeat(MAX_BYTES - size + 1));
    assert!(project(&native, REQUEST, PATH).is_err());
    // The existing API-v1 canonical codec emits six ASCII bytes for \u00e9.
    native["branch"] = json!("é".repeat((MAX_BYTES - size) / 6));
    assert!(project(&native, REQUEST, PATH).is_ok());
    native["branch"] = json!("é".repeat((MAX_BYTES - size) / 6 + 1));
    assert!(project(&native, REQUEST, PATH).is_err());
}

#[test]
fn malformed_known_fields_and_oversized_projection_are_refused() {
    use zerus_mobile_connector::git_status::{project, MAX_BYTES};
    for (key, bad) in [
        ("state", json!("invented")),
        ("remote_state", json!("synced")),
        ("path", json!(false)),
        ("head", json!(null)),
        ("branch", json!(5)),
        ("upstream", json!([])),
        ("root", json!({})),
        ("common_dir", json!(false)),
        ("remote", json!(null)),
        ("remote_ref", json!(0)),
        ("unborn", json!(0)),
        ("detached", json!("false")),
        ("changed_files", json!(true)),
        ("changed_files", json!(1.0)),
        ("changed_files", json!(-1)),
        ("conflicts", json!(null)),
        ("ahead", json!(false)),
        ("behind", json!(-1)),
        ("sampled_at", json!(true)),
        ("remote_checked_at", json!(-0.1)),
        ("remote_age_seconds", json!("0")),
    ] {
        let mut native = snapshot();
        native[key] = bad;
        assert!(project(&native, REQUEST, PATH).is_err(), "{key}: {native}");
    }
    for required in ["state", "path", "remote_state"] {
        let mut native = snapshot();
        native.as_object_mut().unwrap().remove(required);
        assert!(project(&native, REQUEST, PATH).is_err());
    }
    for bad in [Value::Null, json!([]), json!("wrong")] {
        assert!(project(&bad, REQUEST, PATH).is_err());
    }
    for key in ["sampled_at", "remote_checked_at", "remote_age_seconds"] {
        let mut native = snapshot();
        native[key] = serde_json::from_str("1e999").unwrap();
        assert!(project(&native, REQUEST, PATH).is_err());
    }
    let mut huge = snapshot();
    huge["branch"] = json!("x".repeat(MAX_BYTES));
    assert!(project(&huge, REQUEST, PATH).is_err());
    let mut unknown = snapshot();
    unknown["private"] = json!("x".repeat(MAX_BYTES * 2));
    assert_eq!(
        project(&unknown, REQUEST, PATH).unwrap()["snapshot"],
        snapshot()
    );
    let mut large = snapshot();
    large["changed_files"] = serde_json::from_str("18446744073709551616").unwrap();
    assert!(project(&large, REQUEST, PATH).is_ok());
}

#[tokio::test(flavor = "current_thread")]
async fn invalid_native_projection_journals_failed_and_identity_conflict_never_executes() {
    let scratch = Scratch::new("git-status-invalid-native");
    let mut wrong = snapshot();
    wrong["changed_files"] = json!(true);
    let node = connector(&scratch, wrong);
    node.flags.set(Flags::from_help(MARKER));
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(response["result"].is_null());
    assert_eq!(node.execute(request()).await.unwrap(), Outcome::Replayed);
    let mut conflict = request();
    conflict["payload"]["path"] = json!("/fixture/other");
    assert!(matches!(
        node.execute(conflict).await.unwrap(),
        Outcome::Rejected(_)
    ));
    assert_eq!(node.native.calls.borrow().len(), 1);
}

#[tokio::test(flavor = "current_thread")]
async fn native_timeout_stays_failed_and_never_reexecutes_the_same_identity() {
    struct TimedOut(RefCell<Vec<Call>>);
    impl Native for TimedOut {
        async fn call(&self, call: Call) -> NativeResult {
            self.0.borrow_mut().push(call);
            Err(NativeError::Failed(
                zerus_mobile_connector::native::TIMED_OUT,
            ))
        }
    }
    let scratch = Scratch::new("git-status-timeout");
    let node = Connector::new(
        TimedOut(RefCell::new(Vec::new())),
        Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap(),
        Settings {
            interval: 5.0,
            binding: "fixture".into(),
            default_timeout: 45.0,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    );
    node.flags.set(Flags::from_help(MARKER));
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(response["result"].is_null());
    assert_eq!(response["error"], zerus_mobile_connector::native::TIMED_OUT);
    assert_eq!(node.execute(request()).await.unwrap(), Outcome::Replayed);
    assert_eq!(node.native.0.borrow().len(), 1);
}

#[tokio::test(flavor = "current_thread")]
async fn existing_machine_guard_wraps_only_the_exact_git_argv_and_empty_payload() {
    let scratch = Scratch::new("git-status-machine-guard");
    let node = connector(&scratch, snapshot());
    node.flags.set(Flags::from_help(MARKER));
    let machine = "00000000-0000-4000-8000-000000000001";
    let outcome = Connector::<Fake>::guarded(machine.into(), node.execute(request()))
        .await
        .unwrap();
    let Outcome::Finished(response) = outcome else {
        panic!()
    };
    assert_eq!(response["state"], "completed");
    let calls = node.native.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].argv, ["swarm", "__mobile-peer-local", "--json"]);
    assert_eq!(
        calls[0].payload,
        Some(
            json!({"schema":1,"target_machine_id":machine,"argv":["git-status","--path",PATH,"--json"],"payload":null})
        )
    );
    assert_eq!(calls[0].timeout, Some(5.0));
    assert_eq!(calls[0].max_output_bytes, Some(32 * 1024));
}

fn seed(scratch: &Scratch, operation: &str, count: i64, prefix: &str) {
    let mut db = rusqlite::Connection::open(scratch.path("state").join(JOURNAL)).unwrap();
    let tx = db.transaction().unwrap();
    {
        let mut insert=tx.prepare("INSERT INTO requests(request_id,operation,state,created_at,updated_at) VALUES(?,?,'running',?,?)").unwrap();
        for n in 0..count {
            insert
                .execute(rusqlite::params![
                    format!("{prefix}-{n}"),
                    operation,
                    EPOCH,
                    EPOCH
                ])
                .unwrap();
        }
    }
    tx.commit().unwrap();
}

#[test]
fn git_and_legacy_reads_share_the_exact_read_cap() {
    let scratch = Scratch::new("git-status-read-cap");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    seed(&scratch, "inspect", READ_CAP - 1, "legacy");
    assert!(journal
        .claim("git-final", "git_status", Some("hash"), EPOCH)
        .unwrap());
    for op in ["inspect", "git_status"] {
        assert!(journal.claim(op, op, Some("hash"), EPOCH).is_err());
    }
    assert!(journal
        .claim("send-independent", "send", Some("hash"), EPOCH)
        .unwrap());
}

#[test]
fn git_reads_do_not_consume_mutation_quota() {
    let scratch = Scratch::new("git-status-mutation-cap");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    seed(&scratch, "send", 99_999, "mutation");
    seed(&scratch, "git_status", 1, "git");
    assert!(journal
        .claim("mutation-final", "send", Some("hash"), EPOCH)
        .unwrap());
    assert!(journal
        .claim("mutation-excess", "send", Some("hash"), EPOCH)
        .is_err());
}

#[test]
fn rust_read_count_selector_matches_every_connector_only_read_row() {
    use zerus_mobile_connector::{
        capabilities::{Class, CONNECTOR_ONLY},
        journal::GIT_STATUS_COUNT_SQL,
    };
    let db = rusqlite::Connection::open_in_memory().unwrap();
    db.execute_batch("CREATE TABLE requests(operation TEXT)")
        .unwrap();
    for operation in zerus_relay::protocol::OPERATIONS {
        db.execute("DELETE FROM requests", []).unwrap();
        db.execute("INSERT INTO requests VALUES(?)", [operation])
            .unwrap();
        let count: i64 = db
            .query_row(GIT_STATUS_COUNT_SQL, [], |row| row.get(0))
            .unwrap();
        assert_eq!(
            count == 1,
            CONNECTOR_ONLY
                .iter()
                .any(|row| row.operation == *operation && row.class == Class::Read),
            "{operation}"
        );
    }
}

#[test]
fn git_retention_preserves_boundaries_running_pending_and_mutation_tombstones() {
    let scratch = Scratch::new("git-status-retention");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    for (id, state, delivered) in [
        ("completed", "completed", true),
        ("failed", "failed", true),
        ("uncertain", "uncertain", true),
        ("pending", "completed", false),
        ("running", "running", true),
        ("mutation", "completed", true),
    ] {
        let op = if id == "mutation" {
            "send"
        } else {
            "git_status"
        };
        assert!(journal.claim(id, op, Some("hash"), EPOCH).unwrap());
        if state != "running" {
            journal
                .finish(
                    id,
                    &json!({"state":state,"result":{"private":"fixture"},"error":null}),
                    EPOCH,
                )
                .unwrap();
        }
        if delivered {
            journal.delivered(id, EPOCH).unwrap();
        }
    }
    let db = rusqlite::Connection::open(scratch.path("state").join(JOURNAL)).unwrap();
    let exists = |id: &str| {
        db.query_row(
            "SELECT count(*) FROM requests WHERE request_id=?",
            [id],
            |r| r.get::<_, i64>(0),
        )
        .unwrap()
            == 1
    };
    journal.prune(EPOCH + READ_SECONDS).unwrap();
    assert!(exists("completed"));
    journal.prune(EPOCH + READ_SECONDS + 0.1).unwrap();
    for id in ["completed", "failed", "uncertain"] {
        assert!(!exists(id));
    }
    for id in ["pending", "running", "mutation"] {
        assert!(exists(id));
    }
    journal.prune(EPOCH + MUTATION_SECONDS + 1.0).unwrap();
    for id in ["pending", "running", "mutation"] {
        assert!(exists(id));
    }
    assert_eq!(
        db.query_row(
            "SELECT response_expired FROM requests WHERE request_id='mutation'",
            [],
            |r| r.get::<_, i64>(0)
        )
        .unwrap(),
        1
    );
}

#[test]
fn git_prune_is_indexed_bounded_and_deterministic() {
    use zerus_mobile_connector::journal::GIT_STATUS_PRUNE_SQL;
    let scratch = Scratch::new("git-status-bounded-prune");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    seed(&scratch, "git_status", READ_CAP + 1, "git");
    let db = rusqlite::Connection::open(scratch.path("state").join(JOURNAL)).unwrap();
    db.execute(
        "UPDATE requests SET state='completed',delivered=1,delivered_at=?",
        [EPOCH],
    )
    .unwrap();
    let plan: Vec<String> = db
        .prepare(&format!("EXPLAIN QUERY PLAN {GIT_STATUS_PRUNE_SQL}"))
        .unwrap()
        .query_map([EPOCH], |r| r.get(3))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert!(
        plan.iter().any(|line| line.contains("journal_operation")),
        "{plan:?}"
    );
    journal.prune(EPOCH + READ_SECONDS + 1.0).unwrap();
    assert_eq!(
        db.query_row(
            "SELECT count(*) FROM requests WHERE operation='git_status'",
            [],
            |r| r.get::<_, i64>(0)
        )
        .unwrap(),
        1
    );
    journal.prune(EPOCH + READ_SECONDS + 1.0).unwrap();
    assert_eq!(
        db.query_row("SELECT count(*) FROM requests", [], |r| r.get::<_, i64>(0))
            .unwrap(),
        0
    );
}

#[test]
fn restart_recovers_git_read_as_failed_and_preserves_exact_replay_and_hash() {
    let scratch = Scratch::new("git-status-restart");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    assert!(journal
        .claim(REQUEST, "git_status", Some("hash"), EPOCH)
        .unwrap());
    common::close(journal);
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH + 1.0).unwrap();
    assert!(!journal
        .claim(REQUEST, "git_status", Some("hash"), EPOCH + 1.0)
        .unwrap());
    let replay: Value = serde_json::from_str(&journal.replay(REQUEST).unwrap()).unwrap();
    assert_eq!(replay["state"], "failed");
    assert!(journal
        .claim(REQUEST, "git_status", Some("different"), EPOCH + 1.0)
        .is_err());
}
