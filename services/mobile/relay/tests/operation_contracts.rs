//! Accept and reject contracts for operations whose relay behaviour only the
//! Python reference relay's tests covered: context commands, the input queue,
//! settings, recovery, processes, worktrees, the terminal window, archives,
//! speech ownership, attachment budgets and read retention. `ORACLE.md` maps
//! each Python test to its contract here.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use uuid::Uuid;
use zerus_relay::{
    args,
    config::Config,
    db::{i, now, s},
    json::{canonical, digest},
    protocol::{self, MIB},
    store::{Credential, Role, Submission},
};

mod common;
mod support;
use common::Fixture;
use support::Server;

const SESSION: &str = "codex/example/mobile";
const ARCHIVE: &str = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";

/// A session request with the native identity a mutation carries; `extra`
/// adds payload fields.
fn command(computer: &str, op: &str, extra: Value) -> Value {
    let id = Uuid::new_v4().to_string();
    let mut payload =
        json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"conversation"});
    for (k, v) in extra.as_object().unwrap() {
        payload[k] = v.clone();
    }
    json!({"request_id":id,"computer_id":computer,"operation":op,"session":SESSION,"payload":payload})
}
/// A computer-scoped request: empty session, the request ID in the payload.
fn computer_command(computer: &str, op: &str, extra: Value) -> Value {
    let mut b = command(computer, op, json!({}));
    b["session"] = json!("");
    let id = b["request_id"].clone();
    b["payload"] = extra;
    b["payload"]["request_id"] = id;
    b
}
/// An inspection, which names no request ID in its payload.
fn inspect(computer: &str, payload: Value) -> Value {
    let mut b = command(computer, "inspect", json!({}));
    b["payload"] = payload;
    b
}
fn set(mut b: Value, field: &str, value: Value) -> Value {
    b["payload"][field] = value;
    b
}
fn without(mut b: Value, field: &str) -> Value {
    b["payload"].as_object_mut().unwrap().remove(field);
    b
}
/// The wire verdict alone: 202 when valid, else the refusal status.
fn wire(b: &Value) -> u16 {
    protocol::request(b).map_or_else(|e| e.0.as_u16(), |()| 202)
}
/// The submission's status as a phone sees it.
async fn status(f: &Fixture, b: &Value) -> u16 {
    match f.submit(b).await {
        Ok(_) => 202,
        Err(e) => e.0.as_u16(),
    }
}
async fn submitted_by(f: &Fixture, phone: &Credential, b: &Value) -> u16 {
    match f
        .store
        .submit(phone, &Submission::new(b.clone()).unwrap())
        .await
    {
        Ok(_) => 202,
        Err(e) => e.0.as_u16(),
    }
}
async fn reads(f: &Fixture, delta: i64) {
    f.store
        .db
        .exec(
            "UPDATE workspace_usage SET reads=reads+? WHERE workspace_id=?",
            args![delta, &f.phone.workspace],
        )
        .await
        .unwrap();
}
async fn advertise(f: &Fixture, operations: &[&str], features: &[&str]) {
    f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":operations,"features":features}}))
        .await;
}
/// Claims `b`, which must be the oldest queued request, and completes it.
async fn complete(f: &Fixture, b: &Value) {
    let claim = f
        .store
        .claim(&f.node, true)
        .await
        .unwrap()
        .expect("a queued request")
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["request_id"], b["request_id"]);
    let result = json!({"state":"completed","result":{"ok":true},"error":null});
    f.store
        .result(
            &f.node,
            s(b, "request_id"),
            &result,
            Some(&canonical(&result["result"]).unwrap()),
        )
        .await
        .unwrap();
}
async fn age(f: &Fixture, column: &str, b: &Value, seconds: f64) {
    f.store
        .db
        .exec(
            &format!("UPDATE requests SET {column}=? WHERE id=?"),
            args![now() - seconds, &b["request_id"]],
        )
        .await
        .unwrap();
}
async fn stored(f: &Fixture, b: &Value) -> bool {
    f.store
        .db
        .one(
            "SELECT id FROM requests WHERE id=?",
            args![&b["request_id"]],
        )
        .await
        .unwrap()
        .is_some()
}
async fn receipt(f: &Fixture, b: &Value) -> Value {
    f.store.receipt(&f.phone, s(b, "request_id")).await.unwrap()
}

