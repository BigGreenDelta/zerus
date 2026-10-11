//! Claude's workspace trust prompt is shown before SessionStart. Preserve the
//! native disclosure and require a fresh, explicit choice for the pinned run.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Accessing workspace:";
const HINT: &str = "Enter to confirm · Esc to cancel";
const INTRO: &str = "Quick safety check: Is this a project you created or one you trust? (Like your own code, a well-known open source project, or work from your team). If not, take a moment to review what's in this folder first.";
const ACCESS: &str = "Claude Code'll be able to read, edit, and execute files here.";
const LABELS: [&str; 2] = ["No, exit", "Yes, I trust this folder"];

struct Panel {
    folder: String,
    disclosure: String,
    selected: usize,
}

fn parse(screen: &str) -> Option<Panel> {
    let all: Vec<_> = screen.trim().lines().map(str::trim).collect();
    let divider = all.iter().position(|row| row.chars().count() >= 40 && row.chars().all(|c| c == '─'))?;
    if divider > 0 {
        // Already-running launchers can leave these exact notices above the
        // native panel. Never skip arbitrary output to find a quoted dialog.
        if !kimi_tui_choice::launcher_notices(&all[..divider], "claude") {
            let prelude = all[..divider].join(" ").split_whitespace().collect::<Vec<_>>().join(" ");
            let prompt = prelude.strip_prefix("hgs: Claude token is in the locked login keychain (ssh context); unlocking it password to unlock ")?;
            if !prompt.starts_with('/') || !prompt.ends_with("/Library/Keychains/login.keychain-db:") { return None; }
        }
    }
    let rows = &all[divider..];
    if rows.len() < 15
        || rows[0].chars().count() < 40
        || !rows[0].chars().all(|c| c == '─')
        || rows[1] != TITLE
        || !rows[2].is_empty()
        || !rows[3].starts_with('/')
        || !rows[4].is_empty()
        || *rows.last()? != HINT
    {
        return None;
    }
    let footer = rows.len().checked_sub(6)?;
    if rows[footer] != "Security guide"
        || !rows[footer + 1].is_empty()
        || !rows[footer + 4].is_empty()
    {
        return None;
    }
    // The footer has two adjacent options, followed by a blank and the hint.
    // Reject clipped panels, quoted dialogs and unknown native option variants.
    let mut selected = None;
    for (i, expected) in LABELS.iter().enumerate() {
        let row = rows[footer + 2 + i];
        let label = if let Some(label) = row.strip_prefix("❯ ") {
            if selected.replace(i).is_some() { return None; }
            label
        } else { row };
        if label != *expected { return None; }
    }
    let disclosure = rows[5..footer].join("\n").trim().to_owned();
    let normalized = disclosure.split_whitespace().collect::<Vec<_>>().join(" ");
    let expected = format!("{INTRO} {ACCESS}");
    if !normalized.starts_with(&expected) { return None; }
    // Claude itself abbreviates long permission lists. Keep its complete visible
    // warning, including the count and "and N more", without implying expansion.
    if normalized != expected && !normalized.ends_with("These will apply without asking. Only proceed if you trust this configuration.") {
        return None;
    }
    Some(Panel { folder: rows[3].into(), disclosure, selected: selected? })
}

