//! Read-only search over bindings' public messages and recorded activity. Provider
//! histories stay provider-owned: this reads only exact bound conversation files,
//! never scans arbitrary projects, executes agent code, or indexes reasoning.
use super::*;
use regex::{Regex, RegexBuilder};
use rusqlite::{params, Connection, OpenFlags};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::File;
use std::io::{BufRead, BufReader, Seek, SeekFrom};

const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;
const MAX_TOTAL_BYTES: u64 = 256 * 1024 * 1024;
const MAX_LINE_BYTES: usize = 2 * 1024 * 1024;
const MAX_CONTENT_CHARS: usize = 32_000;
const MAX_SESSIONS: usize = 500;
const MAX_JOURNAL_ROWS: usize = 50_000;

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let mut query = None;
    let mut limit = 80;
    let mut index = 0;
    while index < args.len() {
        let value = args
            .get(index + 1)
            .ok_or("usage: hgs search --query <text> [--limit 1..200]")?;
        match args[index].as_str() {
            "--query" if query.is_none() => query = Some(value.as_str()),
            "--limit" => limit = value.parse::<usize>().map_err(|_| "invalid search limit")?,
            _ => return Err("usage: hgs search --query <text> [--limit 1..200]".into()),
        }
        index += 2;
    }
    let query = query.ok_or("search query is required")?.trim();
    if query.is_empty() || query.chars().count() > 256 || query.chars().any(char::is_control) {
        return Err("search query must contain 1..256 printable characters".into());
    }
    if !(1..=200).contains(&limit) {
        return Err("search limit must be between 1 and 200".into());
    }
    let mut bindings = records()?;
    let archives = archive::records()?;
    bindings.retain(|record| !archive::equivalent(record, &archives));
    bindings.extend(archives);
    for mut record in dsh::binding_records() {
        record["agent"] = json!("dsh");
        record["updated"] = record["created"].clone();
        bindings.push(record);
    }
    // Prefer recent sessions when the explicit scan budget is exhausted.
    bindings.sort_by(|a, b| number(b, "updated").total_cmp(&number(a, "updated")));
    println!("{}", search(&root(), bindings, query, limit)?);
    Ok(0)
}

fn number(value: &Value, key: &str) -> f64 {
    value[key]
        .as_f64()
        .filter(|value| value.is_finite())
        .unwrap_or(0.0)
}

struct Search {
    pattern: Regex,
    results: Vec<Value>,
    limit: usize,
    bytes_left: u64,
    rows_left: usize,
    truncated: bool,
    unavailable: BTreeSet<String>,
}

impl Search {
    fn unavailable(&mut self, record: &Value) {
        self.unavailable.insert(format!(
            "{}:{}:{}",
            string(record, "name"),
            string(record, "run_id"),
            string(record, "archive_id")
        ));
    }

    fn accept(&mut self, record: &Value, mut event: Value, role: &str) {
        let original = string(&event, "detail");
        let Some(found) = self.pattern.find(original) else {
            return;
        };
        let snippet = excerpt(original, found.start(), found.end(), 220)
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ");
        let content = excerpt(original, found.start(), found.end(), MAX_CONTENT_CHARS);
        let content_truncated = content != original;
        let at = number(&event, "at");
        // Public transcripts and hooks often record the same message. Keep the
        // full provider text while retaining genuinely repeated later turns.
        if self.results.iter().any(|existing| {
            existing["name"] == record["name"]
                && existing["conversation_id"] == record["conversation_id"]
                && existing["archive_id"] == record.get("archive_id").cloned().unwrap_or(json!(""))
                && string(existing, "role") == role
                && (number(existing, "at") - at).abs() <= 3.0
                && (string(existing, "content") == content
                    || equivalent_excerpt(string(existing, "content"), &content))
        }) {
            return;
        }
        event["detail"] = json!(content);
        let id = if string(&event, "message_id").is_empty() {
            format!(
                "search-{:x}",
                Sha256::digest(format!(
                    "{}:{}:{}:{}",
                    string(record, "run_id"),
                    string(record, "conversation_id"),
                    string(&event, "source"),
                    event
                ))
            )
        } else {
            string(&event, "message_id").to_owned()
        };
        event["message_id"] = json!(id);
        let result = json!({"name":record["name"],"run_id":record["run_id"],
            "conversation_id":record["conversation_id"],
            "archive_id":record.get("archive_id").cloned().unwrap_or(json!("")),
            "agent":record["agent"],"cwd":record["cwd"],"at":at,"role":role,
            "session": if string(record,"archive_id").is_empty(){Value::Null}else{json!({
                "name":record["name"],"cmd":record["agent"],"project":string(record,"name").split('/').nth(1),
                "tag":string(record,"name").splitn(3,'/').nth(2),"run_id":record["run_id"],"conversation_id":record["conversation_id"],
                "archive_id":record["archive_id"],"archived_at":record["archived_at"],"state":"archived","tracked":true,
                "resumable":!string(record,"conversation_id").is_empty(),"cwd":record["cwd"]})},
            "source":event["source"],"message_id":id,"seq":event.get("seq").cloned().unwrap_or(json!(0)),
            "content":content,"content_truncated":content_truncated,"snippet":snippet,"event":event});
        self.results.push(result);
        self.results.sort_by(|a, b| {
            number(b, "at")
                .total_cmp(&number(a, "at"))
                .then_with(|| string(a, "name").cmp(string(b, "name")))
                .then_with(|| string(a, "message_id").cmp(string(b, "message_id")))
        });
        if self.results.len() > self.limit {
            self.results.truncate(self.limit);
            self.truncated = true;
        }
    }

