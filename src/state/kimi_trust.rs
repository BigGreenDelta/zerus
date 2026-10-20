//! Kimi's startup trust panel precedes SessionStart and has no conversation ID.
//! Only a complete panel in a supervised startup run can be answered. Its path
//! and full disclosure are part of the identity, never an implicit approval.
//! The frame must match; the disclosure is shown verbatim, so new wording stays visible.
//! On Windows Kimi prints the full drive path (`C:\Users\…`, never `~\…`),
//! compared with the launch directory without case (`shown_path`).
//!
//! Kimi 2.2 rewrote the panel: two plain choices below a fixed footer, and a
//! disclosure that pages, or asks for a larger terminal, when the pane is short.
//! A page shows only part of the disclosure, so only an unpaged panel counts;
//! a listing grows a pane no client views until Kimi draws it in one page.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Trust this folder?";
const HINT: &str = "↑↓ navigate · Enter select · Esc exit";
const LABELS: [&str; 2] = ["Trust this folder", "Don't trust"];
const DESCRIPTIONS: [&str; 2] = [
    "Enable project MCP servers. Remembered for this folder.",
    "Exit Kimi Code. Asked again next launch.",
];
/// Kimi 2.2's panel.
const PAGED_LABELS: [&str; 2] = ["Trust and continue", "Exit"];
const FOOTER: &str = "Trust is remembered for this folder, including future project config \
changes. Tool approvals follow your permission settings.";
const ENLARGE: &str = "Enlarge terminal to review sources. Esc exit.";

#[derive(Clone, Copy, Debug, PartialEq)]
enum Layout {
    /// Kimi 2.1: described choices below the whole disclosure.
    Described,
    /// Kimi 2.2: plain choices below FOOTER, with a paged disclosure.
    Paged,
}

impl Layout {
    fn labels(self) -> [&'static str; 2] {
        match self {
            Layout::Described => LABELS,
            Layout::Paged => PAGED_LABELS,
        }
    }
    fn descriptions(self) -> [&'static str; 2] {
        match self {
            Layout::Described => DESCRIPTIONS,
            Layout::Paged => ["", ""],
        }
    }
}

struct Panel {
    layout: Layout,
    folder: String,
    disclosure: String,
    selected: usize,
}

/// What a screen shows of the trust panel.
enum Shown {
    Complete(Panel),
    /// Its top scrolled off, or Kimi shows a page of it or asks for a larger
    /// terminal: never answerable as shown, but a taller pane shows it all.
    Clipped,
}

fn border(row: &str) -> bool {
    row.chars().count() >= 40 && row.chars().all(|c| c == '─')
}

/// A selected choice row and whether `label` ends it; a row selects one choice.
fn choice(row: &str, label: &str, selected: &mut Option<usize>, index: usize) -> bool {
    match row.strip_prefix("❯ ") {
        Some(shown) => shown == label && selected.replace(index).is_none(),
        None => row == label,
    }
}

