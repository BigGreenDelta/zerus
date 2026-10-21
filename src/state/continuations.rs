//! Claude's background service runs conversations in processes that inherited
//! the HGS run token of whichever session started the service. Their hooks never
//! belong to that session: a conversation sent to the background belongs to the
//! session whose transcript ends with Claude's `continued-in` mark for it.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Seek, SeekFrom};

/// The Claude process that runs this hook, when Claude named it for this event's
/// own conversation rather than an outer Claude passing its variables on.
fn claude_process(sid: &str) -> Option<u32> {
    if nonempty_env("CLAUDE_CODE_SESSION_ID").as_deref() != Some(sid) {
        return None;
    }
    nonempty_env("CLAUDE_PID")?.parse().ok()
}

/// Whether a Claude process that is neither the session's agent nor runs under it
/// sent the event: the run token it carries is only inherited.
pub(super) fn foreign(record: Option<&Value>, sid: &str) -> bool {
    let Some(claude) = claude_process(sid) else {
        return false;
    };
    let agent = record
        .and_then(|record| record["pid"].as_u64())
        .and_then(|pid| u32::try_from(pid).ok());
    agent != Some(claude) && !agent.is_some_and(runs_under)
}

fn runs_under(agent: u32) -> bool {
    let mut pid = std::process::id();
    for _ in 0..64 {
        if pid == agent {
            return true;
        }
        let Some(process) = crate::platform::process_identity(pid) else {
            return false;
        };
        if process.parent == 0 || process.parent == pid {
            return false;
        }
        pid = process.parent;
    }
    false
}

fn links() -> PathBuf {
    root().join("continued")
}

fn link(sid: &str) -> PathBuf {
    links().join(format!("{:x}.json", Sha256::digest(sid.as_bytes())))
}

/// The session the link names, while that conversation is still its own.
fn linked(path: &Path) -> Option<Value> {
    let link: Value = serde_json::from_str(&fs::read_to_string(path).ok()?).ok()?;
    read_run(string(&link, "name"), string(&link, "run_id"))
        .ok()?
        .filter(|record| record["conversation_id"] == link["conversation_id"])
}

/// The live session whose conversation `sid` continues, bound to it. A main
/// SessionStart finds it by the mark; later events by the link it leaves.
pub(super) fn owner(event: &Value, sid: &str, panes: &Snapshot) -> Result<Option<Value>> {
    let path = link(sid);
    if path.exists() {
        if let Some(record) = linked(&path) {
            return Ok(Some(record));
        }
        let _ = fs::remove_file(&path);
    }
    if string(event, "hook_event_name") != "SessionStart" || !string(event, "agent_id").is_empty() {
        return Ok(None);
    }
    let mut found = records()?.into_iter().filter(|record| {
        string(record, "agent") == "claude"
            && matches(record, panes.get(string(record, "name")))
            && continued_in(record).as_deref() == Some(sid)
    });
    let (Some(mut record), None) = (found.next(), found.next()) else {
        return Ok(None);
    };
    record["conversation_id"] = json!(sid);
    if !string(event, "transcript_path").is_empty() {
        record["transcript"] = event["transcript_path"].clone();
    }
    write(&mut record)?;
    // A link outlives neither its session nor that session's conversation.
    for entry in fs::read_dir(links()).into_iter().flatten().flatten() {
        if linked(&entry.path()).is_none() {
            let _ = fs::remove_file(entry.path());
        }
    }
    let link = json!({"name": record["name"], "run_id": record["run_id"], "conversation_id": sid});
    atomic(&path, &format!("{link}\n"))?;
    Ok(Some(record))
}

/// The conversation that the mark ending the session's transcript names. Claude
/// may add its own bookkeeping after the mark; a later transcript entry means the
/// conversation went on here.
fn continued_in(record: &Value) -> Option<String> {
    const TAIL: u64 = 16 * 1024;
    let mut file = fs::File::open(string(record, "transcript")).ok()?;
    let length = file.metadata().ok()?.len();
    file.seek(SeekFrom::Start(length.saturating_sub(TAIL))).ok()?;
    let mut tail = Vec::new();
    file.take(TAIL).read_to_end(&mut tail).ok()?;
    for line in tail.trim_ascii_end().rsplit(|byte| *byte == b'\n') {
        let entry: Value = serde_json::from_slice(line).ok()?;
        if entry["type"] == "continued-in" {
            return (entry["sessionId"] == record["conversation_id"])
                .then(|| string(&entry, "continuedInSessionId").to_owned())
                .filter(|sid| !sid.is_empty());
        }
        if entry.get("uuid").is_some() {
            return None;
        }
    }
    None
}
