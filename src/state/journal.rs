use super::*;
use rusqlite::{params, Connection, OptionalExtension};
use std::fs::OpenOptions;
use std::os::unix::fs::OpenOptionsExt;
use std::sync::OnceLock;

/// Final replies are the conversation itself, not an excerpt: Activity reads
/// them from the journal once the provider transcript window has moved on.
pub(super) const REPLY_LIMIT: usize = 32_000;

pub(super) fn clipped(value: &Value, limit: usize) -> String {
    let Some(value) = value.as_str() else {
        return String::new();
    };
    static ANSI: OnceLock<regex::Regex> = OnceLock::new();
    let stripped = ANSI
        .get_or_init(|| regex::Regex::new(r"\x1b\[[0-?]*[ -/]*[@-~]").unwrap())
        .replace_all(value, "");
    let mut characters = stripped
        .chars()
        .filter(|c| *c == '\n' || *c == '\t' || *c >= ' ');
    let mut result: String = characters.by_ref().take(limit).collect();
    if characters.next().is_some() {
        result.push('…');
    }
    result
}

/// Claude hands finished background work and other sessions' messages to the
/// agent as a "user" prompt made of XML envelopes the person never wrote: task
/// notifications, subagent reports and cross-session messages. Several may
/// arrive together, and a journal excerpt may be clipped.
pub(super) fn system_prompt(text: &str) -> Option<Value> {
    fn envelope<'a>(body: &'a str, close: &str) -> Option<(&'a str, &'a str)> {
        match body.find(close) {
            Some(end) => Some((&body[..end], body[end + close.len()..].trim_start())),
            None if body.ends_with('…') => Some((body, "")),
            None => None,
        }
    }
    let mut rest = text.trim();
    let (mut summaries, mut status, mut reports, mut agent) = (Vec::new(), String::new(), Vec::new(), String::new());
    let (mut messages, mut sender) = (Vec::new(), String::new());
    while !rest.is_empty() {
        if let Some(body) = rest.strip_prefix("<task-notification>") {
            let (inner, after) = envelope(body, "</task-notification>")?;
            let field = |tag: &str| {
                let value = inner.split_once(&format!("<{tag}>"))?.1.split_once(&format!("</{tag}>"))?.0;
                Some(["&lt;", "&gt;", "&quot;", "&apos;", "&amp;"].iter().zip(["<", ">", "\"", "'", "&"])
                    .fold(value.trim().to_owned(), |text, (from, to)| text.replace(from, to)))
            };
            status = field("status").unwrap_or_default();
            summaries.push(field("summary").filter(|s| !s.is_empty()).unwrap_or_else(|| {
                if status.is_empty() { "Background task finished".into() } else { format!("Background task {status}") }
            }));
            rest = after;
        } else if let Some(body) = rest.strip_prefix("<agent-message from=\"") {
            let (from, body) = body.split_once("\">")?;
            if from.is_empty() || from.len() > 128 || !from.chars().all(|c| c.is_ascii_alphanumeric() || "-_".contains(c)) {
                return None;
            }
            let (inner, after) = envelope(body, "</agent-message>")?;
            // A hand-back starts with Claude's framing paragraph and indents
            // every line of the report itself.
            const FRAME: &str = "The report follows:\n";
            let inner = inner.trim_start_matches('\n');
            let report = match inner.find(FRAME) {
                Some(at) if inner.starts_with('[') => &inner[at + FRAME.len()..],
                _ => inner,
            };
            reports.push(report.lines().map(|line| line.strip_prefix("  ").unwrap_or(line)).collect::<Vec<_>>().join("\n").trim().to_owned());
            if agent.is_empty() { agent = from.to_owned(); }
            rest = after;
        } else if let Some(body) = rest.strip_prefix("<cross-session-message ") {
            // Another local Claude session wrote to this one; the user did not.
            let (attributes, body) = body.split_once('>')?;
            let attribute = |name: &str| attributes.split_once(&format!("{name}=\""))
                .and_then(|(_, value)| value.split_once('"')).map(|(value, _)| value.trim());
            let name = attribute("from-name").filter(|n| !n.is_empty()).or_else(|| attribute("from")).unwrap_or_default();
            if name.is_empty() || name.len() > 200 {
                return None;
            }
            let (inner, after) = envelope(body, "</cross-session-message>")?;
            messages.push(inner.trim().to_owned());
            if sender.is_empty() { sender = name.to_owned(); }
            rest = after;
        } else {
            return None;
        }
    }
    let kind = if !reports.is_empty() { "subagent_report" } else if !messages.is_empty() { "peer_message" } else if !summaries.is_empty() { "task_notification" } else { return None };
    reports.extend(messages);
    Some(json!({"kind": kind, "summary": summaries.join("\n"), "status": status,
        "report": reports.join("\n\n"), "agent_id": agent, "sender": sender}))
}

/// Show background work handed to the agent as a notice rather than the
/// person's message, including journal rows written before HGS recognized it.
pub(super) fn mark_system_prompt(record: &Value, event: &mut Value, prompt: &str) {
    if !string(event, "agent_id").is_empty()
        || !["UserPromptSubmit", "UserPromptQueued", "TurnStarted"].contains(&string(event, "type"))
        || event["origin"].is_string()
    {
        return;
    }
    let text = if prompt.is_empty() { string(event, "detail") } else { prompt };
    let Some(notice) = system_prompt(text) else { return };
    let report = string(&notice, "report");
    let child = &record["subagents"][string(&notice, "agent_id")];
    // Name the subagent as Claude does in its terminal; older reports fall
    // back to their first line.
    let summary = if notice["kind"] == "peer_message" {
        format!("Message from {}", string(&notice, "sender"))
    } else if !string(&notice, "summary").is_empty() {
        string(&notice, "summary").to_owned()
    } else if !string(child, "description").is_empty() {
        format!("Agent \"{}\" finished", string(child, "description"))
    } else {
        report.lines().map(|line| line.trim_start_matches('#').trim()).find(|line| !line.is_empty()).unwrap_or("Subagent report").to_owned()
    };
    event["origin"] = notice["kind"].clone();
    event["detail"] = json!(clipped(&json!(summary), 1200));
    event["task_status"] = notice["status"].clone();
    if !report.is_empty() {
        event["report"] = json!(clipped(&notice["report"], 32000));
        event["from_agent"] = notice["agent_id"].clone();
    }
    if notice["kind"] == "peer_message" {
        event["sender"] = notice["sender"].clone();
    }
}

/// The person's latest request. Older records may hold background work.
pub(super) fn user_prompt(record: &Value) -> &Value {
    static NONE: Value = Value::Null;
    if system_prompt(string(record, "prompt")).is_some() { &NONE } else { &record["prompt"] }
}

fn detail(event: &Value) -> String {
    if event["hook_event_name"] == "StopFailure" {
        for key in [
            "error_message",
            "error_details",
            "last_assistant_message",
            "error",
            "error_type",
        ] {
            if !string(event, key).is_empty() {
                return clipped(&event[key], 1200);
            }
        }
    }
    let tool = &event["tool_input"];
    [
        (&event["prompt"], 1200),
        (&event["error_message"], 1200),
        (&event["error"], 1200),
        (&tool["command"], 1200),
        (&tool["file_path"], 1200),
        (&tool["path"], 1200),
        (&tool["description"], 1200),
        (&event["last_assistant_message"], REPLY_LIMIT),
        (&event["response"], REPLY_LIMIT),
        (&event["body"], 1200),
        (&event["description"], 1200),
    ]
    .into_iter()
    .find(|(value, _)| value.as_str().map(|s| !s.is_empty()).unwrap_or(false))
    .map(|(value, limit)| clipped(value, limit))
    .unwrap_or_default()
}

pub(super) fn event_db() -> Result<Connection> {
    private_dir(&root())?;
    let path = root().join("events.sqlite3");
    OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .mode(0o600)
        .open(&path)
        .map_err(|e| e.to_string())?;
    let db = Connection::open(path).map_err(|e| e.to_string())?;
    db.busy_timeout(std::time::Duration::from_secs(3))
        .map_err(|e| e.to_string())?;
    db.execute_batch("CREATE TABLE IF NOT EXISTS events (seq INTEGER PRIMARY KEY, name TEXT, conversation TEXT, payload TEXT);
        CREATE INDEX IF NOT EXISTS events_session ON events(name, conversation, seq);
        CREATE INDEX IF NOT EXISTS events_main_replies ON events(name, conversation, seq)
        WHERE json_extract(payload, '$.type') = 'Stop' AND json_extract(payload, '$.agent_id') = '';
        CREATE INDEX IF NOT EXISTS events_main_messages ON events(name, conversation, seq)
        WHERE COALESCE(json_extract(payload, '$.agent_id'), '') IN ('', 'main')
          AND json_extract(payload, '$.type') IN ('UserPromptSubmit','UserPromptQueued','UserMessage','TurnStarted','QuestionAnswered','AgentMessage','Stop');").map_err(|e| e.to_string())?;
    Ok(db)
}

// Kimi names what opened a turn: the person, a scheduled job, a task...
fn turn_origin<'a>(record: &Value, event: &'a Value) -> Option<&'a str> {
    let origin = string(event, "origin_kind");
    (string(record, "agent") == "kimi"
        && string(event, "hook_event_name") == "TurnStarted"
        && (1..=40).contains(&origin.len())
        && origin.bytes().all(|b| b.is_ascii_lowercase() || b == b'_'))
    .then_some(origin)
}

