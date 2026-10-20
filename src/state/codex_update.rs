//! Codex's startup update picker precedes SessionStart and has no conversation ID.
//! Its versions and the exact installer command are part of the identity. Update
//! now leaves the TUI, runs that native command in the pane and exits Codex; the
//! session then needs an explicit resume. Skip choices keep Codex's own records.
use super::*;
use kimi_tui_choice::Handoff;
use sha2::{Digest, Sha256};
use std::time::Duration;

const TITLE: &str = "Update available";
const HINT: &str = "enter continue · esc skip";
const NOTES: &str = "Release notes: https://github.com/openai/codex/releases/latest";
// Reviewed Unix `UpdateAction::command_str` values (Codex 0.160). Any other
// command, or a clipped one, stays in Terminal.
const COMMANDS: [&str; 6] = [
    "npm install -g @openai/codex",
    "bun install -g @openai/codex",
    "vp install -g @openai/codex",
    "pnpm add -g @openai/codex",
    "brew upgrade --cask codex",
    "sh -c 'curl -fsSL https://chatgpt.com/codex/install.sh | CODEX_NON_INTERACTIVE=1 sh'",
];
const IDS: [&str; 3] = ["update_now", "skip", "skip_version"];
// After Update now, Codex 0.160.1 and 0.162.0 leave raw mode and clear the
// picker at once, print this announcement within half a second, then run the
// command in the pane and exit when it ends.
const STARTED: &str = "Updating Codex via";
// How long Codex may take to announce the command or exit after Update now.
// The installer itself can take minutes and continues in Terminal; the bound
// keeps `hgs answer` well inside the desktop's 30 s answer timeout.
const INSTALLER_START: Duration = Duration::from_secs(15);
const SKIPS: [&str; 2] = ["Skip", "Skip until next version"];

struct Panel {
    current: String,
    latest: String,
    command: &'static str,
    selected: usize,
}

fn compact(text: &str) -> String {
    text.split_whitespace().collect()
}

fn update_label(command: &str) -> String {
    format!("Update now (runs `{command}`)")
}

// Codex's own announcement of exactly this command, starting a row below the
// closed picker. Narrow panes wrap it, possibly inside a word.
fn installing(screen: &str, command: &str) -> bool {
    let rows: Vec<_> = screen.lines().map(str::trim).collect();
    let announcement = compact(&format!("{STARTED} `{command}`..."));
    !screen.contains(HINT)
        && (0..rows.len()).any(|i| {
            rows[i].starts_with("Updating") && compact(&rows[i..].join(" ")).starts_with(&announcement)
        })
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.lines().map(str::trim).filter(|r| !r.is_empty()).collect();
    if !rows.first()?.starts_with(TITLE) || rows.last()? != &HINT {
        return None;
    }
    // Narrow panes wrap every paragraph; a long URL or command may break inside
    // a word, so compare those without whitespace.
    let mut starts = Vec::new();
    let mut selected = None;
    for (index, row) in rows.iter().enumerate() {
        let option = starts.len() + 1;
        let row = match row.strip_prefix("› ") {
            Some(row) if row.starts_with(&format!("{option}. ")) => {
                if selected.replace(starts.len()).is_some() { return None; }
                row
            }
            _ => row,
        };
        if option <= 3 && row.starts_with(&format!("{option}. ")) {
            starts.push(index);
        }
    }
    if starts.len() != 3 {
        return None;
    }
    let text = |from: usize, to: usize| {
        let mut rows = rows[from..to].to_vec();
        rows[0] = rows[0].strip_prefix("› ").unwrap_or(rows[0]);
        rows.join(" ")
    };
    let header = text(0, starts[0]);
    let versions = header.strip_prefix(&format!("{TITLE} · "))?;
    let (current, rest) = versions.split_once(" → ")?;
    let (latest, notes) = rest.split_once(' ')?;
    let version = |v: &str| !v.is_empty() && v.chars().all(|c| c.is_ascii_alphanumeric() || ".-+".contains(c));
    if !version(current) || !version(latest) || compact(notes) != compact(NOTES) {
        return None;
    }
    let first = compact(&text(starts[0], starts[1]));
    let command = *COMMANDS.iter().find(|c| first == compact(&format!("1. {}", update_label(c))))?;
    for (i, label) in SKIPS.iter().enumerate() {
        let end = if i == 0 { starts[2] } else { rows.len() - 1 };
        if text(starts[i + 1], end) != format!("{}. {label}", i + 2) {
            return None;
        }
    }
    Some(Panel { current: current.into(), latest: latest.into(), command, selected: selected? })
}

