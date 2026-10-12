//! Deliberate, identity-checked input to an existing interactive agent.
//! A receipt acknowledges terminal submission, never a model response. Once input
//! may have reached the PTY we retain its request ID and never automatically retry.
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::fd::AsRawFd;
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::process::{Command, Stdio};
use std::time::Duration;

const MAX_TEXT: usize = 64 * 1024;
const MAX_FILE: usize = 10 * 1024 * 1024;
const MAX_TOTAL: usize = 20 * 1024 * 1024;
const MAX_JSON: usize = 29 * 1024 * 1024;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Attachment {
    name: String,
    mime: String,
    data_base64: String,
    #[serde(default)]
    reference: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    request_id: String,
    text: String,
    expected_run_id: String,
    #[serde(default)]
    expected_conversation_id: String,
    #[serde(default)]
    attachments: Vec<Attachment>,
    #[serde(default)]
    expected_compaction_id: String,
}

fn validate(request: &Request) -> Result<Vec<Vec<u8>>> {
    uuid::Uuid::parse_str(&request.request_id).map_err(|_| "request_id must be a UUID")?;
    if request.expected_run_id.is_empty() || request.expected_run_id.len() > 128 {
        return Err("expected_run_id is required; refresh the session first".into());
    }
    if request.text.len() > MAX_TEXT {
        return Err("message exceeds 64 KiB".into());
    }
    if request
        .text
        .chars()
        .any(|c| c.is_control() && c != '\n' && c != '\t')
    {
        return Err("message contains terminal control characters".into());
    }
    // The composer sends messages, not provider slash commands or shell mode.
    if request.text.trim_start().starts_with(['/', '!']) {
        return Err("use Terminal for slash commands and shell commands".into());
    }
    if request.attachments.len() > 8 {
        return Err("at most 8 attachments are allowed".into());
    }
    if request.text.trim().is_empty() && request.attachments.is_empty() {
        return Err("message is empty".into());
    }
    let mut total = 0;
    attachments::validate_references(
        &request
            .attachments
            .iter()
            .map(|f| f.reference.as_str())
            .collect::<Vec<_>>(),
    )?;
    request
        .attachments
        .iter()
        .map(|attachment| {
            if attachment.name.is_empty()
                || attachment.name.len() > 255
                || attachment.name.chars().any(char::is_control)
                || attachment.mime.len() > 100
                || attachment.mime.chars().any(char::is_control)
            {
                return Err("invalid attachment metadata".into());
            }
            if attachment.data_base64.len() > MAX_FILE.div_ceil(3) * 4 {
                return Err("attachment exceeds 10 MiB".into());
            }
            let bytes = STANDARD
                .decode(&attachment.data_base64)
                .map_err(|_| "invalid attachment base64")?;
            if bytes.is_empty() || bytes.len() > MAX_FILE {
                return Err("attachments must be nonempty and at most 10 MiB".into());
            }
            total += bytes.len();
            if total > MAX_TOTAL {
                return Err("attachments exceed 20 MiB in total".into());
            }
            Ok(bytes)
        })
        .collect()
}

/// Provider TUIs can defer SessionStart until their first submitted turn.
/// Only supervised fresh runs or verifiable native Codex forks are candidates;
/// checked_input separately proves a fork's owned child history before delivery.
pub(super) fn first_message_candidate(record: &Value) -> bool {
    record["run_identity_version"] == 1
        && record["supervisor"].is_object()
        && AGENTS.contains(&string(record, "agent"))
        && !string(record, "run_id").is_empty()
        && string(record, "conversation_id").is_empty()
        && string(record, "expected_id").is_empty()
        && string(record, "requested_id").is_empty()
        && (string(record, "fork_parent_id").is_empty() || resume_input::fork_pending(record))
        && string(record, "error").is_empty()
        && record.get("session_end").is_none()
        && record["last_event_at"].as_f64().unwrap_or(0.0) == 0.0
        && record["input_pending_at"].as_f64().unwrap_or(0.0) == 0.0
        && ["", "unknown", "idle"].contains(&string(record, "activity"))
        && ["", "unknown", "idle"].contains(&string(record, "phase"))
        && record["startup_kind"] != "resume"
}

