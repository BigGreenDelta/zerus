//! Claude's workspace trust prompt is shown before SessionStart. Preserve the
//! native disclosure and require a fresh, explicit choice for the pinned run.
//! The frame (rule, title, folder, guide, exact choices and hint) must match;
//! the disclosure is verbatim identity, so new native wording stays visible.
//! On Windows the folder row is a drive path (`C:\Users\…`), compared with
//! the launch directory without case (`shown_path`).
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Accessing workspace:";
const HINT: &str = "Enter to confirm · Esc to cancel";
const LABELS: [&str; 2] = ["No, exit", "Yes, I trust this folder"];

struct Panel {
    /// Claude wraps a long folder at the panel width without a marker.
    folder: Vec<String>,
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
        || !shown_path::absolute(rows[3])
        || *rows.last()? != HINT
    {
        return None;
    }
    let blank = 3 + rows[3..].iter().position(|row| row.is_empty())?;
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
    // Claude itself abbreviates long permission lists. Keep its complete visible
    // warning, including the count and "and N more", without implying expansion.
    let disclosure = rows.get(blank + 1..footer)?.join("\n").trim().to_owned();
    if disclosure.is_empty() { return None; }
    let folder = rows[3..blank].iter().map(|row| row.to_string()).collect();
    Some(Panel { folder, disclosure, selected: selected? })
}

/// Whether the wrapped rows show exactly this folder. A wrap may drop the
/// spaces it breaks at; no other difference is accepted, except letter case
/// in a Windows drive path.
fn shows(rows: &[String], folder: &str) -> bool {
    let windows = shown_path::drive(folder);
    let mut rest = folder;
    for (i, row) in rows.iter().enumerate() {
        if i > 0 { rest = rest.trim_start_matches(' '); }
        let Some(after) = shown_path::strip_prefix(rest, row, windows) else { return false };
        rest = after;
    }
    rest.is_empty()
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
    let folder = string(record, "launch_dir");
    if !eligible(record) || !shows(&panel.folder, folder) { return None; }
    let hash = format!("{:x}", Sha256::digest(json!([
        record["run_id"], record["pane"], record["pid"], record["process_start"],
        record["expected_id"], folder, panel.disclosure, LABELS
    ]).to_string()));
    Some(json!({"question_id":format!("claude-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"claude_folder_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"claude_folder_trust",
        "can_answer":true,"answer_transport":"claude_tui","answer_unavailable_reason":"",
        "questions":[{"id":"trust","header":"Folder trust","question":"Trust this folder?",
            "body":format!("{}\n\n{}",folder,panel.disclosure),"multi_select":false,"allow_other":false,
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
        return Err("Claude's folder trust request changed; nothing was sent".into());
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
            format!("Previous agent said:\n{SCREEN}"), format!("{SCREEN}\n> "),
            SCREEN.split(" Quick safety").next().unwrap().to_owned()+"\n\n\n\n\n\n Security guide\n\n ❯ No, exit\n   Yes, I trust this folder\n\n Enter to confirm · Esc to cancel\n"] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn unfamiliar_disclosure_is_shown_verbatim() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for screen in [SCREEN.replace("Only proceed if you trust this configuration.", "Only proceed…"),
            SCREEN.replace("Claude Code'll be able to read, edit, and execute files here.", "Claude Code can also run project hooks here.")] {
            let panel = parse(&screen).unwrap();
            let question = card(&record(), &panel).unwrap();
            assert_eq!(question["questions"][0]["body"], format!("/work/example\n\n{}", panel.disclosure));
            assert!(question["can_answer"].as_bool().unwrap());
            assert_ne!(question["question_hash"], original["question_hash"]);
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
    fn long_folder_wrapped_by_claude_still_names_exactly_the_launch_directory() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for (launch, shown) in [("/work/example", " /work/exam\n ple"), ("/work/example", " /work/\n example"),
                                ("/work/my example", " /work/my\n example"), ("/work/example", " /wo\n rk/exa\n mple")] {
            let screen = SCREEN.replace(" /work/example\n", &format!("{shown}\n"));
            let mut record = record(); record["launch_dir"] = json!(launch);
            let question = card(&record, &parse(&screen).unwrap()).unwrap();
            assert_eq!(question["questions"][0]["body"].as_str().unwrap().split("\n\n").next(), Some(launch));
            if launch == "/work/example" { assert_eq!(question["question_hash"], original["question_hash"]); }
        }
        for shown in [" /work/exam\n pl", " /work/exam\n plex", " /work/ex ample", " /work/exam\n\n ple"] {
            let screen = SCREEN.replace(" /work/example\n", &format!("{shown}\n"));
            assert!(parse(&screen).and_then(|panel| card(&record(), &panel)).is_none(), "{shown}");
        }
    }
    // Claude Code 2.1.296 under bundled ConPTY (spike S3), in the normal-screen
    // layout; the account folder is the synthetic user01.
    const WINDOWS: &str = include_str!("../../tests/fixtures/claude-folder-trust-windows.txt");
    const WINDOWS_DIR: &str = r"C:\Users\user01\zerus-dev\p2\s3\homes\claude-trust\work";
    #[test]
    fn windows_drive_folder_names_the_launch_directory_in_any_case() {
        let mut record = record();
        record["launch_dir"] = json!(WINDOWS_DIR);
        let panel = parse(WINDOWS).unwrap();
        assert_eq!((panel.selected, panel.folder.as_slice()), (0, [WINDOWS_DIR.to_owned()].as_slice()));
        let question = card(&record, &panel).unwrap();
        let body = string(&question["questions"][0], "body");
        assert!(body.starts_with(&format!("{WINDOWS_DIR}\n\nQuick safety check")), "{body}");
        assert!(body.contains("4 tool permissions"));
        // Windows names ignore case; the identity keeps the exact launch directory.
        let mut other_case = record.clone();
        other_case["launch_dir"] = json!(WINDOWS_DIR.to_lowercase());
        assert!(card(&other_case, &panel).is_some());
        let wrapped = WINDOWS.replace(r" C:\Users\user01\zerus-dev\p2", " c:\\users\\USER01\\zerus-dev\n \\p2");
        assert_eq!(card(&record, &parse(&wrapped).unwrap()).unwrap()["question_hash"], question["question_hash"]);
        for launch in [r"C:\Users\user01\zerus-dev\p2\s3\homes\claude-trust\work2", r"D:\Users\user01\zerus-dev\p2\s3\homes\claude-trust\work", "/work/example"] {
            record["launch_dir"] = json!(launch);
            assert!(card(&record, &panel).is_none(), "{launch}");
        }
        for shown in [r" \\server\share\work", r" ~\zerus-dev\p2\s3\homes\claude-trust\work", " /c/Users/example/work"] {
            assert!(parse(&WINDOWS.replace(&format!(" {WINDOWS_DIR}"), shown)).and_then(|panel| card(&other_case, &panel)).is_none(), "{shown}");
        }
        // A Unix folder still needs its exact letters.
        assert!(card(&json!({"agent":"claude","run_id":"run","run_identity_version":1,"supervisor":{},"launch_dir":"/Work/Example"}),
            &parse(SCREEN).unwrap()).is_none());
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
