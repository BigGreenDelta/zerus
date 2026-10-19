# Task Commands for Agents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Agents use the project task list through `hgs task` in their own shell.
- A command proves its session the way `hgs self` does.
- Commands work on the session's project.
- An agent reads one-line answers.
- Claude Code and Codex sessions get the task instructions from Zerus's SessionStart hook.

**Architecture:**
- `tasks::actor` proves an agent with `state::session_graph::caller_record`: the run is live, the command descends from the agent, and the session's current name is known.
- A new `swarm::session_project` gives the session's catalog project. Commands fall back to it when no project is named.
- The commands return their JSON instead of printing it, and `dispatch` prints either the JSON or the agent's one-line `answer`.
- `state::hooks::hook` is split into the existing tracking (`track`) and a wrapper. On a main-session SessionStart of Claude Code or Codex, the wrapper prints `tasks::session_context`, the `additionalContext` hook output.
- No MCP server, no launch flags, no new run fields.

**Tech Stack:** Rust 1.99.1 (`hgs`, serde_json, chrono), Python `unittest` through the real CLI, and native contract tests with the real Claude Code and Codex against localhost fixtures.

**Spec:**
- `docs/superpowers/specs/2026-10-13-project-task-queue-design.md`: the sections "Commands for agents", "Answers" and "Instructions at session start".
- `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`: phase 4.

## Global Constraints

- Build only with `cargo +1.99.1 build --locked --target-dir .ci-build/cargo`. Never build into `target/debug` or `target/release`: running agents and the live install use them.
- Python suites run from the repository root against the new build:
  - `from tests import …` suites: `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.<module> -v`
  - `import test_pause` suites: `… python3 -m unittest discover -s tests -p <file> -v`
- Clippy stays clean: `cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings`.
- `crates/replica` remains the only code that imports Loro.
- Repository text is in English. Fixtures use invented names only: project `orchard` (`project-1`), project `team/web` (`project-2`), and sessions `codex/api`, `codex/task-test`, `codex/planner`, `codex/local` and `codex/remote`.
- Spec, verbatim:
  - "A command run inside an agent session acts as that session only when it can prove it, as `hgs self` does: the run is live on this computer and the command descends from its agent. The proof follows a rename to the session's current name. A command whose environment names a session it cannot prove is refused with the reason."
  - "Commands work on the session's catalog project unless a reference (`PROJECT/T-xxxxxx`) or `--project` names another; an agent of a session in no project names one."
  - "Read commands print JSON. A change an agent makes prints one line that says what happened and what to do next; `--json` prints the task instead. The person's commands print JSON as before."
  - The answers are the "Answers" table, verbatim.
  - "Zerus's SessionStart hook gives Claude Code and Codex sessions these instructions … Other agents get them once their hooks are reviewed."
- The desktop never runs as an agent, so its `hgs task` JSON stays exactly as it is.
- Commits:
  - No co-author lines.
  - Stage only this plan's files.
  - Never push, merge, rebase or switch branches; work stays on `task-tracker`.
- Every tmux kill names its socket (`-S`). The fixtures already do.

## Review Focus

1. **A session renamed after it started.** Commands act and file under its current name. Pinned in Task 1 by `test_a_renamed_session_acts_under_its_current_name`.
2. **An environment that names a session whose run ended or never ran here.** The command refuses and records nothing; it never falls back to the person. Pinned in Task 1 by `test_an_unproven_session_is_refused`.
3. **A refused send by an agent.** The line names the destination and says nothing was sent. A paste that did happen ("the prompt was sent, but …") is never reworded. Pinned in Task 2 by `test_a_refused_send_names_its_session`.
4. **A forwarded send or cancel for another computer's session.** The agent gets the same one-line answer as for a local one. Pinned in Task 2 by the routing test `test_an_agents_send_answers_in_one_line`.
5. **A hook run that fails, or a subagent's SessionStart.** Nothing is printed, so the agent never parses a half answer. A main-session SessionStart of Kimi prints nothing either. Pinned in Task 3 by `test_subagents_other_agents_and_other_events_read_nothing`.

---

### Task 1: An agent's command proves its session and works on its project

**Files:**
- Modify: `src/tasks.rs`. Change `actor`; add `own_project` and `project_arg`; `reference`, `list`, `add`, `change` and `settings` decide the caller first.
- Modify: `src/swarm/mod.rs`. Add `session_project`.
- Modify: `tests/test_tasks.py`. Agents in the fixture are proven runs; add the `AgentIdentity` tests and a command runner in the scripted agent.
- Modify: `tests/test_task_routing.py`. Its agent is a proven run on node a.

**Interfaces:**
- Produces:
  - `pub(crate) fn swarm::session_project(config: &Config, name: &str) -> anyhow::Result<Option<(String, String)>>`.
  - In `tests/test_tasks.py`:
    - `agent_record(state, env, name, run_id, pid=None, **fields)` writes a record and returns its path.
    - `join(env, project_id, name)`.
    - `Fixture.hgs(..., session=NAME)`: the test process is NAME's agent.
    - `TaskDelivery.as_agent(*args, text=None, ok=True)` runs `hgs task` inside the scripted agent of `self.name` and returns stdout, or stderr when `ok=False`.

- [ ] **Step 1: Write the failing tests and prove the fixture's agents**

In `tests/test_tasks.py`:

(a) Below the imports, add:

```python
def agent_record(state, env, name, run_id, pid=None, **fields):
    """A live run of `name` whose agent is process `pid` (this test process by
    default): commands its descendants run act as that session's agent."""
    pid = pid or os.getpid()
    start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="],
                                    env=dict(env, LC_ALL="C", TZ="UTC"), text=True).strip()
    record = dict(version=1, name=name, agent=name.split("/")[0], run_id=run_id,
                  conversation_id=str(uuid.uuid4()), pid=pid, process_start=start, activity="idle",
                  phase="idle", active_tools={}, subagents={}, last_event_at=time.time(), created=time.time())
    record.update(fields)
    path = Path(state) / (hashlib.sha256(name.encode()).hexdigest() + ".json")
    path.write_text(json.dumps(record))
    return path


def join(env, project_id, name):
    """Moves this computer's session `name` into the catalog project `project_id`."""
    def swarm(*args, data=None):
        result = subprocess.run([str(HGS), "swarm", *args], input=json.dumps(data) if data is not None else None,
                                env=env, text=True, capture_output=True, timeout=30)
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    node = swarm("export")["node_id"]
    before = swarm("get")
    desired = json.loads(json.dumps(before["organization"]))
    for project in desired["projects"]:
        project["sessions"] = [s for s in project.get("sessions", []) if s.split("\n", 1)[-1] != name]
        if project["id"] == project_id:
            project["sessions"].append(node + "\n" + name)
    swarm("apply", data=dict(base=before["organization"], desired=desired, versions=before["versions"]))
```

