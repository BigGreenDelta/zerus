//! Synthetic source controls for the additive projection read. No real hgs,
//! provider, session or receipt delivery is used by this fixture.
#![cfg(unix)]
//! Uses the Unix private journal fixture; Windows projection is not advertised.
mod common;

use common::{close, document, Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;
use zerus_mobile_connector::capabilities::Flags;
use zerus_mobile_connector::execute::Outcome;
use zerus_mobile_connector::fleet::{route_id, PeerNative, Route};
use zerus_mobile_connector::journal::{is_mutation, Journal, JOURNAL, READ_CAP};
use zerus_mobile_connector::native::{
    Call, Native, NativeError, NativeResult, Output, DEFAULT_TIMEOUT, FAILED,
};
use zerus_mobile_connector::runtime::{Connector, Settings};
use zerus_mobile_connector::validate;
use zerus_relay::{json as codec, protocol};

const ID: &str = "10000000-0000-4000-8000-000000000001";
const SESSION: &str = "codex/example/mobile";
const MARKER: &str =
    "hgs [@host] history-projection <session> --json retired and refreshed history rows";
const ERROR: &str = "native history projection identity or envelope did not match";

struct Scripted {
    answer: RefCell<Value>,
    calls: RefCell<Vec<Call>>,
}

impl Native for Scripted {
    async fn call(&self, call: Call) -> NativeResult {
        let text = !call.json_output;
        self.calls.borrow_mut().push(call);
        let answer = self.answer.borrow().clone();
        if answer.get("$fixture_fail") == Some(&json!(true)) {
            Err(NativeError::Failed(FAILED))
        } else if text && answer.is_string() {
            Ok(Output::Text(answer.as_str().unwrap().into()))
        } else {
            Ok(Output::Json(answer))
        }
    }
}

fn request(payload: Value) -> Value {
    json!({"request_id":ID,"operation":"history_projection","session":SESSION,"payload":payload})
}

fn harness(label: &str, answer: Value, help: &str) -> (Connector<Scripted>, Scratch) {
    let scratch = Scratch::new(label);
    let journal = Journal::open(&scratch.path("state"), "projection-fixture", EPOCH).unwrap();
    let connector = Connector::new(
        Scripted {
            answer: RefCell::new(answer),
            calls: RefCell::new(Vec::new()),
        },
        journal,
        Settings {
            interval: 5.0,
            binding: "projection-fixture".into(),
            default_timeout: DEFAULT_TIMEOUT,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    );
    connector.flags.set(Flags::from_help(help));
    connector.state.borrow_mut().sessions.insert(SESSION.into());
    (connector, scratch)
}

fn edit(value: &mut Value, path: &Value, replacement: Value) {
    let mut target = value;
    for key in path.as_array().unwrap() {
        target = if let Some(index) = key.as_u64() {
            &mut target[index as usize]
        } else {
            &mut target[key.as_str().unwrap()]
        };
    }
    *target = replacement;
}

fn remove(value: &mut Value, path: &Value) {
    let keys = path.as_array().unwrap();
    let mut target = value;
    for key in &keys[..keys.len() - 1] {
        target = if let Some(index) = key.as_u64() {
            &mut target[index as usize]
        } else {
            &mut target[key.as_str().unwrap()]
        };
    }
    target
        .as_object_mut()
        .unwrap()
        .remove(keys.last().unwrap().as_str().unwrap());
}

fn payload_case(case: &Value) -> Value {
    if let Some(payload) = case.get("payload") {
        return payload.clone();
    }
    let mut payload = case["base_payload"].clone();
    let field = case["replace_field"].as_str().unwrap();
    payload.as_object_mut().unwrap().remove(field);
    let mut raw = codec::canonical(&payload).unwrap();
    raw.pop();
    raw.push_str(&format!(
        ",\"{field}\":{}}}",
        case["raw_JSON_literal"].as_str().unwrap()
    ));
    codec::parse(raw.as_bytes(), 32, 1000).unwrap()
}

#[test]
fn projection_marker_is_independent_and_does_not_add_a_feature() {
    let old = Flags::from_help("hgs history <session> --json");
    let projection = Flags::from_help(MARKER);
    assert!(!old.python_operations(true).contains("history_projection"));
    assert!(projection
        .python_operations(true)
        .contains("history_projection"));
    assert!(!projection.python_operations(true).contains("history"));
    assert!(!Flags::default()
        .python_operations(true)
        .contains("history_projection"));
    assert_eq!(
        projection.advertised(false)["features"],
        Flags::default().advertised(false)["features"]
    );
}

#[tokio::test(flavor = "current_thread")]
async fn failed_or_nontext_due_probe_revokes_only_projection_and_never_calls_it() {
    let corpus = document("history_projection/results.json");
    let help = format!("{MARKER}\nhgs history <session> --json");
    for answer in [Value::Null, json!({"$fixture_fail":true})] {
        let (connector, _scratch) = harness("projection-probe-revocation", json!(help), "");
        connector.probe_capabilities().await;
        let mut expected = connector.flags.get();
        assert!(expected.history_projection && expected.history);
        expected.history_projection = false;
        *connector.native.answer.borrow_mut() = answer;
        connector.state.borrow_mut().capabilities_next_poll = 0.0;
        connector.probe_capabilities().await;
        assert_eq!(connector.flags.get(), expected);
        assert!(!connector
            .flags
            .get()
            .python_operations(true)
            .contains("history_projection"));
        assert!(connector.probed.is_set());
        assert!(connector.state.borrow().capabilities_next_poll > connector.clock());
        assert_eq!(
            connector
                .execute(request(corpus["payload"].clone()))
                .await
                .unwrap(),
            Outcome::Finished(
                json!({"state":"failed","result":null,"error":"native history projection ABI is unavailable"})
            )
        );
        let calls = connector.native.calls.borrow();
        assert_eq!(calls.len(), 2);
        assert_eq!(calls[0].argv, ["--help"]);
        assert_eq!(calls[1].argv, ["--help"]);
    }
}

#[test]
fn projection_request_corpus_rejects_coercion_and_unrelated_selectors() {
    let corpus = document("history_projection/requests.json");
    let cases = corpus["cases"].as_array().unwrap();
    assert_eq!(cases.len(), 28);
    let context = validate::Context {
        sessions: [SESSION.into()].into_iter().collect(),
        archives: [(
            SESSION.into(),
            "20000000-0000-4000-8000-000000000002".into(),
        )]
        .into_iter()
        .collect(),
        snapshot_age: 0.0,
        ..Default::default()
    };
    for case in cases {
        let name = case["id"].as_str().unwrap();
        let expected = name.starts_with("valid-");
        let request = request(payload_case(case));
        assert_eq!(
            validate::validate(request.as_object().unwrap(), ID, &context).is_ok(),
            expected,
            "{name}"
        );
        let mut relay = request.clone();
        relay["computer_id"] = json!("40000000-0000-4000-8000-000000000004");
        assert_eq!(protocol::request(&relay).is_ok(), expected, "relay {name}");
    }
    let base = corpus["cases"][1]["payload"].clone();
    for (field, good, bad) in [
        ("expected_run_id", "é".repeat(64), "é".repeat(65)),
        ("expected_conversation_id", "😀".repeat(64), "😀".repeat(65)),
        ("history_epoch", "é".repeat(64), "é".repeat(65)),
        ("agent_id", "é".repeat(80), "é".repeat(81)),
    ] {
        let mut payload = base.clone();
        payload[field] = json!(good);
        assert!(
            validate::validate_history_projection(&payload, ID).is_ok(),
            "{field}"
        );
        payload[field] = json!(bad);
        assert!(
            validate::validate_history_projection(&payload, ID).is_err(),
            "{field}"
        );
        payload[field] = json!("bad\u{85}identity");
        assert!(
            validate::validate_history_projection(&payload, ID).is_err(),
            "{field}"
        );
    }
    for field in ["archive_id", "agent_id"] {
        let mut payload = base.clone();
        payload[field] = Value::Null;
        assert!(validate::validate_history_projection(&payload, ID).is_err());
    }
}

#[tokio::test(flavor = "current_thread")]
async fn projection_returns_one_intact_fixed_range_and_rejects_malformed_native_envelopes() {
    let corpus = document("history_projection/results.json");
    let cases = corpus["cases"].as_array().unwrap();
    assert_eq!(cases.len(), 57);
    for case in cases {
        let mut payload = corpus["payload"].clone();
        if let Some(changes) = case.get("payload_set") {
            for (key, value) in changes.as_object().unwrap() {
                payload[key] = value.clone();
            }
        }
        let mut result = corpus["result"].clone();
        for change in case["set"].as_array().unwrap() {
            edit(&mut result, &change[0], change[1].clone());
        }
        for path in case["remove"].as_array().unwrap() {
            remove(&mut result, path);
        }
        let (connector, _scratch) = harness(case["id"].as_str().unwrap(), result.clone(), MARKER);
        if let Some(archive) = payload.get("archive_id").and_then(Value::as_str) {
            connector
                .state
                .borrow_mut()
                .archives
                .insert((SESSION.into(), archive.into()));
            connector.state.borrow_mut().snapshot_seen_at = Some(connector.clock());
        }
        let submitted = request(payload.clone());
        let expected = if case["accept"] == json!(true) {
            json!({"state":"completed","result":result,"error":null})
        } else {
            json!({"state":"failed","result":null,"error":ERROR})
        };
        assert_eq!(
            connector.execute(submitted.clone()).await.unwrap(),
            Outcome::Finished(expected),
            "{}",
            case["id"]
        );
        {
            let calls = connector.native.calls.borrow();
            assert_eq!(calls.len(), 1, "{}", case["id"]);
            assert_eq!(calls[0].argv, ["history-projection", SESSION, "--json"]);
            assert_eq!(calls[0].payload.as_ref(), Some(&payload));
            assert_eq!(calls[0].timeout, Some(2.0));
            assert_eq!(calls[0].max_output_bytes, Some(1024 * 1024));
        }
        assert_eq!(
            connector.execute(submitted).await.unwrap(),
            Outcome::Replayed
        );
        assert_eq!(connector.native.calls.borrow().len(), 1);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn retained_hgs_refuses_projection_before_archive_lookup() {
    let corpus = document("history_projection/results.json");
    let mut payload = corpus["payload"].clone();
    payload["archive_id"] = json!("20000000-0000-4000-8000-000000000002");
    let (connector, _scratch) = harness(
        "old-hgs",
        corpus["result"].clone(),
        "hgs history <session> --json\nhgs archives --json",
    );
    let outcome = connector.execute(request(payload)).await.unwrap();
    assert_eq!(
        outcome,
        Outcome::Finished(
            json!({"state":"failed","result":null,"error":"native history projection ABI is unavailable"})
        )
    );
    assert!(connector.native.calls.borrow().is_empty());
}

#[tokio::test(flavor = "current_thread")]
async fn ordinary_history_preserves_projection_generation_metadata() {
    let corpus = document("history_projection/results.json");
    let mut submitted = request(corpus["payload"].clone());
    submitted["operation"] = json!("history");
    for key in ["history_epoch", "projection_after", "projection_until"] {
        submitted["payload"].as_object_mut().unwrap().remove(key);
    }
    let result = json!({"request_id":ID,"name":SESSION,"run_id":"run-one","conversation_id":"conversation-one",
        "events":[],"history_epoch":"epoch-one","head":{"complete":true},
        "source_status":{"projection_upgrade":{"state":"complete","generation":9}}});
    let (connector, _scratch) = harness(
        "ordinary-history-generation",
        result.clone(),
        "history <session> --json",
    );
    assert_eq!(
        connector.execute(submitted).await.unwrap(),
        Outcome::Finished(json!({"state":"completed","result":result,"error":null}))
    );
    let calls = connector.native.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].argv, ["history", SESSION, "--json"]);
}

#[tokio::test(flavor = "current_thread")]
async fn projection_enforces_256_entries_and_the_whole_ascii_wire_boundary() {
    let shared: Value = serde_json::from_str(include_str!(
        "../../../../mobile/android/app/src/test/resources/history-projection-wire.json"
    ))
    .unwrap();
    for case in shared["cases"].as_array().unwrap() {
        assert_eq!(
            codec::canonical(&case["value"]).unwrap(),
            case["ascii"].as_str().unwrap()
        );
        assert_eq!(
            codec::size(&case["value"], 1024).unwrap(),
            case["bytes"].as_u64().unwrap() as usize
        );
    }
    let corpus = document("history_projection/results.json");
    for count in [256, 257] {
        let mut payload = corpus["payload"].clone();
        payload["projection_until"] = json!(300);
        let mut result = corpus["result"].clone();
        result["projection_generation"] = json!(300);
        result["projection_changes"] = json!((1..=count).map(|n| json!({"generation":n,"history_id":"a".repeat(64),"change":"retired","resolution":"resolved","successor":null})).collect::<Vec<_>>());
        result["next_projection_after"] = json!(count);
        result["complete"] = json!(false);
        let (connector, _scratch) = harness("entry-boundary", result, MARKER);
        let Outcome::Finished(receipt) = connector.execute(request(payload)).await.unwrap() else {
            panic!("not a fresh request")
        };
        assert_eq!(
            receipt["state"],
            if count == 256 { "completed" } else { "failed" }
        );
        if count == 257 {
            assert_eq!(receipt["error"], ERROR);
        }
    }
    for excess in [0, 1] {
        let mut result = corpus["result"].clone();
        result["projection_changes"] = json!([]);
        result["unicode"] = json!("😀é");
        result["padding"] = json!("");
        let base = codec::size(&result, 880 * 1024).unwrap();
        result["padding"] = json!("x".repeat(880 * 1024 - base + excess));
        assert_eq!(
            codec::size(&result, 881 * 1024).unwrap(),
            880 * 1024 + excess
        );
        let (connector, _scratch) = harness("wire-boundary", result.clone(), MARKER);
        let Outcome::Finished(receipt) = connector
            .execute(request(corpus["payload"].clone()))
            .await
            .unwrap()
        else {
            panic!("not fresh")
        };
        if excess == 0 {
            assert_eq!(
                receipt,
                json!({"state":"completed","result":result,"error":null})
            );
            assert!(codec::python_default_len(&receipt, true).unwrap() <= 1024 * 1024);
        } else {
            assert_eq!(
                receipt,
                json!({"state":"failed","result":null,"error":ERROR})
            );
        }
    }
    let mut result = corpus["result"].clone();
    result["padding"] = json!(vec![0; 400_000]);
    assert!(codec::size(&result, 880 * 1024).is_ok());
    let (connector, _scratch) = harness("outer-receipt-boundary", result, MARKER);
    assert_eq!(
        connector
            .execute(request(corpus["payload"].clone()))
            .await
            .unwrap(),
        Outcome::Finished(
            json!({"state":"failed","result":null,"error":"native result exceeds relay size limit"})
        )
    );
}

#[tokio::test(flavor = "current_thread")]
async fn projection_peer_context_preserves_bound_scope_and_refuses_a_rebound_route() {
    let corpus = document("history_projection/results.json");
    let (gateway, _scratch) = harness(
        "projection-peer",
        corpus["result"].clone(),
        "mobile-peers --json\nmobile-peer --json",
    );
    let machine = "50000000-0000-4000-8000-000000000005";
    let computer = "40000000-0000-4000-8000-000000000004";
    let identity = route_id("peer.example.test", machine);
    let fleet = gateway.fleet.as_ref().unwrap();
    *fleet.local_id.borrow_mut() = Some(computer.into());
    fleet.routes.borrow_mut().insert(
        identity.clone(),
        Route {
            route_id: identity.clone(),
            machine_id: machine.into(),
            via: "peer.example.test".into(),
            name: "Peer".into(),
            online: true,
        },
    );
    let peer = Rc::new(gateway.peer_context(
        machine,
        PeerNative {
            gateway: gateway.native.clone(),
            routes: fleet.routes.clone(),
            slots: fleet.slots.clone(),
            identity: identity.clone(),
            machine: machine.into(),
            default_timeout: DEFAULT_TIMEOUT,
        },
    ));
    peer.flags.set(Flags::from_help(MARKER));
    peer.state.borrow_mut().sessions.insert(SESSION.into());
    peer.snapshot_ready.set();
    fleet.contexts.borrow_mut().insert(identity.clone(), peer);
    let mut submitted = request(corpus["payload"].clone());
    submitted["gateway_route"] =
        json!({"schema":1,"route_id":identity,"computer_id":computer,"machine_id":machine});
    assert_eq!(
        gateway.execute(submitted.clone()).await.unwrap(),
        Outcome::Finished(json!({"state":"completed","result":corpus["result"],"error":null}))
    );
    {
        let calls = gateway.native.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].argv, ["swarm", "mobile-peer", "--json"]);
        assert_eq!(
            calls[0].payload,
            Some(
                json!({"schema":1,"via":"peer.example.test","target_machine_id":machine,
            "argv":["history-projection",SESSION,"--json"],"payload":corpus["payload"]})
            )
        );
        assert_eq!(calls[0].timeout, Some(2.0));
        assert_eq!(calls[0].max_output_bytes, Some(1024 * 1024));
    }
    fleet
        .routes
        .borrow_mut()
        .get_mut(&identity)
        .unwrap()
        .machine_id = computer.into();
    let next = "60000000-0000-4000-8000-000000000006";
    submitted["request_id"] = json!(next);
    submitted["payload"]["request_id"] = json!(next);
    assert_eq!(
        gateway.execute(submitted).await.unwrap(),
        Outcome::Finished(
            json!({"state":"failed","result":null,"error":"direct peer route is unavailable or changed"})
        )
    );
    assert_eq!(gateway.native.calls.borrow().len(), 1);
}

