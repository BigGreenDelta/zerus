//! Claude's cold-conversation quota chooser has no question hook. It can appear
//! on resume or between turns; mirror only the complete bottommost native panel.
//! The title, exact choices and footer frame it; the cost warning and the new
//! conversation description are shown verbatim as part of the identity.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Resume this conversation?";
const NEW_CONVERSATION: &str = "Start a new conversation";
// The panel is gone once neither its title nor this native description shows.
const DESCRIPTION: &str = "Claude can look this one up if you refer to it later";
#[cfg(test)]
const DRAFT_NOTICE: &str = "Your unsent message will still be there after you answer.";
const FOOTER: &str = "Enter to confirm · Esc to resume";

struct Panel {
    body: String,
    description: String,
    selected: Option<usize>,
}

fn normalized(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.trim().lines().map(str::trim).collect();
    let top = rows
        .iter()
        .rposition(|row| row.chars().count() >= 20 && row.chars().all(|c| c == '─'))?;
    let panel = &rows[top + 1..];
    if panel.len() < 8 || panel[0] != TITLE || !panel[1].is_empty() {
        return None;
    }
    let footer = *panel.last()?;
    if footer != FOOTER && footer != format!("↑/↓ to pick a row first · {FOOTER}") {
        return None;
    }
    let first = panel
        .iter()
        .position(|row| matches!(*row, "❯ 1. Resume" | "1. Resume" | "❯ Resume" | "Resume"))?;
    if first < 4 || !panel[first - 1].is_empty() || !panel[panel.len() - 2].is_empty() {
        return None;
    }
    let body = normalized(&panel[2..first - 1].join(" "));
    if body.is_empty() {
        return None;
    }
    let indexed = panel[first].contains("1. ");
    let mut selected = panel[first].starts_with('❯').then_some(0);
    let second = first + 1;
    let row = *panel.get(second)?;
    let label = if let Some(label) = row.strip_prefix('❯') {
        if selected.replace(1).is_some() {
            return None;
        }
        label.trim_start()
    } else {
        row
    };
    let label = if indexed {
        label.strip_prefix("2. ")?
    } else {
        label
    };
    let detail = normalized(
        &std::iter::once(label)
            .chain(panel[second + 1..panel.len() - 2].iter().copied())
            .collect::<Vec<_>>()
            .join(" "),
    );
    let description = detail.strip_prefix(NEW_CONVERSATION)?;
    if !description.is_empty() && !description.starts_with(' ') {
        return None;
    }
    Some(Panel { body, description: description.trim().into(), selected })
}

fn eligible(record: &Value) -> bool {
    record["agent"] == "claude"
        && record["run_identity_version"] == 1
        && record["supervisor"].is_object()
        && !string(record, "run_id").is_empty()
        && !string(record, "conversation_id").is_empty()
        && string(record, "error").is_empty()
        && !pause_active(record)
        && record.get("exited_at").is_none()
        && record.get("session_end").is_none()
        && record["active_tools"]
            .as_object()
            .is_none_or(|tools| tools.is_empty())
        && ["", "unknown", "idle", "input", "approval"].contains(&string(record, "phase"))
}

fn card(record: &Value, panel: &Panel) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let mut new_conversation = json!({"id":"new_conversation","label":NEW_CONVERSATION});
    if !panel.description.is_empty() {
        new_conversation["description"] = json!(panel.description);
    }
    let hash = format!(
        "{:x}",
        Sha256::digest(
            json!([
                record["run_id"],
                record["pane"],
                record["pid"],
                record["process_start"],
                record["conversation_id"],
                record["expected_id"],
                record["launch_dir"],
                TITLE,
                panel.body,
                panel.description
            ])
            .to_string()
        )
    );
    Some(
        json!({"question_id":format!("claude-resume:{hash}"),"question_hash":hash,
        "tool_call_id":"claude_resume","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"claude_resume",
        "can_answer":panel.selected.is_some(),"answer_transport":"claude_tui",
        "answer_unavailable_reason":if panel.selected.is_some() { "" } else { "Pick a row in Terminal before answering; Claude's native draft still has focus." },
        "questions":[{"id":"resume","header":"Resume conversation","question":TITLE,
            "body":panel.body,"multi_select":false,"allow_other":false,
            "options":[{"id":"resume","label":"Resume"},new_conversation]}]}),
    )
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen)?;
    Some((
        card(record, &panel)?["question_hash"].clone(),
        panel.selected?,
    ))
}

pub(super) fn current(record: &Value) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let panel = parse(&kimi_tui_choice::capture(record).ok()?)?;
    if !process_alive(record) || !matches(record, live().ok()?.get(string(record, "name"))) {
        return None;
    }
    input::checked_terminal(record, false).ok()?;
    card(record, &panel)
}

