//! A supervised agent can stop before its conversation exists at a chooser no
//! reviewed parser recognizes: a renamed option, an unreviewed update command,
//! a panel for another folder or unknown launcher output around it. Activity
//! still shows the wait: the visible panel verbatim, answered in Terminal.
//! Zerus never sends keys to it.
//!
//! One reviewed chooser stays here on purpose: the sandbox setup that Codex
//! 0.162 on Windows shows after folder trust. Zerus names it but never answers
//! it (`codex_sandbox`).
use super::*;
use sha2::{Digest, Sha256};

const MARKERS: [char; 2] = ['›', '❯'];

#[derive(Debug, PartialEq)]
struct Choice {
    title: String,
    body: String,
    options: Vec<String>,
}

// Full-width rules and box edges bound a panel.
fn boundary(row: &str) -> bool {
    row.chars().count() >= 20 && row.chars().all(|c| "─━═╭╮╰╯┌┐└┘├┤".contains(c))
}

// The highlighted row: a selection marker, one space and the option text. The
// Claude composer follows its glyph with a non-breaking space instead.
fn marked(row: &str) -> Option<&str> {
    let label = row.trim_start().strip_prefix(MARKERS)?.strip_prefix(' ')?.trim();
    (!label.is_empty()).then_some(label)
}

fn option(row: &str) -> &str {
    marked(row).unwrap_or(row)
}

fn numbered(text: &str) -> Option<usize> {
    let (number, label) = text.split_once(". ")?;
    if number.len() != 1 || !number.chars().all(|c| c.is_ascii_digit()) || label.trim().is_empty() {
        return None;
    }
    number.parse().ok()
}

// A native key hint such as "enter continue · esc quit".
fn hint(row: &str) -> bool {
    let words: Vec<_> = row.split_whitespace().map(str::to_lowercase).collect();
    row.contains('·') && words.iter().any(|w| w == "enter") && words.iter().any(|w| w == "esc")
}

fn text(rows: &[&str]) -> String {
    let mut lines: Vec<&str> = Vec::new();
    for row in rows {
        if !row.is_empty() || lines.last().is_some_and(|last| !last.is_empty()) {
            lines.push(row);
        }
    }
    lines.join("\n").trim().to_owned()
}

fn parse(screen: &str) -> Option<Choice> {
    let rows: Vec<&str> = screen.lines().map(str::trim_end).collect();
    let end = rows.iter().rposition(|row| !row.trim().is_empty())? + 1;
    // The highlighted row nearest the bottom is the live selection.
    let selected = (0..end).rev().find(|&i| marked(rows[i]).is_some())?;
    let top = rows[..selected].iter().rposition(|row| boundary(row.trim())).map_or(0, |i| i + 1);
    let bottom = rows[selected..end].iter().position(|row| boundary(row.trim())).map_or(end, |i| selected + i);
    let panel: Vec<&str> = rows[top..bottom].iter().map(|row| row.trim()).collect();
    let selected = selected - top;
    if numbered(option(panel[selected])).is_none() {
        // Unnumbered choices cannot be told apart from their descriptions, so
        // the panel stays verbatim. Its native key hint shows it is a chooser.
        if !panel.iter().any(|row| hint(row)) {
            return None;
        }
        let title = panel.iter().position(|row| !row.is_empty())?;
        return Some(Choice { title: panel[title].into(), body: text(&panel[title + 1..]), options: Vec::new() });
    }
    // Consecutive numbered options with exactly one highlighted. A row directly
    // below an option continues it; a blank row ends the list unless the next
    // option follows. Only a short footer, such as a key hint, may follow.
    let first = (0..=selected).rev().find(|&i| numbered(option(panel[i])) == Some(1))?;
    let (mut options, mut highlighted, mut index): (Vec<String>, usize, usize) = (Vec::new(), 0, first);
    while index < panel.len() {
        let row = panel[index];
        if row.is_empty() {
            let next = panel[index..].iter().position(|row| !row.is_empty()).map(|i| index + i);
            match next {
                Some(next) if numbered(option(panel[next])) == Some(options.len() + 1) => index = next,
                _ => break,
            }
            continue;
        }
        if numbered(option(row)) == Some(options.len() + 1) {
            highlighted += usize::from(marked(row).is_some());
            options.push(option(row).split_once(". ")?.1.trim().to_owned());
        } else if numbered(option(row)).is_some() || marked(row).is_some() {
            return None;
        } else {
            let last = options.last_mut()?;
            last.push(' ');
            last.push_str(row);
        }
        index += 1;
    }
    if highlighted != 1 || options.len() < 2 || selected >= index
        || panel[index..].iter().filter(|row| !row.is_empty()).count() > 2
    {
        return None;
    }
    let title = panel[..first].iter().position(|row| !row.is_empty());
    Some(Choice {
        title: title.map_or_else(String::new, |title| panel[title].to_owned()),
        body: title.map_or_else(String::new, |title| text(&panel[title + 1..first])),
        options,
    })
}

