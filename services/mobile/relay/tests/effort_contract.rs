//! OpenCode effort in the `launch` and `settings` envelopes (additive, API v1).
//! Database-free: `protocol::request` only.
use serde_json::{json, Value};
use zerus_relay::protocol;

const ID: &str = "c10205cc-39ea-54e7-bd0f-52afe70edb41";
const COMPUTER: &str = "abb2870b-c513-5fa5-9fac-21a337edf246";

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
    protocol::request(
        &json!({"request_id": ID, "computer_id": COMPUTER, "operation": "settings",
        "session": "opencode/example/session",
        "payload": {"request_id": ID, "expected_run_id": "run",
                    "expected_conversation_id": "conversation", "effort": effort}}),
    )
    .is_ok()
}

#[test]
fn a_launch_effort_needs_opencode_a_model_and_a_safe_bounded_id() {
    assert!(launch(json!({})));
    assert!(launch(json!({"effort": "thinking-8k_v1.2"})));
    assert!(launch(json!({"effort": "a".repeat(40)})));
    assert!(launch(json!({"effort": null})));
    assert!(!launch(json!({"model": null})));
    assert!(!launch(
        json!({"agent": "codex", "account_id": null, "model": null})
    ));
    assert!(!launch(
        json!({"agent": "claude", "account_id": null, "model": null})
    ));
    for bad in ["", "a b", "a/b", "a\nb", "a\u{7f}", "caf\u{e9}", "a:b"] {
        assert!(!launch(json!({"effort": bad})), "{bad:?}");
    }
    assert!(!launch(json!({"effort": "a".repeat(41)})));
    assert!(!launch(json!({"effort": 3})));
}

#[test]
fn a_settings_effort_is_a_fixed_name_or_a_safe_variant_id() {
    for fixed in [
        "", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on",
    ] {
        assert!(settings(json!(fixed)), "{fixed}");
    }
    assert!(settings(json!("thinking-8k_v1.2")));
    assert!(settings(json!("a".repeat(40))));
    for bad in ["a b", "a/b", "a\nb", "caf\u{e9}", "a:b"] {
        assert!(!settings(json!(bad)), "{bad:?}");
    }
    assert!(!settings(json!("a".repeat(41))));
    assert!(!settings(json!(7)));
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