(b) In `Fixture.setUp`, after `self.state.mkdir()`, add `self.runs = {}`. Replace `hgs` with:

```python
    def agent(self, name):
        """The run of `name` whose agent is this test process."""
        if name not in self.runs:
            self.runs[name] = "run-" + str(uuid.uuid4())
            agent_record(self.state, self.env, name, self.runs[name])
        return self.runs[name]

    def hgs(self, *args, text=None, ok=True, session=None):
        env = dict(self.env)
        if session:
            # An agent: its session and its run, which this test process owns.
            env.update(HGS_SESSION=session, HGS_RUN_ID=self.agent(session))
        result = subprocess.run([str(HGS), "task", *args], input=text, env=env, text=True,
                                capture_output=True, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr
```

(c) The scripted agent runs the commands the test hands it, the way an agent's shell runs them. Append this to the `AGENT` string, before its `while True:` loop:

```python
import json, subprocess, threading, time
def commands():
    inbox, outbox = root / 'commands', root / 'results'
    while True:
        for request in sorted(inbox.glob('*.json')):
            job = json.loads(request.read_text())
            request.unlink()
            done = subprocess.run(job['argv'], input=job['stdin'], env=job['env'], text=True, capture_output=True)
            answer = outbox / (request.stem + '.tmp')
            answer.write_text(json.dumps(dict(code=done.returncode, stdout=done.stdout, stderr=done.stderr)))
            answer.rename(outbox / request.name)
        time.sleep(.02)
threading.Thread(target=commands, daemon=True).start()
```

(d) In `TaskDelivery.setUp`:
- Create both folders before starting tmux: `(self.root / "commands").mkdir()` and `(self.root / "results").mkdir()`.
- Write the record with `agent_record(self.state, self.env, self.name, self.run_id, pid=pid, agent="codex", pane=pane)` in place of `self.record_path.write_text(...)`. Keep `self.record_path` (the path it returns).
- Set `self.runs[self.name] = self.run_id`.

Then add:

```python
    def as_agent(self, *args, text=None, ok=True):
        """`hgs task ARGS` run by the scripted agent of `self.name`, as its shell would."""
        job = str(uuid.uuid4())
        env = dict(self.env, HGS_SESSION=self.name, HGS_RUN_ID=self.run_id)
        request = self.root / "commands" / (job + ".tmp")
        request.write_text(json.dumps(dict(argv=[str(HGS), "task", *args], stdin=text or "", env=env)))
        request.rename(self.root / "commands" / (job + ".json"))
        result = self.root / "results" / (job + ".json")
        for _ in range(1500):
            if result.exists():
                done = json.loads(result.read_text())
                self.assertEqual(done["code"] == 0, ok, done["stderr"] or done["stdout"])
                return done["stdout"] if ok else done["stderr"]
            time.sleep(.02)
        self.fail("the scripted agent never ran " + " ".join(args))
```

(e) The tests that act as `self.name` now run through the agent: `test_send_report_and_a_second_send`'s two reports, `test_session_rules` and `test_project_settings_bound_agents`'s sends, and `test_cancel…`'s own-task cancel. Change each `self.hgs(ARGS…, session=self.name …)` to `json.loads(self.as_agent(ARGS…, "--json", text=…))`. For refusals, use `self.as_agent(ARGS…, ok=False)`.

Pass `"--json"` now. Task 2 makes agents answer in one line, and until then the flag is just another word, which `hgs task` ignores (`option` and `flag` look names up).

(f) Add the identity tests:

```python
class AgentIdentity(Fixture):
    def test_an_unproven_session_is_refused(self):
        env = dict(self.env, HGS_SESSION="codex/api", HGS_RUN_ID="run-that-ended")
        result = subprocess.run([str(HGS), "task", "add", "--project", "orchard"], input="Fix it", env=env,
                                text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("names the agent session codex/api", result.stderr)
        self.assertEqual(self.ids("todo"), [])
        # A record without the owned process proves nothing either.
        agent_record(self.state, self.env, "codex/api", "run-stale", process_start="not this process")
        env["HGS_RUN_ID"] = "run-stale"
        result = subprocess.run([str(HGS), "task", "list", "--project", "orchard"], env=env,
                                text=True, capture_output=True, timeout=30)
        self.assertIn("names the agent session codex/api", result.stderr)

    def test_a_renamed_session_acts_under_its_current_name(self):
        run = self.agent("codex/api")
        old = self.state / (hashlib.sha256(b"codex/api").hexdigest() + ".json")
        old.unlink()
        agent_record(self.state, self.env, "codex/api-renamed", run)
        task = self.add("Document the setup", session="codex/api")
        self.assertEqual((task["target"]["session"], task["author"]["session"]), ("codex/api-renamed", "codex/api-renamed"))

    def test_an_agent_works_on_its_sessions_project(self):
        self.agent("codex/api")
        self.assertIn("codex/api belongs to no project", self.hgs("list", session="codex/api", ok=False))
        join(self.env, "project-2", "codex/api")
        filed = self.hgs("add", text="Theme the header", session="codex/api")["task"]
        self.assertTrue(filed["ref"].startswith("team/web/"), filed["ref"])
        self.assertEqual(self.hgs("list", session="codex/api")["project"]["name"], "team/web")
        self.assertEqual(self.hgs("get", filed["id"], session="codex/api")["task"]["uuid"], filed["uuid"])
        # A reference or --project still names another project.
        other = self.add("Add rate limiting")
        self.assertEqual(self.hgs("get", other["ref"], session="codex/api")["task"]["uuid"], other["uuid"])
        self.assertEqual(self.hgs("list", "--project", "orchard", session="codex/api")["project"]["name"], "orchard")
        # The person still names the project.
        self.assertIn("usage", self.hgs("list", ok=False))
```

In `tests/test_task_routing.py`, replace `env`:

```python
    def env(self, node, session=None):
        env = dict(self.base, HOME=str(self.root / node / "profile"), HGS_CONFIG_DIR=str(self.root / node / "config"),
                   HGS_STATE_DIR=str(self.root / node / "state"), TASK_FIXTURE=str(self.root / node))
        if session:
            # An agent of this computer: a run this test process owns.
            run = "run-" + session.replace("/", "-")
            test_tasks.agent_record(self.root / node / "state", env, session, run)
            env.update(HGS_SESSION=session, HGS_RUN_ID=run)
        return env
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks -v`.

