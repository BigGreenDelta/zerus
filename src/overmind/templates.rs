//! The text Overmind types into a session (spec Part 4, Templates). A project
//! can replace each built-in template with a `settings_set` event; removing the
//! replacement restores the built-in text.
use super::commands::{check_project, check_text, human_only, normalize, parse, write, Ctx};
use super::fold::Task;
use super::model::*;
use super::store::{Filter, Tx};
use anyhow::Result;
use serde::Deserialize;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;

pub const NAMES: [&str; 4] = ["dispatch", "review", "rework", "answer"];
pub const MAX_TEMPLATE: usize = 16 * 1024;
/// How agents reach Overmind until the MCP server exists (plan 1c).
pub const TOOLS: &str = "`hgs overmind --help`";
const MAX_RENDERED: usize = 64 * 1024;
const TAIL: usize = 5;

const DISPATCH: &str = "[Overmind {task.ref}] {task.title}\n\n{task.prompt}\n\n---\nYou own this task (stage: {task.stage}). Write a short spec and advance to Build; when done, advance to Review with a worklog and evidence. Need a decision: call-human. Tools: {tools}";
const REVIEW: &str = "[Overmind {task.ref}] Review: {task.title}\n\n{task.prompt}\n\n---\nReview this task; another session did the work. Recent journal:\n{task.journal_tail}\n\nRecord your verdict with review (approve or needs_work) and a report. Tools: {tools}";
const REWORK: &str = "[Overmind {task.ref}] Rework: {task.title}\n\nThe review asked for changes:\n{review.report}\n\n---\nFix them, then advance to Review again with a worklog and evidence. Tools: {tools}";
const ANSWER: &str = "[Overmind {task.ref}] Answer: {task.title}\n\n{answer.text}\n\n---\nContinue the task (stage: {task.stage}). Tools: {tools}";

pub fn builtin(name: &str) -> &'static str {
    match name {
        "review" => REVIEW,
        "rework" => REWORK,
        "answer" => ANSWER,
        _ => DISPATCH,
    }
}

/// The project's replacements: the latest `templates` setting written by a person.
fn custom(tx: &Tx, project: &str) -> Result<BTreeMap<String, String>> {
    let events = tx.load(&Filter::Shared(project.into()))?;
    let latest = events.iter().rev().find(|e| {
        e.kind == "settings_set"
            && e.project == project
            && e.v <= EVENT_VERSION
            && e.actor.kind == ActorKind::Human
            && e.payload.get("key").and_then(Value::as_str) == Some("templates")
    });
    let map: BTreeMap<String, String> = latest
        .and_then(|e| serde_json::from_value(e.payload["value"].clone()).ok())
        .unwrap_or_default();
    Ok(map.into_iter().filter(|(name, _)| NAMES.contains(&name.as_str())).collect())
}

pub fn effective(tx: &Tx, project: &str, name: &str) -> Result<String> {
    Ok(custom(tx, project)?.remove(name).unwrap_or_else(|| builtin(name).to_owned()))
}

pub struct Vars<'a> {
    pub task: &'a Task,
    pub reference: &'a str,
    pub answer: &'a str,
    pub tools: &'a str,
}

fn first_line(text: &str) -> String {
    text.lines().next().unwrap_or_default().chars().take(200).collect()
}

/// The last journal entries as one line each, oldest first; refusals are left out.
fn journal_tail(task: &Task) -> String {
    let mut lines: Vec<String> = task
        .journal
        .iter()
        .rev()
        .filter(|j| j.marker != "[rejected]")
        .take(TAIL)
        .map(|j| {
            let text = first_line(&j.text);
            if text.is_empty() { j.marker.clone() } else { format!("{} {text}", j.marker) }
        })
        .collect();
    lines.reverse();
    lines.join("\n")
}

/// The report of the latest review: its text after the verdict line.
fn review_report(task: &Task) -> String {
    task.journal
        .iter()
        .rev()
        .find(|j| j.marker == "[review]")
        .map(|j| j.text.split_once('\n').map_or("", |(_, report)| report).trim().to_owned())
        .unwrap_or_default()
}