// Codex 0.162 on Windows asks how to set up its agent sandbox after folder
// trust (spike S3, 2026-10-13). Either setup changes this computer: one needs
// Administrator permissions, and Codex itself calls the other higher risk. It
// is a known startup choice that Zerus never accepts on anyone's behalf.
const CODEX_SANDBOX: &str = "Set up the Codex agent sandbox to protect your files and control network access. Learn more <https://developers.openai.com/codex/windows>";
const CODEX_SANDBOX_OPTIONS: [&str; 3] = [
    "Set up default sandbox (requires Administrator permissions)",
    "Use non-admin sandbox (higher risk if prompt injected)",
    "Quit",
];
const CODEX_SANDBOX_REASON: &str = "Zerus never chooses how Codex sets up its sandbox: one option needs Administrator permissions and Codex calls the other higher risk. Answer this in Terminal.";

/// The complete reviewed sandbox chooser below the Codex session header.
fn codex_sandbox(choice: &Choice) -> bool {
    let words = |text: &str| text.split_whitespace().collect::<Vec<_>>().join(" ");
    choice.title.starts_with(">_ OpenAI Codex (v")
        && choice.title.ends_with(')')
        && choice.options == CODEX_SANDBOX_OPTIONS
        && choice
            .body
            .rsplit("\n\n")
            .next()
            .is_some_and(|prompt| words(prompt) == CODEX_SANDBOX)
}

fn native_input(name: &str) -> PathBuf {
    root().join("native-input").join(format!("{:x}.lock", Sha256::digest(name.as_bytes())))
}

/// Held while Zerus itself drives a native picker in this session, such as the
/// model picker before the first message. That picker is not a startup wait.
pub(super) fn driving(name: &str) -> Result<Guard> {
    lock(Some(&native_input(name)))
}

fn driven(name: &str) -> bool {
    !matches!(lock_until(&native_input(name), std::time::Instant::now()), Ok(Some(_)))
}

fn eligible(record: &Value) -> bool {
    AGENTS.contains(&string(record, "agent"))
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

fn agent_name(record: &Value) -> &'static str {
    match string(record, "agent") {
        "claude" => "Claude",
        "codex" => "Codex",
        _ => "Kimi",
    }
}

fn card(record: &Value, choice: &Choice) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let hash = format!("{:x}", Sha256::digest(json!([record["run_id"], record["pane"], record["pid"],
        record["process_start"], record["expected_id"], choice.title, choice.body, choice.options]).to_string()));
    let sandbox = string(record, "agent") == "codex" && codex_sandbox(choice);
    let question = if sandbox {
        "Set up the Codex agent sandbox?".to_owned()
    } else if choice.title.is_empty() {
        format!("{} is waiting for a choice", agent_name(record))
    } else {
        choice.title.clone()
    };
    let options: Vec<_> = choice.options.iter().enumerate()
        .map(|(i, label)| json!({"id":(i + 1).to_string(),"label":label})).collect();
    Some(json!({"question_id":format!("startup-choice:{hash}"),"question_hash":hash,
        "tool_call_id":if sandbox { "codex_sandbox_setup" } else { "" },
        "agent_id":"main","run_id":record["run_id"],"conversation_id":record["conversation_id"],"created_at":0,
        "source":"startup_terminal_choice","can_answer":false,"answer_transport":"",
        "answer_unavailable_reason":if sandbox { CODEX_SANDBOX_REASON } else { "Answer this in Terminal." },
        "questions":[{"id":"terminal","header":"Terminal","question":question,"body":choice.body,
            "multi_select":false,"allow_other":false,"options":options}]}))
}

