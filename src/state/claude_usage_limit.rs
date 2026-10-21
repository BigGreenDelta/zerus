//! Claude's usage-limit menu has no question hook. A complete native bottom
//! panel takes precedence over a failed turn or background tool activity.
//! Credits and automatic continuation always require an explicit answer.
use super::*;
use sha2::{Digest, Sha256};

const TITLE: &str = "What do you want to do?";
const STOP: &str = "Stop and wait for limit to reset";
const FOOTER: &str = "Enter to confirm · Esc to cancel";

struct Panel {
    body: String,
    labels: Vec<String>,
    selected: usize,
}

fn normalized(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn rule(row: &str) -> bool {
    let Some(first) = row.chars().next().filter(|c| matches!(c, '─' | '▔')) else {
        return false;
    };
    // Claude can put a status message inside its otherwise full-width rule.
    row.chars().take_while(|c| *c == first).count() >= 20 && row.ends_with(first)
}

fn parse(screen: &str) -> Option<Panel> {
    let rows: Vec<_> = screen.trim().lines().map(str::trim).collect();
    let top = rows.iter().rposition(|row| rule(row))?;
    let panel = &rows[top + 1..];
    if panel.len() < 5 || panel[0] != TITLE || !panel[1].is_empty() || *panel.last()? != FOOTER {
        return None;
    }
    let first = panel
        .iter()
        .position(|row| row.starts_with("1. ") || row.starts_with("❯ 1. "))?;
    if !panel[first - 1].is_empty() || !panel[panel.len() - 2].is_empty() {
        return None;
    }
    let mut labels: Vec<String> = Vec::new();
    let mut selected = None;
    for row in &panel[first..panel.len() - 2] {
        if row.is_empty() {
            continue;
        }
        let text = if let Some(text) = row.strip_prefix('❯') {
            if selected.replace(labels.len()).is_some() {
                return None;
            }
            text.trim_start()
        } else {
            row
        };
        if let Some((number, text)) = text
            .split_once(". ")
            .filter(|(n, _)| n.chars().all(|c| c.is_ascii_digit()))
        {
            if number.parse::<usize>().ok()? != labels.len() + 1 || text.is_empty() {
                return None;
            }
            labels.push(text.into());
        } else {
            if row.starts_with('❯') {
                return None;
            }
            let label = labels.last_mut()?;
            label.push(' ');
            label.push_str(text);
        }
    }
    // The generic title alone must never turn another native menu into this one.
    if labels.len() > 10
        || !labels.iter().any(|label| label == STOP)
        || labels
            .iter()
            .enumerate()
            .any(|(i, label)| labels[..i].contains(label))
    {
        return None;
    }
    // The native error and reset/help link sit above the panel's rule. They
    // remain useful even when a later background hook cleared provider_error.
    let notice = rows[..top]
        .iter()
        .rposition(|row| {
            let row = row.trim_start_matches('⎿').trim_start();
            (row.starts_with("You've hit your ") || row.starts_with("You’ve hit your "))
                && row.contains("limit")
        })
        .map(|start| {
            rows[start..top]
                .iter()
                .take(8)
                .take_while(|row| !row.is_empty())
                .map(|row| row.trim_start_matches('⎿').trim_start())
                .collect::<Vec<_>>()
                .join(" ")
        })
        .unwrap_or_default();
    let banner = rows[top].trim_matches(['─', '▔']).trim();
    let body = normalized(&[notice.as_str(), banner, &panel[2..first].join(" ")].join(" "));
    Some(Panel {
        body,
        labels,
        selected: selected?,
    })
}

fn supported(label: &str) -> bool {
    [
        STOP,
        "Switch to usage credits",
        "Switch to extra usage",
        "Wait here, then continue automatically shortly",
        "Wait here, then continue automatically when the limit resets",
    ]
    .contains(&label)
        || label
            .strip_prefix("Wait here, then continue automatically at ")
            .is_some_and(|at| !at.is_empty() && at.len() <= 100)
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
}

fn card(record: &Value, panel: &Panel) -> Option<Value> {
    if !eligible(record) {
        return None;
    }
    let failure = &record["provider_error"];
    let detail = if ["quota", "session_limit"].contains(&string(failure, "error_kind"))
        && failure["at"].as_f64().unwrap_or(0.)
            >= record["last_main_progress_at"].as_f64().unwrap_or(0.)
    {
        string(failure, "detail")
    } else {
        ""
    };
    let detail = if normalized(&panel.body).contains(&normalized(detail)) {
        ""
    } else {
        detail
    };
    let body = [detail, panel.body.as_str()]
        .into_iter()
        .filter(|s| !s.is_empty())
        .collect::<Vec<_>>()
        .join("\n\n");
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
                body,
                panel.labels
            ])
            .to_string()
        )
    );
    let can_answer = panel.labels.iter().all(|label| supported(label));
    Some(
        json!({"question_id":format!("claude-usage-limit:{hash}"),"question_hash":hash,
        "tool_call_id":"claude_usage_limit","agent_id":"main","run_id":record["run_id"],
        "conversation_id":record["conversation_id"],"created_at":0,"source":"claude_usage_limit",
        "can_answer":can_answer,"answer_transport":"claude_tui",
        "answer_unavailable_reason":if can_answer { "" } else { "Review these account or billing choices in Terminal." },
        "questions":[{"id":"usage_limit","header":"Usage limit","question":TITLE,"body":body,
            "multi_select":false,"allow_other":false,
            "options":panel.labels.iter().enumerate().map(|(i,label)|json!({"id":format!("limit_{i}"),"label":label})).collect::<Vec<_>>()}]}),
    )
}