Expected:
- `test_an_unproven_session_is_refused`, `test_a_renamed_session_acts_under_its_current_name` and `test_an_agent_works_on_its_sessions_project` FAIL. The unproven agent is accepted, the old name is used, and there is no own project.
- Every other test passes, which shows the new fixture proves its agents the way the old one claimed them.

- [ ] **Step 3: Prove the agent and find its project**

In `src/swarm/mod.rs`, after `projects`:

```rust
/// The live catalog project (ID, name) that this computer's session `name`
/// belongs to, if any.
pub(crate) fn session_project(config: &Config, name: &str) -> Result<Option<(String, String)>> {
    let snapshot = LockedStore::load(config)?.snapshot(config);
    // The snapshot names this computer's sessions "<this host>\n<session>".
    let member = format!("{}\n{name}", config.host());
    Ok(snapshot["organization"]["projects"]
        .as_array()
        .into_iter()
        .flatten()
        .find(|p| p["sessions"].as_array().is_some_and(|all| all.iter().any(|s| s.as_str() == Some(member.as_str()))))
        .and_then(|p| Some((p["id"].as_str()?.to_owned(), p["name"].as_str().unwrap_or_default().to_owned()))))
}
```

In `src/tasks.rs`:

(a) Replace the doc comment and the tail of `actor`. The `--agent` branch stays as it is:

```rust
/// The agent of the session that runs this command, or the person. A command
/// whose environment names a session (`HGS_SESSION`, `HGS_RUN_ID`) acts as its
/// agent only when it proves it as `hgs self` does: the run is live on this
/// computer and the command descends from its agent; the proof follows a rename
/// to the session's current name. A send or cancel another computer forwards
/// (`forwardable`) names its agent with `--agent NODE/SESSION`.
fn actor(here: &str, args: &[String], forwardable: bool) -> Result<Actor> {
    let var = |name| std::env::var(name).ok().filter(|v| !v.is_empty());
    if let Some(agent) = option(args, "--agent")? {
        // … unchanged …
    }
    let (Some(claimed), Some(_)) = (var("HGS_SESSION"), var("HGS_RUN_ID")) else {
        return Ok(Actor::Person);
    };
    let record = crate::state::session_graph::caller_record().ok_or_else(|| {
        fail(format!(
            "this command names the agent session {claimed}, but that run is not live on this computer or does not run it; run hgs task from the session's own shell"
        ))
    })?;
    Ok(Actor::Agent(SessionKey {
        machine: here.to_owned(),
        session: record["name"].as_str().unwrap_or(&claimed).to_owned(),
    }))
}
```

(b) After `project`, add:

```rust
/// The project a command works on when it names none: the calling agent's
/// session's catalog project. The person is told `missing`.
fn own_project(config: &Config, who: &Actor, missing: &str) -> Result<(String, String)> {
    let Actor::Agent(own) = who else {
        return Err(fail(missing));
    };
    crate::swarm::session_project(config, &own.session)
        .map_err(fail)?
        .ok_or_else(|| fail(format!("{} belongs to no project; name one: PROJECT/T-xxxxxx or --project P", own.session)))
}

/// `--project P`, or the calling agent's own project.
fn project_arg(config: &Config, args: &[String], who: &Actor) -> Result<(String, String)> {
    match option(args, "--project")? {
        Some(wanted) => project(config, wanted),
        None => own_project(config, who, USAGE),
    }
}
```

(c) Give `reference` the caller. Its tail becomes:

```rust
fn reference(config: &Config, args: &[String], who: &Actor) -> Result<Reference> {
    let value = args.first().ok_or_else(|| fail(USAGE))?;
    let (project_name, task) = match value.rsplit_once('/') {
        Some((project, task)) => (Some(project), task),
        None => (option(args, "--project")?, value.as_str()),
    };
    let project = match project_name {
        Some(name) => project(config, name)?,
        None => own_project(config, who, "name the project: PROJECT/T-xxxxxx or --project P")?,
    };
    Ok(Reference { project, task: task.to_owned() })
}
```

`project` here shadows the function `project` inside the `match`. If the compiler objects, name the binding `found`.

(d) Decide the caller before the project at every call site. Stdin is still read first, and catalog lookups still come before the store is opened:
- `get` in `dispatch`: `let machines = machines(config)?; let who = actor(&machines.here, args, false)?; let r = reference(config, args, &who)?;`, then as before, with `&machines`.
- `list`: begin with `let machines = machines(config)?; let who = actor(&machines.here, args, false)?; let (id, name) = project_arg(config, args, &who)?;`, then drop its later `let machines`.
- `add`: after `let text = stdin_text("prompt")?;`, move `let machines …` and `let author …` up, then `let (id, name) = project_arg(config, args, &author)?;`.
- `change`: after the text, `let machines = machines(config)?; let here = machines.here.as_str(); let who = actor(here, args, matches!(action, "send" | "cancel"))?; let r = reference(config, args, &who)?;`. Keep the rest.
- `settings`: after reading the changes, `let who = actor(&machines(config)?.here, args, false)?; let (id, name) = project_arg(config, args, &who)?;`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks tests.test_task_routing -v`.

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/tasks.rs src/swarm/mod.rs tests/test_tasks.py tests/test_task_routing.py
git commit -m "Prove the agent of a task command as hgs self does and default to its session's project"
```

---

### Task 2: One-line answers for agents

**Files:**
- Create: `src/tasks/answers.rs`.
- Modify: `src/tasks.rs`. Commands return `Value`; add `with_answer` and `place`; `move --position N`; send refusals; the empty-text hint.
- Modify: `src/cli.rs`. The usage line for `task`.
- Test: `tests/test_tasks.py` and `tests/test_task_routing.py`.

