//! Shared notification preference mutation, separate from shared read marks.
use serde_json::Value;

pub const OPERATION: &str = "session_notifications";
pub const HELP_MARKER: &str = "session-notifications-v1";

pub fn validate(payload: &Value) -> Result<(), String> {
    let keys = ["run_id", "conversation_id", "muted", "expected_revision"];
    if !payload
        .as_object()
        .is_some_and(|p| p.len() == keys.len() && keys.iter().all(|k| p.contains_key(*k)))
    {
        return Err(
            "session_notifications requires exact run, conversation, mute and revision fields"
                .into(),
        );
    }
    for (key, maximum) in [("run_id", 128), ("conversation_id", 256)] {
        if !payload[key].as_str().is_some_and(|s| {
            !s.is_empty()
                && s.len() <= maximum
                && !s.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}')
        }) {
            return Err("invalid shared notification identity".into());
        }
    }
    if !payload["muted"].is_boolean()
        || !payload["expected_revision"]
            .as_i64()
            .is_some_and(|r| r >= 0)
    {
        return Err("invalid shared notification preference".into());
    }
    Ok(())
}

pub fn result_valid(result: &Value, payload: &Value) -> bool {
    if result["ok"] == false {
        return result.get("applied").is_none()
            && matches!(
                result["error"].as_str(),
                Some(
                    "not_found"
                        | "stale_identity"
                        | "conflict"
                        | "baseline_unavailable"
                        | "capacity"
                        | "revision_exhausted"
                )
            );
    }
    let preference = &result["preferences"];
    let (Some(applied), Some(expected), Some(revision)) = (
        result["applied"].as_bool(),
        payload["expected_revision"].as_i64(),
        preference["revision"].as_i64(),
    ) else {
        return false;
    };
    result["ok"] == true
        && result["run_id"] == payload["run_id"]
        && result["conversation_id"] == payload["conversation_id"]
        && preference["conversation_id"] == payload["conversation_id"]
        && preference["muted"].is_boolean()
        && preference["muted"] == payload["muted"]
        && expected.checked_add(i64::from(applied)) == Some(revision)
        && preference["suppressed"].is_object()
}
