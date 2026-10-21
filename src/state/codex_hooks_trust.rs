//! Codex's separate startup consent for new/changed hooks precedes SessionStart.
//! Keep native trust ownership: the TUI submits its captured hook keys/hashes to
//! Codex, which verifies them. HGS never edits trust files or bypasses the prompt.
//! The title, exact choices and hint frame the panel; its disclosure, including
//! the hook count and native errors, is shown verbatim as part of the identity.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Hooks need review";
const HINT: &str = "enter confirm · esc skip";
#[cfg(test)]
const DISCLOSURE: &str = "Hooks can run outside the sandbox after you trust them.";
const LABELS: [&str; 3] = [
    "Review hooks",
    "Trust all and continue",
    "Continue without trusting (hooks won't run)",
];
const IDS: [&str; 3] = ["review", "trust", "continue_without_trusting"];
const DESCRIPTIONS: [&str; 3] = [
    "Open the native hook browser in Terminal to inspect commands and permissions.",
    "Save trust for all new or changed hooks shown by Codex and continue startup.",
    "Continue without trusting these hooks. Untrusted hooks will not run.",
];

struct Panel {
    disclosure: String,
    selected: usize,
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.lines().map(str::trim).filter(|r| !r.is_empty()).collect();
    if rows.first()? != &TITLE || rows.last()? != &HINT {
        return None;
    }
    let first = rows.iter().position(|r| r.starts_with("1. ") || r.starts_with("› 1. "))?;
    // Native errors are part of the visible decision and therefore its identity.
    let disclosure = rows[1..first].join(" ");
    if disclosure.is_empty() || disclosure.contains('…') { return None; }
    let mut position = first;
    let mut selected = None;
    for (i, label) in LABELS.iter().enumerate() {
        let row = *rows.get(position)?;
        let row = if let Some(row) = row.strip_prefix("› ") {
            if selected.replace(i).is_some() { return None; }
            row
        } else { row };
        let mut text = row.strip_prefix(&format!("{}. ", i + 1))?.to_owned();
        position += 1;
        while text != *label {
            if !label.starts_with(&(text.clone() + " ")) { return None; }
            text.push(' ');
            text.push_str(rows.get(position)?);
            position += 1;
        }
    }
    if position != rows.len() - 1 { return None; }
    Some(Panel { disclosure, selected: selected? })
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
    let hash = format!("{:x}", Sha256::digest(json!([
        record["run_id"], record["pane"], record["pid"], record["process_start"],
        record["expected_id"], record["launch_dir"], record["agent_home"], panel.disclosure, LABELS
    ]).to_string()));
    Some(json!({"question_id":format!("codex-hooks-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"codex_hooks_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":record["created"].as_f64().unwrap_or(0.),
        "source":"codex_hooks_trust","trust_request":true,"can_answer":true,
        "answer_transport":"codex_tui","answer_unavailable_reason":"",
        "questions":[{"id":"hooks_trust","header":"Hook trust","question":TITLE,
            "body":panel.disclosure,"multi_select":false,"allow_other":false,
            "options":LABELS.iter().enumerate().map(|(i,label)|json!({"id":IDS[i],"label":label,
                "description":DESCRIPTIONS[i]})).collect::<Vec<_>>()}]}))
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
        return Err("Codex's hook trust request changed; nothing was sent".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let choice = answers[0]["selected_option_ids"][0].as_str().unwrap_or("");
    let chosen = IDS.iter().position(|id| *id == choice).ok_or("Choose how to review Codex hooks")?;
    let acknowledged: fn(&str) -> bool = if chosen == 0 {
        |screen| {
            let text = screen.split_whitespace().collect::<Vec<_>>().join(" ");
            !text.contains(TITLE) && text.contains("Lifecycle hooks from config and enabled plugins.")
                && text.contains("esc close")
        }
    } else {
        |screen| !screen.trim().is_empty() && !screen.contains(TITLE) && !screen.contains(HINT)
    };
    kimi_tui_choice::answer(record, question, chosen, LABELS.len(), snapshot, acknowledged, false)
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/codex-hooks-trust.txt");
    fn record() -> Value { json!({"agent":"codex","run_id":"run","run_identity_version":1,"supervisor":{},"created":123.}) }
    #[test]
    fn complete_native_panel_and_wrapped_labels() {
        assert_eq!(parse(SCREEN).unwrap().selected, 0);
        let narrow = SCREEN.replace("sandbox after", "sandbox\n after").replace("(hooks won't run)", "(hooks\n won't run)");
        assert_eq!(card(&record(), &parse(SCREEN).unwrap()), card(&record(), &parse(&narrow).unwrap()));
        assert!(parse(&SCREEN.replace("4 hooks are", "1 hook is")).is_some());
        for invalid in [SCREEN.replace(HINT, "enter confirm"), SCREEN.replace("  2.", "› 2."),
            SCREEN.replace("won't run)", "..."), format!("Agent said:\n{SCREEN}"), format!("{SCREEN}\n› composer"),
            SCREEN.replace("Hooks can run", "Hooks…"), SCREEN.replace("  4 hooks are new or changed.\n  Hooks can run outside the sandbox after you trust them.\n", "")] {
            assert!(parse(&invalid).is_none(), "{invalid}");
        }
    }
    #[test]
    fn unfamiliar_disclosure_is_shown_verbatim() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for screen in [SCREEN.replace(DISCLOSURE, "Hooks can now also run during review."),
            SCREEN.replace(DISCLOSURE, &format!("{DISCLOSURE}\nTrusting hooks..."))] {
            let panel = parse(&screen).unwrap();
            let question = card(&record(), &panel).unwrap();
            assert_eq!(question["questions"][0]["body"], json!(panel.disclosure));
            assert_ne!(question["question_hash"], original["question_hash"]);
        }
    }
    #[test]
    fn identity_changes_with_run_count_error_and_launch_context_not_highlight() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        let moved = SCREEN.replace("› 1.", "  1.").replace("  2.", "› 2.");
        assert_eq!(original, card(&record(), &parse(&moved).unwrap()).unwrap());
        for screen in [SCREEN.replace("4 hooks", "5 hooks"), SCREEN.replace(DISCLOSURE,
            &format!("{DISCLOSURE}\nFailed to trust hooks: hook configuration changed"))] {
            assert_ne!(original["question_hash"], card(&record(), &parse(&screen).unwrap()).unwrap()["question_hash"]);
        }
        for field in ["run_id", "pid", "launch_dir", "agent_home", "expected_id"] {
            let mut changed = record(); changed[field] = json!("changed");
            assert_ne!(original["question_hash"], card(&changed, &parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        }
        for (field, value) in [("conversation_id", json!("started")), ("last_event_at", json!(1.)),
            ("agent", json!("claude")), ("supervisor", Value::Null)] {
            let mut changed = record(); changed[field] = value;
            assert!(card(&changed, &parse(SCREEN).unwrap()).is_none());
        }
    }
}