pub(super) fn first_message_summary(record: &Value, live_pane: bool) -> Value {
    if !string(record, "conversation_id").is_empty() {
        return json!({"first_message_can_send":false});
    }
    let reason = if record["input_pending_at"].as_f64().unwrap_or(0.0) > 0.0 {
        "Your first message was sent. Waiting for the agent to confirm this conversation."
            .to_owned()
    } else if !first_message_candidate(record) {
        "Waiting for the agent to confirm the requested conversation. Open Terminal to finish startup.".to_owned()
    } else if !live_pane {
        "The agent is not running. Open Terminal to finish startup.".to_owned()
    } else {
        checked_input(string(record, "name"), string(record, "run_id"), "", true)
            .and_then(|r| checked_terminal(&r, true))
            .err()
            .unwrap_or_default()
    };
    json!({"first_message_can_send":reason.is_empty(),"first_message_reason":reason})
}

fn checked_record(name: &str, request: &Request) -> Result<Value> {
    let record = checked_input(
        name,
        &request.expected_run_id,
        &request.expected_conversation_id,
        true,
    )?;
    compact_context::check_continuation(&record, &request.expected_compaction_id)?;
    Ok(record)
}

/// The checks `submit` makes before it writes anything, for callers that must
/// refuse before recording their own intent. `submit` checks again.
pub(super) fn preflight(name: &str, run_id: &str, conversation_id: &str) -> Result<()> {
    let record = checked_input(name, run_id, conversation_id, true)?;
    compact_context::check_continuation(&record, "")?;
    checked_terminal(&record, true)
}

pub(super) fn checked_ready(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    checked_input(name, run_id, conversation_id, false)
}

/// Session settings may precede the first user message, when Codex/Kimi have
/// not created a conversation yet. Other idle-only actions keep checked_ready.
pub(super) fn checked_settings(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    if !conversation_id.is_empty() { return checked_ready(name, run_id, conversation_id); }
    let record = checked_input(name, run_id, conversation_id, true)?;
    if !first_message_candidate(&record)
        || telemetry::grouped_active(&record) > 0
        || record["active_tools"].as_object().is_some_and(|tools| !tools.is_empty())
        || record["subagents"].as_object().is_some_and(|children| children.values().any(|child| string(child, "state") == "working"))
    {
        return Err("The new session is not ready for model settings; refresh or open Terminal".into());
    }
    Ok(record)
}

/// Failed and interrupted turns may already accept another message at the native
/// composer. Verify readiness without clearing their recorded outcome.
pub(super) fn attention_message_summary(record: &Value, output: &mut Value, live_pane: bool) {
    let prefix = if output["phase"] == "error" && output["provider_error"].is_object() {
        "error"
    } else if output["phase"] == "interrupted" {
        "interrupted"
    } else {
        return;
    };
    let result = if live_pane {
        checked_input(
            string(record, "name"),
            string(record, "run_id"),
            string(record, "conversation_id"),
            true,
        )
        .and_then(|current| checked_terminal(&current, true))
    } else {
        Err("The agent is not running. Resume it before sending.".into())
    };
    output[format!("{prefix}_message_can_send")] = json!(result.is_ok());
    output[format!("{prefix}_message_reason")] = json!(result.err().unwrap_or_default());
}

