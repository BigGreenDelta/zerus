//! Additive computer-scoped acquisition envelopes. Native hgs owns Git and its receipt.
use crate::error::{Error, Result};
use serde_json::Value;

pub const OPERATIONS: &[&str] = &["repository_check", "repository_clone", "repository_status"];
pub const READS: &[&str] = &["repository_check", "repository_status"];
pub const READ_SQL: &str = "('repository_check','repository_status')";

pub fn validate(operation: &str, payload: &Value, id: &str) -> Result<()> {
    let fields = ["request_id", "expected_machine_id", "url", "parent", "name"];
    let Some(p) = payload.as_object() else {
        return Err(Error::BAD);
    };
    let status = operation == "repository_status";
    if !OPERATIONS.contains(&operation)
        || p.len() != fields.len() + usize::from(status)
        || !fields.iter().all(|key| p.contains_key(*key))
        || p.keys()
            .any(|key| !fields.contains(&key.as_str()) && !(status && key == "clone_request_id"))
        || payload["request_id"] != id
    {
        return Err(Error::BAD);
    }
    for key in ["request_id", "expected_machine_id"] {
        crate::protocol::uuid(&payload[key], key == "expected_machine_id")?;
    }
    if status {
        crate::protocol::uuid(&payload["clone_request_id"], false)?;
    }
    let url = payload["url"].as_str().ok_or(Error::BAD)?;
    let parent = payload["parent"].as_str().ok_or(Error::BAD)?;
    let name = payload["name"].as_str().ok_or(Error::BAD)?;
    if !valid_url(url)
        || !valid_name(name)
        || !(parent.starts_with('/') && !parent.starts_with("//")
            || operation == "repository_check" && parent == "~")
        || parent.len() > 4096
        || parent.len() + name.len() + usize::from(parent != "/") > 4096
        || parent.chars().any(char::is_control)
        || parent.split('/').any(|part| matches!(part, "." | ".."))
    {
        return Err(Error::BAD);
    }
    Ok(())
}

/// Same admission as the native ABI; no local paths, remote helpers or embedded credentials.
pub fn valid_url(url: &str) -> bool {
    if url
        .as_bytes()
        .get(0..2)
        .is_some_and(|b| b[0].is_ascii_alphabetic() && b[1] == b':')
        || url.is_empty()
        || url.len() > 4096
        || url.contains('\\')
        || url.starts_with("//")
        || url.chars().any(|c| c.is_control() || c.is_whitespace())
    {
        return false;
    }
    if let Some(rest) = url.strip_prefix("https://") {
        let host = rest.split('/').next().unwrap_or("");
        return !host.is_empty()
            && !host.contains('@')
            && !rest.contains(['?', '#'])
            && rest.contains('/');
    }
    if let Some(rest) = url.strip_prefix("ssh://") {
        let host = rest.split('/').next().unwrap_or("");
        return !host.is_empty()
            && !host.rsplit('@').next().unwrap_or("").is_empty()
            && !host
                .split_once('@')
                .is_some_and(|(user, _)| user.contains(':'))
            && !rest.contains(['?', '#'])
            && rest.contains('/');
    }
    !url.contains("://")
        && !url.starts_with('-')
        && !url.contains("::")
        && url
            .split_once(':')
            .is_some_and(|(host, path)| !host.is_empty() && !host.contains('/') && !path.is_empty())
}

pub fn valid_name(name: &str) -> bool {
    if name.is_empty()
        || name.len() > 240
        || name != name.trim()
        || matches!(name, "." | "..")
        || name.ends_with('.')
        || name
            .chars()
            .any(|c| c.is_control() || "/\\:*?\"<>|".contains(c))
    {
        return false;
    }
    let stem = name
        .split('.')
        .next()
        .unwrap_or_default()
        .trim_end_matches(' ')
        .to_ascii_uppercase();
    !matches!(
        stem.as_str(),
        "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$"
    ) && !stem
        .strip_prefix("COM")
        .or_else(|| stem.strip_prefix("LPT"))
        .is_some_and(|n| {
            matches!(
                n,
                "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
            )
        })
}
