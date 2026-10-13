//! Failed launch attempts have diagnostic value but no confirmed conversation.
//! They never become Saved or Archive entries and never enable fuzzy resume.
use super::*;

fn identity_gone(identity: &Value, start_key: &str) -> bool {
    let Some(pid) = identity["pid"]
        .as_u64()
        .and_then(|pid| i32::try_from(pid).ok())
        .filter(|pid| *pid > 0)
    else {
        return false;
    };
    // Distinguish a missing process from an unavailable ps command. Inability
    // to inspect a process must not authorize replacing a possibly live run.
    match crate::platform::presence(pid as u32) {
        crate::platform::Presence::Gone => return true,
        crate::platform::Presence::Unknown => return false,
        crate::platform::Presence::Present => {}
    }
    let expected = string(identity, start_key);
    if expected.is_empty() {
        return false;
    }
    let Some(actual) = crate::platform::process_start_time(pid as u32)
        .or_else(|| crate::platform::process_start_fallback(pid as u32))
    else {
        return false;
    };
    !actual.is_empty() && actual != expected
}

pub(super) fn identities_gone(record: &Value) -> bool {
    identity_gone(record, "process_start")
        && match record.get("supervisor") {
            None => true, // Pre-supervisor versions exec'd the tracked process.
            Some(supervisor) => identity_gone(supervisor, "start"),
        }
}

fn present(record: &Value, key: &str) -> bool {
    match record.get(key) {
        None | Some(Value::Null) => false,
        Some(Value::String(value)) => !value.is_empty(),
        Some(Value::Array(value)) => !value.is_empty(),
        Some(Value::Object(value)) => !value.is_empty(),
        Some(Value::Bool(value)) => *value,
        Some(_) => true,
    }
}

/// Positive provider evidence is preserved even if an old/corrupt record has
/// lost its conversation_id. A failed wrapper is not evidence of a conversation.
fn unconfirmed(record: &Value) -> bool {
    ![
        "conversation_id",
        "transcript",
        "last_event_at",
        "session_end",
        "prompt",
        "last_message",
        "model",
        "active_tools",
        "subagents",
        "turn_started",
    ]
    .iter()
    .any(|key| present(record, key))
        && ["", "unknown"].contains(&string(record, "activity"))
        && ["", "unknown"].contains(&string(record, "conversation_state"))
        && string(record, "error") != "agent resumed a different conversation"
}

pub(super) fn retryable(record: &Value, requested: Option<&str>) -> bool {
    if !unconfirmed(record)
        || record.get("pausing").is_some()
        || present(record, "paused")
        || present(record, "restore_archive_id")
        || present(record, "resume_pending")
        || present(record, "archive_id")
    {
        return false;
    }
    match requested {
        Some(id) => {
            if string(record, "requested_id") != id || string(record, "expected_id") != id {
                return false;
            }
        }
        None => {
            if present(record, "requested_id") || present(record, "expected_id") {
                return false;
            }
        }
    }
    identities_gone(record)
}

pub(super) fn binding_exists(name: &str) -> Result<bool> {
    let _guard = lock(None)?;
    if !record_path(name).exists() {
        return Ok(false);
    }
    let record = read(name)?;
    if archive::equivalent(&record, &archive::records()?) {
        return Ok(false);
    }
    // Reads do not retire the record. The launch runner repeats this decision
    // under the writer lock immediately before replacing it.
    Ok(live()?.contains_key(name) || !retryable(&record, None))
}

fn save(record: &Value) -> Result<()> {
    let id = uuid::Uuid::new_v4().to_string();
    let mut attempt = record.clone();
    attempt["attempt_id"] = json!(id);
    attempt["attempt_saved_at"] = json!(now());
    attempt["attempt_reason"] = json!("replaced_unconfirmed_launch");
    atomic(
        &root().join("attempts").join(format!("{id}.json")),
        &format!("{attempt}\n"),
    )
}

/// Called with the state writer lock, after tmux created the prospective new
/// pane but before changing its run marker or replacing the durable binding.
pub(super) fn prepare_replacement(
    record: &Value,
    pane: &str,
    token: &str,
    fresh: bool,
    requested: Option<&str>,
) -> Result<()> {
    let name = string(record, "name");
    let snapshot = live()?;
    let Some(panes) = snapshot.get(name) else {
        return Err(format!("{name}: launch pane disappeared"));
    };
    if panes.len() != 1
        || panes[0].0 != pane
        || panes[0].1 != "0"
        || !["", token, string(record, "run_id")].contains(&panes[0].2.as_str())
    {
        return Err(format!(
            "{name}: session changed while preparing a new launch"
        ));
    }
    let info = backend::current()
        .pane_info(backend::Target::Pane(pane), &[backend::PaneField::PanePid])?;
    if info.value() != std::process::id().to_string() {
        return Err(format!("{name}: launch does not own the new terminal pane"));
    }
    let retry = retryable(record, None) || requested.is_some_and(|id| retryable(record, Some(id)));
    if !(retry || fresh && identities_gone(record) && !pause_active(record)) {
        return Err(format!(
            "{name}: saved or running binding changed; refusing to replace its context"
        ));
    }
    // Explicit --fresh can replace a confirmed stopped conversation by design.
    // Unconfirmed failures always retain their diagnostic record first.
    if unconfirmed(record) {
        save(record)?;
    }
    Ok(())
}

pub(super) fn agent_home_matches(record: &Value, agent: &str) -> Result<bool> {
    let default = if agent == "kimi" {
        ".kimi-code".into()
    } else {
        format!(".{agent}")
    };
    let resolve = |configured: Option<&str>, base: &Path| {
        let path = match configured.filter(|value| !value.is_empty()) {
            None => home().join(&default),
            Some("~") => home(),
            Some(value) if value.starts_with("~/") => home().join(&value[2..]),
            Some(value) => PathBuf::from(value),
        };
        let absolute = if path.is_absolute() {
            path
        } else {
            base.join(path)
        };
        crate::platform::canonical(&absolute).unwrap_or(absolute)
    };
    let current = nonempty_env(home_var(agent)?);
    let cwd = std::env::current_dir().map_err(|error| error.to_string())?;
    Ok(resolve(
        record["agent_home"].as_str(),
        Path::new(string(record, "launch_dir")),
    ) == resolve(current.as_deref(), &cwd))
}

pub(super) fn startup_failure(name: &str) -> Result<Option<String>> {
    let _guard = lock(None)?;
    if !record_path(name).exists() || live()?.contains_key(name) {
        return Ok(None);
    }
    let record = read(name)?;
    if !unconfirmed(&record) || !identities_gone(&record) {
        return Ok(None);
    }
    let cause = if let Some(signal) = record["termination_signal"].as_i64() {
        format!(" (signal {signal})")
    } else if let Some(code) = record["exit_code"].as_i64() {
        format!(" (exit {code})")
    } else if !string(&record, "error").is_empty() {
        format!(" ({})", string(&record, "error"))
    } else {
        String::new()
    };
    Ok(Some(format!(
        "{name}: launch ended before a conversation was confirmed{cause}; run the same command to retry"
    )))
}
