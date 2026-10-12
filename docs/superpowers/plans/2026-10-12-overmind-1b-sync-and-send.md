# Overmind 1b — Replication and Sending Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Overmind tasks reach every machine of the swarm, and a person can send a task
(and its review, rework and answer) into a live agent session on any of them.

**Architecture:** Templates render the text for a session from the task. On the
session's host, `deliver.rs` checks the gates and the session, writes the claim,
types the text through the same code path as `hgs send` (`state::input::submit`)
and records the outcome (`[sent]`, `[sent?]` with `delivery_uncertain`, or
`[unclaim]`). `sync.rs` implements one push-and-pull exchange call over the
existing swarm SSH transport; the swarm worker runs it with every enrolled peer
that advertises `overmind:1`. `remote.rs` fetches attachment blobs in ranges and
forwards a send to the session's host after bringing that host up to date.

**Tech Stack:** Rust 2021 (`anyhow`, `rusqlite`, `serde`, `serde_json`, `sha2`,
`base64`, `uuid` v4), Python `unittest` with fake `ssh` and isolated tmux sockets.

**Spec:** `docs/superpowers/specs/2026-10-11-overmind-core-design.md` — Part 2
(Replication), Part 4 (Templates, CLI), Part 5 (Sending), Part 8 (errors).
Plan 1a (`docs/superpowers/plans/2026-10-11-overmind-1a-local-tracker.md`) built
the store, fold, workflow and CLI this plan extends. MCP, the desktop and the
phone are plans 1c and 1d.

## Decisions this plan takes (record them in the spec in Task 8)

1. **The bundle is an exchange.** A store holds a contiguous `seq` prefix of every
   origin (plan 1a), so it cannot take "all events of one task" alone. Delivering
   to another machine first runs the normal exchange with that host (it then holds
   everything the caller holds), pushes the task's blobs, and runs the command
   there. Costs at most the 4 MiB batches the worker would send anyway.
2. **The forwarded command runs as a person on the host** (`Actor::human(host)`):
   the host writes the claim, as Part 2 "Who writes what" requires.
3. **DeepSeek native sessions are refused** (`session_unavailable`) in 1b: their
   input does not go through `input::submit`.
4. **Confirmations are refusal codes.** `send`, `review-send`, `rework-send` and
   `answer` take `confirm: [codes]`; a confirmable refusal has field `confirm`
   and state `missing`: `agent_busy`, `wip_exceeded`, `predecessors_open`,
   `pinned_elsewhere`.
5. **The worker nudge is the `rev` file.** The swarm worker compares
   `<state>/overmind/rev` every pass (3 s) and exchanges at once when it changed,
   like a changed catalog digest today. No new socket or signal.
6. **The input request ID is the Overmind request ID.** A retried request types
   nothing twice: `hgs send` receipts replay a submitted request and report
   anything else as uncertain.
7. **Deferred:** the `orphaned` flag (Beads task created in Task 8) and the
   rename-ordering question `zerus-89h.2.4`.

## Global Constraints

- Minimum Rust 1.85, edition 2021: no let-chains, no std APIs newer than 1.85.
- No new crates and no new crate features: `cargo build --locked` must pass with the current `Cargo.lock`; 0 warnings.
- `src/overmind/` and every call into it are Unix-only (`#[cfg(unix)]` at each call site in `src/swarm/`); the Windows build must still compile.
- Agent-facing text (templates, help, refusal messages, `note`) is English.
- Journal markers are ASCII and frozen; this plan uses `[claim]`, `[sent]`, `[sent?]`, `[unclaim]`, `[answer]`, `[comment]`.
- `hgs send`'s request schema does not change. Rendered text follows its rules: at most 64 KiB, not starting with `/` or `!` after leading whitespace, no control characters but `\n` and `\t`.
- Swarm transport calls are `hgs swarm <action>` with a JSON body, 25 s timeout, 32 MiB limit; Overmind batches are at most 4 MiB, blob ranges at most 512 KiB.
- Tests never touch live state or the live tmux server: Rust tests use `tempfile::tempdir()`; Python tests use temporary `HOME`/`HGS_CONFIG_DIR`/`HGS_STATE_DIR`, a fake `ssh`, and `tmux -S <temp socket>` only (the `tests/test_input.py` fixture). Never run a bare `tmux kill-server`.
- Python tests that import `test_input` or `test_swarm` run with `PYTHONPATH=tests`, as CI does.
- Commit after each task on branch `overmind` with `git commit --only <own paths>`; plain English sentence messages; never add Claude attribution; `python3 scripts/check-source-privacy.py` must pass.

## Review Focus

- A send interrupted after its claim and retried with the same `request_id` never claims twice and never types twice. Test in Task 2 (`test_an_interrupted_send_resumes_its_claim`).
- A stopped, paused or dialog-blocked session refuses before anything is written; the task stays in Queue with no journal entry. Test in Task 2 (`test_an_unready_session_writes_nothing`).
- A prompt containing template variable names (`{task.prompt}`) is typed literally, never expanded again; a rendered text over 64 KiB is refused before the claim. Tests in Task 1 (`values_are_never_expanded_twice`, `rendered_text_follows_send_rules`) and Task 2 (`send_refuses_an_oversized_text_before_claiming`).
- A peer that is offline or answers garbage leaves the local store untouched and shows the error in `status`; the next exchange recovers. Tests in Task 4 (`a_failed_call_changes_nothing_and_is_reported`) and Task 5 (`test_offline_peer_is_reported_and_recovers`).
- An image that no reachable machine holds refuses the send with `attachment_unavailable` before the claim. Test in Task 6 (`test_missing_blob_refuses_before_claiming`).

---

### Task 1: Templates

**Files:**
- Create: `src/overmind/templates.rs`
- Modify: `src/overmind/mod.rs` (add `mod templates;`), `src/overmind/cli.rs` (commands and help), `tests/test_overmind.py`

**Interfaces:**
- Consumes: `commands::{check_project, check_text, human_only, normalize, parse, write, Ctx}`, `fold::Task`, `model::*`, `store::{Filter, Tx}`, `Store::read`.
- Produces (Tasks 2, 3 rely on these):
  - `templates::NAMES: [&str; 4]`, `templates::TOOLS: &str`, `templates::MAX_TEMPLATE`.
  - `templates::effective(tx: &Tx, project: &str, name: &str) -> Result<String>`.
  - `templates::Vars<'a> { task: &'a Task, reference: &'a str, answer: &'a str, tools: &'a str }`.
  - `templates::render(template: &str, vars: &Vars) -> Result<String, Refusal>` (refusal code `invalid_template`, field `template`).
  - `templates::get(ctx, request)`, `templates::set(ctx, request)`; CLI `templates get --json {project}` and `templates set --json {request_id?, project, templates: {name: text|null}}`.
  - Shared event `settings_set` (task "", project P, payload `{"key": "templates", "value": {name: text}}`), written by people only.

- [ ] **Step 1: Write the failing tests** — append to a new file `src/overmind/templates.rs` (the module body follows in Step 3; write tests first, then the code):

```rust
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo test --locked overmind::templates 2>&1 | tail -5`
Expected: compile errors (`render`, `builtin`, `get`, `set` not defined).

- [ ] **Step 3: Write the module** (above the tests in `src/overmind/templates.rs`)

```rust
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
```

Make `commands::create` and `commands::load` reachable from this test module (they are already `pub`). In `src/overmind/mod.rs` add `mod templates;` after `mod store;`.

- [ ] **Step 4: Wire the CLI.** In `src/overmind/cli.rs` add to the `match` before the agent commands:

```rust
        ("templates", ["get"]) => super::templates::get(&mut ctx, request)?,
        ("templates", ["set"]) => super::templates::set(&mut ctx, request)?,
```

and to `HUMAN_USAGE` after the `workflow` line:

```
  templates get --json {project}  |  templates set --json {project, templates: {dispatch|review|rework|answer: text|null}}
```

- [ ] **Step 5: Add a CLI test** to class `Overmind` in `tests/test_overmind.py`:

```python
    def test_templates_round_trip(self):
        self.ok('templates', 'set', '--json', data={'project': 'demo', 'templates': {'review': 'Check {task.ref}'}})
        listed = self.ok('templates', 'get', '--json', data={'project': 'demo'})['templates']
        self.assertEqual(listed['review'], {'text': 'Check {task.ref}', 'custom': True})
        self.assertTrue(listed['dispatch']['text'].startswith('[Overmind {task.ref}]'))
        self.assertEqual(self.refused('templates', 'set', '--json', data={'project': 'demo', 'templates': {'review': '!rm'}})['code'], 'invalid_template')
```

- [ ] **Step 6: Run the tests**

Run: `cargo test --locked overmind 2>&1 | tail -3` and `cargo build --locked 2>&1 | grep -c '^warning'` and `PYTHONPATH=tests HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind 2>&1 | tail -3`
Expected: all pass, 0 warnings.

- [ ] **Step 7: Commit**

```bash
git add src/overmind/templates.rs
git commit --only src/overmind/templates.rs src/overmind/mod.rs src/overmind/cli.rs tests/test_overmind.py -m "Render Overmind session texts from built-in and per-project templates"
```

---

### Task 2: Sending a task into a session on this machine

**Files:**
- Create: `src/overmind/deliver.rs`
- Modify: `src/state/input.rs` (add `preflight`), `src/state/mod.rs` (add `DeliveryTarget`, `delivery_target`, `delivery_preflight`, `deliver_input`), `src/overmind/commands.rs` (make `simple`, `local_session`, `task_result` `pub`; add `replay`, `remember`, `unpin`; `write` uses `replay`/`remember`), `src/overmind/mod.rs`, `src/overmind/cli.rs`, `tests/test_overmind.py`

**Interfaces:**
- Consumes: Task 1 `templates::{effective, render, Vars, TOOLS}`; plan 1a `commands::{transition, load, load_writable, resolve, reference, view, workflow, parse, human_only, refuse}`, `fold::{fold_many, Task}`, `workflow::{Action, Role, Workflow}`.
- Produces (Tasks 3, 6, 7 rely on these):
  - `crate::state::DeliveryTarget { run_id: String, conversation_id: String, busy: bool }`; `crate::state::delivery_target(name: &str) -> Result<Option<DeliveryTarget>, String>`; `crate::state::delivery_preflight(name: &str, target: &DeliveryTarget) -> Result<(), String>`; `crate::state::deliver_input(name: &str, request: &Value) -> Result<Value, String>`.
  - `commands::replay(tx: &Tx, command: &str, request: &Value) -> Result<Option<Value>>`; `commands::remember(tx: &mut Tx, command: &str, request: &Value, result: &Value) -> Result<()>`.
  - In `deliver.rs`: `pub enum Outcome { Sent, NotSent(String), Uncertain(String) }`; `type Confirmed = BTreeSet<String>`; `fn confirmable(code, &Confirmed, String) -> Result<()>`; `fn local_target(store: &Store, session: &SessionRef) -> Result<DeliveryTarget>`; `fn check_session(session, target, &Confirmed) -> Result<()>`; `fn files(dir: &Path, task: &Task) -> Result<Vec<Value>>`; `fn deliver(session, target, input_id: &str, text: &str, files: Vec<Value>) -> Outcome`; `fn render(tx, task, name, answer) -> Result<String>`; `fn journal(tx, actor, task, marker, text, transition) -> Result<()>`; `fn flag(tx, actor, task, name, value) -> Result<()>`; `fn rule_target(wf, action, stage) -> Option<String>`.
  - Commands `send`, `resolve-delivery`, `unpin` (CLI and `pub fn send/resolve_delivery` in `deliver.rs`, `pub fn unpin` in `commands.rs`).