fn page_row(row: &str) -> bool {
    row.strip_prefix("←→ page · ")
        .and_then(|page| page.split_once(" / "))
        .is_some_and(|(page, count)| {
            [page, count]
                .iter()
                .all(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
        })
}

fn read(screen: &str) -> Option<Shown> {
    let all: Vec<_> = screen.trim().lines().map(str::trim).collect();
    if let [notices @ .., title, enlarge] = all.as_slice() {
        if *title == TITLE
            && *enlarge == ENLARGE
            && (notices.is_empty() || kimi_tui_choice::launcher_notices(notices, "kimi"))
        {
            return Some(Shown::Clipped);
        }
    }
    // Kimi 2.1 keeps no scrollback for a panel taller than its pane: only its
    // described choices show, never its title.
    let scrolled = || (described_bottom(&all) && !all.contains(&TITLE)).then_some(Shown::Clipped);
    let divider = all.iter().position(|row| border(row))?;
    if divider > 0 && !kimi_tui_choice::launcher_notices(&all[..divider], "kimi") {
        return scrolled();
    }
    let rows = &all[divider..];
    if rows.len() < 10
        || !border(rows.last()?)
        || rows[1] != TITLE
        || rows[2] != HINT
        || !rows[3].is_empty()
        || !shown_path::absolute(rows[4])
    {
        return scrolled();
    }
    let (layout, disclosure, selected) = described(rows).or_else(|| paged(rows))?;
    let Some(disclosure) = disclosure else {
        return Some(Shown::Clipped);
    };
    // Kimi asks for every untrusted folder and lists targets only when the
    // project's MCP configuration gates some. A clipped list is incomplete.
    if !rows[5].is_empty() || disclosure.is_empty() || disclosure.contains('…') {
        return None;
    }
    Some(Shown::Complete(Panel {
        layout,
        folder: rows[4].into(),
        disclosure,
        selected,
    }))
}

/// Kimi 2.1's described choices end the screen: the panel's bottom.
fn described_bottom(rows: &[&str]) -> bool {
    let Some(start) = rows.len().checked_sub(7) else {
        return false;
    };
    let mut selected = None;
    border(rows[rows.len() - 1])
        && (0..2).all(|i| {
            choice(rows[start + i * 3], LABELS[i], &mut selected, i)
                && rows[start + i * 3 + 1] == DESCRIPTIONS[i]
                && rows[start + i * 3 + 2].is_empty()
        })
        && selected.is_some()
}

/// Kimi 2.1: the disclosure and described choices, the selected one.
fn described(rows: &[&str]) -> Option<(Layout, Option<String>, usize)> {
    // The complete, fixed choice footer prevents interpreting a clipped prompt
    // or a quoted transcript as the active dialog. No choice is preselected in UI.
    if rows.len() < 15 || !described_bottom(rows) {
        return None;
    }
    let options = rows.len() - 7;
    let selected = (0..2).find(|i| rows[options + i * 3].starts_with("❯ "))?;
    let disclosure = rows[6..options].join("\n").trim().to_owned();
    Some((Layout::Described, Some(disclosure), selected))
}

/// Kimi 2.2: the disclosure above FOOTER and plain choices, or None for the
/// disclosure when only a page of it shows.
fn paged(rows: &[&str]) -> Option<(Layout, Option<String>, usize)> {
    let n = rows.len();
    let mut selected = None;
    if !(0..2).all(|i| choice(rows[n - 3 + i], PAGED_LABELS[i], &mut selected, i))
        || !rows[n - 4].is_empty()
    {
        return None;
    }
    // FOOTER wraps on a narrow pane; its rows follow a blank or the page row.
    let footer = rows[5..n - 4]
        .iter()
        .rposition(|row| row.is_empty() || page_row(row))?
        + 6;
    if rows[footer..n - 4].join(" ") != FOOTER {
        return None;
    }
    let disclosure =
        (!page_row(rows[footer - 1])).then(|| rows[6..footer].join("\n").trim().to_owned());
    Some((Layout::Paged, disclosure, selected?))
}

fn parse(screen: &str) -> Option<Panel> {
    match read(screen)? {
        Shown::Complete(panel) => Some(panel),
        Shown::Clipped => None,
    }
}

/// A trust panel that a taller pane would show completely.
fn clipped(screen: &str) -> bool {
    matches!(read(screen), Some(Shown::Clipped))
}

fn complete(screen: &str) -> bool {
    parse(screen).is_some()
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
    let (labels, descriptions) = (panel.layout.labels(), panel.layout.descriptions());
    let mut identity = json!([
        record["run_id"],
        record["pane"],
        record["pid"],
        record["process_start"],
        record["expected_id"],
        panel.folder,
        panel.disclosure,
        labels,
        descriptions
    ]);
    let mut body = format!("{}\n\n{}", panel.folder, panel.disclosure);
    if panel.layout == Layout::Paged {
        identity.as_array_mut()?.push(json!(FOOTER));
        body = format!("{body}\n\n{FOOTER}");
    }
    let hash = format!("{:x}", Sha256::digest(identity.to_string()));
    let options: Vec<_> = labels
        .iter()
        .zip(descriptions)
        .enumerate()
        .map(|(i, (label, description))| {
            let mut option = json!({"id":format!("trust_{i}"),"label":label});
            if !description.is_empty() {
                option["description"] = json!(description);
            }
            option
        })
        .collect();
    Some(
        json!({"question_id":format!("kimi-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"kimi_folder_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"kimi_folder_trust",
        "can_answer":true,"answer_transport":"kimi_tui","answer_unavailable_reason":"",
        "questions":[{"id":"trust","header":"Folder trust","question":TITLE,
            "body":body,"multi_select":false,"allow_other":false,"options":options}]}),
    )
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen)?;
    Some((
        card(record, &panel)?["question_hash"].clone(),
        panel.selected,
    ))
}

/// Grow the pane no client views until the clipped or paged trust panel of
/// this startup run shows completely in one page (`fit_unviewed`); the exact
/// match in `current` still decides. Only a question listing calls it.
pub(super) fn fit(record: &Value) {
    if !eligible(record) {
        return;
    }
    let Ok(current) = kimi_tui_choice::identity(record) else {
        return;
    };
    if let Ok(screen) = kimi_tui_choice::capture(&current) {
        question_terminal::fit_unviewed(
            &current,
            screen,
            kimi_tui_choice::capture,
            clipped,
            complete,
        );
    }
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
        return Err("Kimi's folder trust request changed; nothing was sent".into());
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
        2,
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
    #[test]
    fn kimi_2_2_panel_needs_its_whole_disclosure_on_one_page() {
        const FULL: &str = include_str!("../../tests/fixtures/kimi-folder-trust-2.2.txt");
        const NARROW: &str =
            include_str!("../../tests/fixtures/kimi-folder-trust-2.2-narrow-50x30.txt");
        const PLAIN: &str = include_str!("../../tests/fixtures/kimi-folder-trust-2.2-plain.txt");
        let record = json!({"agent":"kimi","run_id":"run","run_identity_version":1,
            "supervisor":{},"launch_dir":"/work/example"});
        let panel = parse(FULL).unwrap();
        assert_eq!(
            (panel.layout, panel.selected, panel.folder.as_str()),
            (Layout::Paged, 0, "/work/example")
        );
        assert_eq!(
            panel.disclosure,
            "Start 4 MCP servers automatically\nConfig: .kimi-code/mcp.json, .mcp.json\n\n\
             Load project instructions\nCheck: AGENTS.md · .kimi-code/skills/"
        );
        assert!(complete(FULL) && !clipped(FULL));
        let question = card(&record, &panel).unwrap();
        let item = &question["questions"][0];
        assert_eq!(
            item["options"],
            json!([{"id":"trust_0","label":"Trust and continue"},{"id":"trust_1","label":"Exit"}])
        );
        assert_eq!(
            item["body"],
            format!("/work/example\n\n{}\n\n{FOOTER}", panel.disclosure)
        );
        // A narrow pane wraps the footer; the request is the same.
        assert_eq!(card(&record, &parse(NARROW).unwrap()).unwrap(), question);
        assert_eq!(
            parse(PLAIN).unwrap().disclosure,
            "No project integrations or instructions to activate."
        );
        let moved = FULL
            .replace("❯ Trust and continue", "  Trust and continue")
            .replace("     Exit", "   ❯ Exit");
        let moved = parse(&moved).unwrap();
        assert_eq!(moved.selected, 1);
        assert_eq!(
            card(&record, &moved).unwrap()["question_hash"],
            question["question_hash"]
        );
        // A page or a too short pane shows part of it: grow, never answer.
        for screen in [
            include_str!("../../tests/fixtures/kimi-folder-trust-2.2-page-80x16.txt"),
            include_str!("../../tests/fixtures/kimi-folder-trust-2.2-page-80x12.txt"),
            include_str!("../../tests/fixtures/kimi-folder-trust-2.2-enlarge-80x8.txt"),
        ] {
            assert!(parse(screen).is_none() && clipped(screen), "{screen}");
        }
        for invalid in [
            FULL.replace("Tool approvals follow", "Tools follow"),
            FULL.replace("     Exit", "     Leave"),
            FULL.replace("     Exit", "   ❯ Exit"),
            FULL.replace("mcp.json\n", "mcp.json…\n"),
            format!("agent said:\n{FULL}"),
            format!("{FULL}\n > composer"),
        ] {
            assert!(parse(&invalid).is_none() && !clipped(&invalid), "{invalid}");
        }
    }
    #[test]
    fn kimi_2_1_panel_taller_than_its_pane_is_clipped() {
        const CLIPPED: &str =
            include_str!("../../tests/fixtures/kimi-folder-trust-2.1-clipped-80x24.txt");
        assert!(parse(CLIPPED).is_none() && clipped(CLIPPED) && !complete(CLIPPED));
        assert!(complete(SCREEN) && !clipped(SCREEN));
        // Its title shows, so a quoted panel is not a clipped one.
        assert!(!clipped(&format!("agent said:\n{SCREEN}")));
        assert!(!clipped(&CLIPPED.replace("Don't trust", "Leave")));
    }
}
