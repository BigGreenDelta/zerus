//! Kimi's generic tool approvals in Activity. Kimi asks for them on the main
//! agent's wire (an `interaction.request` of kind "approval") and takes the
//! answer only in its TUI panel: its permission hooks can deny but never
//! approve. The card shows the request from the wire in full. A Bash command
//! can be answered here while it is the only request any Kimi agent of the run
//! waits on and the complete panel on screen draws exactly that request. Each
//! key or paste first checks the wires and the run again; only a matching
//! `interaction.resolved` acknowledges delivery. Every other request stays
//! visible and is answered in Terminal.
use super::question_terminal::{self, Native, Transport};
use super::*;
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::time::{Duration, Instant};

pub(super) const SOURCE: &str = "kimi_tool_approval";
/// Kimi's DEFAULT_APPROVAL_CHOICES, the chooser of every tool without its own.
const LABELS: [&str; 4] = [
    "Approve once",
    "Approve for this session",
    "Reject",
    "Reject with feedback",
];
const RUN: &str = "Run this command?";
const HINT: &str = "↑/↓ select · 1/2/3/4 choose · ↵ confirm";
const TYPING: &str = "Type feedback · ↵ submit.";
const MAX_BODY_CHARS: usize = 16_000;
const MAX_FEEDBACK_CHARS: usize = 2000;
const MAX_MAIN_BYTES: u64 = 8 * 1024 * 1024;
const MAX_AGENTS: usize = 64;
const MAX_AGENT_BYTES: u64 = 4 * 1024 * 1024;
/// How much earlier than its PreToolUse hook Kimi may record a request.
const SKEW_MS: f64 = 60_000.0;
const UNVERIFIED: &str =
    "Zerus cannot confirm that this is the only Kimi request waiting. Answer it in Terminal.";
const NARROW: &str =
    "Kimi's approval does not show this whole request; enlarge Terminal or answer there";

/// Pending approvals of the main agent, each bound to its active tool call
/// and removed once Kimi records a resolution. Plan reviews and goal starts
/// have their own choosers.
pub(super) fn recover(record: &Value, events: &[Value]) -> Vec<Value> {
    if string(record, "agent") != "kimi" {
        return Vec::new();
    }
    open(events)
        .values()
        .filter_map(|event| card(record, event))
        .collect()
}

/// Unresolved interaction requests by ID, in any of `events`.
fn open(events: &[Value]) -> BTreeMap<String, Value> {
    let mut pending = BTreeMap::new();
    for event in events {
        let id = string(event, "id");
        match string(event, "type") {
            "interaction.request" if !id.is_empty() => {
                pending.insert(id.to_owned(), event.clone());
            }
            "interaction.resolved" | "interaction.cancelled" | "interaction.dismissed" => {
                pending.remove(id);
            }
            _ => {}
        }
    }
    pending
}

fn active(record: &Value, call: &str, tool: Option<&str>) -> bool {
    record["active_tools"]
        .get(call)
        .is_some_and(|active| tool.is_none_or(|tool| string(active, "name") == tool))
}

/// Kimi's approval panel title (headerFor).
fn title(tool: &str) -> String {
    match tool {
        "Bash" => RUN.into(),
        "Write" => "Write this file?".into(),
        "Edit" => "Apply these edits?".into(),
        "TaskStop" => "Stop this task?".into(),
        _ => format!("Approve {tool}?"),
    }
}

/// A character Kimi and the terminal both draw one cell wide.
fn cell(ch: char) -> bool {
    matches!(ch, ' '..='~' | 'À'..='ÿ' | 'Ѐ'..='џ')
}

/// Kimi's DANGER_PATTERNS, first match wins. JavaScript's `\b` is an ASCII
/// word boundary; `cell` keeps every other difference out of reach.
fn danger(command: &str) -> Option<&'static str> {
    static PATTERNS: std::sync::OnceLock<Vec<(regex::Regex, &'static str)>> =
        std::sync::OnceLock::new();
    let patterns = PATTERNS.get_or_init(|| {
        [
            (
                r"(?i)(?-u:\b)rm\s+(-[a-z]*[rf][a-z]*|--recursive|--force)",
                "recursive delete",
            ),
            (r"(?i)(?-u:\b)sudo(?-u:\b)", "sudo"),
            (
                r"(?i)(?-u:\b)(curl|wget)(?-u:\b)[^|]*\|\s*(sh|bash|zsh)(?-u:\b)",
                "pipe to shell",
            ),
            (r"(?i)(?-u:\b)dd(?-u:\b)[^|]*(?-u:\b)of=", "dd write"),
            (r"(?i)(?-u:\b)mkfs(?-u:\b)", "mkfs"),
            (r"(?i)>\s*/dev/(sd|nvme|disk|hd)", "write to raw device"),
            (r"(?i)(?-u:\b)chmod\s+-R?\s*777(?-u:\b)", "chmod 777"),
            (r"(?i):\(\)\s*\{\s*:\|:&\s*\}", "fork bomb"),
        ]
        .into_iter()
        .map(|(pattern, label)| (regex::Regex::new(pattern).unwrap(), label))
        .collect()
    });
    patterns
        .iter()
        .find(|(pattern, _)| pattern.is_match(command))
        .map(|(_, label)| *label)
}

/// pi-tui's wrapSingleLine for one-cell characters. None when its rows would
/// not show every character: a row break that swallows more than one space,
/// or trailing spaces that the rows trim, could draw another command the same.
fn wrap(line: &str, width: usize) -> Option<Vec<String>> {
    if width == 0 || line.ends_with(' ') {
        return None;
    }
    if line.chars().count() <= width {
        return Some(vec![line.to_owned()]);
    }
    let mut tokens: Vec<String> = Vec::new();
    for ch in line.chars() {
        match tokens.last_mut() {
            Some(last) if last.starts_with(' ') == (ch == ' ') => last.push(ch),
            _ => tokens.push(ch.to_string()),
        }
    }
    // The spaces a break removes from the end of `row`: exactly one, or none.
    let ended = |row: &str| {
        let kept = row.trim_end_matches(' ');
        (row.len() - kept.len() <= 1 && !kept.is_empty()).then(|| kept.to_owned())
    };
    let mut rows = Vec::new();
    let mut current = String::new();
    for token in tokens {
        let size = token.chars().count();
        let space = token.starts_with(' ');
        if size > width && !space {
            if !current.is_empty() {
                rows.push(ended(&current)?);
            }
            let chars: Vec<char> = token.chars().collect();
            let mut pieces: Vec<String> = chars
                .chunks(width)
                .map(|piece| piece.iter().collect())
                .collect();
            current = pieces.pop().unwrap_or_default();
            rows.extend(pieces);
            continue;
        }
        let used = current.chars().count();
        if used + size > width && used > 0 {
            rows.push(ended(&current)?);
            if space {
                // The break swallows this run of spaces.
                if size > 1 || current.ends_with(' ') {
                    return None;
                }
                current = String::new();
            } else {
                current = token;
            }
        } else {
            current.push_str(&token);
        }
    }
    if !current.is_empty() {
        rows.push(current);
    }
    (!rows
        .iter()
        .any(|row| row.ends_with(' ') || row.chars().count() > width))
    .then_some(rows)
}

/// A Bash command request that Kimi draws with its shell block.
struct Shell<'a> {
    command: &'a str,
    cwd: &'a str,
    description: &'a str,
    danger: Option<&'static str>,
}

