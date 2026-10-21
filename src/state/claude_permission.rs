//! Claude's native auto-mode onboarding has no question hook, even though
//! SessionStart may already have bound a conversation. Mirror only its complete
//! startup panel; changing the account default always needs an explicit answer.
//! The title and exact choices frame it; the explanation is shown verbatim.
//! The banner folder is `/…` or `~/…` on Unix; on Windows it is a drive path or
//! `~\…` below the platform home, both compared without case (`shown_path`).
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Make auto mode your default permission mode?";
const ACCEPT: &str = "Yes, set auto mode as my default permission mode";

struct Panel {
    folder: String,
    body: String,
    labels: [String; 2],
    selected: usize,
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.trim().lines().map(str::trim).collect();
    if rows.len() < 10 || !rows[0].contains("Claude Code v") {
        return None;
    }
    let folder = rows[2].get(shown_path::start(rows[2])?..)?.to_owned();
    let divider = rows
        .iter()
        .position(|row| row.chars().count() >= 40 && row.chars().all(|c| c == '─'))?;
    let end = rows.len().checked_sub(3)?;
    if divider < 3
        || end <= divider + 3
        || rows[divider + 1] != TITLE
        || !rows[divider + 2].is_empty()
        || !rows[end].is_empty()
    {
        return None;
    }
    let body = rows[divider + 3..end].join("\n").trim().to_owned();
    if body.is_empty() {
        return None;
    }
    let mut labels = [String::new(), String::new()];
    let mut selected = None;
    for (i, row) in rows[end + 1..].iter().enumerate() {
        let label = if let Some(value) = row.strip_prefix('❯') {
            if selected.replace(i).is_some() {
                return None;
            }
            value.trim_start()
        } else {
            row
        };
        labels[i] = label.to_string();
    }
    // The native decline label contains the current mode. Preserve it verbatim
    // and include it in the question identity instead of assuming bypass.
    if labels[0] != ACCEPT
        || !labels[1].starts_with("No, keep ")
        || labels[1].len() <= "No, keep ".len()
        || labels[1].len() > 100
    {
        return None;
    }
    Some(Panel {
        folder,
        body,
        labels,
        selected: selected?,
    })
}

fn eligible(record: &Value) -> bool {
    string(record, "agent") == "claude"
        && record["run_identity_version"] == 1
        && record["supervisor"].is_object()
        && !string(record, "run_id").is_empty()
        && string(record, "error").is_empty()
        && !pause_active(record)
        && record.get("exited_at").is_none()
        && record.get("session_end").is_none()
        && record["turn_started"].as_f64().unwrap_or(0.) == 0.
        && record["input_pending_at"].as_f64().unwrap_or(0.) == 0.
        && record["active_tools"]
            .as_object()
            .is_none_or(|tools| tools.is_empty())
        && ["", "unknown", "idle", "input", "approval"].contains(&string(record, "phase"))
}

fn card(record: &Value, panel: &Panel) -> Option<Value> {
    card_below(record, panel, crate::platform::home().as_deref())
}

