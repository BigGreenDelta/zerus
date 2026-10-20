//! Codex's startup trust panel precedes SessionStart and has no conversation ID.
//! Only a complete panel in a supervised startup run can be answered. Its path
//! and full permission disclosure are part of the identity, never an implicit approval.
//! The frame (title, folder, exact choices and hint) must match; the disclosure
//! between them is shown verbatim, so a new native note cannot hide the request.
//! On Windows the folder is a drive path compared without case (`shown_path`),
//! and Codex 0.162 adds its Git root note with the root lower-cased even when
//! the folder is that root: there a note naming the folder names no other root.
//! A root in another form than the folder (`/c/…` for a drive folder) is
//! unfamiliar text, so the panel keeps only the native wording.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "Folder access";
const HINT: &str = "enter continue · esc quit";
const EXPLANATION: &str = "Trust this folder? Codex can read, edit, and run files here, subject to your permission settings. Folder settings can run code automatically, even without a model request. Continue only if you trust these files. Your trust decision will be saved.";
const GIT_ROOT: &str = "Note: You’re in a subdirectory of a Git project. Trusting will apply to the repository root:";
const LABELS: [&str; 2] = ["Trust and continue", "Quit"];
const DESCRIPTIONS: [&str; 2] = ["Trust this folder and remember the decision.", "Exit without trusting this folder."];

struct Panel {
    folder: String,
    disclosure: String,
    // Codex trusts this repository root instead of the launch folder.
    root: Option<String>,
    // Every paragraph is reviewed native text, so our paraphrases are accurate.
    known: bool,
    selected: usize,
}

fn words(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.trim().lines().map(str::trim).collect();
    let filled: Vec<_> = (0..rows.len()).filter(|&i| !rows[i].is_empty()).collect();
    if filled.len()<6 || rows[0]!=TITLE || !shown_path::absolute(rows[1]) || *rows.last()?!=HINT {
        return None;
    }
    let options=filled.len()-3;
    let mut selected=None;
    for i in 0..2 {
        let row=rows[filled[options+i]];
        let label=if let Some(label)=row.strip_prefix("› ") {
            if selected.replace(i).is_some(){return None;} label
        } else {row};
        if label!=format!("{}. {}",i+1,LABELS[i]) {return None;}
    }
    // Wrapped rows are rejoined per paragraph; a clipped disclosure is incomplete.
    let paragraphs: Vec<_> = rows[2..filled[options]].split(|r| r.is_empty()).filter(|p| !p.is_empty()).collect();
    if paragraphs.is_empty() || paragraphs.concat().iter().any(|r| r.contains('…')) {return None;}
    // The root has the folder's own form: a Unix path, or a Windows drive path.
    let windows=shown_path::drive(rows[1]);
    let note=|p: &[&str]| match p {
        [text @ .., path] if (if windows {shown_path::drive(path)} else {path.starts_with('/')}) && words(&text.join(" "))==GIT_ROOT => Some(path.to_string()),
        _ => None,
    };
    let (mut root, mut noted, mut explained, mut known)=(None, false, false, true);
    for p in &paragraphs {
        if !explained && words(&p.join(" "))==EXPLANATION {explained=true;}
        else if let Some(path)=note(p).filter(|_| !noted) {
            noted=true;
            // Only Windows Codex names the folder itself (lower-cased) as root.
            if !(windows && shown_path::same(&path, rows[1])) {root=Some(path);}
        }
        else {known=false;}
    }
    let disclosure=paragraphs.iter().map(|p| p.join(" ")).collect::<Vec<_>>().join("\n\n");
    Some(Panel{folder:rows[1].into(),disclosure,root,known:known&&explained,selected:selected?})
}