#[test]
fn projection_journal_uses_the_read_cap_retention_and_restart_class() {
    assert!(!is_mutation("history_projection"));
    let scratch = Scratch::new("projection-retention");
    let state = scratch.path("state");
    let journal = Journal::open(&state, "projection-fixture", EPOCH).unwrap();
    let receipt =
        json!({"state":"completed","result":{"sentinel":"private synthetic"},"error":null});
    for (id, operation) in [
        ("delivered", "history_projection"),
        ("undelivered", "history_projection"),
        ("running", "history_projection"),
        ("mutation", "send"),
    ] {
        assert!(journal.claim(id, operation, None, EPOCH).unwrap());
        if id != "running" {
            journal.finish(id, &receipt, EPOCH).unwrap();
        }
        if ["delivered", "mutation"].contains(&id) {
            journal.delivered(id, EPOCH).unwrap();
        }
    }
    journal.prune(EPOCH + 3599.0).unwrap();
    let db = rusqlite::Connection::open(state.join(JOURNAL)).unwrap();
    let count = |id: &str| {
        db.query_row(
            "SELECT count(*) FROM requests WHERE request_id=?",
            [id],
            |r| r.get::<_, i64>(0),
        )
        .unwrap()
    };
    assert_eq!(count("delivered"), 1);
    journal.prune(EPOCH + 3601.0).unwrap();
    assert_eq!(count("delivered"), 0);
    for id in ["undelivered", "running", "mutation"] {
        assert_eq!(count(id), 1);
    }
    journal.prune(EPOCH + 8.0 * 86400.0).unwrap();
    assert_eq!(
        db.query_row(
            "SELECT response_expired FROM requests WHERE request_id='mutation'",
            [],
            |r| r.get::<_, i64>(0)
        )
        .unwrap(),
        1
    );
    close(journal);
    let restarted = Journal::open(&state, "projection-fixture", EPOCH + 3602.0).unwrap();
    let recovered: Value = serde_json::from_str(&restarted.replay("running").unwrap()).unwrap();
    assert_eq!(recovered["state"], "failed");
    close(restarted);
    drop(db);
    let scratch = Scratch::new("projection-cap");
    let state = scratch.path("state");
    let journal = Journal::open(&state, "projection-fixture", EPOCH).unwrap();
    let db = rusqlite::Connection::open(state.join(JOURNAL)).unwrap();
    db.execute_batch(&format!("WITH RECURSIVE n(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM n WHERE x<{READ_CAP}) INSERT INTO requests(request_id,operation,state,created_at,updated_at) SELECT CAST(x AS TEXT),'history_projection','running',{EPOCH},{EPOCH} FROM n")).unwrap();
    assert_eq!(
        journal
            .claim("beyond-read-cap", "history_projection", None, EPOCH)
            .map_err(|error| error.message().to_owned()),
        Err("connector retained request limit reached".to_owned())
    );
    assert!(journal
        .claim("independent-mutation-cap", "send", None, EPOCH)
        .unwrap());
}