fn shell(request: &Value) -> Result<Shell<'_>> {
    let display = &request["display"];
    let other =
        "Zerus answers only Kimi's Bash command approvals here. Answer this request in Terminal.";
    if string(request, "toolName") != "Bash" || string(display, "kind") != "command" {
        return Err(other.into());
    }
    let undrawn = "Zerus cannot show this command exactly as Kimi draws it. Answer it in Terminal.";
    let fields = display.as_object().ok_or(other)?;
    if fields
        .keys()
        .any(|key| !["kind", "command", "cwd", "description", "language"].contains(&key.as_str()))
        || fields
            .get("language")
            .is_some_and(|language| language != "bash")
    {
        return Err(undrawn.into());
    }
    let text = |key: &str| match &display[key] {
        Value::Null => Some(""),
        Value::String(text) if text.chars().all(cell) => Some(text.as_str()),
        _ => None,
    };
    let command = display["command"].as_str().filter(|command| {
        !command.trim().is_empty()
            && command.chars().all(|ch| ch == '\n' || cell(ch))
            && command.split('\n').all(|line| !line.ends_with(' '))
    });
    match (command, text("cwd"), text("description")) {
        (Some(command), Some(cwd), Some(description)) => Ok(Shell {
            command,
            cwd,
            description,
            danger: danger(command),
        }),
        _ => Err(undrawn.into()),
    }
}

impl Shell<'_> {
    /// What the card shows: every row Kimi draws for the request, unwrapped.
    fn body(&self) -> String {
        let mut rows = Vec::new();
        if !self.cwd.is_empty() {
            rows.push(format!("cwd: {}", self.cwd));
        }
        if let Some(label) = self.danger {
            rows.push(format!("Dangerous: {label}"));
        }
        for (index, line) in self.command.split('\n').enumerate() {
            rows.push(
                format!("{} {line}", if index == 0 { "$" } else { "·" })
                    .trim_end()
                    .to_owned(),
            );
        }
        if !self.description.is_empty() {
            rows.push(self.description.to_owned());
        }
        rows.join("\n")
    }

    /// The panel's rows above its choices, for a panel `width` cells wide:
    /// rule, title, blank, the request, blank.
    fn head(&self, width: usize) -> Result<Vec<String>> {
        if width < HINT.chars().count() + 2 {
            return Err(NARROW.into());
        }
        let mut rows = vec!["─".repeat(width), format!("  ▶ {RUN}"), String::new()];
        if !self.cwd.is_empty() {
            rows.push(format!("  cwd: {}", self.cwd));
        }
        if let Some(label) = self.danger {
            rows.push(format!("  Dangerous: {label}"));
        }
        for (index, line) in self.command.split('\n').enumerate() {
            let prefix = if index == 0 { "$" } else { "·" };
            let wrapped = wrap(line, width - 4).ok_or(
                "Zerus cannot show this command exactly as Kimi draws it. Answer it in Terminal.",
            )?;
            for (part, text) in wrapped.iter().enumerate() {
                rows.push(match (part, text.is_empty()) {
                    (0, true) => format!("  {prefix}"),
                    (0, false) => format!("  {prefix} {text}"),
                    _ => format!("    {text}"),
                });
            }
        }
        if !self.description.is_empty() {
            rows.push(format!("    {}", self.description));
        }
        rows.push(String::new());
        // Kimi truncates a longer row, hiding the rest of the request.
        if rows.iter().any(|row| row.chars().count() > width) {
            return Err(NARROW.into());
        }
        Ok(rows)
    }
}

/// The request as JSON for a card that is answered only in Terminal.
fn literal(request: &Value) -> String {
    let display = serde_json::to_string_pretty(&request["display"]).unwrap_or_default();
    format!("{}\n\n{display}", string(request, "action"))
        .trim()
        .to_owned()
}

fn card(record: &Value, event: &Value) -> Option<Value> {
    let id = string(event, "id");
    let call = string(event, "toolCallId");
    let request = &event["request"];
    let tool = string(request, "toolName");
    let display = &request["display"];
    let kind = string(display, "kind");
    if string(event, "type") != "interaction.request"
        || string(event, "kind") != "approval"
        || string(event, "agentId") != "main"
        || id.is_empty()
        || id.len() > 240
        || call.is_empty()
        || call.len() > 200
        || string(request, "id") != id
        || string(request, "toolCallId") != call
        || tool.is_empty()
        || tool.chars().count() > 120
        || tool.chars().any(char::is_control)
        || tool == "ExitPlanMode"
        || ["plan_review", "goal_start"].contains(&kind)
        || !display.is_object()
        || !active(record, call, Some(tool))
    {
        return None;
    }
    let (body, problem) = match shell(request) {
        Ok(shell) => (shell.body(), None),
        Err(problem) => (literal(request), Some(problem)),
    };
    let complete = body.chars().count() <= MAX_BODY_CHARS;
    let body = if complete {
        body
    } else {
        let shown: String = body.chars().take(MAX_BODY_CHARS).collect();
        format!("{shown}\n\n…\n\nThe rest of this request is shown only in Terminal.")
    };
    let problem = problem.or_else(|| {
        (!complete)
            .then(|| "This request is too long to review here. Answer it in Terminal.".to_owned())
    });
    let actionable = problem.is_none();
    let action = string(request, "action");
    let session = format!(
        "Kimi also stops asking about this exact command for the rest of this session. It approves at once every other request already waiting with the same action, \"{action}\", even a different command: Kimi's action shows only a command's first 50 characters."
    );
    let options = json!([
        {"id":"1","label":LABELS[0],"description":"","scope":"once"},
        {"id":"2","label":LABELS[1],"description":session,"scope":"broader"},
        {"id":"3","label":LABELS[2],"description":"","scope":"once"}]);
    let mut card = json!({"question_id":id,"tool_call_id":call,"agent_id":"main",
        "run_id":record["run_id"],"conversation_id":record["conversation_id"],
        "created_at":event["time"].as_f64().unwrap_or(0.0) / 1000.0,"source":SOURCE,
        "approval":true,"approval_choices":true,"can_answer":false,"answer_transport":"",
        "answer_unavailable_reason":problem.unwrap_or_else(|| "Open Terminal to answer this approval.".into()),
        "questions":[{"id":"approval","question":title(tool),"header":tool,"body":body,
            "multi_select":false,"allow_other":actionable,"options":options}]});
    if actionable {
        card["questions"][0]["other_label"] = json!(LABELS[3]);
        card["questions"][0]["other_description"] =
            json!("Reject this request with your message; Kimi continues without it");
    }
    // The whole native request and the exact process that shows it.
    card["question_hash"] = json!(format!(
        "{:x}",
        Sha256::digest(
            json!([
                SOURCE,
                id,
                call,
                event["agentId"],
                request,
                record["run_id"],
                record["conversation_id"],
                record["pid"],
                record["process_start"],
                record["pane"],
                actionable,
                card["questions"]
            ])
            .to_string()
        )
    ));
    Some(card)
}

/// The interaction records in the last `max` bytes of a wire, whether older
/// records were left out, and the time of the first record read.
struct Window {
    events: Vec<Value>,
    truncated: bool,
    first: f64,
}

fn window(path: &Path, max: u64, main: bool) -> Result<Window> {
    let unreadable = |_| UNVERIFIED.to_owned();
    let mut file = File::open(path).map_err(unreadable)?;
    let size = file.metadata().map_err(unreadable)?.len();
    let offset = size.saturating_sub(max);
    file.seek(SeekFrom::Start(offset)).map_err(unreadable)?;
    let mut bytes = Vec::new();
    file.take(max).read_to_end(&mut bytes).map_err(unreadable)?;
    let mut window = Window {
        events: Vec::new(),
        truncated: offset > 0,
        first: f64::INFINITY,
    };
    for (index, line) in bytes.split(|byte| *byte == b'\n').enumerate() {
        if (offset > 0 && index == 0) || line.is_empty() {
            continue;
        }
        let interaction = line.windows(12).any(|part| part == b"interaction.");
        if window.first.is_infinite() || interaction {
            // A record cut off while Kimi writes it might be a request.
            let Ok(record) = serde_json::from_slice::<Value>(line) else {
                if interaction {
                    return Err(UNVERIFIED.into());
                }
                continue;
            };
            if window.first.is_infinite() {
                window.first = record["time"].as_f64().unwrap_or(f64::INFINITY);
            }
            if interaction
                && string(&record, "type").starts_with("interaction.")
                && (!main || string(&record, "agentId") == "main")
            {
                window.events.push(record);
            }
        }
    }
    Ok(window)
}

