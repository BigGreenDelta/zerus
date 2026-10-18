//! The prompts Zerus pastes: the task prompt and the cancel message.
use crate::schema::{check_controls, Actor, TaskView, MAX_TEXT};
use crate::{Error, Result};

/// How the prompt names the task tools: the MCP server, or `hgs task`
/// commands for an agent without it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tools {
    Mcp,
    Cli,
}

fn tool(tools: Tools, name: &str, reference: &str) -> String {
    match tools {
        Tools::Mcp => format!("zerus.{name}"),
        Tools::Cli => format!(
            "`hgs task {} {reference}`",
            name.trim_start_matches("task_")
        ),
    }
}

/// Substitutes `{name}` placeholders once; substituted text is not scanned
/// again, so a prompt that contains `{ref}` stays as written.
fn fill(
    template: &str,
    tools: Tools,
    reference: &str,
    value: impl Fn(&str) -> Option<String>,
) -> String {
    let mut out = String::with_capacity(template.len());
    let mut rest = template;
    while let Some(start) = rest.find('{') {
        out.push_str(&rest[..start]);
        let after = &rest[start + 1..];
        let Some(end) = after.find('}') else {
            out.push_str(&rest[start..]);
            return out;
        };
        let key = &after[..end];
        let tail = &after[end + 1..];
        if key == "tools" {
            if let Some(next) = ["task_report", "task_get"]
                .iter()
                .find(|name| tail.starts_with(&format!(".{name}")))
            {
                out.push_str(&tool(tools, next, reference));
                rest = &tail[next.len() + 1..];
                continue;
            }
            out.push_str(match tools {
                Tools::Mcp => "zerus",
                Tools::Cli => "`hgs task`",
            });
            rest = tail;
            continue;
        }
        match value(key) {
            Some(text) => out.push_str(&text),
            None => out.push_str(&rest[start..start + end + 2]),
        }
        rest = tail;
    }
    out.push_str(rest);
    out
}

fn checked(text: String) -> Result<String> {
    if text.len() > MAX_TEXT {
        return Err(Error::new(
            "the rendered prompt is longer than 64 KiB; shorten the task text",
        ));
    }
    if text.starts_with('/') || text.starts_with('!') {
        return Err(Error::new("a prompt cannot start with / or !"));
    }
    check_controls(&text, "rendered prompt")?;
    Ok(text)
}

fn author(task: &TaskView) -> String {
    match &task.author {
        Actor::Person => "the person".into(),
        Actor::Agent(key) => format!("the agent of {}", key.session),
    }
}

/// `project` is the project's current name, shown in the reference.
pub fn task_prompt(template: &str, task: &TaskView, project: &str, tools: Tools) -> Result<String> {
    let reference = format!("{project}/{}", task.id);
    let sent_before = task.log.iter().any(|entry| entry.kind == "sent");
    let history = if sent_before {
        format!(
            "This task was sent before; read its history with {} before you start.",
            tool(tools, "task_get", &reference)
        )
    } else {
        String::new()
    };
    checked(fill(template, tools, &reference, |key| match key {
        "ref" => Some(reference.clone()),
        "author" => Some(author(task)),
        "prompt" => Some(task.text.clone()),
        "history" => Some(history.clone()),
        _ => None,
    }))
}

pub fn cancel_message(
    template: &str,
    task: &TaskView,
    project: &str,
    tools: Tools,
) -> Result<String> {
    let reference = format!("{project}/{}", task.id);
    checked(fill(template, tools, &reference, |key| match key {
        "ref" => Some(reference.clone()),
        "author" => Some(author(task)),
        _ => None,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::project::{DEFAULT_CANCEL_TEMPLATE, DEFAULT_TASK_TEMPLATE};
    use crate::schema::{SessionKey, Stage, Target};
    use crate::task::{NewTask, TaskDoc};

    fn view() -> TaskView {
        TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "Fix {ref} handling",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Person,
            rank: "V",
            now: 1,
        })
        .unwrap()
        .view()
        .unwrap()
    }

    #[test]
    fn a_rendered_prompt_refuses_terminal_control_characters() {
        let template = format!("{DEFAULT_TASK_TEMPLATE}\x07");
        let error = task_prompt(&template, &view(), "orchard", Tools::Cli).unwrap_err();
        assert!(error.to_string().contains("control characters"), "{error}");
    }

    #[test]
    fn first_send_prompt() {
        let text = task_prompt(DEFAULT_TASK_TEMPLATE, &view(), "orchard", Tools::Mcp).unwrap();
        assert!(text.starts_with("[Zerus task orchard/T-a1b2c3]\nWritten by the person.\nFix {ref} handling\n\nWhen you finish"), "{text}");
        assert!(text.contains("report with\nzerus.task_report: one to three"));
    }

    #[test]
    fn later_send_points_to_the_history_and_cli_names_commands() {
        let mut doc = TaskDoc::create(NewTask {
            project: "p1",
            id: "T-a1b2c3",
            text: "Fix it",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Agent(SessionKey {
                machine: "m".into(),
                session: "codex/api".into(),
            }),
            rank: "V",
            now: 1,
        })
        .unwrap();
        doc.record_send(
            &SessionKey {
                machine: "m".into(),
                session: "codex/api".into(),
            },
            "r",
            &Actor::Person,
            2,
        )
        .unwrap();
        let text = task_prompt(
            DEFAULT_TASK_TEMPLATE,
            &doc.view().unwrap(),
            "orchard",
            Tools::Cli,
        )
        .unwrap();
        assert!(text.contains("Written by the agent of codex/api."));
        assert!(text.contains("This task was sent before; read its history with `hgs task get orchard/T-a1b2c3` before you start."));
        assert!(text.contains("report with\n`hgs task report orchard/T-a1b2c3`: one to three"));
        let cancel = cancel_message(
            DEFAULT_CANCEL_TEMPLATE,
            &doc.view().unwrap(),
            "orchard",
            Tools::Cli,
        )
        .unwrap();
        assert!(cancel.starts_with("[Zerus task orchard/T-a1b2c3 cancelled]\nThe person cancelled"));
    }

    #[test]
    fn limits() {
        let mut big = view();
        big.text = "x".repeat(64 * 1024);
        assert!(task_prompt(DEFAULT_TASK_TEMPLATE, &big, "orchard", Tools::Mcp).is_err());
        assert!(task_prompt("/{prompt}", &view(), "orchard", Tools::Mcp).is_err());
        assert_eq!(
            task_prompt("[Zerus task {unknown} {ref", &view(), "o", Tools::Mcp).unwrap(),
            "[Zerus task {unknown} {ref"
        );
    }
}
