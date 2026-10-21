//! Kimi's cache-expiry chooser lives in the TUI, outside its wire interactions.
//! Read only its complete editor-replacement panel. Never choose or resend a
//! stashed prompt without the user's explicit answer.
use super::kimi_tui_choice::{capture, identity};
use super::*;
use sha2::{Digest, Sha256};
const LABELS: [&str; 4] = [
    "Compact and continue",
    "Start a new session",
    "Continue as-is",
    "Don't ask me again",
];
const DESCRIPTIONS: [&str; 4] = [
    "One-time compact cost; cheapest way to keep this topic.",
    "Start with empty context; best for a new task.",
    "Keep the full history; highest cost per turn.",
    "Disable this reminder in Kimi's settings and continue.",
];
const EXPLANATION: &str =
    "Cache expired — the next message re-sends the entire history at full price.";
#[derive(Debug)]
struct Panel {
    title: String,
    selected: usize,
}
fn border(row: &str) -> bool {
    row.trim().chars().count() >= 40 && row.trim().chars().all(|c| c == '─')
}
fn footer(rows: &[&str]) -> bool {
    let rows: Vec<_> = rows
        .iter()
        .map(|r| r.trim())
        .filter(|r| !r.is_empty())
        .collect();
    rows.len() <= 2
        && rows.iter().all(|r| {
            // Kimi hides shortcut hints when the model/directory footer
            // fills the terminal width. Its permission label remains.
            r.starts_with("context: ")
                || r.contains("ctrl+o expand")
                || ["Always Ask", "Ask When Needed", "Never Ask"]
                    .iter()
                    .any(|mode| {
                        r.strip_prefix(mode)
                            .is_some_and(|tail| tail.starts_with(' ') && !tail.trim().is_empty())
                    })
        })
}
fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.lines().map(str::trim_end).collect();
    let start = rows.iter().rposition(|r| {
        r.trim_start()
            .starts_with("This session has been idle for ")
    })?;
    let title = rows[start].trim();
    if !title.contains(" and is ~")
        || !title.ends_with(" tokens.")
        || start == 0
        || !border(rows[start - 1])
    {
        return None;
    }
    let end = start + 9;
    if end >= rows.len()
        || rows[start + 1].trim() != "↑↓ navigate · Enter select · Esc cancel"
        || !rows[start + 2].trim().is_empty()
        || rows[start + 3].trim() != EXPLANATION
        || !rows[start + 8].trim().is_empty()
        || !border(rows[end])
        || !footer(&rows[end + 1..])
    {
        return None;
    }
    let mut selected = None;
    for (index, label) in LABELS.iter().enumerate() {
        let row = rows[start + 4 + index].trim_start();
        let text = if let Some(text) = row.strip_prefix("❯ ") {
            if selected.replace(index).is_some() {
                return None;
            }
            text
        } else {
            row
        };
        let suffix = text.strip_prefix(label)?;
        if !suffix.is_empty() && !suffix.starts_with("    ") {
            return None;
        }
    }
    Some(Panel {
        title: title.into(),
        selected: selected?,
    })
}
fn card(record: &Value, panel: &Panel) -> Value {
    let hash = format!(
        "{:x}",
        Sha256::digest(
            json!([
                record["run_id"],
                record["conversation_id"],
                record["last_main_progress_at"],
                panel.title,
                LABELS
            ])
            .to_string()
        )
    );
    json!({"question_id":format!("kimi-cache:{hash}"),"question_hash":hash,"tool_call_id":"kimi_cache_hint",
        "agent_id":"main","run_id":record["run_id"],"conversation_id":record["conversation_id"],"created_at":0,
        "source":"kimi_cache_hint","can_answer":true,"answer_transport":"kimi_tui","answer_unavailable_reason":"",
        "questions":[{"id":"cache","header":"Context","question":panel.title,"body":EXPLANATION,
        "multi_select":false,"allow_other":false,"options":LABELS.iter().enumerate().map(|(i,label)|json!({"id":format!("cache_{i}"),"label":label,"description":DESCRIPTIONS[i]})).collect::<Vec<_>>()}]})
}
pub(super) fn current(record: &Value) -> Option<Value> {
    if string(record, "agent") != "kimi"
        || string(record, "conversation_id").is_empty()
        || pause_active(record)
    {
        return None;
    }
    let panel = parse(&capture(record).ok()?)?;
    if !process_alive(record) || !matches(record, live().ok()?.get(string(record, "name"))) {
        return None;
    }
    input::checked_terminal(record, false).ok()?;
    Some(card(record, &panel))
}
pub(super) fn available(record: &Value, question: &Value) -> Result<()> {
    let record = identity(record)?;
    let panel = parse(&capture(&record)?)
        .ok_or("Kimi's context choice is no longer visible; nothing was sent")?;
    if card(&record, &panel)["question_hash"] != question["question_hash"] {
        return Err("Kimi's context choice changed; nothing was sent".into());
    }
    Ok(())
}
pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let chosen = answers[0]["selected_option_ids"][0]
        .as_str()
        .and_then(|id| id.strip_prefix("cache_"))
        .and_then(|v| v.parse::<usize>().ok())
        .filter(|i| *i < 4)
        .ok_or("Choose one of Kimi's context options")?;
    kimi_tui_choice::answer(
        record,
        question,
        chosen,
        LABELS.len(),
        |record, screen| {
            let panel = parse(screen)?;
            Some((
                card(record, &panel)["question_hash"].clone(),
                panel.selected,
            ))
        },
        |screen| {
            !screen.contains("↑↓ navigate · Enter select · Esc cancel")
                && !screen.contains("Compact and continue")
        },
        false,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    fn screen(selected: usize) -> String {
        let b = "─".repeat(120);
        format!("previous activity\n{b}\n This session has been idle for 2h and is ~217k tokens.\n ↑↓ navigate · Enter select · Esc cancel\n\n {EXPLANATION}\n{}\n\n{b}\n\n",LABELS.iter().enumerate().map(|(i,l)|format!("  {} {l}    description",if i==selected{"❯"}else{" "})).collect::<Vec<_>>().join("\n"))
    }
    #[test]
    fn only_complete_active_chooser_is_recognized() {
        for i in 0..4 {
            assert_eq!(parse(&screen(i)).unwrap().selected, i);
        }
        assert!(
            parse(&screen(0).replace("Start a new session", "Some different option")).is_none()
        );
        assert!(parse(&(screen(0) + " > composer")).is_none());
        assert!(parse(
            &(screen(0)
                + " Ask When Needed K3 thinking: max ctrl+o expand\n context: 22% (217k/1M)")
        )
        .is_some());
        assert!(parse(&screen(0).replace(" tokens.", " tok…")).is_none());
        assert!(parse(&screen(0).replace("    Continue as-is", "  ❯ Continue as-is")).is_none());
        assert!(parse("The terminal may ask: Compact and continue").is_none());
    }
    #[test]
    fn native_narrow_footer_does_not_require_shortcut_hints() {
        let screen = include_str!("../../tests/fixtures/kimi-cache-hint-narrow.txt");
        let panel = parse(screen).expect("native chooser with optional shortcut hidden");
        assert_eq!(panel.selected, 0);
        assert_eq!(
            panel.title,
            "This session has been idle for 2d 13h and is ~217k tokens."
        );
        for mode in ["Always Ask", "Never Ask"] {
            assert!(parse(&screen.replace("Ask When Needed", mode)).is_some());
        }
        assert!(parse(&screen.replace("Ask When Needed", "Unknown prompt")).is_none());
        assert!(parse(&(screen.to_owned() + " > new composer")).is_none());
    }
    #[test]
    fn choice_does_not_change_identity_but_new_run_does() {
        let record = json!({"run_id":"one","conversation_id":"session"});
        let a = card(&record, &parse(&screen(0)).unwrap());
        let b = card(&record, &parse(&screen(3)).unwrap());
        assert_eq!(a["question_hash"], b["question_hash"]);
        assert_eq!(a["questions"][0]["allow_other"], false);
        assert_ne!(
            a["question_hash"],
            card(
                &json!({"run_id":"two","conversation_id":"session"}),
                &parse(&screen(0)).unwrap()
            )["question_hash"]
        );
    }
}