/// Every other agent of the conversation, such as a subagent, draws its
/// approvals in the same panel. None of them may wait on a request.
fn others(main: &Path) -> Result<()> {
    let agents = main.parent().and_then(Path::parent).ok_or(UNVERIFIED)?;
    let root = crate::platform::canonical(agents).map_err(|_| UNVERIFIED)?;
    let mut count = 0;
    for entry in fs::read_dir(&root).map_err(|_| UNVERIFIED)? {
        let entry = entry.map_err(|_| UNVERIFIED)?;
        if entry.file_name() == "main" {
            continue;
        }
        count += 1;
        if count > MAX_AGENTS {
            return Err(UNVERIFIED.into());
        }
        let path = entry.path().join("wire.jsonl");
        if fs::symlink_metadata(&path).is_err() {
            continue;
        }
        let resolved = crate::platform::canonical(&path).map_err(|_| UNVERIFIED)?;
        let size = fs::metadata(&resolved).map_err(|_| UNVERIFIED)?.len();
        if !resolved.starts_with(&root) || !resolved.is_file() || size > MAX_AGENT_BYTES {
            return Err(UNVERIFIED.into());
        }
        if !open(&window(&resolved, MAX_AGENT_BYTES, false)?.events).is_empty() {
            return Err("Another Kimi agent is waiting for an answer. Answer in Terminal.".into());
        }
    }
    Ok(())
}

/// The exact unresolved wire request behind `card`, read again: the same
/// request with its active tool, the same hash, and the only request any agent
/// of this run may show. The caller checks the screen.
fn bound(record: &Value, card: &Value) -> Result<Value> {
    let paths = questions::wire_paths(record)?;
    let [path] = paths.as_slice() else {
        return Err(UNVERIFIED.into());
    };
    let main = window(path, MAX_MAIN_BYTES, true)?;
    let id = string(card, "question_id");
    let waiting = open(&main.events);
    let gone = "This approval is no longer pending; refresh Activity";
    let event = waiting.get(id).ok_or(gone)?;
    let fresh = self::card(record, event).ok_or(gone)?;
    if fresh["question_hash"] != card["question_hash"]
        || fresh["questions"][0]["allow_other"] != true
    {
        return Err("This approval changed; refresh Activity".into());
    }
    // A request recorded since the run's oldest active tool started may be
    // shown, whatever its tool; an older one belongs to an earlier process.
    let floor = record["active_tools"]
        .as_object()
        .into_iter()
        .flatten()
        .map(|(_, tool)| {
            tool["started"]
                .as_f64()
                .map_or(f64::NEG_INFINITY, |at| at * 1000.0 - SKEW_MS)
        })
        .fold(f64::INFINITY, f64::min);
    if main.truncated && main.first > floor {
        return Err(UNVERIFIED.into());
    }
    let shown = waiting
        .values()
        .filter(|other| {
            active(record, string(other, "toolCallId"), None)
                || other["time"].as_f64().is_none_or(|at| at >= floor)
        })
        .count();
    if shown != 1 {
        return Err("Another Kimi request is waiting. Answer in Terminal.".into());
    }
    others(path)?;
    Ok(event.clone())
}

#[derive(Debug, PartialEq)]
enum Shown {
    /// The chooser waits for a choice.
    Choosing,
    /// Reject with feedback shows its input line with this visible text.
    Feedback(String),
}

/// A full-width rule: its left margin and its width.
fn rule(row: &str) -> Option<(usize, usize)> {
    let rest = row.trim_start_matches(' ');
    let margin = row.len() - rest.len();
    let width = rest.chars().count();
    (margin <= 8 && width >= 20 && rest.chars().all(|ch| ch == '─')).then_some((margin, width))
}

/// `row` without the panel's left margin; a blank row has none.
fn inside(row: &str, margin: usize) -> Option<&str> {
    if row.is_empty() {
        return Some("");
    }
    row.strip_prefix(&" ".repeat(margin))
}

/// The four default choices from `rows[0]`, then a blank row, the key hint
/// and the closing rule: what is selected, and Reject with feedback's text.
fn chooser(rows: &[&str], margin: usize, width: usize) -> Option<Shown> {
    let mut selected = None;
    let mut typed = None;
    for (index, label) in LABELS.iter().enumerate() {
        let row = inside(rows.get(index)?, margin)?;
        let number = format!("{}. {label}", index + 1);
        if row == format!("    {number}") {
            continue;
        }
        let marked = row.strip_prefix("  ▶ ")?;
        if selected.replace(index).is_some() {
            return None;
        }
        if marked != number {
            let text = marked.strip_prefix(number.as_str())?.strip_prefix("  ")?;
            if index != 3 || text.is_empty() {
                return None;
            }
            typed = Some(text.to_owned());
        }
    }
    let selected = selected?;
    if !inside(rows.get(4)?, margin)?.is_empty()
        || *rows.get(6)? != format!("{}{}", " ".repeat(margin), "─".repeat(width))
    {
        return None;
    }
    match inside(rows.get(5)?, margin)?.strip_prefix("  ")? {
        HINT if typed.is_none() => Some(Shown::Choosing),
        TYPING if selected == 3 => Some(Shown::Feedback(typed.unwrap_or_default())),
        _ => None,
    }
}

/// The bottom of an approval panel with the default choices: the first
/// choice row, the panel's margin and width.
fn bottom(rows: &[&str]) -> Option<(usize, usize, usize)> {
    (6..rows.len()).rev().find_map(|end| {
        let (margin, width) = rule(rows[end])?;
        let first = end - 6;
        (first > 0
            && inside(rows[first - 1], margin)?.is_empty()
            && chooser(&rows[first..], margin, width).is_some())
        .then_some((first, margin, width))
    })
}

/// Its top rule and title above the blank row before the choices.
fn top(rows: &[&str], first: usize, margin: usize, width: usize) -> Option<usize> {
    let start = rows[..first - 1]
        .iter()
        .rposition(|row| rule(row) == Some((margin, width)))?;
    let heading = inside(rows.get(start + 1)?, margin)?.strip_prefix("  ▶ ")?;
    (heading.ends_with('?') && inside(rows.get(start + 2)?, margin)?.is_empty()).then_some(start)
}

fn plain_rows(screen: &str) -> Option<String> {
    question_terminal::plain(screen).ok()
}

/// A panel whose choices show while its top is above the first row.
fn clipped(screen: &str) -> bool {
    let Some(plain) = plain_rows(screen) else {
        return false;
    };
    let rows: Vec<&str> = plain.lines().map(str::trim_end).collect();
    bottom(&rows).is_some_and(|(first, margin, width)| top(&rows, first, margin, width).is_none())
}

fn complete(screen: &str) -> bool {
    let Some(plain) = plain_rows(screen) else {
        return false;
    };
    let rows: Vec<&str> = plain.lines().map(str::trim_end).collect();
    bottom(&rows).is_some_and(|(first, margin, width)| top(&rows, first, margin, width).is_some())
}