    fn transcript(&mut self, record: &Value, path: &Path) {
        if self.bytes_left == 0 {
            self.truncated = true;
            return;
        }
        let Ok(mut file) = File::open(path) else {
            self.unavailable(record);
            return;
        };
        let Ok(metadata) = file.metadata() else {
            self.unavailable(record);
            return;
        };
        if !metadata.is_file() {
            self.unavailable(record);
            return;
        }
        let size = metadata.len();
        let budget = size.min(MAX_FILE_BYTES).min(self.bytes_left);
        // A huge transcript must not freeze search. The newest bounded portion
        // is searched and the response explicitly marks incomplete coverage.
        let offset = size.saturating_sub(budget);
        if offset > 0 {
            self.truncated = true;
        }
        if file.seek(SeekFrom::Start(offset)).is_err() {
            self.unavailable(record);
            return;
        }
        self.bytes_left -= budget;
        let mut reader = BufReader::new(file.take(budget));
        if offset > 0 && skip_line(&mut reader).is_err() {
            self.unavailable(record);
            return;
        }
        let mut line = Vec::new();
        loop {
            line.clear();
            let mut oversized = false;
            match bounded_line(&mut reader, &mut line, &mut oversized) {
                Ok(false) => break,
                Err(_) => {
                    self.unavailable(record);
                    break;
                }
                Ok(true) => {}
            }
            if oversized {
                self.truncated = true;
                continue;
            }
            if line.iter().all(u8::is_ascii_whitespace) {
                continue;
            }
            match serde_json::from_slice::<Value>(&line) {
                Ok(value) => {
                    if let Some((event, role)) = provider_event(string(record, "agent"), &value) {
                        self.accept(record, event, role);
                    }
                }
                // The active writer can leave its final JSONL row incomplete.
                // Ignore it; a malformed completed row makes coverage partial.
                Err(_) if !line.ends_with(b"\n") => {}
                Err(_) => self.truncated = true,
            }
        }
    }

    fn journal(&mut self, record: &Value, db: &Connection) -> Result<()> {
        if self.rows_left == 0 {
            self.truncated = true;
            return Ok(());
        }
        let name = if string(record, "journal_name").is_empty() {
            string(record, "name")
        } else {
            string(record, "journal_name")
        };
        let mut statement = db.prepare(&format!("SELECT seq, payload FROM events WHERE {} ORDER BY seq DESC LIMIT ?4", journal::TIMELINE))
            .map_err(|error| error.to_string())?;
        let mut rows = statement
            .query(params![
                name,
                record["conversation_id"].as_str(),
                clear_context::earlier_conversations(record).to_string(),
                self.rows_left + 1
            ])
            .map_err(|error| error.to_string())?;
        while let Some(row) = rows.next().map_err(|error| error.to_string())? {
            if self.rows_left == 0 {
                self.truncated = true;
                break;
            }
            self.rows_left -= 1;
            let seq: i64 = row.get(0).map_err(|error| error.to_string())?;
            let payload: String = row.get(1).map_err(|error| error.to_string())?;
            if payload.len() > MAX_LINE_BYTES {
                self.truncated = true;
                continue;
            }
            let Ok(mut event) = serde_json::from_str::<Value>(&payload) else {
                self.truncated = true;
                continue;
            };
            if !event.is_object() {
                self.truncated = true;
                continue;
            }
            // A conversation's journal can span several resumed processes. Its
            // binding is the navigation target, while event.run_id stays intact.
            journal::mark_system_prompt(record, &mut event, "");
            let role = journal_role(&event);
            if string(&event, "detail").is_empty() {
                continue;
            }
            event["seq"] = json!(seq);
            event["source"] = json!("journal");
            self.accept(record, event, role);
        }
        Ok(())
    }
}