/// The rules of `hgs send`, checked before anything is written.
fn check_rendered(text: &str) -> Result<(), Refusal> {
    let bad = |message: &str| Refusal::field("invalid_template", "template", "invalid", message);
    let start = text.trim_start();
    if start.is_empty() {
        return Err(bad("the text for the session is empty"));
    }
    if start.starts_with('/') || start.starts_with('!') {
        return Err(bad("the text for the session must not start with / or !"));
    }
    if text.len() > MAX_RENDERED {
        return Err(bad("the text for the session exceeds 64 KiB; shorten the prompt or the template"));
    }
    if text.chars().any(|c| c.is_control() && c != '\n' && c != '\t') {
        return Err(bad("the text for the session contains control characters"));
    }
    Ok(())
}

/// Replaces known `{names}` in one pass, so values are never expanded again;
/// any other brace stays as written.
pub fn render(template: &str, vars: &Vars) -> Result<String, Refusal> {
    let tail = journal_tail(vars.task);
    let report = review_report(vars.task);
    let values: [(&str, &str); 8] = [
        ("{task.ref}", vars.reference),
        ("{task.title}", vars.task.title.as_str()),
        ("{task.prompt}", vars.task.prompt.as_str()),
        ("{task.stage}", vars.task.stage.as_str()),
        ("{task.journal_tail}", tail.as_str()),
        ("{review.report}", report.as_str()),
        ("{answer.text}", vars.answer),
        ("{tools}", vars.tools),
    ];
    let mut out = String::new();
    let mut rest = template;
    while let Some(start) = rest.find('{') {
        out.push_str(&rest[..start]);
        let tail = &rest[start..];
        match values.iter().find(|(name, _)| tail.starts_with(name)) {
            Some((name, value)) => {
                out.push_str(value);
                rest = &tail[name.len()..];
            }
            None => {
                out.push('{');
                rest = &tail[1..];
            }
        }
    }
    out.push_str(rest);
    check_rendered(&out)?;
    Ok(out)
}