pub(super) fn log_event(record: &Value, event: &Value) -> Result<()> {
    if string(event, "hook_event_name") == "SessionHeartbeat" {
        return Ok(());
    }
    let mut data = json!({"type": event["hook_event_name"], "at": now(), "run_id": record["run_id"],
        "agent_id": clipped(&event["agent_id"], 160), "tool": clipped(&event["tool_name"], 100), "detail": detail(event)});
    if string(event, "hook_event_name") == "SessionCleared" {
        data["at"] = event["at"].clone();
        data["activity_key"] = event["activity_key"].clone();
    }
    if let Some(id)=processes::event_job(record,event) {data["process_id"]=json!(id);}
    mark_system_prompt(record, &mut data, event["prompt"].as_str().unwrap_or(""));
    if string(record, "agent") == "kimi"
        && string(event, "agent_id") == "main"
        && !["SubagentStart", "SubagentStop"].contains(&string(event, "hook_event_name"))
    {
        data["agent_id"] = json!("");
    }
    if let Some(origin) = turn_origin(record, event) {
        data["turn_origin"] = json!(origin);
    }
    if let Some(question) = questions::from_hook(record, event) {
        data["question_request"] = question;
    }
    if string(event, "hook_event_name") == "QuestionAnswered" {
        for key in ["question_id", "question_hash", "tool_call_id"] {
            data[key] = event[key].clone();
        }
        if event["at"].as_f64().is_some_and(|at| at > 0.0) {
            data["at"] = event["at"].clone();
        }
    }
    let mut db = event_db()?;
    let transaction = db.transaction().map_err(|e| e.to_string())?;
    transaction
        .execute(
            "INSERT INTO events(name, conversation, payload) VALUES (?, ?, ?)",
            params![
                journal_name(record),
                record["conversation_id"].as_str(),
                data.to_string()
            ],
        )
        .map_err(|e| e.to_string())?;
    let seq = transaction.last_insert_rowid();
    if seq % 100 == 0 {
        transaction
            .execute("DELETE FROM events WHERE seq <= ?", [seq - 50000])
            .map_err(|e| e.to_string())?;
    }
    transaction.commit().map_err(|e| e.to_string())
}

pub(super) fn normalized_activity(record: &Value, live_pane: bool) -> Value {
    let alive = live_pane && process_alive(record);
    let mut activity = record.get("activity").cloned().unwrap_or(json!("unknown"));
    let mut phase = record.get("phase").cloned().unwrap_or(json!("unknown"));
    // Legacy SessionEnd was overloaded as process death. It is not evidence of
    // either process death or idleness; old manifests remain restorable.
    if live_pane && (activity == "ended" || phase == "ended") && alive {
        activity = json!("unknown");
        phase = json!("unknown");
    }
    if alive && telemetry::grouped_active(record) > 0 && activity == "idle" {
        activity = json!("busy");
        phase = json!("working");
    }
    json!({"activity": activity, "phase": phase,
        "conversation_state": record.get("conversation_state").cloned().unwrap_or_else(||
            json!(if string(record, "activity") == "ended" { "ended" } else { "active" })),
        "runtime_state": if live_pane {"live"} else {"stopped"},
        "process_state": if alive {"running"} else if string(record, "process_start").is_empty() {"unknown"} else {"exited"}})
}

// A durable Stop event identifies a response, even if busy/idle transitions were
// missed between polls. Child stops and initial SessionStart are not replies.
// Reading the retained journal also covers sessions created before this feature.
fn latest_reply(record: &Value, recurring_schedules: Option<u64>) -> Result<Value> {
    let conversation = string(record, "conversation_id");
    if conversation.is_empty() {
        return Ok(json!({}));
    }
    let db = event_db()?;
    let reply = |seq: i64, at: Value, scheduled: bool| {
        json!({"reply_id":format!("{}:{}", seq, at), "reply_at":at, "reply_scheduled":scheduled})
    };
    if string(record, "agent") == "kimi" {
        let skip = recurring_schedules.is_some_and(|count| count > 0);
        return Ok(kimi_reply(&db, record, skip)?
            .map_or(json!({}), |(seq, at, scheduled)| reply(seq, at, scheduled)));
    }
    let result: Option<(i64, String)> = db
        .query_row(
            "SELECT seq, payload FROM events WHERE name = ? AND conversation = ?
         AND json_extract(payload, '$.type') = 'Stop' AND json_extract(payload, '$.agent_id') = ''
         ORDER BY seq DESC LIMIT 1",
            params![journal_name(record), conversation],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .optional()
        .map_err(|e| e.to_string())?;
    let Some((seq, payload)) = result else {
        return Ok(json!({}));
    };
    let event: Value = serde_json::from_str(&payload).map_err(|e| e.to_string())?;
    Ok(reply(seq, event["at"].clone(), false))
}

// A scheduled job opens its turn without a prompt from the person; the latest
// opening before a Stop decides, so a prompt queued meanwhile is the person's.
// While a recurring job remains, its routine ticks are not replies: they would
// mark the conversation unread every few minutes. Returns (seq, at, scheduled).
fn kimi_reply(db: &Connection, record: &Value, skip_scheduled: bool) -> Result<Option<(i64, Value, bool)>> {
    let mut statement = db
        .prepare(
            "SELECT seq, json_extract(payload, '$.type'), json_extract(payload, '$.turn_origin'), payload FROM events
         WHERE name = ? AND conversation = ?
           AND COALESCE(json_extract(payload, '$.agent_id'), '') IN ('', 'main')
           AND json_extract(payload, '$.type') IN ('UserPromptSubmit','UserPromptQueued','UserMessage','TurnStarted','QuestionAnswered','AgentMessage','Stop')
         ORDER BY seq DESC LIMIT 4000",
        )
        .map_err(|e| e.to_string())?;
    let rows = statement
        .query_map(
            params![journal_name(record), string(record, "conversation_id")],
            |row| Ok((row.get::<_, i64>(0)?, row.get::<_, Option<String>>(1)?,
                row.get::<_, Option<String>>(2)?, row.get::<_, String>(3)?)),
        )
        .map_err(|e| e.to_string())?;
    let mut stop: Option<(i64, Value)> = None;
    for row in rows {
        let (seq, kind, origin, payload) = row.map_err(|e| e.to_string())?;
        match (kind.as_deref(), stop.take()) {
            (Some("AgentMessage"), pending) => stop = pending,
            (Some("Stop"), None) => {
                let event: Value = serde_json::from_str(&payload).map_err(|e| e.to_string())?;
                stop = Some((seq, event["at"].clone()));
            }
            (Some("TurnStarted"), Some((seq, at))) => {
                let scheduled = matches!(origin.as_deref(), Some("cron_job" | "cron_missed"));
                if !scheduled || !skip_scheduled {
                    return Ok(Some((seq, at, scheduled)));
                }
            }
            (_, Some((seq, at))) => return Ok(Some((seq, at, false))),
            (_, None) => {}
        }
    }
    Ok(stop.map(|(seq, at)| (seq, at, false)))
}

pub(super) fn summary(record: &Value, live_pane: bool) -> Value {
    let mut output = normalized_activity(record, live_pane);
    output["conversation_id"] = record["conversation_id"].clone();
    output
        .as_object_mut()
        .unwrap()
        .extend(goals::summary(record).as_object().unwrap().clone());
    if let Ok(reply) = latest_reply(record, output["recurring_schedules"].as_u64()) {
        output
            .as_object_mut()
            .unwrap()
            .extend(reply.as_object().unwrap().clone());
    }
    if !string(record, "launch_id").is_empty() {
        output["launch_id"] = record["launch_id"].clone();
    }
    let last_tool = record["active_tools"]
        .as_object()
        .filter(|_| live_pane && output["activity"] == "busy")
        .and_then(|tools| {
            tools.values().max_by(|a, b| {
                a["started"]
                    .as_f64()
                    .unwrap_or_default()
                    .total_cmp(&b["started"].as_f64().unwrap_or_default())
            })
        });
    let tool = last_tool.map(|tool| string(tool, "name")).unwrap_or("");
    let detail = last_tool.map(|tool| string(tool, "detail")).unwrap_or("");
    let telemetry = telemetry::summary(record, &output, tool, detail);
    output
        .as_object_mut()
        .unwrap()
        .extend(telemetry.as_object().unwrap().clone());
    output.as_object_mut().unwrap().extend(json!({"tracked": true, "run_id":record["run_id"], "model": string(record, "model"),
        "cwd":record["cwd"].as_str().unwrap_or_else(||string(record,"launch_dir")), "cwd_source":if live_pane {"hook"} else {"saved"},
        "prompt": clipped(user_prompt(record), 240), "last_event_at": record.get("last_event_at").unwrap_or(&json!(0)),
        "turn_started": record.get("turn_started").unwrap_or(&json!(0)),
        "compaction_started": if record["phase"] == "compacting" { record.get("compaction_started").or_else(||record.get("last_main_progress_at")).unwrap_or(&json!(0)).clone() } else { json!(0) },
        "current_tool": last_tool.map(|tool| string(tool, "name")).unwrap_or(""),
        "tool_detail": last_tool.map(|tool| string(tool, "detail")).unwrap_or("")}).as_object().unwrap().clone());
    output.as_object_mut().unwrap().extend(
        effort::summary(record, live_pane)
            .as_object()
            .unwrap()
            .clone(),
    );
    output.as_object_mut().unwrap().extend(
        fork::summary(record, live_pane)
            .as_object()
            .unwrap()
            .clone(),
    );
    output.as_object_mut().unwrap().extend(session_action::summary(record,live_pane).as_object().unwrap().clone());
    output["terminal_supported"] = json!(live_pane && string(record,"archive_id").is_empty() && process_alive(record));
    output["terminal_reason"] = json!(if output["terminal_supported"] == true { "" } else { "Terminal requires this exact live tracked tmux pane" });
    output["subagent_count"] = output["subagent_active_count"].clone();
    let account = account_binding(record);
    output["account_id"] = account["id"].clone();
    output["account_home"] = account["home"].clone();
    if ["input", "approval", "error"].contains(&string(&output, "phase")) {
        // Stable across heartbeats, different for a new question even when the
        // intervening working state fell between two desktop polls.
        output["attention_id"] = json!(record["active_tools"]
            .as_object()
            .map(|tools| tools.keys().cloned().collect::<Vec<_>>().join("\n"))
            .unwrap_or_default());
        if output["phase"] != "error" {
            output["attention_started"] = record.get("attention_started").cloned().unwrap_or(json!(0));
        }
    }
    resume_input::enrich(record, &mut output, live_pane);
    provider_errors::enrich(record, &mut output, live_pane);
    if live_pane {
        if let Some(question) = claude_resume::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Claude is waiting for your conversation resume choice");
        }
        if let Some(question) = claude_permission::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Claude is waiting for your default permission mode choice");
        }
        if let Some(question) = claude_trust::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Claude is waiting for folder trust approval");
        }
        if let Some(question) = codex_trust::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Codex is waiting for folder trust approval");
        }
        if let Some(question) = codex_hooks_trust::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Codex is waiting for hook trust review");
        }
        if let Some(question) = codex_update::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("input");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Codex is waiting for an update choice");
        }
        if let Some(question) = kimi_trust::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("approval");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Kimi is waiting for folder trust approval");
        }
        if let Some(question) = kimi_cache_hint::current(record) {
            output["activity"] = json!("busy");
            output["phase"] = json!("input");
            output["attention_id"] = question["question_id"].clone();
            output["status_detail"] = json!("Kimi is waiting for your context choice");
        }
    }
    recovery::enrich(record, &mut output);
    output
}