fn search(directory: &Path, mut bindings: Vec<Value>, query: &str, limit: usize) -> Result<Value> {
    let pattern = RegexBuilder::new(&regex::escape(query))
        .case_insensitive(true)
        .build()
        .map_err(|error| error.to_string())?;
    let mut search = Search {
        pattern,
        results: Vec::new(),
        limit,
        bytes_left: MAX_TOTAL_BYTES,
        rows_left: MAX_JOURNAL_ROWS,
        truncated: bindings.len() > MAX_SESSIONS,
        unavailable: BTreeSet::new(),
    };
    bindings.truncate(MAX_SESSIONS);
    let journal_path = directory.join("events.sqlite3");
    let db = if journal_path.exists() {
        Connection::open_with_flags(&journal_path, OpenFlags::SQLITE_OPEN_READ_ONLY).ok()
    } else {
        None
    };
    if let Some(db) = &db {
        let _ = db.busy_timeout(std::time::Duration::from_millis(300));
    }
    // DeepSeek history stays in its native engine: one exact query per host.
    let native: Vec<&Value> = bindings.iter().filter(|record| record["backend"] == "dsh").collect();
    let native = if native.is_empty() { BTreeMap::new() } else { dsh::history_search(&native, query, limit.min(20)) };
    for record in &bindings {
        if record["backend"] == "dsh" {
            match native.get(string(record, "name")) {
                Some(Some((events, truncated))) => {
                    search.truncated |= *truncated;
                    for event in events {
                        let role = if event["type"] == "UserPromptSubmit" { "user" } else { "assistant" };
                        search.accept(record, event.clone(), role);
                    }
                }
                _ => search.unavailable(record),
            }
            continue;
        }
        if string(record, "agent") == "kimi" {
            match questions::wire_paths(record) {
                Ok(paths) if !paths.is_empty() => {
                    for path in paths {
                        search.transcript(record, &path);
                    }
                }
                _ => search.unavailable(record),
            }
        } else {
            let transcript = string(record, "transcript");
            if transcript.is_empty() {
                search.unavailable(record);
            } else {
                search.transcript(record, Path::new(transcript));
            }
        }
        if let Some(db) = &db {
            if search.journal(record, db).is_err() {
                search.unavailable(record);
            }
        } else if journal_path.exists() {
            search.unavailable(record);
        }
        // Latest snapshots remain useful if a provider removed old history.
        // Their timestamps are explicitly the corresponding hook timestamps.
        for (field, event_type, role) in [
            ("prompt", "UserPromptSubmit", "user"),
            ("last_message", "AgentMessage", "assistant"),
        ] {
            let text = if field == "prompt" {
                journal::user_prompt(record).as_str().unwrap_or("")
            } else {
                string(record, field)
            };
            if text.is_empty() {
                continue;
            }
            let at = if field == "prompt" {
                number(record, "turn_started")
            } else {
                number(record, "last_event_at")
            };
            if search.results.iter().any(|hit| {
                hit["name"] == record["name"]
                    && hit["conversation_id"] == record["conversation_id"]
                    && string(hit, "role") == role
                    && equivalent_excerpt(string(hit, "content"), text)
            }) {
                continue;
            }
            search.accept(
                record,
                json!({"type":event_type,"source":"snapshot","at":at,
                "agent_id":"","detail":text}),
                role,
            );
        }
    }
    Ok(
        json!({"query":query,"results":search.results,"truncated":search.truncated,
        "scanned_sessions":bindings.len(),"unavailable_sessions":search.unavailable.len(),
        "scope":"public_messages_and_recorded_activity"}),
    )
}

fn equivalent_excerpt(left: &str, right: &str) -> bool {
    if left == right {
        return true;
    }
    (left.ends_with('…') && right.starts_with(left.trim_end_matches('…')))
        || (right.ends_with('…') && left.starts_with(right.trim_end_matches('…')))
}

fn journal_role(event: &Value) -> &'static str {
    if string(event, "agent_id").is_empty() && !event["origin"].is_string() {
        match string(event, "type") {
            "UserPromptSubmit" | "UserPromptQueued" | "TurnStarted" | "QuestionAnswered" => {
                return "user"
            }
            "Stop" | "AgentMessage" => return "assistant",
            _ => {}
        }
    }
    if !string(event, "tool").is_empty() {
        "tool"
    } else {
        "event"
    }
}

// Character limits, not byte slicing, preserve UTF-8 around Cyrillic/emoji hits.
fn excerpt(text: &str, start: usize, end: usize, max: usize) -> String {
    let characters: Vec<(usize, char)> = text.char_indices().collect();
    if characters.len() <= max {
        return text.to_owned();
    }
    let matched = characters.partition_point(|(byte, _)| *byte < start);
    let match_end = characters.partition_point(|(byte, _)| *byte < end);
    let first = matched
        .saturating_sub(max / 4)
        .min(characters.len().saturating_sub(max));
    let last = (first + max).max(match_end).min(characters.len());
    let begin_byte = characters[first].0;
    let end_byte = characters
        .get(last)
        .map(|(byte, _)| *byte)
        .unwrap_or(text.len());
    format!(
        "{}{}{}",
        if first > 0 { "…" } else { "" },
        &text[begin_byte..end_byte],
        if last < characters.len() { "…" } else { "" }
    )
}