#[tokio::test]
async fn context_commands_are_gated_per_operation_and_protocol_version() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    // The relay declares exactly the operations the Python connector
    // advertises with every capability, as its golden vectors record them.
    let vectors: Value =
        serde_json::from_str(include_str!("../../vectors/operations.json")).unwrap();
    let declared = protocol::capabilities(vec![])["operations"].clone();
    let set = |v: &Value| -> std::collections::BTreeSet<String> {
        v.as_array()
            .unwrap()
            .iter()
            .map(|op| op.as_str().unwrap().to_string())
            .collect()
    };
    assert_eq!(
        set(&declared),
        set(&vectors["capabilities"]["all"]["operations"])
    );
    assert_eq!(declared, json!(protocol::OPERATIONS));
    let compact = || command(&n, "compact_context", json!({}));
    let clear = || command(&n, "clear_context", json!({}));
    // A computer that advertises nothing, or other operations, gets neither.
    for snapshot in [
        json!({"sessions":[]}),
        json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":[]}}),
        json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":["inspect","send"]}}),
    ] {
        f.beat(&snapshot).await;
        assert_eq!(status(&f, &compact()).await, 409, "{snapshot}");
        assert_eq!(status(&f, &clear()).await, 409, "{snapshot}");
    }
    // An ordinary send needs no advertisement.
    let send = command(&n, "send", json!({"text":"Legacy send"}));
    assert_eq!(f.submit(&send).await.unwrap()["state"], "queued");
    // Each operation is advertised on its own, under protocol version 1 exactly.
    advertise(&f, &["compact_context"], &[]).await;
    assert_eq!(status(&f, &compact()).await, 202);
    assert_eq!(status(&f, &clear()).await, 409);
    for version in [json!(true), json!(1.0), json!(2), json!("1")] {
        f.beat(&json!({"sessions":[],"mobile_capabilities":{"protocol_version":version,"operations":protocol::OPERATIONS}}))
            .await;
        assert_eq!(status(&f, &compact()).await, 409, "{version}");
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn context_receipts_survive_a_capability_change() {
    let f = Fixture::new().await;
    advertise(&f, protocol::OPERATIONS, &[]).await;
    let request = command(&f.node.id, "compact_context", json!({}));
    assert_eq!(f.submit(&request).await.unwrap()["state"], "queued");
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(claim["requests"][0]["payload"], request["payload"]);
    // A computer that stops advertising still answers the existing identity.
    advertise(&f, &[], &[]).await;
    assert_eq!(f.submit(&request).await.unwrap()["state"], "claimed");
    let mut different = request.clone();
    different["operation"] = json!("clear_context");
    assert_eq!(status(&f, &different).await, 409);
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[test]
fn answers_hold_only_integers_python_reads() {
    // The connector refuses integers over 4,300 digits, as Python's json does;
    // the relay refuses them at submission rather than queue a request that
    // could only expire unclaimed (zerus-3rqx.7).
    let answer = |digits: usize| {
        let mut b = command("computer", "answer",
            json!({"question_id":"q","expected_question_hash":"h","answers":[["Yes"]]}));
        b["payload"]["answers"] = json!([[0]]);
        let text = canonical(&b).unwrap().replace("[[0]]", &format!("[[{}]]", "9".repeat(digits)));
        zerus_relay::json::parse(text.as_bytes(), 32, 50_000).unwrap()
    };
    assert_eq!(wire(&answer(zerus_relay::json::MAX_INT_DIGITS)), 202);
    assert_eq!(wire(&answer(zerus_relay::json::MAX_INT_DIGITS + 1)), 400);
    // Anywhere in the request, as the connector checks the whole of it.
    let mut b = command("computer", "answer",
        json!({"question_id":"q","expected_question_hash":"h","answers":[["Yes"]],"extra":0}));
    b["payload"].as_object_mut().unwrap().remove("extra");
    let text = canonical(&b).unwrap().replace("\"computer_id\"", &format!("\"n\":{},\"computer_id\"", "1".repeat(4301)));
    let parsed = zerus_relay::json::parse(text.as_bytes(), 32, 50_000).unwrap();
    assert_eq!(wire(&parsed), 400);
}

#[test]
fn context_payloads_are_exact() {
    let n = Uuid::new_v4().to_string();
    for op in ["compact_context", "clear_context"] {
        assert_eq!(wire(&command(&n, op, json!({}))), 202, "{op}");
        for (field, value) in [
            ("text", json!("/compact")),
            ("archive_id", json!(Uuid::new_v4().to_string())),
            ("expected_run_id", json!("")),
            ("expected_conversation_id", json!("")),
            ("expected_conversation_id", json!(3)),
        ] {
            let b = set(command(&n, op, json!({})), field, value);
            assert_eq!(wire(&b), 400, "{op} {field}");
        }
    }
    let mut renamed = command(&n, "compact_context", json!({}));
    renamed["operation"] = json!("compact-context");
    assert_eq!(wire(&renamed), 400);
    // A send names the compaction it continues by UUID, or "" for none.
    let send = |c: Value| {
        command(
            &n,
            "send",
            json!({"text":"Hello","expected_compaction_id":c}),
        )
    };
    assert_eq!(wire(&send(json!(""))), 202);
    assert_eq!(wire(&send(json!(Uuid::new_v4().to_string()))), 202);
    for c in [json!("../../file"), json!(3)] {
        assert_eq!(wire(&send(c.clone())), 400, "{c}");
    }
}

#[tokio::test]
async fn queue_settings_recovery_and_process_controls_are_exact_and_gated() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let queue = "a".repeat(64);
    let job = Uuid::new_v4().to_string();
    let nil = "00000000-0000-0000-0000-000000000000";
    let valid = [
        command(&n, "send_now", json!({"queue_id":queue})),
        command(
            &n,
            "settings",
            json!({"model":"fixture-fast","effort":"low"}),
        ),
        command(
            &n,
            "settings",
            json!({"effort":"","expected_pending_id":Uuid::new_v4().to_string()}),
        ),
        command(&n, "recovery_action", json!({"job_id":job,"action":"now"})),
        command(
            &n,
            "recovery_action",
            json!({"job_id":job,"action":"cancel"}),
        ),
        command(
            &n,
            "process_output",
            json!({"process_id":"fixture-job","generation":"g:1"}),
        ),
        // A process read of a live session may name the nil archive.
        command(
            &n,
            "process_output",
            json!({"process_id":"fixture-job","archive_id":nil}),
        ),
        command(&n, "process_stop", json!({"process_id":"fixture-job"})),
    ];
    for b in &valid {
        assert_eq!(status(&f, b).await, 409, "{}", b["operation"]);
    }
    advertise(
        &f,
        &[
            "send_now",
            "settings",
            "recovery_action",
            "process_output",
            "process_stop",
        ],
        &[],
    )
    .await;
    for b in &valid {
        assert_eq!(f.submit(b).await.unwrap()["state"], "queued", "{b}");
    }
    let invalid = [
        command(&n, "send_now", json!({"queue_id":"A".repeat(64)})),
        command(&n, "send_now", json!({"queue_id":"a".repeat(63)})),
        command(&n, "send_now", json!({})),
        command(&n, "send_now", json!({"queue_id":queue,"text":"Now"})),
        command(&n, "settings", json!({})),
        command(&n, "settings", json!({"effort":"extreme"})),
        command(&n, "settings", json!({"model":""})),
        command(&n, "settings", json!({"model":"x".repeat(257)})),
        command(
            &n,
            "settings",
            json!({"model":"fixture","expected_pending_id":"pending"}),
        ),
        command(
            &n,
            "recovery_action",
            json!({"job_id":job,"action":"later"}),
        ),
        command(&n, "recovery_action", json!({"job_id":nil,"action":"now"})),
        command(&n, "recovery_action", json!({"action":"now"})),
        command(&n, "process_output", json!({"process_id":"../job"})),
        command(
            &n,
            "process_output",
            json!({"process_id":"job","generation":"a b"}),
        ),
        command(
            &n,
            "process_stop",
            json!({"process_id":"job","archive_id":Uuid::new_v4().to_string()}),
        ),
        command(&n, "process_stop", json!({"process_id":"x".repeat(161)})),
    ];
    for b in &invalid {
        assert_eq!(status(&f, b).await, 400, "{b}");
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn inspection_and_send_extensions_need_their_advertised_features() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let identity = json!({"expected_run_id":"run","expected_conversation_id":"conversation"});
    let with = |extra: Value| {
        let mut payload = identity.clone();
        for (k, v) in extra.as_object().unwrap() {
            payload[k] = v.clone();
        }
        inspect(&n, payload)
    };
    let extended = [
        with(json!({"after":1})),
        with(json!({"agent_id":"child"})),
        command(&n, "send", json!({"text":"Hello","agent_id":"child"})),
    ];
    for b in &extended {
        assert_eq!(status(&f, b).await, 409, "{b}");
    }
    advertise(&f, protocol::OPERATIONS, protocol::FEATURES).await;
    for b in &extended {
        assert_eq!(f.submit(b).await.unwrap()["state"], "queued", "{b}");
    }
    // An extension names the native identity it applies to; a child is never paged.
    for b in [
        inspect(&n, json!({"after":1})),
        with(json!({"after":-1})),
        with(json!({"agent_id":"a/b"})),
        with(json!({"agent_id":"child","after":1})),
        with(json!({"expected_run_id":""})),
        command(
            &n,
            "send",
            json!({"text":"Hello","agent_id":"child","expected_compaction_id":Uuid::new_v4().to_string()}),
        ),
    ] {
        assert_eq!(status(&f, &b).await, 400, "{b}");
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn worktree_controls_are_exact_absolute_and_gated() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let list = || computer_command(&n, "worktrees", json!({"path":"/workspace/project"}));
    let create = || {
        computer_command(
            &n,
            "worktree_create",
            json!({"path":"/workspace/project","common_dir":"/workspace/project/.git",
            "destination":"/workspace/project-feature","branch":"feature","base":"main"}),
        )
    };
    assert_eq!(status(&f, &list()).await, 409);
    assert_eq!(status(&f, &create()).await, 409);
    advertise(&f, &["worktrees", "worktree_create"], &[]).await;
    assert_eq!(f.submit(&list()).await.unwrap()["state"], "queued");
    assert_eq!(f.submit(&create()).await.unwrap()["state"], "queued");
    let mut in_session = create();
    in_session["session"] = json!(SESSION);
    let mut other_id = create();
    other_id["payload"]["request_id"] = json!(Uuid::new_v4().to_string());
    for b in [
        in_session,
        other_id,
        set(create(), "path", json!("workspace/project")),
        set(create(), "common_dir", json!("")),
        set(create(), "destination", json!("../project-feature")),
        set(create(), "branch", json!("-feature")),
        set(create(), "branch", json!(" feature")),
        set(create(), "base", json!("")),
        set(create(), "base", json!("x".repeat(257))),
        set(create(), "name", json!("feature")),
        without(create(), "common_dir"),
        set(list(), "branch", json!("feature")),
    ] {
        assert_eq!(status(&f, &b).await, 400, "{b}");
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn reads_leave_after_an_hour_and_mutations_keep_their_receipts() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    advertise(&f, protocol::OPERATIONS, protocol::FEATURES).await;
    let reads = [
        command(&n, "process_output", json!({"process_id":"job"})),
        computer_command(&n, "worktrees", json!({"path":"/workspace/project"})),
        inspect(
            &n,
            json!({"after":1,"expected_run_id":"run","expected_conversation_id":"conversation"}),
        ),
    ];
    let mutations = [
        command(&n, "process_stop", json!({"process_id":"job"})),
        command(&n, "send_now", json!({"queue_id":"a".repeat(64)})),
        computer_command(
            &n,
            "worktree_create",
            json!({"path":"/workspace/project","common_dir":"/workspace/project/.git",
            "destination":"/workspace/project-feature","branch":"feature","base":"main"}),
        ),
    ];
    for b in reads.iter().chain(&mutations) {
        f.submit(b).await.unwrap();
        complete(&f, b).await;
        age(&f, "updated", b, 3601.0).await;
    }
    f.store.maintain(false).await.unwrap();
    for b in &reads {
        assert!(!stored(&f, b).await, "{}", b["operation"]);
        // A pruned read is new again.
        assert_eq!(f.submit(b).await.unwrap()["state"], "queued");
    }
    for b in &mutations {
        assert!(stored(&f, b).await, "{}", b["operation"]);
        assert_eq!(f.submit(b).await.unwrap()["state"], "completed");
    }
    f.check_accounting().await;
    f.close().await;
}

/// After the retention period a mutation keeps only its identity: the same
/// request is answered with the bodiless receipt and is never delivered again.
#[tokio::test]
async fn expired_mutation_history_leaves_a_tombstone_that_never_replays() {
    let f = Fixture::new().await;
    let b = command(&f.node.id, "send", json!({"text":"Hello"}));
    f.submit(&b).await.unwrap();
    complete(&f, &b).await;
    age(&f, "updated", &b, 8.0 * 86400.0).await;
    f.store.maintain(false).await.unwrap();
    let tombstone = f.submit(&b).await.unwrap();
    assert_eq!(
        (tombstone["state"].clone(), tombstone["result"].clone()),
        (json!("completed"), Value::Null)
    );
    let mut changed = b.clone();
    changed["session"] = json!("changed");
    assert_eq!(status(&f, &changed).await, 409);
    assert!(f.store.claim(&f.node, true).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn read_quota_never_consumes_mutation_capacity() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    // The workspace holds the 5,000 reads it may retain.
    reads(&f, 5000).await;
    assert_eq!(status(&f, &inspect(&n, json!({}))).await, 429);
    let send = command(&n, "send", json!({"text":"Hello"}));
    assert_eq!(f.submit(&send).await.unwrap()["state"], "queued");
    reads(&f, -5000).await;
    assert_eq!(status(&f, &inspect(&n, json!({}))).await, 202);
    f.close().await;
}

#[tokio::test]
async fn terminal_input_needs_a_fresh_computer_and_expires_before_delivery() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let input = || {
        command(
            &n,
            "terminal_input",
            json!({"terminal_binding_id":"a".repeat(64),"text":"echo synthetic","enter":true}),
        )
    };
    assert_eq!(status(&f, &input()).await, 409);
    advertise(&f, &["terminal_snapshot", "terminal_input"], &[]).await;
    let first = input();
    let admitted = now();
    assert_eq!(f.submit(&first).await.unwrap()["state"], "queued");
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    let expires = claim["requests"][0]["expires_at"].as_f64().unwrap();
    assert!(
        (admitted + 5.0..=now() + 5.0).contains(&expires),
        "{expires}"
    );
    let uncertain = json!({"state":"uncertain","result":null,"error":"fixture"});
    f.store
        .result(&f.node, s(&first, "request_id"), &uncertain, None)
        .await
        .unwrap();
    // A computer silent for 45 s takes no input; its next heartbeat restores it.
    f.store
        .db
        .exec(
            "UPDATE nodes SET last_seen=? WHERE id=?",
            args![now() - 46.0, &n],
        )
        .await
        .unwrap();
    assert_eq!(status(&f, &input()).await, 409);
    advertise(&f, &["terminal_snapshot", "terminal_input"], &[]).await;
    // Input that waits past its window fails instead of reaching the terminal.
    let late = input();
    f.submit(&late).await.unwrap();
    age(&f, "expires_at", &late, 0.5).await;
    let late_receipt = receipt(&f, &late).await;
    assert_eq!(
        (late_receipt["state"].clone(), late_receipt["error"].clone()),
        (
            json!("failed"),
            json!("terminal input expired before delivery")
        )
    );
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    // A screen read has no window of its own.
    let screen = command(&n, "terminal_snapshot", json!({}));
    f.submit(&screen).await.unwrap();
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert!(claim["requests"][0].get("expires_at").is_none());
    f.check_accounting().await;
    f.close().await;
}

/// The relay refuses what the connector's history, launch and lifecycle
/// call sites refuse, so neither ever reaches a native command.
#[test]
fn history_launch_and_lifecycle_wires_refuse_what_the_connector_refuses() {
    let n = Uuid::new_v4().to_string();
    let history = |extra: Value| command(&n, "history", extra);
    assert_eq!(wire(&history(json!({}))), 202);
    assert_eq!(wire(&history(json!({"limit":100,"before":"cursor"}))), 202);
    assert_eq!(
        wire(&history(
            json!({"around_incoming_seq":1,"history_epoch":"epoch"})
        )),
        202
    );
    for extra in [
        json!({"path":"/private"}),
        json!({"limit":true}),
        json!({"limit":101}),
        json!({"limit":0}),
        json!({"before":"cursor","after":"cursor"}),
        json!({"around_incoming_seq":1}),
        json!({"agent_id":"../other"}),
    ] {
        assert_eq!(wire(&history(extra.clone())), 400, "{extra}");
    }
    assert_eq!(
        wire(&set(
            history(json!({})),
            "expected_conversation_id",
            json!("")
        )),
        400
    );
    let launch = |extra: Value| {
        let mut payload =
            json!({"agent":"codex","directory":"/example/mobile","tag":"new-session"});
        for (k, v) in extra.as_object().unwrap() {
            payload[k] = v.clone();
        }
        computer_command(&n, "launch", payload)
    };
    assert_eq!(wire(&launch(json!({}))), 202);
    assert_eq!(wire(&launch(json!({"account_id":"native-codex"}))), 202);
    for extra in [
        json!({"directory":"../relative"}),
        json!({"tag":"x/evil"}),
        json!({"tag":"x.y"}),
        json!({"agent":"shell"}),
        json!({"command":"rm"}),
        json!({"account_id":"../secret"}),
    ] {
        assert_eq!(wire(&launch(extra.clone())), 400, "{extra}");
    }
    for (op, extra) in [
        ("pause", json!({})),
        ("rename", json!({"new_name":"renamed"})),
        ("fork", json!({"tag":"copy"})),
        ("restore", json!({"archive_id":ARCHIVE})),
    ] {
        assert_eq!(wire(&command(&n, op, extra)), 202, "{op}");
    }
    for (op, extra) in [
        ("pause", json!({"archive_id":Uuid::new_v4().to_string()})),
        ("pause", json!({"action":"terminate"})),
        ("rename", json!({})),
        ("rename", json!({"new_name":"a".repeat(513)})),
        ("rename", json!({"new_name":" padded"})),
        ("fork", json!({"tag":"../invalid"})),
        ("restore", json!({})),
    ] {
        assert_eq!(wire(&command(&n, op, extra.clone())), 400, "{op} {extra}");
    }
}

#[test]
fn terminal_input_is_literal_bounded_and_keys_are_allowlisted() {
    let n = Uuid::new_v4().to_string();
    let binding = "a".repeat(64);
    let text = |text: Value, enter: Value| {
        command(
            &n,
            "terminal_input",
            json!({"terminal_binding_id":binding,"text":text,"enter":enter}),
        )
    };
    let key = |key: &str| {
        command(
            &n,
            "terminal_input",
            json!({"terminal_binding_id":binding,"key":key}),
        )
    };
    for b in [
        text(json!("line\n\tindented"), json!(true)),
        text(json!("no enter"), json!(false)),
        text(json!(""), json!(true)),
        text(json!("x".repeat(65536)), json!(false)),
        // The bound counts the text, not its larger escaped form.
        text(json!("\"\\\t".repeat(21000)), json!(true)),
        key("Enter"),
        key("C-c"),
        key("BTab"),
    ] {
        assert_eq!(wire(&b), 202, "{b}");
    }
    for b in [
        text(json!(""), json!(false)),
        text(json!("x".repeat(65537)), json!(false)),
        text(json!("\u{1b}[31m"), json!(true)),
        text(json!("\u{7f}"), json!(true)),
        text(json!("carriage\r"), json!(true)),
        text(json!("yes"), json!("yes")),
        key("C-z"),
        set(key("Enter"), "text", json!("also text")),
        set(key("Enter"), "terminal_binding_id", json!("A".repeat(64))),
        without(key("Enter"), "terminal_binding_id"),
        set(
            key("Enter"),
            "archive_id",
            json!(Uuid::new_v4().to_string()),
        ),
        set(key("Enter"), "action", json!("input")),
    ] {
        assert_eq!(wire(&b), 400, "{b}");
    }
    // Only the relay sets the input window; a phone cannot.
    let mut windowed = key("Enter");
    windowed["expires_at"] = json!(1_900_000_000.0);
    assert_eq!(wire(&windowed), 400);
}

#[tokio::test]
async fn sustained_screens_and_abandoned_reads_expire_without_evicting_input() {
    let f = Fixture::config(Config {
        background: false,
        maintenance_batch: 16,
        ..Config::default()
    })
    .await;
    let n = f.node.id.clone();
    advertise(&f, &["terminal_snapshot", "terminal_input"], &[]).await;
    let screen = || command(&n, "terminal_snapshot", json!({}));
    for _ in 0..40 {
        let b = screen();
        f.submit(&b).await.unwrap();
        complete(&f, &b).await;
    }
    let input = command(
        &n,
        "terminal_input",
        json!({"terminal_binding_id":"a".repeat(64),"text":"literal","enter":true}),
    );
    f.submit(&input).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    f.store
        .result(
            &f.node,
            s(&input, "request_id"),
            &json!({"state":"uncertain","result":null,"error":"fixture"}),
            None,
        )
        .await
        .unwrap();
    // One screen is claimed and never answered, another never claimed.
    let unanswered = screen();
    f.submit(&unanswered).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let abandoned = screen();
    f.submit(&abandoned).await.unwrap();
    f.store
        .db
        .exec(
            "UPDATE requests SET updated=? WHERE operation='terminal_snapshot' AND state='completed'",
            args![now() - 121.0],
        )
        .await
        .unwrap();
    age(&f, "created", &abandoned, 121.0).await;
    age(&f, "claimed", &unanswered, 91.0).await;
    age(&f, "updated", &input, 1000.0).await;
    let screens = "requests WHERE operation='terminal_snapshot'";
    assert_eq!(f.count(screens).await, 42);
    // Each maintenance pass deletes at most one batch.
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count(screens).await, 42 - 16);
    for _ in 0..4 {
        f.store.maintain(false).await.unwrap();
    }
    // The abandoned reads are terminal now, and leave once they age as well.
    assert_eq!(f.count(screens).await, 2);
    assert_eq!(receipt(&f, &abandoned).await["state"], "failed");
    assert_eq!(receipt(&f, &unanswered).await["state"], "uncertain");
    for b in [&abandoned, &unanswered] {
        age(&f, "updated", b, 121.0).await;
    }
    f.store.maintain(false).await.unwrap();
    assert_eq!(f.count(screens).await, 0);
    assert_eq!(f.count("requests").await, 1);
    assert_eq!(receipt(&f, &input).await["state"], "uncertain");
    assert_eq!(f.submit(&input).await.unwrap()["state"], "uncertain");
    let usage = f
        .store
        .db
        .one(
            "SELECT reads,mutations FROM workspace_usage WHERE workspace_id=?",
            args![&f.phone.workspace],
        )
        .await
        .unwrap()
        .unwrap();
    assert_eq!((i(&usage, "reads"), i(&usage, "mutations")), (0, 1));
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn archive_ids_are_exact_on_reads_and_refused_on_ordinary_mutations() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let archived = inspect(&n, json!({"archive_id":ARCHIVE}));
    assert_eq!(f.submit(&archived).await.unwrap()["state"], "queued");
    let claim = f
        .store
        .claim(&f.node, false)
        .await
        .unwrap()
        .unwrap()
        .decode()
        .unwrap();
    assert_eq!(
        claim["requests"][0]["payload"],
        json!({"archive_id":ARCHIVE})
    );
    for archive in [
        json!(""),
        json!("../../outside"),
        json!(ARCHIVE.to_uppercase()),
        json!(7),
        Value::Null,
    ] {
        assert_eq!(
            status(&f, &inspect(&n, json!({"archive_id":archive}))).await,
            400,
            "{archive}"
        );
    }
    // Each mutation is valid without the archive and refused with it.
    for (op, extra) in [
        ("send", json!({"text":"Hello"})),
        (
            "answer",
            json!({"question_id":"q","expected_question_hash":"h","answers":{}}),
        ),
        ("interrupt", json!({"expected_turn_started":1.5})),
    ] {
        let b = command(&n, op, extra);
        assert_eq!(wire(&b), 202, "{op}");
        let archived = set(b, "archive_id", json!(ARCHIVE));
        assert_eq!(status(&f, &archived).await, 400, "{op}");
    }
    assert_eq!(
        f.submit(&inspect(&n, json!({}))).await.unwrap()["state"],
        "queued"
    );
    // Archive actions name a nonzero archive; only a live process read may name the nil one.
    let nil = "00000000-0000-0000-0000-000000000000";
    assert_eq!(
        wire(&command(&n, "restore", json!({"archive_id":ARCHIVE}))),
        202
    );
    assert_eq!(
        wire(&command(
            &n,
            "rename",
            json!({"new_name":"renamed","archive_id":ARCHIVE})
        )),
        202
    );
    for b in [
        command(&n, "restore", json!({"archive_id":nil})),
        command(&n, "restore", json!({})),
        command(&n, "forget", json!({"archive_id":nil})),
        command(&n, "fork", json!({"tag":"copy","archive_id":nil})),
        command(
            &n,
            "rename",
            json!({"new_name":"renamed","archive_id":ARCHIVE.to_uppercase()}),
        ),
        inspect(&n, json!({"archive_id":nil.replace('0', "g")})),
    ] {
        assert_eq!(wire(&b), 400, "{b}");
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn archived_rows_never_change_the_live_event_baseline() {
    let f = Fixture::new().await;
    let live = json!({"name":SESSION,"run_id":"live-run","conversation_id":"live-conversation","phase":"working","activity":"busy"});
    let archive = json!({"name":SESSION,"archive_id":ARCHIVE,"state":"archived","run_id":"saved-run","conversation_id":"saved-conversation","phase":"idle","activity":"idle"});
    let mut question = live.clone();
    question["phase"] = json!("input");
    question["attention_id"] = json!("question");
    let mut idle = live.clone();
    idle["phase"] = json!("idle");
    idle["activity"] = json!("idle");
    for session in [&live, &question, &question, &idle] {
        f.beat(&json!({"sessions":[session, archive]})).await;
    }
    let events = f.store.events(&f.phone, 0).await.unwrap();
    let kinds: Vec<_> = events["events"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|e| e["kind"] != "alert")
        .map(|e| e["kind"].as_str().unwrap())
        .collect();
    assert_eq!(kinds, ["attention", "completed"]);
    f.close().await;
}

fn attachment(bytes: usize, extra: Value) -> Value {
    let mut file = json!({"name":"example.txt","mime":"text/plain","data_base64":STANDARD.encode(vec![b'x'; bytes])});
    for (k, v) in extra.as_object().unwrap() {
        file[k] = v.clone();
    }
    file
}
fn upload(computer: &str, files: Vec<Value>) -> Value {
    command(
        computer,
        "send",
        json!({"text":"Read these files","attachments":files}),
    )
}

#[tokio::test]
async fn attachment_limits_are_advertised_and_paths_are_refused() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let limits = &protocol::capabilities(vec![])["attachment_limits"];
    assert_eq!(limits["max_total_bytes"], 20 * MIB);
    assert_eq!(limits["max_file_bytes"], 10 * MIB);
    for file in [
        attachment(16, json!({"path":"/etc/passwd"})),
        attachment(16, json!({"name":"../../outside"})),
        attachment(16, json!({"data_base64":"not base64"})),
    ] {
        assert_eq!(status(&f, &upload(&n, vec![file])).await, 400);
    }
    // A snapshot beyond the ordinary 1 MiB bound is refused.
    let large = json!({"sessions":[],"padding":"x".repeat(MIB)});
    let refused = f
        .store
        .heartbeat(
            &f.node,
            &json!({ "snapshot": large }),
            &canonical(&large).unwrap(),
        )
        .await
        .unwrap_err();
    assert_eq!(refused.0.as_u16(), 413);
    f.close().await;
}

#[tokio::test]
async fn retained_bodies_reach_the_budget_while_exact_retries_and_controls_fit() {
    // The canonical size does not depend on which UUIDs the request carries.
    let sizing = upload(
        &Uuid::new_v4().to_string(),
        vec![attachment(3000, json!({}))],
    );
    let bytes = canonical(&sizing).unwrap().len() as i64;
    let f = Fixture::config(Config {
        background: false,
        max_queue_bytes: bytes + 100 + MIB as i64,
        ..Config::default()
    })
    .await;
    let n = f.node.id.clone();
    let first = upload(&n, vec![attachment(3000, json!({}))]);
    assert_eq!(canonical(&first).unwrap().len() as i64, bytes);
    f.submit(&first).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let done = json!({"state":"completed","result":null,"error":null});
    f.store
        .result(&f.node, s(&first, "request_id"), &done, None)
        .await
        .unwrap();
    // The completed body is still retained, so another upload is over budget.
    assert_eq!(
        status(&f, &upload(&n, vec![attachment(3000, json!({}))])).await,
        429
    );
    // Its exact retry is a receipt, and a small control has its own allowance.
    assert_eq!(f.submit(&first).await.unwrap()["state"], "completed");
    let control = command(&n, "send", json!({"text":"Hello"}));
    assert_eq!(f.submit(&control).await.unwrap()["state"], "queued");
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn many_large_results_reach_the_workspace_budget() {
    let f = Fixture::config(Config {
        background: false,
        max_queue_bytes: 3 * MIB as i64,
        ..Config::default()
    })
    .await;
    let n = f.node.id.clone();
    let reply =
        json!({"state":"completed","result":{"synthetic":"x".repeat(800_000)},"error":null});
    let encoded = canonical(&reply["result"]).unwrap();
    let mut admitted = vec![];
    for _ in 0..8 {
        let b = inspect(&n, json!({}));
        match f.submit(&b).await {
            Ok(_) => {}
            Err(e) => {
                assert_eq!(e.0.as_u16(), 429);
                break;
            }
        }
        assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
        for _ in 0..2 {
            f.store
                .result(&f.node, s(&b, "request_id"), &reply, Some(&encoded))
                .await
                .unwrap();
        }
        admitted.push(b);
    }
    assert!(
        (1..8).contains(&admitted.len()),
        "{} large results",
        admitted.len()
    );
    for b in &admitted {
        assert_eq!(f.submit(b).await.unwrap()["state"], "completed");
    }
    assert!(f.store.claim(&f.node, false).await.unwrap().is_none());
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn node_revocation_charges_errors_and_releases_reservations_exactly() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    for _ in 0..2 {
        f.submit(&command(&n, "send", json!({"text":"Synthetic"})))
            .await
            .unwrap();
    }
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    f.store.revoke(Role::Node, &n).await.unwrap();
    let mut t = f.store.db.begin().await.unwrap();
    let rows = t
        .all(
            "SELECT state,error,result,result_bytes,reserved_bytes FROM requests",
            &[],
        )
        .await
        .unwrap();
    t.commit().await.unwrap();
    let states: std::collections::BTreeSet<_> = rows.iter().map(|r| s(r, "state")).collect();
    assert_eq!(
        states,
        std::collections::BTreeSet::from(["failed", "uncertain"])
    );
    for r in &rows {
        assert_eq!(i(r, "reserved_bytes"), 0);
        assert!(r["result"].is_null());
        assert_eq!(i(r, "result_bytes") as usize, s(r, "error").len());
    }
    f.check_accounting().await;
    f.close().await;
}

#[tokio::test]
async fn speech_and_management_status_belong_to_the_original_phone_and_computer() {
    let f = Fixture::new().await;
    let n = f.node.id.clone();
    let speech = json!({"sessions":[],"mobile_capabilities":{"protocol_version":1,"operations":protocol::SPEECH}});
    f.beat(&speech).await;
    let audio =
        json!({"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAAAAA=="});
    let upload = computer_command(
        &n,
        "speech_submit",
        json!({"profile_id":"gigaam-ru","audio":audio}),
    );
    f.submit(&upload).await.unwrap();
    assert!(f.store.claim(&f.node, false).await.unwrap().is_some());
    let job = json!({"schema":1,"job_id":upload["request_id"],"state":"queued"});
    f.store
        .result(
            &f.node,
            s(&upload, "request_id"),
            &json!({"state":"completed","result":job,"error":null}),
            Some(&canonical(&job).unwrap()),
        )
        .await
        .unwrap();
    let invitation = f.store.invite(&f.phone.workspace).await.unwrap();
    let paired = f
        .store
        .pair(s(&invitation, "pair_code"), "Other phone")
        .await
        .unwrap();
    let other = f
        .store
        .authenticate(s(&paired, "device_token"), Role::Device)
        .await
        .unwrap();
    let follow = |op: &str, computer: &str| {
        let mut b = command(computer, op, json!({}));
        b["session"] = json!("");
        b["payload"] = json!({"job_id":upload["request_id"]});
        b
    };
    for op in ["speech_status", "speech_cancel"] {
        assert_eq!(submitted_by(&f, &other, &follow(op, &n)).await, 404, "{op}");
    }
    // Another computer of the workspace, advertising speech, cannot read the job.
    let second = f
        .store
        .node(&f.phone.workspace, "Other computer")
        .await
        .unwrap();
    let node = f
        .store
        .authenticate(s(&second, "node_token"), Role::Node)
        .await
        .unwrap();
    f.store
        .heartbeat(
            &node,
            &json!({ "snapshot": speech }),
            &canonical(&speech).unwrap(),
        )
        .await
        .unwrap();
    assert_eq!(
        status(&f, &follow("speech_status", s(&second, "node_id"))).await,
        404
    );
    assert_eq!(status(&f, &follow("speech_status", &n)).await, 202);
    // Management status follows the same ownership as its sealed change.
    let owner = "00000000-0000-4000-8000-000000000001";
    let change = computer_command(
        &n,
        "speech_profile_upsert",
        json!({"expected_machine_id":owner,"expected_revision":0,
        "profile":{"id":"fixture","name":"Fixture provider","adapter":"openai_http","model":"fixture",
            "language":"ru","base_url":"https://speech.example.invalid/v1","allow_insecure_http":false},
        "credential":{"mode":"keep"}}),
    );
    f.submit(&change).await.unwrap();
    let management = |operation: &str| {
        let mut b = command(&n, "speech_config_status", json!({}));
        b["session"] = json!("");
        b["payload"] = json!({"operation_id":operation,"expected_machine_id":owner});
        b
    };
    let status_request = management(s(&change, "request_id"));
    assert_eq!(submitted_by(&f, &other, &status_request).await, 404);
    assert_eq!(
        status(&f, &management(&Uuid::new_v4().to_string())).await,
        404
    );
    assert_eq!(status(&f, &status_request).await, 202);
    // A plaintext replacement key is refused before anything is stored.
    let mut plaintext = change.clone();
    let id = Uuid::new_v4().to_string();
    plaintext["request_id"] = json!(id);
    plaintext["payload"]["request_id"] = json!(id);
    plaintext["payload"]["credential"] = json!({"mode":"replace","value":"fixture-only-key"});
    assert_eq!(status(&f, &plaintext).await, 400);
    assert_eq!(
        f.store.receipt(&f.phone, &id).await.unwrap_err().0.as_u16(),
        404
    );
    f.check_accounting().await;
    f.close().await;
}

/// Sets a credential's request count in its open one-minute window.
async fn spend(f: &Fixture, c: &Credential, count: i64) {
    let key = json!([c.role.table(), c.workspace, c.id]);
    let changed = f
        .store
        .db
        .exec(
            "UPDATE rate_limits SET count=? WHERE key=?",
            args![count, &digest(canonical(&key).unwrap())],
        )
        .await
        .unwrap();
    assert_eq!(changed, 1, "the window is open");
}

/// Over HTTP, so the limits are the ones the relay applies to each request.
#[tokio::test]
async fn terminal_frames_fit_each_credentials_rate_budget() {
    let f = Fixture::new().await;
    let server = Server::start(&f.store).await;
    let client = reqwest::Client::new();
    let (phone, node) = (
        s(&f.paired, "device_token"),
        s(&f.credentials, "node_token"),
    );
    let get = |path: &str, token: &str| {
        let request = client.get(server.at(path)).bearer_auth(token).send();
        async move { request.await.unwrap().status().as_u16() }
    };
    let (computers, claims) = ("/v1/computers", "/v1/node/requests");
    // The first request opens each credential's one-minute window.
    assert_eq!(get(computers, phone).await, 200);
    assert_eq!(get(claims, node).await, 200);
    // Four frames a second for the whole window: each frame is a phone
    // submission and three receipt reads, and a node claim and result.
    spend(&f, &f.phone, 4 * 4 * 60 - 1).await;
    assert_eq!(get(computers, phone).await, 200);
    spend(&f, &f.node, 2 * 4 * 60 - 1).await;
    assert_eq!(get(claims, node).await, 200);
    // The budget is 2,400 requests a credential, with room to spare, and
    // still a bound that leaves other credentials alone.
    spend(&f, &f.phone, 2399).await;
    assert_eq!(get(computers, phone).await, 200);
    assert_eq!(get(computers, phone).await, 429);
    assert_eq!(get(claims, node).await, 200);
    // Failed logins and pairing keep their own, much smaller, bounds.
    for _ in 0..20 {
        assert_eq!(get(computers, "not-a-credential").await, 401);
    }
    assert_eq!(get(computers, "not-a-credential").await, 429);
    let pair = || {
        let request = client
            .post(server.at("/v1/pair"))
            .json(&json!({"code":"synthetic-invalid","device_name":"Synthetic phone"}))
            .send();
        async move { request.await.unwrap().status().as_u16() }
    };
    for _ in 0..10 {
        assert_eq!(pair().await, 401);
    }
    assert_eq!(pair().await, 429);
    assert_eq!(get(claims, node).await, 200);
    server.stop().await;
    f.close().await;
}