fn card_below(record: &Value, panel: &Panel, home: Option<&str>) -> Option<Value> {
    let launch_dir = string(record, "launch_dir");
    // Claude's startup banner abbreviates paths inside the user's home. Derive
    // that one display alias from the exact launch directory; never infer a
    // folder from the suffix after '~' or resolve arbitrary relative paths.
    if !eligible(record) || !shown_path::names(&panel.folder, launch_dir, home) {
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
                record["conversation_id"],
                record["expected_id"],
                record["launch_dir"],
                panel.folder,
                TITLE,
                panel.body,
                panel.labels
            ])
            .to_string()
        )
    );
    Some(
        json!({"question_id":format!("claude-permissions:{hash}"),"question_hash":hash,
        "tool_call_id":"claude_permission_mode","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"claude_permission_mode",
        "can_answer":true,"answer_transport":"claude_tui","answer_unavailable_reason":"",
        "questions":[{"id":"permission_mode","header":"Default permission mode","question":TITLE,
            "body":panel.body,"multi_select":false,"allow_other":false,
            "options":panel.labels.iter().enumerate().map(|(i,label)|json!({"id":format!("mode_{i}"),"label":label})).collect::<Vec<_>>()}]}),
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
    if snapshot(&record, &kimi_tui_choice::capture(&record)?).map(|s| s.0)
        != Some(question["question_hash"].clone())
    {
        return Err("Claude's permission mode question changed; nothing was sent".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let chosen = match answers[0]["selected_option_ids"][0].as_str() {
        Some("mode_0") => 0,
        Some("mode_1") => 1,
        _ => return Err("Choose whether to use auto mode or keep the current mode".into()),
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
                && !screen.contains(ACCEPT)
                && !screen.contains("No, keep ")
        },
        false,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/claude-auto-mode.txt");
    #[cfg(unix)]
    const HOME_SCREEN: &str = include_str!("../../tests/fixtures/claude-auto-mode-home.txt");
    fn record() -> Value {
        json!({"agent":"claude","run_id":"run","run_identity_version":1,
        "supervisor":{},"launch_dir":"/work/example","conversation_id":"bound","phase":"idle","last_event_at":100})
    }
    #[test]
    fn recognizes_complete_native_question_after_session_start() {
        let panel = parse(SCREEN).unwrap();
        let q = card(&record(), &panel).unwrap();
        assert_eq!(
            q["questions"][0]["options"][1]["label"],
            "No, keep bypass permissions"
        );
        assert_eq!(q["conversation_id"], "bound");
        assert!(q["questions"][0]["options"][0].get("selected").is_none());
        let blank = SCREEN.lines().map(|row| if row.starts_with("   Auto mode") || row.starts_with("   call for")
            || row.starts_with("   as lower") { "" } else { row }).collect::<Vec<_>>().join("\n");
        for invalid in [
            blank,
            SCREEN.replace(ACCEPT, "Yes, continue"),
            format!("User quoted:\n{SCREEN}"),
            format!("{SCREEN}\n❯ "),
            SCREEN.replace("No, keep", "❯ No, keep"),
            SCREEN.replace("❯ Yes", "Yes"),
        ] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn unfamiliar_explanation_is_shown_verbatim() {
        let original = card(&record(), &parse(SCREEN).unwrap()).unwrap();
        for screen in [SCREEN.replace("and blocks the rest.", "and asks about the rest."), SCREEN.replace("and blocks the rest.", "")] {
            let panel = parse(&screen).unwrap();
            let question = card(&record(), &panel).unwrap();
            assert_eq!(question["questions"][0]["body"], json!(panel.body));
            assert_ne!(question["question_hash"], original["question_hash"]);
        }
    }
    // The platform home with a POSIX `~/` banner; the Windows forms are covered
    // by windows_banner_names_the_launch_directory_below_the_platform_home.
    #[cfg(unix)]
    #[test]
    fn home_abbreviations_match_only_the_exact_launch_directory() {
        let mut r = record();
        r["launch_dir"] = json!(home().join("work/example"));
        let panel = parse(HOME_SCREEN).unwrap();
        assert_eq!(panel.folder, "~/work/example");
        let question = card(&r, &panel).unwrap();
        assert_eq!(question["source"], "claude_permission_mode");
        assert_eq!(question["questions"][0]["options"][1]["label"], "No, keep bypass permissions");
        assert!(card(&record(), &panel).is_none());
        for folder in ["~/work/other", "~other/work/example", "~/../work/example", "~//work/example"] {
            assert!(card(&r, &parse(&HOME_SCREEN.replace("~/work/example", folder)).unwrap()).is_none());
        }
        r["launch_dir"] = json!(home());
        assert!(card(&r, &parse(&HOME_SCREEN.replace("~/work/example", "~")).unwrap()).is_some());
        // A banner repaint does not change a decision's identity.
        let changed_banner = HOME_SCREEN.replace("Got 47 features", "Got 48 features");
        r["launch_dir"] = json!(home().join("work/example"));
        assert_eq!(question["question_hash"], card(&r, &parse(&changed_banner).unwrap()).unwrap()["question_hash"]);
        for invalid in [
            format!("User quoted:\n{HOME_SCREEN}"),
            format!("{HOME_SCREEN}\nAnother prompt"),
        ] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn windows_banner_names_the_launch_directory_below_the_platform_home() {
        // Claude Code 2.1.296 under bundled ConPTY (spike S3), in the
        // normal-screen layout; the account folder is the synthetic user01.
        const WINDOWS: &str =
            include_str!("../../tests/fixtures/claude-auto-mode-home-windows.txt");
        const HOME: &str = r"C:\Users\user01";
        let launch = r"C:\Users\user01\zerus-dev\p2\s3\homes\claude-automode\work";
        let shown = r"~\zerus-dev\p2\s3\homes\claude-automode\work";
        let mut r = record();
        r["launch_dir"] = json!(launch);
        let panel = parse(WINDOWS).unwrap();
        assert_eq!((panel.folder.as_str(), panel.selected), (shown, 0));
        let question = card_below(&r, &panel, Some(HOME)).unwrap();
        assert_eq!(
            question["questions"][0]["options"][1]["label"],
            "No, keep manual mode"
        );
        // Windows names ignore case, in the folder and in the home.
        assert!(card_below(&r, &panel, Some(r"c:\users\USER01\")).is_some());
        r["launch_dir"] = json!(launch.to_uppercase());
        assert!(card_below(&r, &panel, Some(HOME)).is_some());
        for home in [Some(r"C:\Users\other"), Some("/home/example"), None] {
            assert!(card_below(&r, &panel, home).is_none(), "{home:?}");
        }
        r["launch_dir"] = json!(HOME);
        assert!(card_below(
            &r,
            &parse(&WINDOWS.replace(shown, "~")).unwrap(),
            Some(HOME)
        )
        .is_some());
        // Outside the home Claude prints the drive path itself.
        r["launch_dir"] = json!(r"D:\src\work");
        let outside = parse(&WINDOWS.replace(shown, r"D:\src\work")).unwrap();
        assert_eq!(outside.folder, r"D:\src\work");
        assert!(card_below(&r, &outside, Some(HOME)).is_some());
        assert!(card_below(
            &r,
            &parse(&WINDOWS.replace(shown, r"D:\src\work2")).unwrap(),
            Some(HOME)
        )
        .is_none());
        for folder in [
            r"~/zerus-dev/p2/s3/homes/claude-automode/work",
            r"~\zerus-dev\p2\s3\homes\claude-automode\work2",
            r"~\..\user01\zerus-dev\p2\s3\homes\claude-automode\work",
        ] {
            r["launch_dir"] = json!(launch);
            assert!(
                card_below(
                    &r,
                    &parse(&WINDOWS.replace(shown, folder)).unwrap(),
                    Some(HOME)
                )
                .is_none(),
                "{folder}"
            );
        }
        // A Unix banner keeps its exact `~/` form below a Unix home.
        r["launch_dir"] = json!("/home/example/work/example");
        let unix = parse(include_str!(
            "../../tests/fixtures/claude-auto-mode-home.txt"
        ))
        .unwrap();
        assert!(card_below(&r, &unix, Some("/home/example")).is_some());
        assert!(card_below(&r, &unix, Some("/HOME/example")).is_none());
        assert!(card_below(&r, &unix, Some(HOME)).is_none());
    }
    #[test]
    fn highlights_are_not_identity_but_modes_runs_and_conversations_are() {
        let r = record();
        let q = card(&r, &parse(SCREEN).unwrap()).unwrap();
        let moved = SCREEN
            .replace("❯ Yes", "  Yes")
            .replace("  No, keep", "❯ No, keep");
        assert_eq!(
            q["question_hash"],
            card(&r, &parse(&moved).unwrap()).unwrap()["question_hash"]
        );
        let changed = SCREEN.replace("bypass permissions", "default permissions");
        assert_ne!(
            q["question_hash"],
            card(&r, &parse(&changed).unwrap()).unwrap()["question_hash"]
        );
        let mut changed = r.clone();
        changed["conversation_id"] = json!("other");
        assert_ne!(
            q["question_hash"],
            card(&changed, &parse(SCREEN).unwrap()).unwrap()["question_hash"]
        );
        changed["turn_started"] = json!(101);
        assert!(card(&changed, &parse(SCREEN).unwrap()).is_none());
        changed = r;
        changed["launch_dir"] = json!("/other");
        assert!(card(&changed, &parse(SCREEN).unwrap()).is_none());
    }
}