fn snapshot(record: &Value, screen: &str) -> Option<(Value, usize)> {
    let panel = parse(screen)?;
    let card = card(record, &panel)?;
    (card["can_answer"] == true).then(|| (card["question_hash"].clone(), panel.selected))
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
        return Err("Claude's usage-limit choices changed; nothing was sent".into());
    }
    Ok(())
}

pub(super) fn answer(record: &Value, question: &Value, answers: &Value) -> Result<()> {
    let options = question["questions"][0]["options"]
        .as_array()
        .ok_or("Usage-limit choices are missing")?;
    let chosen = options
        .iter()
        .position(|option| option["id"] == answers[0]["selected_option_ids"][0])
        .ok_or("Choose one of Claude's usage-limit options")?;
    kimi_tui_choice::answer(
        record,
        question,
        chosen,
        options.len(),
        snapshot,
        |screen| !screen.trim().is_empty() && !screen.contains(TITLE) && !screen.contains(STOP),
        false,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    const SCREEN: &str = include_str!("../../tests/fixtures/claude-usage-limit.txt");
    fn record() -> Value {
        json!({"agent":"claude","run_id":"run","run_identity_version":1,"supervisor":{},
            "launch_dir":"/work/example","conversation_id":"bound","phase":"error",
            "provider_error":{"error_kind":"quota","at":20,"detail":"You've hit your weekly limit · resets Oct 20, 1pm (UTC)"}})
    }
    #[test]
    fn complete_choices_preserve_reset_and_cost_without_approval_or_default() {
        let panel = parse(SCREEN).unwrap();
        let q = card(&record(), &panel).unwrap();
        assert_eq!(q["can_answer"], true);
        assert_eq!(
            q["questions"][0]["options"][2]["label"],
            "Switch to usage credits"
        );
        assert!(q["questions"][0]["body"]
            .as_str()
            .unwrap()
            .contains("Oct 20, 1pm"));
        assert!(q.get("approval").is_none());
        assert!(q["questions"][0]["options"]
            .as_array()
            .unwrap()
            .iter()
            .all(|o| o.get("selected").is_none()));
        let moved = SCREEN.replace("❯ 1.", "1.").replace("  3.", "❯ 3.");
        assert_eq!(
            snapshot(&record(), SCREEN).unwrap().0,
            snapshot(&record(), &moved).unwrap().0
        );
        assert_eq!(parse(&moved).unwrap().selected, 2);
    }
    #[test]
    fn complete_native_panel_overrides_working_and_background_tools() {
        let mut r = record();
        r["phase"] = json!("working");
        r["active_tools"] = json!({"shell":{"name":"Bash"}});
        assert!(card(&r, &parse(SCREEN).unwrap()).is_some());
        r.as_object_mut().unwrap().remove("provider_error");
        let body = card(&r, &parse(SCREEN).unwrap()).unwrap()["questions"][0]["body"]
            .as_str()
            .unwrap()
            .to_owned();
        assert!(body.contains("weekly limit") && body.contains("claude/reset"));
        for field in ["run_id", "conversation_id"] {
            let mut r = r.clone();
            r[field] = json!("");
            assert!(!eligible(&r));
        }
        r["session_end"] = json!(20);
        assert!(!eligible(&r));
    }
    #[test]
    fn rejects_partial_quoted_and_unfamiliar_frames() {
        for invalid in [
            SCREEN.replace(FOOTER, "Enter to confirm"),
            SCREEN.replace("❯ 1.", "1."),
            SCREEN.replace("  3.", "❯ 3."),
            SCREEN.replace("  3.", "  4."),
            SCREEN.replace(STOP, "Wait"),
            SCREEN.replace('─', " "),
            format!("{SCREEN}\n❯ new draft"),
        ] {
            assert!(parse(&invalid).is_none(), "{invalid}");
        }
    }
    #[test]
    fn wrapping_status_banners_and_unknown_billing_choices() {
        let wrapped = SCREEN.replace(
            "continue automatically at",
            "continue automatically\n      at",
        );
        assert_eq!(
            snapshot(&record(), SCREEN).unwrap().0,
            snapshot(&record(), &wrapped).unwrap().0
        );
        let banner = SCREEN.replace(
            "────────────────────────────────────────",
            "▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔ Usage credit balance: $5 ▔",
        );
        assert!(
            card(&record(), &parse(&banner).unwrap()).unwrap()["questions"][0]["body"]
                .as_str()
                .unwrap()
                .contains("$5")
        );
        let unknown = SCREEN.replace(
            "Switch to usage credits",
            "Add funds to continue with usage credits",
        );
        assert_eq!(
            card(&record(), &parse(&unknown).unwrap()).unwrap()["can_answer"],
            false
        );
        assert!(snapshot(&record(), &unknown).is_none());
        let changed = SCREEN.replace("Oct 20", "Oct 21");
        assert_ne!(
            snapshot(&record(), SCREEN).unwrap().0,
            snapshot(&record(), &changed).unwrap().0
        );
    }
}