fn listing(tx: &Tx, project: &str) -> Result<Value> {
    let custom = custom(tx, project)?;
    let mut templates = Map::new();
    for name in NAMES {
        let text = custom.get(name).cloned().unwrap_or_else(|| builtin(name).to_owned());
        templates.insert(name.into(), json!({"text": text, "custom": custom.contains_key(name)}));
    }
    Ok(json!({"ok": true, "project": project, "templates": templates}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct GetRequest {
    project: String,
}

pub fn get(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: GetRequest = parse(&request)?;
    check_project(&req.project)?;
    ctx.store.read(|tx| listing(tx, &req.project))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SetRequest {
    #[serde(default)]
    request_id: String,
    project: String,
    templates: BTreeMap<String, Option<String>>,
}

pub fn set(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: SetRequest = parse(&request)?;
    check_project(&req.project)?;
    if req.project.is_empty() {
        return Err(Refusal::field("missing_field", "project", "empty", "templates belong to a project").into());
    }
    let mut changes = BTreeMap::new();
    for (name, text) in &req.templates {
        if !NAMES.contains(&name.as_str()) {
            return Err(Refusal::field("invalid_field", "templates", "invalid", "templates are dispatch, review, rework and answer").into());
        }
        let text = text.as_deref().map(normalize);
        if let Some(text) = &text {
            check_text(name, text, MAX_TEMPLATE, true)?;
            let start = text.trim_start();
            if start.is_empty() || start.starts_with('/') || start.starts_with('!') {
                return Err(Refusal::field("invalid_template", "template", "invalid", format!("the {name} template must have text and must not start with / or !")).into());
            }
        }
        changes.insert(name.clone(), text);
    }
    write(ctx, "templates set", &request, |tx, actor| {
        let mut custom = custom(tx, &req.project)?;
        for (name, text) in &changes {
            match text {
                Some(text) => {
                    custom.insert(name.clone(), text.clone());
                }
                None => {
                    custom.remove(name);
                }
            }
        }
        tx.append(&Draft {
            actor: actor.clone(),
            task: String::new(),
            project: req.project.clone(),
            kind: "settings_set".into(),
            base: String::new(),
            request_id: req.request_id.clone(),
            payload: json!({"key": "templates", "value": custom}),
        })?;
        listing(tx, &req.project)
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands::{create, load};
    use crate::overmind::store::Store;
    use serde_json::json;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn task(store: &mut Store, prompt: &str) -> Task {
        let mut ctx = Ctx { store, actor: Actor::human(MACHINE), cwd: None };
        let created = create(&mut ctx, json!({"project": "p", "prompt": prompt, "stage": "Queue"})).unwrap();
        let uuid = created["task"]["uuid"].as_str().unwrap().to_owned();
        store.read(|tx| load(tx, &uuid)).unwrap()
    }
    fn vars<'a>(task: &'a Task, answer: &'a str) -> Vars<'a> {
        Vars { task, reference: "om-abcde", answer, tools: TOOLS }
    }

    #[test]
    fn dispatch_names_the_task_and_the_tools() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let t = task(&mut store, "Fix the drafts\nDetails");
        let text = render(builtin("dispatch"), &vars(&t, "")).unwrap();
        assert!(text.starts_with("[Overmind om-abcde] Fix the drafts\n\nFix the drafts\nDetails\n\n---\n"), "{text}");
        assert!(text.contains("(stage: Queue)") && text.ends_with(TOOLS), "{text}");
    }

    #[test]
    fn values_are_never_expanded_twice() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let t = task(&mut store, "Print {task.ref} and {unknown} literally");
        let text = render("{task.prompt} / {task.ref} / {nope}", &vars(&t, "")).unwrap();
        assert_eq!(text, "Print {task.ref} and {unknown} literally / om-abcde / {nope}");
    }

    #[test]
    fn rendered_text_follows_send_rules() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let t = task(&mut store, &"x".repeat(60 * 1024));
        assert_eq!(render("{task.prompt}{task.prompt}", &vars(&t, "")).unwrap_err().code, "invalid_template");
        assert_eq!(render("  /compact {task.ref}", &vars(&t, "")).unwrap_err().code, "invalid_template");
        assert_eq!(render("!{task.ref}", &vars(&t, "")).unwrap_err().code, "invalid_template");
        assert_eq!(render("   ", &vars(&t, "")).unwrap_err().code, "invalid_template");
        assert!(render("{answer.text}", &vars(&t, "Use REST")).unwrap() == "Use REST");
    }

    #[test]
    fn journal_tail_and_review_report_come_from_the_journal() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut t = task(&mut store, "p");
        let entry = |marker: &str, text: &str| crate::overmind::fold::JournalEntry {
            marker: marker.into(), text: text.into(), at: 0, actor: Actor::human(MACHINE), event: String::new(),
        };
        for i in 0..6 {
            t.journal.push(entry("[comment]", &format!("note {i}\nsecond line")));
        }
        t.journal.push(entry("[rejected]", "ignored"));
        t.journal.push(entry("[review]", "NEEDS WORK\nAdd tests"));
        let tail = render("{task.journal_tail}", &vars(&t, "")).unwrap();
        assert_eq!(tail.lines().count(), 5, "{tail}");
        assert!(tail.ends_with("[review] NEEDS WORK") && !tail.contains("ignored") && !tail.contains("second line"), "{tail}");
        assert_eq!(render("{review.report}", &vars(&t, "")).unwrap(), "Add tests");
    }

    #[test]
    fn projects_replace_and_reset_templates() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let before = task(&mut store, "p");
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        set(&mut ctx, json!({"project": "p", "templates": {"dispatch": "Do {task.ref}\r\nnow"}})).unwrap();
        let listed = get(&mut ctx, json!({"project": "p"})).unwrap();
        assert_eq!(listed["templates"]["dispatch"], json!({"text": "Do {task.ref}\nnow", "custom": true}));
        assert_eq!(listed["templates"]["review"]["custom"], false);
        set(&mut ctx, json!({"project": "p", "templates": {"dispatch": null}})).unwrap();
        assert_eq!(get(&mut ctx, json!({"project": "p"})).unwrap()["templates"]["dispatch"]["custom"], false);
        let refused = set(&mut ctx, json!({"project": "p", "templates": {"other": "x"}})).unwrap_err();
        assert_eq!(refused.downcast::<Refusal>().unwrap().field, "templates");
        let refused = set(&mut ctx, json!({"project": "p", "templates": {"dispatch": "/clear"}})).unwrap_err();
        assert_eq!(refused.downcast::<Refusal>().unwrap().code, "invalid_template");
        let mut agent = Ctx { store: &mut store, actor: Actor::agent(MACHINE, "w1"), cwd: None };
        assert!(set(&mut agent, json!({"project": "p", "templates": {"dispatch": "x"}})).is_err());
        // Settings never change tasks.
        let after = store.read(|tx| load(tx, &before.uuid)).unwrap();
        assert_eq!((after.read_only, after.head.clone()), (false, before.head.clone()));
    }
}
