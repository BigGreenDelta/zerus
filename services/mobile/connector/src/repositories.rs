//! Reuse native pinned-directory/worker custody, with the original clone UUID and fingerprint.
use crate::native::Call;
use ring::digest::{digest, SHA256};
use serde_json::{json, Value};

pub const HELP_MARKER: &str = "Mobile repository acquisition ABI: repository-acquisition-v1";
pub const UNAVAILABLE: &str = "native repository acquisition ABI is unavailable";
pub const MAX_RECEIPT_BYTES: usize = 16 * 1024;
pub const CLONE_TIMEOUT: f64 = 75.0;
pub const INVALID: &str = "native repository receipt did not match this computer and destination";

pub fn call(operation: &str, p: &Value) -> Call {
    let subcommand = match operation {
        "repository_check" => "check",
        "repository_clone" => "clone",
        _ => "status",
    };
    let mut argv = vec!["repository", subcommand, "--json"];
    if operation != "repository_status" {
        argv.extend([
            "--url",
            p["url"].as_str().expect("validated"),
            "--parent",
            p["parent"].as_str().expect("validated"),
            "--name",
            p["name"].as_str().expect("validated"),
        ]);
    }
    if operation != "repository_check" {
        let id = if operation == "repository_status" {
            &p["clone_request_id"]
        } else {
            &p["request_id"]
        };
        argv.extend(["--request-id", id.as_str().expect("validated")]);
    }
    // Keep the mobile wrapper within the gateway's 90-second stop allowance.
    // Native Git also has its own deadline/owned worker; wrapper cancellation
    // stops that Git operation and preserves files for explicit recovery.
    let mut call = Call::new(argv).timeout(if operation == "repository_clone" {
        CLONE_TIMEOUT
    } else {
        30.0
    });
    call.max_output_bytes = Some(32 * 1024);
    call
}

pub fn project(operation: &str, p: &Value, raw: &Value) -> Result<Value, &'static str> {
    let parent = raw["parent"].as_str().ok_or(INVALID)?;
    let path = raw["path"].as_str().ok_or(INVALID)?;
    let name = p["name"].as_str().ok_or(INVALID)?;
    if crate::launch::canonical_directory(raw.get("parent")).is_err()
        || path
            != format!(
                "{}{name}",
                if parent == "/" {
                    "/".into()
                } else {
                    format!("{parent}/")
                }
            )
        || raw["name"] != name
        || (operation != "repository_check" && p["parent"] != parent)
    {
        return Err(INVALID);
    }
    let status = raw["status"].as_str().ok_or(INVALID)?;
    let clone_id = if operation == "repository_status" {
        &p["clone_request_id"]
    } else {
        &p["request_id"]
    };
    if operation == "repository_check" {
        if status != "ready" {
            return Err(INVALID);
        }
    } else {
        let bytes =
            serde_json::to_vec(&json!([p["url"], p["parent"], p["name"]])).map_err(|_| INVALID)?;
        let fingerprint: String = digest(&SHA256, &bytes)
            .as_ref()
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        if raw["request_id"] != *clone_id
            || raw["fingerprint"] != fingerprint
            || !["cloned", "failed", "unconfirmed"].contains(&status)
        {
            return Err(INVALID);
        }
    }
    let mut result = json!({"schema":1,"request_id":p["request_id"],"clone_request_id":clone_id,
        "machine_id":p["expected_machine_id"],"status":status,"parent":parent,"name":name,"path":path});
    if let Some(detail) = raw["detail"].as_str() {
        result["detail"] = json!("");
        let base = serde_json::to_vec(&result).map_err(|_| INVALID)?.len();
        let mut remaining = MAX_RECEIPT_BYTES.checked_sub(base).ok_or(INVALID)?;
        let mut bounded = String::new();
        for character in detail.chars().take(2048) {
            let cost = serde_json::to_string(&character)
                .map_err(|_| INVALID)?
                .len()
                - 2;
            if cost > remaining {
                break;
            }
            bounded.push(character);
            remaining -= cost;
        }
        result["detail"] = json!(bounded);
    }
    if serde_json::to_vec(&result).map_err(|_| INVALID)?.len() > MAX_RECEIPT_BYTES {
        return Err(INVALID);
    }
    Ok(result)
}