- [ ] **Step 1: Session plumbing in `src/state`.** In `src/state/input.rs` add after `checked_record`:

```rust
/// The checks `submit` makes before it writes anything, for callers that must
/// refuse before recording their own intent. `submit` checks again.
pub(super) fn preflight(name: &str, run_id: &str, conversation_id: &str) -> Result<()> {
    let record = checked_input(name, run_id, conversation_id, true)?;
    compact_context::check_continuation(&record, "")?;
    checked_terminal(&record, true)
}
```

In `src/state/mod.rs` add next to `caller_session` (use the same helpers `rename.rs` uses: `record_path`, `read`, `string`; `dsh::exists` exists in both the Unix and Windows state modules):

```rust
/// A session on this machine as Overmind needs it to deliver a task.
pub(crate) struct DeliveryTarget {
    pub run_id: String,
    pub conversation_id: String,
    /// The agent is working; the message waits in its own queue.
    pub busy: bool,
}

/// The tracked session `name` on this machine, or None when there is none.
pub(crate) fn delivery_target(name: &str) -> Result<Option<DeliveryTarget>> {
    if dsh::exists(name) {
        return Err("Overmind cannot type into DeepSeek sessions yet; open the session and send the prompt yourself".into());
    }
    if !record_path(name).exists() {
        return Ok(None);
    }
    let record = read(name)?;
    Ok(Some(DeliveryTarget {
        run_id: string(&record, "run_id").to_owned(),
        conversation_id: string(&record, "conversation_id").to_owned(),
        busy: string(&record, "activity") == "busy",
    }))
}

/// Refuses when `hgs send` would refuse now; writes nothing.
pub(crate) fn delivery_preflight(name: &str, target: &DeliveryTarget) -> Result<()> {
    input::preflight(name, &target.run_id, &target.conversation_id)
}

/// Types `request` (the `hgs send` JSON) into the session exactly like `hgs send`.
pub(crate) fn deliver_input(name: &str, request: &Value) -> Result<Value> {
    input::submit(name, request.to_string().as_bytes(), None)
}
```

- [ ] **Step 2: Receipts as separate steps in `commands.rs`.** Replace the body of `write` so the receipt logic is reusable by multi-step commands:

```rust
/// The stored result of an earlier request with this `request_id`; a reused ID
/// with another command or body is refused.
pub fn replay(tx: &Tx, command: &str, request: &Value) -> Result<Option<Value>> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default();
    if request_id.is_empty() {
        return Ok(None);
    }
    if uuid::Uuid::parse_str(request_id).is_err() {
        return refuse("invalid_request", "request_id must be a UUID");
    }
    match tx.receipt(request_id)? {
        Some((stored, _)) if stored != request_digest(command, request) => {
            refuse("request_reused", "this request_id was already used for a different request")
        }
        Some((_, result)) => Ok(Some(result)),
        None => Ok(None),
    }
}

pub fn remember(tx: &mut Tx, command: &str, request: &Value, result: &Value) -> Result<()> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default();
    if !request_id.is_empty() {
        tx.put_receipt(request_id, &request_digest(command, request), result)?;
    }
    Ok(())
}

pub fn write(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    f: impl FnOnce(&mut Tx, &Actor) -> Result<Value>,
) -> Result<Value> {
    let actor = ctx.actor.clone();
    ctx.store.write(|tx| {
        if let Some(result) = replay(tx, command, request)? {
            return Ok(result);
        }
        let result = f(tx, &actor)?;
        remember(tx, command, request, &result)?;
        Ok(result)
    })
}
```

Make `simple`, `local_session` and `task_result` `pub`. Add `unpin` after `pin`:

```rust
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct UnpinRequest {
    #[serde(default)]
    request_id: String,
    task: String,
}

pub fn unpin(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: UnpinRequest = parse(&request)?;
    write(ctx, "unpin", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "pin_set", json!({"session": null}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}
```

Run `cargo test --locked overmind` now: the existing receipt tests must still pass.

- [ ] **Step 3: Write the failing Rust tests** in a new file `src/overmind/deliver.rs` (`#[cfg(test)] mod tests` at its end). They exercise the parts that need no live session:

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands::{create, link};
    use serde_json::json;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn new_task(store: &mut Store, stage: &str) -> String {
        let mut ctx = Ctx { store, actor: Actor::human(MACHINE), cwd: None };
        let created = create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": stage})).unwrap();
        created["task"]["uuid"].as_str().unwrap().to_owned()
    }
    fn session(name: &str) -> SessionRef {
        SessionRef { machine: MACHINE.into(), name: name.into() }
    }
    fn claim(store: &mut Store, uuid: &str, name: &str, request_id: &str) -> Event {
        store
            .write(|tx| {
                let task = load(tx, uuid)?;
                transition(tx, &Actor::human(MACHINE), &task, Action::Claim, "Design", BTreeMap::new(), Some(session(name)), vec![], request_id)
            })
            .unwrap()
    }
    fn code(result: Result<impl std::fmt::Debug>) -> String {
        result.unwrap_err().downcast::<Refusal>().unwrap().code
    }

    #[test]
    fn gates_refuse_epics_and_ask_to_confirm_the_rest() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let first = new_task(&mut store, "Queue");
        let second = new_task(&mut store, "Queue");
        let none = Confirmed::new();
        let gates = |store: &mut Store, uuid: &str, confirmed: &Confirmed| {
            store.read(|tx| {
                let task = load(tx, uuid)?;
                send_gates(tx, &workflow(tx, "p")?, &task, &session("w1"), confirmed)
            })
        };
        assert_eq!(gates(&mut store, &first, &none).unwrap(), "Design");
        claim(&mut store, &first, "w1", "");
        assert_eq!(code(gates(&mut store, &second, &none)), "wip_exceeded");
        let wip = Confirmed::from(["wip_exceeded".to_string()]);
        assert!(gates(&mut store, &second, &wip).is_ok());
        let third = new_task(&mut store, "Queue");
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        link(&mut ctx, json!({"task": third, "relation": "follows", "target": second, "present": true})).unwrap();
        assert_eq!(code(gates(&mut store, &third, &wip)), "predecessors_open");
        let refusal = gates(&mut store, &third, &wip).unwrap_err().downcast::<Refusal>().unwrap();
        assert_eq!((refusal.field.as_str(), refusal.state.as_str()), ("confirm", "missing"));
        assert_eq!(code(gates(&mut store, &first, &wip)), "transition_not_allowed");
    }

    #[test]
    fn outcomes_are_recorded_and_never_retyped() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let sent = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &sent, "w1", "");
        let request = json!({"task": sent});
        let result = settle_send(&mut ctx, &request, &sent, &c, &session("w1"), Outcome::Sent).unwrap();
        assert_eq!(result["delivery"], "sent");
        let journal = &result["task"]["journal"];
        assert!(journal.as_array().unwrap().iter().any(|j| j["marker"] == "[sent]" && j["text"].as_str().unwrap().contains(&c.id)));

        let uncertain = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &uncertain, "w2", "");
        let result = settle_send(&mut ctx, &json!({"task": uncertain}), &uncertain, &c, &session("w2"), Outcome::Uncertain("lost".into())).unwrap();
        assert_eq!(result["delivery"], "uncertain");
        assert!(result["task"]["flags"].as_array().unwrap().contains(&json!("delivery_uncertain")));

        let failed = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &failed, "w3", "");
        let error = settle_send(&mut ctx, &json!({"task": failed}), &failed, &c, &session("w3"), Outcome::NotSent("stopped".into())).unwrap_err();
        assert_eq!(error.downcast::<Refusal>().unwrap().code, "not_delivered");
        let task = ctx.store.read(|tx| load(tx, &failed)).unwrap();
        assert_eq!((task.stage.as_str(), task.assignee.is_none()), ("Queue", true));
        assert!(task.journal.iter().any(|j| j.marker == "[unclaim]" && j.text.contains("stopped")));
    }

    #[test]
    fn a_pending_claim_is_found_by_its_request() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        let request_id = "0b9c5c1e-34c4-4d5e-9a64-1d2a3b4c5d6e";
        let c = claim(&mut store, &uuid, "w1", request_id);
        let found = store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, request_id)).unwrap();
        assert_eq!(found.map(|e| e.id), Some(c.id.clone()));
        assert!(store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, "other")).unwrap().is_none());
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        settle_send(&mut ctx, &json!({"task": uuid}), &uuid, &c, &session("w1"), Outcome::Sent).unwrap();
        assert!(store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, request_id)).unwrap().is_none());
    }

    #[test]
    fn a_person_resolves_an_uncertain_delivery() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        assert_eq!(code(resolve_delivery(&mut ctx, json!({"task": new_task(ctx.store, "Queue"), "delivered": true}))), "not_uncertain");
        for delivered in [true, false] {
            let uuid = new_task(ctx.store, "Queue");
            let c = claim(ctx.store, &uuid, "w1", "");
            settle_send(&mut ctx, &json!({"task": uuid}), &uuid, &c, &session("w1"), Outcome::Uncertain("?".into())).unwrap();
            let result = resolve_delivery(&mut ctx, json!({"task": uuid, "delivered": delivered})).unwrap();
            assert!(!result["task"]["flags"].as_array().unwrap().contains(&json!("delivery_uncertain")));
            assert_eq!(result["task"]["stage"], if delivered { "Design" } else { "Queue" });
        }
    }
}
```

Run: `cargo test --locked overmind::deliver 2>&1 | tail -5` — Expected: compile errors (functions missing).

- [ ] **Step 4: Write `src/overmind/deliver.rs`** (above the tests):

```rust
//! Sending tasks into sessions on this machine (spec Part 5). The gates and the
//! session are checked first, then the claim is written, the text is typed like
//! `hgs send`, and the outcome is recorded. Overmind never types a text twice:
//! the input request ID is the Overmind request ID.
use super::blobs;
use super::commands::{
    self, human_only, load, load_writable, local_session, parse, reference, refuse, resolve, simple, transition, view,
    workflow, Ctx,
};
use super::fold::{fold_many, Task};
use super::model::*;
use super::store::{Filter, Store, Tx};
use super::templates;
use super::workflow::{Action, Role, Workflow};
use crate::state::DeliveryTarget;
use anyhow::Result;
use base64::Engine;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

pub enum Outcome {
    Sent,
    NotSent(String),
    Uncertain(String),
}

impl Outcome {
    fn name(&self) -> &'static str {
        match self {
            Outcome::Sent => "sent",
            Outcome::NotSent(_) => "not_sent",
            Outcome::Uncertain(_) => "uncertain",
        }
    }
    fn reason(&self) -> &str {
        match self {
            Outcome::Sent => "",
            Outcome::NotSent(reason) | Outcome::Uncertain(reason) => reason,
        }
    }
}

/// Refusal codes a person confirmed for this request.
pub type Confirmed = BTreeSet<String>;