fn skip_line(reader: &mut impl BufRead) -> std::io::Result<()> {
    loop {
        let bytes = reader.fill_buf()?;
        if bytes.is_empty() {
            return Ok(());
        }
        let newline = bytes.iter().position(|byte| *byte == b'\n');
        let count = newline.map(|index| index + 1).unwrap_or(bytes.len());
        reader.consume(count);
        if newline.is_some() {
            return Ok(());
        }
    }
}

fn bounded_line(
    reader: &mut impl BufRead,
    line: &mut Vec<u8>,
    oversized: &mut bool,
) -> std::io::Result<bool> {
    loop {
        let bytes = reader.fill_buf()?;
        if bytes.is_empty() {
            return Ok(!line.is_empty() || *oversized);
        }
        let newline = bytes.iter().position(|byte| *byte == b'\n');
        let count = newline.map(|index| index + 1).unwrap_or(bytes.len());
        if !*oversized && line.len() + count <= MAX_LINE_BYTES {
            line.extend_from_slice(&bytes[..count]);
        } else {
            *oversized = true;
            line.clear();
        }
        reader.consume(count);
        if newline.is_some() {
            return Ok(true);
        }
    }
}

fn text_parts(value: &Value, allowed: &[&str]) -> String {
    if let Some(text) = value.as_str() {
        return text.to_owned();
    }
    value
        .as_array()
        .map(|parts| {
            parts
                .iter()
                .filter(|part| allowed.contains(&string(part, "type")))
                .filter_map(|part| part["text"].as_str())
                .collect::<Vec<_>>()
                .join("\n")
        })
        .unwrap_or_default()
}

/// Claude's own record of a local slash command or of its output, which hooks
/// never report as a prompt: the complete raw shapes Claude writes as a user
/// turn without human origin. It is matched before any paste is unwrapped, so
/// tags a person types or pastes stay their message.
fn claude_local_command(event: &Value) -> bool {
    static COMMAND: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    const OUTPUT: (&str, &str) = ("<local-command-stdout>", "</local-command-stdout>");
    let Some(text) = event["message"]["content"].as_str() else { return false };
    if event["origin"]["kind"] == "human" {
        return false;
    }
    let output = text.strip_prefix(OUTPUT.0).and_then(|rest| rest.strip_suffix(OUTPUT.1))
        .is_some_and(|body| !body.contains(OUTPUT.0) && !body.contains(OUTPUT.1));
    output || COMMAND.get_or_init(|| Regex::new(
        r"\A<command-name>[^<]*</command-name>\s*<command-message>[^<]*</command-message>\s*<command-args>[^<]*</command-args>\z",
    ).unwrap()).is_match(text)
}
fn public_user(text: &str) -> bool {
    let text = text.trim_start();
    ![
        "<environment_context>",
        "<permissions instructions>",
        "<collaboration_mode>",
        "<subagent_notification>",
        "<turn_aborted>",
        "# AGENTS.md instructions",
        "<system-reminder>",
    ]
    .iter()
    .any(|prefix| text.starts_with(prefix))
}