pub(super) fn available(record: &Value, question: &Value) -> Result<()> {
    let record = kimi_tui_choice::identity(record)?;
    if snapshot(&record, &kimi_tui_choice::capture(&record)?).map(|s| s.0)
        != Some(question["question_hash"].clone())
    {
        return Err(
            "Claude's resume question changed or its draft has focus; nothing was sent".into(),
        );
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let chosen = match answers[0]["selected_option_ids"][0].as_str() {
        Some("resume") => 0,
        Some("new_conversation") => 1,
        _ => return Err("Choose Resume or Start a new conversation".into()),
    };
    kimi_tui_choice::answer(
        record,
        question,
        chosen,
        2,
        snapshot,
        |screen| {
            !screen.trim().is_empty()
                && !screen.contains(TITLE)
                && !screen.contains("Resuming it will use")
                && !screen.contains(DESCRIPTION)
                && !screen.contains("Esc to resume")
        },
        false,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/claude-resume.txt");
    fn record() -> Value {
        json!({"agent":"claude","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":"/work/example","conversation_id":"bound","phase":"idle"})
    }
    #[test]
    fn complete_warning_and_choices_preserve_cost_without_default_answer() {
        let panel = parse(SCREEN).unwrap();
        let q = card(&record(), &panel).unwrap();
        assert!(q["questions"][0]["body"]
            .as_str()
            .unwrap()
            .contains("about 5%"));
        assert_eq!(q["questions"][0]["options"][1]["description"], DESCRIPTION);
        assert!(q["questions"][0]["options"][0].get("selected").is_none());
        assert_eq!(panel.selected, Some(0));
        let all = SCREEN.replace("about 5%", "all");
        assert!(parse(&all).is_some());
        for invalid in [
            SCREEN.replace("❯ 1. Resume", "❯ 1. Continue"),
            SCREEN.replace("Start a new conversation", "Start a new chat"),
            SCREEN.replace("2. Start", "❯ 2. Start"),
            SCREEN.replace(FOOTER, "Enter to confirm"),
            SCREEN.replace(" This conversation has been inactive for 1h 25m and is 293.6k tokens long. Resuming it will\n use about 5% of your 5-hour usage limit.\n", "\n"),
            SCREEN.replace("────────────────────────────────────────", ""),
            format!("{SCREEN}\n❯ another native prompt"),
            SCREEN.replace("2. Start", "3. Start"),
        ] {
            assert!(parse(&invalid).is_none(), "{invalid}");
        }
    }
    #[test]
    fn unfamiliar_warning_and_description_are_shown_verbatim() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for screen in [SCREEN.replace("5-hour", "7-hour"), SCREEN.replace("293.6k tokens long.", ""),
            SCREEN.replace(DESCRIPTION, "Claude keeps a summary of this one"), SCREEN.replace(DESCRIPTION, "")] {
            let panel = parse(&screen).unwrap();
            let question = card(&record(), &panel).unwrap();
            assert_eq!(question["questions"][0]["body"], json!(panel.body));
            assert_eq!(question["questions"][0]["options"][1]["description"].as_str().unwrap_or(""), panel.description);
            assert_ne!(question["question_hash"], original["question_hash"]);
        }
        let question = card(&record(), &parse(&SCREEN.replace(DESCRIPTION, "")).unwrap()).unwrap();
        assert!(question["questions"][0]["options"][1].get("description").is_none());
    }
    #[test]
    fn idle_unindexed_panel_and_wrapping_keep_identity_and_draft_focus() {
        let idle = SCREEN
            .replace("1. Resume", "Resume")
            .replace("2. Start", "Start");
        let q = card(&record(), &parse(&idle).unwrap()).unwrap();
        let moved = idle
            .replace("❯ Resume", "Resume")
            .replace("  Start", "❯ Start");
        assert_eq!(
            q["question_hash"],
            card(&record(), &parse(&moved).unwrap()).unwrap()["question_hash"]
        );
        let wrapped = idle
            .replace(
                "Claude can look this one up",
                "\n     Claude can look this one up",
            )
            .replace("use about", "use\n about");
        assert_eq!(
            q["question_hash"],
            card(&record(), &parse(&wrapped).unwrap()).unwrap()["question_hash"]
        );
        let draft = idle
            .replace("❯ Resume", "Resume")
            .replace(FOOTER, &format!("↑/↓ to pick a row first · {FOOTER}"));
        assert_eq!(
            card(&record(), &parse(&draft).unwrap()).unwrap()["can_answer"],
            false
        );
        assert!(snapshot(&record(), &draft).is_none());
        assert!(
            parse(&idle.replace("\n\n❯ Resume", &format!("\n{DRAFT_NOTICE}\n\n❯ Resume")))
                .is_some()
        );
    }
    #[test]
    fn resident_idle_turns_are_supported_but_working_or_foreign_runs_are_not() {
        let panel = parse(SCREEN).unwrap();
        let r = record();
        let hash = card(&r, &panel).unwrap()["question_hash"].clone();
        let mut resident = r.clone();
        resident["turn_started"] = json!(100);
        resident["input_pending_at"] = json!(101);
        assert!(card(&resident, &panel).is_some());
        for key in [
            "run_id",
            "conversation_id",
            "pane",
            "pid",
            "process_start",
            "launch_dir",
            "expected_id",
        ] {
            let mut changed = r.clone();
            changed[key] = json!("other");
            assert_ne!(hash, card(&changed, &panel).unwrap()["question_hash"]);
        }
        for (key, value) in [
            ("phase", json!("working")),
            ("agent", json!("codex")),
            ("conversation_id", json!(null)),
            ("active_tools", json!({"tool":{}})),
            ("exited_at", json!(10)),
        ] {
            let mut changed = r.clone();
            changed[key] = value;
            assert!(card(&changed, &panel).is_none());
        }
    }
}
