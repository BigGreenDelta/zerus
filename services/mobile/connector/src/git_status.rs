//! Bounded computer-scoped Git evidence; no file list, endpoint or mutation is transported.
use crate::native::Call;
use serde_json::{json, Map, Value};

pub const OPERATION: &str = "git_status";
pub const HELP_MARKER: &str = "Mobile Git status ABI: git-status-v1";
pub const UNAVAILABLE: &str = "native Git status ABI is unavailable";
pub const INVALID: &str = "native Git status has invalid format";
pub const MAX_BYTES: usize = 32 * 1024;
const STRINGS: &[&str] = &[
    "state",
    "path",
    "remote_state",
    "head",
    "branch",
    "upstream",
    "root",
    "common_dir",
    "remote",
    "remote_ref",
];
const STATES: &[&str] = &[
    "ok",
    "unavailable",
    "folder_unavailable",
    "timeout",
    "git_unavailable",
    "metadata_error",
    "too_large",
    "not_repo",
    "changed_during_check",
];
const REMOTE_STATES: &[&str] = &[
    "unknown",
    "detached",
    "no_upstream",
    "local_upstream",
    "unavailable",
    "verified",
    "changed",
    "missing",
];

pub fn validate(payload: &Value, request_id: &str) -> Result<(), String> {
    let valid = payload.as_object().is_some_and(|object| {
        object.len() == 2
            && object.get("request_id").and_then(Value::as_str) == Some(request_id)
            && object
                .get("path")
                .and_then(Value::as_str)
                .is_some_and(|path| {
                    path.starts_with('/')
                        && path.len() <= 4096
                        && !path.chars().any(char::is_control)
                })
    });
    if valid {
        Ok(())
    } else {
        Err("invalid Git status fields or absolute path".into())
    }
}

pub fn call(payload: &Value) -> Call {
    let mut call = Call::new([
        "git-status",
        "--path",
        payload["path"].as_str().expect("validated"),
        "--json",
    ])
    .timeout(5.0);
    call.max_output_bytes = Some(MAX_BYTES);
    call
}

/// Native canonical paths are display evidence, never authority for the
/// original request path. Unknown native keys are omitted, optional keys stay absent.
pub fn project(
    native: &Value,
    request_id: &str,
    requested_path: &str,
) -> Result<Value, &'static str> {
    let object = native.as_object().ok_or(INVALID)?;
    if !object
        .get("state")
        .and_then(Value::as_str)
        .is_some_and(|state| STATES.contains(&state))
        || !object.get("path").is_some_and(Value::is_string)
        || !object
            .get("remote_state")
            .and_then(Value::as_str)
            .is_some_and(|state| REMOTE_STATES.contains(&state))
    {
        return Err(INVALID);
    }
    let mut snapshot = Map::new();
    for (key, value) in object {
        let valid = if STRINGS.contains(&key.as_str()) {
            value.is_string()
        } else {
            match key.as_str() {
                "unborn" | "detached" => value.is_boolean(),
                "changed_files" | "conflicts" => nonnegative_integer(value),
                "ahead" | "behind" => value.is_null() || nonnegative_integer(value),
                "sampled_at" | "remote_checked_at" | "remote_age_seconds" => {
                    value.as_f64().is_some_and(|n| n.is_finite() && n >= 0.0)
                }
                _ => continue,
            }
        };
        if !valid {
            return Err(INVALID);
        }
        snapshot.insert(key.clone(), value.clone());
    }
    let result = json!({"schema":1,"request_id":request_id,"requested_path":requested_path,"snapshot":snapshot});
    if !zerus_relay::json::canonical(&result).is_ok_and(|text| text.len() <= MAX_BYTES) {
        return Err("native Git status exceeds size limit");
    }
    Ok(result)
}

fn nonnegative_integer(value: &Value) -> bool {
    crate::pyvalue::int_literal(value).is_some_and(|digits| !digits.starts_with('-'))
}