**Interfaces:**
- Consumes (Task 1): `actor`, `Fixture.hgs(session=…)` and `TaskDelivery.as_agent`.
- Produces:
  - The JSON of a change an agent makes carries `"answer": "<line>"`.
  - `dispatch` prints that line unless `--json` is given.
  - `hgs task move REF --position N`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_tasks.py`. A helper on `Fixture`:

```python
    def says(self, *args, text=None, session="codex/api"):
        """The line an agent reads after a change."""
        env = dict(self.env, HGS_SESSION=session, HGS_RUN_ID=self.agent(session))
        result = subprocess.run([str(HGS), "task", *args], input=text, env=env, text=True,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
        return result.stdout.strip()
```

The tests:

```python
class AgentAnswers(TaskDelivery):
    def setUp(self):
        super().setUp()
        self.env["TZ"] = "UTC"
        self.agent("codex/api")
        join(self.env, "project-1", "codex/api")

    def test_changes_answer_in_one_line(self):
        self.add("Earlier work")
        filed = self.says("add", text="Add rate limiting")
        self.assertRegex(filed, r"^Filed orchard/T-\w+ in To do, position 2, for codex/api\.$")
        ref = filed.split()[1]
        self.assertRegex(self.says("add", "--backlog", "--position", "1", text="Measure first"),
                         r"^Filed orchard/T-\w+ in Backlog, position 1, for codex/api\.$")
        self.assertEqual(self.says("move", ref, "--position", "1"), ref + " is position 1 in To do.")
        self.assertEqual(self.says("move", ref, "--position", "9"), ref + " is position 2 in To do.")
        self.assertEqual(self.says("edit", ref, text="Add rate limiting, edited"), ref + " has the new prompt.")
        self.assertEqual(self.says("target", ref, self.name), ref + " now goes to codex/task-test.")
        self.assertEqual(self.says("target", ref, "any"), ref + " now goes to any session.")
        self.assertEqual(self.says("comment", ref, text="Measured: 40 rps."), "Added your comment to " + ref + ".")
        self.assertEqual(self.says("stage", ref, "backlog"), ref + " is in Backlog.")
        # --json prints the task, with the answer beside it.
        out = self.hgs("stage", ref, "todo", "--json", session="codex/api")
        self.assertEqual((out["task"]["stage"], out["answer"]), ("todo", ref + " is in To do."))
        # The person's commands print JSON as before, without an answer.
        self.assertNotIn("answer", self.hgs("stage", ref, "backlog"))

    def test_send_report_and_cancel_answer_in_one_line(self):
        task = self.add("Add rate limiting", "--target", self.name)
        self.assertEqual(self.says("send", task["ref"]), task["ref"] + " sent to codex/task-test.")
        self.wait_received("Add rate limiting")
        self.assertEqual(self.as_agent("report", task["ref"], text="Token bucket; 6 tests.").strip(), task["ref"] + " is In review.")
        self.assertEqual(self.says("report", task["ref"], text="Also the README."),
                         task["ref"] + " is In review; your report was added to its history.")
        again = self.add("Measure the limits", "--target", self.name)
        self.hgs("send", again["ref"])
        self.assertEqual(self.says("cancel", again["ref"], "--reason", "Wrong branch"),
                         again["ref"] + " is In review; codex/task-test got the cancel message.")
        cancelled = self.hgs("get", again["ref"])["task"]
        at = datetime.datetime.fromtimestamp(cancelled["cancelled_at"] / 1000, datetime.timezone.utc).strftime("%H:%M")
        self.assertEqual(self.as_agent("report", again["ref"], text="Stopped halfway.").strip(),
                         again["ref"] + " is In review (cancelled at " + at + "); your report was added to its history.")

    def test_a_refused_send_names_its_session(self):
        task = self.add("Add rate limiting", "--target", self.name)
        self.says("send", task["ref"])
        refusal = self.hgs("send", task["ref"], session="codex/api", ok=False)
        self.assertIn("codex/task-test cannot take it now: ", refusal)
        self.assertIn("already In progress in codex/task-test. Nothing was sent.", refusal)

    def test_an_empty_text_says_where_it_goes(self):
        task = self.add("Add rate limiting")
        self.assertIn("the report is empty; give it on stdin, for example: hgs task report REF <<'EOF'",
                      self.hgs("report", task["ref"], text="", session="codex/api", ok=False))
```

Add `import datetime` to the imports. In the tests from Task 1 and earlier where an agent's change command parses JSON, pass `"--json"`: `test_an_agent_files_tasks_for_its_own_session`'s `self.add(…, session=…)` and `test_session_rules`' `self.hgs("send", …, session="codex/planner")`. Do this by giving `Fixture.add` an extra `"--json"` when `session` is set.

In `tests/test_task_routing.py`, add:

```python
    def test_an_agents_send_answers_in_one_line(self):
        task = self.add("Add rate limiting", "--target", "codex/remote", "--host", "b")
        result = subprocess.run([str(HGS), "task", "send", task["id"], "--project", "project-1"],
                                env=self.env("a", "codex/local"), text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "orchard/" + task["id"] + " sent to codex/remote.")
```

In its `test_an_agent_sends_as_itself`, add `"--json"` to the agent's send.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks.AgentAnswers tests.test_task_routing -v`.

Expected: the `AgentAnswers` tests and `test_an_agents_send_answers_in_one_line` FAIL, because stdout is JSON, not one line. The other routing tests pass.

- [ ] **Step 3: The answers**

Create `src/tasks/answers.rs`:

```rust
//! The line an agent reads after a change: what happened and what to do next
//! (docs/superpowers/specs/2026-10-13-project-task-queue-design.md, "Answers").
use serde_json::Value;
use zerus_replica::Stage;

fn name(task: &Value) -> &str {
    task["ref"].as_str().unwrap_or_default()
}

fn label(task: &Value) -> &'static str {
    task["stage"].as_str().and_then(|s| Stage::parse(s).ok()).map_or("its stage", Stage::label)
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
    Local.timestamp_millis_opt(ms).single().map_or_else(String::new, |t| format!("{:02}:{:02}", t.hour(), t.minute()))
}

/// The answer to `action` whose JSON is `out`. `place` is the task's place in
/// its list (from 1) after an add or move, `caller` the agent's session and
/// `destination` the session a send went to.
pub(super) fn answer(action: &str, out: &Value, place: usize, caller: &str, destination: &str) -> String {
    let task = &out["task"];
    let it = name(task);
    match action {
        "add" => format!("Filed {it} in {}, position {place}, for {caller}.", label(task)),
        "move" => format!("{it} is position {place} in {}.", label(task)),
        "edit" => format!("{it} has the new prompt."),
        "stage" => format!("{it} is in {}.", label(task)),
        "target" if task["target"]["session"].is_string() => format!("{it} now goes to {}.", session(&task["target"])),
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
        "send" => {
            // A send to another computer whose copy could not come back yet.
            let behind = if out["synced"] == false { " This computer's copy follows with the next sync." } else { "" };
            format!("{it} sent to {destination}.{behind}")
        }
        "cancel" if out["cancel_message"]["sent"] == true => {
            format!("{it} is In review; {} got the cancel message.", session(&task["session"]))
        }
        "cancel" => format!(
            "{it} is In review; the cancel message was not sent: {}.",
            out["cancel_message"]["error"].as_str().unwrap_or("unknown reason").trim_end_matches('.')
        ),
        _ => String::new(),
    }
}

/// A send that was refused, as an agent reads it.
pub(super) fn refused(destination: &str, reason: &str) -> String {
    format!("{destination} cannot take it now: {}. Nothing was sent.", reason.trim_end_matches('.'))
}
```

In `src/tasks.rs`, add `mod answers;` below the imports.

- [ ] **Step 4: Commands return their JSON; changes carry the answer**

In `src/tasks.rs`:

(a) Rename `dispatch`'s match into `fn execute(config: &Config, args: &[String]) -> Result<Value>`, with every arm returning a value, and make `dispatch`:

```rust
pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    if dry {
        return Err(fail("task commands do not support --dry-run"));
    }
    let out = execute(config, args)?;
    // An agent reads one line saying what happened; --json prints the task.
    match out.get("answer").and_then(Value::as_str) {
        Some(line) if !flag(args, "--json") => println!("{line}"),
        _ => println!("{out}"),
    }
    Ok(0)
}
```

Next, make every command return its JSON:
- `list`, `add`, `change`, `forwarded`, `cancel_here` and `settings` return `Result<Value>`.
- Each `print(x)` becomes `Ok(x)`, and `get`'s `print(json!(…))` becomes `Ok(json!(…))`.
- Remove `print`.

(b) Add the helpers after `check_prompt`:

```rust
/// The place of `task` in its list, counting from 1.
fn place(store: &ProjectStore, task: &TaskDoc) -> Result<usize> {
    let stage = task.stage().map_err(fail)?;
    let (tasks, _) = store.listing().map_err(fail)?;
    Ok(tasks
        .iter()
        .filter(|t| t.stage().ok() == Some(stage))
        .position(|t| t.uuid() == task.uuid())
        .map_or(0, |i| i + 1))
}

/// `out` with the line an agent reads (`answer`), for a change an agent made.
fn with_answer(who: &Actor, action: &str, mut out: Value, place: usize, destination: &str) -> Value {
    if let Actor::Agent(own) = who {
        out["answer"] = json!(answers::answer(action, &out, place, &own.session, destination));
    }
    out
}
```

(c) `add` ends with:

```rust
    store.save(&mut task).map_err(fail)?;
    let at = place(&store, &task)?;
    Ok(with_answer(&author, "add", json!({"task": view(&task, &name, true, &machines)?}), at, ""))
```

(d) In `change`:

Right after `who`, add the refusal wording:

```rust
    // An agent reads a refused send as "SESSION cannot take it now: …"; a paste
    // that happened is never reworded.
    let refuse = |error: Error, to: &str| -> Error {
        if matches!(who, Actor::Agent(_)) && action == "send" && !error.message.starts_with("the prompt was sent") {
            fail(answers::refused(to, &error.message))
        } else {
            error
        }
    };
```

The remote branch becomes:

```rust
    if let Some((session, _)) = session.as_ref().filter(|(session, _)| session.machine != here) {
        let to = session.session.clone();
        if action == "send" {
            task.check_send().map_err(|e| refuse(fail(e), &to))?;
        }
        let uuid = task.uuid().to_owned();
        drop(store);
        let out = match reach(config, &machines, project_id, session) {
            Ok(alias) => forwarded(config, &machines, &r.project, &uuid, action, args, session, &alias, &who),
            Err(reason) if action == "cancel" => cancel_here(config, &machines, &r.project, &uuid, args, &who, reason),
            Err(reason) => Err(fail(reason)),
        }
        .map_err(|e| refuse(e, &to))?;
        return Ok(with_answer(&who, action, out, 0, &to));
    }
```

In the local arms:
- `"send"` becomes `let (session, pin) = session.expect("a send has its session"); destination = session.session.clone(); extra = send(&store, &mut task, project_name, session, pin, &who).map_err(|e| refuse(e, &destination))?;`. Declare `let mut destination = String::new();` before the `match action`.
- `"move"` gets a branch before the final `else`: `} else if let Some(index) = position_index(args)? { Position::Index(index) } else {`.

The end of `change` becomes:

```rust
    store.save(&mut task).map_err(fail)?;
    let at = if action == "move" { place(&store, &task)? } else { 0 };
    let mut out = json!({"task": view(&task, project_name, true, &machines)?});
    for (key, value) in extra.as_object().expect("object") {
        out[key] = value.clone();
    }
    Ok(with_answer(&who, action, out, at, &destination))
```

(e) Above `add`, add `position_index`, and use it in `add` in place of its `--position` match:

```rust
/// `--position N`, counting from 1, as a list index.
fn position_index(args: &[String]) -> Result<Option<usize>> {
    option(args, "--position")?
        .map(|p| {
            p.parse::<usize>()
                .ok()
                .filter(|p| *p >= 1)
                .map(|p| p - 1)
                .ok_or_else(|| fail("--position is a number from 1"))
        })
        .transpose()
}
```

```rust
    let position = position_index(args)?.map_or(Position::End, Position::Index);
```

(f) Give `stdin_text` an example of where the text goes. Change its signature to `fn stdin_text(what: &str, example: &str) -> Result<String>` and end it with:

```rust
    let text = String::from_utf8(bytes).map_err(|_| fail(format!("the {what} is not UTF-8")))?;
    // Agents tend to pass texts as arguments; say where they go.
    if text.trim().is_empty() && !example.is_empty() {
        return Err(fail(format!("the {what} is empty; give it on stdin, for example: hgs task {example} <<'EOF'")));
    }
    Ok(text)
```

The calls:
- `stdin_text("prompt", "add")` in `add`.
- `"edit" => Some(stdin_text("prompt", "edit REF")?)`, `"comment" => Some(stdin_text("comment", "comment REF")?)` and `"report" => Some(stdin_text("report", "report REF")?)`.
- `stdin_text("settings", "")` in `settings`.

(g) In `USAGE`, replace `move REF --top|--before REF|--after REF` with `move REF --top|--before REF|--after REF|--position N`, and add ` [--json]` after `report REF | comment REF`. In `src/cli.rs`, change the usage line `hgs task list|get|add|… project tasks (JSON)` to end with `project tasks (JSON; agents read one line)`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks tests.test_task_routing tests.test_swarm -v`.

Expected: all pass.

Then run the desktop suites that read `hgs task` JSON: `ctest --test-dir .ci-build/gui -R "tasksview|hgsclient|taskdrag" --output-on-failure`. Expected: pass; nothing they read changed.

- [ ] **Step 6: Commit**

```bash
git add src/tasks.rs src/tasks/answers.rs src/cli.rs tests/test_tasks.py tests/test_task_routing.py
git commit -m "Answer an agent's task change in one line, and name the session of a refused send"
```

---

### Task 3: The task instructions at session start, and `hgs guide tasks`

**Files:**
- Modify: `src/tasks.rs`. Add `AGENT_INSTRUCTIONS`, `session_context` and a unit test.
- Modify: `src/state/hooks.rs`. Split `hook` into a wrapper and `track`.
- Create: `src/agent_guide/tasks.md`.
- Modify: `src/agent_guide.rs` and `src/agent_guide/index.md`.
- Modify: `src/cli.rs`. The usage line for guide.
- Create: `tests/test_task_context.py`.

**Interfaces:**
- Produces: `pub(crate) fn tasks::session_context(agent: &str) -> Option<serde_json::Value>`, the SessionStart hook output.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_task_context.py`:

```python
#!/usr/bin/env python3
"""Zerus's SessionStart hook gives Claude Code and Codex sessions the project
task instructions: fake agents through the real lifecycle."""
import json
import unittest

import test_fork


class TaskContext(test_fork.Base):
    def hook_output(self, name, agent, kind="SessionStart", **fields):
        record = self.binding(name)
        event = dict(session_id=record["conversation_id"], hook_event_name=kind, **fields)
        env = dict(self.env, HGS_SESSION=name, HGS_RUN_ID=record["run_id"], HGS_AGENT=agent)
        return self.hook(event, env).stdout

    def test_claude_and_codex_sessions_read_the_task_commands_at_start(self):
        for agent in ("claude", "codex"):
            with self.subTest(agent=agent):
                name = self.start(agent, "context")
                output = json.loads(self.hook_output(name, agent, source="startup"))["hookSpecificOutput"]
                self.assertEqual(output["hookEventName"], "SessionStart")
                self.assertIn("hgs task add", output["additionalContext"])
                self.assertIn("поставь таск", output["additionalContext"])
                self.assertIn("hgs guide tasks", output["additionalContext"])
                # Tracking goes on: the session is still idle in its conversation.
                self.assertEqual(self.binding(name)["activity"], "idle")

    def test_subagents_other_agents_and_other_events_read_nothing(self):
        name = self.start("claude", "quiet")
        self.assertEqual(self.hook_output(name, "claude", agent_id="agent-1"), "")
        self.assertEqual(self.hook_output(name, "claude", kind="UserPromptSubmit", prompt="hello"), "")
        kimi = self.start("kimi", "plain")
        self.assertEqual(self.hook_output(kimi, "kimi"), "")


if __name__ == "__main__":
    unittest.main()
```

Add to `src/tasks.rs`, at its end:

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claude_and_codex_read_the_instructions_others_do_not() {
        for agent in ["claude", "codex"] {
            let context = session_context(agent).unwrap();
            assert_eq!(context["hookSpecificOutput"]["hookEventName"], "SessionStart");
            let text = context["hookSpecificOutput"]["additionalContext"].as_str().unwrap();
            for phrase in ["Zerus tasks are this project's shared task list.", "\"поставь таск\"", "hgs task report <ref>", "hgs guide tasks"] {
                assert!(text.contains(phrase), "{phrase}");
            }
        }
        assert!(session_context("kimi").is_none());
    }
}
```

The guide's own tests (`src/agent_guide.rs`) cover the new page once it is listed: size, ASCII, line length, the index entry, and the commands named in `cli::USAGE`.

- [ ] **Step 2: Run the tests to verify they fail**

Run:
- `cargo +1.99.1 test --locked --target-dir .ci-build/cargo tasks::tests`. Expected: FAIL to compile, since `session_context` is not defined.
- `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest discover -s tests -p test_task_context.py -v`. Expected: `test_claude_and_codex…` FAILs, because `json.loads("")` raises. `test_subagents…` passes; it pins today's silence.

- [ ] **Step 3: The instructions and the hook output**

In `src/tasks.rs`, after `USAGE`:

```rust
/// What Claude Code and Codex sessions read about the project's tasks each time
/// their SessionStart hooks run (spec "Instructions at session start").
const AGENT_INSTRUCTIONS: &str = concat!(
    "Zerus tasks are this project's shared task list. Run `hgs task` in your shell: it acts as your session, on your session's project; `hgs guide tasks` lists the commands. Texts go on stdin, for example hgs task report <ref> <<'EOF' ... EOF.\n\n",
    "Filing. When the person says \"поставь таск\", \"добавь задачу\", \"запиши задачу\", \"в очередь\", \"на потом\", \"file a task\", \"add to the queue\" or \"for later\", file it with `hgs task add`. Never put these into TodoWrite or your own plan. New tasks go to the end of To do unless the person names a place (--backlog, --position N).\n\n",
    "Working. A message starting \"[Zerus task <ref>]\" is a task for you; do it like any request from the person. When the prompt says the task was sent before, read its history with `hgs task get <ref>` before you start. If you need the person, ask in this conversation as usual. When the work is done, or you find that it cannot be done, report with `hgs task report <ref>`: what you changed, how you checked it, and what is left undone or why it cannot be done.\n\n",
    "Cancelled. \"[Zerus task <ref> cancelled]\" means stop that task now and follow the message.\n\n",
    "Managing. `hgs task list` and `hgs task get` show tasks and their history; move, stage, target, edit, send and cancel change them as the person can. Accept a task (`hgs task stage <ref> done`) only when the person asks you to.\n\n",
    "Comments. `hgs task comment <ref>` adds a note for people and agents; it is never pasted into a session."
);

