//! Reconcile missing Claude child hooks with exact native turn boundaries.
//! Silence, a completed parent and ordinary tool output never end a child turn.
use super::*;
use std::fs::{self, File};
use std::io::{Seek, SeekFrom};
use sha2::{Digest, Sha256};

const MAX_WORKFLOWS: usize = 128;
const MAX_ENTRIES: usize = 4096;
const TAIL_BYTES: u64 = 64 * 1024;
const INTERRUPTED: &str = "[Request interrupted by user]";

fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 160 && !id.contains(['/', '\\'])
        && !id.chars().any(char::is_control)
}

/// Resolve only roster members in this conversation's account-owned directory.
/// Workflow children live one directory below subagents/workflows; duplicate
/// identities or an incomplete bounded discovery never choose an arbitrary file.
pub(super) fn paths(record: &Value, ids: &[&str]) -> BTreeMap<String, PathBuf> {
    locate(record, ids).unwrap_or_default()
}

fn locate(record: &Value, ids: &[&str]) -> Option<BTreeMap<String, PathBuf>> {
    if string(record, "agent") != "claude" || ids.iter().any(|id| !valid_id(id)) {
        return None;
    }
    let transcript = Path::new(string(record, "transcript"));
    let session = string(record, "conversation_id");
    if uuid::Uuid::parse_str(session).is_err()
        || !transcript.is_absolute()
        || transcript.file_stem()?.to_str()? != session {
        return None;
    }
    let home = if string(record, "agent_home").is_empty() {
        home().join(".claude")
    } else {
        PathBuf::from(string(record, "agent_home"))
    };
    let home = crate::platform::canonical(&home).ok()?;
    let root = crate::platform::canonical(transcript.with_extension("").join("subagents")).ok()?;
    if !root.starts_with(home) { return None; }
    let mut candidates = BTreeMap::<String, Vec<PathBuf>>::new();
    for id in ids {
        let path = root.join(format!("agent-{id}.jsonl"));
        if path.is_file() { candidates.entry((*id).to_owned()).or_default().push(path); }
    }
    let workflows = root.join("workflows");
    if workflows.exists() {
        // Do not follow a workflow-directory symlink out of the conversation.
        if fs::symlink_metadata(&workflows).ok()?.file_type().is_symlink() { return None; }
        let mut entries = 0;
        for (index, workflow) in fs::read_dir(workflows).ok()?.enumerate() {
            if index >= MAX_WORKFLOWS { return None; }
            let workflow = workflow.ok()?;
            if !workflow.file_type().ok()?.is_dir() { continue; }
            for entry in fs::read_dir(workflow.path()).ok()? {
                entries += 1;
                if entries > MAX_ENTRIES { return None; }
                let entry = entry.ok()?;
                let filename = entry.file_name();
                let Some(id) = filename.to_str().and_then(|name| name.strip_prefix("agent-"))
                    .and_then(|name| name.strip_suffix(".jsonl")) else { continue; };
                if ids.contains(&id) && entry.file_type().ok()?.is_file() {
                    candidates.entry(id.to_owned()).or_default().push(entry.path());
                }
            }
        }
    }
    Some(candidates.into_iter().filter_map(|(id, paths)| {
        let [path] = paths.as_slice() else { return None; };
        let path = crate::platform::canonical(path).ok()?;
        path.starts_with(&root).then_some((id, path))
    }).collect())
}

fn owned_row(row: &Value, session: &str, id: &str) -> bool {
    row["isSidechain"] == true && row["agentId"] == id && row["sessionId"] == session
}