/// The panel on `screen` that draws exactly `request`, row by row.
fn read(screen: &str, request: &Value) -> Result<Shown> {
    let shell = shell(request)?;
    let plain = question_terminal::plain(screen)?;
    let rows: Vec<&str> = plain.lines().map(str::trim_end).collect();
    let mut found = Vec::new();
    let mut narrow = None;
    for (start, row) in rows.iter().enumerate() {
        let Some((margin, width)) = rule(row) else {
            continue;
        };
        let head = match shell.head(width) {
            Ok(head) => head,
            Err(error) => {
                narrow = Some(error);
                continue;
            }
        };
        let drawn = head.iter().enumerate().all(|(index, expected)| {
            rows.get(start + index).and_then(|row| inside(row, margin)) == Some(expected.as_str())
        });
        if let Some(shown) = drawn
            .then(|| chooser(&rows[start + head.len()..], margin, width))
            .flatten()
        {
            found.push(shown);
        }
    }
    match found.len() {
        1 => Ok(found.pop().unwrap()),
        0 if clipped(screen) => Err(NARROW.into()),
        0 if complete(screen) => Err(narrow.unwrap_or_else(|| {
            "The visible Kimi approval differs from this request; answer it in Terminal".into()
        })),
        0 => Err("Kimi's approval is not visible; open Terminal".into()),
        _ => Err("Kimi shows this approval more than once; answer it in Terminal".into()),
    }
}

/// The choice index and Reject with feedback's text.
fn chosen(card: &Value, answers: &Value) -> Result<(usize, String)> {
    let answer = answers
        .as_array()
        .filter(|items| items.len() == 1)
        .map(|items| &items[0])
        .ok_or("answer the approval")?;
    let text = string(answer, "text").trim().to_owned();
    let ids = answer["selected_option_ids"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    if !text.is_empty() {
        if !ids.is_empty() {
            return Err("choose one approval action".into());
        }
        if card["questions"][0]["allow_other"] != true
            || text.chars().count() > MAX_FEEDBACK_CHARS
            || text.chars().any(char::is_control)
        {
            return Err("this feedback cannot be sent to Kimi".into());
        }
        return Ok((3, text));
    }
    match ids.as_slice() {
        [id] => match id.as_str() {
            Some("1") => Ok((0, String::new())),
            Some("2") => Ok((1, String::new())),
            Some("3") => Ok((2, String::new())),
            _ => Err("unknown approval action".into()),
        },
        _ => Err("choose one approval action".into()),
    }
}

/// Whether Kimi recorded exactly the submitted decision and nothing else.
pub(super) fn response_matches(card: &Value, answers: &Value, event: &Value) -> bool {
    let Ok((index, feedback)) = chosen(card, answers) else {
        return false;
    };
    let mut expected = serde_json::Map::new();
    expected.insert(
        "decision".into(),
        json!(if index < 2 { "approved" } else { "rejected" }),
    );
    if index == 1 {
        expected.insert("scope".into(), json!("session"));
    }
    if index == 3 {
        expected.insert("feedback".into(), json!(feedback));
    }
    string(event, "type") == "interaction.resolved"
        && event["response"].as_object() == Some(&expected)
}

/// The response as the journal's question-answer summary expects it.
pub(super) fn journal_response(card: &Value, event: &Value) -> Value {
    let response = &event["response"];
    let feedback = string(response, "feedback");
    let answer = match (string(response, "decision"), string(response, "scope")) {
        ("approved", "session") => LABELS[1].to_owned(),
        ("approved", _) => LABELS[0].to_owned(),
        _ if !feedback.is_empty() => format!("{}: {feedback}", LABELS[3]),
        _ => LABELS[2].to_owned(),
    };
    json!({"time":event["time"],"response":{"answers":{string(&card["questions"][0], "question"):answer}}})
}

/// Let an unviewed pane grow until a clipped panel of this request shows.
pub(super) fn fit(record: &Value, card: &Value) {
    if card["questions"][0]["allow_other"] != true {
        return;
    }
    let Ok(current) = Native::new(record, card).identity() else {
        return;
    };
    // Only a clipped panel of this exact, sole request may resize its pane.
    let Ok(screen) = Native::capture(&current) else {
        return;
    };
    if clipped(&screen) && bound(&current, card).is_ok() {
        question_terminal::fit_unviewed(&current, screen, Native::capture, clipped, complete);
    }
}

pub(super) fn available(record: &Value, card: &Value) -> Result<()> {
    if string(record, "agent") != "kimi" || string(card, "source") != SOURCE {
        return Err("Not a Kimi tool approval".into());
    }
    if card["questions"][0]["allow_other"] != true {
        return Err(string(card, "answer_unavailable_reason").to_owned());
    }
    let current = Native::new(record, card).identity()?;
    let event = bound(&current, card)?;
    match read(&Native::capture(&current)?, &event["request"])? {
        Shown::Choosing => Ok(()),
        Shown::Feedback(_) => Err("Finish the feedback in Terminal".into()),
    }
}

pub(super) fn answer(record: &Value, card: &Value, answers: &Value) -> Result<()> {
    available(record, card)?;
    let guard = Native::new(record, card);
    let request = bound(&guard.identity()?, card)?["request"].clone();
    // Every write first finds the same request, alone, in the same run.
    let mut check = || bound(&guard.identity()?, card).map(|_| ());
    let mut native = Native::new(record, card);
    drive(&mut native, &mut check, &request, card, answers).map_err(|error| {
        if native.touched() {
            format!("delivery uncertain: {error}; inspect Terminal and do not resend automatically")
        } else {
            error
        }
    })
}

fn wait<T: Transport>(
    io: &mut T,
    request: &Value,
    expected: impl Fn(&Shown) -> bool,
) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        if expected(&read(&io.screen()?, request)?) {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Kimi's approval did not reach the expected state".into());
        }
        io.pause();
    }
}

