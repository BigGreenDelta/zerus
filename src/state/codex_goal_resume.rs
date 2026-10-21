//! After `codex resume`, Codex 0.162 asks whether to resume a paused, stalled or
//! usage-limited goal before the first turn confirms the conversation. Its
//! dialog floats centered over the transcript, which stays visible beside it,
//! so only the dialog's own cells are read. The goal comes from Codex's goal
//! database and must match the visible dialog. Resume goal lets Codex continue
//! the goal by itself; Leave paused keeps the conversation waiting for a message.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Resume paused goal?";
const HINT: &str = "enter select · esc back";
const IDS: [&str; 2] = ["resume_goal", "leave_paused"];
const LABELS: [&str; 2] = ["Resume goal", "Leave paused"];
const DESCRIPTIONS: [&str; 2] = [
    "Mark it active and continue when idle",
    "Keep it paused; use /goal resume later",
];
// Codex offers the dialog for these goal states (thread_goal_actions.rs).
const PAUSED: [&str; 3] = ["paused", "blocked", "usage_limited"];
pub(super) const WAIT: &str =
    "Codex is waiting for your paused goal choice. Choose an option in Activity or Terminal before sending a message.";

#[derive(Debug, PartialEq)]
struct Panel {
    // "Goal: <objective>" without whitespace; Codex wraps it anywhere.
    goal: String,
    selected: usize,
}

fn compact(text: &str) -> String {
    text.split_whitespace().collect()
}

// Terminal cells of a character as tmux lays it out. A guess that differs
// from the terminal shifts the dialog's columns, so the parse fails closed.
fn cells(c: char) -> usize {
    match c as u32 {
        0x0300..=0x036F | 0x200B..=0x200F | 0xFE00..=0xFE0F => 0,
        0x1100..=0x115F | 0x231A..=0x231B | 0x23E9..=0x23EC | 0x23F0 | 0x23F3 | 0x25FD..=0x25FE
        | 0x2614..=0x2615 | 0x2648..=0x2653 | 0x267F | 0x2693 | 0x26A1 | 0x26AA..=0x26AB
        | 0x26BD..=0x26BE | 0x26C4..=0x26C5 | 0x26CE | 0x26D4 | 0x26EA | 0x26F2..=0x26F3
        | 0x26F5 | 0x26FA | 0x26FD | 0x2705 | 0x270A..=0x270B | 0x2728 | 0x274C | 0x274E
        | 0x2753..=0x2755 | 0x2757 | 0x2795..=0x2797 | 0x27B0 | 0x27BF | 0x2B1B..=0x2B1C
        | 0x2B50 | 0x2B55 | 0x2E80..=0x303E | 0x3041..=0x33FF | 0x3400..=0x4DBF
        | 0x4E00..=0x9FFF | 0xA000..=0xA4CF | 0xAC00..=0xD7A3 | 0xF900..=0xFAFF
        | 0xFE30..=0xFE4F | 0xFF00..=0xFF60 | 0xFFE0..=0xFFE6 | 0x1F004 | 0x1F0CF | 0x1F18E
        | 0x1F191..=0x1F19A | 0x1F200..=0x1F2FF | 0x1F300..=0x1F64F | 0x1F680..=0x1F6FF
        | 0x1F7E0..=0x1F7EB | 0x1F900..=0x1F9FF | 0x1FA70..=0x1FAFF | 0x20000..=0x3FFFD => 2,
        _ => 1,
    }
}

// Codex centers dialogs at most 72 cells wide, two cells from each edge
// (bottom_pane/view_stack.rs `dialog_area`).
fn columns(width: usize) -> (usize, usize) {
    let span = width.saturating_sub(4).max(width.min(8)).min(72);
    ((width - span) / 2, span)
}

// A row's cells inside the dialog; None when a wide character straddles an edge.
fn slice(row: &str, left: usize, span: usize) -> Option<String> {
    let (mut column, mut inside) = (0, String::new());
    for c in row.chars() {
        let end = column + cells(c);
        if (column < left && end > left) || (column < left + span && end > left + span) {
            return None;
        }
        if column >= left && end <= left + span {
            inside.push(c);
        }
        column = end;
    }
    Some(inside.trim_end().to_owned())
}

