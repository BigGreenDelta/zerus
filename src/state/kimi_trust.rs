//! Kimi's startup trust panel precedes SessionStart and has no conversation ID.
//! Only a complete panel in a supervised startup run can be answered. Its path
//! and full MCP disclosure are part of the identity, never an implicit approval.
//! The frame must match; the disclosure is shown verbatim, so new wording stays visible.
//! On Windows Kimi prints the full drive path (`C:\Users\…`, never `~\…`),
//! compared with the launch directory without case (`shown_path`).
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Trust this folder?";
const HINT: &str = "↑↓ navigate · Enter select · Esc exit";
const LABELS: [&str; 2] = ["Trust this folder", "Don't trust"];
const DESCRIPTIONS: [&str; 2] = [
    "Enable project MCP servers. Remembered for this folder.",
    "Exit Kimi Code. Asked again next launch.",
];

struct Panel {
    folder: String,
    disclosure: String,
    selected: usize,
}

fn border(row: &str) -> bool {
    row.chars().count() >= 40 && row.chars().all(|c| c == '─')
}

fn parse(screen: &str) -> Option<Panel> {
    let all: Vec<_> = screen.trim().lines().map(str::trim).collect();
    let divider = all.iter().position(|row| border(row))?;
    if divider > 0 && !kimi_tui_choice::launcher_notices(&all[..divider], "kimi") {
        return None;
    }
    let rows = &all[divider..];
    if rows.len() < 15
        || !border(rows.last()?)
        || rows[1] != TITLE
        || rows[2] != HINT
        || !rows[3].is_empty()
        || !shown_path::absolute(rows[4])
        || !rows[5].is_empty()
    {
        return None;
    }
    // The complete, fixed choice footer prevents interpreting a clipped prompt
    // or a quoted transcript as the active dialog. No choice is preselected in UI.
    let options = rows.len().checked_sub(7)?;
    let mut selected = None;
    for i in 0..2 {
        let row = rows[options + i * 3];
        let label = if let Some(label) = row.strip_prefix("❯ ") {
            if selected.replace(i).is_some() {
                return None;
            }
            label
        } else {
            row
        };
        if label != LABELS[i]
            || rows[options + i * 3 + 1] != DESCRIPTIONS[i]
            || !rows[options + i * 3 + 2].is_empty()
        {
            return None;
        }
    }
    // Kimi asks for every untrusted folder and lists targets only when the
    // project's MCP configuration gates some. A clipped list is incomplete.
    let disclosure = rows[6..options].join("\n").trim().to_owned();
    if disclosure.is_empty() || disclosure.contains('…') {
        return None;
    }
    Some(Panel {
        folder: rows[4].into(),
        disclosure,
        selected: selected?,
    })
}