fn drive<T: Transport>(
    io: &mut T,
    check: &mut dyn FnMut() -> Result<()>,
    request: &Value,
    card: &Value,
    answers: &Value,
) -> Result<()> {
    let (index, feedback) = chosen(card, answers)?;
    if read(&io.screen()?, request)? != Shown::Choosing {
        return Err("Finish the feedback in Terminal".into());
    }
    // The chooser's own number key selects and submits one choice; Reject
    // with feedback opens its input line instead.
    check()?;
    io.key(&(index + 1).to_string())?;
    if index == 3 {
        wait(io, request, |shown| {
            *shown == Shown::Feedback(String::new())
        })?;
        check()?;
        io.paste(&feedback)?;
        // A line wider than its input scrolls; its visible end must match.
        wait(
            io,
            request,
            |shown| matches!(shown, Shown::Feedback(text) if !text.trim_start_matches('…').is_empty() && feedback.ends_with(text.trim_start_matches('…'))),
        )?;
        check()?;
        io.key("Enter")?;
    }
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if let Some(response) = io.resolved()? {
            let event = json!({"type":"interaction.resolved","response":response});
            if !response_matches(card, answers, &event) {
                return Err("Kimi recorded a different decision".into());
            }
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err("Kimi has not confirmed the decision".into());
        }
        io.pause();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const COMMAND: &str = "rm -rf build/cache";

    fn record(home: &Path) -> Value {
        json!({"agent":"kimi","agent_home":home,"run_id":"run","conversation_id":"session_test","pid":42,
            "process_start":"start","pane":"%3","active_tools":{"tool_bash":{"name":"Bash","started":1000.0}}})
    }
    fn request(id: &str, call: &str, command: &str) -> Value {
        json!({"type":"interaction.request","agentId":"main","id":id,"kind":"approval","toolCallId":call,
            "request":{"id":id,"sessionId":"session_test","agentId":"main","turnId":1,"toolCallId":call,"toolName":"Bash",
                "action":format!("Running: {}", command.chars().take(50).collect::<String>()),
                "display":{"kind":"command","command":command,"cwd":"/workspace/example","description":"Remove the cached build","language":"bash"}},
            "time":1_000_500})
    }
    fn resolved(id: &str) -> Value {
        json!({"type":"interaction.resolved","agentId":"main","id":id,"response":{"decision":"approved"}})
    }
    fn only(record: &Value, events: &[Value]) -> Value {
        let cards = recover(record, events);
        let [card] = cards.as_slice() else {
            panic!("{cards:?}")
        };
        card.clone()
    }
    /// A Kimi home whose main agent and other agents wrote these records.
    fn home(main: &[Value], agents: &[(&str, Vec<Value>)]) -> tempfile::TempDir {
        let home = tempfile::tempdir().unwrap();
        let session = home.path().join("sessions/workspace/session_test/agents");
        for (agent, events) in [("main", main.to_vec())]
            .into_iter()
            .chain(agents.iter().map(|(a, e)| (*a, e.clone())))
        {
            fs::create_dir_all(session.join(agent)).unwrap();
            let lines: String = events.iter().map(|event| format!("{event}\n")).collect();
            fs::write(session.join(agent).join("wire.jsonl"), lines).unwrap();
        }
        home
    }

    #[test]
    fn a_bash_approval_shows_every_native_row_and_choice() {
        let home = tempfile::tempdir().unwrap();
        let card = only(
            &record(home.path()),
            &[request("approval_a", "tool_bash", COMMAND)],
        );
        assert_eq!(
            (
                card["source"].clone(),
                card["question_id"].clone(),
                card["tool_call_id"].clone(),
                card["created_at"].clone()
            ),
            (
                json!(SOURCE),
                json!("approval_a"),
                json!("tool_bash"),
                json!(1000.5)
            )
        );
        assert_eq!(
            (
                card["approval"].clone(),
                card["approval_choices"].clone(),
                card["can_answer"].clone()
            ),
            (json!(true), json!(true), json!(false))
        );
        let item = &card["questions"][0];
        assert_eq!(
            (item["question"].clone(), item["header"].clone()),
            (json!(RUN), json!("Bash"))
        );
        assert_eq!(item["body"], "cwd: /workspace/example\nDangerous: recursive delete\n$ rm -rf build/cache\nRemove the cached build");
        let options: Vec<_> = item["options"]
            .as_array()
            .unwrap()
            .iter()
            .map(|o| {
                (
                    string(o, "id").to_owned(),
                    string(o, "label").to_owned(),
                    string(o, "scope").to_owned(),
                )
            })
            .collect();
        assert_eq!(
            options,
            [
                ("1", "Approve once", "once"),
                ("2", "Approve for this session", "broader"),
                ("3", "Reject", "once")
            ]
            .map(|(a, b, c)| (a.to_owned(), b.to_owned(), c.to_owned()))
        );
        // The session choice discloses both of its native effects.
        let session = string(&item["options"][1], "description");
        assert!(
            session.contains("this exact command for the rest of this session"),
            "{session}"
        );
        assert!(
            session.contains("\"Running: rm -rf build/cache\", even a different command"),
            "{session}"
        );
        assert!(session.contains("first 50 characters"), "{session}");
        assert_eq!(
            (item["allow_other"].clone(), item["other_label"].clone()),
            (json!(true), json!("Reject with feedback"))
        );
        // Multi-line commands keep Kimi's own line markers.
        let mut lines = request("approval_a", "tool_bash", "cd /tmp\n\nls");
        lines["request"]["display"]
            .as_object_mut()
            .unwrap()
            .remove("description");
        assert_eq!(
            only(&record(home.path()), &[lines])["questions"][0]["body"],
            "cwd: /workspace/example\n$ cd /tmp\n·\n· ls"
        );
    }

    #[test]
    fn the_hash_binds_the_whole_request_and_the_process() {
        let home = tempfile::tempdir().unwrap();
        let base = record(home.path());
        let event = request("approval_a", "tool_bash", COMMAND);
        let hash = only(&base, std::slice::from_ref(&event))["question_hash"].clone();
        let mut changed = Vec::new();
        for (key, value) in [
            ("pid", json!(43)),
            ("process_start", json!("later")),
            ("pane", json!("%4")),
            ("run_id", json!("run-2")),
            ("conversation_id", json!("session_other")),
        ] {
            let mut r = base.clone();
            r[key] = value;
            changed.push(only(&r, std::slice::from_ref(&event))["question_hash"].clone());
        }
        for (path, value) in [
            ("/request/turnId", json!(2)),
            ("/request/sessionId", json!("session_other")),
            ("/request/action", json!("Running: rm -rf build/cache2")),
            ("/request/display/cwd", json!("/workspace/other")),
            ("/request/display/description", json!("Remove it")),
            ("/request/display/command", json!("rm -rf build/cachf")),
        ] {
            let mut e = event.clone();
            *e.pointer_mut(path).unwrap() = value;
            changed.push(only(&base, &[e])["question_hash"].clone());
        }
        let mut renamed = request("approval_b", "tool_bash", COMMAND);
        renamed["request"]["id"] = json!("approval_b");
        changed.push(only(&base, &[renamed])["question_hash"].clone());
        assert!(changed.iter().all(|other| *other != hash), "{changed:?}");
    }

    #[test]
    fn other_requests_have_no_card_or_only_a_terminal_card() {
        let home = tempfile::tempdir().unwrap();
        let r = record(home.path());
        let event = request("approval_a", "tool_bash", COMMAND);
        let mut stopped = r.clone();
        stopped["active_tools"] = json!({});
        assert!(recover(&stopped, std::slice::from_ref(&event)).is_empty());
        assert!(recover(&r, &[event.clone(), resolved("approval_a")]).is_empty());
        assert!(recover(
            &r,
            &[
                event.clone(),
                json!({"type":"interaction.cancelled","id":"approval_a"})
            ]
        )
        .is_empty());
        let mut claude = r.clone();
        claude["agent"] = json!("claude");
        assert!(recover(&claude, std::slice::from_ref(&event)).is_empty());
        for (path, value) in [
            ("/request/toolName", json!("ExitPlanMode")),
            ("/request/display/kind", json!("plan_review")),
            ("/request/display/kind", json!("goal_start")),
            ("/request/id", json!("approval_other")),
            ("/agentId", json!("child")),
        ] {
            let mut e = event.clone();
            *e.pointer_mut(path).unwrap() = value;
            assert!(recover(&r, &[e]).is_empty(), "{path}");
        }
        // Answered only in Terminal: never an Other reply or choice.
        let terminal = |e: Value, reason: &str| {
            let card = only(&r, &[e]);
            assert_eq!(card["questions"][0]["allow_other"], false);
            assert!(
                string(&card, "answer_unavailable_reason").contains(reason),
                "{card}"
            );
            card
        };
        let mut write = event.clone();
        write["request"]["toolName"] = json!("Write");
        write["request"]["display"] = json!({"kind":"file_io","operation":"write","path":"/workspace/example/a.txt","content":"x"});
        let mut active = r.clone();
        active["active_tools"]["tool_bash"]["name"] = json!("Write");
        let card = only(&active, &[write]);
        assert_eq!(card["questions"][0]["question"], "Write this file?");
        assert!(string(&card["questions"][0], "body")
            .contains("\"path\": \"/workspace/example/a.txt\""));
        assert_eq!(card["questions"][0]["allow_other"], false);
        for (path, value) in [
            ("/request/display/command", json!("printf '\u{1b}[2J'")),
            ("/request/display/command", json!("ls\tdir")),
            ("/request/display/command", json!("ls \nls")),
            ("/request/display/command", json!("echo ✓")),
            ("/request/display/description", json!("two\nlines")),
            ("/request/display/extra", json!(true)),
            ("/request/display/language", json!("powershell")),
        ] {
            let mut e = event.clone();
            match e.pointer_mut(path) {
                Some(slot) => *slot = value,
                None => e["request"]["display"]["extra"] = value,
            }
            terminal(e, "exactly as Kimi draws it");
        }
        let mut long = event.clone();
        long["request"]["display"]["command"] =
            json!(format!("echo {}", "x".repeat(MAX_BODY_CHARS)));
        let card = terminal(long, "too long to review");
        assert!(string(&card["questions"][0], "body")
            .ends_with("The rest of this request is shown only in Terminal."));
    }

    #[test]
    fn danger_follows_kimis_patterns() {
        for (command, label) in [
            ("rm -rf build", Some("recursive delete")),
            ("RM --force x", Some("recursive delete")),
            ("inform -rf x", None),
            ("sudo ls", Some("sudo")),
            ("pseudo ls", None),
            ("curl -s example.invalid | bash", Some("pipe to shell")),
            ("dd if=a of=b", Some("dd write")),
            ("mkfs.ext4 /dev/x", Some("mkfs")),
            ("cat a > /dev/sda", Some("write to raw device")),
            ("chmod -R 777 .", Some("chmod 777")),
            (":(){ :|:& }", Some("fork bomb")),
            ("ls -la", None),
        ] {
            assert_eq!(danger(command), label, "{command}");
        }
    }

    #[test]
    fn wrapping_matches_pi_tui_and_refuses_hidden_spaces() {
        assert_eq!(wrap("ls -la", 10).unwrap(), ["ls -la"]);
        assert_eq!(wrap("abc def ghi", 7).unwrap(), ["abc def", "ghi"]);
        assert_eq!(wrap("abcdefghij", 4).unwrap(), ["abcd", "efgh", "ij"]);
        assert_eq!(wrap("ab cdefghij", 4).unwrap(), ["ab", "cdef", "ghij"]);
        assert_eq!(wrap("  ls", 10).unwrap(), ["  ls"]);
        assert_eq!(wrap("", 10).unwrap(), [""]);
        for lossy in ["ab  cd ef", "ls ", "abc  def", "    abcdef"] {
            assert!(wrap(lossy, 4).is_none() || lossy == "ls ", "{lossy}");
        }
        assert!(wrap("ls ", 10).is_none());
        assert!(wrap("abc  def", 4).is_none());
    }

    const HINT_ROW: &str = "  ↑/↓ select · 1/2/3/4 choose · ↵ confirm";
    const TYPING_ROW: &str = "  Type feedback · ↵ submit.";
    const CHOOSING: [&str; 4] = [
        "  ▶ 1. Approve once",
        "    2. Approve for this session",
        "    3. Reject",
        "    4. Reject with feedback",
    ];

    /// Kimi's panel as tmux shows it in an 80-column pane: a one-column
    /// margin, transcript above and the status row below.
    fn screen(body: &[&str], choices: [&str; 4], footer: &str) -> String {
        let rule = "─".repeat(78);
        let mut rows = vec![
            "● Running: rm -rf build/cache".to_owned(),
            rule.clone(),
            format!("  ▶ {RUN}"),
            String::new(),
        ];
        rows.extend(body.iter().map(|row| row.to_string()));
        rows.push(String::new());
        rows.extend(choices.iter().map(|row| row.to_string()));
        rows.extend([String::new(), footer.to_owned(), rule]);
        let mut text: Vec<String> = rows
            .iter()
            .map(|row| {
                if row.is_empty() {
                    String::new()
                } else {
                    format!(" {row}")
                }
            })
            .collect();
        text.push("  Ask When Needed  K3".into());
        text.join("\n")
    }
    const BODY: [&str; 4] = [
        "  cwd: /workspace/example",
        "  Dangerous: recursive delete",
        "  $ rm -rf build/cache",
        "    Remove the cached build",
    ];

    #[test]
    fn the_panel_must_draw_exactly_this_request() {
        let req = &request("approval_a", "tool_bash", COMMAND)["request"];
        let choosing = screen(&BODY, CHOOSING, HINT_ROW);
        assert_eq!(read(&choosing, req).unwrap(), Shown::Choosing);
        assert!(complete(&choosing) && !clipped(&choosing));
        let moved = [
            "    1. Approve once",
            "    2. Approve for this session",
            "  ▶ 3. Reject",
            "    4. Reject with feedback",
        ];
        assert_eq!(
            read(&screen(&BODY, moved, HINT_ROW), req).unwrap(),
            Shown::Choosing
        );
        let typing = [
            "    1. Approve once",
            "    2. Approve for this session",
            "    3. Reject",
            "  ▶ 4. Reject with feedback",
        ];
        assert_eq!(
            read(&screen(&BODY, typing, TYPING_ROW), req).unwrap(),
            Shown::Feedback(String::new())
        );
        // Arrow keys may select Reject with feedback without opening its input.
        assert_eq!(
            read(&screen(&BODY, typing, HINT_ROW), req).unwrap(),
            Shown::Choosing
        );
        let typed = [
            "    1. Approve once",
            "    2. Approve for this session",
            "    3. Reject",
            "  ▶ 4. Reject with feedback  use a dry run",
        ];
        assert_eq!(
            read(&screen(&BODY, typed, TYPING_ROW), req).unwrap(),
            Shown::Feedback("use a dry run".into())
        );
        // Kimi's own styling is presentation, not content.
        let styled = choosing.replace("$ rm -rf", "\x1b[1m$\x1b[0m rm -rf");
        assert_eq!(read(&styled, req).unwrap(), Shown::Choosing);
        let other = |body: &[&str]| screen(body, CHOOSING, HINT_ROW);
        for wrong in [
            other(&[
                "  cwd: /workspace/example",
                "  Dangerous: recursive delete",
                "  $ rm -rf build/cachf",
                "    Remove the cached build",
            ]),
            other(&[
                "  cwd: /workspace/example",
                "  $ rm -rf build/cache",
                "    Remove the cached build",
            ]),
            other(&[
                "  cwd: /workspace/example",
                "  Dangerous: sudo",
                "  $ rm -rf build/cache",
                "    Remove the cached build",
            ]),
            other(&[
                "  cwd: /workspace/exa...",
                "  Dangerous: recursive delete",
                "  $ rm -rf build/cache",
                "    Remove the cached build",
            ]),
            other(&[
                "  cwd: /workspace/example",
                "  Dangerous: recursive delete",
                "  $ rm -rf build/cache",
                "    Remove the cached build",
                "  $ ls",
            ]),
            screen(
                &BODY,
                [
                    "  ▶ 1. Approve once",
                    "  ▶ 2. Approve for this session",
                    "    3. Reject",
                    "    4. Reject with feedback",
                ],
                HINT_ROW,
            ),
            screen(
                &BODY,
                [
                    "    1. Approve once",
                    "  ▶ 2. Approve for this session  text",
                    "    3. Reject",
                    "    4. Reject with feedback",
                ],
                TYPING_ROW,
            ),
            screen(&BODY, CHOOSING, "  Esc to cancel"),
            screen(
                &BODY,
                [
                    "  ▶ 1. Approve",
                    "    2. Approve for this session",
                    "    3. Reject",
                    "    4. Reject with feedback",
                ],
                HINT_ROW,
            ),
            choosing.replace(RUN, "Approve Bash?"),
            choosing.replace("$ rm", "\x1b[2J$ rm"),
            format!("{choosing}\n{choosing}"),
        ] {
            assert!(read(&wrong, req).is_err(), "{wrong}");
        }
        // A panel taller than its pane loses its top rows.
        let clipped_screen = choosing.lines().skip(4).collect::<Vec<_>>().join("\n");
        assert!(clipped(&clipped_screen) && !complete(&clipped_screen));
        assert_eq!(read(&clipped_screen, req).unwrap_err(), NARROW);
    }

    #[test]
    fn long_commands_must_wrap_exactly_as_kimi_wraps_them() {
        let command = "printf '%s\\n' alpha beta gamma delta epsilon zeta eta theta iota kappa";
        let mut event = request("approval_a", "tool_bash", command);
        event["request"]["display"]
            .as_object_mut()
            .unwrap()
            .remove("description");
        let req = &event["request"];
        // A 50-column panel wraps the command to 46 columns.
        let rule = "─".repeat(50);
        let rows = |second: &str| {
            [
                rule.as_str(),
                "  ▶ Run this command?",
                "",
                "  cwd: /workspace/example",
                "  $ printf '%s\\n' alpha beta gamma delta epsilon",
                second,
                "",
                CHOOSING[0],
                CHOOSING[1],
                CHOOSING[2],
                CHOOSING[3],
                "",
                HINT_ROW,
                rule.as_str(),
            ]
            .join("\n")
        };
        assert_eq!(
            read(&rows("    zeta eta theta iota kappa"), req).unwrap(),
            Shown::Choosing
        );
        assert!(read(&rows("    zeta eta theta iota kappb"), req).is_err());
        assert!(read(&rows("    zeta eta theta iota"), req).is_err());
        // Two spaces at a break would show the same rows: refuse.
        let doubled = &request(
            "approval_a",
            "tool_bash",
            &command.replace("epsilon zeta", "epsilon  zeta"),
        )["request"];
        assert!(read(&rows("    zeta eta theta iota kappa"), doubled).is_err());
        // The cwd does not fit a narrow panel and Kimi would truncate it.
        let mut far = event.clone();
        far["request"]["display"]["cwd"] = json!(format!("/workspace/{}", "d".repeat(50)));
        assert_eq!(
            read(&rows("    zeta eta theta iota kappa"), &far["request"]).unwrap_err(),
            NARROW
        );
    }

    #[test]
    fn only_the_one_waiting_request_of_every_agent_is_bound() {
        let a = request("approval_a", "tool_bash", COMMAND);
        let h = home(std::slice::from_ref(&a), &[]);
        let r = record(h.path());
        let card_a = only(&r, std::slice::from_ref(&a));
        assert_eq!(bound(&r, &card_a).unwrap()["id"], "approval_a");
        // The same first 50 characters, another command: both wait, neither binds.
        let long = format!("rm -rf {}", "x".repeat(60));
        let b1 = request("approval_b1", "tool_bash", &format!("{long}1"));
        let mut b2 = request("approval_b2", "tool_b2", &format!("{long}2"));
        b2["time"] = json!(1_000_600);
        let mut both = r.clone();
        both["active_tools"]["tool_b2"] = json!({"name":"Bash","started":1000.1});
        let h = home(&[b1.clone(), b2.clone()], &[]);
        both["agent_home"] = json!(h.path());
        let cards = recover(&both, &[b1.clone(), b2.clone()]);
        assert_eq!(cards.len(), 2);
        assert_eq!(
            cards[0]["questions"][0]["options"][1]["description"],
            cards[1]["questions"][0]["options"][1]["description"]
        );
        for card in &cards {
            assert!(bound(&both, card)
                .unwrap_err()
                .contains("Another Kimi request"));
        }
        let h = home(&[b1.clone(), b2.clone(), resolved("approval_b2")], &[]);
        both["agent_home"] = json!(h.path());
        assert!(bound(&both, &cards[0]).is_ok());
        // A request from an earlier process is ignored; a recent one without
        // an active tool may be the one Kimi shows.
        let mut stale = request("approval_old", "tool_old", "ls");
        stale["time"] = json!(1000);
        let mut recent = stale.clone();
        recent["time"] = json!(990_000);
        for (other, ok) in [(stale, true), (recent, false)] {
            let h = home(&[other, a.clone()], &[]);
            let mut r = r.clone();
            r["agent_home"] = json!(h.path());
            let card = only(&r, std::slice::from_ref(&a));
            assert_eq!(bound(&r, &card).is_ok(), ok);
        }
        // Another agent's waiting request, unreadable or oversized history,
        // or too many agents: never bound.
        let child = json!({"type":"interaction.request","agentId":"child","id":"approval_c","kind":"approval","toolCallId":"tool_c","time":1_000_700});
        type Agents<'a> = Vec<(&'a str, Vec<Value>)>;
        let cases: Vec<(Agents, bool)> = vec![
            (vec![("child", vec![child.clone()])], false),
            (
                vec![(
                    "child",
                    vec![
                        child.clone(),
                        json!({"type":"interaction.resolved","agentId":"child","id":"approval_c"}),
                    ],
                )],
                true,
            ),
        ];
        for (agents, ok) in cases {
            let h = home(std::slice::from_ref(&a), &agents);
            let mut r = r.clone();
            r["agent_home"] = json!(h.path());
            assert_eq!(bound(&r, &only(&r, std::slice::from_ref(&a))).is_ok(), ok);
        }
        let h = home(std::slice::from_ref(&a), &[("child", vec![])]);
        let agents = h.path().join("sessions/workspace/session_test/agents");
        let mut r = r.clone();
        r["agent_home"] = json!(h.path());
        let card = only(&r, std::slice::from_ref(&a));
        fs::write(
            agents.join("child/wire.jsonl"),
            "{\"type\":\"interaction.request\",\"id\":\"cut",
        )
        .unwrap();
        assert!(bound(&r, &card).is_err());
        fs::write(
            agents.join("child/wire.jsonl"),
            vec![b' '; MAX_AGENT_BYTES as usize + 1],
        )
        .unwrap();
        assert!(bound(&r, &card).is_err());
        fs::write(agents.join("child/wire.jsonl"), "").unwrap();
        assert!(bound(&r, &card).is_ok());
        for index in 0..MAX_AGENTS {
            fs::create_dir_all(agents.join(format!("agent_{index}"))).unwrap();
        }
        assert!(bound(&r, &card).is_err());
    }

    #[test]
    fn a_resolved_request_never_binds_an_identical_new_one() {
        let a = request("approval_a", "tool_bash", COMMAND);
        let h = home(std::slice::from_ref(&a), &[]);
        let r = record(h.path());
        let old = only(&r, std::slice::from_ref(&a));
        // Kimi answered it in Terminal and asks the same command again.
        let again = request("approval_a2", "tool_bash", COMMAND);
        let h = home(&[a.clone(), resolved("approval_a"), again.clone()], &[]);
        let mut r = r.clone();
        r["agent_home"] = json!(h.path());
        assert!(bound(&r, &old).unwrap_err().contains("no longer pending"));
        let new = only(&r, &[a, resolved("approval_a"), again]);
        assert_ne!(new["question_hash"], old["question_hash"]);
        assert_eq!(bound(&r, &new).unwrap()["id"], "approval_a2");
        // Another process of the same run: the hash no longer matches.
        let mut restarted = r.clone();
        restarted["pid"] = json!(43);
        assert!(bound(&restarted, &new).unwrap_err().contains("changed"));
    }

    #[test]
    fn a_truncated_history_must_reach_back_to_the_oldest_active_tool() {
        let a = request("approval_a", "tool_bash", COMMAND);
        let h = home(std::slice::from_ref(&a), &[]);
        let wire = h
            .path()
            .join("sessions/workspace/session_test/agents/main/wire.jsonl");
        let filler = format!(
            "{}\n",
            json!({"type":"context.append_message","time":900_000,"text":"x".repeat(1000)})
        );
        let mut text = filler.repeat((MAX_MAIN_BYTES as usize / filler.len()) + 2);
        text.push_str(&format!("{a}\n"));
        fs::write(&wire, &text).unwrap();
        let r = record(h.path());
        let card = only(&r, std::slice::from_ref(&a));
        assert!(bound(&r, &card).is_ok());
        let late = text.replace("\"time\":900000", "\"time\":999999");
        fs::write(&wire, late).unwrap();
        assert!(bound(&r, &card).unwrap_err().contains("only Kimi request"));
    }

    struct Fixture {
        screens: Vec<String>,
        keys: Vec<String>,
        pastes: Vec<String>,
        response: Option<Value>,
    }
    impl Transport for Fixture {
        fn screen(&mut self) -> Result<String> {
            Ok(self.screens[0].clone())
        }
        fn key(&mut self, key: &str) -> Result<()> {
            self.keys.push(key.into());
            if self.screens.len() > 1 {
                self.screens.remove(0);
            }
            Ok(())
        }
        fn paste(&mut self, text: &str) -> Result<()> {
            self.pastes.push(text.into());
            if self.screens.len() > 1 {
                self.screens.remove(0);
            }
            Ok(())
        }
        fn resolved(&mut self) -> Result<Option<Value>> {
            Ok(self.response.clone().filter(|_| self.screens.len() == 1))
        }
        fn pause(&mut self) {}
    }

    #[test]
    fn every_write_is_checked_first_and_one_key_chooses() {
        let home = tempfile::tempdir().unwrap();
        let event = request("approval_a", "tool_bash", COMMAND);
        let card = only(&record(home.path()), std::slice::from_ref(&event));
        let req = &event["request"];
        let choosing = screen(&BODY, CHOOSING, HINT_ROW);
        let pick =
            |id: &str| json!([{"question_id":"approval","selected_option_ids":[id],"text":""}]);
        for (id, key, response) in [
            ("1", "1", json!({"decision":"approved"})),
            ("2", "2", json!({"decision":"approved","scope":"session"})),
            ("3", "3", json!({"decision":"rejected"})),
        ] {
            let mut io = Fixture {
                screens: vec![choosing.clone(), String::new()],
                keys: vec![],
                pastes: vec![],
                response: Some(response),
            };
            let mut checks = 0;
            drive(
                &mut io,
                &mut || {
                    checks += 1;
                    Ok(())
                },
                req,
                &card,
                &pick(id),
            )
            .unwrap();
            assert_eq!((io.keys, checks), (vec![key.to_owned()], 1));
        }
        let feedback = "use a dry run";
        let typing = [
            "    1. Approve once",
            "    2. Approve for this session",
            "    3. Reject",
            "  ▶ 4. Reject with feedback",
        ];
        let typed = [
            "    1. Approve once",
            "    2. Approve for this session",
            "    3. Reject",
            "  ▶ 4. Reject with feedback  use a dry run",
        ];
        let screens = vec![
            choosing.clone(),
            screen(&BODY, typing, TYPING_ROW),
            screen(&BODY, typed, TYPING_ROW),
            String::new(),
        ];
        let answer = json!([{"question_id":"approval","selected_option_ids":[],"text":feedback}]);
        let mut io = Fixture {
            screens: screens.clone(),
            keys: vec![],
            pastes: vec![],
            response: Some(json!({"decision":"rejected","feedback":feedback})),
        };
        let mut checks = 0;
        drive(
            &mut io,
            &mut || {
                checks += 1;
                Ok(())
            },
            req,
            &card,
            &answer,
        )
        .unwrap();
        assert_eq!(
            (io.keys, io.pastes, checks),
            (
                vec!["4".to_owned(), "Enter".into()],
                vec![feedback.to_owned()],
                3
            )
        );
        // A failed check stops before the next write.
        let mut io = Fixture {
            screens,
            keys: vec![],
            pastes: vec![],
            response: None,
        };
        let mut checks = 0;
        let result = drive(
            &mut io,
            &mut || {
                checks += 1;
                if checks == 2 {
                    Err("This approval changed; refresh Activity".into())
                } else {
                    Ok(())
                }
            },
            req,
            &card,
            &answer,
        );
        assert!(result.is_err());
        assert_eq!((io.keys, io.pastes.len()), (vec!["4".to_owned()], 0));
        // No write at all unless the chooser waits for a choice.
        let mut io = Fixture {
            screens: vec![screen(&BODY, typing, TYPING_ROW)],
            keys: vec![],
            pastes: vec![],
            response: None,
        };
        assert!(drive(&mut io, &mut || Ok(()), req, &card, &pick("1")).is_err());
        assert!(io.keys.is_empty());
        let mut io = Fixture {
            screens: vec![choosing.clone()],
            keys: vec![],
            pastes: vec![],
            response: None,
        };
        assert!(drive(
            &mut io,
            &mut || Err("Another Kimi request is waiting. Answer in Terminal.".into()),
            req,
            &card,
            &pick("1")
        )
        .is_err());
        assert!(io.keys.is_empty());
        // A different recorded decision is reported, never accepted or retried.
        let mut io = Fixture {
            screens: vec![choosing, String::new()],
            keys: vec![],
            pastes: vec![],
            response: Some(json!({"decision":"approved","scope":"session"})),
        };
        assert!(drive(&mut io, &mut || Ok(()), req, &card, &pick("1")).is_err());
        assert_eq!(io.keys, ["1"]);
    }

    #[test]
    fn the_acknowledgement_must_be_exactly_the_decision() {
        let home = tempfile::tempdir().unwrap();
        let card = only(
            &record(home.path()),
            &[request("approval_a", "tool_bash", COMMAND)],
        );
        let pick =
            |id: &str| json!([{"question_id":"approval","selected_option_ids":[id],"text":""}]);
        let feedback =
            json!([{"question_id":"approval","selected_option_ids":[],"text":"use a dry run"}]);
        let resolved = |response: Value| json!({"type":"interaction.resolved","response":response});
        assert!(response_matches(
            &card,
            &pick("1"),
            &resolved(json!({"decision":"approved"}))
        ));
        assert!(!response_matches(
            &card,
            &pick("1"),
            &resolved(json!({"decision":"approved","scope":"session"}))
        ));
        assert!(!response_matches(
            &card,
            &pick("1"),
            &resolved(json!({"decision":"approved","selectedLabel":"Approve once"}))
        ));
        assert!(response_matches(
            &card,
            &pick("2"),
            &resolved(json!({"decision":"approved","scope":"session"}))
        ));
        assert!(!response_matches(
            &card,
            &pick("2"),
            &resolved(json!({"decision":"approved"}))
        ));
        assert!(response_matches(
            &card,
            &pick("3"),
            &resolved(json!({"decision":"rejected"}))
        ));
        assert!(!response_matches(
            &card,
            &pick("3"),
            &resolved(json!({"decision":"cancelled"}))
        ));
        assert!(response_matches(
            &card,
            &feedback,
            &resolved(json!({"decision":"rejected","feedback":"use a dry run"}))
        ));
        assert!(!response_matches(
            &card,
            &feedback,
            &resolved(json!({"decision":"rejected","feedback":"use a dry"}))
        ));
        assert!(!response_matches(
            &card,
            &feedback,
            &resolved(json!({"decision":"rejected"}))
        ));
        assert!(!response_matches(
            &card,
            &pick("4"),
            &resolved(json!({"decision":"rejected"}))
        ));
        let logged = journal_response(
            &card,
            &json!({"time":5000,"response":{"decision":"rejected","feedback":"use a dry run"}}),
        );
        assert_eq!(
            logged["response"]["answers"][RUN],
            "Reject with feedback: use a dry run"
        );
        let logged = journal_response(
            &card,
            &json!({"time":5000,"response":{"decision":"approved","scope":"session"}}),
        );
        assert_eq!(
            logged["response"]["answers"][RUN],
            "Approve for this session"
        );
    }
}