fn eligible(record: &Value) -> bool {
    string(record, "agent") == "codex"
        && record["run_identity_version"] == 1
        && record["supervisor"].is_object()
        && !string(record, "run_id").is_empty()
        && string(record, "conversation_id").is_empty()
        && string(record, "error").is_empty()
        && !pause_active(record)
        && record.get("session_end").is_none()
        && record["last_event_at"].as_f64().unwrap_or(0.) == 0.
        && record["input_pending_at"].as_f64().unwrap_or(0.) == 0.
}

fn card(record: &Value, panel: &Panel) -> Option<Value> {
    if !eligible(record) { return None; }
    let label = update_label(panel.command);
    let hash = format!("{:x}", Sha256::digest(json!([
        record["run_id"], record["pane"], record["pid"], record["process_start"],
        record["expected_id"], record["launch_dir"], record["agent_home"],
        panel.current, panel.latest, panel.command, IDS
    ]).to_string()));
    let descriptions = [
        format!("Run `{}` in Terminal. Codex exits after the update; resume the session to continue.", panel.command),
        "Continue starting Codex. It asks again on a later start.".to_owned(),
        format!("Continue starting Codex without asking again about {}.", panel.latest),
    ];
    let labels = [label.as_str(), SKIPS[0], SKIPS[1]];
    Some(json!({"question_id":format!("codex-update:{hash}"),"question_hash":hash,
        "tool_call_id":"codex_update_prompt","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":record["created"].as_f64().unwrap_or(0.),
        "source":"codex_update","can_answer":true,"answer_transport":"codex_tui","answer_unavailable_reason":"",
        "questions":[{"id":"codex_update","header":"Codex update",
            "question":format!("Update Codex {} → {}?", panel.current, panel.latest),
            "body":format!("{TITLE} · {} → {}\n{NOTES}", panel.current, panel.latest),
            "multi_select":false,"allow_other":false,
            "options":(0..3).map(|i|json!({"id":IDS[i],"label":labels[i],"description":descriptions[i]})).collect::<Vec<_>>()}]}))
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen)?;
    Some((card(record, &panel)?["question_hash"].clone(), panel.selected))
}

pub(super) fn current(record: &Value) -> Option<Value> {
    if !eligible(record) { return None; }
    let panel = parse(&kimi_tui_choice::capture(record).ok()?)?;
    if !process_alive(record) || !matches(record, live().ok()?.get(string(record, "name"))) { return None; }
    input::checked_terminal(record, false).ok()?;
    card(record, &panel)
}

pub(super) fn available(record: &Value, question: &Value) -> Result<()> {
    let active = kimi_tui_choice::identity(record)?;
    if snapshot(&active, &kimi_tui_choice::capture(&active)?).map(|s| s.0) != Some(question["question_hash"].clone()) {
        return Err("Codex's update request changed; nothing was sent".into());
    }
    Ok(())
}