fn eligible(record: &Value) -> bool {
    string(record, "agent") == "claude"
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
    if !eligible(record) || panel.folder != string(record, "launch_dir") { return None; }
    let hash = format!("{:x}", Sha256::digest(json!([
        record["run_id"], record["pane"], record["pid"], record["process_start"],
        record["expected_id"], panel.folder, panel.disclosure, LABELS
    ]).to_string()));
    Some(json!({"question_id":format!("claude-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"claude_folder_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"claude_folder_trust",
        "can_answer":true,"answer_transport":"claude_tui","answer_unavailable_reason":"",
        "questions":[{"id":"trust","header":"Folder trust","question":"Trust this folder?",
            "body":format!("{}\n\n{}",panel.folder,panel.disclosure),"multi_select":false,"allow_other":false,
            "options":LABELS.iter().enumerate().map(|(i,label)|json!({"id":format!("trust_{i}"),"label":label})).collect::<Vec<_>>()}]}))
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
    let record = kimi_tui_choice::identity(record)?;
    let screen = kimi_tui_choice::capture(&record)?;
    if snapshot(&record, &screen).map(|s| s.0) != Some(question["question_hash"].clone()) {
        return Err("Claude's folder trust request changed; refresh Activity".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let chosen = match answers[0]["selected_option_ids"][0].as_str() {
        Some("trust_0") => 0,
        Some("trust_1") => 1,
        _ => return Err("Choose whether to trust this folder".into()),
    };
    kimi_tui_choice::answer(record, question, chosen, LABELS.len(), snapshot,
        |screen| !screen.trim().is_empty() && !screen.contains(TITLE) && !screen.contains(HINT), chosen == 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/claude-folder-trust.txt");
    fn record() -> Value { json!({"agent":"claude","run_id":"run","run_identity_version":1,
        "supervisor":{},"launch_dir":"/work/example"}) }
    #[test]
    fn native_panel_preserves_permissions_and_needs_explicit_choice() {
        let panel = parse(SCREEN).unwrap();
        assert_eq!(panel.selected, 0);
        let question = card(&record(), &panel).unwrap();
        assert!(string(&question["questions"][0], "body").contains("17 tool permissions"));
        assert!(string(&question["questions"][0], "body").contains("and 9 more"));
        assert!(question["questions"][0]["options"][0].get("selected").is_none());
        let simple = SCREEN.split(" ⚠").next().unwrap().to_owned() + " Security guide\n\n ❯ No, exit\n   Yes, I trust this folder\n\n Enter to confirm · Esc to cancel\n";
        assert!(parse(&simple).is_some());
        for invalid in [SCREEN.replace(HINT,"Enter to confirm"), SCREEN.replace("Yes, I trust", "❯ Yes, I trust"),
            SCREEN.replace("Security guide", ""), SCREEN.replace("No, exit","No, continue without these permissions"),
            format!("Previous agent said:\n{SCREEN}"), format!("{SCREEN}\n> "), SCREEN.replace("Only proceed if you trust this configuration.", "Only proceed…")] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn legacy_keychain_prelude_does_not_hide_the_native_trust_panel() {
        let prefix = "hgs: Claude token is in the locked login keychain (ssh context); unlocking it\npassword to unlock /Users/test/Library/Keychains/login.keychain-db: \n\n";
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        let prefixed = card(&record(), &parse(&format!("{prefix}{SCREEN}")).unwrap()).unwrap();
        assert_eq!(original["question_hash"], prefixed["question_hash"]);
        assert!(parse(&format!("{prefix}unrelated command output\n{SCREEN}")).is_none());
        assert!(parse(&format!("{prefix}{SCREEN}\nAnother question")).is_none());
    }
    #[test]
    fn bitwarden_launch_notices_preserve_the_exact_native_question() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for prefix in [
            "Bitwarden master password:\nunlock: SOPS_AGE_KEY loaded for this session.\nrun_claude: secrets unlocked → launching claude\n\n",
            "? Master password: [hidden]\nunlock: SOPS_AGE_KEY loaded for this session.\nrun_claude: secrets unlocked → launching claude\n\n",
            "run_claude: SOPS_AGE_KEY already set — reusing it.\nrun_claude: secrets unlocked → launching claude\n\n",
        ] {
            let screen = format!("{prefix}{SCREEN}");
            let question = card(&record(), &parse(&screen).unwrap()).unwrap();
            assert_eq!(original, question);
            for invalid in [
                format!("Previous agent said:\n{screen}"),
                format!("{prefix}unrelated command output\n{SCREEN}"),
                format!("{screen}\nAnother question"),
                screen.replace("launching claude", "launching codex"),
                screen.replace("secrets unlocked", "secrets unavailable"),
                screen.replace(HINT, "Enter to confirm"),
            ] {
                assert!(parse(&invalid).is_none());
            }
        }
    }
    #[test]
    fn binds_folder_run_and_disclosure_independently_of_highlight() {
        let record = record();
        let hash = card(&record,&parse(SCREEN).unwrap()).unwrap()["question_hash"].clone();
        let changed = SCREEN.replace("❯ No, exit", "  No, exit").replace("  Yes, I trust", "❯ Yes, I trust");
        assert_eq!(hash, card(&record,&parse(&changed).unwrap()).unwrap()["question_hash"]);
        assert_ne!(hash, card(&record,&parse(&SCREEN.replace("17 tool", "18 tool")).unwrap()).unwrap()["question_hash"]);
        assert!(card(&record,&parse(&SCREEN.replace("/work/example", "/other")).unwrap()).is_none());
        let mut active = record; active["conversation_id"] = json!("started");
        assert!(card(&active,&parse(SCREEN).unwrap()).is_none());
    }
}