fn workflow_result(path: &Path, session: &str, id: &str, result_hash: &str) -> bool {
    let Some(parent) = path.parent().filter(|p| p.parent().is_some_and(|p| p.ends_with("workflows"))) else { return false; };
    let Ok(journal) = crate::platform::canonical(parent.join("journal.jsonl")) else { return false; };
    if !journal.starts_with(parent) { return false; }
    let Some(results) = scan_cache::cached("claude-workflow-results-v1", &journal, session, || {
        let mut file = File::open(&journal).ok()?;
        let limit = 256 * 1024;
        let offset = file.metadata().ok()?.len().saturating_sub(limit);
        file.seek(SeekFrom::Start(offset)).ok()?;
        let mut bytes = Vec::new();
        file.take(limit + 1).read_to_end(&mut bytes).ok()?;
        if bytes.len() as u64 > limit || !bytes.ends_with(b"\n") { return None; }
        let mut started = BTreeMap::new();
        let mut results = serde_json::Map::new();
        for (index, line) in bytes.split_inclusive(|b| *b == b'\n').enumerate() {
            if offset > 0 && index == 0 { continue; }
            let row: Value = serde_json::from_slice(line).ok()?;
            let id = string(&row, "agentId");
            let key = string(&row, "key");
            if !valid_id(id) || key.is_empty() || key.len() > 160 { continue; }
            if row["type"] == "started" {
                started.insert(id.to_owned(), key.to_owned());
                results.remove(id);
            } else if row["type"] == "result" {
                results.remove(id);
                if started.get(id).is_some_and(|start| start == key) {
                    if let Some(result) = row["result"].as_str() {
                        results.insert(id.to_owned(), json!(format!("{:x}", Sha256::digest(result))));
                    }
                }
            }
        }
        Some(Value::Object(results))
    }) else { return false; };
    !result_hash.is_empty() && results[id] == result_hash
}

fn native_turn(path: &Path, session: &str, id: &str) -> Option<Value> {
    let key = format!("{session}/{id}");
    let value = scan_cache::cached("claude-child-turn-v3", path, &key, || {
        let mut file = File::open(path).ok()?;
        let offset = file.metadata().ok()?.len().saturating_sub(TAIL_BYTES);
        file.seek(SeekFrom::Start(offset)).ok()?;
        let mut bytes = Vec::new();
        file.take(TAIL_BYTES + 1).read_to_end(&mut bytes).ok()?;
        // A partially appended or over-budget tail cannot establish completion.
        if bytes.len() as u64 > TAIL_BYTES || !bytes.ends_with(b"\n") {
            return Some(Value::Null);
        }
        let lines: Vec<_> = bytes.split_inclusive(|b| *b == b'\n').collect();
        let Some((index, line)) = lines.iter().enumerate().rev()
            .find(|(_, line)| line.iter().any(|b| !b.is_ascii_whitespace())) else {
                return Some(Value::Null);
            };
        if offset > 0 && index == 0 { return Some(Value::Null); }
        let Ok(row) = serde_json::from_slice::<Value>(line) else { return Some(Value::Null); };
        let content = &row["message"]["content"];
        let exact_marker = content.as_array().is_some_and(|parts| parts.len() == 1
            && parts[0]["type"] == "text" && parts[0]["text"] == INTERRUPTED);
        if !owned_row(&row, session, id) || row["isMeta"] == true || row["isApiErrorMessage"] == true {
            return Some(Value::Null);
        }
        let at = search::timestamp(&row["timestamp"]);
        if at <= 0.0 { return Some(Value::Null); }
        let text = content.as_array().into_iter().flatten().filter(|p| p["type"] == "text")
            .map(|p| string(p, "text")).collect::<String>();
        let (state, kind, detail) = if row["type"] == "user" && row["userType"] == "external"
            && row["message"]["role"] == "user" && exact_marker {
            ("finished", "interrupt", "Request interrupted by user".to_owned())
        } else if row["type"] == "assistant" && row["message"]["role"] == "assistant"
            && row["message"]["stop_reason"] == "end_turn"
            && content.as_array().is_some_and(|parts| !parts.is_empty()
                && parts.iter().all(|p| ["text", "thinking", "redacted_thinking"].contains(&string(p, "type")))) {
            ("finished", "end_turn", text.clone())
        } else if row["type"] == "user" && row["message"]["role"] == "user"
            && row["toolEndsTurn"] == true {
            let source = string(&row, "sourceToolAssistantUUID");
            let sources: Vec<Value> = lines[..index].iter().filter_map(|line| serde_json::from_slice::<Value>(line).ok())
                .filter(|candidate| candidate["uuid"] == source && !source.is_empty()).collect();
            let [source] = sources.as_slice() else { return Some(Value::Null); };
            if !owned_row(source, session, id) || source["type"] != "assistant"
                || source["message"]["role"] != "assistant" || source["message"]["stop_reason"] != "tool_use"
                || search::timestamp(&source["timestamp"]) <= 0.0
                || search::timestamp(&source["timestamp"]) > at { return Some(Value::Null); }
            let calls: Vec<_> = source["message"]["content"].as_array().into_iter().flatten()
                .filter(|part| part["type"] == "tool_use").map(|part| string(part, "id")).collect();
            let valid = content.as_array().is_some_and(|parts| !parts.is_empty() && parts.iter().all(|part|
                part["type"] == "tool_result" && !string(part, "tool_use_id").is_empty()
                && calls.contains(&string(part, "tool_use_id")) && part["is_error"] != true));
            if !valid { return Some(Value::Null); }
            ("finished", "tool_end", "Native child turn completed".to_owned())
        } else if row["type"] == "assistant" && row["message"]["role"] == "assistant"
            && row["message"]["stop_reason"] == "tool_use"
            && content.as_array().is_some_and(|parts| parts.iter().any(|p| p["type"] == "tool_use")) {
            ("working", "tool_use", "Native child activity resumed".to_owned())
        } else if row["type"] == "user" && row["userType"] == "external"
            && row["message"]["role"] == "user" && !text.is_empty()
            && content.as_array().is_some_and(|parts| parts.iter().all(|p| p["type"] == "text")) {
            ("working", "user_message", "Native child activity resumed".to_owned())
        } else { return Some(Value::Null); };
        Some(json!({"state":state,"kind":kind,"at":at,"id":row["uuid"],
            "detail":journal::clipped(&json!(detail),240),
            "text_hash":if text.is_empty() { String::new() } else { format!("{:x}", Sha256::digest(&text)) }}))
    })?;
    value.is_object().then_some(value)
}

