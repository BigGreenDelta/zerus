//! Deliberate, identity-checked input to an existing interactive agent.
//! A receipt acknowledges terminal submission, never a model response. Once input
//! may have reached the PTY we retain its request ID and never automatically retry.
//! Codex's one-line status footer names its folder as `/…` or `~/…`, and with a
//! Windows platform home also as a drive path or `~\…`. Under inbox ConPTY the
//! hidden Claude and Kimi cursor stays at the end of the last painted row, so
//! their empty-composer checks fail closed there (spike S3).
use super::*;
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::Write;
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

pub(super) fn checked_ready(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    checked_input(name, run_id, conversation_id, false)
}

/// A resume before its deferred SessionStart, verified as for its first message.
pub(super) fn checked_resumed(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    let record = checked_input(name, run_id, conversation_id, true)?;
    if !resume_input::clearable(&record) {
        return Err("The agent is no longer waiting for its resumed conversation; refresh".into());
    }
    Ok(record)
}

/// A Claude turn stopped with Esc, whose prompt may take /clear before Ready.
pub(super) fn checked_interrupted(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    let record = checked_input(name, run_id, conversation_id, true)?;
    if !clear_context::stopped(&record) {
        return Err("The session is no longer interrupted; refresh before clearing".into());
    }
    Ok(record)
}

/// Session settings may precede the first user message, when Codex/Kimi have
/// not created a conversation yet. Other idle-only actions keep checked_ready.
pub(super) fn checked_settings(name: &str, run_id: &str, conversation_id: &str) -> Result<Value> {
    if !conversation_id.is_empty() { return checked_ready(name, run_id, conversation_id); }
    let record = checked_input(name, run_id, conversation_id, true)?;
    if !first_message_ready(&record) {
        return Err("The new session is not ready for model settings; refresh or open Terminal".into());
    }
    Ok(record)
}

/// A new session waiting at its first prompt with no work remaining, as the
/// listing reports it; its settings may change before the first message.
pub(super) fn first_message_ready(record: &Value) -> bool {
    first_message_candidate(record) && !telemetry::work_remains(record)
}