pub(super) fn provider_event(agent: &str, event: &Value) -> Option<(Value, &'static str)> {
    let (role, text, id, at, source) = match agent {
        "kimi" => {
            if string(event, "type") != "agent.message.appended"
                || !["", "main"].contains(&string(event, "agentId"))
            {
                return None;
            }
            let message = &event["message"]["message"];
            let role = match string(message, "role") {
                "user" => "user",
                "assistant" => "assistant",
                _ => return None,
            };
            (
                role,
                text_parts(&message["content"], &["text"]),
                string(&event["message"]["meta"], "messageId"),
                number(event, "time") / 1000.0,
                "kimi_wire",
            )
        }
        "claude" => {
            // A compaction summary is Claude's own context, written as a user turn.
            if event["isMeta"] == true || event["isSidechain"] == true || event["isCompactSummary"] == true {
                return None;
            }
            let role = match string(event, "type") {
                "user" => "user",
                "assistant" => "assistant",
                _ => return None,
            };
            let message = &event["message"];
            // Claude records finished background work as a system "user" turn.
            if string(message, "role") != role
                || ["task-notification", "peer"].contains(&string(&event["origin"], "kind"))
                || (role == "user"
                    && (journal::system_prompt(&text_parts(&message["content"], &["text"])).is_some()
                        || claude_local_command(event)))
            {
                return None;
            }
            (
                role,
                journal::pasted_text(&text_parts(&message["content"], &["text"])).into_owned(),
                string(event, "uuid"),
                timestamp(&event["timestamp"]),
                "claude_transcript",
            )
        }
        "codex" => {
            let payload = &event["payload"];
            if string(event, "type") == "event_msg" && string(payload, "type") == "item_completed" {
                let item = &payload["item"];
                if ["analysis", "summary"].contains(&string(item, "phase")) {
                    return None;
                }
                let role = match string(item, "type") {
                    "UserMessage" => "user",
                    "AgentMessage" => "assistant",
                    _ => return None,
                };
                let at = if number(payload, "completed_at_ms") > 0.0 {
                    number(payload, "completed_at_ms") / 1000.0
                } else {
                    timestamp(&event["timestamp"])
                };
                (
                    role,
                    text_parts(
                        &item["content"],
                        &["text", "input_text", "output_text", "Text"],
                    ),
                    string(item, "id"),
                    at,
                    "codex_transcript",
                )
            } else if string(event, "type") == "event_msg"
                && ["user_message", "agent_message"].contains(&string(payload, "type"))
                && !["analysis", "summary"].contains(&string(payload, "phase"))
            {
                let role = if string(payload, "type") == "user_message" {
                    "user"
                } else {
                    "assistant"
                };
                (
                    role,
                    string(payload, "message").to_owned(),
                    string(payload, "id"),
                    timestamp(&event["timestamp"]),
                    "codex_transcript",
                )
            } else if string(event, "type") == "response_item"
                && string(payload, "type") == "message"
                && string(payload, "role") == "assistant"
                && ["", "final", "commentary"].contains(&string(payload, "channel"))
                && ["", "final", "final_answer", "commentary"].contains(&string(payload, "phase"))
            {
                (
                    "assistant",
                    text_parts(&payload["content"], &["output_text", "text"]),
                    string(payload, "id"),
                    timestamp(&event["timestamp"]),
                    "codex_transcript",
                )
            } else {
                return None;
            }
        }
        _ => return None,
    };
    if text.trim().is_empty() || (role == "user" && !public_user(&text)) {
        return None;
    }
    let text = journal::clipped(&json!(text), MAX_LINE_BYTES);
    let mut message = json!({"type":if role == "user" { "UserPromptSubmit" } else { "AgentMessage" },
        "source":source,"at":at,"message_id":id,"agent_id":"","detail":text});
    if agent == "codex" && role == "assistant" {
        let payload = &event["payload"];
        let phase = [string(&payload["item"], "phase"), string(payload,"phase"), string(payload,"channel")]
            .into_iter().find(|value| !value.is_empty()).unwrap_or_default();
        if !phase.is_empty() { message["message_phase"] = json!(phase); }
    }
    Some((message, role))
}

// Provider timestamps are RFC3339; avoid invoking a process for every message.
pub(super) fn timestamp(value: &Value) -> f64 {
    if let Some(number) = value.as_f64() {
        return if number > 0.0 && number.is_finite() {
            number
        } else {
            0.0
        };
    }
    let Some(value) = value.as_str() else {
        return 0.0;
    };
    parse_timestamp(value).unwrap_or(0.0)
}