// An option's first row: the selection marker or its blank cells, the number
// and the label, then the description unless the dialog is too narrow for it.
fn option(row: &str, index: usize) -> Option<(bool, &str)> {
    let (marked, rest) = match row.strip_prefix("› ") {
        Some(rest) => (true, rest),
        None => (false, row.strip_prefix("  ")?),
    };
    let rest = rest.strip_prefix(&format!("{}. {}", index + 1, LABELS[index]))?;
    (rest.is_empty() || rest.starts_with("  ")).then_some((marked, rest))
}

fn parse(screen: &str, width: usize) -> Option<Panel> {
    let (left, span) = columns(width);
    let rows: Vec<_> = screen.lines().map(|row| slice(row, left, span)).collect();
    let row = |i: usize| rows[i].as_deref();
    // The dialog clears its cells, so the nearest title above the bottom hint
    // starts it and every row between them belongs to it.
    let hint = rows.iter().rposition(|r| r.as_deref() == Some(&format!("  {HINT}") as &str))?;
    let title = (0..hint).rev().find(|&i| row(i) == Some(&format!("  {TITLE}") as &str))?;
    let body: Vec<&str> = (title + 1..hint).map(row).collect::<Option<_>>()?;
    let blank = body.iter().position(|r| r.is_empty())?;
    let subtitle = &body[..blank];
    if !subtitle.first()?.starts_with("  Goal: ") || subtitle.iter().any(|r| !r.starts_with("  ")) {
        return None;
    }
    // Options follow blank rows; wrapped descriptions continue on indented rows.
    let choices: Vec<&str> = body[blank..].iter().copied().skip_while(|r| r.is_empty()).collect();
    let choices = &choices[..choices.iter().rposition(|r| !r.is_empty())? + 1];
    let second = choices.iter().position(|r| option(r, 1).is_some()).filter(|&i| i > 0)?;
    let mut selected = None;
    for (index, rows) in [&choices[..second], &choices[second..]].into_iter().enumerate() {
        let (marked, description) = option(rows[0], index)?;
        let rest = &rows[1..];
        if rest.iter().any(|r| r.is_empty() || !r.starts_with("     ")) {
            return None;
        }
        let description = std::iter::once(description).chain(rest.iter().copied()).collect::<Vec<_>>().join(" ");
        let description = description.split_whitespace().collect::<Vec<_>>().join(" ");
        if !description.is_empty() && description != DESCRIPTIONS[index] {
            return None;
        }
        if marked && selected.replace(index).is_some() {
            return None;
        }
    }
    Some(Panel { goal: compact(&subtitle.join(" ")), selected: selected? })
}

fn width(record: &Value) -> Option<usize> {
    let info = backend::current()
        .pane_info(backend::Target::Pane(string(record, "pane")), &[backend::PaneField::Width])
        .ok()?;
    info.value().parse().ok()
}

fn eligible(record: &Value) -> bool {
    resume_input::pending(record) && !pause_active(record)
}

fn card(record: &Value, panel: &Panel) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let goal = goals::summary(record)["goal"].clone();
    let objective = string(&goal, "objective");
    if !PAUSED.contains(&string(&goal, "status")) || compact(&format!("Goal: {objective}")) != panel.goal {
        return None;
    }
    let hash = format!("{:x}", Sha256::digest(json!([
        record["run_id"], record["pane"], record["pid"], record["process_start"],
        record["conversation_id"], record["expected_id"], record["launch_dir"], record["agent_home"],
        goal["id"], objective, IDS
    ]).to_string()));
    Some(json!({"question_id":format!("codex-goal-resume:{hash}"),"question_hash":hash,
        "tool_call_id":"codex_goal_resume","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":record["created"].as_f64().unwrap_or(0.),
        "source":"codex_goal_resume","can_answer":true,"answer_transport":"codex_tui","answer_unavailable_reason":"",
        "questions":[{"id":"codex_goal_resume","header":"Paused goal","question":TITLE,
            "body":format!("Goal: {objective}"),"multi_select":false,"allow_other":false,
            "options":(0..2).map(|i| json!({"id":IDS[i],"label":LABELS[i],"description":DESCRIPTIONS[i]}))
                .collect::<Vec<_>>()}]}))
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen, width(record)?)?;
    Some((card(record, &panel)?["question_hash"].clone(), panel.selected))
}