/// Settings changes, clear and compaction require an entirely idle session:
/// Ready as the listing reports it, which also gates forks.
fn idle_only(record: &Value) -> Result<()> {
    if telemetry::settled(record) {
        return Ok(());
    }
    Err("agent is busy or waiting for approval; wait for Ready or use Terminal".into())
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
        .and_then(|current| {
            checked_terminal(&current, true).map_err(|error| {
                // Stop returns ordinary Codex follow-ups to its input as one draft.
                if prefix == "interrupted" && error == DRAFT && input_queue::returned_by_stop(&current) {
                    RETURNED_FOLLOW_UPS.into()
                } else {
                    error
                }
            })
        })
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
    if claude_approval::current(&record).is_some() {
        return Err("Claude is waiting for tool or safety approval. Review the request in Activity or Terminal before sending a message.".into());
    }
    if claude_resume::current(&record).is_some() {
        return Err("Claude is waiting for a conversation resume choice. Choose an option in Activity or Terminal before sending a message.".into());
    }
    if claude_usage_limit::current(&record).is_some() {
        return Err("Claude is waiting for a usage-limit choice. Choose an option in Activity or Terminal before sending a message.".into());
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
    if codex_goal_resume::current(&record).is_some() {
        return Err(codex_goal_resume::WAIT.into());
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
    if !allow_working {
        idle_only(&record)?;
    }
    if record["input_pending_at"]
        .as_f64()
        .is_some_and(|pending| record["last_event_at"].as_f64().unwrap_or(0.0) <= pending)
    {
        return Err("previous input has not been confirmed by the agent; inspect Terminal before sending again".into());
    }
    if settings_unconfirmed(&record) {
        return Err(SETTINGS_UNCONFIRMED.into());
    }
    Ok(record)
}

/// Keys of a model or effort change the agent may not have read yet. Unlike
/// sent text it is not a first message, so a new session keeps its first
/// message state; a later native event settles it.
fn settings_unconfirmed(record: &Value) -> bool {
    record["settings_unconfirmed_at"]
        .as_f64()
        .is_some_and(|at| record["last_event_at"].as_f64().unwrap_or(0.0) <= at)
}

const SETTINGS_UNCONFIRMED: &str = "the agent did not react to the last model settings change; finish or cancel it in Terminal, then send a message there to continue";

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
    composer_empty_for_model(agent, screen, cursor_x, cursor_y, None)
}

// A local model has no GPT prefix. Accept its native footer only when the
// model/effort label matches this exact session's confirmed or launch model.
fn codex_model_footer(footer: &str, model: Option<&str>) -> bool {
    let Some(model) = model.filter(|model| !model.is_empty()) else { return false; };
    let label = footer.trim_start().split(" · ").next().unwrap_or("");
    let mut words = label.split_whitespace();
    words.next().is_some_and(|word| word.eq_ignore_ascii_case(model))
        && words.next().is_some_and(|effort| {
            ["default", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"].contains(&effort)
        })
        && words.next().is_none()
}

/// Codex's one-line model/folder status: this session's exact model or a GPT
/// label, then a folder.
fn codex_status_row(plain: &str, model: Option<&str>) -> bool {
    let footer = plain.to_ascii_lowercase();
    (codex_model_footer(plain, model) || footer.trim_start().starts_with("gpt-"))
        && footer.split_once(" · ").is_some_and(|(_, path)| {
            shown_path::status_folder(path, crate::platform::home().as_deref())
        })
}

fn codex_footer_model(record: &Value) -> Option<&str> {
    if let Some(model) = record["model"].as_str().filter(|model| !model.is_empty()) {
        return Some(model);
    }
    // SessionStart can be deferred until the first turn. In that case the
    // saved native launch argv, not an unrelated account/model, is evidence.
    let args = record["base"].as_array()?;
    let mut model = None;
    for (index, arg) in args.iter().enumerate() {
        let Some(arg) = arg.as_str() else { continue; };
        if arg == "--" { break; }
        if ["-m", "--model"].contains(&arg) {
            model = args.get(index + 1).and_then(Value::as_str);
        } else if let Some(value) = arg.strip_prefix("--model=") {
            model = Some(value);
        }
    }
    model
}

fn composer_empty_for_model(agent: &str, screen: &str, cursor_x: usize, cursor_y: usize, model: Option<&str>) -> bool {
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
            let exact_model = codex_model_footer(&plain, model);
            if exact_model || ["gpt-", "context", "tokens", "? for shortcuts", "%"]
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
                // Windows Codex prints `~\…` or a drive path (`shown_path`).
                let single_line_footer = placeholder
                    && lines[index + 1..].iter().all(|line| line.trim().is_empty())
                    && codex_status_row(&plain, model);
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
    let terminal = backend::current();
    let state = terminal.input_state(&backend::InputClaim {
        pane,
        name: string(record, "name"),
        run_id: string(record, "run_id"),
        agent: string(record, "agent"),
        pid: record["pid"].as_u64(),
    })?;
    if empty {
        let screen = terminal.capture(pane, backend::Capture::Styled)?;
        let screen = String::from_utf8_lossy(&screen);
        let cursor_y: usize = state
            .cursor_y
            .parse()
            .map_err(|_| "invalid terminal cursor")?;
        let cursor_x: usize = state
            .cursor_x
            .parse()
            .map_err(|_| "invalid terminal cursor")?;
        if !composer_empty_for_model(
            string(record, "agent"),
            &screen,
            cursor_x,
            cursor_y,
            codex_footer_model(record),
        ) {
            return Err(DRAFT.into());
        }
    }
    Ok(())
}

const DRAFT: &str = "terminal has a draft, dialog, or unrecognized prompt; open Terminal and return to an empty agent prompt";
/// The send-block reason while Codex's input holds exactly the follow-ups Stop returned.
const RETURNED_FOLLOW_UPS: &str =
    "The queued follow-ups are back in Codex's input as an unsent draft. Submit or clear them in Terminal.";

/// The whole draft in Codex's input: its `›` row and indented continuation rows,
/// then a blank row before the model/folder status row of its footer in the last
/// two screen rows. The cursor must be above that blank row; any other layout is unread.
pub(super) fn codex_draft(record: &Value, screen: &str, cursor_y: usize) -> Option<String> {
    let lines: Vec<&str> = screen.lines().map(str::trim_end).collect();
    let model = codex_footer_model(record);
    let footer = (lines.len().saturating_sub(2)..lines.len())
        .find(|&index| codex_status_row(lines[index], model))?;
    let (start, first) = lines[..footer]
        .iter()
        .enumerate()
        .rev()
        .find_map(|(index, row)| Some((index, row.strip_prefix('›')?)))?;
    let rows = &lines[start + 1..footer];
    if !(start..footer - 1).contains(&cursor_y)
        || rows.last().is_none_or(|row| !row.is_empty())
        || rows.iter().any(|row| !row.is_empty() && !row.starts_with("  "))
    {
        return None;
    }
    let first = first.strip_prefix(' ').unwrap_or(first);
    let rows = rows.iter().map(|row| row.get(2..).unwrap_or(""));
    Some(std::iter::once(first).chain(rows).collect::<Vec<_>>().join("\n").trim_end().to_owned())
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

pub(super) fn submit(name: &str, input: &[u8], recovery_id: Option<&str>) -> Result<Value> {
    let request: Request =
        serde_json::from_slice(input).map_err(|e| format!("invalid send payload: {e}"))?;
    let files = validate(&request)?;
    let digest = format!("{:x}", Sha256::digest(input));
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
    crate::platform::restrict_dir(attachments.path()).map_err(|e| e.to_string())?;
    let mut paths = Vec::new();
    for (index, (attachment, bytes)) in request.attachments.iter().zip(files.iter()).enumerate() {
        let path = attachments
            .path()
            .join(safe_filename(&attachment.name, index));
        let mut file = crate::platform::private_open(OpenOptions::new().create_new(true).write(true))
            .open(&path)
            .map_err(|e| e.to_string())?;
        file.write_all(bytes).map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
        paths.push(
            json!({"name":attachment.name,"mime":attachment.mime,"path":path,"bytes":bytes.len(),"reference":attachment.reference}),
        );
    }
    let text = attachments::terminal_text(request.text.trim(), &paths);
    // Claude and Kimi treat a trailing backslash plus the next Enter as line
    // continuation. Paste a final newline with the literal text so our single
    // Enter submits it; never retry Enter, which could answer a subsequently
    // opened dialog.
    let pasted = if matches!(string(&record, "agent"), "claude" | "kimi") && text.ends_with('\\') {
        format!("{text}\n")
    } else {
        text.clone()
    };
    let buffer = format!("hgs-input-{request_id}");
    let terminal = backend::current();
    if !terminal.stage(&buffer, pasted.as_bytes(), backend::StageErrors::Read)? {
        return Err("could not stage terminal input; nothing was submitted".into());
    }
    let cleanup = || terminal.discard(&buffer);
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
    if let Some(target) = session_graph::target_receipt(&record) {
        receipt["graph_target"] = target;
    }
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
        terminal.paste(&buffer, pane, backend::Paste::Composer)?;
        // Let bracketed-paste handlers finish before Enter. Never send a second
        // Enter as a retry: that could approve a new dialog or duplicate a turn.
        std::thread::sleep(Duration::from_millis(180));
        #[cfg(windows)]
        if string(&record, "agent") == "codex" {
            settle_codex_paste(terminal, pane, &pasted);
        }
        if !process_alive(&record) || !matches(&record, live()?.get(name)) {
            return Err("agent changed after paste".into());
        }
        checked_terminal(&record, false)?;
        terminal.send(pane, &[backend::Key::Named("Enter")])?;
        receipt["status"] = json!("submitted");
        atomic(&receipt_path, &receipt.to_string())?;
        Ok(())
    })();
    cleanup();
    if let Err(error) = result {
        crate::analytics::adapters::delivery(&root, &record, &receipt, crate::analytics::adapters::Delivery::Uncertain);
        return Err(format!(
            "delivery uncertain: {error}; inspect Terminal before retrying"
        ));
    }
    crate::analytics::adapters::delivery(&root, &record, &receipt, crate::analytics::adapters::Delivery::Submitted);
    Ok(receipt)
}

/// Codex on Windows reads console key events, not VT input, so the pseudoconsole
/// turns a bracketed paste into typed keys, and Codex's paste-burst handling makes
/// an Enter within 120 ms of the last of them a newline. Wait until the composer
/// shows the paste (or Codex's placeholder for a long one), then past that
/// window; when it never shows within the limit, Enter goes as before.
#[cfg(windows)]
fn settle_codex_paste(terminal: &dyn backend::TerminalBackend, pane: &str, pasted: &str) {
    let limit = std::time::Instant::now()
        + Duration::from_millis(2_000 + 4 * pasted.len().min(4_000) as u64);
    while std::time::Instant::now() < limit {
        let shown = terminal
            .capture(pane, backend::Capture::Plain)
            .is_ok_and(|screen| paste_shown(&String::from_utf8_lossy(&screen), pasted));
        if shown {
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    std::thread::sleep(Duration::from_millis(200));
}

/// Whether `screen` shows all of `pasted` (wrapping, indentation and borders
/// aside), only its end when it is too long to fit, or Codex's placeholder for a
/// long paste. The whole text counts, as a reply's closing tag repeats its opening.
#[cfg_attr(not(windows), allow(dead_code))]
fn paste_shown(screen: &str, pasted: &str) -> bool {
    let squeeze = |text: &str| -> String {
        text.chars()
            .filter(|ch| !ch.is_whitespace() && !matches!(ch, '│' | '┃' | '|'))
            .collect()
    };
    if screen.contains("[Pasted Content") {
        return true;
    }
    let pasted: Vec<char> = squeeze(pasted).chars().collect();
    let shown: String = pasted[pasted.len().saturating_sub(2_000)..].iter().collect();
    !shown.is_empty() && squeeze(screen).contains(&shown)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unread_settings_keys_wait_for_a_native_event() {
        let mut record = json!({"settings_unconfirmed_at": 10.0, "last_event_at": 9.0});
        assert!(settings_unconfirmed(&record));
        record["last_event_at"] = json!(10.0);
        assert!(settings_unconfirmed(&record));
        record["last_event_at"] = json!(10.5);
        assert!(!settings_unconfirmed(&record));
        assert!(!settings_unconfirmed(&json!({"last_event_at": 0.0})));
    }

    #[test]
    fn idle_only_dispatch_and_new_session_settings_follow_the_listing() {
        // Clear, compaction and settings dispatch accept the Ready session the
        // listing reports, though a replaced Kimi hook entry still says working.
        let first = |record: &Value| {
            let mut record = record.clone();
            record["conversation_id"] = json!("");
            record["last_event_at"] = json!(0);
            record["run_identity_version"] = json!(1);
            record["supervisor"] = json!({});
            record
        };
        let stale = telemetry::stale_hook_child();
        assert!(idle_only(&stale).is_ok());
        assert!(first_message_candidate(&first(&stale)) && first_message_ready(&first(&stale)));
        for (case, record) in telemetry::remaining_work() {
            assert_eq!(idle_only(&record).unwrap_err(),
                "agent is busy or waiting for approval; wait for Ready or use Terminal", "{case}");
            assert!(!first_message_ready(&first(&record)), "{case}");
        }
    }
    #[test]
    fn a_paste_shows_by_its_end_through_wrapping_or_as_a_placeholder() {
        let envelope = "<send_user_message_question_reply>\n[{\"answer\":\"Two\",\"question\":\"Which option?\"}]\n</send_user_message_question_reply>";
        // Codex wraps the composer's rows; the end still reads through.
        let screen = "› <send_user_message_question_reply>\n  [{\"answer\":\"Two\",\"quest\n  ion\":\"Which option?\"}]\n  </send_user_message_question_reply>\n\n  GPT default · ~\\project";
        assert!(paste_shown(screen, envelope));
        // Only the start so far: not shown yet.
        assert!(!paste_shown("› <send_user_message_question_reply>\n  [{\"answer\":", envelope));
        assert!(paste_shown("› [Pasted Content 4096 chars]", &"x".repeat(4096)));
        assert!(!paste_shown("› ", ""));
    }
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
    fn codex_local_model_footer_uses_exact_session_model() {
        // Released Codex 0.162.1 with an Ollama model, dim empty placeholder,
        // unshaded composer and its ordinary model/path footer.
        let screen = "Ready\n\n\x1b[1m›\x1b[0m \x1b[2mAsk Codex to do anything\x1b[0m\n\n  \x1b[38;2;246;226;183mqwen3-coder:30b default\x1b[39m · \x1b[38;2;171;223;167m/project\x1b[39m\n  \x1b[1m?\x1b[0m for shortcuts";
        let model = Some("qwen3-coder:30b");
        assert!(composer_empty_for_model("codex", screen, 2, 2, model));
        let single = screen.replace("\n  \x1b[1m?\x1b[0m for shortcuts", "\n");
        assert!(composer_empty_for_model("codex", &single, 2, 2, model));
        assert!(!composer_empty_for_model("codex", screen, 2, 2, None));
        assert!(!composer_empty_for_model("codex", screen, 2, 2, Some("another-model")));
        for replacement in ["", "my draft", "\n  qwen3-coder:30b default · /project"] {
            assert!(!composer_empty_for_model("codex", &screen.replace("\x1b[2mAsk Codex to do anything\x1b[0m", replacement), 2, 2, model));
        }
        for footer in ["Approve command?", "qwen3-coder:30b approve", "qwen3-coder:30b default trailing draft"] {
            assert!(!composer_empty_for_model("codex", &screen.replace("qwen3-coder:30b default", footer), 2, 2, model));
        }
        assert!(!composer_empty_for_model("codex", &single.replace("/project", "arbitrary text"), 2, 2, model));
        assert!(!composer_empty_for_model("codex", &(screen.to_owned() + "\n  more input"), 2, 2, model));
        assert!(!composer_empty_for_model("codex", screen, 3, 2, model));
    }
    #[test]
    fn windows_composers_under_conpty() {
        // Spike S3 (bundled ConPTY): Codex 0.162.0 prints `~\…` in its footer;
        // Claude 2.1.296 draws its normal-screen layout. Inbox ConPTY leaves
        // Claude's hidden cursor at the end of the last row, which fails closed.
        let codex = include_str!("../../tests/fixtures/codex-composer-empty-windows.txt");
        assert!(composer_empty("codex", codex, 2, 36));
        let single = codex.replace("\n  \x1b[1m?\x1b[0m for shortcuts", "\n");
        assert_ne!(single, codex);
        let windows_home = crate::platform::home().is_some_and(|home| shown_path::drive(&home));
        assert_eq!(composer_empty("codex", &single, 2, 36), windows_home);
        assert!(!composer_empty(
            "codex",
            &single.replace(r"~\zerus-dev", r"\\server\zerus-dev"),
            2,
            36
        ));
        let claude = include_str!("../../tests/fixtures/claude-composer-empty-windows.txt");
        assert!(composer_empty("claude", claude, 2, 6));
        assert!(!composer_empty("claude", claude, 118, 8));
    }
    #[test]
    fn codex_footer_model_tracks_native_identity_or_launch_model() {
        assert_eq!(codex_footer_model(&json!({"model":"confirmed", "base":["codex","-m","launch"]})), Some("confirmed"));
        assert_eq!(codex_footer_model(&json!({"base":["codex","--oss","--local-provider","ollama","-m","qwen3-coder:30b"]})), Some("qwen3-coder:30b"));
        assert_eq!(codex_footer_model(&json!({"base":["codex","--model=first","--model","last"]})), Some("last"));
        assert_eq!(codex_footer_model(&json!({"base":["codex","--","--model=prompt"]})), None);
        assert_eq!(codex_footer_model(&json!({"base":["codex","--model"]})), None);
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