/// Update now reports whether Codex was `installing` or had already `exited`.
pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<Option<&'static str>> {
    let choice = answers[0]["selected_option_ids"][0].as_str().unwrap_or("");
    let chosen = IDS.iter().position(|id| *id == choice).ok_or("Choose whether to update Codex")?;
    if chosen == 0 {
        // The card's hash pins its command. A quick installer failure may end
        // the pinned process before the pane is read.
        let label = &question["questions"][0]["options"][0]["label"];
        let command = *COMMANDS.iter().find(|command| *label == update_label(command))
            .ok_or("Codex's update request changed; nothing was sent")?;
        let started = |screen: &str| installing(screen, command);
        let handoff = kimi_tui_choice::answer_handoff(record, question, chosen, IDS.len(), snapshot, &started, INSTALLER_START)?;
        return Ok(Some(if handoff == Handoff::Running { "installing" } else { "exited" }));
    }
    // Codex adds its own "Update available" notice to the main view after a
    // skip, so only the picker's hint marks the panel as still open.
    let acknowledged: fn(&str) -> bool = |screen| !screen.trim().is_empty() && !screen.contains(HINT);
    kimi_tui_choice::answer(record, question, chosen, IDS.len(), snapshot, acknowledged, false).map(|()| None)
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/codex-update-prompt.txt");
    const STANDALONE: &str = COMMANDS[5];
    fn record() -> Value { json!({"agent":"codex","run_id":"run","run_identity_version":1,"supervisor":{},"created":123.}) }

    #[test]
    fn complete_native_panel_and_wrapped_rows() {
        let panel = parse(SCREEN).unwrap();
        assert_eq!((panel.current.as_str(), panel.latest.as_str(), panel.command, panel.selected),
            ("0.160.0", "0.160.1", STANDALONE, 0));
        // Codex's own 28-column layout breaks the URL and the command inside words.
        let narrow = "  Update available · 0.160.0\n  → 0.160.1\n  Release notes:\n  https://github.com/opena\n  i/codex/releases/latest\n\n\
            › 1. Update now (runs `npm\n     install -g @openai/cod\n     ex`)\n  2. Skip\n  3. Skip until next\n     version\n\n  enter continue · esc skip";
        let panel = parse(narrow).unwrap();
        assert_eq!((panel.command, panel.selected), (COMMANDS[0], 0));
        assert_eq!(card(&record(), &parse(&SCREEN.replace("› 1.", "  1.").replace("  2. Skip", "› 2. Skip")).unwrap())
            .unwrap()["question_hash"], card(&record(), &parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        for invalid in [SCREEN.replace(HINT, "enter continue · esc"), SCREEN.replace("  2. Skip", "› 2. Skip"),
            SCREEN.replace("chatgpt.com", "example.com"), SCREEN.replace("3. Skip until next version", "↑"),
            SCREEN.replace("0.160.1", "0.160.1 beta"), SCREEN.replace("releases/latest", "releases"),
            SCREEN.replace("  2. Skip\n", ""), format!("Agent said:\n{SCREEN}"), format!("{SCREEN}\n› composer"),
            "  Update available · 0.0.0\n  → 9.9.9\n  Release notes:\n  https://github.com/openai/codex/releases/latest\n↑\n› 3. Skip until next version\n\n  enter continue · esc skip".into()] {
            assert!(parse(&invalid).is_none(), "{invalid}");
        }
    }

    #[test]
    fn installer_announcement_names_the_exact_command() {
        let started = |command: &str| format!("WARNING: launcher notice\n\nUpdating Codex via `{command}`...\nadded 1 package in 3s\n");
        for command in COMMANDS {
            assert!(installing(&started(command), command), "{command}");
        }
        // A 28-column pane wraps the announcement inside a word.
        let line = format!("Updating Codex via `{STANDALONE}`...");
        let narrow: Vec<String> = line.chars().collect::<Vec<_>>().chunks(28).map(|row| row.iter().collect()).collect();
        assert!(installing(&narrow.join("\n"), STANDALONE));
        for (screen, command) in [(started(COMMANDS[0]), COMMANDS[4]), (started("npm install -g @openai/codex-beta"), COMMANDS[0]),
            (format!("{}\n  {HINT}", started(COMMANDS[0])), COMMANDS[0]), (started(COMMANDS[0]).replace("`...", "`"), COMMANDS[0]),
            (format!("Agent said: Updating Codex via `{}`...", COMMANDS[0]), COMMANDS[0]), (SCREEN.into(), STANDALONE),
            ("✨ Update available! 0.160.0 -> 0.160.1\nReady for your first message".into(), COMMANDS[0]), (String::new(), COMMANDS[0])] {
            assert!(!installing(&screen, command), "{screen}");
        }
    }

    #[test]
    fn identity_pins_versions_command_and_run_but_not_highlight() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        assert_eq!(original["questions"][0]["options"][0]["label"], update_label(STANDALONE));
        assert!(original["questions"][0]["options"][0]["description"].as_str().unwrap().contains("Codex exits"));
        let brew = SCREEN.replace("sh -c 'curl -fsSL https://chatgpt.com/codex/install.sh |\n     CODEX_NON_INTERACTIVE=1 sh'", "brew upgrade --cask codex");
        for screen in [SCREEN.replace("0.160.1", "0.161.0"), SCREEN.replace("0.160.0 →", "0.159.0 →"), brew] {
            assert_ne!(original["question_hash"], card(&record(), &parse(&screen).unwrap()).unwrap()["question_hash"]);
        }
        for field in ["run_id", "pid", "launch_dir", "agent_home", "expected_id"] {
            let mut changed = record(); changed[field] = json!("changed");
            assert_ne!(original["question_hash"], card(&changed, &parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        }
        for (field, value) in [("conversation_id", json!("started")), ("last_event_at", json!(1.)),
            ("agent", json!("claude")), ("supervisor", Value::Null), ("error", json!("failed"))] {
            let mut changed = record(); changed[field] = value;
            assert!(card(&changed, &parse(SCREEN).unwrap()).is_none());
        }
    }
}
