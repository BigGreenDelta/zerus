//! Verified key navigation for native startup trust and Kimi context choices.
use super::*;
use std::time::{Duration, Instant};

/// Repository secret launchers can leave these exact notices above a native
/// startup panel. They never join the request identity; arbitrary output above
/// a panel is never skipped to find a quoted dialog.
pub(super) fn launcher_notices(rows: &[&str], agent: &str) -> bool {
    let prelude = rows.join(" ").split_whitespace().collect::<Vec<_>>().join(" ");
    let launch = format!("run_{agent}: secrets unlocked → launching {agent}");
    let unlocked = "unlock: SOPS_AGE_KEY loaded for this session.";
    [
        format!("Bitwarden master password: {unlocked} {launch}"),
        format!("? Master password: [hidden] {unlocked} {launch}"),
        format!("run_{agent}: SOPS_AGE_KEY already set — reusing it. {launch}"),
    ]
    .contains(&prelude)
}

pub(super) fn capture(record: &Value) -> Result<String> {
    let out = backend::current().capture(string(record, "pane"), backend::Capture::Plain)?;
    if out.len() > 512 * 1024 {
        return Err("Terminal snapshot is too large".into());
    }
    String::from_utf8(out).map_err(|_| "Invalid terminal text".into())
}
pub(super) fn identity(original: &Value) -> Result<Value> {
    let record = read_run(string(original, "name"), string(original, "run_id"))?
        .ok_or("Session run disappeared")?;
    if [
        "pane",
        "pid",
        "process_start",
        "conversation_id",
        "expected_id",
        "launch_dir",
    ]
    .iter()
    .any(|key| record[*key] != original[*key])
        || !process_alive(&record)
        || !matches(&record, live()?.get(string(&record, "name")))
        || pause_active(&record)
        || !string(&record, "error").is_empty()
    {
        return Err("Session identity changed; open Terminal".into());
    }
    input::checked_terminal(&record, false)?;
    Ok(record)
}
// The parser returns the exact question hash and highlighted option. A trust
// decline can legitimately exit the pinned process after Enter; other choices
// must observe the dialog disappear while the same process owns the terminal.
pub(super) fn answer(
    record: &Value,
    question: &Value,
    chosen: usize,
    option_count: usize,
    parse: fn(&Value, &str) -> Option<(Value, usize)>,
    acknowledged: fn(&str) -> bool,
    expect_exit: bool,
) -> Result<()> {
    navigate(record, question, chosen, option_count, parse, acknowledged, expect_exit, false)
}

// Claude shows queued tool approvals one after another, so the next complete
// approval may replace the answered one before any other screen is drawn.
pub(super) fn answer_queued(
    record: &Value,
    question: &Value,
    chosen: usize,
    option_count: usize,
    parse: fn(&Value, &str) -> Option<(Value, usize)>,
    acknowledged: fn(&str) -> bool,
) -> Result<()> {
    navigate(record, question, chosen, option_count, parse, acknowledged, false, true)
}

#[allow(clippy::too_many_arguments)]
fn navigate(
    record: &Value,
    question: &Value,
    chosen: usize,
    option_count: usize,
    parse: fn(&Value, &str) -> Option<(Value, usize)>,
    acknowledged: fn(&str) -> bool,
    expect_exit: bool,
    queued: bool,
) -> Result<()> {
    let mut touched = false;
    let result = (|| {
        // One verified arrow at a time: never apply a saved index to a different
        // dialog, or send Enter while an arrow is still being processed.
        for _ in 0..option_count {
            let active = identity(record)?;
            let screen = capture(&active)?;
            let panel = parse(&active, &screen).ok_or("The agent's dialog disappeared")?;
            if panel.0 != question["question_hash"] {
                return Err("The agent's dialog changed".into());
            }
            let key = if panel.1 == chosen {
                "Enter"
            } else if panel.1 < chosen {
                "Down"
            } else {
                "Up"
            };
            let active = identity(record)?;
            // Compare the complete parsed request and highlighted option.
            // Unrelated terminal repainting (for example a spinner above the
            // dialog) must not invalidate an otherwise identical decision.
            if parse(&active, &capture(&active)?) != Some(panel.clone()) {
                return Err("Terminal changed before the answer; inspect Terminal".into());
            }
            touched = true;
            backend::current().send(string(&active, "pane"), &[backend::Key::Named(key)])?;
            let expected = if key == "Down" {
                panel.1 + 1
            } else {
                panel.1.saturating_sub(1)
            };
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                std::thread::sleep(Duration::from_millis(40));
                // A new-session choice can legitimately change conversation_id.
                let active = read_run(string(record, "name"), string(record, "run_id"))?
                    .ok_or("Session disappeared after answer")?;
                if ["pane", "pid", "process_start"]
                    .iter()
                    .any(|key| active[*key] != record[*key])
                {
                    return Err("Agent changed after answer".into());
                }
                if !process_alive(&active) {
                    if key == "Enter" && expect_exit {
                        return Ok(());
                    }
                    return Err("Agent exited before confirming the answer".into());
                }
                if let Err(error) = input::checked_terminal(&active, false) {
                    // Native startup can briefly leave input mode after Enter
                    // while replacing the trust dialog with its main screen.
                    // Wait without sending more keys; acknowledge only after
                    // the pinned process owns a verified input terminal again.
                    if key == "Enter" && string(question, "source") == "claude_folder_trust"
                        && Instant::now() < deadline {
                        continue;
                    }
                    return Err(error);
                }
                let screen = capture(&active)?;
                let next = parse(&active, &screen);
                if key == "Enter" {
                    // A clipped or partially redrawn chooser is not an acknowledgement.
                    // Elsewhere a different dialog may report a native failure.
                    let gone = match &next {
                        None => acknowledged(&screen),
                        Some(next) => queued && next.0 != question["question_hash"],
                    };
                    if gone {
                        return Ok(());
                    }
                } else if let Some(next) = next {
                    if next.0 != question["question_hash"] {
                        return Err("The agent's dialog changed after navigation".into());
                    }
                    if next.1 == expected {
                        break;
                    }
                } else {
                    return Err("The agent's dialog disappeared after navigation".into());
                }
                if Instant::now() >= deadline {
                    return Err("The agent has not confirmed the selection; inspect Terminal".into());
                }
            }
        }
        Err("The agent's selection did not finish".into())
    })();
    result.map_err(|e: String| {
        if touched {
            format!("delivery uncertain: {e}")
        } else {
            e
        }
    })
}