pub fn confirmable(code: &str, confirmed: &Confirmed, message: String) -> Result<()> {
    if confirmed.contains(code) {
        return Ok(());
    }
    Err(Refusal::field(code, "confirm", "missing", message).into())
}

/// The tracked session on this machine that `session` names.
pub fn local_target(store: &Store, session: &SessionRef) -> Result<DeliveryTarget> {
    if session.machine != store.machine() {
        return refuse("remote_session", "this session is on another machine");
    }
    crate::state::delivery_target(&session.name)
        .map_err(|e| Refusal::field("session_unavailable", "session", "invalid", e))?
        .ok_or_else(|| Refusal::field("unknown_session", "session", "invalid", format!("there is no session {} on this machine", session.name)).into())
}

/// Refuses, writing nothing, when the session cannot take input now.
pub fn check_session(session: &SessionRef, target: &DeliveryTarget, confirmed: &Confirmed) -> Result<()> {
    if target.busy {
        confirmable("agent_busy", confirmed, format!("{} is working; the text will wait in its queue", session.name))?;
    }
    crate::state::delivery_preflight(&session.name, target)
        .map_err(|e| Refusal::field("session_unavailable", "session", "invalid", e).into())
}

/// The task's images as `hgs send` attachments, referenced as `[Image #n]`.
pub fn files(dir: &Path, task: &Task) -> Result<Vec<Value>> {
    let mut out = Vec::new();
    for (index, attachment) in task.attachments.iter().enumerate() {
        let bytes = blobs::path(dir, &attachment.sha256)
            .ok()
            .and_then(|path| std::fs::read(path).ok())
            .ok_or_else(|| {
                Refusal::field("attachment_unavailable", "attachments", "missing", format!("the image {} is not on this machine", attachment.name))
            })?;
        out.push(json!({
            "name": attachment.name, "mime": attachment.mime,
            "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes),
            "reference": format!("[Image #{}]", index + 1),
        }));
    }
    Ok(out)
}

pub fn deliver(session: &SessionRef, target: &DeliveryTarget, input_id: &str, text: &str, files: Vec<Value>) -> Outcome {
    let request = json!({
        "request_id": input_id, "text": text, "expected_run_id": target.run_id,
        "expected_conversation_id": target.conversation_id, "attachments": files,
    });
    match crate::state::deliver_input(&session.name, &request) {
        Ok(receipt) if receipt["status"] == "submitted" => Outcome::Sent,
        Ok(_) => Outcome::Uncertain("the session did not confirm the input".into()),
        // A retry whose text changed since the first attempt may already be typed.
        Err(error) if error.starts_with("delivery uncertain") || error.contains("already used for different content") => {
            Outcome::Uncertain(error)
        }
        Err(error) => Outcome::NotSent(error),
    }
}

pub fn render(tx: &Tx, task: &Task, name: &str, answer: &str) -> Result<String> {
    let template = templates::effective(tx, &task.project, name)?;
    let reference = reference(tx, &task.uuid)?;
    Ok(templates::render(&template, &templates::Vars { task, reference: &reference, answer, tools: templates::TOOLS })?)
}

pub fn journal(tx: &mut Tx, actor: &Actor, task: &Task, marker: &str, text: &str, transition: &str) -> Result<()> {
    let mut payload = json!({"marker": marker, "text": text});
    if !transition.is_empty() {
        payload["transition"] = json!(transition);
    }
    simple(tx, actor, task, "journal", payload, "")?;
    Ok(())
}

pub fn flag(tx: &mut Tx, actor: &Actor, task: &Task, name: &str, value: bool) -> Result<()> {
    simple(tx, actor, task, "flag_set", json!({"flag": name, "value": value}), "")?;
    Ok(())
}

/// Where a person's `action` takes a task from `stage`, if the workflow allows it.
pub fn rule_target(wf: &Workflow, action: Action, stage: &str) -> Option<String> {
    wf.rules
        .iter()
        .find(|r| r.action == action && r.from.iter().any(|f| f == stage) && r.actors.contains(&ActorKind::Human))
        .map(|r| r.to.clone())
}

fn reason(text: &str) -> BTreeMap<String, String> {
    BTreeMap::from([("reason".to_owned(), text.to_owned())])
}

/// Tasks the session holds in active or needs-human stages, in every project.
fn active_tasks(tx: &Tx, session: &SessionRef) -> Result<usize> {
    let events = tx.load(&Filter::All)?;
    let mut count = 0;
    for task in fold_many(&events).values().filter(|t| !t.deleted && t.assignee.as_ref() == Some(session)) {
        if matches!(workflow(tx, &task.project)?.role(&task.stage), Some(Role::Active | Role::NeedsHuman)) {
            count += 1;
        }
    }
    Ok(count)
}

/// The stage a send claims the task into, after the gates of Part 3.
fn send_gates(tx: &Tx, wf: &Workflow, task: &Task, session: &SessionRef, confirmed: &Confirmed) -> Result<String> {
    if task.labels.contains("epic") {
        return refuse("epic", "an epic is a container and is never sent to a session");
    }
    let to = rule_target(wf, Action::Claim, &task.stage)
        .ok_or_else(|| Refusal::new("transition_not_allowed", format!("a task in {} cannot be sent", task.stage)))?;
    if let Some(pinned) = task.pinned.as_ref().filter(|pinned| *pinned != session) {
        confirmable("pinned_elsewhere", confirmed, format!("this task is reserved for {}", pinned.name))?;
    }
    let mut open = Vec::new();
    for uuid in &task.follows {
        if let Ok(before) = load(tx, uuid) {
            if !matches!(workflow(tx, &before.project)?.role(&before.stage), Some(Role::Review | Role::Done)) {
                open.push(reference(tx, uuid)?);
            }
        }
    }
    if !open.is_empty() {
        confirmable("predecessors_open", confirmed, format!("the tasks this one follows are not in review yet: {}", open.join(", ")))?;
    }
    let active = active_tasks(tx, session)?;
    if active >= wf.session_wip as usize {
        confirmable("wip_exceeded", confirmed, format!("{} already holds {active} active task(s)", session.name))?;
    }
    Ok(to)
}

/// The claim this request wrote whose outcome is not recorded yet: a send that
/// stopped between claiming and recording.
fn pending_claim(tx: &Tx, task: &Task, request_id: &str) -> Result<Option<Event>> {
    if request_id.is_empty() {
        return Ok(None);
    }
    let events = tx.load(&Filter::Task(task.uuid.clone()))?;
    let Some(claim) = events.iter().find(|e| {
        e.id == task.head && e.kind == "transition" && e.request_id == request_id && e.payload["action"] == "claim"
    }) else {
        return Ok(None);
    };
    let settled = events.iter().any(|e| e.kind == "journal" && e.payload["transition"] == claim.id.as_str());
    Ok((!settled).then(|| claim.clone()))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SendRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    session: SessionRef,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: SendRequest = parse(&request)?;
    let session = local_session("session", Some(req.session.clone()), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "send", &request))? {
        return Ok(result);
    }
    let request_id = if req.request_id.is_empty() { uuid::Uuid::new_v4().to_string() } else { req.request_id.clone() };
    let target = local_target(ctx.store, &session)?;
    let dir = ctx.store.dir().to_owned();
    // Everything that can refuse runs before the claim is written.
    let (uuid, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if pending_claim(tx, &task, &request_id)?.is_none() {
            send_gates(tx, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
        }
        Ok((uuid, files(&dir, &task)?))
    })?;
    check_session(&session, &target, &req.confirm)?;
    let actor = ctx.actor.clone();
    let (claim, text) = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        let claim = match pending_claim(tx, &task, &request_id)? {
            Some(claim) => claim,
            None => {
                let to = send_gates(tx, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
                transition(tx, &actor, &task, Action::Claim, &to, BTreeMap::new(), Some(session.clone()), vec![], &request_id)?
            }
        };
        // A text that `hgs send` would refuse rolls the claim back.
        let text = render(tx, &load(tx, &uuid)?, "dispatch", "")?;
        Ok((claim, text))
    })?;
    let outcome = deliver(&session, &target, &request_id, &text, attachments);
    settle_send(ctx, &request, &uuid, &claim, &session, outcome)
}

/// Records how a send ended. A definite failure returns the task to its queue
/// and refuses; the other outcomes are remembered for the request.
fn settle_send(ctx: &mut Ctx, request: &Value, uuid: &str, claim: &Event, session: &SessionRef, outcome: Outcome) -> Result<Value> {
    let actor = ctx.actor.clone();
    let result = ctx.store.write(|tx| {
        let task = load(tx, uuid)?;
        match &outcome {
            Outcome::Sent => journal(tx, &actor, &task, "[sent]", &session.name, &claim.id)?,
            Outcome::Uncertain(why) => {
                journal(tx, &actor, &task, "[sent?]", why, &claim.id)?;
                flag(tx, &actor, &task, "delivery_uncertain", true)?;
            }
            Outcome::NotSent(why) => {
                let back = rule_target(&workflow(tx, &task.project)?, Action::Unclaim, &task.stage)
                    .ok_or_else(|| Refusal::new("transition_not_allowed", "the claim cannot be taken back"))?;
                transition(tx, &actor, &task, Action::Unclaim, &back, reason(why), None, vec![], "")?;
            }
        }
        let task = load(tx, uuid)?;
        let result = json!({"ok": true, "delivery": outcome.name(), "reason": outcome.reason(), "task": view(&task, &reference(tx, uuid)?, true)});
        if !matches!(outcome, Outcome::NotSent(_)) {
            commands::remember(tx, "send", request, &result)?;
        }
        Ok(result)
    })?;
    if let Outcome::NotSent(why) = outcome {
        return Err(Refusal::new("not_delivered", format!("not delivered: {why}")).into());
    }
    Ok(result)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ResolveRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    delivered: bool,
}

pub fn resolve_delivery(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ResolveRequest = parse(&request)?;
    commands::write(ctx, "resolve-delivery", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if !task.flags.contains("delivery_uncertain") {
            return refuse("not_uncertain", "this task has no unconfirmed delivery");
        }
        flag(tx, actor, &task, "delivery_uncertain", false)?;
        let task = load(tx, &uuid)?;
        let back = rule_target(&workflow(tx, &task.project)?, Action::Unclaim, &task.stage);
        match (req.delivered, back) {
            (true, _) => journal(tx, actor, &task, "[sent]", "delivery confirmed by a person", "")?,
            (false, Some(back)) if task.assignee.is_some() => {
                transition(tx, actor, &task, Action::Unclaim, &back, reason("not delivered, confirmed by a person"), None, vec![], "")?;
            }
            (false, _) => journal(tx, actor, &task, "[comment]", "not delivered, confirmed by a person", "")?,
        }
        commands::task_result(tx, &uuid)
    })
}
```

Add `mod deliver;` to `src/overmind/mod.rs`. In `cli.rs` add:

```rust
        ("send", []) => super::deliver::send(&mut ctx, request)?,
        ("resolve-delivery", []) => super::deliver::resolve_delivery(&mut ctx, request)?,
        ("unpin", []) => commands::unpin(&mut ctx, request)?,
```

and to `HUMAN_USAGE` (replace the `reorder`/`pin` line accordingly):

```
  reorder --json {task, before?}  |  pin --json {task, session?}  |  unpin --json {task}
  send --json {task, session: {machine?, name}, confirm?: [codes]}   claim and type the task into a session
  resolve-delivery --json {task, delivered: true|false}