const MESSAGE_EVENTS_LIMIT: usize = 100;
const MESSAGE_EVENTS_BYTES: usize = 256 * 1024;

// Activity continues across confirmed clears: rows of the current conversation
// and of the conversations it replaced. Sequence numbers are global, so a
// reader's cursor stays valid when a clear changes the conversation.
pub(super) const TIMELINE: &str = "name=?1 AND (conversation IS ?2 OR conversation IN (SELECT value FROM json_each(?3)))";

// The activity tail includes tools and children. Keep main public messages in
// a separate bounded window so a cold reader does not lose its newest prompt
// merely because that turn has produced more than one hundred tool events.
fn message_events(db: &Connection, name: &str, conversation: Option<&str>, earlier: &str) -> Result<(Vec<Value>, bool)> {
    let mut query = db.prepare(&format!("SELECT seq,payload FROM events WHERE {TIMELINE}
        AND COALESCE(json_extract(payload, '$.agent_id'), '') IN ('', 'main')
        AND json_extract(payload, '$.type') IN ('SessionCleared','UserPromptSubmit','UserPromptQueued','UserMessage','TurnStarted','QuestionAnswered','AgentMessage','Stop')
        ORDER BY seq DESC LIMIT 101")).map_err(|e| e.to_string())?;
    let rows = query.query_map(params![name, conversation, earlier], |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?)))
        .map_err(|e| e.to_string())?;
    let (mut events, mut bytes, mut truncated) = (Vec::new(), 2, false);
    for row in rows {
        let (seq, payload) = row.map_err(|e| e.to_string())?;
        if events.len() == MESSAGE_EVENTS_LIMIT { truncated = true; break; }
        let mut event: Value = serde_json::from_str(&payload).map_err(|e| e.to_string())?;
        event["seq"] = json!(seq);
        let size = event.to_string().len() + usize::from(!events.is_empty());
        if bytes + size > MESSAGE_EVENTS_BYTES { truncated = true; break; }
        bytes += size;
        events.push(event);
    }
    events.reverse();
    Ok((events, truncated))
}

