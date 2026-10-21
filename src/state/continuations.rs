//! Claude runs a conversation sent to the background in another process, which
//! inherited the HGS run token of whichever session started Claude's background
//! service. Its hooks belong to the session whose conversation it continues:
//! that session's transcript ends with Claude's `continued-in` mark.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Seek, SeekFrom};

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

/// The conversation that the mark ending the session's transcript names.
fn continued_in(record: &Value) -> Option<String> {
    const TAIL: u64 = 16 * 1024;
    let mut file = fs::File::open(string(record, "transcript")).ok()?;
    let length = file.metadata().ok()?.len();
    file.seek(SeekFrom::Start(length.saturating_sub(TAIL))).ok()?;
    let mut tail = Vec::new();
    file.take(TAIL).read_to_end(&mut tail).ok()?;
    let mark: Value = serde_json::from_slice(tail.trim_ascii_end().rsplit(|byte| *byte == b'\n').next()?).ok()?;
    (mark["type"] == "continued-in" && mark["sessionId"] == record["conversation_id"])
        .then(|| string(&mark, "continuedInSessionId").to_owned())
        .filter(|sid| !sid.is_empty())
}
