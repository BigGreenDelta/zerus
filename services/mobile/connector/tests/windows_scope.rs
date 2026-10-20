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