/// Callers check the reviewed startup parsers first; this only reports a wait
/// none of them can answer.
pub(super) fn current(record: &Value) -> Option<Value> {
    if !eligible(record) || driven(string(record, "name")) {
        return None;
    }
    let record = kimi_tui_choice::identity(record).ok()?;
    card(&record, &parse(&kimi_tui_choice::capture(&record).ok()?)?)
}

pub(super) fn detail(record: &Value, question: &Value) -> String {
    if question["tool_call_id"] == "codex_sandbox_setup" {
        return "Codex is waiting for its sandbox setup choice in Terminal".to_owned();
    }
    format!("{} is waiting for a choice in Terminal", agent_name(record))
}

#[cfg(test)]
mod tests {
    use super::*;
    const CODEX_TRUST: &str = include_str!("../../tests/fixtures/codex-folder-trust.txt");
    const CODEX_UPDATE: &str = include_str!("../../tests/fixtures/codex-update-prompt.txt");
    const CLAUDE_TRUST: &str = include_str!("../../tests/fixtures/claude-folder-trust.txt");
    const KIMI_TRUST: &str = include_str!("../../tests/fixtures/kimi-folder-trust.txt");
    fn record(agent: &str) -> Value {
        json!({"agent":agent,"run_id":"run","run_identity_version":1,"supervisor":{},"launch_dir":"/work/example"})
    }
    #[test]
    fn numbered_choices_keep_native_title_text_and_labels() {
        let renamed = parse(&CODEX_TRUST.replace("Trust and continue", "Trust and start")).unwrap();
        assert_eq!(renamed.title, "Folder access");
        assert!(renamed.body.starts_with("/work/example\n\nTrust this folder? Codex can read"));
        assert_eq!(renamed.options, ["Trust and start", "Quit"]);
        let update = parse(&CODEX_UPDATE.replace("sh -c 'curl", "fish -c 'curl")).unwrap();
        assert_eq!(update.title, "Update available · 0.160.0 → 0.160.1");
        assert_eq!(update.options[0], "Update now (runs `fish -c 'curl -fsSL https://chatgpt.com/codex/install.sh | CODEX_NON_INTERACTIVE=1 sh'`)");
        assert_eq!(update.options[1..], ["Skip", "Skip until next version"]);
    }
    #[test]
    fn unnumbered_choices_stay_verbatim_inside_their_frame() {
        let claude = parse(&CLAUDE_TRUST.replace("No, exit", "No, quit")).unwrap();
        assert_eq!(claude.title, "Accessing workspace:");
        assert!(claude.body.contains("17 tool permissions") && claude.body.contains("❯ No, quit"));
        assert!(claude.options.is_empty());
        let kimi = parse(&format!("unrelated launcher output\n{KIMI_TRUST}")).unwrap();
        assert_eq!(kimi.title, "Trust this folder?");
        assert!(!kimi.body.contains("launcher") && kimi.body.ends_with("Exit Kimi Code. Asked again next launch."));
    }
    #[test]
    fn composers_and_incomplete_lists_are_not_choices() {
        let codex_idle = "╭────────────────────────────────────────────╮\n│ >_ OpenAI Codex (v0.162.1)                 │\n\
            ╰────────────────────────────────────────────╯\n\n  To get started, describe a task or try one of these commands:\n\n\
            \x20 /init - create an AGENTS.md file with instructions for Codex\n\n› Ask Codex to do anything\n\n\
            \x20 ? for shortcuts                                         100% context left\n";
        let claude_idle = "────────────────────────────────────────\n❯\u{a0}\n────────────────────────────────────────\n  ? for shortcuts\n";
        for screen in [codex_idle.to_owned(), codex_idle.replace("Ask Codex to do anything", "1. first step"),
            claude_idle.to_owned(), String::new(), CODEX_TRUST.replace("  2. Quit", "› 2. Quit"),
            CODEX_TRUST.replace("  2. Quit", "  3. Quit"), format!("{CODEX_TRUST}\nmore\noutput\nhere"),
            CLAUDE_TRUST.replace(" Enter to confirm · Esc to cancel", "")] {
            assert!(parse(&screen).is_none(), "{screen}");
        }
    }
    #[test]
    fn codex_windows_sandbox_setup_is_known_but_never_answered() {
        // Codex 0.162.0 under bundled ConPTY after folder trust (spike S3).
        const SANDBOX: &str = include_str!("../../tests/fixtures/codex-sandbox-setup-windows.txt");
        let choice = parse(SANDBOX).unwrap();
        assert_eq!(choice.title, ">_ OpenAI Codex (v0.162.0)");
        assert_eq!(choice.options, CODEX_SANDBOX_OPTIONS);
        assert!(choice
            .body
            .starts_with(r"~\zerus-dev\p2\s3\homes\codex-trust\work"));
        assert!(codex_sandbox(&choice));
        let question = card(&record("codex"), &choice).unwrap();
        let native = &question["questions"][0];
        assert_eq!(native["question"], "Set up the Codex agent sandbox?");
        assert_eq!(native["body"], choice.body);
        assert_eq!(native["options"][1]["label"], CODEX_SANDBOX_OPTIONS[1]);
        assert_eq!(
            (
                question["can_answer"].clone(),
                question["source"].clone(),
                question["answer_transport"].clone()
            ),
            (json!(false), json!("startup_terminal_choice"), json!(""))
        );
        assert_eq!(question["answer_unavailable_reason"], CODEX_SANDBOX_REASON);
        assert_eq!(
            detail(&record("codex"), &question),
            "Codex is waiting for its sandbox setup choice in Terminal"
        );
        // Changed wording, options or header are an unreviewed chooser, still read-only.
        for screen in [
            SANDBOX.replace("control network access", "control access"),
            SANDBOX.replace("(higher risk if prompt injected)", "(recommended)"),
            SANDBOX.replace("  3. Quit", "  3. Quit\n  4. Skip"),
            SANDBOX.replace(">_ OpenAI Codex (v0.162.0)", "Quoted panel"),
        ] {
            let other = parse(&screen).unwrap();
            assert!(!codex_sandbox(&other), "{screen}");
            let question = card(&record("codex"), &other).unwrap();
            assert_eq!(
                (
                    question["can_answer"].clone(),
                    question["tool_call_id"].clone()
                ),
                (json!(false), json!(""))
            );
            assert_eq!(
                detail(&record("codex"), &question),
                "Codex is waiting for a choice in Terminal"
            );
        }
        assert_eq!(card(&record("kimi"), &choice).unwrap()["tool_call_id"], "");
    }
    #[test]
    fn read_only_card_pins_run_and_visible_panel_before_a_conversation() {
        let choice = parse(&CODEX_TRUST.replace("Trust and continue", "Trust and start")).unwrap();
        let question = card(&record("codex"), &choice).unwrap();
        assert_eq!((question["can_answer"].clone(), question["source"].clone()), (json!(false), json!("startup_terminal_choice")));
        assert_eq!(question["questions"][0]["options"][0]["label"], "Trust and start");
        let mut other = record("codex");
        other["run_id"] = json!("other");
        assert_ne!(question["question_hash"], card(&other, &choice).unwrap()["question_hash"]);
        for (field, value) in [("conversation_id", json!("started")), ("last_event_at", json!(1.)),
            ("supervisor", Value::Null), ("agent", json!("other"))] {
            let mut changed = record("codex");
            changed[field] = value;
            assert!(card(&changed, &choice).is_none());
        }
        let untitled = Choice { title: String::new(), body: String::new(), options: vec!["A".into(), "B".into()] };
        assert_eq!(card(&record("kimi"), &untitled).unwrap()["questions"][0]["question"], "Kimi is waiting for a choice");
    }
}
