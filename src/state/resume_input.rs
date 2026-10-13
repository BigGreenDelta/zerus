//! Codex can defer SessionStart until the first turn after native resume or fork.
//! Require an owned rollout and an empty native composer before that turn.
//! This never fabricates a hook or confirms a run.
use super::*;
use std::fs;
use std::io::{BufRead, BufReader};

pub(super) fn fork_pending(record: &Value) -> bool {
    record["agent"] == "codex"
        && record["startup_kind"] == "new"
        && uuid::Uuid::parse_str(string(record, "fork_parent_id")).is_ok()
        && record["base"].as_array().is_some_and(|args| {
            args.get(1).is_some_and(|arg| arg == "fork")
                && args.get(2) == record.get("fork_parent_id")
        })
        && string(record, "conversation_id").is_empty()
        && string(record, "expected_id").is_empty()
        && string(record, "requested_id").is_empty()
}

/// The answer a Codex launch check gives when the open files cannot be listed.
fn open_files_error(error: crate::platform::RunError) -> String {
    use crate::platform::RunError;
    match error {
        RunError::Spawn(error) | RunError::Io(error) => error.to_string(),
        RunError::Stopped => "timed out verifying agent history".into(),
        RunError::TooLarge | RunError::Failed | RunError::Unsupported => {
            "Cannot verify the Codex process's open history files".into()
        }
    }
}

fn agent_pid(pid: u64) -> Result<u32> {
    u32::try_from(pid).map_err(|_| "The tracked agent process is not the supervised Codex".into())
}

fn history_meta(path: &Path) -> Result<Value> {
    let mut line = String::new();
    BufReader::new(fs::File::open(path).map_err(|e| e.to_string())?)
        .take(256 * 1024)
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&line).map_err(|_| "Cannot verify native conversation history".into())
}

fn fork_directory(record: &Value) -> Result<PathBuf> {
    let launch = Path::new(string(record, "launch_dir"));
    let mut cwd = launch.to_path_buf();
    let args = recipes::argv(record)?;
    let mut index = 3;
    while index < args.len() && args[index] != "--" {
        let (key, value) = args[index]
            .split_once('=')
            .map_or((args[index].as_str(), None), |(key, value)| {
                (key, Some(value))
            });
        if ["-C", "--cd"].contains(&key) {
            let folder = if let Some(value) = value {
                value
            } else {
                index += 1;
                args.get(index)
                    .ok_or("The fork's directory option is missing its value")?
            };
            cwd = launch.join(folder);
        }
        index += 1;
    }
    crate::platform::canonical(cwd)
        .map_err(|_| "The fork's working directory is unavailable".into())
}

pub(super) fn pending(record: &Value) -> bool {
    record["agent"] == "codex"
        && record["run_identity_version"] == 1
        && record["startup_kind"] == "resume"
        && record["supervisor"].is_object()
        && !string(record, "conversation_id").is_empty()
        && record["expected_id"] == record["conversation_id"]
        && string(record, "requested_id").is_empty()
        && string(record, "error").is_empty()
        && record.get("session_end").is_none()
        && record["last_event_at"].as_f64().unwrap_or(0.0) == 0.0
        && ["", "unknown"].contains(&string(record, "activity"))
        && ["", "unknown"].contains(&string(record, "phase"))
}

fn owns_rollout(record: &Value) -> Result<()> {
    let path = crate::platform::canonical(string(record, "transcript"))
        .map_err(|_| "Saved conversation history is unavailable")?;
    if !path.is_file() {
        return Err("Saved conversation history is not a file".into());
    }
    let meta = history_meta(&path)?;
    if meta["type"] != "session_meta" || meta["payload"]["id"] != record["expected_id"] {
        return Err("Saved conversation history has a different identity".into());
    }
    let pid = record["pid"].as_u64().ok_or("Missing agent process")?;
    if !crate::platform::holds_open(agent_pid(pid)?, &path).map_err(open_files_error)? {
        return Err("Codex has not opened the saved conversation yet".into());
    }
    Ok(())
}