pub(super) fn reconcile(record: &mut Value) {
    if string(record, "agent") != "claude" { return; }
    let ids: Vec<_> = record["subagents"].as_object().into_iter().flatten()
        .filter(|(_, child)| child["external"] != true && ["working", "finished"].contains(&string(child,"state")))
        .map(|(id, _)| id.as_str()).collect();
    if ids.is_empty() { return; }
    let paths = paths(record, &ids);
    let session = string(record, "conversation_id").to_owned();
    let mut changed = false;
    for (id, path) in paths {
        let child = &mut record["subagents"][&id];
        let Some(updated) = child["updated"].as_f64().filter(|at| *at > 0.0) else { continue; };
        let Some(evidence) = native_turn(&path, &session, &id) else { continue; };
        let Some(at) = evidence["at"].as_f64() else { continue; };
        let refresh_result = at == updated && child["tracking_evidence"]["source"] == "claude_transcript"
            && child["tracking_evidence"]["native_id"] == evidence["id"] && evidence["kind"] == "end_turn";
        if (at <= updated && !refresh_result) || at <= child["started"].as_f64().unwrap_or(updated) { continue; }
        let workflow_confirmed = evidence["kind"] == "end_turn"
            && workflow_result(&path, &session, &id, string(&evidence,"text_hash"));
        // The workflow result can arrive after the native completion was saved.
        if refresh_result {
            child["tracking_evidence"]["workflow_result_confirmed"] = json!(workflow_confirmed);
            continue;
        }
        let state = string(&evidence, "state");
        let already_working = state == "working" && child["state"] == "working";
        child["state"] = json!(state);
        if !already_working {
            child["current_tool"] = json!("");
            child["detail"] = evidence["detail"].clone();
        }
        child["updated"] = json!(at);
        child["completion_source"] = json!(format!("claude_transcript_{}", string(&evidence,"kind")));
        child["tracking_evidence"] = json!({"source":"claude_transcript","kind":evidence["kind"],
            "at":at,"native_id":evidence["id"]});
        if evidence["kind"] == "end_turn" {
            child["tracking_evidence"]["workflow_result_confirmed"] = json!(workflow_confirmed);
        }
        for key in ["display_state", "attention_id"] { child.as_object_mut().unwrap().remove(key); }
        // Reconciliation does not replace the hook's existing reply/read identity.
        changed = true;
    }
    if changed {
        journal::settle_children(record);
        if record["main_done"] == true && ["idle", "working"].contains(&string(record,"phase"))
            && record["subagents"].as_object().is_some_and(|children| children.values().any(|child| child["state"] == "working")) {
            record["activity"] = json!("busy");
            record["phase"] = json!("working");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const SESSION: &str = "11111111-1111-4111-8111-111111111111";

    fn fixture() -> (tempfile::TempDir, Value, PathBuf) {
        let temp = tempfile::tempdir().unwrap();
        let home = temp.path().join("account");
        let transcript = home.join("projects/fixture").join(format!("{SESSION}.jsonl"));
        let root = transcript.with_extension("").join("subagents");
        fs::create_dir_all(&root).unwrap();
        fs::write(&transcript, "{}\n").unwrap();
        let record = json!({"agent":"claude","agent_home":home,"transcript":transcript,
            "conversation_id":SESSION,"main_done":true,"activity":"busy","phase":"working",
            "active_tools":{},"reply_id":"parent-reply","read":{"reply_id":"parent-reply"},
            "subagents":{"worker":{"name":"workflow-subagent","announced":true,"state":"working",
                "started":1.0,"updated":2.0,"current_tool":"Bash","reply_id":"earlier-child-reply"}}});
        (temp, record, root)
    }

    fn marker() -> Value {
        json!({"type":"user","userType":"external","isSidechain":true,"agentId":"worker",
            "sessionId":SESSION,"timestamp":"2026-10-13T16:01:12.203Z",
            "message":{"role":"user","content":[{"type":"text","text":INTERRUPTED}]}})
    }

    fn write(path: &Path, row: &Value) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, format!("{row}\n")).unwrap();
    }

    #[test]
    fn exact_workflow_interrupt_settles_child_and_preserves_replies() {
        let (_temp, mut record, root) = fixture();
        let path = root.join("workflows/wf-fixture/agent-worker.jsonl");
        write(&path, &marker());
        reconcile(&mut record);
        assert_eq!(record["subagents"]["worker"]["state"], "finished");
        assert_eq!(record["subagents"]["worker"]["current_tool"], "");
        assert_eq!(record["subagents"]["worker"]["reply_id"], "earlier-child-reply");
        assert_eq!(record["reply_id"], "parent-reply");
        assert_eq!(record["read"]["reply_id"], "parent-reply");
        assert_eq!(record["phase"], "idle");
        assert_eq!(paths(&record, &["worker"])["worker"], crate::platform::canonical(&path).unwrap());
        let settled = record.clone();
        reconcile(&mut record);
        assert_eq!(record, settled);
    }

    #[test]
    fn interruption_does_not_settle_other_child_or_parent_work() {
        for (key, value) in [("main_done", json!(false)),
            ("active_tools", json!({"tool":{"name":"Bash"}})),
            ("subagent_groups", json!({"group":{"active_count":1}}))] {
            let (_temp, mut record, root) = fixture();
            record[key] = value;
            write(&root.join("agent-worker.jsonl"), &marker());
            reconcile(&mut record);
            assert_eq!(record["subagents"]["worker"]["state"], "finished");
            assert_eq!(record["phase"], "working");
        }
        let (_temp, mut record, root) = fixture();
        record["subagents"]["live"] = json!({"name":"Review","state":"working","updated":2.0});
        write(&root.join("agent-worker.jsonl"), &marker());
        reconcile(&mut record);
        assert_eq!(record["subagents"]["live"]["state"], "working");
        assert_eq!(record["phase"], "working");
    }

    #[test]
    fn foreign_or_prose_markers_are_not_completion() {
        let mut session = marker(); session["sessionId"] = json!("foreign");
        let mut child = marker(); child["agentId"] = json!("foreign");
        let mut prose = marker(); prose["message"]["content"][0]["text"] = json!(format!("Example: {INTERRUPTED}"));
        let mut user = marker(); user["userType"] = json!("internal");
        let mut parent = marker(); parent["isSidechain"] = json!(false);
        for row in [session, child, prose, user, parent] {
            let (_temp, mut record, root) = fixture();
            write(&root.join("agent-worker.jsonl"), &row);
            let before = record.clone();
            reconcile(&mut record);
            assert_eq!(record["subagents"]["worker"]["state"], "working");
            assert_eq!(record["read"], before["read"]);
            assert_eq!(record["reply_id"], before["reply_id"]);
            assert_eq!(record["subagents"]["worker"]["current_tool"], before["subagents"]["worker"]["current_tool"]);
        }
    }

    #[test]
    fn ambiguous_partial_or_superseded_evidence_keeps_child_working() {
        for scenario in ["duplicate", "partial", "later-row", "later-hook", "external", "missing", "large", "bad-parent"] {
            let (_temp, mut record, root) = fixture();
            let path = root.join("agent-worker.jsonl");
            write(&path, &marker());
            match scenario {
                "duplicate" => write(&root.join("workflows/wf-fixture/agent-worker.jsonl"), &marker()),
                "partial" => { fs::write(&path, marker().to_string()).unwrap(); }
                "later-row" => { fs::write(&path, format!("{}\n{{\"type\":\"assistant\"}}\n", marker())).unwrap(); }
                "later-hook" => record["subagents"]["worker"]["updated"] = json!(search::timestamp(&marker()["timestamp"]) + 1.0),
                "external" => record["subagents"]["worker"]["external"] = json!(true),
                "missing" => { fs::remove_file(&path).unwrap(); }
                "large" => { fs::write(&path, format!("{}\n", "x".repeat(TAIL_BYTES as usize))).unwrap(); }
                "bad-parent" => record["conversation_id"] = json!("22222222-2222-4222-8222-222222222222"),
                _ => unreachable!(),
            }
            let before = record.clone();
            reconcile(&mut record);
            assert_eq!(record, before, "{scenario}");
        }
    }

    #[cfg(unix)]
    #[test]
    fn foreign_account_symlinks_do_not_establish_completion() {
        let (temp, mut record, root) = fixture();
        let foreign = temp.path().join("foreign/agent-worker.jsonl");
        write(&foreign, &marker());
        std::os::unix::fs::symlink(&foreign, root.join("agent-worker.jsonl")).unwrap();
        let before = record.clone();
        reconcile(&mut record);
        assert_eq!(record, before);
        fs::remove_file(root.join("agent-worker.jsonl")).unwrap();
        std::os::unix::fs::symlink(foreign.parent().unwrap(), root.join("workflows")).unwrap();
        reconcile(&mut record);
        assert_eq!(record, before);
    }

    fn assistant(at: &str, reason: &str, content: Value) -> Value {
        json!({"type":"assistant","isSidechain":true,"agentId":"worker","sessionId":SESSION,
            "uuid":"native-result","timestamp":at,"message":{"role":"assistant","stop_reason":reason,"content":content}})
    }

    #[test]
    fn final_native_reply_settles_a_missing_stop_and_journal_confirms_exact_attempt() {
        let (_temp, mut record, root) = fixture();
        let path = root.join("workflows/wf-fixture/agent-worker.jsonl");
        let result = "A complete native result. ".repeat(20);
        write(&path, &assistant("2026-10-13T16:02:00Z", "end_turn", json!([{"type":"text","text":result}])));
        reconcile(&mut record);
        assert_eq!(record["subagents"]["worker"]["state"], "finished");
        assert_eq!(record["subagents"]["worker"]["tracking_evidence"]["workflow_result_confirmed"], false);
        let journal = path.parent().unwrap().join("journal.jsonl");
        let started = json!({"type":"started","agentId":"worker","key":"attempt-one"});
        let returned = json!({"type":"result","agentId":"worker","key":"attempt-one","result":result});
        fs::write(&journal, format!("{started}\n{returned}\n")).unwrap();
        reconcile(&mut record);
        let child = &record["subagents"]["worker"];
        assert_eq!(child["state"], "finished");
        assert_eq!(child["tracking_evidence"]["kind"], "end_turn");
        assert_eq!(child["tracking_evidence"]["workflow_result_confirmed"], true);
        assert_eq!(child["reply_id"], "earlier-child-reply");
        let hash = format!("{:x}", Sha256::digest(&result));
        assert!(workflow_result(&path, SESSION, "worker", &hash));
        // A new attempt invalidates the old result, even with the same child ID.
        fs::write(&journal, format!("{started}\n{returned}\n{started}\n")).unwrap();
        assert!(!workflow_result(&path, SESSION, "worker", &hash));
        reconcile(&mut record);
        assert_eq!(record["subagents"]["worker"]["tracking_evidence"]["workflow_result_confirmed"], false);
        let foreign = json!({"type":"result","agentId":"worker","key":"other-attempt","result":result});
        fs::write(&journal, format!("{started}\n{foreign}\n")).unwrap();
        assert!(!workflow_result(&path, SESSION, "worker", &hash));
    }

    #[test]
    fn typed_tool_end_requires_the_exact_owned_call_and_successful_result() {
        for scenario in ["valid", "ordinary-result", "foreign-source", "wrong-call", "error", "missing-source"] {
            let (_temp, mut record, root) = fixture();
            let path = root.join("agent-worker.jsonl");
            let mut source = assistant("2026-10-13T16:02:00Z", "tool_use",
                json!([{"type":"tool_use","id":"call-one","name":"StructuredOutput","input":{}}]));
            let mut result = json!({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"call-one","content":"Complete"}]},
                "isSidechain":true,"agentId":"worker","sessionId":SESSION,"uuid":"tool-result",
                "timestamp":"2026-10-13T16:02:01Z","toolEndsTurn":true,"sourceToolAssistantUUID":"native-result"});
            match scenario {
                "ordinary-result" => result["toolEndsTurn"] = json!(false),
                "foreign-source" => source["agentId"] = json!("other"),
                "wrong-call" => result["message"]["content"][0]["tool_use_id"] = json!("other-call"),
                "error" => result["message"]["content"][0]["is_error"] = json!(true),
                _ => (),
            }
            let data = if scenario == "missing-source" { format!("{result}\n") } else { format!("{source}\n{result}\n") };
            fs::write(&path, data).unwrap();
            reconcile(&mut record);
            assert_eq!(record["subagents"]["worker"]["state"], if scenario == "valid" { "finished" } else { "working" }, "{scenario}");
        }
    }

    #[test]
    fn missing_start_is_recovered_after_native_or_hook_completion_and_newer_hooks_win() {
        for prior in ["native", "hook"] {
            let (_temp, mut record, root) = fixture();
            let path = root.join("agent-worker.jsonl");
            if prior == "native" {
                write(&path, &marker());
                reconcile(&mut record);
            } else {
                record["subagents"]["worker"]["state"] = json!("finished");
                record["subagents"]["worker"]["updated"] = json!(search::timestamp(&marker()["timestamp"]));
                record["phase"] = json!("idle");
                record["activity"] = json!("idle");
            }
            let resumed = assistant("2026-10-13T16:03:00Z", "tool_use",
                json!([{"type":"tool_use","id":"next-call","name":"Read","input":{}}]));
            write(&path, &resumed);
            reconcile(&mut record);
            assert_eq!(record["subagents"]["worker"]["state"], "working", "{prior}");
            assert_eq!(record["phase"], "working");
            assert_eq!(record["reply_id"], "parent-reply");
            record["subagents"]["worker"]["state"] = json!("finished");
            record["subagents"]["worker"]["updated"] = json!(search::timestamp(&resumed["timestamp"]) + 1.0);
            let before = record.clone();
            reconcile(&mut record);
            assert_eq!(record, before);
        }
    }

    #[test]
    fn ordinary_text_and_unreviewed_stop_reasons_never_finish_children() {
        for reason in ["", "max_tokens", "refusal", "tool_use"] {
            let (_temp, mut record, root) = fixture();
            write(&root.join("agent-worker.jsonl"), &assistant("2026-10-13T16:03:00Z", reason,
                json!([{"type":"text","text":"Done"}])));
            let before = record.clone();
            reconcile(&mut record);
            assert_eq!(record, before);
        }
        for flag in ["isApiErrorMessage", "isMeta"] {
            let (_temp, mut record, root) = fixture();
            let mut row = assistant("2026-10-13T16:03:00Z", "end_turn", json!([{"type":"text","text":"Done"}]));
            row[flag] = json!(true);
            write(&root.join("agent-worker.jsonl"), &row);
            let before = record.clone();
            reconcile(&mut record);
            assert_eq!(record, before);
        }
    }
}
