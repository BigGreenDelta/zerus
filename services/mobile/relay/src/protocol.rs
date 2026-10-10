//! Strict v1 envelopes. Native agents remain the authority for execution.
use crate::{
    error::{Error, Result},
    json,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use std::collections::HashSet;
use uuid::Uuid;
pub const MIB: usize = 1024 * 1024;
pub const REQUEST_MAX: usize = 29 * MIB;
pub const READS: &[&str] = &[
    "inspect",
    "history",
    "process_output",
    "terminal_snapshot",
    "catalog",
    "dirs",
    "worktrees",
    "mark_read",
];
pub const READ_SQL: &str =
    "('inspect','history','process_output','terminal_snapshot','catalog','dirs','worktrees','mark_read')";
pub const LAUNCH: &[&str] = &["catalog", "dirs", "launch", "worktrees", "worktree_create"];
/// Computer-scoped: no session, delivered to the gateway computer itself.
pub const COMPUTER: &[&str] = &["set_mobile_delivery"];
pub const MOBILE_DELIVERY: &[&str] = &[
    "immediate",
    "away",
    "away_5",
    "away_10",
    "away_15",
    "away_30",
];
pub const OPERATIONS: &[&str] = &[
    "answer",
    "archive",
    "catalog",
    "clear_context",
    "compact_context",
    "dirs",
    "forget",
    "fork",
    "history",
    "inspect",
    "interrupt",
    "launch",
    "mark_read",
    "pause",
    "process_output",
    "process_stop",
    "recovery_action",
    "rename",
    "restore",
    "resume",
    "send",
    "send_now",
    "set_mobile_delivery",
    "settings",
    "terminal_input",
    "terminal_snapshot",
    "terminate",
    "worktree_create",
    "worktrees",
];
pub const FEATURES: &[&str] = &["inspect_after", "inspect_agent", "send_agent"];
const ID: &[&str] = &["request_id", "expected_run_id", "expected_conversation_id"];
pub fn fields(v: &Value, required: &[&str], optional: &[&str]) -> Result<()> {
    let o = v.as_object().ok_or(Error::BAD)?;
    if required.iter().any(|k| !o.contains_key(*k))
        || o.keys()
            .any(|k| !required.contains(&k.as_str()) && !optional.contains(&k.as_str()))
    {
        return Err(Error::BAD);
    }
    Ok(())
}
pub fn text(v: &Value, max: usize, empty: bool) -> Result<&str> {
    let s = v.as_str().ok_or(Error::BAD)?;
    if (!empty && s.is_empty())
        || s.chars().count() > max
        || s.chars().any(|c| c < ' ' || c == '\u{7f}')
    {
        return Err(Error::BAD);
    }
    Ok(s)
}
pub fn uuid(v: &Value, nonzero: bool) -> Result<&str> {
    let s = text(v, 36, false)?;
    let id = Uuid::parse_str(s).map_err(|_| Error::BAD)?;
    if id.to_string() != s || (nonzero && id.is_nil()) {
        return Err(Error::BAD);
    }
    Ok(s)
}
fn identity(p: &Value, id: &str, empty: bool) -> Result<()> {
    if p["request_id"] != id {
        return Err(Error::BAD);
    }
    text(&p["expected_run_id"], 128, false)?;
    text(&p["expected_conversation_id"], 256, empty)?;
    Ok(())
}
fn absolute(v: &Value) -> Result<()> {
    if !text(v, 4096, false)?.starts_with('/') {
        return Err(Error::BAD);
    }
    Ok(())
}
fn native_token(v: &Value, max: usize) -> Result<()> {
    let s = text(v, max, false)?;
    if !s
        .bytes()
        .all(|c| c.is_ascii_alphanumeric() || b"._:-".contains(&c))
    {
        return Err(Error::BAD);
    }
    Ok(())
}
fn hex(v: &Value) -> Result<()> {
    let s = text(v, 64, false)?;
    if s.len() != 64
        || !s
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    {
        return Err(Error::BAD);
    }
    Ok(())
}
pub fn supports(s: &Value, op: &str, field: &str) -> bool {
    s["mobile_capabilities"]["protocol_version"].as_i64() == Some(1)
        && s["mobile_capabilities"][field]
            .as_array()
            .is_some_and(|a| a.iter().any(|v| v == op))
}
pub fn request(v: &Value) -> Result<()> {
    fields(
        v,
        &[
            "request_id",
            "computer_id",
            "operation",
            "session",
            "payload",
        ],
        &[],
    )?;
    let id = uuid(&v["request_id"], false)?;
    text(&v["computer_id"], 36, false)?;
    let op = v["operation"]
        .as_str()
        .filter(|op| OPERATIONS.contains(op))
        .ok_or(Error::BAD)?;
    let p = &v["payload"];
    if !p.is_object() {
        return Err(Error::BAD);
    }
    text(
        &v["session"],
        1024,
        LAUNCH.contains(&op) || COMPUTER.contains(&op),
    )?;
    if LAUNCH.contains(&op) {
        if v["session"] != "" {
            return Err(Error::BAD);
        }
        launch(op, p, id)?;
    } else if COMPUTER.contains(&op) {
        fields(p, &["value"], &[])?;
        if v["session"] != "" || !MOBILE_DELIVERY.iter().any(|x| p["value"] == *x) {
            return Err(Error::BAD);
        }
    } else if op == "inspect" {
        fields(
            p,
            &[],
            &[
                "archive_id",
                "after",
                "agent_id",
                "expected_run_id",
                "expected_conversation_id",
            ],
        )?;
        if [
            "after",
            "agent_id",
            "expected_run_id",
            "expected_conversation_id",
        ]
        .iter()
        .any(|k| p.get(k).is_some())
        {
            text(&p["expected_run_id"], 128, false)?;
            text(&p["expected_conversation_id"], 256, false)?;
        }
        if let Some(a) = p.get("after") {
            if a.as_i64().is_none_or(|n| n < 0) {
                return Err(Error::BAD);
            }
        }
        if p.get("agent_id").is_some() {
            agent(&p["agent_id"])?;
            if p.get("after").is_some() {
                return Err(Error::BAD);
            }
        }
        if p.get("archive_id").is_some() {
            uuid(&p["archive_id"], false)?;
        }
    } else if op == "mark_read" {
        // C4: exact host read-mark identity; the host decides what is current.
        fields(
            p,
            &[
                "run_id",
                "conversation_id",
                "reply_id",
                "attention_signature",
            ],
            &[],
        )?;
        text(&p["run_id"], 128, false)?;
        text(&p["conversation_id"], 256, false)?;
        if !p["reply_id"].is_null() {
            text(&p["reply_id"], 256, false)?;
        }
        if !p["attention_signature"].is_null() {
            hex(&p["attention_signature"])?;
        }
    } else {
        let extras: &[&str] = match op {
            "send" => &["text", "attachments", "expected_compaction_id", "agent_id"],
            "answer" => &["question_id", "expected_question_hash", "answers"],
            "interrupt" => &["expected_turn_started"],
            "send_now" => &["queue_id"],
            "settings" => &["model", "effort", "expected_pending_id"],
            "process_output" => &["process_id", "generation", "archive_id"],
            "process_stop" => &["process_id", "generation"],
            "history" => &[
                "archive_id",
                "agent_id",
                "limit",
                "before",
                "after",
                "around",
                "around_incoming_seq",
                "history_epoch",
            ],
            "recovery_action" => &["job_id", "action"],
            "rename" => &["new_name", "archive_id"],
            "fork" => &["tag", "archive_id"],
            "restore" | "forget" => &["archive_id"],
            "terminal_input" => &["terminal_binding_id", "text", "enter", "key"],
            _ => &[],
        };
        fields(p, ID, extras)?;
        identity(
            p,
            id,
            ![
                "compact_context",
                "clear_context",
                "history",
                "recovery_action",
                "send_now",
            ]
            .contains(&op),
        )?;
        match op {
            "send" => send(p)?,
            "answer" => {
                for k in ["question_id", "expected_question_hash", "answers"] {
                    if p.get(k).is_none() {
                        return Err(Error::BAD);
                    }
                }
            }
            "interrupt" => {
                if p.get("expected_turn_started").is_none() {
                    return Err(Error::BAD);
                }
            }
            "send_now" => hex(&p["queue_id"])?,
            "settings" => {
                if p.get("model").is_none() && p.get("effort").is_none() {
                    return Err(Error::BAD);
                }
                if p.get("model").is_some() {
                    text(&p["model"], 120, false)?;
                }
                if let Some(e) = p.get("effort") {
                    if ![
                        "", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max",
                        "ultra", "on",
                    ]
                    .iter()
                    .any(|x| e == x)
                    {
                        return Err(Error::BAD);
                    }
                }
                if p.get("expected_pending_id").is_some() {
                    uuid(&p["expected_pending_id"], false)?;
                }
            }
            "process_output" | "process_stop" => {
                native_token(&p["process_id"], 160)?;
                if p.get("generation").is_some() {
                    native_token(&p["generation"], 256)?;
                }
            }
            "history" => history(p)?,
            "recovery_action" => {
                uuid(&p["job_id"], true)?;
                if p["action"] != "now" && p["action"] != "cancel" {
                    return Err(Error::BAD);
                }
            }
            "rename" => {
                let s = text(&p["new_name"], 512, false)?;
                if s.len() > 512 || s.trim() != s {
                    return Err(Error::BAD);
                }
            }
            "fork" => {
                if let Some(v) = p.get("tag") {
                    tag(v, false)?;
                }
            }
            "restore" => {
                uuid(&p["archive_id"], true)?;
            }
            "terminal_input" => terminal(p)?,
            _ => {}
        }
        if p.get("archive_id").is_some() {
            uuid(&p["archive_id"], op != "process_output")?;
        }
    }
    if op != "send" {
        json::size(v, MIB)?;
    }
    Ok(())
}
fn agent(v: &Value) -> Result<()> {
    if text(v, 160, false)?.contains(['/', '\\']) {
        return Err(Error::BAD);
    }
    Ok(())
}
fn tag(v: &Value, launch: bool) -> Result<()> {
    let s = text(v, 120, false)?;
    if s.len() > 120 || s.trim() != s || s.contains(['/', '.', ':']) || (launch && s.contains('\\'))
    {
        return Err(Error::BAD);
    }
    Ok(())
}
fn history(p: &Value) -> Result<()> {
    if let Some(v) = p.get("limit") {
        if v.as_i64().is_none_or(|n| !(1..=100).contains(&n)) {
            return Err(Error::BAD);
        }
    }
    if ["before", "after", "around", "around_incoming_seq"]
        .iter()
        .filter(|k| p.get(k).is_some())
        .count()
        > 1
    {
        return Err(Error::BAD);
    }
    for k in ["before", "after", "around"] {
        if let Some(v) = p.get(k) {
            text(v, 2048, false)?;
        }
    }
    if p.get("around_incoming_seq").is_some() != p.get("history_epoch").is_some() {
        return Err(Error::BAD);
    }
    if let Some(v) = p.get("around_incoming_seq") {
        if v.as_i64().is_none_or(|n| n < 0) {
            return Err(Error::BAD);
        }
        text(&p["history_epoch"], 128, false)?;
    }
    if let Some(v) = p.get("agent_id") {
        agent(v)?;
    }
    Ok(())
}
fn launch(op: &str, p: &Value, id: &str) -> Result<()> {
    if p["request_id"] != id {
        return Err(Error::BAD);
    }
    match op {
        "catalog" => fields(p, &["request_id"], &[])?,
        "dirs" => {
            fields(p, &["request_id", "path"], &[])?;
            let path = text(&p["path"], 4096, false)?;
            if path != "~" && !path.starts_with('/') {
                return Err(Error::BAD);
            }
        }
        "worktrees" | "worktree_create" => {
            fields(
                p,
                if op == "worktrees" {
                    &["request_id", "path"]
                } else {
                    &[
                        "request_id",
                        "path",
                        "common_dir",
                        "destination",
                        "branch",
                        "base",
                    ]
                },
                &[],
            )?;
            absolute(&p["path"])?;
            if op == "worktree_create" {
                absolute(&p["common_dir"])?;
                absolute(&p["destination"])?;
                for k in ["branch", "base"] {
                    let s = text(&p[k], 256, false)?;
                    if s.trim() != s || s.starts_with('-') {
                        return Err(Error::BAD);
                    }
                }
            }
        }
        "launch" => {
            fields(
                p,
                &["request_id", "agent", "directory", "tag"],
                &[
                    "account_id",
                    "swarm_id",
                    "project_id",
                    "project_folder_id",
                    "add_folder",
                    "worktree_folder_id",
                    "worktree_common_dir",
                ],
            )?;
            if !["codex", "claude", "kimi", "dsh"]
                .iter()
                .any(|a| p["agent"] == *a)
            {
                return Err(Error::BAD);
            }
            absolute(&p["directory"])?;
            tag(&p["tag"], true)?;
            if let Some(v) = p.get("account_id") {
                let s = text(v, 80, false)?;
                if !s
                    .bytes()
                    .enumerate()
                    .all(|(i, c)| c.is_ascii_alphanumeric() || c == b'_' || (i > 0 && c == b'-'))
                {
                    return Err(Error::BAD);
                }
            }
            if ["worktree_folder_id", "worktree_common_dir"]
                .iter()
                .any(|k| p.get(k).is_some())
            {
                for k in [
                    "swarm_id",
                    "project_id",
                    "add_folder",
                    "worktree_folder_id",
                    "worktree_common_dir",
                ] {
                    if p.get(k).is_none() {
                        return Err(Error::BAD);
                    }
                }
                if p["add_folder"] != false || p.get("project_folder_id").is_some() {
                    return Err(Error::BAD);
                }
                bounded_id(&p["worktree_folder_id"])?;
                absolute(&p["worktree_common_dir"])?;
            }
            if ["swarm_id", "project_id", "project_folder_id", "add_folder"]
                .iter()
                .any(|k| p.get(k).is_some())
            {
                text(&p["swarm_id"], 128, false)?;
                bounded_id(&p["project_id"])?;
                if !p["add_folder"].is_boolean() {
                    return Err(Error::BAD);
                }
                if let Some(v) = p.get("project_folder_id") {
                    bounded_id(v)?;
                    if p["add_folder"] != false {
                        return Err(Error::BAD);
                    }
                }
            }
        }
        _ => return Err(Error::BAD),
    }
    Ok(())
}
fn bounded_id(v: &Value) -> Result<()> {
    let s = text(v, 128, false)?;
    if s.len() > 128 || s.trim() != s {
        return Err(Error::BAD);
    }
    Ok(())
}
fn terminal(p: &Value) -> Result<()> {
    hex(&p["terminal_binding_id"])?;
    if let Some(k) = p.get("key") {
        let keys = [
            "Enter", "Escape", "Tab", "BTab", "BSpace", "Up", "Down", "Left", "Right", "Home",
            "End", "PPage", "NPage", "DC", "C-c", "C-d", "C-l", "C-a", "C-e", "C-u", "C-w",
        ];
        if p.as_object().unwrap().len() != 5 || !keys.iter().any(|x| k == x) {
            return Err(Error::BAD);
        }
    } else {
        let s = p["text"].as_str().ok_or(Error::BAD)?;
        if p.as_object().unwrap().len() != 6
            || !p["enter"].is_boolean()
            || s.len() > 65536
            || (s.is_empty() && p["enter"] != true)
            || s.chars()
                .any(|c| (c < ' ' && c != '\n' && c != '\t') || c == '\u{7f}')
        {
            return Err(Error::BAD);
        }
    }
    Ok(())
}
fn send(p: &Value) -> Result<()> {
    if let Some(c) = p.get("expected_compaction_id") {
        if c != "" {
            uuid(c, false)?;
        }
    }
    if let Some(a) = p.get("agent_id") {
        agent(a)?;
        text(&p["expected_conversation_id"], 256, false)?;
        if p.get("expected_compaction_id").is_some_and(|v| v != "") {
            return Err(Error::BAD);
        }
    }
    let text = p["text"].as_str().ok_or(Error::BAD)?;
    if text.len() > 65536
        || text
            .chars()
            .any(|c| c.is_control() && c != '\n' && c != '\t')
        || text.trim_start().starts_with(['/', '!'])
    {
        return Err(Error::BAD);
    }
    let empty = vec![];
    let files = match p.get("attachments") {
        Some(v) => v.as_array().ok_or(Error::BAD)?,
        None => &empty,
    };
    if files.len() > 8 || (text.trim().is_empty() && files.is_empty()) {
        return Err(Error::BAD);
    }
    let mut total = 0;
    let mut refs = HashSet::new();
    let mut decoded = [0u8; 49152];
    for f in files {
        fields(f, &["name", "mime", "data_base64"], &["reference"])?;
        let name = f["name"].as_str().ok_or(Error::BAD)?;
        let mime = f["mime"].as_str().ok_or(Error::BAD)?;
        if name.is_empty()
            || name.len() > 255
            || [".", ".."].contains(&name)
            || name.contains(['/', '\\'])
            || name.chars().any(char::is_control)
            || mime.is_empty()
            || mime.len() > 100
            || mime.chars().any(char::is_control)
        {
            return Err(Error::BAD);
        }
        if let Some(r) = f.get("reference") {
            let r = r.as_str().ok_or(Error::BAD)?;
            if !r.is_empty() {
                let n = r
                    .strip_prefix("[Image #")
                    .or_else(|| r.strip_prefix("[File #"))
                    .and_then(|s| s.strip_suffix(']'))
                    .ok_or(Error::BAD)?;
                if n.is_empty()
                    || n.len() > 9
                    || n.starts_with('0')
                    || !n.bytes().all(|c| c.is_ascii_digit())
                    || !refs.insert(r)
                {
                    return Err(Error::BAD);
                }
            }
        }
        let data = f["data_base64"].as_str().ok_or(Error::BAD)?;
        if data.is_empty() || data.len() > (10 * MIB).div_ceil(3) * 4 {
            return Err(Error::BAD);
        }
        let mut size = 0;
        let chunks = data.as_bytes().chunks(65536);
        let count = chunks.len();
        for (index, chunk) in chunks.enumerate() {
            if index + 1 < count && chunk.contains(&b'=') {
                return Err(Error::BAD);
            }
            size += STANDARD
                .decode_slice(chunk, &mut decoded)
                .map_err(|_| Error::BAD)?;
        }
        if size == 0 || size > 10 * MIB {
            return Err(Error::BAD);
        }
        total += size;
        if total > 20 * MIB {
            return Err(Error::BAD);
        }
    }
    Ok(())
}
pub fn snapshot(v: &Value) -> Result<()> {
    let a = v["sessions"].as_array().ok_or(Error::BAD)?;
    if !v.is_object() || a.len() > 5000 {
        return Err(Error::BAD);
    }
    for s in a {
        if !s.is_object() {
            return Err(Error::BAD);
        }
        if let Some(n) = s.get("name") {
            text(n, 1024, false)?;
        }
    }
    Ok(())
}
pub fn heartbeat(v: &Value) -> Result<()> {
    fields(v, &["snapshot"], &["machine_id", "peers"])?;
    snapshot(&v["snapshot"])?;
    if let Some(m) = v.get("machine_id") {
        uuid(m, false)?;
    }
    if let Some(p) = v.get("peers") {
        let p = p.as_array().ok_or(Error::BAD)?;
        if p.len() > 32 || (!p.is_empty() && v.get("machine_id").is_none()) {
            return Err(Error::BAD);
        }
        let mut seen = HashSet::new();
        for r in p {
            fields(r, &["route_id", "machine_id", "name", "online"], &[])?;
            let id = uuid(&r["route_id"], false)?;
            uuid(&r["machine_id"], false)?;
            text(&r["name"], 128, false)?;
            if !r["online"].is_boolean() || !seen.insert(id) {
                return Err(Error::BAD);
            }
        }
    }
    Ok(())
}
pub fn result(v: &Value, id: &str) -> Result<()> {
    fields(v, &["state", "result", "error"], &[])?;
    if !["completed", "failed", "uncertain"]
        .iter()
        .any(|s| v["state"] == *s)
    {
        return Err(Error::BAD);
    }
    if !v["error"].is_null() && v["error"].as_str().is_none_or(|s| s.chars().count() > 4096) {
        return Err(Error::BAD);
    }
    json::size(
        &json!({"request_id":id,"state":v["state"],"result":v["result"],"error":v["error"]}),
        MIB,
    )?;
    Ok(())
}
pub fn capabilities(providers: Vec<&str>) -> Value {
    json!({"protocol_version":1,"operations":OPERATIONS,"features":FEATURES,"push_providers":providers,"attachment_limits":{"max_count":8,"max_file_bytes":10*MIB,"max_total_bytes":20*MIB,"max_text_bytes":65536,"max_request_bytes":REQUEST_MAX}})
}