fn parse_timestamp(value: &str) -> Option<f64> {
    let date = value.get(..19)?;
    let parts: Vec<_> = date
        .split(['-', 'T', ':', ' '])
        .map(str::parse::<i64>)
        .collect();
    if parts.len() != 6 {
        return None;
    }
    let values: Vec<i64> = parts
        .into_iter()
        .collect::<std::result::Result<_, _>>()
        .ok()?;
    let (year, month, day, hour, minute, second) = (
        values[0], values[1], values[2], values[3], values[4], values[5],
    );
    if !(1970..=9999).contains(&year)
        || !(1..=12).contains(&month)
        || !(1..=31).contains(&day)
        || !(0..=23).contains(&hour)
        || !(0..=59).contains(&minute)
        || !(0..=60).contains(&second)
    {
        return None;
    }
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let days_in_month = match month {
        2 => {
            if leap {
                29
            } else {
                28
            }
        }
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    if day > days_in_month {
        return None;
    }
    let mut remainder = &value[19..];
    let mut fraction = 0.0;
    if remainder.starts_with('.') {
        let end = remainder[1..]
            .find(|c: char| !c.is_ascii_digit())
            .map(|index| index + 1)
            .unwrap_or(remainder.len());
        fraction = format!("0{}", &remainder[..end]).parse().ok()?;
        remainder = &remainder[end..];
    }
    let zone = if remainder == "Z" {
        0
    } else {
        if remainder.len() != 6
            || !b"+-".contains(&remainder.as_bytes()[0])
            || remainder.as_bytes()[3] != b':'
        {
            return None;
        }
        let zone_hour = remainder.get(1..3)?.parse::<i64>().ok()?;
        let zone_minute = remainder.get(4..6)?.parse::<i64>().ok()?;
        if zone_hour > 23 || zone_minute > 59 {
            return None;
        }
        (zone_hour * 3600 + zone_minute * 60) * if remainder.starts_with('-') { -1 } else { 1 }
    };
    // Gregorian civil date to days since the Unix epoch.
    let adjusted_year = year - i64::from(month <= 2);
    let era = adjusted_year / 400;
    let year_of_era = adjusted_year - era * 400;
    let month_prime = month + if month > 2 { -3 } else { 9 };
    let day_of_year = (153 * month_prime + 2) / 5 + day - 1;
    let day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    let days = era * 146097 + day_of_era - 719468;
    Some((days * 86400 + hour * 3600 + minute * 60 + second - zone) as f64 + fraction)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn record(path: &Path) -> Value {
        json!({"version":1,"name":"codex/docs","agent":"codex","run_id":"run",
            "conversation_id":"conversation","cwd":"/work/docs","transcript":path,"updated":42})
    }

    fn json_lines(path: &Path, events: &[Value]) {
        let mut output = Vec::new();
        for event in events {
            serde_json::to_writer(&mut output, event).unwrap();
            output.push(b'\n');
        }
        std::fs::write(path, output).unwrap();
    }

    #[test]
    fn unicode_literal_old_public_messages_and_snippets() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("history.jsonl");
        let mut events = vec![
            json!({"type":"event_msg","timestamp":"2026-01-01T10:00:00Z",
            "payload":{"type":"user_message","message":"Начни с РЕЗЕРВНАЯ КОПИЯ [a.*] 🙂"}}),
        ];
        for i in 0..150 {
            events.push(json!({"type":"event_msg","timestamp":"2026-01-01T11:00:00Z",
                "payload":{"type":"agent_message","message":format!("Unrelated later message {i}")}}));
        }
        events.push(json!({"type":"response_item","timestamp":"2026-01-01T12:00:00Z",
            "payload":{"type":"message","role":"developer","content":[{"type":"input_text","text":"секрет резервная копия"}]}}));
        events.push(json!({"type":"response_item","timestamp":"2026-01-01T12:00:00Z",
            "payload":{"type":"message","role":"assistant","channel":"analysis","content":[{"type":"output_text","text":"секрет резервная копия"}]}}));
        events.push(json!({"type":"response_item","payload":{"type":"reasoning","summary":[{"text":"секрет резервная копия"}]}}));
        json_lines(&path, &events);
        let result = search(
            temporary.path(),
            vec![record(&path)],
            "резервная копия [a.*]",
            80,
        )
        .unwrap();
        let hits = result["results"].as_array().unwrap();
        assert_eq!(hits.len(), 1);
        assert_eq!(hits[0]["role"], "user");
        assert!(string(&hits[0], "content").contains("РЕЗЕРВНАЯ"));
        assert!(string(&hits[0], "snippet").contains("🙂"));
        assert_eq!(hits[0]["event"]["type"], "UserPromptSubmit");
        assert_eq!(result["unavailable_sessions"], 0);
        assert_eq!(result["truncated"], false);
        assert!(
            search(temporary.path(), vec![record(&path)], "секрет", 80).unwrap()["results"]
                .as_array()
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn canonical_codex_items_do_not_leak_hidden_context_or_reasoning() {
        let mut event = json!({"type":"event_msg","timestamp":"2026-01-01T00:00:00Z",
            "payload":{"type":"item_completed","completed_at_ms":1767225600000u64,
                "item":{"type":"UserMessage","id":"msg-one","content":[{"type":"text","text":"Actual user request"}]}}});
        let (message, role) = provider_event("codex", &event).unwrap();
        assert_eq!(role, "user");
        assert_eq!(message["message_id"], "msg-one");
        event["payload"]["item"]["content"][0]["text"] =
            json!("<environment_context>hidden environment</environment_context>");
        assert!(provider_event("codex", &event).is_none());
        event["payload"]["item"] =
            json!({"type":"AgentMessage","phase":"analysis","content":"Private reasoning"});
        assert!(provider_event("codex", &event).is_none());
        event["payload"]["item"]["phase"] = json!("final");
        event["payload"]["item"]["content"] = json!("Public final answer");
        assert_eq!(
            provider_event("codex", &event).unwrap().0["detail"],
            "Public final answer"
        );
    }

    #[test]
    fn claude_and_kimi_ignore_private_parts_and_children() {
        let mut claude = json!({"type":"assistant","uuid":"message","timestamp":"2026-01-01T01:00:00+01:00",
            "message":{"role":"assistant","content":[{"type":"thinking","thinking":"private"},
                {"type":"text","text":"Public response"},{"type":"tool_use","input":{"secret":"private"}}]}});
        assert_eq!(
            provider_event("claude", &claude).unwrap().0["detail"],
            "Public response"
        );
        claude["isSidechain"] = json!(true);
        assert!(provider_event("claude", &claude).is_none());
        let pasted = json!({"type":"user","uuid":"pasted","timestamp":"2026-01-01T01:00:00Z","message":{"role":"user",
            "content":"<pasted_content id=\"1b93\">\nFrom the phone\n</pasted_content id=\"1b93\">"}});
        assert_eq!(provider_event("claude", &pasted).unwrap().0["detail"], "From the phone");
        // Claude 2.1.296 frames a prompt that is only a paste with blank lines.
        let mut framed = pasted.clone();
        framed["message"]["content"] = json!("\n\n<pasted_content id=\"c9a0\">\nFrom the phone\n</pasted_content id=\"c9a0\">\n");
        assert_eq!(provider_event("claude", &framed).unwrap().0["detail"], "From the phone");
        // A compaction and the slash command that ran it are not messages.
        let summary = json!({"type":"user","uuid":"summary","timestamp":"2026-01-01T01:00:00Z","isCompactSummary":true,
            "isVisibleInTranscriptOnly":true,"message":{"role":"user","content":"This session is being continued from a previous conversation."}});
        assert!(provider_event("claude", &summary).is_none());
        // Claude's own complete records, as 2.1.289-2.1.296 write them, carry no human origin.
        for command in ["<command-name>/compact</command-name>\n            <command-message>compact</command-message>\n            <command-args></command-args>",
            "<command-name>/effort</command-name>\n            <command-message>effort</command-message>\n            <command-args>xhigh</command-args>",
            "<local-command-stdout>\u{1b}[2mCompacted (ctrl+o to see full summary)\u{1b}[22m\n\u{1b}[2mPostCompact [hgs __state hook] completed successfully\u{1b}[22m</local-command-stdout>",
            "<local-command-stdout>Set effort level to xhigh (saved for new sessions)</local-command-stdout>"] {
            let row = json!({"type":"user","uuid":"command","timestamp":"2026-01-01T01:00:00Z","message":{"role":"user","content":command}});
            assert!(provider_event("claude", &row).is_none(), "{command}");
            let literal = journal::clipped(&json!(command), MAX_LINE_BYTES);
            // The same literal text from a person stays their message: typed, pasted, or with an image.
            let mut typed = row.clone();
            typed["origin"] = json!({"kind":"human"});
            assert_eq!(provider_event("claude", &typed).unwrap().0["detail"], literal);
            let mut pasted = row.clone();
            pasted["message"]["content"] = json!(format!("\n\n<pasted_content id=\"c9a0\">\n{command}\n</pasted_content id=\"c9a0\">\n"));
            assert_eq!(provider_event("claude", &pasted).unwrap().0["detail"], literal);
            let mut parts = row.clone();
            parts["message"]["content"] = json!([{"type":"text","text":command}]);
            assert_eq!(provider_event("claude", &parts).unwrap().0["detail"], literal);
            // Other agents' projection is unchanged.
            let codex = json!({"type":"event_msg","timestamp":"2026-01-01T00:00:00Z","payload":{"type":"user_message","message":command}});
            assert_eq!(provider_event("codex", &codex).unwrap().0["detail"], literal);
            let kimi = json!({"type":"agent.message.appended","agentId":"main","time":1767225600000u64,
                "message":{"message":{"role":"user","content":[{"type":"text","text":command}]},"meta":{"messageId":"user"}}});
            assert_eq!(provider_event("kimi", &kimi).unwrap().0["detail"], literal);
        }
        // Text that is not one complete record stays, whoever wrote it.
        for typed in ["/compact", "Explain <command-name> tags", "<command-name>/compact</command-name>",
            "<local-command-stdout>one</local-command-stdout> and more",
            "<local-command-stdout>one</local-command-stdout><local-command-stdout>two</local-command-stdout>"] {
            let row = json!({"type":"user","uuid":"typed","timestamp":"2026-01-01T01:00:00Z","message":{"role":"user","content":typed}});
            assert_eq!(provider_event("claude", &row).unwrap().0["detail"], typed);
        }
        let notice = "<task-notification>\n<status>completed</status>\n<summary>Background command \"Run checks\" completed</summary>\n</task-notification>";
        let mut task = json!({"type":"user","uuid":"task","timestamp":"2026-01-01T01:00:00Z","promptSource":"system",
            "origin":{"kind":"task-notification"},"message":{"role":"user","content":notice}});
        assert!(provider_event("claude", &task).is_none());
        task.as_object_mut().unwrap().remove("origin");
        assert!(provider_event("claude", &task).is_none());
        assert_eq!(journal_role(&json!({"type":"UserPromptSubmit","origin":"task_notification"})), "event");
        assert_eq!(journal_role(&json!({"type":"UserPromptSubmit","origin":"subagent_report"})), "event");
        task["message"]["content"] = json!("<agent-message from=\"a15\">\nPlease rebase.\n</agent-message>");
        assert!(provider_event("claude", &task).is_none());
        let mut kimi = json!({"type":"agent.message.appended","agentId":"main","time":1767225600000u64,
            "message":{"message":{"role":"assistant","content":[{"type":"think","think":"private"},
                {"type":"text","text":"Public response"}]},"meta":{"messageId":"message"}}});
        assert_eq!(
            provider_event("kimi", &kimi).unwrap().0["detail"],
            "Public response"
        );
        kimi["agentId"] = json!("child");
        assert!(provider_event("kimi", &kimi).is_none());
    }

    #[test]
    fn full_retained_journal_renamed_and_archived_identity() {
        let temporary = tempfile::tempdir().unwrap();
        let db = Connection::open(temporary.path().join("events.sqlite3")).unwrap();
        db.execute_batch("CREATE TABLE events (seq INTEGER PRIMARY KEY, name TEXT, conversation TEXT, payload TEXT);").unwrap();
        for index in 0..300 {
            let event = json!({"type":"Stop","at":index,"detail":if index == 1 {"Старая миграция"} else {"other"}});
            db.execute("INSERT INTO events(name,conversation,payload) VALUES ('codex/old','conversation',?)", [event.to_string()]).unwrap();
        }
        let mut binding = record(&temporary.path().join("missing.jsonl"));
        binding["journal_name"] = json!("codex/old");
        binding["archive_id"] = json!("archive-id");
        let result = search(temporary.path(), vec![binding], "МИГРАЦИЯ", 80).unwrap();
        assert_eq!(result["results"][0]["name"], "codex/docs");
        assert_eq!(result["results"][0]["archive_id"], "archive-id");
        assert_eq!(result["results"][0]["seq"], 2);
        assert_eq!(result["results"][0]["event"]["seq"], 2);
        assert_eq!(result["unavailable_sessions"], 1);
        // Opening a search does not create or write an activity database.
        assert!(!temporary.path().join("search.sqlite3").exists());
    }

    #[test]
    fn corrupt_partial_and_oversized_lines_preserve_other_results() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("history.jsonl");
        let mut file = File::create(&path).unwrap();
        writeln!(file, "not-json").unwrap();
        writeln!(file, "{}", "x".repeat(MAX_LINE_BYTES + 1)).unwrap();
        writeln!(
            file,
            "{}",
            json!({"type":"event_msg","timestamp":"2026-01-01T00:00:00Z",
            "payload":{"type":"agent_message","message":"Found valid answer"}})
        )
        .unwrap();
        write!(file, "{{\"unfinished").unwrap();
        let result = search(temporary.path(), vec![record(&path)], "valid", 80).unwrap();
        assert_eq!(result["results"].as_array().unwrap().len(), 1);
        assert_eq!(result["truncated"], true);
    }

    #[test]
    fn bounded_context_contains_late_match_and_preserves_unicode() {
        let text = format!(
            "{}{}{}",
            "начало 🙂 ".repeat(8000),
            "ИСКОМОЕ",
            " конец".repeat(8000)
        );
        let found = Regex::new("ИСКОМОЕ").unwrap().find(&text).unwrap();
        let snippet = excerpt(&text, found.start(), found.end(), 220);
        assert!(snippet.contains("ИСКОМОЕ"));
        assert!(snippet.starts_with('…') && snippet.ends_with('…'));
        assert!(snippet.chars().count() <= 222);
        let content = excerpt(&text, found.start(), found.end(), MAX_CONTENT_CHARS);
        assert!(content.contains("ИСКОМОЕ"));
        assert!(content.chars().count() <= MAX_CONTENT_CHARS + 2);
    }

    #[test]
    fn results_are_newest_first_and_limit_is_explicit() {
        let temporary = tempfile::tempdir().unwrap();
        let path = temporary.path().join("history.jsonl");
        json_lines(
            &path,
            &(0..5)
                .map(|i| {
                    json!({"type":"event_msg","timestamp":format!("2026-01-01T00:00:0{i}Z"),
            "payload":{"type":"agent_message","message":format!("needle response {i}")}})
                })
                .collect::<Vec<_>>(),
        );
        let result = search(temporary.path(), vec![record(&path)], "needle", 2).unwrap();
        assert_eq!(result["results"].as_array().unwrap().len(), 2);
        assert_eq!(result["results"][0]["content"], "needle response 4");
        assert_eq!(result["truncated"], true);
    }

    #[test]
    fn iso_timestamps_include_fraction_and_offset() {
        assert_eq!(timestamp(&json!("1970-01-01T00:00:00Z")), 0.0);
        assert_eq!(
            timestamp(&json!("2026-01-01T00:00:00.123Z")),
            1767225600.123
        );
        assert_eq!(
            timestamp(&json!("2026-01-01T03:30:00.123+03:30")),
            1767225600.123
        );
        assert_eq!(timestamp(&json!("2025-12-31T19:00:00-05:00")), 1767225600.0);
        assert_eq!(timestamp(&json!("not a date")), 0.0);
        assert_eq!(timestamp(&json!("2026-02-29T00:00:00Z")), 0.0);
        assert_eq!(timestamp(&json!("2024-02-29T00:00:00Z")), 1709164800.0);
        assert_eq!(timestamp(&json!("2026-01-01T00:00:00+99:99")), 0.0);
    }
}