fn checked_process(record: &Value) -> Result<u64> {
    if !process_alive(record) || !matches(record, live()?.get(string(record, "name"))) {
        return Err("The tracked agent is no longer running".into());
    }
    let supervisor = &record["supervisor"];
    let parent = supervisor["pid"]
        .as_u64()
        .ok_or("Missing session supervisor")?;
    if parent > u32::MAX as u64
        || string(supervisor, "start").is_empty()
        || process_start(parent as u32) != string(supervisor, "start")
    {
        return Err("The tracked session supervisor has changed".into());
    }
    let pid = record["pid"].as_u64().ok_or("Missing agent process")?;
    let (actual_parent, name) =
        crate::platform::parent_and_name(agent_pid(pid)?).map_err(|e| e.to_string())?;
    if actual_parent.map(u64::from) != Some(parent)
        || name
            .and_then(|name| crate::platform::program_name(Path::new(&name)))
            .is_none_or(|name| name != "codex")
    {
        return Err("The tracked agent process is not the supervised Codex".into());
    }
    Ok(pid)
}

/// A fork has no requested child ID yet. The native process must own exactly
/// one interactive rollout whose header links a distinct ID to the saved parent.
pub(super) fn fork_ready(record: &Value) -> Result<()> {
    if !fork_pending(record) || !input::first_message_candidate(record) || pause_active(record) {
        return Err("Waiting for the agent to open the forked conversation".into());
    }
    let pid = checked_process(record)?;
    let agent_home = if string(record, "agent_home").is_empty() {
        home().join(".codex")
    } else {
        PathBuf::from(string(record, "agent_home"))
    };
    let sessions = crate::platform::canonical(agent_home.join("sessions"))
        .map_err(|_| "Forked conversation history is unavailable")?;
    let cwd = fork_directory(record)?;
    let mut children = std::collections::BTreeSet::new();
    for path in crate::platform::open_files(agent_pid(pid)?).map_err(open_files_error)? {
        if !path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with("rollout-"))
            || path
                .extension()
                .is_none_or(|extension| extension != "jsonl")
        {
            continue;
        }
        let Ok(path) = crate::platform::canonical(&path) else {
            continue;
        };
        if !path.starts_with(&sessions) {
            continue;
        }
        let Ok(meta) = history_meta(&path) else {
            continue;
        };
        let id = string(&meta["payload"], "id");
        if meta["type"] == "session_meta"
            && uuid::Uuid::parse_str(id).is_ok()
            && id != string(record, "fork_parent_id")
            && meta["payload"]["forked_from_id"] == record["fork_parent_id"]
            && meta["payload"]["source"] == "cli"
            && crate::platform::canonical(string(&meta["payload"], "cwd"))
                .is_ok_and(|path| path == cwd)
        {
            children.insert((path, id.to_owned()));
        }
    }
    if children.len() != 1 {
        return Err("Codex has not opened a single verified fork of this conversation yet".into());
    }
    input::checked_terminal(record, true)?;
    if !process_alive(record) {
        return Err("The forked agent process has changed".into());
    }
    Ok(())
}

pub(super) fn ready(record: &Value) -> Result<()> {
    if !pending(record) || pause_active(record) {
        return Err("Waiting for the agent to confirm this conversation".into());
    }
    if record["input_pending_at"].as_f64().unwrap_or(0.0) > 0.0 {
        return Err("Message sent. Waiting for the agent to confirm this conversation.".into());
    }
    checked_process(record)?;
    owns_rollout(record)?;
    input::checked_terminal(record, true)?;
    if !process_alive(record) {
        return Err("The resumed agent process has changed".into());
    }
    Ok(())
}

pub(super) fn enrich(record: &Value, output: &mut Value, live: bool) {
    if !live || !pending(record) {
        return;
    }
    let reason = ready(record).err().unwrap_or_default();
    output["resume_message_can_send"] = json!(reason.is_empty());
    output["resume_message_reason"] = json!(reason);
    if reason.is_empty() {
        output["activity"] = json!("idle");
        output["phase"] = json!("idle");
        output["activity_summary"] = json!("Waiting for your next message");
    } else {
        output["phase"] = json!("starting");
        output["activity_summary"] = json!("Restoring conversation");
        output["activity_detail"] = json!(reason);
    }
}