pub(super) fn inspection(name: &str, after: i64, archive_id: Option<&str>, include_processes: bool) -> Result<Value> {
    let _guard = lock(None)?;
    let untracked = || {
        let mut value = json!({"name": name, "tracked": false, "events": [], "cursor": 0});
        value
            .as_object_mut()
            .unwrap()
            .extend(telemetry::unavailable().as_object().unwrap().clone());
        workspace::enrich(std::slice::from_mut(&mut value), &workspace::pane_cwds());
        value
    };
    if archive_id.is_none() && !record_path(name).exists() {
        drop(_guard);
        return Ok(untracked());
    }
    let mut record = if let Some(id) = archive_id {
        archive::read_archive(name, id)?
    } else {
        read(name)?
    };
    let snapshot = live()?;
    let panes = if archive_id.is_some() {
        None
    } else {
        snapshot.get(name)
    };
    if panes.is_some() && !matches(&record, panes) {
        drop(_guard);
        return Ok(untracked());
    }
    if archive_id.is_none() && panes.is_some() {
        let _ = clear_context::observe_terminal(&mut record);
    }
    let mut output = json!({});
    for key in [
        "name",
        "conversation_id",
        "run_id",
        "cwd",
        "agent",
        "model",
        "phase",
        "activity",
        "prompt",
        "last_message",
        "last_error",
        "active_tools",
        "subagents",
        "subagent_groups",
        "subagent_groups_complete",
        "task_lists",
        "last_event_at",
        "turn_started",
        "compaction_started",
        "created",
        "updated",
        "archive_id",
        "archived_at",
        "completion_source",
        "completion_reason",
        "exit_code",
        "termination_signal",
        "exited_at",
        "session_end",
        "last_restore_error",
        "error",
        "expected_id",
        "requested_id",
        "prompt_suggestion",
    ] {
        if let Some(value) = record.get(key) {
            output[key] = value.clone();
        }
    }
    output.as_object_mut().unwrap().extend(
        summary(&record, panes.is_some())
            .as_object()
            .unwrap()
            .clone(),
    );
    // The compact list preview must not shorten the existing inspector field.
    if !user_prompt(&record).is_null() {
        output["prompt"] = record["prompt"].clone();
    }
    output["tracked"] = json!(true);
    if archive_id.is_some() {
        output["state"] = json!("archived");
    }
    drop(_guard);
    if archive_id.is_none() {
        output.as_object_mut().unwrap().extend(
            input::first_message_summary(&record, panes.is_some())
                .as_object()
                .unwrap()
                .clone(),
        );
        input::attention_message_summary(&record, &mut output, panes.is_some());
    }
    match questions::inspection(&record, panes.is_some()) {
        Ok(pending) => output["pending_questions"] = pending,
        Err(error) => {
            output["pending_questions"] = json!([]);
            output["pending_questions_error"] = json!(error);
        }
    }
    output["input_queue"] = if archive_id.is_none() { input_queue::inspection(&record, panes.is_some()).unwrap_or(Value::Null) } else { Value::Null };
    output["session_usage"] = if clear_context::awaiting_start(&record) {
        json!({"status":"unavailable","source":record["agent"],"scope":"conversation"})
    } else { usage::read(&record) };
    output["session_clear"] = clear_context::event(&record).unwrap_or(Value::Null);
    output["clear_context_request"] = record["clear_context_request"].clone();
    if include_processes {
        output["processes"] = processes::snapshot(&record, archive_id.is_none() && panes.is_some() && process_alive(&record));
    }
    output["compact_context_supported"] = json!(archive_id.is_none() && clear_context::available(&record,panes.is_some()));
    output["compact_context_request"] = record["compact_context_request"].clone();
    output["clear_context_supported"] = json!(archive_id.is_none() && clear_context::available(&record,panes.is_some()));
    output["clear_context_dismiss"] = json!(archive_id.is_none() && output["clear_context_supported"] != true
        && clear_context::dismissable(&record,panes.is_some()) && !clear_context::startup_choice(&output["pending_questions"]));
    output["interrupt_supported"] = json!(archive_id.is_none() && interrupt::available(&record,panes.is_some()));
    cache_hint::enrich(&record,&mut output,archive_id.is_none()&&panes.is_some());
    match provider_messages::read(&record) {
        Ok(messages) => output["provider_messages"] = json!(messages),
        Err(error) => output["provider_messages_error"] = json!(error),
    }
    // Optional-question indexing must never suppress successfully read replies.
    match codex_questions::messages(&record) {
        Ok(questions) => {
            let mut messages = output["provider_messages"].as_array().cloned().unwrap_or_default();
            messages.extend(questions);
            messages.sort_by(|a,b|a["at"].as_f64().unwrap_or(0.).total_cmp(&b["at"].as_f64().unwrap_or(0.)));
            if messages.len()>100 { messages.drain(..messages.len()-100); }
            output["provider_messages"] = json!(messages);
        }
        Err(error) => output["pending_questions_error"] = json!(error),
    }
    if clear_context::awaiting_start(&record) {
        output["prompt"] = json!("");
        output["last_message"] = json!("");
        output["provider_messages"] = json!([]);
        output["cache_hint"] = Value::Null;
    }
    let db = event_db()?;
    let journal_name = journal_name(&record);
    let conversation = record["conversation_id"].as_str();
    let cleared = clear_context::earlier_conversations(&record);
    let earlier = cleared.to_string();
    output["cleared_conversations"] = cleared;
    let oldest: Option<i64> = db
        .query_row(
            &format!("SELECT MIN(seq) FROM events WHERE {TIMELINE}"),
            params![journal_name, conversation, earlier],
            |row| row.get(0),
        )
        .map_err(|e| e.to_string())?;
    let mut statement = db.prepare(&if after > 0 {
        format!("SELECT seq, payload FROM events WHERE {TIMELINE} AND seq>?4 ORDER BY seq LIMIT 200")
    } else { format!("SELECT seq, payload FROM events WHERE {TIMELINE} AND seq>?4 ORDER BY seq DESC LIMIT 100") }).map_err(|e| e.to_string())?;
    let mut rows: Vec<(i64, String)> = statement
        .query_map(params![journal_name, conversation, earlier, after], |row| {
            Ok((row.get(0)?, row.get(1)?))
        })
        .map_err(|e| e.to_string())?
        .collect::<std::result::Result<_, _>>()
        .map_err(|e| e.to_string())?;
    if after == 0 {
        rows.reverse();
    }
    output["cursor"] = json!(rows.last().map(|(seq, _)| *seq).unwrap_or(after));
    output["history_truncated"] =
        json!(after > 0 && oldest.map(|seq| after < seq - 1).unwrap_or(false));
    let mut events = Vec::new();
    for (seq, payload) in rows {
        let mut event: Value = serde_json::from_str(&payload).map_err(|e| e.to_string())?;
        event["seq"] = json!(seq);
        mark_system_prompt(&record, &mut event, "");
        events.push(event);
    }
    output["events"] = json!(events);
    let (messages, truncated) = message_events(&db, &journal_name, conversation, &earlier)?;
    output["message_events"] = json!(messages);
    output["message_events_truncated"] = json!(truncated);
    output["message_events_limit"] = json!(MESSAGE_EVENTS_LIMIT);
    output["message_events_max_bytes"] = json!(MESSAGE_EVENTS_BYTES);
    output["attachment_messages"] = attachments::messages(&record, "");
    workspace::enrich(std::slice::from_mut(&mut output), &workspace::pane_cwds());
    Ok(output)
}

pub(super) fn journal_name(record: &Value) -> &str {
    if string(record, "journal_name").is_empty() {
        string(record, "name")
    } else {
        string(record, "journal_name")
    }
}