pub(super) fn current(record: &Value) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let panel = parse(&kimi_tui_choice::capture(record).ok()?, width(record)?)?;
    if !process_alive(record) || !matches(record, live().ok()?.get(string(record, "name"))) {
        return None;
    }
    input::checked_terminal(record, false).ok()?;
    card(record, &panel)
}

pub(super) fn available(record: &Value, question: &Value) -> Result<()> {
    let active = kimi_tui_choice::identity(record)?;
    if snapshot(&active, &kimi_tui_choice::capture(&active)?).map(|s| s.0) != Some(question["question_hash"].clone()) {
        return Err("Codex's paused goal question changed; nothing was sent".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let choice = answers[0]["selected_option_ids"][0].as_str().unwrap_or("");
    let chosen = IDS.iter().position(|id| *id == choice).ok_or("Choose whether to resume the paused goal")?;
    // Resume goal changes the goal state before the dialog closes, so only the
    // closed dialog acknowledges the choice.
    kimi_tui_choice::answer(record, question, chosen, IDS.len(), snapshot,
        |screen| !(screen.trim().is_empty() || screen.contains(TITLE) && screen.contains(HINT)), false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use rusqlite::Connection;
    const SCREEN: &str = include_str!("../../tests/fixtures/codex-goal-resume.txt");
    const OBJECTIVE: &str = "Finish the example export: verify the staging copy, compare row counts with the source, \
        publish the reviewed archive and keep the previous backup until the replacement is accepted.";
    const CONVERSATION: &str = "12345678-1234-1234-1234-123456789abc";
    // Codex's own snapshot (tui/src/chatwidget/snapshots, 0.162.1) at 100 cells:
    // the composer and model line remain visible left of the dialog.
    const UPSTREAM: &str = "                Resume paused goal?\n\
        › Ask Codex to  Goal: Keep improving the bare goal command until it feels calm and\n\
        \x20               useful.\n\
        \x20 GPT-5.6-Sol\n\n\
        \x20             › 1. Resume goal   Mark it active and continue when idle\n\
        \x20               2. Leave paused  Keep it paused; use /goal resume later\n\n\
        \x20               enter select · esc back\n";

    fn record(home: &Path) -> Value {
        json!({"agent":"codex","run_id":"run","run_identity_version":1,"supervisor":{},"startup_kind":"resume",
            "conversation_id":CONVERSATION,"expected_id":CONVERSATION,"agent_home":home,"created":123.,
            "phase":"unknown","activity":"unknown","last_event_at":0})
    }
    fn goals(status: &str, objective: &str) -> tempfile::TempDir {
        let home = tempfile::tempdir().unwrap();
        let db = Connection::open(home.path().join("goals_1.sqlite")).unwrap();
        db.execute_batch("CREATE TABLE thread_goals(thread_id TEXT PRIMARY KEY, goal_id TEXT, objective TEXT, status TEXT, token_budget INTEGER, tokens_used INTEGER, time_used_seconds INTEGER, created_at_ms INTEGER, updated_at_ms INTEGER);").unwrap();
        db.execute("INSERT INTO thread_goals VALUES (?1, 'goal', ?2, ?3, NULL, 10, 10, 1000, 2000)", [CONVERSATION, objective, status]).unwrap();
        home
    }

    #[test]
    fn centered_dialog_is_read_beside_the_visible_transcript() {
        assert_eq!(columns(87), (7, 72));
        assert_eq!(columns(100), (14, 72));
        assert_eq!(columns(50), (2, 46));
        let panel = parse(SCREEN, 87).unwrap();
        assert_eq!((panel.goal, panel.selected), (compact(&format!("Goal: {OBJECTIVE}")), 0));
        let upstream = parse(UPSTREAM, 100).unwrap();
        assert_eq!(upstream.goal, compact("Goal: Keep improving the bare goal command until it feels calm and useful."));
        let second = SCREEN.replace("› 1. Resume goal", "  1. Resume goal").replace("  2. Leave paused", "› 2. Leave paused");
        assert_eq!(parse(&second, 87).unwrap().selected, 1);
        // Narrow dialogs hide the descriptions or wrap them onto indented rows.
        // Two cells of transcript stay visible left of a narrow dialog.
        let beside = |rows: &[&str]| rows.iter().map(|row| format!("ab{row}")).collect::<Vec<_>>().join("\n");
        let narrow = beside(&["  Resume paused goal?", "  Goal: Ship the example export", "", "› 1. Resume goal",
            "  2. Leave paused", "", "  enter select · esc back"]);
        assert_eq!(parse(&narrow, 50).unwrap().selected, 0);
        let wrapped = beside(&["  Resume paused goal?", "  Goal: Ship the example export", "",
            "› 1. Resume goal   Mark it active and continue", "                   when idle",
            "  2. Leave paused  Keep it paused; use /goal resume", "                   later", "", "  enter select · esc back"]);
        assert_eq!(parse(&wrapped, 60).unwrap().selected, 0);
        // Wide transcript characters beside the dialog keep its columns.
        assert!(parse(&SCREEN.replace("• Searc  Goal", "• 検索c  Goal"), 87).is_some());
    }

    #[test]
    fn incomplete_or_shifted_dialogs_are_not_read() {
        for screen in [SCREEN.replace("  2. Leave paused", "› 2. Leave paused"), SCREEN.replace("› 1. Resume goal", "  1. Resume goal"),
            SCREEN.replace("Leave paused", "Leave it paused"), SCREEN.replace("esc back", "esc close"),
            SCREEN.replace("Goal: ", "Plan: "), SCREEN.replace("continue when idle", "continue now"),
            SCREEN.replace("  2. Leave paused", "  3. Leave paused"), SCREEN.replace("Resume paused goal?", "Resume goal?"),
            // A wide character across the dialog's edge cannot be placed.
            SCREEN.replace("• Searc  Goal", "• Sear検 Goal"),
            SCREEN.replace("ted\n", "ted\n\n")] {
            assert!(parse(&screen, 87).is_none(), "{screen}");
        }
        // Another terminal width moves the dialog's columns.
        assert!(parse(SCREEN, 100).is_none());
        assert!(parse(UPSTREAM, 87).is_none());
    }

    #[test]
    fn card_follows_the_paused_goal_of_this_resumed_conversation() {
        let panel = parse(SCREEN, 87).unwrap();
        let home = goals("usage_limited", OBJECTIVE);
        let question = card(&record(home.path()), &panel).unwrap();
        assert!(question["question_id"].as_str().unwrap().starts_with("codex-goal-resume:"));
        assert_eq!((question["source"].clone(), question["can_answer"].clone()), (json!("codex_goal_resume"), json!(true)));
        let item = &question["questions"][0];
        assert_eq!(item["body"], format!("Goal: {OBJECTIVE}"));
        assert_eq!(item["options"][0]["id"], "resume_goal");
        assert_eq!(item["options"][1]["label"], "Leave paused");
        for status in ["paused", "blocked"] {
            assert!(card(&record(goals(status, OBJECTIVE).path()), &panel).is_some());
        }
        // An active goal, another goal or a confirmed conversation has no question.
        for home in [goals("active", OBJECTIVE), goals("complete", OBJECTIVE), goals("paused", "Another objective")] {
            assert!(card(&record(home.path()), &panel).is_none());
        }
        for (field, value) in [("last_event_at", json!(1.)), ("startup_kind", json!("new")), ("expected_id", json!("other")),
            ("phase", json!("idle")), ("agent", json!("claude")), ("supervisor", Value::Null), ("error", json!("failed"))] {
            let mut changed = record(home.path());
            changed[field] = value;
            assert!(card(&changed, &panel).is_none(), "{field}");
        }
        // The highlight is not part of the request; the run is.
        let second = parse(&SCREEN.replace("› 1. Resume goal", "  1. Resume goal").replace("  2. Leave paused", "› 2. Leave paused"), 87).unwrap();
        assert_eq!(card(&record(home.path()), &second).unwrap()["question_hash"], question["question_hash"]);
        let mut other = record(home.path());
        other["pid"] = json!(7);
        assert_ne!(card(&other, &panel).unwrap()["question_hash"], question["question_hash"]);
    }
}
