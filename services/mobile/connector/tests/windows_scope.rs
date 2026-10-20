#![cfg(unix)]
//! Exercise the Windows policy through real durable journal dispatch on Unix.
//! This is portable policy evidence, not Windows runtime qualification.
mod common;
use common::{document, Scratch, EPOCH};
use serde_json::json;
use std::cell::Cell;
use zerus_mobile_connector::{
    capabilities::{Flags, WINDOWS_HELP_MARKER, WINDOWS_OPERATIONS},
    execute::Outcome,
    journal::Journal,
    native::{Call, Native, NativeResult},
    runtime::{Connector, Settings},
};
struct NeverNative(Cell<usize>);
impl Native for NeverNative {
    async fn call(&self, _: Call) -> NativeResult {
        self.0.set(self.0.get() + 1);
        panic!("unsupported Windows operation reached native");
    }
}
#[test]
fn windows_advertisement_rejects_unix_and_unsupported_provider_features() {
    let help = document("operations.json")["capabilities"]["help_text_all"]
        .as_str()
        .unwrap()
        .to_owned();
    let flags = Flags::from_help(&format!("{help}\n{WINDOWS_HELP_MARKER}\n"));
    let offered = flags.advertised(true);
    for operation in offered["operations"].as_array().unwrap() {
        assert!(
            WINDOWS_OPERATIONS.contains(&operation.as_str().unwrap()),
            "{operation}"
        );
    }
    for operation in [
        "send",
        "answer",
        "inspect",
        "history",
        "terminal_snapshot",
        "terminal_input",
    ] {
        assert!(
            offered["operations"]
                .as_array()
                .unwrap()
                .contains(&json!(operation)),
            "{operation}"
        );
    }
    for feature in [
        "send_agent",
        "gateway_one_hop",
        "voice_video",
        "opencode_launch_effort",
        "launch_project",
    ] {
        assert!(
            !offered["features"]
                .as_array()
                .unwrap()
                .contains(&json!(feature)),
            "{feature}"
        );
    }
    assert!(!flags.peers && !flags.launch && !flags.voice && !flags.speech);
}
#[tokio::test(flavor = "current_thread")]
async fn media_and_path_refusals_are_durable_and_cannot_replay_after_scope_changes() {
    let scratch = Scratch::new("windows-scope");
    let connector = Connector::new(
        NeverNative(Cell::new(0)),
        Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap(),
        Settings {
            interval: 5.0,
            binding: "fixture".into(),
            default_timeout: 45.0,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    );
    for (index, operation) in [
        "voice_submit",
        "speech_submit",
        "file_read",
        "launch",
        "worktree_create",
        "git_status",
    ]
    .iter()
    .enumerate()
    {
        connector.flags.set(Flags::default().windows_local());
        let request = json!({"request_id":format!("00000000-0000-4000-8000-{index:012}"),"operation":operation,"session":"codex/fixture/windows","payload":{}});
        let first = match connector.execute(request.clone()).await.unwrap() {
            Outcome::Finished(value) => value,
            other => panic!("{other:?}"),
        };
        assert_eq!(first["state"], "failed");
        assert!(first["error"]
            .as_str()
            .unwrap()
            .contains("Windows local gateway"));
        connector.flags.set(Flags::default());
        assert_eq!(connector.execute(request).await.unwrap(), Outcome::Replayed);
    }
    assert_eq!(connector.native.0.get(), 0);
}

use serde_json::Value;
use std::cell::RefCell;
use zerus_mobile_connector::native::Output;
struct Providers {
    provider: RefCell<Value>,
    page: Value,
    calls: RefCell<Vec<Vec<String>>>,
    writes: Cell<usize>,
}
impl Native for Providers {
    async fn call(&self, call: Call) -> NativeResult {
        self.calls.borrow_mut().push(call.argv.clone());
        let provider = self.provider.borrow().clone();
        if call.argv.first().map(String::as_str) == Some("archives") {
            if call.argv.get(1).map(String::as_str) == Some("lookup") {
                return Ok(Output::Json(
                    json!({"version":1,"name":"codex/project/test","archive_id":ARCHIVE,
                    "run_id":"run","conversation_id":"conversation","cmd":provider}),
                ));
            }
            return Ok(Output::Json(self.page.clone()));
        }
        if call.argv.first().map(String::as_str) == Some("inspect") {
            let mut detail = json!({"name":"codex/project/test","run_id":"run","conversation_id":"conversation","agent":provider});
            if call.argv.iter().any(|v| v == "--archive") {
                detail["archive_id"] = json!(ARCHIVE);
            }
            return Ok(Output::Json(detail));
        }
        assert_eq!(call.argv.first().map(String::as_str), Some("send"));
        self.writes.set(self.writes.get() + 1);
        Ok(Output::Json(json!({"status":"submitted"})))
    }
}
const ARCHIVE: &str = "00000000-0000-4000-8000-000000000001";
fn providers(
    scratch: &Scratch,
    windows: bool,
    provider: Value,
    rows: Vec<Value>,
) -> Connector<Providers> {
    let connector = Connector::new(
        Providers {
            provider: RefCell::new(provider),
            page: json!({"version":1,"sessions":rows,"total":99,"position":7,"previous_cursor":"previous","next_cursor":"next","from":0,"until":253402300800u64,"query":""}),
            calls: RefCell::new(Vec::new()),
            writes: Cell::new(0),
        },
        Journal::open(&scratch.path("state"), "fixture", EPOCH).unwrap(),
        Settings {
            interval: 5.0,
            binding: "fixture".into(),
            default_timeout: 45.0,
            clock_scale: 1.0,
        },
        Box::new(|| EPOCH),
    );
    let flags = Flags {
        archive: true,
        ..Flags::default()
    };
    connector.flags.set(if windows {
        flags.windows_local()
    } else {
        flags
    });
    connector
        .state
        .borrow_mut()
        .sessions
        .insert("codex/project/test".into());
    connector
}
fn archive_row(provider: Value, index: usize) -> Value {
    json!({"name":"codex/project/test","archive_id":format!("00000000-0000-4000-8000-{index:012}"),"cmd":provider,"state":"archived","archived_at":100-index})
}
fn finished(outcome: Outcome) -> Value {
    match outcome {
        Outcome::Finished(v) => v,
        other => panic!("{other:?}"),
    }
}
#[tokio::test(flavor = "current_thread")]
async fn archive_pages_refuse_mixed_providers_without_fabricating_totals_or_admission() {
    for windows in [true, false] {
        for provider in [
            json!("codex"),
            json!("claude"),
            json!("kimi"),
            json!("opencode"),
            json!("dsh"),
            json!("unknown"),
            Value::Null,
        ] {
            let scratch = Scratch::new("archive-provider-page");
            let rows = vec![
                archive_row(json!("codex"), 1),
                archive_row(provider.clone(), 2),
            ];
            let connector = providers(&scratch, windows, provider.clone(), rows.clone());
            let request = json!({"request_id":"00000000-0000-4000-8000-000000000010","operation":"archive_list","session":"","payload":{}});
            let answer = finished(connector.execute(request.clone()).await.unwrap());
            let supported =
                !windows || zerus_mobile_connector::capabilities::windows_provider(&provider);
            assert_eq!(
                answer["state"],
                if supported { "completed" } else { "failed" },
                "{provider} {answer}"
            );
            assert_eq!(
                connector.state.borrow().paged_archives.len(),
                if supported { 2 } else { 0 }
            );
            if supported {
                assert_eq!(answer["result"]["sessions"], json!(rows));
                for field in ["total", "position", "next_cursor", "previous_cursor"] {
                    assert_eq!(answer["result"][field], connector.native.page[field]);
                }
            }
            let argv = connector.native.calls.borrow()[0].clone();
            assert_eq!(
                argv.last().map(String::as_str),
                Some(if windows {
                    "--windows-mobile"
                } else {
                    "--json"
                })
            );
            let count = connector.native.calls.borrow().len();
            connector.flags.set(Flags::default());
            assert_eq!(connector.execute(request).await.unwrap(), Outcome::Replayed);
            assert_eq!(connector.native.calls.borrow().len(), count);
        }
    }
}
#[tokio::test(flavor = "current_thread")]
async fn direct_archive_and_mutation_targets_require_authoritative_supported_provider() {
    for windows in [true, false] {
        for provider in [
            json!("codex"),
            json!("claude"),
            json!("kimi"),
            json!("opencode"),
            json!("dsh"),
            json!("unknown"),
            Value::Null,
        ] {
            for archived in [true, false] {
                let scratch = Scratch::new("direct-provider");
                let connector = providers(&scratch, windows, provider.clone(), vec![]);
                // An old admission is deliberately planted. Windows still
                // needs the authoritative lookup, never this cached membership.
                connector
                    .state
                    .borrow_mut()
                    .paged_archives
                    .insert(("codex/project/test".into(), ARCHIVE.into()), EPOCH);
                let payload = if archived {
                    json!({"archive_id":ARCHIVE,"expected_run_id":"run","expected_conversation_id":"conversation"})
                } else {
                    json!({"request_id":"00000000-0000-4000-8000-000000000011","text":"fixture input","expected_run_id":"run","expected_conversation_id":"conversation"})
                };
                let request = json!({"request_id":"00000000-0000-4000-8000-000000000011","operation":if archived {"inspect"} else {"send"},"session":"codex/project/test","payload":payload});
                let answer = finished(connector.execute(request.clone()).await.unwrap());
                let supported =
                    !windows || zerus_mobile_connector::capabilities::windows_provider(&provider);
                assert_eq!(
                    answer["state"],
                    if supported { "completed" } else { "failed" },
                    "{windows} {provider} {answer}"
                );
                assert_eq!(
                    connector.native.writes.get(),
                    usize::from(supported && !archived)
                );
                if windows && archived {
                    assert_eq!(
                        connector.native.calls.borrow()[0],
                        vec!["archives", "lookup", "--json", "--windows-mobile"]
                    );
                }
                let count = connector.native.calls.borrow().len();
                *connector.native.provider.borrow_mut() = json!("codex");
                connector.flags.set(Flags::default());
                assert_eq!(connector.execute(request).await.unwrap(), Outcome::Replayed);
                assert_eq!(connector.native.calls.borrow().len(), count);
            }
        }
    }
}

#[test]
fn held_admission_refuses_changed_revision_before_journal_or_runtime() {
    use std::os::unix::fs::PermissionsExt;
    let scratch = Scratch::new("revision-admission");
    let path = scratch.path("connector.json");
    let state = scratch.path("receipts");
    let first=json!({"server_url":"https://relay.example.test","node_token":"synthetic-a","state_dir":state}).to_string();
    std::fs::write(&path, &first).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let revision = zerus_relay::json::digest(first.as_bytes());
    let (release, held) = std::sync::mpsc::channel();
    let child_path = path.clone();
    let cwd = scratch.root.clone();
    let child = std::thread::spawn(move || {
        held.recv_timeout(std::time::Duration::from_secs(5))
            .unwrap();
        zerus_mobile_connector::start_bound(
            &child_path,
            false,
            &zerus_mobile_connector::config::Environment {
                home: Some(cwd.clone()),
                cwd,
            },
            EPOCH,
            Some(&revision),
        )
        .is_err()
    });
    let second=json!({"server_url":"https://relay.example.test","node_token":"synthetic-b","state_dir":state}).to_string();
    std::fs::write(&path, &second).unwrap();
    release.send(()).unwrap();
    assert!(child.join().unwrap());
    assert!(!state.exists());
    assert_eq!(std::fs::read_to_string(path).unwrap(), second);
    // This exercises the shared admission routine, not Windows lock/process
    // behavior. The native Save/Start ordering case remains a separate gate.
}