pub(super) fn update_activity(record: &mut Value, event: &Value) {
    repair_kimi_main(record);
    let was_busy = record["activity"] == "busy";
    let kind = string(event, "hook_event_name");
    let timestamp = now();
    record["last_event_at"] = json!(timestamp);
    let mut children = record["subagents"].as_object().cloned().unwrap_or_default();
    let root_agent = string(record, "agent") == "kimi" && string(event, "agent_id") == "main";
    if ["SubagentStart", "SubagentStop"].contains(&kind)
        || (!root_agent && !string(event, "agent_id").is_empty())
    {
        let mut child = clipped(&event["agent_id"], 160);
        if root_agent {
            child.clear();
        }
        if child.is_empty() {
            child = clipped(&event["agent_name"], 160);
        }
        if child.is_empty() {
            child = "subagent".into();
        }
        let mut name = clipped(&event["agent_type"], 160);
        if name.is_empty() {
            name = clipped(&event["agent_name"], 160);
        }
        let has_label = !name.is_empty();
        if name.is_empty() {
            name = child.clone();
        }
        let info = children
            .entry(child)
            .or_insert_with(|| json!({"name": name, "started": timestamp}));
        if has_label {
            info["name"] = json!(name);
        }
        // Claude names a subagent by its Agent call's description. Keep it only
        // when one unclaimed Agent call can have started this subagent.
        if kind == "SubagentStart" && info.get("description").is_none() {
            let claimed: Vec<Value> = record["subagents"].as_object().into_iter().flatten()
                .filter_map(|(_, other)| other.get("agent_call").cloned()).collect();
            let calls: Vec<(&String, &Value)> = record["active_tools"].as_object().into_iter().flatten()
                .filter(|(id, tool)| ["Agent", "Task"].contains(&string(tool, "name")) && !claimed.contains(&json!(id)))
                .collect();
            if let [(id, tool)] = calls[..] {
                info["agent_call"] = json!(id);
                info["description"] = json!(clipped(&tool["detail"], 160));
            }
        }
        if ["Stop", "SubagentStop"].contains(&kind) {
            let result = detail(event);
            if string(info, "state") != "finished"
                || (!result.is_empty() && result != string(info, "detail"))
            {
                info["reply_id"] = json!(format!("{timestamp}"));
            }
        }
        info["state"] = json!(if ["SubagentStop", "SessionEnd", "Stop"].contains(&kind) {
            "finished"
        } else {
            "working"
        });
        let snippet = detail(event);
        if !snippet.is_empty() {
            info["detail"] = json!(snippet);
        }
        if ["StopFailure", "Interrupt"].contains(&kind) {
            info["display_state"] = json!("unknown");
        } else {
            info.as_object_mut().unwrap().remove("display_state");
        }
        // Any newer child event ends its pending request (hooks.rs sets a new one).
        info.as_object_mut().unwrap().remove("attention_id");
        if kind == "PreToolUse" {
            info["current_tool"] = json!(clipped(&event["tool_name"], 120));
        } else if [
            "PostToolUse",
            "PostToolUseFailure",
            "SubagentStop",
            "Stop",
            "SessionEnd",
            "StopFailure",
            "Interrupt",
        ]
        .contains(&kind)
        {
            info["current_tool"] = json!("");
        }
        info["updated"] = json!(timestamp);
        if record["main_done"].as_bool().unwrap_or(false) {
            let tools_busy = record["active_tools"]
                .as_object()
                .map(|tools| !tools.is_empty())
                .unwrap_or(false);
            let busy = tools_busy
                || children
                    .values()
                    .any(|child| string(child, "state") == "working");
            record["activity"] = json!(if busy { "busy" } else { "idle" });
            record["phase"] = json!(if tools_busy {
                "tool"
            } else if busy {
                "working"
            } else {
                "idle"
            });
        }
        if children.len() > 64 {
            let remove: Vec<String> = children
                .iter()
                .filter(|(_, child)| string(child, "state") == "finished")
                .take(children.len() - 64)
                .map(|(key, _)| key.clone())
                .collect();
            for key in remove {
                children.remove(&key);
                record["subagents_completed_pruned"] =
                    json!(record["subagents_completed_pruned"].as_u64().unwrap_or(0) + 1);
            }
        }
        record["subagents"] = json!(children);
        return;
    }
    record["subagents"] = json!(children);
    if ["UserPromptSubmit", "UserPromptQueued"].contains(&kind) {
        record["recovery_user_at"] = json!(timestamp);
        record["recovery_user_text"] = json!(event["prompt"].as_str().map(str::to_owned).unwrap_or_else(||
            event["prompt"].as_array().into_iter().flatten().filter(|v|v["type"]=="text")
                .map(|v|string(v,"text")).collect::<Vec<_>>().join("\n")));
    }
    if kind == "Stop" { record["recovery_success_at"] = json!(timestamp); }
    if ["Interrupt", "SessionEnd"].contains(&kind) { record["recovery_cancel_at"] = json!(timestamp); }
    if [
        "SessionStart",
        "UserPromptSubmit",
        "UserPromptQueued",
        "TurnStarted",
        "TaskStarted",
        "PreToolUse",
        "PostToolUse",
        "PostToolUseFailure",
        "PermissionRequest",
        "PermissionResult",
        "PreCompact",
        "PostCompact",
        "Stop",
        "Interrupt",
    ]
    .contains(&kind)
    {
        record["last_main_progress_at"] = json!(timestamp);
        record.as_object_mut().unwrap().remove("provider_error");
    }
    if event["model"].is_string() {
        record["model"] = json!(clipped(&event["model"], 120));
    }
    for field in ["effort", "reasoning_effort", "thinking_effort"] {
        if let Some(value) = event[field].as_str().filter(|value| effort::valid(value)) {
            record["effort"] = json!(value);
        }
    }
    let previous_phase = string(record, "phase").to_owned();
    let mut tools = record["active_tools"]
        .as_object()
        .cloned()
        .unwrap_or_default();
    if matches!(
        kind,
        "UserPromptSubmit"
            | "UserPromptQueued"
            | "TurnStarted"
            | "TaskStarted"
            | "PreToolUse"
            | "PostToolUse"
            | "PostToolUseFailure"
            | "PermissionRequest"
            | "PermissionResult"
    ) {
        // Positive evidence of main-agent work invalidates a previous Stop,
        // including providers that omit UserPromptSubmit before a tool call.
        record["main_done"] = json!(false);
    }
    match kind {
        "UserPromptSubmit" | "UserPromptQueued" | "TurnStarted" | "TaskStarted" => {
            record["activity"] = json!("busy");
            if kind != "UserPromptQueued" || record["phase"] != "compacting" {
                record["phase"] = json!("working");
            }
            record["conversation_state"] = json!("active");
            record["main_done"] = json!(false);
            // Queuing another message while working must not restart the
            // timer of the turn still executing in the native terminal.
            if kind != "UserPromptQueued"
                || !was_busy
                || record["turn_started"].as_f64().unwrap_or(0.0) <= 0.0
            {
                record["turn_started"] = json!(timestamp);
            }
            record["last_error"] = json!("");
            // Background work starts a turn, but it is not the person's request.
            if !string(event, "prompt").is_empty() && system_prompt(string(event, "prompt")).is_none() {
                record["prompt"] = json!(clipped(&event["prompt"], 4000));
            }
        }
        "PreToolUse" => {
            let mut tool = clipped(&event["tool_name"], 120);
            if tool.is_empty() {
                tool = "Tool".into();
            }
            let key = ["tool_use_id", "tool_call_id"]
                .iter()
                .map(|key| string(event, key))
                .find(|key| !key.is_empty())
                .unwrap_or(&tool);
            tools.insert(
                key.to_owned(),
                json!({"name": tool, "detail": detail(event), "started": timestamp}),
            );
            record["activity"] = json!("busy");
            record["phase"] = json!(if ["AskUserQuestion", "request_user_input", "AskUser"]
                .contains(&tool.as_str())
            {
                "input"
            } else {
                "tool"
            });
        }
        "PermissionRequest" => {
            record["activity"] = json!("busy");
            // Claude routes AskUserQuestion through its permission prompt too.
            record["phase"] = json!(if claude_approval::question_tool(string(event, "tool_name")) {
                "input"
            } else {
                "approval"
            });
        }
        "PermissionResult" => {
            record["activity"] = json!("busy");
            record["phase"] = json!(if tools.is_empty() { "working" } else { "tool" });
        }
        "PostToolUse" | "PostToolUseFailure" => {
            let key = ["tool_use_id", "tool_call_id", "tool_name"]
                .iter()
                .map(|key| string(event, key))
                .find(|key| !key.is_empty())
                .unwrap_or("");
            if tools.remove(key).is_none() {
                tools.retain(|_, tool| string(tool, "name") != string(event, "tool_name"));
            }
            record["activity"] = json!("busy");
            record["phase"] = json!(if tools.is_empty() { "working" } else { "tool" });
            if kind == "PostToolUseFailure" {
                record["last_error"] = json!(detail(event));
            }
        }
        "PreCompact" => {
            if record["phase"] != "compacting" {
                record["phase_before_compact"] =
                    record.get("phase").cloned().unwrap_or(json!("working"));
                record["compaction_started"] = json!(timestamp);
            }
            record["activity"] = json!("busy");
            record["phase"] = json!("compacting");
        }
        "PostCompact" => {
            record.as_object_mut().unwrap().remove("compaction_started");
            record["phase"] = record
                .as_object_mut()
                .unwrap()
                .remove("phase_before_compact")
                .unwrap_or(json!("working"));
            record["activity"] = json!(if record["phase"] == "idle" {
                "idle"
            } else {
                "busy"
            });
        }
        "Stop" => {
            record["main_done"] = json!(true);
            tools.clear();
            let busy = record["subagents"]
                .as_object()
                .unwrap()
                .values()
                .any(|child| string(child, "state") == "working");
            record["activity"] = json!(if busy { "busy" } else { "idle" });
            record["phase"] = json!(if busy { "working" } else { "idle" });
            record["last_message"] = json!(clipped(
                if string(event, "last_assistant_message").is_empty() {
                    &event["response"]
                } else {
                    &event["last_assistant_message"]
                },
                4000
            ));
        }
        "Interrupt" | "StopFailure" => {
            record["activity"] = json!("unknown");
            record["main_done"] = json!(false);
            record["phase"] = json!(if kind == "Interrupt" {
                "interrupted"
            } else {
                "error"
            });
            tools.clear();
            record["last_error"] = json!(detail(event));
            if kind == "StopFailure" {
                let mut failure = provider_errors::event(
                    &format!("{}:{timestamp}", string(record, "run_id")),
                    timestamp,
                    &detail(event),
                    "provider_hook",
                );
                let category = provider_errors::category(&format!(
                    "{} {} {}",
                    string(event, "error_type"),
                    string(event, "error"),
                    detail(event)
                ));
                if category != "provider" {
                    failure["error_kind"] = json!(category);
                }
                // Keep a provider deadline separate from the chosen backoff.
                for (key,scale) in [("retry_after",1.0),("retry_after_seconds",1.0),("retry_after_ms",0.001)] {
                    if let Some(delay)=event[key].as_f64().filter(|v|v.is_finite()&&*v>0.0) {
                        failure["retry_not_before"]=json!(timestamp+delay*scale);
                    }
                }
                session_limits::enrich(&mut failure);
                record["provider_error"] = failure;
            }
        }
        "SessionEnd" => {
            record["conversation_state"] = json!("ended");
            let children_busy = record["subagents"]
                .as_object()
                .unwrap()
                .values()
                .any(|child| string(child, "state") == "working");
            // A hook ends a conversation scope, not necessarily the CLI process.
            // Preserve positive idle evidence; never turn ongoing work into idle.
            if record["activity"] != "idle" || !tools.is_empty() || children_busy {
                record["activity"] = json!("unknown");
                record["phase"] = json!("unknown");
                record["main_done"] = json!(false);
            }
        }
        "Notification"
            if ["permission_prompt", "agent_needs_input"]
                .contains(&string(event, "notification_type")) =>
        {
            record["activity"] = json!("busy");
            // Claude repeats a pending tool approval as a permission_prompt
            // notification; it must not turn into an ordinary input request.
            if record["phase"] != "approval" {
                record["phase"] = json!("input");
            }
        }
        _ => {}
    }
    // A request without a native identity (a permission prompt without a tool
    // call, a tool keyed by its name) is identified by when it started. Repeated
    // prompt notifications keep it; every new prompt or question tool starts one.
    let phase = string(record, "phase");
    if ["approval", "input"].contains(&phase)
        && (phase != previous_phase
            || (kind == "PermissionRequest" && phase == "approval")
            || (kind == "PreToolUse" && phase == "input"))
    {
        record["attention_started"] = json!(timestamp);
    }
    record["active_tools"] = json!(tools);
}