/// The SessionStart hook output that gives a session of `agent` the task
/// instructions; other agents get them once their hooks are reviewed.
pub(crate) fn session_context(agent: &str) -> Option<Value> {
    matches!(agent, "claude" | "codex").then(|| {
        json!({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": AGENT_INSTRUCTIONS}})
    })
}
```

In `src/state/hooks.rs`, rename the current `pub(super) fn hook() -> Result<i32>` to `fn track(name: String, token: String, mut event: Value) -> Result<i32>`. Remove its first lines, which read `HGS_SESSION` and `HGS_RUN_ID` and call `let mut event = stdin_json()?;`, and keep the rest. Then add before it:

```rust
pub(super) fn hook() -> Result<i32> {
    let (Some(name), Some(token)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID"))
    else {
        return Ok(0);
    };
    let event = stdin_json()?;
    let main_start = string(&event, "hook_event_name") == "SessionStart" && string(&event, "agent_id").is_empty();
    let code = track(name, token, event)?;
    // The project's task commands, in the agent's context from its first turn on
    // (docs/superpowers/specs/2026-10-13-project-task-queue-design.md).
    if code == 0 && main_start {
        if let Some(context) = nonempty_env("HGS_AGENT").and_then(|agent| crate::tasks::session_context(&agent)) {
            println!("{context}");
        }
    }
    Ok(code)
}
```

Inside `track`, `name` and `token` are now owned `String`s, as before. `read_run(&name, &token)` stays.

- [ ] **Step 4: `hgs guide tasks`**

Create `src/agent_guide/tasks.md`. It must stay ASCII, at most 2400 bytes, with lines of at most 100 characters:

```
Project tasks (hgs guide tasks)

The project's shared task list. Run these in your own session: they act as it
and on its project unless a reference names another (PROJECT/T-xxxxxx). Texts
go on stdin; a change prints one line saying what happened (--json: the task).

  hgs task list [--stage review|active|todo|backlog|done]   the tasks, JSON
  hgs task get REF                          one task with its history, JSON
  hgs task add [--backlog] [--position N] <<'EOF'   file a task for yourself
  hgs task report REF <<'EOF'               report a task you were sent
  hgs task comment REF <<'EOF'              a note, never pasted into a session
  hgs task edit REF <<'EOF'                 a new prompt (To do, Backlog)
  hgs task move REF --position N            priority in To do or Backlog
  hgs task stage REF todo|backlog|done      done only when the person asks
  hgs task target REF SESSION|any [--host ALIAS]   where it will be sent
  hgs task send REF [--session SESSION [--host ALIAS]]   Send now
  hgs task cancel REF [--reason TEXT]       stop another session's task

Rules
- A report says what changed, how it was checked and what is left undone or
  why it cannot be done; it moves the task to In review.
- You cannot cancel a task you were sent; report what is left undone instead.
- When the person asks to keep something for later, file it with add, not in
  your own plan.
```

In `src/agent_guide.rs`:
- Add `("tasks", include_str!("agent_guide/tasks.md")),` to `TOPICS`.
- Make `USAGE` `"usage: hgs guide [send|wait|launch|stop|tasks]"`.

In `src/agent_guide/index.md`, add after the `hgs guide stop` line:

```
  hgs guide tasks             the project's task list: file, report, send
```

In `src/cli.rs`, change the guide usage text `how agents message, wait for, launch and stop sessions` to `how agents message, wait for, launch and stop sessions and use tasks`.

- [ ] **Step 5: Run the tests to verify they pass**

Run:
- `cargo +1.99.1 test --locked --target-dir .ci-build/cargo -- tasks::tests agent_guide`
- `cargo +1.99.1 build --locked --target-dir .ci-build/cargo && env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest discover -s tests -p test_task_context.py -v`
- The hook's own suites: `… discover -s tests -p test_pause.py -v`, `-p test_questions.py` and `-p test_agent_guide.py`

Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/tasks.rs src/state/hooks.rs src/agent_guide.rs src/agent_guide/index.md src/agent_guide/tasks.md src/cli.rs tests/test_task_context.py
git commit -m "Give Claude Code and Codex sessions the task commands at session start, and add hgs guide tasks"
```

---

### Task 4: The instructions with the real agents, and the docs

**Files:**
- Create: `tests/test_task_context_native.py`.
- Modify: `scripts/ci/agent-compatibility.py`. Add the suites to the Codex list (about line 47) and to the Claude list (about line 58).
- Modify: `docs/reference.md`. The agent paragraph in "Project tasks", replacing "The MCP tools come in a later release."

**Interfaces:**
- Consumes (Task 3): the hook output of `hgs __state hook`.

- [ ] **Step 1: Write the native tests**

Create `tests/test_task_context_native.py`:

```python
#!/usr/bin/env python3
"""Keyless contract of the real Claude Code and Codex with the task instructions
that Zerus's SessionStart hook prints: the agent hands them to the model. Each
agent runs the real `hgs __state hook` and talks to a localhost model fixture.

Run with HGS_CLAUDE_TEST_BIN and HGS_CODEX_TEST_BIN (otherwise skipped)."""
import http.server
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
PHRASE = "Run `hgs task` in your shell"


class Native(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-task-context-native-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        (self.root / "state").mkdir()
        self.requests = []

    def serve(self, handler):
        handler.requests = self.requests
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return server.server_port

    def session(self, agent):
        return dict(HGS_SESSION=agent + "/orchard/api", HGS_RUN_ID="run-native", HGS_AGENT=agent,
                    HGS_STATE_DIR=str(self.root / "state"), HGS_CONFIG_DIR=str(self.root / "config"))

    def hooks(self):
        return {"SessionStart": [{"hooks": [{"type": "command", "command": shlex.join([str(HGS), "__state", "hook"])}]}]}


class Claude(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if not self.path.startswith("/v1/messages") or "count_tokens" in self.path:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"input_tokens":1}')
            return
        self.requests.append(body)
        message = dict(id="msg_1", type="message", role="assistant", model=body.get("model"), content=[],
                       stop_reason=None, stop_sequence=None, usage=dict(input_tokens=1, output_tokens=1))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for kind, data in [("message_start", dict(message=message)),
                           ("content_block_start", dict(index=0, content_block=dict(type="text", text=""))),
                           ("content_block_delta", dict(index=0, delta=dict(type="text_delta", text="Fixture complete."))),
                           ("content_block_stop", dict(index=0)),
                           ("message_delta", dict(delta=dict(stop_reason="end_turn", stop_sequence=None), usage=dict(output_tokens=1))),
                           ("message_stop", {})]:
            self.wfile.write(("event: " + kind + "\ndata: " + json.dumps(dict(type=kind, **data)) + "\n\n").encode())
            self.wfile.flush()


class Codex(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.requests.append(json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}"))
        item = dict(type="message", id="msg_1", role="assistant", status="completed",
                    content=[dict(type="output_text", text="Fixture complete.", annotations=[])])
        response = dict(id="resp_1", object="response", status="completed", output=[item],
                        usage=dict(input_tokens=1, output_tokens=1, total_tokens=2))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for kind, value in [("response.created", dict(response=dict(response, status="in_progress", output=[]))),
                            ("response.output_item.added", dict(output_index=0, item=item)),
                            ("response.output_item.done", dict(output_index=0, item=item)),
                            ("response.completed", dict(response=response))]:
            self.wfile.write(("event: " + kind + "\ndata: " + json.dumps(dict(type=kind, **value)) + "\n\n").encode())
            self.wfile.flush()


@unittest.skipUnless(os.environ.get("HGS_CLAUDE_TEST_BIN"), "Set HGS_CLAUDE_TEST_BIN for the isolated native Claude task context test")
class NativeClaudeTaskContext(Native):
    def test_claude_hands_the_task_instructions_to_the_model(self):
        port = self.serve(Claude)
        home = self.root / "home"
        (home / ".claude").mkdir(parents=True)
        (home / ".claude.json").write_text(json.dumps(dict(hasCompletedOnboarding=True,
                                                           customApiKeyResponses=dict(approved=["fixture-key"], rejected=[]))))
        (home / ".claude/settings.json").write_text(json.dumps({"hooks": self.hooks()}))
        env = dict(HOME=str(home), PATH=os.environ["PATH"], CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1",
                   ANTHROPIC_API_KEY="fixture-key", ANTHROPIC_BASE_URL="http://127.0.0.1:" + str(port), **self.session("claude"))
        run = subprocess.run([os.environ["HGS_CLAUDE_TEST_BIN"], "-p", "Hello.", "--model", "claude-sonnet-4-6"],
                             env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(self.requests, "Claude sent no request")
        self.assertIn(PHRASE, json.dumps(self.requests[-1]["messages"]))


@unittest.skipUnless(os.environ.get("HGS_CODEX_TEST_BIN"), "Set HGS_CODEX_TEST_BIN for the isolated native Codex task context test")
class NativeCodexTaskContext(Native):
    def test_codex_hands_the_task_instructions_to_the_model(self):
        port = self.serve(Codex)
        home = self.root / "codex"
        home.mkdir()
        work = self.root / "work"
        work.mkdir()
        (home / "hooks.json").write_text(json.dumps({"hooks": self.hooks()}))
        (home / "config.toml").write_text(
            'model="gpt-6.1-sol"\nmodel_provider="fixture"\ncheck_for_update_on_startup=false\n'
            '[model_providers.fixture]\nname="Fixture"\nbase_url="http://127.0.0.1:' + str(port) + '/v1"\n'
            'wire_api="responses"\nrequest_max_retries=0\nstream_max_retries=0\n[features]\nhooks=true\n')
        env = dict(HOME=str(self.root), PATH=os.environ["PATH"], CODEX_HOME=str(home), OPENAI_API_KEY="fixture",
                   **self.session("codex"))
        run = subprocess.run([os.environ["HGS_CODEX_TEST_BIN"], "exec", "--ephemeral", "--skip-git-repo-check",
                              "-s", "read-only", "--dangerously-bypass-hook-trust", "-C", str(work), "--color", "never", "Hello."],
                             env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(self.requests, "Codex sent no request")
        developer = [item for item in self.requests[0].get("input", []) if item.get("role") == "developer"]
        self.assertIn(PHRASE, json.dumps(developer))


if __name__ == "__main__":
    unittest.main()
```

The state directory is empty, so `track` finds no run and returns 0 without recording anything. The hook still prints the instructions, which is the contract this test pins: the agents read the output and hand it on.

- [ ] **Step 2: Run them against the agents, and once against a mutation**

Run: `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs HGS_CLAUDE_TEST_BIN=$(readlink -f ~/.local/bin/claude) HGS_CODEX_TEST_BIN=$(readlink -f ~/.local/bin/codex) python3 -m unittest discover -s tests -p test_task_context_native.py -v`.

Expected: both pass.

Then check that the tests can fail:
1. Temporarily change `"additionalContext"` in `session_context` to `"additional_context"` and rebuild. Expected: both tests FAIL on the phrase.
2. Restore it, rebuild, and run again until both pass.

- [ ] **Step 3: Register the suites in CI**

In `scripts/ci/agent-compatibility.py`:
- Append `'test_task_context_native.NativeCodexTaskContext'` to the Codex `suites` list, after `'test_codex_async_questions_native'`.
- Append `'test_task_context_native.NativeClaudeTaskContext'` to the Claude list, after `'test_claude_statusline_native'`.

Run `python3 -m unittest discover -s tests -p test_ci_security.py -v`. Expected: pass. If it pins the suite lists, add the entries there too.

- [ ] **Step 4: Document the commands for agents**

In `docs/reference.md`, replace the sentence "The MCP tools come in a later release." with:

```markdown
Agents use the same `hgs task` commands in their own shell. A command run inside
an agent session acts as that session when it proves it as `hgs self` does (the
run is live here and the command descends from the agent; renames are
followed) and is refused when its environment names a session it cannot prove.
It works on the session's catalog project unless a reference or `--project`
names another. A change an agent makes prints one line saying what happened,
for example `orchard/T-a1b2c3 sent to codex/api.`; `--json` prints the task, and
reads print JSON. Claude Code and Codex sessions read the instructions for the
task list from Zerus's SessionStart hook; `hgs guide tasks` lists the commands.
There is no MCP server; sessions without bypassed permissions are not covered
(Claude Code asks first, Codex's sandbox blocks the task store and SSH).
```

- [ ] **Step 5: Run the whole check**

Run:
- `cargo +1.99.1 clippy --locked --all-targets --target-dir .ci-build/cargo -- -D warnings`
- `cargo +1.99.1 test --locked --target-dir .ci-build/cargo`
- `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks tests.test_task_routing tests.test_swarm -v`
- `env HGS_TEST_BIN=$PWD/.ci-build/cargo/debug/hgs python3 -m unittest discover -s tests -p 'test_task_context.py' -v`, and the same with `-p test_pause.py`, `-p test_agent_guide.py` and `-p test_questions.py`
- `python3 scripts/ci/check-source.py`

Expected: everything passes, with no clippy warnings.

- [ ] **Step 6: Commit**

```bash
git add tests/test_task_context_native.py scripts/ci/agent-compatibility.py docs/reference.md
git commit -m "Pin the session-start task instructions with the real Claude Code and Codex, and document agent commands"
```
