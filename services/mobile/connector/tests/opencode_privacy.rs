#![cfg(unix)]
//! The real connector claim/route/receipt path with memory native replies and
//! a private SQLite journal. No provider, HTTP server or native agent runs.
mod common;

use common::{Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use zerus_mobile_connector::execute::Outcome;
use zerus_mobile_connector::fleet::{route_id, PeerNative, Route};
use zerus_mobile_connector::journal::Journal;
use zerus_mobile_connector::native::{Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::opencode_privacy as privacy;
use zerus_mobile_connector::runtime::{Connector, Settings};

const LOCAL: &str = "11111111-1111-4111-8111-111111111111";
const PEER: &str = "22222222-2222-4222-8222-222222222222";
const COMPUTER: &str = "33333333-3333-4333-8333-333333333333";
const SESSION: &str = "custom/example/session";
const PRIVATE: &str =
    "https://provider.example.invalid/v1?key=fixture-YO85-DIAGNOSTIC\u{1b}[31m\u{7}";

struct Fixture {
    calls: RefCell<Vec<Value>>,
    status: Cell<&'static str>,
    local_provider: Cell<&'static str>,
}

impl Fixture {
    fn row(&self, machine: &str) -> Value {
        let cmd = if machine == LOCAL {
            self.local_provider.get()
        } else {
            "opencode"
        };
        json!({"name":SESSION,"cmd":cmd,"run_id":"run","conversation_id":"ses_A",
            "activity":"unknown","phase":"unknown","inspection_unavailable":true,
            "inspection_reason":PRIVATE,"activity_detail":PRIVATE,
            "events":[{"type":"AgentMessage","detail":"literal reply","at":1}],
            "pending_questions":[{"question_id":"q1","title":"literal question"}]})
    }
}

impl Native for Fixture {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls
            .borrow_mut()
            .push(json!({"argv":call.argv,"payload":call.payload}));
        let envelope = call.payload.as_ref();
        let guarded = call.argv.first().map(String::as_str) == Some("swarm")
            && call
                .argv
                .get(1)
                .is_some_and(|arg| ["mobile-peer", "__mobile-peer-local"].contains(&arg.as_str()));
        let machine = if guarded {
            envelope.unwrap()["target_machine_id"].as_str().unwrap()
        } else {
            LOCAL
        };
        let argv: Vec<&str> = if guarded {
            envelope.unwrap()["argv"]
                .as_array()
                .unwrap()
                .iter()
                .map(|arg| arg.as_str().unwrap())
                .collect()
        } else {
            call.argv.iter().map(String::as_str).collect()
        };
        let payload = if guarded {
            envelope.unwrap().get("payload")
        } else {
            envelope
        };
        if argv == ["--help"] {
            return Ok(Output::Text("hgs session-action <session> --json\nhgs mobile-peers --json\nhgs mobile-peer --json".into()));
        }
        let result = match argv.as_slice() {
            ["ls", "--json", "--local"] => json!({"sessions":[self.row(machine)]}),
            ["inspect", SESSION] => self.row(machine),
            ["session-action", SESSION, "--json"] => {
                let status = self.status.get();
                let mut result = json!({"request_id":payload.unwrap()["request_id"],"name":SESSION,
                    "run_id":"run","conversation_id":"ses_A","status":status});
                if status == "completed" {
                    result["result_target"] = json!({"name":"custom/example/new","run_id":"run","conversation_id":"ses_A"});
                } else {
                    // An older hgs response has no error_source marker.
                    result["error"] = json!(PRIVATE);
                }
                result
            }
            ["account", "ls"] => json!({"profiles":[]}),
            ["swarm", "get"] => json!({"projects":[]}),
            _ => return Err(NativeError::Failed("native command failed")),
        };
        Ok(Output::Json(result))
    }
}

struct Harness {
    connector: Rc<Connector<Fixture>>,
    peer: Rc<Connector<PeerNative>>,
    route: String,
    next_request: Cell<u64>,
    _scratch: Scratch,
}

impl Harness {
    async fn new(label: &str) -> Self {
        let scratch = Scratch::new(label);
        let journal = Journal::open(&scratch.path("state"), "fixture-binding", EPOCH).unwrap();
        let connector = Rc::new(Connector::new(
            Fixture {
                calls: RefCell::default(),
                status: Cell::new("failed"),
                local_provider: Cell::new("opencode"),
            },
            journal,
            Settings {
                interval: 5.0,
                binding: "fixture-binding".into(),
                default_timeout: 3.0,
                clock_scale: 1.0,
            },
            Box::new(|| EPOCH),
        ));
        let identity = route_id("fixture-peer", PEER);
        let fleet = connector.fleet.as_ref().unwrap();
        *fleet.local_id.borrow_mut() = Some(LOCAL.into());
        fleet.routes.borrow_mut().insert(
            identity.clone(),
            Route {
                route_id: identity.clone(),
                machine_id: PEER.into(),
                via: "fixture-peer".into(),
                name: "Peer".into(),
                online: true,
            },
        );
        let peer = Rc::new(connector.peer_context(
            PEER,
            PeerNative {
                gateway: connector.native.clone(),
                routes: fleet.routes.clone(),
                slots: fleet.slots.clone(),
                identity: identity.clone(),
                machine: PEER.into(),
                default_timeout: 3.0,
            },
        ));
        fleet
            .contexts
            .borrow_mut()
            .insert(identity.clone(), peer.clone());
        connector.snapshot().await.unwrap();
        peer.snapshot().await.unwrap();
        Harness {
            connector,
            peer,
            route: identity,
            next_request: Cell::new(0),
            _scratch: scratch,
        }
    }

    fn request(&self, operation: &str, peer: bool) -> Value {
        let next = self.next_request.get() + 1;
        self.next_request.set(next);
        let id = format!("44444444-4444-4444-8444-{next:012x}");
        let payload = if operation == "inspect" {
            json!({})
        } else {
            json!({"request_id":id,"expected_run_id":"run","expected_conversation_id":"ses_A","new_name":"custom/example/new"})
        };
        json!({"request_id":id,"operation":operation,"session":SESSION,"payload":payload,
            "gateway_route":{"schema":1,"route_id":if peer { json!(self.route) } else { Value::Null },
                "computer_id":COMPUTER,"machine_id":if peer { PEER } else { LOCAL }}})
    }

    async fn run(&self, request: Value) -> Value {
        match self.connector.execute(request).await.unwrap() {
            Outcome::Finished(value) => value,
            other => panic!("unexpected {other:?}"),
        }
    }

    async fn duplicate(&self, request: Value, first: &Value) {
        let count = self.connector.native.calls.borrow().len();
        assert!(matches!(
            self.connector.execute(request.clone()).await.unwrap(),
            Outcome::Replayed
        ));
        assert_eq!(self.connector.native.calls.borrow().len(), count);
        let stored = self
            .connector
            .journal
            .replay(request["request_id"].as_str().unwrap())
            .unwrap();
        assert_eq!(serde_json::from_str::<Value>(&stored).unwrap(), *first);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn local_and_guarded_peer_rows_and_inspections_reach_only_safe_journal_receipts() {
    let h = Harness::new("opencode-private-inspect").await;
    for peer in [false, true] {
        let snapshot = if peer {
            h.peer.snapshot().await.unwrap()
        } else {
            h.connector.snapshot().await.unwrap()
        };
        let row = &snapshot["sessions"][0];
        assert_eq!(row["inspection_reason"], privacy::INSPECTION);
        assert_eq!(row["activity_detail"], privacy::INSPECTION);
        assert_eq!(row["events"], h.connector.native.row(LOCAL)["events"]);
        assert_eq!(
            row["pending_questions"],
            h.connector.native.row(LOCAL)["pending_questions"]
        );
        assert!(!snapshot.to_string().contains("fixture-YO85-DIAGNOSTIC"));
        let request = h.request("inspect", peer);
        let first = h.run(request.clone()).await;
        assert_eq!(first["state"], "completed");
        assert_eq!(first["result"]["inspection_reason"], privacy::INSPECTION);
        assert_eq!(
            first["result"]["pending_questions"],
            row["pending_questions"]
        );
        h.duplicate(request, &first).await;
    }
    let stored = h.connector.journal.pending().unwrap();
    assert!(!serde_json::to_string(&stored)
        .unwrap()
        .contains("fixture-YO85-DIAGNOSTIC"));
    let calls = h.connector.native.calls.borrow();
    assert!(calls
        .iter()
        .any(|call| call["argv"] == json!(["swarm", "mobile-peer", "--json"])));
    assert!(calls
        .iter()
        .any(|call| call["argv"] == json!(["swarm", "__mobile-peer-local", "--json"])));
}

#[tokio::test(flavor = "current_thread")]
async fn local_and_peer_actions_keep_status_identity_and_duplicate_no_replay() {
    let h = Harness::new("opencode-private-actions").await;
    for peer in [false, true] {
        for (status, error) in [
            ("failed", Some(privacy::ACTION_FAILED)),
            ("uncertain", Some(privacy::ACTION_UNCERTAIN)),
            ("completed", None),
        ] {
            h.connector.native.status.set(status);
            let request = h.request("rename", peer);
            let first = h.run(request.clone()).await;
            assert_eq!(first["state"], status);
            assert_eq!(
                first["error"],
                error.map_or(Value::Null, |text| json!(text))
            );
            assert_eq!(first["result"]["request_id"], request["request_id"]);
            assert_eq!(first["result"]["run_id"], "run");
            assert_eq!(first["result"]["conversation_id"], "ses_A");
            if status == "completed" {
                assert_eq!(
                    first["result"]["result_target"]["name"],
                    "custom/example/new"
                );
            }
            h.duplicate(request, &first).await;
            if peer {
                h.peer.snapshot().await.unwrap();
            } else {
                h.connector.snapshot().await.unwrap();
            }
        }
    }
    assert!(
        !serde_json::to_string(&h.connector.journal.pending().unwrap())
            .unwrap()
            .contains("fixture-YO85-DIAGNOSTIC")
    );
}

#[tokio::test(flavor = "current_thread")]
async fn provider_classification_is_current_and_does_not_cross_machine_contexts() {
    let h = Harness::new("opencode-private-identities").await;
    h.connector.native.local_provider.set("codex");
    h.connector.snapshot().await.unwrap();
    assert!(!h
        .connector
        .state
        .borrow()
        .opencode_sessions
        .contains(SESSION));
    assert!(h.peer.state.borrow().opencode_sessions.contains(SESSION));
    let local = h.run(h.request("rename", false)).await;
    assert_eq!(local["error"], PRIVATE);
    let peer = h.run(h.request("rename", true)).await;
    assert_eq!(peer["error"], privacy::ACTION_FAILED);
}