fn checked_input(
    name: &str,
    run_id: &str,
    conversation_id: &str,
    allow_working: bool,
) -> Result<Value> {
    let record = read(name)?;
    if string(&record, "run_id") != run_id || string(&record, "conversation_id") != conversation_id
    {
        return Err("session identity changed; refresh before sending".into());
    }
    if !matches(&record, live()?.get(name)) || !process_alive(&record) {
        return Err("the exact agent process is no longer running; resume it explicitly".into());
    }
    if claude_resume::current(&record).is_some() {
        return Err("Claude is waiting for a conversation resume choice. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if claude_permission::current(&record).is_some() {
        return Err("Claude is waiting for a permission mode choice. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if claude_trust::current(&record).is_some() {
        return Err("Claude is waiting for folder trust approval. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if codex_trust::current(&record).is_some() {
        return Err("Codex is waiting for folder trust approval. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if codex_hooks_trust::current(&record).is_some() {
        return Err("Codex is waiting for hook trust review. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if codex_update::current(&record).is_some() {
        return Err("Codex is waiting for an update choice. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if kimi_trust::current(&record).is_some() {
        return Err("Kimi is waiting for folder trust approval. Choose an option in Activity or Terminal before sending a message.".into());
    }
    let first_message =
        allow_working && conversation_id.is_empty() && first_message_candidate(&record);
    if first_message && !string(&record, "fork_parent_id").is_empty() {
        resume_input::fork_ready(&record)?;
    }
    let resumed_message = allow_working && resume_input::pending(&record);
    if resumed_message {
        resume_input::ready(&record)?;
    }
    if (string(&record, "conversation_id").is_empty() && !first_message)
        || (!string(&record, "expected_id").is_empty() && !resumed_message)
        || !string(&record, "error").is_empty()
        || pause_active(&record)
    {
        return Err(
            "agent identity is unconfirmed or the session is pausing; open Terminal".into(),
        );
    }
    if kimi_cache_hint::current(&record).is_some() {
        return Err("Kimi is waiting for your context choice. Choose an option in Activity or Terminal before sending another message.".into());
    }
    let ready = string(&record, "activity") == "idle" && string(&record, "phase") == "idle";
    let working = string(&record, "activity") == "busy"
        && ["idle", "working", "tool", "compacting"].contains(&string(&record, "phase"));
    let provider_failed = record["phase"] == "error" && record["provider_error"].is_object();
    let interrupted = record["phase"] == "interrupted";
    if allow_working && !ready && !working && !provider_failed && !interrupted && !first_message && !resumed_message
    {
        return Err(
            "agent is waiting for input, approval, or has an unknown state; open Terminal".into(),
        );
    }
    // Text can be submitted to a working agent's native composer. The terminal
    // checks below still require an empty recognized prompt, never a dialog.
    // Settings changes continue to require an entirely idle session.
    if !allow_working
        && (string(&record, "activity") != "idle"
            || string(&record, "phase") != "idle"
            || telemetry::grouped_active(&record) > 0
            || record["active_tools"]
                .as_object()
                .is_some_and(|tools| !tools.is_empty())
            || record["subagents"].as_object().is_some_and(|children| {
                children
                    .values()
                    .any(|child| string(child, "state") == "working")
            }))
    {
        return Err("agent is busy or waiting for approval; wait for Ready or use Terminal".into());
    }
    if record["input_pending_at"]
        .as_f64()
        .is_some_and(|pending| record["last_event_at"].as_f64().unwrap_or(0.0) <= pending)
    {
        return Err("previous input has not been confirmed by the agent; inspect Terminal before sending again".into());
    }
    Ok(record)
}

/// Capture ANSI attributes so a faint Codex placeholder is distinguishable from
/// a user's existing draft. Never clear/edit the terminal's current draft.
#[derive(Default)]
struct TerminalStyle {
    dim: bool,
    background: bool,
}

fn styled_cells(row: &str, style: &mut TerminalStyle) -> Vec<(char, bool, bool)> {
    let mut chars = row.chars().peekable();
    let mut result = Vec::new();
    while let Some(ch) = chars.next() {
        if ch == '\x1b' && chars.peek() == Some(&'[') {
            chars.next();
            let mut code = String::new();
            for ch in chars.by_ref() {
                if ch.is_ascii_alphabetic() {
                    if ch == 'm' {
                        // Extended color components are data, not SGR instructions.
                        let parameters: Vec<_> = code.split(';').collect();
                        let mut i = 0;
                        while i < parameters.len() {
                            match parameters[i] {
                                "" | "0" => {
                                    style.dim = false;
                                    style.background = false;
                                }
                                "22" => style.dim = false,
                                "2" => style.dim = true,
                                "49" => style.background = false,
                                "38" | "48" | "58" => {
                                    if parameters[i] == "48" {
                                        style.background = true;
                                    }
                                    i += if parameters.get(i + 1) == Some(&"2") {
                                        4
                                    } else {
                                        2
                                    };
                                }
                                code if code.parse::<u8>().is_ok_and(|code| {
                                    (40..=47).contains(&code) || (100..=107).contains(&code)
                                }) =>
                                {
                                    style.background = true
                                }
                                _ => {}
                            }
                            i += 1;
                        }
                    }
                    break;
                }
                code.push(ch);
            }
        } else {
            result.push((ch, style.dim, style.background));
        }
    }
    result
}

fn styled_row(row: &str) -> Vec<(char, bool)> {
    styled_cells(row, &mut TerminalStyle::default())
        .into_iter()
        .map(|(ch, dim, _)| (ch, dim))
        .collect()
}

fn prompt_empty(agent: &str, row: &str, previous: &str, screen: &str, cursor: usize) -> bool {
    let styled = styled_row(row);
    let plain: String = styled.iter().map(|(ch, _)| ch).collect();
    let previous: String = styled_row(previous).iter().map(|(ch, _)| ch).collect();
    match agent {
        "codex" => {
            cursor == 2
                && (plain == "›" || plain.starts_with("› "))
                && styled
                    .iter()
                    .skip(2)
                    .all(|(ch, dim)| ch.is_whitespace() || *dim)
        }
        "claude" => {
            // Native Claude uses a non-breaking space after its prompt glyph.
            let mut chars = plain.chars();
            cursor == 2
                && matches!(chars.next(), Some('❯' | '>'))
                && chars.next().is_none_or(char::is_whitespace)
                && styled.iter().skip(2).all(|(ch, dim)| ch.is_whitespace() || *dim)
        }
        "kimi" => {
            // kimi-code 2.x uses a boxed > prompt; its shell mode uses !.
            if cursor == 5
                && plain.starts_with(" │ > ")
                && previous.trim_start().starts_with('╭')
                && plain.chars().skip(5).collect::<String>().trim() == "│"
            {
                return true;
            }
            let recent: Vec<_> = screen.lines().rev().take(5).collect();
            let agent_mode = recent.iter().any(|line| {
                let plain: String = styled_row(line).iter().map(|(ch, _)| ch).collect();
                let words: Vec<_> = plain.split_whitespace().collect();
                words.iter().take(4).any(|word| *word == "agent")
                    && !words.iter().take(4).any(|word| *word == "shell")
            });
            // The older Python kimi-cli uses an unboxed panel and mode footer.
            agent_mode && (cursor == 1 && plain.trim().is_empty() && previous.contains(" input "))
        }
        _ => false,
    }
}

/// Cursor at the start is not evidence that the entire composer is empty: a
/// multiline draft can continue underneath it. Scan until the provider's input
/// boundary, preserving ANSI state across rows as tmux capture-pane does.
pub(super) fn composer_empty(agent: &str, screen: &str, cursor_x: usize, cursor_y: usize) -> bool {
    let lines: Vec<_> = screen.lines().collect();
    let Some(row) = lines.get(cursor_y) else {
        return false;
    };
    let previous = cursor_y
        .checked_sub(1)
        .and_then(|y| lines.get(y))
        .copied()
        .unwrap_or("");
    if !prompt_empty(agent, row, previous, screen, cursor_x) {
        return false;
    }
    let placeholder = styled_row(row)
        .iter()
        .skip(cursor_x)
        .any(|(ch, dim)| !ch.is_whitespace() && *dim);
    let mut style = TerminalStyle::default();
    let mut input_background = false;
    for (index, row) in lines.iter().enumerate().take(cursor_y + 1) {
        let cells = styled_cells(row, &mut style);
        if index == cursor_y {
            input_background = style.background
                || cells
                    .iter()
                    .skip(cursor_x)
                    .any(|(_, _, background)| *background);
        }
    }
    let boxed_kimi = agent == "kimi" && cursor_x == 5;
    let mut previous_blank = false;
    for (index, row) in lines.iter().enumerate().skip(cursor_y + 1) {
        let cells = styled_cells(row, &mut style);
        let plain: String = cells.iter().map(|(ch, _, _)| ch).collect();
        if boxed_kimi {
            // Input text always stays inside │…│; the separate bottom border
            // cannot be confused with a continuation beginning with spaces.
            if plain.starts_with(" ╰")
                && plain.trim_end().ends_with('╯')
                && plain
                    .chars()
                    .all(|ch| ch.is_whitespace() || "╰╯─".contains(ch))
            {
                return true;
            }
            if !plain.starts_with(" │") || plain.chars().skip(2).collect::<String>().trim() != "│"
            {
                return false;
            }
            continue;
        }
        if plain.trim().is_empty() {
            previous_blank = true;
            continue;
        }
        // Claude and Python kimi-cli terminate the input with an unindented
        // full-width separator. Draft continuation lines have an input indent.
        if matches!(agent, "claude" | "kimi")
            && !plain.starts_with(char::is_whitespace)
            && plain.chars().count() >= 12
            && plain.chars().all(|ch| "─━╌".contains(ch))
        {
            return true;
        }
        if agent == "codex"
            && (input_background || placeholder)
            && previous_blank
            && cells
                .iter()
                .filter(|(ch, _, _)| !ch.is_whitespace())
                .all(|(_, _, bg)| !bg)
            && index + 2 >= lines.len()
        {
            // Codex's shaded composer ends at its unshaded model/help footer.
            // Unknown layouts fail closed instead of treating arbitrary text
            // as status. A configured footer can always use the Terminal tab.
            let footer = plain.to_ascii_lowercase();
            if ["gpt-", "context", "tokens", "? for shortcuts", "%"]
                .iter()
                .any(|token| footer.contains(token))
            {
                // Codex can render its composer without a shaded background
                // (for example an unfocused pane or a terminal theme). A dim
                // placeholder plus the actual shortcuts footer identifies this
                // layout; an arbitrary blank first line of a draft does not.
                // Codex 0.153 can show only its model/path footer, without
                // shortcuts. Require the dim placeholder and exact one-line
                // model/path shape; drafts never retain that placeholder.
                let single_line_footer = placeholder
                    && lines[index + 1..].iter().all(|line| line.trim().is_empty())
                    && footer.trim_start().starts_with("gpt-")
                    && footer.split_once(" · ").is_some_and(|(_, path)| path.starts_with('/') || path.starts_with("~/"));
                return input_background || single_line_footer
                    || lines.last().is_some_and(|line| {
                        let plain: String = styled_row(line).iter().map(|(ch, _)| ch).collect();
                        plain.trim_start().starts_with("? for shortcuts")
                    });
            }
        }
        return false;
    }
    !boxed_kimi
}

pub(super) fn checked_terminal(record: &Value, empty: bool) -> Result<()> {
    let pane = string(record, "pane");
    if !pane
        .strip_prefix('%')
        .is_some_and(|s| !s.is_empty() && s.chars().all(|ch| ch.is_ascii_digit()))
    {
        return Err("invalid tracked pane".into());
    }
    let output = tmux(&["display-message", "-p", "-t", pane,
        "#{pane_tty}\t#{cursor_x}\t#{cursor_y}\t#{pane_in_mode}\t#{pane_current_command}\t#{pane_dead}\t#{@hgs_run}\t#{session_name}\t#{bracket_paste_flag}"]
        .map(str::to_owned), true)?;
    let text = String::from_utf8_lossy(&output.stdout);
    let fields: Vec<_> = text.trim_end_matches('\n').split('\t').collect();
    if fields.len() != 9
        || fields[3] != "0"
        || fields[5] != "0"
        || fields[6] != string(record, "run_id")
        || fields[7] != string(record, "name")
        || fields[8] != "1"
    {
        return Err("terminal is not in the expected agent input mode; open Terminal".into());
    }
    let tty = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(libc::O_NOCTTY | libc::O_NONBLOCK)
        .open(fields[0])
        .map_err(|e| e.to_string())?;
    let pid = record["pid"].as_u64().ok_or("missing agent process")? as i32;
    let mut attributes: libc::termios = unsafe { std::mem::zeroed() };
    if unsafe { libc::tcgetattr(tty.as_raw_fd(), &mut attributes) } != 0
        || attributes.c_lflag & (libc::ICANON | libc::ECHO) != 0
    {
        return Err("agent does not own an interactive input terminal; open Terminal".into());
    }
    // Ensure the pinned process is attached to this tty, not just a PID sharing a group.
    let tty_output = Command::new("ps")
        .args(["-p", &pid.to_string(), "-o", "tty=,pgid=,tpgid=,comm="])
        .output()
        .map_err(|e| e.to_string())?;
    let tty_text = String::from_utf8_lossy(&tty_output.stdout);
    let process_fields: Vec<_> = tty_text.split_whitespace().collect();
    if process_fields.len() < 4
        || process_fields[0] != fields[0].trim_start_matches("/dev/")
        || process_fields[1] != process_fields[2]
        || !process_fields[2].parse::<i32>().is_ok_and(|id| id > 0)
    {
        return Err("tracked agent is not attached to the selected terminal".into());
    }
    if ["sh", "bash", "zsh", "fish", "dash", "tcsh"].contains(&fields[4])
        && !(string(record, "agent") == "kimi"
            && Path::new(process_fields[3])
                .file_name()
                .is_some_and(|name| name == "kimi"))
    {
        return Err("terminal is not in the expected agent input mode; open Terminal".into());
    }
    if empty {
        let screen = tmux(
            &["capture-pane", "-p", "-e", "-t", pane].map(str::to_owned),
            true,
        )?;
        let screen = String::from_utf8_lossy(&screen.stdout);
        let cursor_y: usize = fields[2].parse().map_err(|_| "invalid terminal cursor")?;
        let cursor_x: usize = fields[1].parse().map_err(|_| "invalid terminal cursor")?;
        if !composer_empty(string(record, "agent"), &screen, cursor_x, cursor_y) {
            return Err("terminal has a draft, dialog, or unrecognized prompt; open Terminal and return to an empty agent prompt".into());
        }
    }
    Ok(())
}

pub(super) fn safe_filename(name: &str, index: usize) -> String {
    let name: String = name
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ['.', '_', '-'].contains(&ch) {
                ch
            } else {
                '_'
            }
        })
        .take(100)
        .collect();
    format!("{:02}-{}", index + 1, name)
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    if args.len() != 2 || args[1] != "--json" {
        return Err("usage: hgs send <session> --json (JSON from stdin)".into());
    }
    let mut input = Vec::new();
    io::stdin()
        .take((MAX_JSON + 1) as u64)
        .read_to_end(&mut input)
        .map_err(|e| e.to_string())?;
    if input.len() > MAX_JSON {
        return Err("message payload exceeds 29 MiB".into());
    }
    let receipt = submit(&args[0], &input, None)?;
    println!("{receipt}");
    Ok(0)
}

/// Whether `submit` stored a receipt for this request ID, that is whether the
/// text may already have been typed.
pub(super) fn receipt_exists(request_id: &str) -> bool {
    let Ok(id) = uuid::Uuid::parse_str(request_id) else { return false };
    absolute_root()
        .map(|root| root.join("input_receipts").join(format!("{id}.json")).exists())
        .unwrap_or(false)
}

pub(super) fn submit(name: &str, input: &[u8], recovery_id: Option<&str>) -> Result<Value> {
    let request: Request =
        serde_json::from_slice(&input).map_err(|e| format!("invalid send payload: {e}"))?;
    let files = validate(&request)?;
    let digest = format!("{:x}", Sha256::digest(&input));
    let request_id = uuid::Uuid::parse_str(&request.request_id)
        .unwrap()
        .to_string();
    let root = absolute_root()?;
    let receipt_path = root
        .join("input_receipts")
        .join(format!("{request_id}.json"));
    let _guard = lock(None)?;
    if receipt_path.exists() {
        let contents = fs::read_to_string(&receipt_path).map_err(|e| e.to_string())?;
        let receipt: Value = serde_json::from_str(&contents).map_err(|e| e.to_string())?;
        if string(&receipt, "digest") != digest || string(&receipt, "name") != name {
            return Err("request ID was already used for different content".into());
        }
        if receipt["status"] == "submitted" {
            // Replay the exact durable acknowledgement. Reformatting parsed
            // floats can round its subsecond timestamp differently.
            return Ok(receipt);
        }
        return Err("delivery uncertain: this request may already be in Terminal; do not resend automatically".into());
    }
    let mut record = effort::apply_pending(name, checked_record(name, &request)?)?;
    if let Some(id) = recovery_id { recovery::check_submission(&record, id)?; }
    checked_terminal(&record, true)?;
    let attachments_root = root.join("attachments");
    private_dir(&attachments_root)?;
    let attachments = tempfile::Builder::new()
        .prefix("message-")
        .tempdir_in(attachments_root)
        .map_err(|e| e.to_string())?;
    fs::set_permissions(attachments.path(), fs::Permissions::from_mode(0o700))
        .map_err(|e| e.to_string())?;
    let mut paths = Vec::new();
    for (index, (attachment, bytes)) in request.attachments.iter().zip(files.iter()).enumerate() {
        let path = attachments
            .path()
            .join(safe_filename(&attachment.name, index));
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(&path)
            .map_err(|e| e.to_string())?;
        file.write_all(bytes).map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
        paths.push(
            json!({"name":attachment.name,"mime":attachment.mime,"path":path,"bytes":bytes.len(),"reference":attachment.reference}),
        );
    }
    let text = attachments::terminal_text(request.text.trim(), &paths);
    // Claude treats a trailing backslash plus the next Enter as line continuation.
    // Paste a final newline with the literal text so our single Enter submits it;
    // never retry Enter, which could answer a subsequently opened dialog.
    let pasted = if string(&record, "agent") == "claude" && text.ends_with('\\') {
        format!("{text}\n")
    } else {
        text.clone()
    };
    let buffer = format!("hgs-input-{request_id}");
    let mut loader = Command::new("tmux")
        .args(["load-buffer", "-b", &buffer, "-"])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;
    let written = loader
        .stdin
        .take()
        .ok_or("missing buffer input")?
        .write_all(pasted.as_bytes());
    let loaded = loader.wait_with_output().map_err(|e| e.to_string())?;
    if written.is_err() || !loaded.status.success() {
        return Err("could not stage terminal input; nothing was submitted".into());
    }
    let cleanup = || {
        let _ = tmux(&["delete-buffer", "-b", &buffer].map(str::to_owned), false);
    };
    // Recheck immediately before the first input byte. All concurrent hgs writes
    // share this lock; a crash/exit can still race the PTY, so receipts are durable.
    if let Err(error) = checked_record(name, &request).and_then(|r| checked_terminal(&r, true)) {
        cleanup();
        return Err(error);
    }
    if let Some(id) = recovery_id { recovery::check_submission(&read(name)?, id)?; }
    let submitted_at = now();
    if recovery_id.is_none() { recovery::cancel_for_input(name)?; }
    let mut receipt = json!({"request_id":request_id,"name":name,"run_id":request.expected_run_id,
        "conversation_id":string(&record,"conversation_id"),"first_message":string(&record,"conversation_id").is_empty(),"digest":digest,"status":"in_progress",
        "submitted_at":submitted_at,"submitted_text":text,"text":request.text.trim(),"attachments":paths,"transport":"terminal"});
    if let Err(error) = atomic(&receipt_path, &receipt.to_string()) {
        cleanup();
        return Err(error);
    }
    // Preserve attachments after any possible input: the agent may read them later.
    let _ = attachments.keep();
    record["input_pending_at"] = json!(submitted_at);
    if let Err(error) = write(&mut record) {
        cleanup();
        return Err(error);
    }
    let pane = string(&record, "pane");
    let result: Result<()> = (|| {
        tmux(
            &["paste-buffer", "-p", "-r", "-d", "-b", &buffer, "-t", pane].map(str::to_owned),
            true,
        )?;
        // Let bracketed-paste handlers finish before Enter. Never send a second
        // Enter as a retry: that could approve a new dialog or duplicate a turn.
        std::thread::sleep(Duration::from_millis(180));
        if !process_alive(&record) || !matches(&record, live()?.get(name)) {
            return Err("agent changed after paste".into());
        }
        checked_terminal(&record, false)?;
        tmux(&["send-keys", "-t", pane, "Enter"].map(str::to_owned), true)?;
        receipt["status"] = json!("submitted");
        atomic(&receipt_path, &receipt.to_string())?;
        Ok(())
    })();
    cleanup();
    if let Err(error) = result {
        return Err(format!(
            "delivery uncertain: {error}; inspect Terminal before retrying"
        ));
    }
    Ok(receipt)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn entire_input_region_must_be_empty() {
        assert!(!composer_empty(
            "kimi",
            " ╭────────────╮\n │ > │        │\n ╰────────────╯",
            5,
            1
        ));
        assert!(!composer_empty(
            "kimi",
            " ╭────────────╮\n │ >          │\n │  │         │\n ╰────────────╯",
            5,
            1
        ));
        assert!(!composer_empty(
            "codex",
            "›\n  existing second line\n",
            2,
            0
        ));
        assert!(!composer_empty(
            "claude",
            "❯\n  existing second line\n──────────────\nstatus",
            2,
            0
        ));
        assert!(composer_empty("claude", "❯\n──────────────\nstatus", 2, 0));
        assert!(!composer_empty(
            "kimi",
            " ╭────╮\n │ >  │\n │ old│\n ╰────╯\nstatus",
            5,
            1
        ));
        assert!(composer_empty(
            "kimi",
            "\x1b[38;2;90;90;90m ╭────╮\n │ >  │\n ╰────╯\nstatus",
            5,
            1
        ));
        let codex = "\x1b[48;2;61;64;64m› \x1b[2mAsk anything\x1b[0m\x1b[48;2;61;64;64m\n\n\x1b[49m  gpt-6-astra · 80% context";
        assert!(composer_empty("codex", codex, 2, 0));
        assert!(!composer_empty(
            "codex",
            &codex.replace("\n\n", "\n  existing\n\n"),
            2,
            0
        ));
        // A color component equal to49 must not reset an explicit RGB background.
        let colored_draft = "\x1b[48;2;61;64;64m› \n\n\x1b[38;2;49;49;49m  gpt-6-astra text";
        assert!(!composer_empty("codex", colored_draft, 2, 0));
    }
    #[test]
    fn claude_nonbreaking_prompt_space_is_empty_but_real_drafts_are_not() {
        // Claude 2.1.289 uses NBSP after the native prompt glyph.
        let screen = "Ready\n\x1b[39m❯\u{a0}\n──────────────\n  auto mode on";
        assert!(composer_empty("claude", screen, 2, 1));
        for replacement in ["❯\u{a0}draft", "❯\u{a0}\n  second line", "❯\u{a0}Yes, trust this folder", "$ "] {
            assert!(!composer_empty("claude", &screen.replace("❯\u{a0}", replacement), 2, 1));
        }
        assert!(!composer_empty("claude", screen, 3, 1));
    }
    #[test]
    fn placeholder_is_not_a_draft_or_dialog() {
        assert!(prompt_empty(
            "codex",
            "\x1b[1m›\x1b[0m \x1b[2mAsk anything\x1b[0m",
            "",
            "",
            2
        ));
        assert!(!prompt_empty("codex", "› do something", "", "", 2));
        assert!(!prompt_empty(
            "codex",
            "› \x1b[38;2;22;2;2mdo something",
            "",
            "",
            2
        ));
        assert!(!prompt_empty("codex", "› approve", "", "", 9));
        assert!(prompt_empty("claude", "❯ ", "", "", 2));
        assert!(!prompt_empty("kimi", "$ ", "", "shell ~/repo", 2));
        assert!(prompt_empty(
            "kimi",
            " ",
            "── input ──",
            "agent (kimi-k2) ~/repo",
            1
        ));
    }
    #[test]
    fn codex_unshaded_composer_with_two_line_status_bar() {
        // Native capture of the unfocused Codex composer; all user content in
        // the status bar is replaced. Colors are optional, not input state.
        let screen = "Working\n\n\x1b[1m›\x1b[0m \x1b[2mAsk Codex to do anything\x1b[0m\n\n  \x1b[38;2;246;226;183mGPT-6-Astra xhigh\x1b[39m · /project · Session title    Pursuing goal (1h)\n  \x1b[1m?\x1b[0m for shortcuts";
        assert!(composer_empty("codex", screen, 2, 2));
        // Real drafts, including a blank first line followed by text resembling
        // a status bar, are still rejected. Never clear or append to them.
        assert!(!composer_empty(
            "codex",
            &screen.replace("\x1b[2mAsk Codex to do anything\x1b[0m", ""),
            2,
            2
        ));
        assert!(!composer_empty(
            "codex",
            &screen.replace("\x1b[2mAsk Codex to do anything\x1b[0m", "a draft"),
            2,
            2
        ));
        assert!(!composer_empty(
            "codex",
            &screen.replace("\n\n  \x1b[38", "\n  draft continuation\n\n  \x1b[38"),
            2,
            2
        ));
        assert!(!composer_empty(
            "codex",
            &screen.replace("?\x1b[0m for shortcuts", "Approve command?"),
            2,
            2
        ));
    }
    #[test]
    fn codex_unshaded_single_line_model_footer() {
        let screen = "Provider error\n\n\x1b[1m›\x1b[0m \x1b[2mAsk Codex to do anything\x1b[0m\n\n  \x1b[38;2;246;226;183mgpt-5.6-sol default\x1b[2m\x1b[39m · \x1b[0m\x1b[38;2;171;223;167m/private/project\n";
        assert!(composer_empty("codex", screen, 2, 2));
        assert!(composer_empty("codex", &screen.replace("/private/project", "~/project         ⚠ 3 warnings · f2 to view"), 2, 2));
        for replacement in ["", "my draft", "\n  second line"] {
            assert!(!composer_empty("codex", &screen.replace("\x1b[2mAsk Codex to do anything\x1b[0m", replacement), 2, 2));
        }
        assert!(!composer_empty("codex", &screen.replace("gpt-5.6-sol default", "Approve command?"), 2, 2));
        assert!(!composer_empty("codex", &(screen.to_owned() + "  more input"), 2, 2));
    }
    #[test]
    fn malicious_terminal_input_and_attachment_paths_are_rejected() {
        let mut r = Request {
            request_id: uuid::Uuid::new_v4().to_string(),
            text: "hello".into(),
            expected_run_id: "run".into(),
            expected_conversation_id: String::new(),
            expected_compaction_id: String::new(),
            attachments: vec![],
        };
        assert!(validate(&r).is_ok());
        for text in ["hi\x1b[201~", "hello\rbye", " /exit", "!rm test"] {
            r.text = text.into();
            assert!(validate(&r).is_err(), "{text:?}");
        }
        assert_eq!(
            safe_filename("../../bad image.png", 0),
            "01-.._.._bad_image.png"
        );
    }
}
