//! OpenCode effort in the `launch` and `settings` envelopes (additive, API v1).
//! Database-free: `protocol::request` only.
use serde_json::{json, Value};
use zerus_relay::protocol;

const ID: &str = "c10205cc-39ea-54e7-bd0f-52afe70edb41";
const COMPUTER: &str = "abb2870b-c513-5fa5-9fac-21a337edf246";
/// The shape contract every layer shares with the native catalog projection.
const EFFORT_IDS: &str = include_str!("../../../../tests/fixtures/opencode-effort-ids.json");
const FIXED: [&str; 11] = [
    "", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on",
];

fn effort_ids(kind: &str) -> Vec<String> {
    let fixture: Value = serde_json::from_str(EFFORT_IDS).unwrap();
    let ids: Vec<String> = fixture[kind]
        .as_array()
        .unwrap()
        .iter()
        .map(|id| id.as_str().unwrap().to_owned())
        .collect();
    assert!(!ids.is_empty(), "{kind}");
    ids
}

fn launch(changes: Value) -> bool {
    let mut payload = json!({"request_id": ID, "agent": "opencode", "directory": "/example/mobile",
        "tag": "new-session", "account_id": "native-opencode", "model": "fixture/local",
        "effort": "high"});
    for (key, value) in changes.as_object().unwrap() {
        if value.is_null() {
            payload.as_object_mut().unwrap().remove(key);
        } else {
            payload[key] = value.clone();
        }
    }
    protocol::request(
        &json!({"request_id": ID, "computer_id": COMPUTER, "operation": "launch",
        "session": "", "payload": payload}),
    )
    .is_ok()
}

fn settings(effort: Value) -> bool {
    settings_for("opencode/example/session", effort)
}

fn settings_for(session: &str, effort: Value) -> bool {
    protocol::request(
        &json!({"request_id": ID, "computer_id": COMPUTER, "operation": "settings",
        "session": session,
        "payload": {"request_id": ID, "expected_run_id": "run",
                    "expected_conversation_id": "conversation", "effort": effort}}),
    )
    .is_ok()
}

#[test]
fn a_launch_effort_needs_opencode_a_model_and_a_native_effort_id() {
    assert!(launch(json!({})));
    assert!(launch(json!({"effort": null})));
    assert!(!launch(json!({"model": null})));
    for agent in ["codex", "claude", "kimi", "dsh"] {
        assert!(
            !launch(json!({"agent": agent, "account_id": null, "model": null})),
            "{agent}"
        );
        assert!(launch(
            json!({"agent": agent, "account_id": null, "model": null, "effort": null})
        ));
    }
    assert!(!launch(json!({"effort": 3})));
    assert!(!launch(json!({"effort": ["high"]})));
}

#[test]
fn the_shared_fixture_decides_every_opencode_launch_effort() {
    for id in effort_ids("accept") {
        assert!(launch(json!({"effort": id})), "{id:?}");
        // The model, not the agent, gates it: no model, no effort.
        assert!(!launch(json!({"model": null, "effort": id})), "{id:?}");
    }
    for id in effort_ids("reject") {
        assert!(!launch(json!({"effort": id})), "{id:?}");
    }
}

#[test]
fn the_shared_fixture_decides_every_opencode_settings_effort() {
    for fixed in FIXED {
        assert!(settings(json!(fixed)), "{fixed}");
    }
    for id in effort_ids("accept") {
        assert!(settings(json!(id)), "{id:?}");
    }
    for id in effort_ids("reject").into_iter().filter(|id| !id.is_empty()) {
        assert!(!settings(json!(id)), "{id:?}");
    }
    assert!(!settings(json!(7)));
    assert!(!settings(json!(null)));
}

#[test]
fn only_opencode_sessions_admit_a_variant_id_in_settings() {
    for session in [
        "codex/example/session",
        "claude/example/session",
        "kimi/example/session",
        "dsh/example/session",
        "opencodex/example/session",
    ] {
        for fixed in FIXED {
            assert!(settings_for(session, json!(fixed)), "{session} {fixed}");
        }
        for id in effort_ids("accept")
            .into_iter()
            .filter(|id| !FIXED.contains(&id.as_str()))
        {
            assert!(!settings_for(session, json!(id)), "{session} {id:?}");
        }
        assert!(!settings_for(session, json!("thinking-8k")), "{session}");
    }
    assert!(settings_for(
        "opencode/example/session",
        json!("thinking-8k")
    ));
}

#[test]
fn the_relay_advertises_the_launch_effort_feature() {
    assert!(protocol::FEATURES.contains(&"opencode_launch_effort"));
    let capabilities = protocol::capabilities(Vec::new());
    assert!(capabilities["features"]
        .as_array()
        .unwrap()
        .contains(&json!("opencode_launch_effort")));
}
