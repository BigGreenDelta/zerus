#![cfg(unix)]
mod common;
use common::{Scratch, EPOCH};
use ring::digest::{digest, SHA256};
use serde_json::{json, Value};
use std::cell::RefCell;
use zerus_mobile_connector::{
    capabilities::Flags,
    execute::Outcome,
    journal::{is_mutation, Journal},
    native::{Call, Native, NativeResult, Output},
    repositories,
    runtime::{Connector, Settings},
};
const ID: &str = "11111111-1111-4111-8111-111111111111";
const MACHINE: &str = "22222222-2222-4222-8222-222222222222";
fn request(op: &str) -> Value {
    let mut p = json!({"request_id":ID,"expected_machine_id":MACHINE,"url":"git@forge:owner/source.git","parent":"/fixture/parent","name":"checkout ; $ literal"});
    if op == "repository_status" {
        p["clone_request_id"] = json!(MACHINE);
    }
    json!({"request_id":ID,"computer_id":"fixture","operation":op,"session":"","payload":p})
}
fn native(op: &str, p: &Value) -> Value {
    let bytes = serde_json::to_vec(&json!([p["url"], p["parent"], p["name"]])).unwrap();
    let fp: String = digest(&SHA256, &bytes)
        .as_ref()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect();
    json!({"status":if op=="repository_check" {"ready"} else {"cloned"},"request_id":if op=="repository_status" {&p["clone_request_id"]} else {&p["request_id"]},
        "fingerprint":fp,"parent":p["parent"],"name":p["name"],"path":format!("{}/{}",p["parent"].as_str().unwrap(),p["name"].as_str().unwrap()),"staging_path":"private-internal-path"})
}
struct Fake {
    calls: RefCell<Vec<Call>>,
    reply: Value,
}
impl Native for Fake {
    async fn call(&self, c: Call) -> NativeResult {
        self.calls.borrow_mut().push(c);
        Ok(Output::Json(self.reply.clone()))
    }
}
fn node(s: &Scratch, reply: Value) -> Connector<Fake> {
    Connector::new(
        Fake {
            calls: RefCell::new(vec![]),
            reply,
        },
        Journal::open(&s.path("state"), "fixture", EPOCH).unwrap(),
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
fn older_native_and_python_never_offer_acquisition() {
    for text in [
        "",
        "Repository acquisition ABI: repository-clone-v1",
        "Mobile repository acquisition ABI: repository-acquisition-v10",
    ] {
        assert!(!Flags::from_help(text).repository);
    }
    let f = Flags::from_help(repositories::HELP_MARKER);
    assert!(f.repository);
    for op in zerus_relay::repositories::OPERATIONS {
        assert!(!f.python_operations(true).contains(op));
        assert!(f.advertised(false)["operations"]
            .as_array()
            .unwrap()
            .iter()
            .any(|v| v == op));
    }
    assert!(is_mutation("repository_clone"));
    assert!(!is_mutation("repository_check"));
    assert!(!is_mutation("repository_status"));
}
#[tokio::test(flavor = "current_thread")]
async fn guarded_native_identity_original_uuid_and_no_replay() {
    for op in zerus_relay::repositories::OPERATIONS {
        let s = Scratch::new(op);
        let r = request(op);
        let n = node(&s, native(op, &r["payload"]));
        n.flags.set(Flags::from_help(repositories::HELP_MARKER));
        let Outcome::Finished(receipt) = n.execute(r.clone()).await.unwrap() else {
            panic!()
        };
        assert_eq!(receipt["state"], "completed", "{receipt}");
        assert!(receipt["result"].get("staging_path").is_none());
        assert_eq!(n.execute(r.clone()).await.unwrap(), Outcome::Replayed);
        let mut changed = r.clone();
        changed["payload"]["url"] = json!("git@forge:other/repo");
        assert!(matches!(
            n.execute(changed).await.unwrap(),
            Outcome::Rejected(_)
        ));
        let calls = n.native.calls.borrow();
        assert_eq!(calls.len(), 1);
        assert_eq!(calls[0].argv, ["swarm", "__mobile-peer-local", "--json"]);
        let guarded = calls[0].payload.as_ref().unwrap();
        assert_eq!(guarded["target_machine_id"], MACHINE);
        assert_eq!(
            guarded["argv"],
            json!(repositories::call(op, &r["payload"]).argv)
        );
    }
}
#[tokio::test(flavor = "current_thread")]
async fn missing_marker_and_wrong_route_never_reach_native() {
    let s = Scratch::new("repo-refusals");
    let r = request("repository_clone");
    let n = node(&s, native("repository_clone", &r["payload"]));
    let Outcome::Finished(receipt) = n.execute(r).await.unwrap() else {
        panic!()
    };
    assert_eq!(receipt["state"], "failed");
    assert!(n.native.calls.borrow().is_empty());
    n.flags.set(Flags::from_help(repositories::HELP_MARKER));
    let mut r = request("repository_clone");
    r["request_id"] = json!(MACHINE);
    r["payload"]["request_id"] = json!(MACHINE);
    r["gateway_route"] = json!({"computer_id":"fixture","machine_id":ID});
    let Outcome::Finished(receipt) = n.execute(r).await.unwrap() else {
        panic!()
    };
    assert_eq!(receipt["state"], "failed");
    assert!(n.native.calls.borrow().is_empty());
}
#[test]
fn receipt_projection_requires_original_fingerprint_and_exact_path() {
    for op in ["repository_clone", "repository_status"] {
        let r = request(op);
        let p = &r["payload"];
        let raw = native(op, p);
        repositories::project(op, p, &raw).unwrap();
        for key in ["fingerprint", "request_id", "path", "parent", "name"] {
            let mut wrong = raw.clone();
            wrong[key] = json!("wrong");
            assert!(repositories::project(op, p, &wrong).is_err(), "{key}");
        }
        let mut failed = raw.clone();
        failed["status"] = json!("failed");
        failed["detail"] = json!("界\\\"".repeat(6000));
        let result = repositories::project(op, p, &failed).unwrap();
        assert!(serde_json::to_vec(&result).unwrap().len() < 16384);
        let mut long = p.clone();
        long["parent"] = json!(format!("/{}", "p".repeat(3800)));
        let mut error = native(op, &long);
        error["status"] = json!("failed");
        error["detail"] = json!("\u{0001}".repeat(16000));
        let bounded = repositories::project(op, &long, &error).unwrap();
        assert!(serde_json::to_vec(&bounded).unwrap().len() <= repositories::MAX_RECEIPT_BYTES);
        let call = repositories::call(op, p);
        assert_eq!(
            call.timeout,
            Some(if op == "repository_clone" {
                repositories::CLONE_TIMEOUT
            } else {
                30.0
            })
        );
    }
}

#[test]
fn additive_reads_expire_without_losing_clone_tombstone() {
    let s = Scratch::new("repository-journal");
    let journal = Journal::open(&s.path("state"), "fixture", EPOCH).unwrap();
    for (index, op) in ["repository_check", "repository_status", "repository_clone"]
        .iter()
        .enumerate()
    {
        let id = format!("00000000-0000-4000-8000-{index:012}");
        assert!(journal.claim(&id, op, Some("fingerprint"), EPOCH).unwrap());
        journal
            .finish(
                &id,
                &json!({"state":"completed","result":null,"error":null}),
                EPOCH,
            )
            .unwrap();
        journal.delivered(&id, EPOCH).unwrap();
    }
    journal.prune(EPOCH + 3601.0).unwrap();
    assert!(journal
        .claim(
            "00000000-0000-4000-8000-000000000000",
            "repository_check",
            Some("fingerprint"),
            EPOCH + 3601.0
        )
        .unwrap());
    assert!(journal
        .claim(
            "00000000-0000-4000-8000-000000000001",
            "repository_status",
            Some("fingerprint"),
            EPOCH + 3601.0
        )
        .unwrap());
    assert!(!journal
        .claim(
            "00000000-0000-4000-8000-000000000002",
            "repository_clone",
            Some("fingerprint"),
            EPOCH + 3601.0
        )
        .unwrap());
}

#[test]
fn read_only_check_accepts_canonical_alias_but_mutations_keep_exact_destination() {
    let mut r = request("repository_check");
    r["payload"]["parent"] = json!("/var/tmp");
    let p = &r["payload"];
    let mut raw = native("repository_check", p);
    raw["parent"] = json!("/private/var/tmp");
    raw["path"] = json!(format!("/private/var/tmp/{}", p["name"].as_str().unwrap()));
    let checked = repositories::project("repository_check", p, &raw).unwrap();
    assert_eq!(checked["parent"], "/private/var/tmp");
    for op in ["repository_clone", "repository_status"] {
        let mut r = request(op);
        r["payload"]["parent"] = json!("/var/tmp");
        let mut changed = native(op, &r["payload"]);
        changed["parent"] = raw["parent"].clone();
        changed["path"] = raw["path"].clone();
        assert!(repositories::project(op, &r["payload"], &changed).is_err());
        r["payload"]["parent"] = raw["parent"].clone();
        repositories::project(op, &r["payload"], &native(op, &r["payload"])).unwrap();
    }
}