```

Run: `cargo test --locked overmind 2>&1 | tail -3` — Expected: PASS.

- [ ] **Step 5: Write the failing live-session tests.** In `tests/test_overmind.py` extend class `AgentSession` (it already reuses the `test_input.InputTransport` fixture). Add `received = fixtures.InputTransport.received` to the class attributes, and these helpers and tests:

```python
    def person(self, *args, data=None):
        return self.ok(*args, data=data, agent=False)

    def task(self, **fields):
        return self.person('create', '--json', data={'project': 'demo', 'prompt': 'Fix the drafts', 'stage': 'Queue', **fields})['task']['ref']

    def send(self, ref, **fields):
        return self.overmind('send', '--json', data={'task': ref, 'session': {'name': self.name}, **fields}, agent=False)

    def test_send_claims_and_types_the_dispatch_text(self):
        image = base64.b64encode(b'\x89PNG fixture').decode()
        ref = self.task(attachments=[{'name': 'shot.png', 'mime': 'image/png', 'data_base64': image}])
        code, body = self.send(ref)
        self.assertEqual((code, body['delivery']), (0, 'sent'), body)
        self.assertEqual((body['task']['stage'], body['task']['assignee']['name']), ('Design', self.name))
        self.assertEqual([j['marker'] for j in body['task']['journal']][-2:], ['[claim]', '[sent]'])
        self.wait_for(lambda: b'\r' in self.received())
        typed = self.received()
        self.assertIn(f'[Overmind {ref}] Fix the drafts'.encode(), typed)
        self.assertIn(b'Inspect the attached image', typed)

    def test_send_retry_with_the_same_request_never_types_twice(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        first = self.send(ref, request_id=request_id)
        self.wait_for(lambda: b'\r' in self.received())
        self.assertEqual(self.send(ref, request_id=request_id), first)
        self.assertEqual(self.received().count(b'[Overmind '), 1)

    def test_an_interrupted_send_resumes_its_claim(self):
        request_id = str(uuid.uuid4())
        ref = self.task()
        self.claim(request_id=request_id)  # as if hgs stopped right after the claim
        code, body = self.send(ref, request_id=request_id)
        self.assertEqual((code, body['delivery']), (0, 'sent'), body)
        markers = [j['marker'] for j in body['task']['journal']]
        self.assertEqual((markers.count('[claim]'), markers.count('[sent]')), (1, 1))

    def test_an_unready_session_writes_nothing(self):
        ref = self.task()
        self.record['expected_id'] = 'pending'
        self.write_record()
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'session_unavailable'))
        task = self.person('show', ref)['task']
        self.assertEqual((task['stage'], [j['marker'] for j in task['journal']]), ('Queue', ['[created]']))
        self.assertEqual(self.received(), b'')

    def test_a_busy_agent_and_a_second_task_need_confirmation(self):
        first, second = self.task(), self.task()
        self.record['activity'] = 'busy'
        self.write_record()
        code, body = self.send(first)
        self.assertEqual((code, body['error']['code'], body['error']['field']), (1, 'agent_busy', 'confirm'))
        self.assertEqual(self.send(first, confirm=['agent_busy'])[1]['delivery'], 'sent')
        code, body = self.send(second, confirm=['agent_busy'])
        self.assertEqual(body['error']['code'], 'wip_exceeded')

    def test_send_refuses_an_oversized_text_before_claiming(self):
        self.person('templates', 'set', '--json', data={'project': 'demo', 'templates': {'dispatch': '{task.prompt}{task.prompt}'}})
        ref = self.task(prompt='x' * (40 * 1024))
        code, body = self.send(ref)
        self.assertEqual((code, body['error']['code']), (1, 'invalid_template'))
        self.assertEqual(self.person('show', ref)['task']['stage'], 'Queue')
```

Generalize the existing `claim(self)` helper so it accepts `request_id=''` and stores it in the inserted event (`request_id=request_id` instead of `''`). Add `import base64` at the top of the file.

Run: `cargo build --locked && PYTHONPATH=tests HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind.AgentSession 2>&1 | tail -5` — Expected before Step 4 is wired: failures with `unknown_command`; after: PASS. (If the fake agent's `received` file holds the bracketed paste, `b'\r'` marks the Enter.)

- [ ] **Step 6: Full check and commit**

Run: `cargo test --locked 2>&1 | grep '^test result'`, `cargo build --locked 2>&1 | grep -c '^warning'` (0), `PYTHONPATH=tests HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind tests.test_input 2>&1 | tail -3`, `python3 scripts/ci/check-source.py 2>&1 | tail -2`.

```bash
git add src/overmind/deliver.rs
git commit --only src/overmind/deliver.rs src/overmind/commands.rs src/overmind/mod.rs src/overmind/cli.rs src/state/input.rs src/state/mod.rs tests/test_overmind.py -m "Send Overmind tasks into sessions on this machine with gates, confirmations and recorded outcomes"
```

---

### Task 3: Review, rework and answer

**Files:**
- Modify: `src/overmind/deliver.rs`, `src/overmind/cli.rs`, `tests/test_overmind.py`

**Interfaces:**
- Consumes: Task 2 `deliver.rs` helpers (`Outcome`, `Confirmed`, `confirmable`, `local_target`, `check_session`, `files`, `deliver`, `render`, `journal`, `flag`, `reason`), `commands::{replay, remember, transition, simple}`, `workflow::RESUME`.
- Produces: `pub fn review_send`, `pub fn rework_send`, `pub fn answer` in `deliver.rs` (Task 7 forwards them); CLI `review-send --json {task, session, confirm?}`, `rework-send --json {task, confirm?}`, `answer --json {task, answer, confirm?}`.

Shared behavior: refuse before writing (gates, session check, missing images); write the intent event (none for rework); type the template; record `[sent]` (text names the purpose and session), or `[sent?]` plus `delivery_uncertain`, or undo and refuse with `not_delivered`. The input request ID is the request's `request_id` (or a fresh UUID when none was given). Successful and uncertain results are remembered under the command name.

- [ ] **Step 1: Write the failing Rust tests** (append to the tests module of `deliver.rs`):

```rust
    #[test]
    fn review_needs_another_session_and_a_task_in_review() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        claim(&mut store, &uuid, "w1", "");
        let gate = |store: &mut Store, name: &str| store.read(|tx| review_gates(tx, &load(tx, &uuid)?, &session(name)));
        assert_eq!(code(gate(&mut store, "w2")), "transition_not_allowed");
        store
            .write(|tx| {
                let agent = Actor::agent(MACHINE, "w1");
                let task = load(tx, &uuid)?;
                transition(tx, &agent, &task, Action::Advance, "Build", BTreeMap::from([("spec".to_owned(), "s".to_owned())]), None, vec![], "")?;
                let task = load(tx, &uuid)?;
                let fields = BTreeMap::from([("worklog".to_owned(), "w".to_owned()), ("evidence".to_owned(), "e".to_owned())]);
                transition(tx, &agent, &task, Action::Advance, "Review", fields, None, vec![], "")
            })
            .unwrap();
        assert_eq!(code(gate(&mut store, "w1")), "not_independent");
        assert!(gate(&mut store, "w2").is_ok());
    }

    #[test]
    fn rework_and_answer_go_to_the_assignee() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "rework"))), "transition_not_allowed");
        claim(&mut store, &uuid, "w1", "");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "rework"))), "not_reviewed");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "answer"))), "no_question");
    }
```

- [ ] **Step 2: Implement** in `deliver.rs`:

```rust
/// Review goes to another session, for a task in a review stage.
fn review_gates(tx: &Tx, task: &Task, session: &SessionRef) -> Result<()> {
    if task.labels.contains("epic") {
        return refuse("epic", "an epic is a container and is never reviewed");
    }
    if workflow(tx, &task.project)?.role(&task.stage) != Some(Role::Review) {
        return refuse("transition_not_allowed", "only a task in review can be sent for review");
    }
    if task.assignee.as_ref() == Some(session) {
        return refuse("not_independent", "the session that did the work cannot review it");
    }
    Ok(())
}

/// The session that rework or an answer goes to.
fn assignee_for(task: &Task, purpose: &str) -> Result<SessionRef> {
    let assignee = task
        .assignee
        .clone()
        .ok_or_else(|| Refusal::new("transition_not_allowed", "this task is not assigned to a session"))?;
    match purpose {
        "rework" if !task.labels.contains("review-failed") => refuse("not_reviewed", "rework follows a review that asked for changes"),
        "answer" if task.resume_stage.is_empty() => refuse("no_question", "this task is not waiting for an answer"),
        _ => Ok(assignee),
    }
}

/// Records a delivery that is not a claim; `undo` runs when it surely failed.
#[allow(clippy::too_many_arguments)]
fn settle(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    uuid: &str,
    purpose: &str,
    session: &SessionRef,
    outcome: Outcome,
    undo: impl FnOnce(&mut Tx, &Actor, &Task) -> Result<()>,
) -> Result<Value> {
    let actor = ctx.actor.clone();
    let result = ctx.store.write(|tx| {
        let task = load(tx, uuid)?;
        match &outcome {
            Outcome::Sent => journal(tx, &actor, &task, "[sent]", &format!("{purpose}: {}", session.name), "")?,
            Outcome::Uncertain(why) => {
                journal(tx, &actor, &task, "[sent?]", &format!("{purpose}: {why}"), "")?;
                flag(tx, &actor, &task, "delivery_uncertain", true)?;
            }
            Outcome::NotSent(_) => undo(tx, &actor, &task)?,
        }
        let task = load(tx, uuid)?;
        let result = json!({"ok": true, "delivery": outcome.name(), "reason": outcome.reason(), "task": view(&task, &reference(tx, uuid)?, true)});
        if !matches!(outcome, Outcome::NotSent(_)) {
            commands::remember(tx, command, request, &result)?;
        }
        Ok(result)
    })?;
    if let Outcome::NotSent(why) = outcome {
        return Err(Refusal::new("not_delivered", format!("not delivered: {why}")).into());
    }
    Ok(result)
}

