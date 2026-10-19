//! The line an agent reads after a change: what happened and what to do next
//! (docs/superpowers/specs/2026-10-13-project-task-queue-design.md, "Answers").
use serde_json::Value;
use zerus_replica::Stage;

fn name(task: &Value) -> &str {
    task["ref"].as_str().unwrap_or_default()
}

fn label(task: &Value) -> &'static str {
    task["stage"]
        .as_str()
        .and_then(|s| Stage::parse(s).ok())
        .map_or("its stage", Stage::label)
}

/// A session as callers read it: its name, and its computer when that is another one.
fn session(reference: &Value) -> String {
    let name = reference["session"].as_str().unwrap_or("its session");
    match reference["host"].as_str().filter(|h| !h.is_empty()) {
        Some(host) => format!("{name} on {host}"),
        None => name.to_owned(),
    }
}

/// The time of day of a Unix-millisecond instant on this computer, as "14:06".
fn clock(ms: i64) -> String {
    use chrono::{Local, TimeZone, Timelike};
    Local
        .timestamp_millis_opt(ms)
        .single()
        .map_or_else(String::new, |t| format!("{:02}:{:02}", t.hour(), t.minute()))
}

/// What an agent reads after a send or cancel on another computer whose copy
/// could not come back yet.
fn behind(out: &Value) -> &'static str {
    if out["synced"] == false {
        " This computer's copy follows with the next sync."
    } else {
        ""
    }
}

/// The answer to `action` whose JSON is `out`. `place` is the task's place in
/// its list (from 1) after an add or move, `caller` the agent's session and
/// `destination` the session a send went to, with its computer when that is
/// another one ("codex/api on mac").
pub(super) fn answer(action: &str, out: &Value, place: usize, caller: &str, destination: &str) -> String {
    let task = &out["task"];
    let it = name(task);
    match action {
        "add" => format!("Filed {it} in {}, position {place}, for {caller}.", label(task)),
        "move" => format!("{it} is position {place} in {}.", label(task)),
        "edit" => format!("{it} has the new prompt."),
        "stage" => format!("{it} is in {}.", label(task)),
        "target" if task["target"]["session"].is_string() => {
            format!("{it} now goes to {}.", session(&task["target"]))
        }
        "target" => format!("{it} now goes to any session."),
        "comment" => format!("Added your comment to {it}."),
        "report" if out["moved_to_review"] == true => format!("{it} is In review."),
        "report" => {
            let when = match task["cancelled_at"].as_i64() {
                Some(at) if task["stage"] == "review" => format!(" (cancelled at {})", clock(at)),
                _ => String::new(),
            };
            format!("{it} is {}{when}; your report was added to its history.", label(task))
        }
        "send" => format!("{it} sent to {destination}.{}", behind(out)),
        "cancel" if out["cancel_message"]["sent"] == true => format!(
            "{it} is In review; {} got the cancel message.{}",
            session(&task["session"]),
            behind(out)
        ),
        "cancel" => format!(
            "{it} is In review; the cancel message was not sent: {}.{}",
            out["cancel_message"]["error"]
                .as_str()
                .unwrap_or("unknown reason")
                .trim_end_matches('.'),
            behind(out)
        ),
        _ => String::new(),
    }
}

/// A send that was refused, as an agent reads it.
pub(super) fn refused(destination: &str, reason: &str) -> String {
    format!(
        "{destination} cannot take it now: {}. Nothing was sent.",
        reason.trim_end_matches('.')
    )
}
