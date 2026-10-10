//! Shared read marks. The host that runs a session is their only writer and
//! source of truth: desktops and phones read them from `hgs ls --json` and
//! write them with `hgs [@host] read`.
use super::*;
use sha2::{Digest, Sha256};
use std::fs;

const ACTIONABLE: &[&str] = &["approval", "input", "error"];

pub(super) fn path() -> PathBuf {
    root().join("read-marks.json")
}

/// Marks by owner (a tracked session's stable journal name, otherwise its
/// name) and conversation. A missing or unreadable file means no marks.
pub(super) fn load() -> Value {
    fs::read_to_string(path())
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .filter(|value| value["version"] == 1 && value["sessions"].is_object())
        .map(|value| value["sessions"].clone())
        .unwrap_or_else(|| json!({}))
}

pub(super) fn record<'a>(marks: &'a Value, owner: &str, conversation: &str) -> &'a Value {
    static NONE: Value = Value::Null;
    if conversation.is_empty() {
        return &NONE;
    }
    marks.get(owner).and_then(|m| m.get(conversation)).unwrap_or(&NONE)
}

/// Canonical identity of what needs the owner now, or None. Status-only
/// changes (activity text, timestamps, tool progress, finished children)
/// never change it; a new request, question or failed turn always does.
pub(super) fn signature(session: &Value, questions: &[Value]) -> Option<String> {
    let running = ["", "running"].contains(&string(session, "state"))
        && string(session, "process_state") != "exited";
    if !running {
        return None;
    }
    let phase = string(session, "phase");
    let actionable = ACTIONABLE.contains(&phase);
    let attention = if !actionable {
        Value::Null
    } else if !string(session, "attention_id").is_empty() {
        session["attention_id"].clone()
    } else if phase == "error" {
        // A failed turn without a native error identity is still a new event.
        json!(["turn", session["turn_started"]])
    } else {
        json!("")
    };
    // The same identities the mobile connector publishes as `mobile_attention`.
    let mut asked: Vec<Value> = questions
        .iter()
        .filter(|card| card["answer_delivery"]["status"] != "submitted")
        .map(|card| json!([card["question_id"], card["question_hash"]]))
        .collect();
    asked.sort_by_key(Value::to_string);
    asked.dedup();
    let mut children = Vec::new();
    if string(session, "subagent_source") != "hook_profiles" {
        for (id, child) in session["subagents"].as_object().into_iter().flatten() {
            let state = child["display_state"]
                .as_str()
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| string(child, "state"));
            if ["approval", "input", "attention"].contains(&state) {
                let request = child["attention_id"]
                    .as_str()
                    .or_else(|| child["question_id"].as_str())
                    .unwrap_or("");
                children.push(json!([id, state, request]));
            }
        }
    }
    children.sort_by_key(Value::to_string);
    if !actionable && asked.is_empty() && children.is_empty() {
        return None;
    }
    let canonical = json!([1, session["run_id"], session["conversation_id"],
        if actionable { phase } else { "" }, attention, asked, children]);
    Some(format!("{:x}", Sha256::digest(canonical.to_string().as_bytes())))
}

/// Pending native question identities of a live tracked session. Only Codex
/// asks without changing phase; other providers are read only while waiting.
/// Errors contribute nothing: a listing never fails on optional indexes.
pub(super) fn pending_questions(record: &Value, session: &Value, live_pane: bool) -> Vec<Value> {
    if !live_pane
        || (string(record, "agent") != "codex" && !ACTIONABLE.contains(&string(session, "phase")))
        || !process_alive(record)
    {
        return Vec::new();
    }
    questions::current(record).unwrap_or_default()
}