fn input_id(request_id: &str) -> String {
    if request_id.is_empty() { uuid::Uuid::new_v4().to_string() } else { request_id.to_owned() }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReviewSendRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    session: SessionRef,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn review_send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReviewSendRequest = parse(&request)?;
    let session = local_session("session", Some(req.session.clone()), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "review-send", &request))? {
        return Ok(result);
    }
    let target = local_target(ctx.store, &session)?;
    let dir = ctx.store.dir().to_owned();
    let (uuid, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        review_gates(tx, &task, &session)?;
        Ok((uuid, files(&dir, &task)?))
    })?;
    check_session(&session, &target, &req.confirm)?;
    let actor = ctx.actor.clone();
    let text = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        review_gates(tx, &task, &session)?;
        simple(tx, &actor, &task, "reviewer_set", json!({"session": session}), "")?;
        render(tx, &load(tx, &uuid)?, "review", "")
    })?;
    let outcome = deliver(&session, &target, &input_id(&req.request_id), &text, attachments);
    settle(ctx, "review-send", &request, &uuid, "review", &session, outcome, |tx, actor, task| {
        simple(tx, actor, task, "reviewer_set", json!({"session": null}), "")?;
        Ok(())
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReworkRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn rework_send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReworkRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "rework-send", &request))? {
        return Ok(result);
    }
    let dir = ctx.store.dir().to_owned();
    let (uuid, session, text, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let session = assignee_for(&task, "rework")?;
        Ok((uuid, session, render(tx, &task, "rework", "")?, files(&dir, &task)?))
    })?;
    let target = local_target(ctx.store, &session)?;
    check_session(&session, &target, &req.confirm)?;
    let outcome = deliver(&session, &target, &input_id(&req.request_id), &text, attachments);
    settle(ctx, "rework-send", &request, &uuid, "rework", &session, outcome, |_, _, _| Ok(()))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AnswerRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    answer: String,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn answer(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: AnswerRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "answer", &request))? {
        return Ok(result);
    }
    let text = commands::normalize(&req.answer);
    let (uuid, session) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        Ok((uuid.clone(), assignee_for(&load_writable(tx, &uuid)?, "answer")?))
    })?;
    let target = local_target(ctx.store, &session)?;
    check_session(&session, &target, &req.confirm)?;
    let actor = ctx.actor.clone();
    let typed = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        let fields = BTreeMap::from([("answer".to_owned(), text.clone())]);
        transition(tx, &actor, &task, Action::Answer, super::workflow::RESUME, fields, None, vec![], "")?;
        render(tx, &load(tx, &uuid)?, "answer", &text)
    })?;
    let outcome = deliver(&session, &target, &input_id(&req.request_id), &typed, Vec::new());
    // The answer stays recorded; the person sees it was not delivered.
    settle(ctx, "answer", &request, &uuid, "answer", &session, outcome, |tx, actor, task| {
        journal(tx, actor, task, "[comment]", &format!("the answer was not delivered to {}", session.name), "")
    })
}
```

In `cli.rs`:

```rust
        ("review-send", []) => super::deliver::review_send(&mut ctx, request)?,
        ("rework-send", []) => super::deliver::rework_send(&mut ctx, request)?,
        ("answer", []) => super::deliver::answer(&mut ctx, request)?,
```

`HUMAN_USAGE`:

```
  review-send --json {task, session, confirm?}  |  rework-send --json {task, confirm?}  |  answer --json {task, answer, confirm?}
```

- [ ] **Step 3: Live-session tests** in class `AgentSession`. Generalize the SQLite helper into `append(self, kind, payload, actor=None, base=None, request_id='')` that inserts one event after the latest one (same columns as `claim`), with `actor` defaulting to the creation's actor and `base` defaulting to the task's latest transition (or the creation). Keep `claim()` as a thin wrapper. Then:

```python
    def agent_reaches_review(self, ref, assignee):
        """The task moves to Review as if `assignee` did the work."""
        machine = self.person('show', ref)['task']['created_by']['machine']
        self.claim(name=assignee)
        agent = {'kind': 'agent', 'machine': machine, 'session': assignee}
        self.append('transition', {'action': 'advance', 'to': 'Build', 'fields': {'spec': 's'}}, actor=agent)
        self.append('transition', {'action': 'advance', 'to': 'Review', 'fields': {'worklog': 'w', 'evidence': 'e'}}, actor=agent)

    def test_review_send_types_the_review_text_to_another_session(self):
        ref = self.task()
        self.agent_reaches_review(ref, 'codex/other')
        code, body = self.overmind('review-send', '--json', data={'task': ref, 'session': {'name': self.name}}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['reviewer']['name']), (0, 'sent', self.name), body)
        self.wait_for(lambda: b'Review: Fix the drafts' in self.received())
        verdict = self.ok('review', '--json', data={'task': ref, 'verdict': 'needs_work', 'report': 'Add tests'})
        self.assertEqual(verdict['task']['stage'], 'Build')

    def test_rework_and_answer_reach_the_assignee(self):
        ref = self.task()
        self.claim()
        self.ok('advance', '--json', data={'task': ref, 'to': 'Build', 'spec': 's'})
        self.ok('call-human', '--json', data={'task': ref, 'question': 'REST or gRPC?'})
        code, body = self.overmind('answer', '--json', data={'task': ref, 'answer': 'REST'}, agent=False)
        self.assertEqual((code, body['delivery'], body['task']['stage']), (0, 'sent', 'Build'), body)
        self.wait_for(lambda: b'REST' in self.received())
        code, body = self.overmind('rework-send', '--json', data={'task': ref}, agent=False)
        self.assertEqual((code, body['error']['code']), (1, 'not_reviewed'))
```

- [ ] **Step 4: Run, check, commit**

Run the checks of Task 2 Step 6.

```bash
git commit --only src/overmind/deliver.rs src/overmind/cli.rs tests/test_overmind.py -m "Send Overmind reviews, rework and answers into sessions with the same delivery rules"
```

---

### Task 4: The exchange protocol and peer status

**Files:**
- Create: `src/overmind/sync.rs`
- Modify: `src/overmind/store.rs` (peers table), `src/overmind/mod.rs`, `src/overmind/cli.rs` (`status`), `tests/test_overmind.py`

**Interfaces:**
- Consumes: `Store::{version_vector, missing, insert_raw, machine}`, `Received`.
- Produces (Tasks 5–7 rely on these):
  - `store::PeerState { machine: String, alias: String, vector: Option<BTreeMap<String, u64>>, last_ok: u64, last_error: String, updated: u64 }`; `Store::peer(&self, &str) -> Result<Option<PeerState>>`, `Store::peers(&self) -> Result<Vec<PeerState>>`, `Store::put_peer(&self, &PeerState) -> Result<()>` (local only, never replicated, never bumps `rev`).
  - `sync::FEATURE = "overmind:1"`, `sync::BATCH = 4 MiB`.
  - `sync::serve(store: &mut Store, request: Value) -> Result<Value>` — the peer side of one call.
  - `sync::exchange(store: &mut Store, peer: &str, alias: &str, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<Summary>`; `Summary { rounds, sent, inserted, refused }`.
  - CLI `status` → `{ok, machine, cursor, vector, peers: [{machine, alias, last_ok, last_error, updated, behind}]}`.

Protocol (request and response are JSON objects; unknown fields are ignored for forward compatibility):

```
request  {"v": 1, "from": <caller machine>, "vector": {origin: seq}, "events": [raw events]}
response {"v": 1, "machine": <peer machine>, "vector": {origin: seq}, "events": [raw events], "more": bool}
```

The caller pushes what the peer lacks according to the vector it learned last time (nothing in the first round), the peer inserts them, and answers with what the caller lacks. Rounds continue while either side has more, at most 8 per exchange.

- [ ] **Step 1: Peers table.** In `store.rs` add to `SCHEMA`:

```sql
CREATE TABLE IF NOT EXISTS peers(
  machine TEXT PRIMARY KEY, alias TEXT NOT NULL, vector TEXT,
  last_ok INTEGER NOT NULL, last_error TEXT NOT NULL, updated INTEGER NOT NULL);
```

and:

```rust
/// What this machine learned about a peer in exchanges. Local only.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PeerState {
    pub machine: String,
    pub alias: String,
    /// The peer's version vector after the last exchange; None before the first.
    pub vector: Option<BTreeMap<String, u64>>,
    pub last_ok: u64,
    pub last_error: String,
    pub updated: u64,
}

fn peer_row(row: &rusqlite::Row) -> rusqlite::Result<PeerState> {
    let vector: Option<String> = row.get(2)?;
    Ok(PeerState {
        machine: row.get(0)?,
        alias: row.get(1)?,
        vector: vector.and_then(|v| serde_json::from_str(&v).ok()),
        last_ok: row.get::<_, i64>(3)? as u64,
        last_error: row.get(4)?,
        updated: row.get::<_, i64>(5)? as u64,
    })
}

impl Store {
    pub fn peer(&self, machine: &str) -> Result<Option<PeerState>> {
        Ok(self
            .conn
            .query_row("SELECT machine, alias, vector, last_ok, last_error, updated FROM peers WHERE machine = ?1", params![machine], peer_row)
            .optional()?)
    }

    pub fn peers(&self) -> Result<Vec<PeerState>> {
        let mut statement = self.conn.prepare("SELECT machine, alias, vector, last_ok, last_error, updated FROM peers ORDER BY machine")?;
        let peers = statement.query_map([], peer_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(peers)
    }

    pub fn put_peer(&self, peer: &PeerState) -> Result<()> {
        let vector = peer.vector.as_ref().map(serde_json::to_string).transpose()?;
        self.conn.execute(
            "INSERT OR REPLACE INTO peers(machine, alias, vector, last_ok, last_error, updated) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![peer.machine, peer.alias, vector, peer.last_ok as i64, peer.last_error, peer.updated as i64],
        )?;
        Ok(())
    }
}
```

Remove the `#[allow(dead_code)]` attributes of `version_vector`, `missing`, `insert_raw` and `Received` once `sync.rs` uses them.

- [ ] **Step 2: Write the failing tests** at the end of a new `src/overmind/sync.rs`:

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::fold::fold_many;
    use crate::overmind::model::{Actor, Draft};
    use crate::overmind::store::Filter;
    use serde_json::json;

    const A: &str = "11111111-1111-4111-8111-111111111111";
    const B: &str = "22222222-2222-4222-8222-222222222222";

    fn comment(store: &mut Store, task: &str, size: usize) {
        let machine = store.machine().to_owned();
        store
            .write(|tx| {
                tx.append(&Draft {
                    actor: Actor::human(&machine), task: task.into(), project: "p".into(), kind: "journal".into(),
                    base: String::new(), request_id: String::new(),
                    payload: json!({"marker": "[comment]", "text": "x".repeat(size)}),
                })
            })
            .unwrap();
    }
    fn stores() -> (tempfile::TempDir, Store, Store) {
        let dir = tempfile::tempdir().unwrap();
        let a = Store::open(&dir.path().join("a"), A).unwrap();
        let b = Store::open(&dir.path().join("b"), B).unwrap();
        (dir, a, b)
    }