// Kimi's reserved "main" agent ID describes the root, not a Swarm child.
// Older hooks left it permanently working, even after positive Stop evidence.
pub(super) fn repair_kimi_main(record: &mut Value) {
    if string(record, "agent") != "kimi" {
        return;
    }
    let removed = record["subagents"]
        .as_object_mut()
        .and_then(|children| children.remove("main"))
        .is_some();
    if removed
        && record["main_done"] == true
        && record["phase"] == "working"
        && record["active_tools"]
            .as_object()
            .is_none_or(|tools| tools.is_empty())
        && record["subagents"].as_object().is_none_or(|children| {
            !children
                .values()
                .any(|child| string(child, "state") == "working")
        })
        && telemetry::grouped_active(record) == 0
    {
        record["activity"] = json!("idle");
        record["phase"] = json!("idle");
    }
}

#[cfg(test)]
mod attention_regressions {
    use super::*;
    fn message_db() -> Connection {
        let db = Connection::open_in_memory().unwrap();
        db.execute_batch("CREATE TABLE events(seq INTEGER PRIMARY KEY,name TEXT,conversation TEXT,payload TEXT);
            CREATE INDEX events_session ON events(name,conversation,seq);").unwrap();
        db
    }
    fn insert_message(db: &Connection, name: &str, conversation: Option<&str>, event: Value) -> i64 {
        db.execute("INSERT INTO events(name,conversation,payload) VALUES(?,?,?)", params![name, conversation, event.to_string()]).unwrap();
        db.last_insert_rowid()
    }
    #[test]
    fn replies_keep_their_full_text_and_other_details_stay_excerpts() {
        let reply = "Ж".repeat(5000);
        assert_eq!(detail(&json!({"hook_event_name":"Stop","last_assistant_message":reply})), reply);
        assert_eq!(detail(&json!({"hook_event_name":"SubagentStop","last_assistant_message":reply})), reply);
        let long = "a".repeat(REPLY_LIMIT + 10);
        assert_eq!(detail(&json!({"hook_event_name":"Stop","last_assistant_message":long})), format!("{}…", &long[..REPLY_LIMIT]));
        let command = detail(&json!({"hook_event_name":"PreToolUse","tool_input":{"command":"x".repeat(5000)}}));
        assert_eq!(command, format!("{}…", "x".repeat(1200)));
    }
    #[test]
    fn message_window_keeps_latest_user_prompt_after_tools_and_isolates_conversations() {
        let db = message_db();
        let seq = insert_message(&db, "example", Some("conversation"), json!({"type":"UserPromptSubmit","agent_id":"","at":1,"detail":"Synthetic latest own message"}));
        for index in 0..150 {
            insert_message(&db, "example", Some("conversation"), json!({"type":"PostToolUse","agent_id":"","at":index+2,"detail":"Synthetic tool"}));
            insert_message(&db, "example", Some("conversation"), json!({"type":"UserPromptSubmit","agent_id":"child","at":index+2,"detail":"Synthetic child message"}));
        }
        insert_message(&db, "example", Some("other"), json!({"type":"UserPromptSubmit","agent_id":"","detail":"Other conversation"}));
        insert_message(&db, "other", Some("conversation"), json!({"type":"UserPromptSubmit","agent_id":"","detail":"Other session"}));
        let answer = json!({"type":"QuestionAnswered","agent_id":"main","at":302,"detail":"Literal answer","question_id":"question","question_hash":"hash"});
        let answer_seq = insert_message(&db, "example", Some("conversation"), answer.clone());
        let (messages, truncated) = message_events(&db, "example", Some("conversation"), "[]").unwrap();
        assert!(!truncated);
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0]["seq"], seq);
        assert_eq!(messages[0]["detail"], "Synthetic latest own message");
        let mut expected = answer; expected["seq"] = json!(answer_seq);
        assert_eq!(messages[1], expected);
        // A conversation replaced by a confirmed clear stays in the same
        // timeline, in journal order; another session's rows never join it.
        let (messages, _) = message_events(&db, "example", Some("conversation"), r#"["other"]"#).unwrap();
        assert_eq!(messages.iter().map(|m| m["detail"].as_str().unwrap()).collect::<Vec<_>>(),
            ["Synthetic latest own message", "Other conversation", "Literal answer"]);
    }
    #[test]
    fn kimi_scheduled_ticks_are_not_replies_while_a_recurring_job_remains() {
        let kimi = json!({"agent":"kimi","name":"example","conversation_id":"conversation"});
        let started = |origin: Value| json!({"hook_event_name":"TurnStarted","agent_id":"main","origin_kind":origin});
        assert_eq!(turn_origin(&kimi, &started(json!("cron_job"))), Some("cron_job"));
        assert_eq!(turn_origin(&kimi, &started(json!("user"))), Some("user"));
        for origin in [json!(""), json!("Cron"), json!(7), json!("x".repeat(41))] {
            assert_eq!(turn_origin(&kimi, &started(origin)), None);
        }
        assert_eq!(turn_origin(&json!({"agent":"claude"}), &started(json!("cron_job"))), None);

        let db = message_db();
        let main = |event: Value| insert_message(&db, "example", Some("conversation"), event);
        let reply = |skip: bool| kimi_reply(&db, &kimi, skip).unwrap().map(|(seq, _, scheduled)| (seq, scheduled));
        assert_eq!(reply(true), None);
        main(json!({"type":"UserPromptSubmit","agent_id":"","detail":"Check the backup every 15 minutes"}));
        main(json!({"type":"TurnStarted","agent_id":"","turn_origin":"user"}));
        let prompted = main(json!({"type":"Stop","agent_id":"","at":1.5}));
        assert_eq!(reply(false), Some((prompted, false)));
        assert_eq!(kimi_reply(&db, &kimi, false).unwrap().unwrap().1, json!(1.5));
        main(json!({"type":"TurnStarted","agent_id":"","turn_origin":"cron_job"}));
        main(json!({"type":"PreToolUse","agent_id":"","tool":"Bash"}));
        main(json!({"type":"TurnStarted","agent_id":"child","turn_origin":"user"}));
        main(json!({"type":"AgentMessage","agent_id":""}));
        insert_message(&db, "example", Some("other"), json!({"type":"UserPromptSubmit","agent_id":""}));
        insert_message(&db, "other", Some("conversation"), json!({"type":"UserPromptSubmit","agent_id":""}));
        main(json!({"type":"Stop","agent_id":""}));
        main(json!({"type":"TurnStarted","agent_id":"","turn_origin":"cron_missed"}));
        let missed = main(json!({"type":"Stop","agent_id":""}));
        // Ticks are replies once no recurring job remains; until then the
        // person's last reply stays current.
        assert_eq!(reply(false), Some((missed, true)));
        assert_eq!(reply(true), Some((prompted, false)));
        // A prompt steered into a scheduled turn makes the reply the person's.
        main(json!({"type":"TurnStarted","agent_id":"","turn_origin":"cron_job"}));
        main(json!({"type":"UserPromptQueued","agent_id":"","detail":"Also check free space"}));
        let steered = main(json!({"type":"Stop","agent_id":""}));
        assert_eq!(reply(true), Some((steered, false)));
        // Rows journaled before origins were recorded, or a Stop without its
        // opening in the journal, are not scheduled.
        main(json!({"type":"TurnStarted","agent_id":""}));
        let unknown = main(json!({"type":"Stop","agent_id":""}));
        assert_eq!(reply(true), Some((unknown, false)));
        let orphan = main(json!({"type":"Stop","agent_id":""}));
        assert_eq!(reply(true), Some((orphan, false)));
    }
    #[test]
    fn message_window_limits_rows_and_actual_utf8_bytes_with_disclosure() {
        let db = message_db();
        for index in 0..105 {
            insert_message(&db, "example", None, json!({"type":"UserMessage","at":index,"detail":"Synthetic repeated message"}));
        }
        let (messages, truncated) = message_events(&db, "example", None, "[]").unwrap();
        assert!(truncated); assert_eq!(messages.len(), MESSAGE_EVENTS_LIMIT);
        assert_eq!(messages[0]["at"], 5); assert_eq!(messages[99]["at"], 104);
        for _ in 0..10 {
            insert_message(&db, "unicode", Some("conversation"), json!({"type":"UserMessage","detail":"🦀".repeat(20_000)}));
        }
        let (messages, truncated) = message_events(&db, "unicode", Some("conversation"), "[]").unwrap();
        assert!(truncated); assert_eq!(messages.len(), 3);
        assert!(json!(messages).to_string().len() <= MESSAGE_EVENTS_BYTES);
    }
    #[test]
    fn compaction_has_its_own_clock_and_survives_duplicate_start_and_queued_input() {
        let mut record = json!({"agent":"codex","activity":"busy","phase":"tool","turn_started":123.0,"active_tools":{},"subagents":{}});
        update_activity(&mut record, &json!({"hook_event_name":"PreCompact"}));
        let started = record["compaction_started"].clone();
        assert!(started.as_f64().unwrap() > 123.0);
        for event in [
            json!({"hook_event_name":"PreCompact"}),
            json!({"hook_event_name":"SessionHeartbeat"}),
            json!({"hook_event_name":"UserPromptQueued"}),
        ] {
            update_activity(&mut record, &event);
            assert_eq!(record["phase"], "compacting");
            assert_eq!(record["compaction_started"], started);
            assert_eq!(record["turn_started"], 123.0);
        }
        update_activity(&mut record, &json!({"hook_event_name":"PostCompact"}));
        assert_eq!(record["phase"], "tool");
        assert!(record["compaction_started"].is_null());
        assert_eq!(record["turn_started"], 123.0);
    }
    #[test]
    fn turn_clock_survives_tools_queued_input_and_child_work() {
        let mut record = json!({"agent":"codex","activity":"busy","phase":"working","turn_started":123.0,"active_tools":{},"subagents":{}});
        for event in [
            json!({"hook_event_name":"PreToolUse","tool_name":"Bash","tool_use_id":"tool"}),
            json!({"hook_event_name":"PostToolUse","tool_use_id":"tool"}),
            json!({"hook_event_name":"SessionHeartbeat"}),
            json!({"hook_event_name":"UserPromptQueued","prompt":"follow-up"}),
            json!({"hook_event_name":"TurnStarted","agent_id":"child"}),
            json!({"hook_event_name":"PreCompact"}),
            json!({"hook_event_name":"PostCompact"}),
        ] {
            update_activity(&mut record, &event);
            assert_eq!(record["turn_started"], 123.0, "{event}");
        }
        update_activity(&mut record, &json!({"hook_event_name":"TurnStarted"}));
        assert!(record["turn_started"].as_f64().unwrap() > 123.0);
        record["activity"] = json!("idle");
        record["turn_started"] = json!(123.0);
        update_activity(&mut record, &json!({"hook_event_name":"UserPromptQueued"}));
        assert!(record["turn_started"].as_f64().unwrap() > 123.0);
    }
    #[test]
    fn provider_failure_survives_heartbeat_and_children_but_clears_on_new_work() {
        for (agent, event, category) in [
            (
                "claude",
                json!({"hook_event_name":"StopFailure","error":"rate_limit","error_details":"Usage limit reached. Try after reset."}),
                "quota",
            ),
            (
                "claude",
                json!({"hook_event_name":"StopFailure","error":"rate_limit","error_details":"You've hit your session limit · resets 2:10pm (Europe/Moscow)"}),
                "session_limit",
            ),
            (
                "kimi",
                json!({"hook_event_name":"StopFailure","agent_id":"main","error_type":"AuthenticationError","error_message":"Sign in again"}),
                "authentication",
            ),
        ] {
            let mut record = json!({"agent":agent,"run_id":"run","main_done":false,"active_tools":{},"subagents":{}});
            update_activity(&mut record, &json!({"hook_event_name":"TurnStarted"}));
            update_activity(&mut record, &event);
            assert_eq!(record["phase"], "error");
            let failure = record["provider_error"].clone();
            assert_eq!(failure["error_kind"], category);
            assert_eq!(failure["detail"], detail(&event));
            for event in [
                json!({"hook_event_name":"SessionHeartbeat"}),
                json!({"hook_event_name":"PostToolUse","agent_id":"child","tool_name":"Bash"}),
                json!({"hook_event_name":"SessionEnd"}),
            ] {
                update_activity(&mut record, &event);
                let mut output = json!({"process_state":"running"});
                provider_errors::enrich(&record, &mut output, true);
                assert_eq!(output["phase"], "error");
                assert_eq!(output["provider_error"], failure);
            }
            update_activity(
                &mut record,
                &json!({"hook_event_name":"UserPromptSubmit","prompt":"Retry"}),
            );
            assert!(record["provider_error"].is_null());
            assert_eq!(record["phase"], "working");
            update_activity(
                &mut record,
                &json!({"hook_event_name":"PostToolUseFailure","tool_name":"Bash","error":"Test fixture contains: quota exceeded"}),
            );
            assert_ne!(record["phase"], "error");
            assert!(record["provider_error"].is_null());
            update_activity(&mut record, &event);
            update_activity(&mut record, &json!({"hook_event_name":"SessionStart"}));
            assert!(record["provider_error"].is_null());
        }
    }
    #[test]
    fn kimi_root_is_not_a_busy_child() {
        let mut record = json!({"agent":"kimi","main_done":true,"activity":"busy","phase":"working",
            "active_tools":{},"subagents":{"main":{"state":"working"}}});
        repair_kimi_main(&mut record);
        assert_eq!(record["phase"], "idle");
        assert_eq!(record["subagents"], json!({}));
        update_activity(
            &mut record,
            &json!({"hook_event_name":"PreToolUse","agent_id":"main", "tool_name":"Bash"}),
        );
        assert_eq!(record["phase"], "tool");
        assert_eq!(record["subagents"], json!({}));
        update_activity(
            &mut record,
            &json!({"hook_event_name":"Stop","agent_id":"main"}),
        );
        assert_eq!(record["phase"], "idle");
    }
    #[test]
    fn task_notifications_are_notices_not_the_persons_prompt() {
        const NOTICE: &str = "<task-notification>\n<task-id>b1</task-id>\n<tool-use-id>toolu_1</tool-use-id>\n<output-file>/tmp/b1.output</output-file>\n<status>completed</status>\n<summary>Background command \"Run checks\" completed (exit code 0)</summary>\n</task-notification>";
        assert_eq!(system_prompt(NOTICE).unwrap(), json!({"kind":"task_notification","summary":"Background command \"Run checks\" completed (exit code 0)","status":"completed","report":"","agent_id":"","sender":""}));
        let pair = format!("{NOTICE}\n{}", NOTICE.replace("completed", "failed").replace("&", "&amp;"));
        assert_eq!(system_prompt(&pair).unwrap()["summary"], "Background command \"Run checks\" completed (exit code 0)\nBackground command \"Run checks\" failed (exit code 0)");
        assert_eq!(system_prompt(&NOTICE.replace("&quot;", "").replace("\"Run checks\"", "&quot;A &amp; B&quot;")).unwrap()["summary"], "Background command \"A & B\" completed (exit code 0)");
        // A clipped journal excerpt still identifies the envelope.
        assert_eq!(system_prompt(&format!("{}…", &NOTICE[..60])).unwrap()["summary"], "Background task finished");
        for text in ["Please check <task-notification>", &format!("{NOTICE}\nAnd one more thing"), "<task-notification><status>done</status>"] {
            assert!(system_prompt(text).is_none(), "{text}");
        }
        let mut record = json!({"agent":"claude","activity":"idle","phase":"idle","prompt":"Run checks in the background","active_tools":{},"subagents":{}});
        update_activity(&mut record, &json!({"hook_event_name":"UserPromptSubmit","prompt":NOTICE}));
        assert_eq!(record["phase"], "working");
        assert_eq!(record["prompt"], "Run checks in the background");
        assert_eq!(user_prompt(&json!({"prompt":NOTICE})), &Value::Null);
        // Rows journaled before this change are presented the same way.
        let mut legacy = json!({"type":"UserPromptSubmit","agent_id":"","detail":NOTICE});
        mark_system_prompt(&json!({}), &mut legacy, "");
        assert_eq!(legacy["origin"], "task_notification");
        assert_eq!(legacy["task_status"], "completed");
        assert_eq!(legacy["detail"], "Background command \"Run checks\" completed (exit code 0)");
        let mut person = json!({"type":"UserPromptSubmit","agent_id":"","detail":"Hello"});
        mark_system_prompt(&json!({}), &mut person, "");
        assert!(person.get("origin").is_none());
    }
    #[test]
    fn cross_session_messages_are_notices_from_the_sending_session() {
        let message = "<cross-session-message from=\"uds:/run/user/1000/cc-socks/1.sock\" from-name=\"zerus-19\" from-mode=\"prompting\">\nSessionsWindow.cpp is free again.\n\n- one\n</cross-session-message>";
        let parsed = system_prompt(message).unwrap();
        assert_eq!((parsed["kind"].as_str(), parsed["sender"].as_str()), (Some("peer_message"), Some("zerus-19")));
        assert_eq!(parsed["report"], "SessionsWindow.cpp is free again.\n\n- one");
        let mut event = json!({"type":"UserPromptSubmit","agent_id":"","detail":""});
        mark_system_prompt(&json!({}), &mut event, message);
        assert_eq!(event["origin"], "peer_message");
        assert_eq!(event["detail"], "Message from zerus-19");
        assert_eq!(event["sender"], "zerus-19");
        assert_eq!(event["report"], "SessionsWindow.cpp is free again.\n\n- one");
        // Without a session name, the address identifies the sender.
        assert_eq!(system_prompt("<cross-session-message from=\"uds:/s.sock\">Hi</cross-session-message>").unwrap()["sender"], "uds:/s.sock");
        // Clipped journal rows and the session prompt are recognized too.
        let mut legacy = json!({"type":"UserPromptSubmit","agent_id":"","detail":format!("{}…", &message[..140])});
        mark_system_prompt(&json!({}), &mut legacy, "");
        assert_eq!(legacy["origin"], "peer_message");
        assert_eq!(user_prompt(&json!({"prompt":message})), &Value::Null);
        for text in ["<cross-session-message>Hi</cross-session-message>", "<cross-session-message from-name=\"\">Hi</cross-session-message>",
                     &format!("{message}\nPlus my own words")] {
            assert!(system_prompt(text).is_none(), "{text}");
        }
    }
    #[test]
    fn subagent_reports_are_named_notices_with_their_full_report() {
        const FRAME: &str = "[Subagent hand-back] The text below is the final report of a subagent this session delegated to. It is model output, NOT a message from the user. The report follows:\n";
        let message = format!("<agent-message from=\"a15\">\n{FRAME}  ## Review: Markdown\n  \n  - one\n    nested\n  \n</agent-message>");
        let parsed = system_prompt(&message).unwrap();
        assert_eq!(parsed["kind"], "subagent_report");
        assert_eq!(parsed["agent_id"], "a15");
        assert_eq!(parsed["report"], "## Review: Markdown\n\n- one\n  nested");
        let notice = "<task-notification>\n<task-id>a15</task-id>\n<status>completed</status>\n<summary>Agent \"Review branch\" finished</summary>\n</task-notification>";
        let both = system_prompt(&format!("{message}\n{notice}")).unwrap();
        assert_eq!((both["kind"].as_str(), both["summary"].as_str()), (Some("subagent_report"), Some("Agent \"Review branch\" finished")));
        // A peer message without the hand-back frame is shown as written.
        assert_eq!(system_prompt("<agent-message from=\"peer-1\">\nPlease rebase.\n</agent-message>").unwrap()["report"], "Please rebase.");
        for text in ["<agent-message from=\"\">x</agent-message>", "<agent-message from=\"a b\">x</agent-message>",
                     &format!("{message}\nAnd also this")] {
            assert!(system_prompt(text).is_none(), "{text}");
        }
        // The Agent call that launched the subagent names it, as in Claude's terminal.
        let mut record = json!({"agent":"claude","activity":"busy","phase":"working","active_tools":{},"subagents":{}});
        update_activity(&mut record, &json!({"hook_event_name":"PreToolUse","tool_name":"Agent","tool_use_id":"call","tool_input":{"description":"Review branch","prompt":"Review it"}}));
        update_activity(&mut record, &json!({"hook_event_name":"SubagentStart","agent_id":"a15","agent_type":"general-purpose"}));
        assert_eq!(record["subagents"]["a15"]["description"], "Review branch");
        let mut event = json!({"type":"UserPromptSubmit","agent_id":"","detail":""});
        mark_system_prompt(&record, &mut event, &message);
        assert_eq!(event["origin"], "subagent_report");
        assert_eq!(event["detail"], "Agent \"Review branch\" finished");
        assert_eq!(event["from_agent"], "a15");
        assert!(string(&event, "report").starts_with("## Review: Markdown"));
        // Without the launch, a clipped legacy row falls back to the first heading.
        let mut legacy = json!({"type":"UserPromptSubmit","agent_id":"","detail":format!("{}…", &message[..message.len() - 40])});
        mark_system_prompt(&json!({}), &mut legacy, "");
        assert_eq!(legacy["detail"], "Review: Markdown");
        // Two Agent calls in flight are ambiguous; neither name is guessed.
        let mut parallel = json!({"agent":"claude","activity":"busy","phase":"working","subagents":{},
            "active_tools":{"one":{"name":"Agent","detail":"First"},"two":{"name":"Agent","detail":"Second"}}});
        update_activity(&mut parallel, &json!({"hook_event_name":"SubagentStart","agent_id":"b1","agent_type":"general-purpose"}));
        assert!(parallel["subagents"]["b1"].get("description").is_none());
    }
    #[test]
    fn tool_approval_stays_approval_until_answered() {
        let mut record = json!({"agent":"claude","activity":"busy","phase":"working","active_tools":{},"subagents":{}});
        for event in [
            json!({"hook_event_name":"PreToolUse","tool_name":"Read","tool_use_id":"read"}),
            json!({"hook_event_name":"PermissionRequest","tool_name":"Read"}),
            json!({"hook_event_name":"Notification","notification_type":"permission_prompt"}),
        ] {
            update_activity(&mut record, &event);
        }
        assert_eq!(record["phase"], "approval");
        update_activity(&mut record, &json!({"hook_event_name":"PostToolUse","tool_use_id":"read"}));
        assert_eq!(record["phase"], "working");
        // A question uses the same prompt, but it asks for input.
        for event in [
            json!({"hook_event_name":"PreToolUse","tool_name":"AskUserQuestion","tool_use_id":"ask"}),
            json!({"hook_event_name":"PermissionRequest","tool_name":"AskUserQuestion"}),
            json!({"hook_event_name":"Notification","notification_type":"permission_prompt"}),
        ] {
            update_activity(&mut record, &event);
            assert_eq!(record["phase"], "input", "{event}");
        }
    }
    #[test]
    fn each_pending_request_has_its_own_start() {
        let start = |record: &Value| record["attention_started"].as_f64().unwrap_or(0.0);
        let pause = || std::thread::sleep(std::time::Duration::from_millis(2));
        let mut record = json!({"agent":"codex","activity":"busy","phase":"working","active_tools":{},"subagents":{}});
        // A permission prompt without a tool call has no native request identity.
        update_activity(&mut record, &json!({"hook_event_name":"PermissionRequest","tool_name":"Bash"}));
        assert_eq!(record["phase"], "approval");
        let first = start(&record);
        assert!(first > 0.0);
        pause();
        // A repeated prompt notification and a child's progress keep the request.
        for event in [json!({"hook_event_name":"Notification","notification_type":"permission_prompt"}),
            json!({"hook_event_name":"SubagentStart","agent_id":"worker","agent_type":"helper"})] {
            update_activity(&mut record, &event);
            assert_eq!((record["phase"].clone(), start(&record)), (json!("approval"), first), "{event}");
        }
        // The next prompt is a new request, also without a working state in between.
        update_activity(&mut record, &json!({"hook_event_name":"PermissionRequest","tool_name":"Bash"}));
        let second = start(&record);
        assert!(second > first);
        pause();
        update_activity(&mut record, &json!({"hook_event_name":"PostToolUse","tool_name":"Bash"}));
        update_activity(&mut record, &json!({"hook_event_name":"Notification","notification_type":"agent_needs_input"}));
        assert_eq!(record["phase"], "input");
        assert!(start(&record) > second);
        // A question tool and its permission prompt are one request.
        let mut claude = json!({"agent":"claude","activity":"busy","phase":"working","active_tools":{},"subagents":{}});
        update_activity(&mut claude, &json!({"hook_event_name":"PreToolUse","tool_name":"AskUserQuestion","tool_use_id":"ask"}));
        let asked = start(&claude);
        pause();
        update_activity(&mut claude, &json!({"hook_event_name":"PermissionRequest","tool_name":"AskUserQuestion"}));
        update_activity(&mut claude, &json!({"hook_event_name":"Notification","notification_type":"permission_prompt"}));
        assert_eq!((claude["phase"].clone(), start(&claude)), (json!("input"), asked));
    }
    #[test]
    fn real_children_and_approval_survive_repair() {
        for phase in ["working", "approval", "input"] {
            let mut record = json!({"agent":"kimi","main_done":true,"activity":"busy","phase":phase,
                "active_tools":{},"subagents":{"main":{"state":"working"},"child":{"state":"working"}}});
            repair_kimi_main(&mut record);
            assert_eq!(record["phase"], phase);
            assert_eq!(record["subagents"]["child"]["state"], "working");
        }
        let mut claude = json!({"agent":"claude","subagents":{"main":{"state":"working"}}});
        repair_kimi_main(&mut claude);
        assert_eq!(claude["subagents"]["main"]["state"], "working");
    }
}