fn eligible(record: &Value) -> bool {
    string(record, "agent") == "kimi"
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
    if !eligible(record) || !shown_path::same(&panel.folder, string(record, "launch_dir")) {
        return None;
    }
    let hash = format!(
        "{:x}",
        Sha256::digest(
            json!([
                record["run_id"],
                record["pane"],
                record["pid"],
                record["process_start"],
                record["expected_id"],
                panel.folder,
                panel.disclosure,
                LABELS,
                DESCRIPTIONS
            ])
            .to_string()
        )
    );
    Some(
        json!({"question_id":format!("kimi-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"kimi_folder_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"kimi_folder_trust",
        "can_answer":true,"answer_transport":"kimi_tui","answer_unavailable_reason":"",
        "questions":[{"id":"trust","header":"Folder trust","question":TITLE,
            "body":format!("{}\n\n{}",panel.folder,panel.disclosure),"multi_select":false,"allow_other":false,
            "options":LABELS.iter().enumerate().map(|(i,label)|json!({"id":format!("trust_{i}"),"label":label,"description":DESCRIPTIONS[i]})).collect::<Vec<_>>()}]}),
    )
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen)?;
    Some((
        card(record, &panel)?["question_hash"].clone(),
        panel.selected,
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
    let screen = kimi_tui_choice::capture(&record)?;
    if snapshot(&record, &screen).map(|s| s.0) != Some(question["question_hash"].clone()) {
        return Err("Kimi's folder trust request changed; refresh Activity".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let chosen = match answers[0]["selected_option_ids"][0].as_str() {
        Some("trust_0") => 0,
        Some("trust_1") => 1,
        _ => return Err("Choose whether to trust this folder".into()),
    };
    kimi_tui_choice::answer(
        record,
        question,
        chosen,
        LABELS.len(),
        snapshot,
        |screen| !screen.trim().is_empty() && !screen.contains(TITLE) && !screen.contains(HINT),
        chosen == 1,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/kimi-folder-trust.txt");
    #[test]
    fn complete_trust_panel_only() {
        let panel = parse(SCREEN).unwrap();
        assert_eq!(panel.selected, 0);
        assert_eq!(panel.folder, "/work/example");
        assert!(panel.disclosure.contains("command=uvx"));
        for invalid in [
            SCREEN.replace(HINT, "Enter select"),
            SCREEN.replace("Don't trust", "❯ Don't trust"),
            SCREEN.replace("Exit Kimi Code. Asked again next launch.", "Exit Kimi…"),
            format!("agent said:\n{SCREEN}"),
            format!("{SCREEN}\n > composer"),
        ] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn launcher_notices_and_panel_without_targets() {
        const LAUNCHED: &str = include_str!("../../tests/fixtures/kimi-folder-trust-launcher.txt");
        let record = json!({"agent":"kimi","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":"/work/example"});
        let prelude = "? Master password: [hidden]\nunlock: SOPS_AGE_KEY loaded for this session.\nrun_kimi: secrets unlocked → launching kimi\n";
        let native = LAUNCHED.strip_prefix(prelude).unwrap();
        let panel = parse(LAUNCHED).unwrap();
        assert_eq!((panel.selected, panel.folder.as_str()), (0, "/work/example"));
        assert!(!panel.disclosure.contains("Project MCP targets"));
        let question = card(&record, &panel).unwrap();
        assert_eq!(question, card(&record, &parse(native).unwrap()).unwrap());
        assert!(!string(&question["questions"][0], "body").contains("launching kimi"));
        let reused = "run_kimi: SOPS_AGE_KEY already set — reusing it.\nrun_kimi: secrets unlocked → launching kimi\n";
        assert_eq!(question, card(&record, &parse(&format!("{reused}{native}")).unwrap()).unwrap());
        for invalid in [
            format!("agent said:\n{LAUNCHED}"),
            format!("{prelude}unrelated output\n{native}"),
            LAUNCHED.replace("launching kimi", "launching claude"),
            LAUNCHED.replace("secrets unlocked", "secrets unavailable"),
            format!("{LAUNCHED} > composer"),
        ] {
            assert!(parse(&invalid).is_none(), "{invalid}");
        }
    }
    #[test]
    fn unfamiliar_disclosure_is_shown_verbatim() {
        let record = json!({"agent":"kimi","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":"/work/example"});
        let original = card(&record, &parse(SCREEN).unwrap()).unwrap();
        for screen in [SCREEN.replace("remembers this folder.", "remembers this folder.\n   Project hooks also run after trust."),
            SCREEN.replace("    tracker (stdio): command=uvx\n", "")] {
            let panel = parse(&screen).unwrap();
            let question = card(&record, &panel).unwrap();
            assert_eq!(question["questions"][0]["body"], format!("/work/example\n\n{}", panel.disclosure));
            assert_ne!(question["question_hash"], original["question_hash"]);
        }
        assert!(parse(&SCREEN.replace("command=uvx", "command=uv…")).is_none());
    }
    #[test]
    fn windows_drive_folder_names_the_launch_directory_in_any_case() {
        // Kimi Code 2.1.1 under bundled ConPTY (spike S3), a folder without
        // project MCP targets; the account folder is the synthetic user01.
        const WINDOWS: &str =
            include_str!("../../tests/fixtures/kimi-folder-trust-plain-windows.txt");
        const FOLDER: &str = r"C:\Users\user01\zerus-dev\p2\s3\homes\kimi-trust\work";
        let record = |folder: &str| {
            json!({"agent":"kimi","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":folder})
        };
        let panel = parse(WINDOWS).unwrap();
        assert_eq!((panel.selected, panel.folder.as_str()), (0, FOLDER));
        assert!(!panel.disclosure.contains("Project MCP targets"));
        let question = card(&record(FOLDER), &panel).unwrap();
        assert_eq!(
            question["questions"][0]["body"],
            format!("{FOLDER}\n\n{}", panel.disclosure)
        );
        let other_case = card(&record(&FOLDER.to_lowercase()), &panel).unwrap();
        assert_eq!(other_case["question_hash"], question["question_hash"]);
        for folder in [
            r"C:\Users\user01\zerus-dev\p2\s3\homes\kimi-trust",
            "/work/example",
            r"D:\work",
        ] {
            assert!(card(&record(folder), &panel).is_none(), "{folder}");
        }
        for shown in [
            r"~\zerus-dev\p2\s3\homes\kimi-trust\work",
            r"\\server\share\work",
        ] {
            assert!(parse(&WINDOWS.replace(FOLDER, shown)).is_none(), "{shown}");
        }
        // A Unix folder still needs its exact letters.
        assert!(card(&record("/Work/Example"), &parse(SCREEN).unwrap()).is_none());
    }
    #[test]
    fn trust_identity_includes_folder_and_disclosure_but_not_selection() {
        let record = json!({"agent":"kimi","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":"/work/example"});
        let question = card(&record, &parse(SCREEN).unwrap()).unwrap();
        let changed_selection = SCREEN
            .replace("❯ Trust this folder", "  Trust this folder")
            .replace("  Don't trust", "❯ Don't trust");
        assert_eq!(
            question["question_hash"],
            card(&record, &parse(&changed_selection).unwrap()).unwrap()["question_hash"]
        );
        assert_ne!(
            question["question_hash"],
            card(
                &record,
                &parse(&SCREEN.replace("command=uvx", "command=evil")).unwrap()
            )
            .unwrap()["question_hash"]
        );
        assert!(card(
            &record,
            &parse(&SCREEN.replace("/work/example", "/other/folder")).unwrap()
        )
        .is_none());
        assert!(card(&json!({"conversation_id":"old"}), &parse(SCREEN).unwrap()).is_none());
    }
}