    #[test]
    fn one_exchange_converges_both_ways() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..3 { comment(&mut a, "t", 10); }
        for _ in 0..2 { comment(&mut b, "u", 10); }
        let mut first = true;
        let summary = exchange(&mut a, B, "b", &mut |request| {
            if first {
                assert!(request["events"].as_array().unwrap().is_empty(), "the first round only pulls");
                first = false;
            }
            serve(&mut b, request.clone())
        })
        .unwrap();
        assert_eq!((summary.inserted, summary.refused), (2, 0));
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap());
        let all = |s: &Store| serde_json::to_value(fold_many(&s.load(&Filter::All).unwrap())).unwrap();
        assert_eq!(all(&a), all(&b));
        let peer = a.peer(B).unwrap().unwrap();
        assert_eq!((peer.alias.as_str(), peer.last_error.as_str(), peer.vector.is_some()), ("b", "", true));
        assert!(peer.last_ok > 0);
    }

    #[test]
    fn large_backlogs_move_in_batches() {
        let (_dir, mut a, mut b) = stores();
        for _ in 0..80 { comment(&mut b, "t", 60 * 1024); }
        let summary = exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert!(summary.rounds >= 2, "{summary:?}");
        assert_eq!(a.version_vector().unwrap(), b.version_vector().unwrap());
    }

    #[test]
    fn a_failed_call_changes_nothing_and_is_reported() {
        let (_dir, mut a, mut b) = stores();
        comment(&mut b, "t", 10);
        let before = a.cursor().unwrap();
        assert!(exchange(&mut a, B, "b", &mut |_| anyhow::bail!("connection to b timed out")).is_err());
        assert!(exchange(&mut a, B, "b", &mut |_| Ok(json!({"v": 1, "machine": A, "vector": {}, "events": []}))).is_err());
        assert_eq!(a.cursor().unwrap(), before);
        let peer = a.peer(B).unwrap().unwrap();
        assert!(peer.last_error.contains("identity"), "{peer:?}");
        exchange(&mut a, B, "b", &mut |request| serve(&mut b, request.clone())).unwrap();
        assert_eq!(a.peer(B).unwrap().unwrap().last_error, "");
    }

    #[test]
    fn serve_refuses_other_versions_and_itself() {
        let (_dir, _a, mut b) = stores();
        assert!(serve(&mut b, json!({"v": 2, "from": A, "vector": {}})).is_err());
        assert!(serve(&mut b, json!({"v": 1, "from": B, "vector": {}})).is_err());
        assert!(serve(&mut b, json!({"v": 1, "from": A, "vector": {}, "later": true})).is_ok());
    }
}
```

Run: `cargo test --locked overmind::sync 2>&1 | tail -5` — Expected: compile errors.

- [ ] **Step 3: Write `sync.rs`** (above the tests):

```rust
//! Replication between machines (spec Part 2). One call pushes and pulls, so a
//! peer reachable in one direction only still converges. The transport is the
//! caller's business: the swarm worker passes its SSH call.
use super::model::now;
use super::store::{PeerState, Store};
use anyhow::{anyhow, ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

/// Advertised in swarm `hello`; peers without it are skipped.
pub const FEATURE: &str = "overmind:1";
pub const BATCH: usize = 4 * 1024 * 1024;
const ROUNDS: usize = 8;

#[derive(Serialize, Deserialize)]
struct ExchangeRequest {
    v: u32,
    from: String,
    vector: BTreeMap<String, u64>,
    #[serde(default)]
    events: Vec<Value>,
}

#[derive(Serialize, Deserialize)]
struct ExchangeResponse {
    v: u32,
    machine: String,
    vector: BTreeMap<String, u64>,
    #[serde(default)]
    events: Vec<Value>,
    #[serde(default)]
    more: bool,
}

#[derive(Debug, Default, PartialEq)]
pub struct Summary {
    pub rounds: usize,
    pub sent: usize,
    pub inserted: usize,
    pub refused: usize,
}

/// The peer's side of one call: store what arrived, answer with what the caller lacks.
pub fn serve(store: &mut Store, request: Value) -> Result<Value> {
    let request: ExchangeRequest =
        serde_json::from_value(request).map_err(|e| anyhow!("invalid Overmind exchange request: {e}"))?;
    ensure!(request.v == 1, "this machine speaks Overmind exchange version 1; update HGS on the newer machine");
    ensure!(
        uuid::Uuid::parse_str(&request.from).is_ok() && request.from != store.machine(),
        "invalid Overmind exchange request"
    );
    store.insert_raw(&request.events)?;
    let (events, more) = store.missing(&request.vector, BATCH)?;
    let response = ExchangeResponse { v: 1, machine: store.machine().into(), vector: store.version_vector()?, events, more };
    Ok(serde_json::to_value(response)?)
}

/// Exchanges with one peer until both sides are current or the rounds run out;
/// the outcome is kept for `status`.
pub fn exchange(store: &mut Store, peer: &str, alias: &str, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<Summary> {
    let result = rounds(store, peer, call);
    let mut state = store.peer(peer)?.unwrap_or_else(|| PeerState { machine: peer.into(), ..PeerState::default() });
    state.alias = alias.into();
    state.updated = now();
    match &result {
        Ok(_) => {
            state.last_ok = state.updated;
            state.last_error.clear();
        }
        Err(error) => state.last_error = format!("{error:#}"),
    }
    store.put_peer(&state)?;
    result
}

fn rounds(store: &mut Store, peer: &str, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<Summary> {
    let mut summary = Summary::default();
    while summary.rounds < ROUNDS {
        let known = store.peer(peer)?.and_then(|p| p.vector);
        let (events, push_more) = match &known {
            Some(vector) => store.missing(vector, BATCH)?,
            None => (Vec::new(), true),
        };
        summary.sent += events.len();
        let request = ExchangeRequest { v: 1, from: store.machine().into(), vector: store.version_vector()?, events };
        let response: ExchangeResponse = serde_json::from_value(call(&serde_json::to_value(&request)?)?)
            .map_err(|e| anyhow!("the peer's Overmind answer is not valid: {e}"))?;
        ensure!(response.v == 1 && response.machine == peer, "the machine behind this peer changed identity");
        let received = store.insert_raw(&response.events)?;
        summary.inserted += received.inserted;
        summary.refused += received.refused;
        summary.rounds += 1;
        let mut state = store.peer(peer)?.unwrap_or_else(|| PeerState { machine: peer.into(), ..PeerState::default() });
        state.vector = Some(response.vector);
        store.put_peer(&state)?;
        if !push_more && !response.more {
            break;
        }
    }
    Ok(summary)
}
```

Add `mod sync;` to `mod.rs`. Add `status` to `commands.rs`:

```rust
pub fn status(ctx: &mut Ctx) -> Result<Value> {
    let vector = ctx.store.version_vector()?;
    let peers: Vec<Value> = ctx
        .store
        .peers()?
        .into_iter()
        .map(|peer| {
            let behind = peer.vector.as_ref().map(|theirs| {
                vector.iter().map(|(origin, seq)| seq.saturating_sub(theirs.get(origin).copied().unwrap_or(0))).sum::<u64>()
            });
            json!({"machine": peer.machine, "alias": peer.alias, "last_ok": peer.last_ok,
                   "last_error": peer.last_error, "updated": peer.updated, "behind": behind})
        })
        .collect();
    Ok(json!({"ok": true, "machine": ctx.store.machine(), "cursor": ctx.store.cursor()?, "vector": vector, "peers": peers}))
}
```

CLI: `("status", []) => commands::status(&mut ctx)?,`; `HUMAN_USAGE`: `status   replication state with every peer`. Python test in class `Overmind`:

```python
    def test_status_on_a_lone_machine(self):
        self.ok('create', '--json', data={'project': 'demo', 'prompt': 'x'})
        status = self.ok('status')
        self.assertEqual((status['peers'], list(status['vector'].values())), ([], [1]))
```

- [ ] **Step 4: Run, check, commit**

Run: `cargo test --locked overmind 2>&1 | tail -3`, warnings 0, Python `tests.test_overmind`.

```bash
git add src/overmind/sync.rs
git commit --only src/overmind/sync.rs src/overmind/store.rs src/overmind/mod.rs src/overmind/commands.rs src/overmind/cli.rs tests/test_overmind.py -m "Exchange Overmind events with a peer in one push-and-pull call and report each peer's state"
```

---

### Task 5: Replication through the swarm worker

**Files:**
- Modify: `src/swarm/transport.rs` (`hello`, `sync_peer`, `worker`), `src/swarm/mod.rs` (`execute` arm, `call`), `src/overmind/mod.rs` (`serve`, `replicate`, `revision`, re-export `FEATURE`)
- Create: `tests/test_overmind_sync.py`

**Interfaces:**
- Consumes: Task 4 `sync::{serve, exchange, FEATURE}`, `swarm::local_node_id`, `transport::remote`.
- Produces (Tasks 6, 7 rely on these):
  - `crate::overmind::serve(config: &Config, action: &str, request: Value) -> anyhow::Result<Value>` — dispatches `overmind-*` swarm actions (Task 5: `overmind-exchange`).
  - `crate::overmind::replicate(config: &Config, alias: &str, node: &str, call: &mut dyn FnMut(&Value) -> anyhow::Result<Value>) -> anyhow::Result<()>`.
  - `crate::overmind::revision(config: &Config) -> String` — contents of `<state>/overmind/rev`, empty when absent.
  - `crate::swarm::call(config: &Config, alias: &str, action: &str, request: Option<&Value>) -> anyhow::Result<Value>` — `transport::remote` for other modules.
  - Swarm `hello` gains `"features": ["overmind:1"]` on Unix builds.

- [ ] **Step 1: Overmind entry points** in `src/overmind/mod.rs`:

```rust
pub use sync::FEATURE;

fn open(config: &crate::config::Config) -> anyhow::Result<store::Store> {
    store::Store::open(&config.state.join("overmind"), &crate::swarm::local_node_id(config)?)
}

/// Serves an Overmind request that arrived over the swarm channel.
pub fn serve(config: &crate::config::Config, action: &str, request: serde_json::Value) -> anyhow::Result<serde_json::Value> {
    let mut store = open(config)?;
    match action {
        "overmind-exchange" => sync::serve(&mut store, request),
        _ => anyhow::bail!("this machine does not know the Overmind request {action}; update HGS on it"),
    }
}

/// One exchange with a swarm peer; the outcome is kept for `hgs overmind status`.
pub fn replicate(
    config: &crate::config::Config,
    alias: &str,
    node: &str,
    call: &mut dyn FnMut(&serde_json::Value) -> anyhow::Result<serde_json::Value>,
) -> anyhow::Result<()> {
    sync::exchange(&mut open(config)?, node, alias, call).map(|_| ())
}

/// Changes with every committed Overmind write, so the worker can exchange at once.
pub fn revision(config: &crate::config::Config) -> String {
    std::fs::read_to_string(config.state.join("overmind/rev")).unwrap_or_default()
}
```

- [ ] **Step 2: Swarm hooks.** In `src/swarm/mod.rs` add (cfg-free; transport is cross-platform):

```rust
/// One swarm transport call for other modules (Overmind replication and delivery).
pub(crate) fn call(config: &Config, alias: &str, action: &str, request: Option<&Value>) -> Result<Value> {
    transport::remote(config, alias, action, request)
}
```

and in `execute`, before the final catch-all arm:

```rust
        action if action.starts_with("overmind-") => {
            #[cfg(unix)]
            {
                crate::overmind::serve(config, action, input()?)
            }
            #[cfg(not(unix))]
            {
                bail!("Overmind is not available on this machine")
            }
        }
```

In `src/swarm/transport.rs`:
- `hello`: after building `result`, add `#[cfg(unix)] { result["features"] = json!([crate::overmind::FEATURE]); }`.
- `sync_peer`: after the final `local.save()` that records `last_sync` (keep its result), release the catalog lock — `replicate` opens the Overmind store, and `local_node_id` takes that same lock, which would block this process forever — then run the Overmind step without failing the catalog sync:

```rust
    let saved = local.save();
    drop(local);
    #[cfg(unix)]
    if identity["features"].as_array().is_some_and(|features| features.iter().any(|f| f == crate::overmind::FEATURE)) {
        let node = identity["node_id"].as_str().unwrap_or_default().to_owned();
        let mut call = |request: &Value| remote(config, alias, "overmind-exchange", Some(request));
        if let Err(error) = crate::overmind::replicate(config, alias, &node, &mut call) {
            eprintln!("hgs swarm: Overmind exchange with {alias}: {error:#}");
        }
    }
    saved
```

- `worker`: next to `last_digest` keep `let mut last_overmind = String::new();`. At the start of each pass read `#[cfg(unix)] let overmind = crate::overmind::revision(&config);` (`String::new()` elsewhere) and use `let changed = digest != last_digest || overmind != last_overmind;`. At the end of the pass, next to `last_digest = ...`, set `last_overmind = overmind;` — the value read at the start. A pass that received events therefore leaves `rev` changed, so the next pass exchanges with every peer once more (forwarding what arrived) and then settles.

- [ ] **Step 3: Two-machine tests.** Create `tests/test_overmind_sync.py`. Reuse `test_swarm` for the fake `ssh` (`import test_swarm as swarm`), extended with one marker: a file `no-overmind-<alias>` makes the fake remote strip `features` from `hello` output (write your own `SSH` constant: copy `swarm.SSH` and add `if args[1:] == ['hello'] and (root / ('no-overmind-' + alias)).exists():` handling that runs hgs, drops `features`, prints the result). Nodes `a` and `b` as in `Swarm.setUp` (config `HGS_SELF`/`HGS_PEERS`, `machines.local.json`, `initialize`), joined both ways.

```python
class Replication(unittest.TestCase):
    # setUp: like test_swarm.Swarm.setUp for nodes a, b (peers of each other), then join a->b and b->a.

    def overmind(self, node, *args, data=None):
        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / node), HGS_STATE_DIR=str(self.root / node / 'state'))
        result = subprocess.run([str(swarm.HGS), 'overmind', *args], input=None if data is None else json.dumps(data),
                                env=env, text=True, capture_output=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return json.loads(result.stdout)

    def refs(self, node):
        return sorted(t['ref'] for t in self.overmind(node, 'board', '--json', data={'project': 'demo'})['tasks'])

    def test_sync_replicates_both_ways_over_one_direction(self):
        made_on_a = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'from a'})['task']['ref']
        made_on_b = self.overmind('b', 'create', '--json', data={'project': 'demo', 'prompt': 'from b'})['task']['ref']
        self.call('a', 'sync')  # only a connects
        self.assertEqual(self.refs('a'), self.refs('b'))
        self.assertEqual(set(self.refs('a')), {made_on_a, made_on_b})
        peers = self.overmind('a', 'status')['peers']
        self.assertEqual([(p['alias'], p['last_error'], p['behind']) for p in peers], [('b', '', 0)])

    def test_peers_without_overmind_are_skipped(self):
        (self.root / 'no-overmind-b').write_text('')
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})
        self.call('a', 'sync')
        calls = (self.root / 'calls').read_text()
        self.assertNotIn('overmind-exchange', calls)
        self.assertEqual(self.overmind('a', 'status')['peers'], [])

    def test_offline_peer_is_reported_and_recovers(self):
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})
        self.call('a', 'sync')
        (self.root / 'offline-b').write_text('')
        self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'y'})
        self.call('a', 'sync')  # the catalog sync reports b offline; Overmind keeps its last good state
        (self.root / 'offline-b').unlink()
        self.call('a', 'sync')
        self.assertEqual(self.refs('a'), self.refs('b'))

    def test_the_worker_exchanges_soon_after_a_local_write(self):
        env = dict(self.env, HGS_CONFIG_DIR=str(self.root / 'a'), HGS_STATE_DIR=str(self.root / 'a/state'))
        worker = subprocess.Popen([str(swarm.HGS), 'swarm', 'worker'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            time.sleep(1)
            ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x'})['task']['ref']
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline and ref not in self.refs('b'):
                time.sleep(.2)
            self.assertIn(ref, self.refs('b'))
        finally:
            worker.terminate(); worker.communicate(timeout=5)
```

(`call` is the swarm helper from `test_swarm.Swarm`; copy it. `hgs swarm sync` exits 0 when a peer is offline only if the existing command reports per-peer errors in its JSON — check `tests/test_swarm.py` for how it behaves offline and use `ok=False` if needed.)

- [ ] **Step 4: Run, check, commit**

Run: `cargo build --locked`, warnings 0, `cargo test --locked 2>&1 | grep '^test result'`, `PYTHONPATH=tests HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind_sync tests.test_swarm 2>&1 | tail -3`, `python3 scripts/ci/check-source.py 2>&1 | tail -2`.

```bash
git add tests/test_overmind_sync.py
git commit --only src/swarm/transport.rs src/swarm/mod.rs src/overmind/mod.rs tests/test_overmind_sync.py -m "Replicate Overmind events with every swarm peer that advertises overmind:1, at once after a local write"
```

---

### Task 6: Attachment blobs between machines

**Files:**
- Create: `src/overmind/remote.rs`
- Modify: `src/overmind/mod.rs` (`serve` arms, `mod remote;`), `src/overmind/deliver.rs` (fetch missing images before refusing), `src/overmind/commands.rs` or `remote.rs` (`attachment` command), `src/overmind/cli.rs`, `src/swarm/mod.rs` (`alias_for`), `tests/test_overmind_sync.py`

**Interfaces:**
- Consumes: Task 5 `swarm::call`, `overmind::serve`; `blobs::{put, path}`; `fold::Attachment`.
- Produces (Task 7 relies on these):
  - `crate::swarm::alias_for(config: &Config, machine: &str) -> Option<String>` — a configured peer alias whose verified binding is that machine (`verified_bindings` inverted, restricted to `config.peers`).
  - Swarm actions `overmind-blob` `{sha256, offset, length ≤ 524288}` → `{size, data_base64}`, and `overmind-blobs` `{want?: [sha256], put?: [{sha256, data_base64}]}` → `{missing: [sha256]}` (puts are verified against their digest).
  - `remote::RANGE = 512 * 1024`; `remote::ensure_blobs(store: &Store, task: &Task) -> Result<()>` — fetches every missing image from the peers in `store.peers()` (the task creator's machine first), refusing `attachment_unavailable` when none has it; `remote::push_blobs(config, alias, store, task) -> Result<()>`.
  - CLI `attachment --json {task, sha256}` → `{ok, path, name, mime, bytes}` for people, and for the task's assignee or reviewer session.

- [ ] **Step 1: Failing two-machine tests** (append to `tests/test_overmind_sync.py`):

```python
    def image(self, size):
        data = bytes(range(256)) * (size // 256)
        return {'name': 'shot.png', 'mime': 'image/png', 'data_base64': base64.b64encode(data).decode()}, data

    def test_images_are_fetched_in_ranges_when_needed(self):
        attachment, data = self.image(700 * 1024)
        task = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment]})['task']
        self.call('a', 'sync')
        sha = task['attachments'][0]['sha256']
        got = self.overmind('b', 'attachment', '--json', data={'task': task['ref'], 'sha256': sha})
        self.assertEqual(Path(got['path']).read_bytes(), data)
        calls = (self.root / 'calls').read_text()
        self.assertEqual(calls.count('overmind-blob\n'), 2)

    def test_missing_blob_refuses_before_claiming(self):
        attachment, _ = self.image(1024)
        task = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'attachments': [attachment], 'stage': 'Queue'})['task']
        self.call('a', 'sync')
        (self.root / 'offline-a').write_text('')
        result = self.overmind_raw('b', 'attachment', '--json', data={'task': task['ref'], 'sha256': task['attachments'][0]['sha256']})
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'attachment_unavailable')
```

(`overmind_raw` returns the `CompletedProcess` without asserting success; add it next to `overmind`. A send test for this refusal lives in Task 7, where a live session exists.)

- [ ] **Step 2: Implement.** `src/swarm/mod.rs`:

```rust
/// The configured peer alias whose verified binding is `machine`.
#[cfg(unix)]
pub(crate) fn alias_for(config: &Config, machine: &str) -> Option<String> {
    let local = saved_node_id(config)?;
    verified_bindings(config, &local)
        .into_iter()
        .find(|(alias, id)| id == machine && config.peers.contains(alias))
        .map(|(alias, _)| alias)
}
```

`src/overmind/remote.rs`:

```rust
//! Reaching other machines for blobs and deliveries, over the swarm channel.
use super::blobs;
use super::fold::{Attachment, Task};
use super::model::Refusal;
use super::store::Store;
use crate::config::Config;
use anyhow::{anyhow, ensure, Result};
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub const RANGE: usize = 512 * 1024;

fn unavailable(attachment: &Attachment) -> anyhow::Error {
    Refusal::field("attachment_unavailable", "attachments", "missing", format!("no reachable machine holds the image {}", attachment.name)).into()
}

/// Serves one range of a stored blob.
pub fn serve_blob(store: &Store, request: &Value) -> Result<Value> {
    let sha256 = request["sha256"].as_str().unwrap_or_default();
    let offset = request["offset"].as_u64().unwrap_or(0) as usize;
    let length = (request["length"].as_u64().unwrap_or(RANGE as u64) as usize).min(RANGE);
    let bytes = std::fs::read(blobs::path(store.dir(), sha256)?).map_err(|_| anyhow!("missing"))?;
    let end = (offset + length).min(bytes.len());
    ensure!(offset <= end, "invalid range");
    Ok(json!({"size": bytes.len(), "data_base64": base64::engine::general_purpose::STANDARD.encode(&bytes[offset..end])}))
}

/// Stores pushed blobs after checking their digests; answers which wanted ones are missing.
pub fn serve_blobs(store: &Store, request: &Value) -> Result<Value> {
    for item in request["put"].as_array().into_iter().flatten() {
        let bytes = base64::engine::general_purpose::STANDARD.decode(item["data_base64"].as_str().unwrap_or_default())?;
        ensure!(format!("{:x}", Sha256::digest(&bytes)) == item["sha256"].as_str().unwrap_or_default(), "a pushed image does not match its digest");
        blobs::put(store.dir(), &bytes)?;
    }
    let missing: Vec<&str> = request["want"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|sha| blobs::path(store.dir(), sha).map_or(true, |path| !path.exists()))
        .collect();
    Ok(json!({"missing": missing}))
}

fn present(store: &Store, attachment: &Attachment) -> bool {
    blobs::path(store.dir(), &attachment.sha256).is_ok_and(|path| path.exists())
}

fn fetch(config: &Config, alias: &str, store: &Store, attachment: &Attachment) -> Result<()> {
    let mut bytes = Vec::new();
    while (bytes.len() as u64) < attachment.bytes {
        let part = crate::swarm::call(
            config,
            alias,
            "overmind-blob",
            Some(&json!({"sha256": attachment.sha256, "offset": bytes.len(), "length": RANGE})),
        )?;
        let chunk = base64::engine::general_purpose::STANDARD.decode(part["data_base64"].as_str().unwrap_or_default())?;
        ensure!(!chunk.is_empty(), "the peer returned an empty range");
        bytes.extend(chunk);
    }
    ensure!(format!("{:x}", Sha256::digest(&bytes)) == attachment.sha256, "a fetched image does not match its digest");
    blobs::put(store.dir(), &bytes)?;
    Ok(())
}

/// Fetches every image of `task` that this machine lacks, trying the
/// creator's machine first and then every other peer.
pub fn ensure_blobs(store: &Store, task: &Task) -> Result<()> {
    let missing: Vec<&Attachment> = task.attachments.iter().filter(|a| !present(store, a)).collect();
    if missing.is_empty() {
        return Ok(());
    }
    let config = Config::load().map_err(|e| anyhow!(e.message))?;
    let mut aliases: Vec<String> = Vec::new();
    if let Some(alias) = crate::swarm::alias_for(&config, &task.created_by.machine) {
        aliases.push(alias);
    }
    for peer in store.peers()? {
        if !peer.alias.is_empty() && !aliases.contains(&peer.alias) {
            aliases.push(peer.alias);
        }
    }
    for attachment in missing {
        let found = aliases.iter().any(|alias| fetch(&config, alias, store, attachment).is_ok());
        if !found {
            return Err(unavailable(attachment));
        }
    }
    Ok(())
}

/// Gives the machine behind `alias` the task's images it lacks.
pub fn push_blobs(config: &Config, alias: &str, store: &Store, task: &Task) -> Result<()> {
    let want: Vec<&str> = task.attachments.iter().map(|a| a.sha256.as_str()).collect();
    if want.is_empty() {
        return Ok(());
    }
    let answer = crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"want": want})))?;
    let mut put = Vec::new();
    for sha in answer["missing"].as_array().into_iter().flatten().filter_map(Value::as_str) {
        let attachment = task.attachments.iter().find(|a| a.sha256 == sha).ok_or_else(|| anyhow!("unexpected image request"))?;
        let bytes = std::fs::read(blobs::path(store.dir(), sha)?).map_err(|_| unavailable(attachment))?;
        put.push(json!({"sha256": sha, "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes)}));
    }
    if !put.is_empty() {
        crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"put": put})))?;
    }
    Ok(())
}
```

In `mod.rs` `serve`: add `"overmind-blob" => remote::serve_blob(&store, &request)` and `"overmind-blobs" => remote::serve_blobs(&store, &request)`; add `mod remote;`.

`attachment` command (in `remote.rs` or `commands.rs`):

```rust
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct AttachmentRequest {
    task: String,
    sha256: String,
}

pub fn attachment(ctx: &mut super::commands::Ctx, request: Value) -> Result<Value> {
    let req: AttachmentRequest = super::commands::parse(&request)?;
    let actor = ctx.actor.clone();
    let task = ctx.store.read(|tx| super::commands::load(tx, &super::commands::resolve(tx, &req.task)?))?;
    if actor.kind != super::model::ActorKind::Human {
        let me = actor.session_ref();
        if me.is_none() || (me != task.assignee && me != task.reviewer) {
            return super::commands::refuse("not_assigned", "this task is not assigned to your session");
        }
    }
    let attachment = task
        .attachments
        .iter()
        .find(|a| a.sha256 == req.sha256)
        .ok_or_else(|| Refusal::field("invalid_field", "sha256", "invalid", "this task has no such image"))?;
    let only = Task { attachments: vec![attachment.clone()], ..task.clone() };
    ensure_blobs(ctx.store, &only)?;
    Ok(json!({"ok": true, "path": blobs::path(ctx.store.dir(), &attachment.sha256)?, "name": attachment.name,
              "mime": attachment.mime, "bytes": attachment.bytes}))
}
```

CLI `("attachment", []) => super::remote::attachment(&mut ctx, request)?,`; add it to both usages (`attachment --json {task, sha256}   local path of a task image, fetched from another machine when needed`).

In `deliver.rs`, before every `files(&dir, &task)?` call, add `super::remote::ensure_blobs(ctx.store, &task)?;` in the read phase — `ensure_blobs` takes `&Store`; do it after the `read` closure returns the task (re-read the task in the closure that calls `files`). Keep `files` refusing when bytes are still missing.

- [ ] **Step 3: Run, check, commit**

Same checks as Task 5 Step 4.

```bash
git add src/overmind/remote.rs
git commit --only src/overmind/remote.rs src/overmind/mod.rs src/overmind/deliver.rs src/overmind/cli.rs src/swarm/mod.rs tests/test_overmind_sync.py -m "Fetch Overmind images from other machines in ranges and push them where a task is delivered"
```

---

### Task 7: Delivering to a session on another machine

**Files:**
- Modify: `src/overmind/remote.rs` (`forward`, `serve_deliver`), `src/overmind/deliver.rs` (route by the session's machine), `src/overmind/mod.rs` (`serve` arm), `tests/test_overmind_sync.py`

**Interfaces:**
- Consumes: Tasks 2–3 commands, Task 4 `sync::exchange`, Task 5 `swarm::call`, Task 6 `alias_for`, `ensure_blobs`, `push_blobs`.
- Produces: swarm action `overmind-deliver` `{command: "send"|"review-send"|"rework-send"|"answer", request}` → the command's JSON result, or `{"ok": false, "error": refusal}`; `send`, `review-send`, `rework-send` and `answer` work for sessions on any machine of the swarm.

Routing: the session that receives the text decides the machine (`session` for `send` and `review-send`; the task's `assignee` for `rework-send` and `answer`). When it is another machine:

1. `alias = swarm::alias_for(machine)`, else refuse `host_unreachable` (field `session`): "this machine has no verified route to the session's machine; connect it in the swarm".
2. `hello` on that alias; without `overmind:1` in `features` refuse `host_outdated`: "update HGS on the session's machine".
3. `sync::exchange` with it (pushes everything the host lacks, so the host knows the task as this machine does).
4. `ensure_blobs` locally, then `push_blobs` to the host.
5. `swarm::call(alias, "overmind-deliver", {command, request})`.
6. `sync::exchange` again to pull the claim and its outcome (a failure here only delays the board).
7. Return the host's result; a host refusal becomes the same `Refusal` here.

On the host, `overmind-deliver` builds `Ctx { store, actor: Actor::human(host machine), cwd: None }` and runs the command with routing disabled: a session that is not local there refuses with `remote_session` (no chains).

- [ ] **Step 1: Failing two-machine test with a live session on `b`.** The fixture combines `test_swarm`'s nodes with `test_input`'s isolated tmux socket and fake agent: start the fake agent session as `InputTransport.setUp` does, but with `HGS_STATE_DIR` set to node `b`'s state directory (write the binding record there; the `tmux` shim in `PATH` points both nodes at the same temporary socket, and only `b` has the record). Make the fake `ssh` keep that `PATH` (it inherits the test environment). Then:

```python
    def test_send_reaches_a_session_on_another_machine(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'Fix the drafts', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        result = self.overmind('a', 'send', '--json', data={'task': ref, 'session': {'machine': b, 'name': self.name}})
        self.assertEqual(result['delivery'], 'sent', result)
        self.wait_for(lambda: f'[Overmind {ref}]'.encode() in self.received())
        on_a = self.overmind('a', 'show', ref)['task']
        self.assertEqual((on_a['stage'], on_a['assignee']['machine']), ('Design', b))
        self.assertIn('[sent]', [j['marker'] for j in on_a['journal']])
        claim = next(j for j in on_a['journal'] if j['marker'] == '[claim]')
        self.assertEqual(claim['actor']['machine'], b)  # the host wrote the claim

    def test_unreachable_and_outdated_hosts_refuse(self):
        ref = self.overmind('a', 'create', '--json', data={'project': 'demo', 'prompt': 'x', 'stage': 'Queue'})['task']['ref']
        b = self.overmind('b', 'status')['machine']
        request = {'task': ref, 'session': {'machine': b, 'name': self.name}}
        (self.root / 'no-overmind-b').write_text('')
        self.assertEqual(json.loads(self.overmind_raw('a', 'send', '--json', data=request).stdout)['error']['code'], 'host_outdated')
        (self.root / 'no-overmind-b').unlink()
        (self.root / 'offline-b').write_text('')
        self.assertEqual(self.overmind_raw('a', 'send', '--json', data=request).returncode, 1)
        self.assertEqual(self.overmind('a', 'show', ref)['task']['stage'], 'Queue')
```

- [ ] **Step 2: Implement** `forward` and `serve_deliver` in `remote.rs` following the routing steps above (use `super::sync::exchange(store, machine, alias, &mut |r| crate::swarm::call(&config, alias, "overmind-exchange", Some(r)))`; convert a host `{"ok": false, "error": {...}}` with `serde_json::from_value::<Refusal>` into `Err(refusal.into())`). In `deliver.rs`, give each of the four commands an internal variant with a `local_only: bool` parameter: the public function passes `false`; `serve_deliver` passes `true`. When the receiving session's machine differs from `ctx.store.machine()`, `local_only` refuses with `remote_session`, otherwise the command calls `remote::forward(ctx.store, command, &request, &task_uuid, &machine)` before writing anything. Add `"overmind-deliver" => remote::serve_deliver(&mut store, &request)` to `serve`.

- [ ] **Step 3: Run, check, commit**

Same checks as Task 5 Step 4, plus `tests.test_overmind`.

```bash
git commit --only src/overmind/remote.rs src/overmind/deliver.rs src/overmind/mod.rs tests/test_overmind_sync.py -m "Deliver Overmind tasks to sessions on other machines through their host"
```

---

### Task 8: Documentation, spec and tracker

**Files:**
- Modify: `docs/reference.md` (section "Overmind tasks"), `docs/superpowers/specs/2026-10-11-overmind-core-design.md`

- [ ] **Step 1: Reference.** In "Overmind tasks" add, in plain English, short paragraphs on:
  - Sending: `send` claims a task in Queue for a session and types the project's `dispatch` template like `hgs send`; refusals that a person may confirm (`agent_busy`, `wip_exceeded`, `predecessors_open`, `pinned_elsewhere`) are repeated with `confirm`; outcomes `sent`, `uncertain` (flag `delivery_uncertain`, settled with `resolve-delivery`) and `not_delivered` (the task returns to Queue). Retrying with the same `request_id` never types twice. DeepSeek native sessions are not supported yet.
  - `review-send`, `rework-send`, `answer`; `templates get|set` with the variables `{task.ref}`, `{task.title}`, `{task.prompt}`, `{task.stage}`, `{task.journal_tail}`, `{review.report}`, `{answer.text}`, `{tools}`.
  - Replication: the swarm worker exchanges with every swarm peer that runs a version with Overmind, within seconds of a local write; `hgs overmind status` shows each peer's last exchange and error; images are fetched when needed (`attachment`); a send to a session on another machine runs on that machine.
- [ ] **Step 2: Spec.** Record the decisions of this plan: Part 5 step 2 ("the bundle is an exchange", reason and cost), the forwarded actor, DeepSeek refusal, confirmations as refusal codes, the `rev` nudge, `hello` `features`, and the deferred `orphaned` flag. Keep the Part 4 command list in sync (`status`, `attachment`, `unpin`).
- [ ] **Step 3: Beads.** `bd create --parent zerus-89h --type task --priority 2 --title "Overmind: flag tasks whose assignee session was terminated or archived (orphaned)" --description "<why and where: archive/terminate/kill paths on the session's host, best effort like session_renamed>"`. Move `zerus-89h.2.4` under the epic: `bd update zerus-89h.2.4 --parent zerus-89h`.
- [ ] **Step 4: Check and commit.** `python3 scripts/ci/check-source.py 2>&1 | tail -2`.

```bash
git commit --only docs/reference.md docs/superpowers/specs/2026-10-11-overmind-core-design.md -m "Document sending Overmind tasks and replication between machines"
```