// Unreviewed text keeps the native labels and verbatim disclosure only.
fn wording(panel: &Panel) -> (&'static str, [String; 2]) {
    match (&panel.root, panel.known) {
        (_, false) => ("Allow Codex folder access?", [String::new(), String::new()]),
        (None, true) => ("Trust this folder?", DESCRIPTIONS.map(str::to_owned)),
        (Some(root), true) => ("Trust this Git repository?", [
            format!("Trust the repository root {root} and remember the decision."),
            "Exit without trusting this repository.".into(),
        ]),
    }
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
    if !eligible(record) || !shown_path::same(&panel.folder, string(record, "launch_dir")) {
        return None;
    }
    let (question, descriptions) = wording(panel);
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
                descriptions
            ])
            .to_string()
        )
    );
    Some(
        json!({"question_id":format!("codex-trust:{hash}"),"question_hash":hash,
        "tool_call_id":"codex_folder_trust","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"codex_folder_trust",
        "can_answer":true,"answer_transport":"codex_tui","answer_unavailable_reason":"",
        "questions":[{"id":"trust","header":"Folder trust","question":question,
            "body":format!("{}\n\n{}",panel.folder,panel.disclosure),"multi_select":false,"allow_other":false,
            "options":LABELS.iter().enumerate().map(|(i,label)|json!({"id":format!("trust_{i}"),"label":label,"description":descriptions[i]})).collect::<Vec<_>>()}]}),
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
        return Err("Codex's folder trust request changed; nothing was sent".into());
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
    const SCREEN: &str = include_str!("../../tests/fixtures/codex-folder-trust.txt");
    const GIT_ROOT_SCREEN: &str = include_str!("../../tests/fixtures/codex-folder-trust-git-root.txt");
    fn record() -> Value {
        json!({"agent":"codex","run_id":"run","run_identity_version":1,"supervisor":{},"launch_dir":"/work/example"})
    }
    #[test]
    fn complete_native_panel_only() {
        assert_eq!(parse(SCREEN).unwrap().selected,0);
        let bare=SCREEN.lines().filter(|row|!row.starts_with("  Trust this")&&!row.starts_with("  settings.")&&!row.starts_with("  Continue only")).collect::<Vec<_>>().join("\n");
        for invalid in [SCREEN.replace(HINT,"enter continue"),SCREEN.replace("  2. Quit","› 2. Quit"),
            SCREEN.replace("Your trust decision will be saved.","…"),format!("Agent said:\n{SCREEN}"),format!("{SCREEN}\n› composer"),
            SCREEN.replace("  2. Quit","  2. Quit\n  3. Trust parent"),bare] {
            assert!(parse(&invalid).is_none());
        }
    }
    #[test]
    fn git_root_note_names_the_repository_root() {
        let panel=parse(GIT_ROOT_SCREEN).unwrap();
        assert_eq!(panel.root.as_deref(),Some("/work/repository"));
        let question=card(&record(),&panel).unwrap();
        let body=question["questions"][0]["body"].as_str().unwrap();
        assert!(body.starts_with("/work/example\n\nNote: You’re in a subdirectory of a Git project. Trusting will apply to the repository root: /work/repository\n\nTrust this folder?"));
        assert_eq!(question["questions"][0]["question"],"Trust this Git repository?");
        assert!(question["questions"][0]["options"][0]["description"].as_str().unwrap().contains("/work/repository"));
        assert_ne!(question["question_hash"],card(&record(),&parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        let other=parse(&GIT_ROOT_SCREEN.replace("/work/repository","/work/other")).unwrap();
        assert_ne!(question["question_hash"],card(&record(),&other).unwrap()["question_hash"]);
    }
    #[test]
    fn unfamiliar_text_is_shown_verbatim_with_native_labels_only() {
        let extra=SCREEN.replace("\n  Trust this folder?","\n  Note: A newer Codex explains\n  something else.\n\n  Trust this folder?");
        let reworded=SCREEN.replace("Your trust decision will be saved.","Your decision will be remembered.");
        for screen in [extra,reworded] {
            let panel=parse(&screen).unwrap();
            let question=card(&record(),&panel).unwrap();
            let native=&question["questions"][0];
            assert_eq!(native["question"],"Allow Codex folder access?");
            assert_eq!(native["body"],format!("/work/example\n\n{}",panel.disclosure));
            assert!(native["options"].as_array().unwrap().iter().all(|option|option["description"]==""));
            assert_eq!(native["options"][0]["label"],"Trust and continue");
            assert!(question["can_answer"].as_bool().unwrap());
            assert_ne!(question["question_hash"],card(&record(),&parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        }
        let panel=parse(&SCREEN.replace("\n  Trust this folder?","\n  Note: A newer Codex explains\n  something else.\n\n  Trust this folder?")).unwrap();
        assert!(panel.disclosure.starts_with("Note: A newer Codex explains something else.\n\nTrust this folder?"));
    }
    // Codex 0.162.0 under bundled ConPTY (spike S3); the account folder is the
    // synthetic user01. Codex adds its Git root note, lower-cased, even when
    // the folder is the repository root.
    const WINDOWS: &str = include_str!("../../tests/fixtures/codex-folder-trust-windows.txt");
    const WINDOWS_GIT_ROOT: &str = include_str!("../../tests/fixtures/codex-folder-trust-git-root-windows.txt");
    const WINDOWS_DIR: &str = r"C:\Users\user01\zerus-dev\p2\s3\homes\codex-trust\work";
    fn windows_record(folder: &str) -> Value {
        let mut record = record();
        record["launch_dir"] = json!(folder);
        record
    }
    #[test]
    fn windows_root_note_naming_the_folder_itself_names_no_other_root() {
        let panel = parse(WINDOWS).unwrap();
        assert_eq!((panel.folder.as_str(), panel.root.as_deref(), panel.known), (WINDOWS_DIR, None, true));
        let question = card(&windows_record(WINDOWS_DIR), &panel).unwrap();
        let native = &question["questions"][0];
        assert_eq!(native["question"], "Trust this folder?");
        assert_eq!(native["options"][0]["description"], DESCRIPTIONS[0]);
        // The native note stays visible verbatim in the body.
        assert!(native["body"].as_str().unwrap().starts_with(&format!(
            "{WINDOWS_DIR}\n\nNote: You’re in a subdirectory of a Git project. Trusting will apply to the repository root: {}\n\nTrust this folder?",
            WINDOWS_DIR.to_lowercase())));
        // Windows names ignore case; Unix and other folders do not match.
        assert!(card(&windows_record(&WINDOWS_DIR.to_uppercase()), &panel).is_some());
        for folder in [r"C:\Users\user01\zerus-dev\p2\s3\homes\codex-trust", "/work/example", r"D:\work"] {
            assert!(card(&windows_record(folder), &panel).is_none(), "{folder}");
        }
        // Only one note is reviewed: a second one makes the text unfamiliar.
        let second = WINDOWS.replace("\n\n  Trust this folder?",
            "\n\n  Note: You’re in a subdirectory of a Git project. Trusting will apply to the repository root:\n  c:\\users\\user01\n\n  Trust this folder?");
        let panel = parse(&second).unwrap();
        assert_eq!((panel.root.as_deref(), panel.known), (None, false));
        assert_eq!(card(&windows_record(WINDOWS_DIR), &panel).unwrap()["questions"][0]["question"], "Allow Codex folder access?");
        // Unix notes read as before, even one naming the folder itself.
        for root in ["/WORK/example", "/work/example"] {
            assert_eq!(parse(&GIT_ROOT_SCREEN.replace("/work/repository", root)).unwrap().root.as_deref(), Some(root));
        }
    }
    #[test]
    fn windows_git_root_note_names_the_lower_cased_repository_root() {
        let folder = r"C:\Users\user01\zerus-dev\p2\s3\homes\codex-trust-gitroot\repo\sub";
        let root = r"c:\users\user01\zerus-dev\p2\s3\homes\codex-trust-gitroot\repo";
        let panel = parse(WINDOWS_GIT_ROOT).unwrap();
        assert_eq!((panel.folder.as_str(), panel.root.as_deref(), panel.known), (folder, Some(root), true));
        let question = card(&windows_record(folder), &panel).unwrap();
        assert_eq!(question["questions"][0]["question"], "Trust this Git repository?");
        assert_eq!(question["questions"][0]["options"][0]["description"],
            format!("Trust the repository root {root} and remember the decision."));
        assert!(card(&windows_record(&root.replace(r"\repo", r"\Repo")), &panel).is_none());
        assert!(parse(&WINDOWS_GIT_ROOT.replace(&format!("  {root}\n"), "  \\\\server\\share\\repo\n")).is_some_and(|panel| !panel.known));
        // A root in another path form than the folder is not a reviewed note.
        let msys = parse(&WINDOWS_GIT_ROOT.replace(&format!("  {root}\n"), "  /c/users/user01/repo\n")).unwrap();
        assert_eq!((msys.root, msys.known), (None, false));
        let drive = parse(&GIT_ROOT_SCREEN.replace("/work/repository", r"C:\work\repository")).unwrap();
        assert_eq!((drive.root, drive.known), (None, false));
    }
    #[test]
    fn startup_identity_pins_folder_and_run_but_not_highlight() {
        let record=record();
        let question=card(&record,&parse(SCREEN).unwrap()).unwrap();
        assert_eq!(question["questions"][0]["question"],"Trust this folder?");
        assert_eq!(question["questions"][0]["options"][0]["description"],DESCRIPTIONS[0]);
        let moved=SCREEN.replace("› 1.","  1.").replace("  2. Quit","› 2. Quit");
        assert_eq!(question["question_hash"],card(&record,&parse(&moved).unwrap()).unwrap()["question_hash"]);
        let mut other=record.clone();other["run_id"]=json!("other");
        assert_ne!(question["question_hash"],card(&other,&parse(SCREEN).unwrap()).unwrap()["question_hash"]);
        other=record.clone();other["conversation_id"]=json!("already-started");
        assert!(card(&other,&parse(SCREEN).unwrap()).is_none());
        assert!(card(&record,&parse(&SCREEN.replace("/work/example","/other/folder")).unwrap()).is_none());
    }
}
