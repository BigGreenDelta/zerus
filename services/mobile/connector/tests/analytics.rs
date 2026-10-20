#![cfg(unix)]
//! Private aggregate transport, with synthetic native replies and journals.
mod common;

use common::{Scratch, EPOCH};
use serde_json::{json, Value};
use std::cell::RefCell;
use zerus_mobile_connector::{
    capabilities::Flags,
    execute::Outcome,
    journal::{is_mutation, Journal},
    native::{Call, Native, NativeResult, Output},
    runtime::{Connector, Settings},
};

const MACHINE: &str = "00000000-0000-4000-8000-000000000001";
const REQUEST: &str = "00000000-0000-4000-8000-000000000002";
const MARKER: &str = "hgs [@host] analytics --json";

fn request() -> Value {
    json!({"request_id": REQUEST, "computer_id": MACHINE, "operation":"analytics_read",
        "session":"", "payload":{"schema":1,"from":"2026-01-01","through":"2026-03-31"}})
}

fn summary() -> Value {
    json!({"schema":1,"machine_id":MACHINE,"source":"agent","source_epoch":REQUEST,
        "revision":3,"generated_at":1800000000.25,
        "range":{"from":"2026-01-01","through":"2026-03-31"},
        "collection":{"enabled":false},"coverage":{"complete":false},"buckets":[]})
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
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    Connector::new(
        Fake {
            calls: RefCell::new(vec![]),
            reply,
        },
        journal,
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
fn capability_is_selected_native_marker_gated_and_read_only() {
    assert!(!is_mutation("analytics_read"));
    for (help, offered) in [("", false), ("analytics --json", false), (MARKER, true)] {
        let flags = Flags::from_help(help);
        assert_eq!(
            flags.advertised(false)["operations"]
                .as_array()
                .unwrap()
                .iter()
                .any(|op| op == "analytics_read"),
            offered
        );
    }
}

#[tokio::test(flavor = "current_thread")]
async fn native_summary_is_bounded_unchanged_and_receipt_deduplicated() {
    let scratch = Scratch::new("analytics-summary");
    let node = connector(&scratch, summary());
    node.flags.set(Flags::from_help(MARKER));
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "completed", "{response}");
    assert_eq!(response["result"], summary());
    assert_eq!(node.execute(request()).await.unwrap(), Outcome::Replayed);
    let calls = node.native.calls.borrow();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].argv, ["analytics", "--json"]);
    assert_eq!(
        calls[0].payload,
        Some(json!({"schema":1,"action":"summary","from":"2026-01-01","through":"2026-03-31"}))
    );
    assert_eq!(calls[0].max_output_bytes, Some(1024 * 1024));
    assert_eq!(calls[0].timeout, Some(10.0));
}

#[tokio::test(flavor = "current_thread")]
async fn unsupported_cli_and_remote_collection_mutations_never_dispatch() {
    let scratch = Scratch::new("analytics-unsupported");
    let node = connector(&scratch, summary());
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(node.native.calls.borrow().is_empty());
    node.flags.set(Flags::from_help(MARKER));
    let mut bad = request();
    bad["request_id"] = json!("00000000-0000-4000-8000-000000000003");
    bad["payload"]["action"] = json!("purge");
    let Outcome::Finished(response) = node.execute(bad).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(node.native.calls.borrow().is_empty());
}

#[tokio::test(flavor = "current_thread")]
async fn different_range_is_refused_without_retaining_aggregate_reply() {
    let scratch = Scratch::new("analytics-wrong-range");
    let mut wrong = summary();
    wrong["range"]["through"] = json!("2026-03-30");
    let node = connector(&scratch, wrong);
    node.flags.set(Flags::from_help(MARKER));
    let Outcome::Finished(response) = node.execute(request()).await.unwrap() else {
        panic!()
    };
    assert_eq!(response["state"], "failed");
    assert!(response["result"].is_null());
}

#[test]
fn delivered_analytics_expire_after_two_minutes_pending_uploads_remain() {
    let scratch = Scratch::new("analytics-retention");
    let journal = Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap();
    let pending = "00000000-0000-4000-8000-000000000003";
    let receipt = json!({"state":"completed","result":summary(),"error":null});
    for id in [REQUEST, pending] {
        assert!(journal
            .claim(id, "analytics_read", Some("fixture-hash"), EPOCH)
            .unwrap());
        journal.finish(id, &receipt, EPOCH).unwrap();
    }
    journal.delivered(REQUEST, EPOCH).unwrap();
    journal.prune(EPOCH + 121.0).unwrap();
    assert_eq!(journal.pending().unwrap().len(), 1);
    assert_eq!(journal.pending().unwrap()[0].0, pending);
    assert!(journal
        .claim(
            REQUEST,
            "analytics_read",
            Some("fixture-hash"),
            EPOCH + 121.0
        )
        .unwrap());
}
