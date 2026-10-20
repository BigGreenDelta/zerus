#![cfg(unix)]
//! Exact metadata mutation boundary: a lost acknowledgement never permits replay.
//! This Unix fixture uses the supported project-mutation path. `windows_scope`
//! separately checks that Windows refuses these operations before native calls.
mod common;

use common::*;
use serde_json::{json, Value};
use std::cell::Cell;
use zerus_mobile_connector::capabilities::Flags;
use zerus_mobile_connector::execute::Outcome;
use zerus_mobile_connector::journal::{Journal, RESTART_ERROR};
use zerus_mobile_connector::native::{self, Call, Native, NativeError, NativeResult, Output};
use zerus_mobile_connector::runtime::{Connector, Settings};

const SESSION: &str = "codex/example/session";
const ID: &str = "11111111-1111-4111-8111-111111111111";
const MACHINE: &str = "22222222-2222-4222-8222-222222222222";

struct SyntheticNative {
    calls: Cell<usize>,
    saved_project: Cell<bool>,
    reply: Result<Value, &'static str>,
}

impl Native for SyntheticNative {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.set(self.calls.get() + 1);
        assert!(
            call.argv == ["swarm", "assign-session", "--json"]
                || call.argv == ["swarm", "session-project", "--json"]
        );
        assert_eq!(call.payload.as_ref().unwrap()["name"], SESSION);
        assert_eq!(call.payload.as_ref().unwrap()["request_id"], ID);
        if call.argv[1] == "assign-session" {
            // Model the native catalog already saved before its acknowledgement is lost.
            self.saved_project.set(true);
        }
        match &self.reply {
            Ok(value) => Ok(Output::Json(value.clone())),
            Err(message) => Err(NativeError::Failed(message)),
        }
    }
}

fn request(operation: &str) -> Value {
    let mut payload = json!({"request_id":ID,"expected_run_id":"run-one",
        "expected_conversation_id":"one","expected_machine_id":MACHINE});
    if operation == "project_assign" {
        payload["swarm_id"] = json!("33333333-3333-4333-8333-333333333333");
        payload["project_id"] = json!("two");
        payload["membership_token"] = json!("a".repeat(64));
        payload["catalog_epoch"] = json!("b".repeat(64));
    }
    json!({"request_id":ID,"operation":operation,"session":SESSION,"payload":payload})
}

fn connector(scratch: &Scratch, reply: Result<Value, &'static str>) -> Connector<SyntheticNative> {
    let journal =
        Journal::open(&scratch.path("state"), "synthetic-project-binding", EPOCH).unwrap();
    let settings = Settings {
        interval: 5.0,
        binding: "synthetic-project-binding".into(),
        default_timeout: native::DEFAULT_TIMEOUT,
        clock_scale: 1.0,
    };
    let connector = Connector::new(
        SyntheticNative {
            calls: Cell::new(0),
            saved_project: Cell::new(false),
            reply,
        },
        journal,
        settings,
        Box::new(|| EPOCH),
    );
    connector.flags.set(Flags::from_help(
        "swarm session-project --json\nswarm assign-session --json\nMobile project assignment ABI: session-project-v1",
    ));
    connector.state.borrow_mut().sessions.insert(SESSION.into());
    connector
}

#[tokio::test(flavor = "current_thread")]
async fn saved_project_with_lost_or_invalid_ack_is_uncertain_and_duplicate_never_calls_native() {
    for reply in [
        Err(native::TIMED_OUT),
        Ok(json!({"status":"assigned","project_id":"two"})),
    ] {
        umask();
        let scratch = Scratch::new("project-unknown-boundary");
        let connector = connector(&scratch, reply);
        let submission = request("project_assign");
        let Outcome::Finished(result) = connector.execute(submission.clone()).await.unwrap() else {
            panic!("new request was not executed")
        };
        assert_eq!(result["state"], "uncertain", "{result}");
        assert!(connector.native.saved_project.get());
        assert_eq!(connector.native.calls.get(), 1);
        let pending = connector.journal.pending().unwrap();
        assert_eq!(pending.len(), 1);
        assert_eq!(pending[0].0, ID);
        assert_eq!(
            serde_json::from_str::<Value>(&pending[0].1).unwrap()["state"],
            "uncertain"
        );
        assert_eq!(
            connector.execute(submission).await.unwrap(),
            Outcome::Replayed
        );
        assert_eq!(connector.native.calls.get(), 1);
    }
}

#[tokio::test(flavor = "current_thread")]
async fn read_failure_and_stopped_before_write_remain_definite_without_native_mutation() {
    for operation in ["project_assignment", "project_assign"] {
        umask();
        let scratch = Scratch::new("project-read-stop-boundary");
        let connector = connector(&scratch, Err(native::TIMED_OUT));
        if operation == "project_assign" {
            connector.stopping.set();
        }
        let submission = request(operation);
        let Outcome::Finished(result) = connector.execute(submission.clone()).await.unwrap() else {
            panic!("new request had no receipt")
        };
        assert_eq!(result["state"], "failed", "{result}");
        assert_eq!(
            result["error"],
            if operation == "project_assign" {
                RESTART_ERROR
            } else {
                native::TIMED_OUT
            }
        );
        assert!(!connector.native.saved_project.get());
        assert_eq!(
            connector.native.calls.get(),
            usize::from(operation == "project_assignment")
        );
        assert_eq!(
            connector.execute(submission).await.unwrap(),
            Outcome::Replayed
        );
        assert_eq!(
            connector.native.calls.get(),
            usize::from(operation == "project_assignment")
        );
    }
}