/// Add the C1 fields to one `hgs ls --json` session object.
pub(super) fn enrich(session: &mut Value, marks: &Value, owner: &str, questions: &[Value]) {
    let mark = record(marks, owner, string(session, "conversation_id")).clone();
    let digest = signature(session, questions);
    session["attention_signature"] = json!(digest);
    session["read"] = mark;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn waiting() -> Value {
        json!({"name":"codex/project/review","run_id":"run-1","conversation_id":"conversation-1",
            "phase":"input","attention_id":"call-1","process_state":"running","activity":"busy",
            "activity_summary":"Input needed","last_event_at":10.0,"turn_started":5.0,
            "subagent_source":"hooks","subagents":{}})
    }

    fn hex(value: &str) -> bool {
        value.len() == 64 && value.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    }

    #[test]
    fn signature_ignores_status_only_updates() {
        let first = signature(&waiting(), &[]).unwrap();
        assert!(hex(&first));
        let mut status = waiting();
        status["activity_summary"] = json!("Still waiting");
        status["last_event_at"] = json!(99.0);
        status["current_tool"] = json!("Bash");
        status["prompt"] = json!("Another preview");
        status["subagents"] = json!({"worker":{"state":"working","display_state":null,"updated":99.0}});
        assert_eq!(signature(&status, &[]), Some(first));
    }

    #[test]
    fn signature_changes_for_each_new_request() {
        let first = signature(&waiting(), &[]).unwrap();
        let mut next = waiting();
        next["attention_id"] = json!("call-2");
        assert_ne!(signature(&next, &[]).unwrap(), first);
        let question = json!({"question_id":"q-1","question_hash":"h-1"});
        assert_ne!(signature(&waiting(), &[question.clone()]).unwrap(), first);
        let mut child = waiting();
        child["subagents"] = json!({"helper":{"state":"working","display_state":"approval"}});
        assert_ne!(signature(&child, &[]).unwrap(), first);
        for (key, value) in [("run_id", "run-2"), ("conversation_id", "conversation-2"), ("phase", "approval")] {
            let mut changed = waiting();
            changed[key] = json!(value);
            assert_ne!(signature(&changed, &[]).unwrap(), first, "{key}");
        }
    }

    #[test]
    fn nothing_actionable_or_not_running_has_no_signature() {
        let mut working = waiting();
        working["phase"] = json!("working");
        assert_eq!(signature(&working, &[]), None);
        for (key, value) in [("state", "paused"), ("state", "stopped"), ("state", "archived"), ("process_state", "exited")] {
            let mut stopped = waiting();
            stopped[key] = json!(value);
            assert_eq!(signature(&stopped, &[]), None, "{key}={value}");
        }
        // Kimi profile groups carry no per-child request identity.
        let mut profiles = working.clone();
        profiles["subagent_source"] = json!("hook_profiles");
        profiles["subagents"] = json!({"helper":{"state":"input"}});
        assert_eq!(signature(&profiles, &[]), None);
    }

    #[test]
    fn pending_questions_need_attention_until_their_answer_is_submitted() {
        let mut working = waiting();
        working["phase"] = json!("working");
        let question = json!({"question_id":"q-1","question_hash":"h-1"});
        assert!(signature(&working, &[question.clone()]).is_some());
        let mut submitted = question;
        submitted["answer_delivery"] = json!({"status":"submitted"});
        assert_eq!(signature(&working, &[submitted]), None);
    }

    #[test]
    fn identity_less_errors_are_distinct_per_turn() {
        let mut failed = waiting();
        failed["phase"] = json!("error");
        failed["attention_id"] = json!("");
        let first = signature(&failed, &[]).unwrap();
        failed["last_event_at"] = json!(500.0);
        assert_eq!(signature(&failed, &[]).unwrap(), first);
        failed["turn_started"] = json!(6.0);
        assert_ne!(signature(&failed, &[]).unwrap(), first);
    }

    #[test]
    fn records_are_found_by_owner_and_conversation_only() {
        let marks = json!({"codex/project/original":{"conversation-1":{"conversation_id":"conversation-1",
            "reply_id":"7:100.5","attention_signature":null,"at":1.0}}});
        assert_eq!(record(&marks, "codex/project/original", "conversation-1")["reply_id"], "7:100.5");
        assert!(record(&marks, "codex/project/original", "conversation-2").is_null());
        assert!(record(&marks, "codex/project/original", "").is_null());
        let mut session = waiting();
        enrich(&mut session, &marks, "codex/project/original", &[]);
        assert_eq!(session["read"]["reply_id"], "7:100.5");
        assert!(hex(session["attention_signature"].as_str().unwrap()));
        enrich(&mut session, &marks, "codex/project/other-owner", &[]);
        assert!(session["read"].is_null());
    }
}
