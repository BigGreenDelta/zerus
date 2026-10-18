# Task tracker, phase 3a: the desktop Tasks tab — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Tasks tab beside Activity and Terminal that shows the open session's project tasks from the local replica and lets the person file, order, send, cancel, review and accept them, with the task, cancel and project-settings dialogs.

**Architecture:** The Qt GUI never reads the replica itself: every read and change is an `hgs task …` call on this computer through `HgsClient::requestTask` (stdin always closed). `TaskBoard` turns a listing into the tab's rules (stages, queue numbers, the open session's tasks, the Done window) without widgets; `TasksView` draws the tiles, `TaskRow` rows and the task input (a `MessageComposer` in task mode); `TaskDialogs` holds the three dialogs; `SessionsWindow` gives the tab its project, open session and sessions. hgs gains `stage_at` in task views, `--host ALIAS` targets and a `host` on each session reference.

**Tech Stack:** Qt 6 Widgets, C++17, QtTest offscreen with a fake `hgs` (Python); Rust 1.99.1 for the hgs and crate changes.

**Spec:** `docs/superpowers/specs/2026-10-13-project-task-queue-design.md` (Desktop interface; Model; Sending; Cancel; In review and Done) and phase 3 of `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`. The approved prototype is `design/task-tracker/index.html`. Drag and drop (reordering To do, dropping on tiles and on sessions) is phase 3b, a separate plan.

## Global Constraints

- Work on branch `task-tracker`, which the shared main checkout has checked out; never switch branches, merge, rebase or push. Other sessions may work in the same checkout: stage only this plan's files.
- Never rebuild binaries a running Zerus uses. Build Rust with `--target-dir .ci-build/cargo` and run `tests/test_tasks.py` with `HGS_TEST_BIN=.ci-build/cargo/debug/hgs`; build the GUI in `.ci-build/gui`, never in `tray/build`.
- The GUI build: configure once with
  `cmake -S tray -B .ci-build/gui -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON $(grep -E '^(Qt6_DIR|KF6[A-Za-z]*_DIR)[:=]' tray/build/CMakeCache.txt 2>/dev/null | sed 's/^\([^:=]*\)[^=]*=/-D\1=/')`
  (it takes the Qt and KF6 paths of an existing local tray build, if any), then build one suite with `cmake --build .ci-build/gui --target <suite> -j 8` and run it with `QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/<suite>`.
- Rust: `cargo +1.99.1`; the replica crate stays rustfmt-formatted and clippy-clean with `-D warnings`, hgs clippy-clean with `-D warnings`.
- Tray code follows its surroundings: compact Qt C++17, objectNames for everything tests reach, colours per theme through `setTheme(bool dark)`, icons through `workspaceIcon`.
- The interface is English. Synthetic names only (`orchard`, `codex/api`, `orchard-mac`); never real project or session names.
- A session in no catalog project (`ungrouped`) has no tasks; the tab says so and never calls hgs.

## Review Focus

- An older hgs without `hgs task`: the tab's first call is `hgs task list --project …`, which such an hgs refuses; the tab shows "Tasks could not be read: …" and makes no other call — `aFailedListingIsShownInsteadOfTasks` (Task 4).
- A pinned session that no longer exists, does not run or runs on another computer: Send now is disabled with hgs's reason, on the row and in the dialog — `sendNowNamesWhyASessionCannotTakeIt` (Task 4).
- A refused add: the typed prompt stays in the input and the reason is shown — `theInputFilesATaskForTheOpenSession` (Task 4).
- An action from the task settings after editing the prompt or the session: the edits are applied first, in order, and the action meets the edited task — `theTabOpensTheDialogsAndAppliesThem` (Task 5).
- The hidden Tasks tab must leave no enabled field in the window, or Escape and focus change for the whole window — `multiSelectionKeepsConversationAndGroupMenu` still passes with the tab (Task 6).

## File Structure

| Path | Responsibility |
|---|---|
| `crates/replica/src/schema.rs`, `task.rs`, `tests/vectors.rs` | `TaskView.stage_at`; vectors accept fields a newer build adds |
| `src/tasks.rs`, `tests/test_tasks.py` | `--host ALIAS` targets and `host` on session references |
| `tray/src/TaskBoard.h/.cpp`, `tray/tests/test_taskboard.cpp` | A listing as the tab's rules, without widgets |
| `tray/src/HgsClient.h/.cpp`, `tray/tests/test_hgsclient.cpp` | `requestTask`: `hgs task …` on this computer |
| `tray/src/MessageComposer.h/.cpp`, `ComposerDraftStore.h/.cpp`, `tray/tests/test_messagecomposer.cpp` | The task input: task mode and task-prompt drafts |
| `tray/src/WorkspaceIcons.h` | The task icons |
| `tray/src/TaskRow.h/.cpp` | One row: meta, hover actions, clamped text, the report |
| `tray/src/TasksView.h/.cpp`, `tray/tests/support/FakeTasksCli.h`, `tray/tests/test_tasksview.cpp` | The tab: tiles, list, input, refresh, actions, the label badge |
| `tray/src/TaskDialogs.h/.cpp`, `tray/tests/test_taskdialogs.cpp` | Task settings, cancel confirmation, project task settings |
| `tray/src/SessionsWindow*.cpp/.h`, `tray/tests/test_sessionswindow.cpp` | The tab in the window; Plan & agents |
| `tray/CMakeLists.txt`, `tray/tests/CMakeLists.txt` | Sources and suites |
| `docs/reference.md` | `--host`, the Tasks tab, Plan & agents |

**Known limits of this phase:** sending and cancelling for a session on another computer are refused until phase 2 (replication); tasks are pinned to such sessions by their computer's verified alias. Every refresh lists the whole project (2.5 s while the tab shows, 15 s otherwise). The tab label counts the project's tasks, not the filtered ones. Existing, outside this plan: in an active window, Escape in the session list with several sessions selected is taken by the window's Escape shortcut (it may interrupt the open session) instead of clearing the selection; and `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7 of `test_sessionswindow` on some machines also without this work.

---

### Task 0: Record the plan

- [ ] **Step 1: Check the branch and privacy**

Run: `git branch --show-current && python3 scripts/check-source-privacy.py`
Expected: `task-tracker`; `0 privacy guard findings`.

- [ ] **Step 2: Commit the plan**

```bash
git add docs/superpowers/plans/2026-10-18-task-tracker-phase3a-desktop.md
git commit -m "Plan the desktop Tasks tab"
```

---

### Task 1: Stage times and other computers in `hgs task`

**Files:**
- Modify: `crates/replica/src/task.rs`, `tests/test_tasks.py`, `crates/replica/src/schema.rs`, `crates/replica/tests/vectors.rs`, `docs/reference.md`, `src/tasks.rs`

**Interfaces:**
- Produces: `TaskView.stage_at: i64` (Unix ms when the task entered its stage; documents without it report `created_at`), in every `hgs task` view. `hgs task add … --target S --host ALIAS` and `hgs task target REF S --host ALIAS` pin to a session on a computer this one verified (`swarm::verified_bindings`), refused otherwise with "ALIAS is not a verified computer of this swarm". Every `target` and `session` object with a `machine` gets `host`: `""` for this computer, the alias of a verified member, or `null`.

The golden vectors keep checking every value they recorded; a newer build may add fields to the view (`contains` in `tests/vectors.rs`).

- [ ] **Step 1: Write the failing tests**

Apply this change to `crates/replica/src/task.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/src/task.rs b/crates/replica/src/task.rs
index f13b6a4..729a717 100644
--- a/crates/replica/src/task.rs
+++ b/crates/replica/src/task.rs
@@ -735,6 +735,24 @@ mod tests {
         );
     }
 
+    #[test]
+    fn the_time_a_task_entered_its_stage_is_kept() {
+        let mut t = task(Stage::Todo);
+        assert_eq!(t.view().unwrap().stage_at, 1);
+        t.comment("no stage change", &Actor::Person, 3).unwrap();
+        assert_eq!(t.view().unwrap().stage_at, 1);
+        t.record_send(&api(), "r", &Actor::Person, 5).unwrap();
+        assert_eq!(t.view().unwrap().stage_at, 5);
+        t.record_report("Done.", &agent("codex/api"), 9).unwrap();
+        assert_eq!(t.view().unwrap().stage_at, 9);
+        t.set_stage(Stage::Done, &Actor::Person, 12).unwrap();
+        assert_eq!(t.view().unwrap().stage_at, 12);
+        // Documents written before the field existed fall back to creation.
+        t.fields().delete("stage_at").unwrap();
+        t.doc.commit();
+        assert_eq!(t.view().unwrap().stage_at, 1);
+    }
+
     #[test]
     fn a_newer_task_shows_read_only_with_fields_it_cannot_read() {
         let t = task(Stage::Todo);
````

Apply this change to `tests/test_tasks.py` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tests/test_tasks.py b/tests/test_tasks.py
index 0cc57db..023e654 100644
--- a/tests/test_tasks.py
+++ b/tests/test_tasks.py
@@ -189,6 +189,27 @@ class TaskCommands(Fixture):
         self.hgs("stage", "orchard/" + second, "backlog")
         self.assertEqual((self.ids("todo"), self.ids("backlog")), ([later, first], [second, other]))
 
+    def test_tasks_name_the_computer_of_their_session(self):
+        here = self.add("Local work", "--target", "codex/api")
+        self.assertEqual(here["target"]["host"], "")
+        refused = self.hgs("add", "--project", "orchard", "--target", "codex/api", "--host", "orchard-mac",
+                           text="Remote work", ok=False)
+        self.assertIn("orchard-mac is not a verified computer", refused)
+        # This computer verified orchard-mac itself: tasks can be pinned to its sessions.
+        catalog = self.root / "config" / "swarm" / "catalog.json"
+        data = json.loads(catalog.read_text())
+        mac = str(uuid.uuid4())
+        data.setdefault("bindings", {})["orchard-mac"] = {"id": mac, "verified": True}
+        catalog.write_text(json.dumps(data))
+        there = self.hgs("add", "--project", "orchard", "--target", "codex/web", "--host", "orchard-mac",
+                         text="Remote work")["task"]
+        self.assertEqual((there["target"]["machine"], there["target"]["host"]), (mac, "orchard-mac"))
+        moved = self.hgs("target", here["ref"], "codex/web", "--host", "orchard-mac")["task"]
+        self.assertEqual(moved["target"]["host"], "orchard-mac")
+        listed = {t["id"]: t for t in self.hgs("list", "--project", "orchard")["tasks"]}
+        self.assertEqual(listed[there["id"]]["target"]["host"], "orchard-mac")
+        self.assertIn("on another computer", self.hgs("send", there["ref"], ok=False))
+
     def test_a_project_name_with_a_slash(self):
         task = self.hgs("add", "--project", "team/web", text="Ship it")["task"]
         self.assertEqual(task["ref"], "team/web/" + task["id"])
````

- [ ] **Step 2: Run them to see them fail**

Run:
```bash
(cd crates/replica && cargo +1.99.1 test --locked -q --lib)
cargo +1.99.1 build --target-dir .ci-build/cargo -q && HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks
```
Expected: the crate fails to compile its tests — `no field `stage_at` on type `schema::TaskView``; the CLI suite fails `test_tasks_name_the_computer_of_their_session` with `KeyError: 'host'`.

- [ ] **Step 3: Implement**

Apply this change to `crates/replica/src/schema.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/src/schema.rs b/crates/replica/src/schema.rs
index 2043a89..14ed58e 100644
--- a/crates/replica/src/schema.rs
+++ b/crates/replica/src/schema.rs
@@ -96,6 +96,8 @@ pub struct TaskView {
     pub schema: i64,
     pub writable: bool,
     pub created_at: i64,
+    /// When the task entered its current stage (Unix milliseconds).
+    pub stage_at: i64,
     pub author: Actor,
     pub stage: Stage,
     pub target: Target,
````

Apply this change to `crates/replica/src/task.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/src/task.rs b/crates/replica/src/task.rs
index 729a717..b28b22b 100644
--- a/crates/replica/src/task.rs
+++ b/crates/replica/src/task.rs
@@ -77,6 +77,7 @@ impl TaskDoc {
         meta.insert("author", serde_json::to_string(&task.author)?)?;
         let fields = doc.get_map("fields");
         fields.insert("stage", task.stage.as_str())?;
+        fields.insert("stage_at", task.now)?;
         fields.insert("target", serde_json::to_string(&task.target)?)?;
         fields.insert("rank", task.rank)?;
         fields.insert("summary", "")?;
@@ -238,6 +239,10 @@ impl TaskDoc {
             schema: self.schema(),
             writable,
             created_at: integer(&meta, "created_at").unwrap_or(0),
+            // Documents written before this field existed report their creation.
+            stage_at: integer(&fields, "stage_at")
+                .or_else(|| integer(&meta, "created_at"))
+                .unwrap_or(0),
             author,
             stage: self.stage()?,
             target,
@@ -344,6 +349,7 @@ impl TaskDoc {
             )));
         }
         self.fields().insert("stage", to.as_str())?;
+        self.fields().insert("stage_at", now)?;
         self.log("stage", actor, now, to.as_str(), None)?;
         self.doc.commit();
         Ok(())
@@ -389,6 +395,7 @@ impl TaskDoc {
         self.check_send()?;
         let fields = self.fields();
         fields.insert("stage", Stage::Active.as_str())?;
+        fields.insert("stage_at", now)?;
         fields.insert("session", serde_json::to_string(session)?)?;
         fields.delete("cancelled_at")?;
         fields.delete("cancel_reason")?;
@@ -415,6 +422,7 @@ impl TaskDoc {
         if moved {
             let fields = self.fields();
             fields.insert("stage", Stage::Review.as_str())?;
+            fields.insert("stage_at", now)?;
             fields.insert("summary", summary)?;
         }
         self.log("report", actor, now, summary, None)?;
@@ -430,6 +438,7 @@ impl TaskDoc {
         }
         let fields = self.fields();
         fields.insert("stage", Stage::Review.as_str())?;
+        fields.insert("stage_at", now)?;
         fields.insert("cancelled_at", now)?;
         fields.insert("cancel_reason", reason.trim())?;
         self.log("cancelled", actor, now, "", None)?;
````

Apply this change to `crates/replica/tests/vectors.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/crates/replica/tests/vectors.rs b/crates/replica/tests/vectors.rs
index 1606f18..0c006a6 100644
--- a/crates/replica/tests/vectors.rs
+++ b/crates/replica/tests/vectors.rs
@@ -82,6 +82,29 @@ fn bless_task() {
     fs::write(path(&format!("{TASK}-merged.json")), merged).unwrap();
 }
 
+/// Every value the stored view has, this build's view has too. Later builds
+/// may add fields; they never drop or change the ones a vector recorded.
+fn contains(actual: &serde_json::Value, expected: &serde_json::Value, path: &str) {
+    use serde_json::Value;
+    match (actual, expected) {
+        (Value::Object(actual), Value::Object(expected)) => {
+            for (key, value) in expected {
+                let found = actual
+                    .get(key)
+                    .unwrap_or_else(|| panic!("{path}.{key} is missing"));
+                contains(found, value, &format!("{path}.{key}"));
+            }
+        }
+        (Value::Array(actual), Value::Array(expected)) => {
+            assert_eq!(actual.len(), expected.len(), "{path} length");
+            for (index, (found, value)) in actual.iter().zip(expected).enumerate() {
+                contains(found, value, &format!("{path}[{index}]"));
+            }
+        }
+        _ => assert_eq!(actual, expected, "{path}"),
+    }
+}
+
 fn merged(first: &str, second: &str) -> TaskView {
     let mut task = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
     task.import(&read(&format!("{TASK}-{first}.update")))
@@ -101,7 +124,7 @@ fn task_schema1_loro_1_16() {
     // Concurrent updates merge to the same task in either order.
     for (first, second) in [("a", "b"), ("b", "a")] {
         let view = merged(first, second);
-        assert_eq!(serde_json::to_value(&view).unwrap(), expected);
+        contains(&serde_json::to_value(&view).unwrap(), &expected, "view");
     }
     // The history reads oldest first, whatever order the merge gave its entries.
     let times: Vec<i64> = merged("a", "b").log.iter().map(|e| e.at).collect();
@@ -115,9 +138,10 @@ fn task_schema1_loro_1_16() {
         .unwrap();
     let mut fresh = TaskDoc::from_bytes(&read(&format!("{TASK}.loro"))).unwrap();
     fresh.import(&delta).unwrap();
-    assert_eq!(
-        serde_json::to_value(fresh.view().unwrap()).unwrap(),
-        expected
+    contains(
+        &serde_json::to_value(fresh.view().unwrap()).unwrap(),
+        &expected,
+        "view",
     );
     // The stored document still accepts this build's edits.
     both.set_stage(Stage::Done, &Actor::Person, 1_760_000_004_000)
````

Apply this change to `docs/reference.md` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/docs/reference.md b/docs/reference.md
index 14c848b..afd0cf5 100644
--- a/docs/reference.md
+++ b/docs/reference.md
@@ -1859,7 +1859,7 @@ JSON. Tasks belong to a catalog project, named by its ID or name; a task is
 `T-` plus six characters, and `PROJECT/T-xxxxxx` names it anywhere.
 
 ```sh
-hgs task add --project orchard [--backlog] [--target codex/api|any] [--position N] < prompt.txt
+hgs task add --project orchard [--backlog] [--target codex/api|any [--host ALIAS]] [--position N] < prompt.txt
 hgs task list --project orchard [--stage todo]
 hgs task get orchard/T-a1b2c3
 hgs task send orchard/T-a1b2c3 [--session codex/api]
@@ -1869,7 +1869,7 @@ hgs task stage orchard/T-a1b2c3 backlog|todo|done
 hgs task move orchard/T-a1b2c3 --top|--before REF|--after REF
 hgs task edit orchard/T-a1b2c3 < prompt.txt
 hgs task comment orchard/T-a1b2c3 < comment.txt
-hgs task target orchard/T-a1b2c3 codex/api|any
+hgs task target orchard/T-a1b2c3 codex/api|any [--host ALIAS]
 hgs task settings --project orchard [--set < settings.json]
 ```
 
@@ -1885,6 +1885,11 @@ for that session, and the project settings can keep agents to their own
 session or limit how many tasks they start per hour. `settings --set` reads a
 JSON object and changes only the fields it names.
 
+`--host ALIAS` pins a task to a session on another computer of the swarm that
+this computer verified itself; sending there comes with replication. Listings
+name each session's computer as `host`: empty for this one, the alias of a
+verified member, or null.
+
 Tasks are Loro documents below `HGS_STATE_DIR/replica`, one directory per
 project; a task file that fails to load is listed under `damaged` and left in
 place. Replication between computers, the Tasks view and the MCP tools come in
````

Apply this change to `src/tasks.rs` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/src/tasks.rs b/src/tasks.rs
index 903f267..cfbccf8 100644
--- a/src/tasks.rs
+++ b/src/tasks.rs
@@ -9,6 +9,7 @@
 use crate::cli::{Error, Result};
 use crate::config::Config;
 use serde_json::{json, Value};
+use std::collections::BTreeMap;
 use std::io::Read;
 use std::time::{SystemTime, UNIX_EPOCH};
 use zerus_replica::{
@@ -16,7 +17,7 @@ use zerus_replica::{
     Stage, Target, TaskDoc, Tools, MAX_TEXT,
 };
 
-const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any | send REF [--session SESSION] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
+const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any [--host ALIAS]] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any [--host ALIAS] | send REF [--session SESSION] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
   REF is T-xxxxxx with --project P, or PROJECT/T-xxxxxx; texts and --set settings are read from stdin";
 
 fn fail(message: impl std::fmt::Display) -> Error {
@@ -95,8 +96,43 @@ fn store(config: &Config, project_id: &str) -> Result<ProjectStore> {
     ProjectStore::open(&config.state.join("replica"), project_id).map_err(fail)
 }
 
-fn machine(config: &Config) -> Result<String> {
-    crate::swarm::local_node_id(config).map_err(fail)
+/// This computer and the swarm members it verified itself, by SSH alias.
+struct Machines {
+    here: String,
+    peers: BTreeMap<String, String>,
+}
+
+fn machines(config: &Config) -> Result<Machines> {
+    let here = crate::swarm::local_node_id(config).map_err(fail)?;
+    #[cfg(unix)]
+    let peers = crate::swarm::verified_bindings(config, &here);
+    #[cfg(not(unix))]
+    let peers = BTreeMap::new();
+    Ok(Machines { here, peers })
+}
+
+impl Machines {
+    /// The computer `--host ALIAS` names, or this one without it.
+    fn node(&self, host: Option<&str>) -> Result<String> {
+        match host.filter(|h| !h.is_empty()) {
+            None => Ok(self.here.clone()),
+            Some(alias) => self.peers.get(alias).cloned().ok_or_else(|| {
+                fail(format!("{alias} is not a verified computer of this swarm"))
+            }),
+        }
+    }
+
+    /// How callers name a computer: "" for this one, a verified member's
+    /// alias, or null when this computer does not know it.
+    fn host(&self, node: &str) -> Value {
+        if node == self.here {
+            return json!("");
+        }
+        self.peers
+            .iter()
+            .find(|(_, id)| id.as_str() == node)
+            .map_or(Value::Null, |(alias, _)| json!(alias))
+    }
 }
 
 /// The agent of the session that runs this command, as hooks know it (both
@@ -112,9 +148,14 @@ fn actor(here: &str) -> Actor {
     }
 }
 
-fn view(task: &TaskDoc, project_name: &str, with_log: bool) -> Result<Value> {
+fn view(task: &TaskDoc, project_name: &str, with_log: bool, machines: &Machines) -> Result<Value> {
     let mut value = serde_json::to_value(task.view().map_err(fail)?).map_err(fail)?;
     value["ref"] = json!(format!("{project_name}/{}", task.id()));
+    for key in ["target", "session"] {
+        if let Some(machine) = value[key]["machine"].as_str().map(str::to_owned) {
+            value[key]["host"] = machines.host(&machine);
+        }
+    }
     if !with_log {
         value.as_object_mut().expect("object").remove("log");
     }
@@ -146,10 +187,11 @@ pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
         "list" => list(config, args),
         "get" => {
             let r = reference(config, args)?;
+            let machines = machines(config)?;
             let task = store(config, &r.project.0)?
                 .find(&r.task)
                 .map_err(fail)?;
-            print(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true)?}))
+            print(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true, &machines)?}))
         }
         "add" => add(config, args),
         "edit" | "move" | "stage" | "target" | "comment" | "report" | "send" | "cancel" => {
@@ -169,6 +211,7 @@ fn list(config: &Config, args: &[String]) -> Result<i32> {
         .map(Stage::parse)
         .transpose()
         .map_err(fail)?;
+    let machines = machines(config)?;
     // One load of the project; each stage is then a filter of the sorted list.
     let (tasks, damaged) = store(config, &id)?.listing().map_err(fail)?;
     let mut out = Vec::new();
@@ -183,7 +226,7 @@ fn list(config: &Config, args: &[String]) -> Result<i32> {
             continue;
         }
         for task in tasks.iter().filter(|t| t.stage().ok() == Some(stage)) {
-            out.push(view(task, &name, false)?);
+            out.push(view(task, &name, false, &machines)?);
         }
     }
     print(json!({"project": {"id": id, "name": name}, "tasks": out, "damaged": damaged}))
@@ -195,8 +238,8 @@ fn add(config: &Config, args: &[String]) -> Result<i32> {
         config,
         option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
     )?;
-    let here = machine(config)?;
-    let author = actor(&here);
+    let machines = machines(config)?;
+    let author = actor(&machines.here);
     let stage = if flag(args, "--backlog") {
         Stage::Backlog
     } else {
@@ -212,7 +255,7 @@ fn add(config: &Config, args: &[String]) -> Result<i32> {
         (Actor::Agent(own), None) => Target::Session(own.clone()),
         (Actor::Person, None | Some("any")) => Target::Any,
         (Actor::Person, Some(session)) => Target::Session(SessionKey {
-            machine: here.clone(),
+            machine: machines.node(option(args, "--host")?)?,
             session: session.to_owned(),
         }),
     };
@@ -242,7 +285,7 @@ fn add(config: &Config, args: &[String]) -> Result<i32> {
     .map_err(fail)?;
     check_prompt(&store, &task, &name)?;
     store.save(&mut task).map_err(fail)?;
-    print(json!({"task": view(&task, &name, true)?}))
+    print(json!({"task": view(&task, &name, true, &machines)?}))
 }
 
 fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
@@ -254,8 +297,9 @@ fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
     };
     let r = reference(config, args)?;
     let (project_id, project_name) = (&r.project.0, &r.project.1);
-    let here = machine(config)?;
-    let who = actor(&here);
+    let machines = machines(config)?;
+    let here = machines.here.as_str();
+    let who = actor(here);
     let store = store(config, project_id)?;
     let mut task = store.find(&r.task).map_err(fail)?;
     let text = text.unwrap_or_default();
@@ -304,18 +348,18 @@ fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
                 Target::Any
             } else {
                 Target::Session(SessionKey {
-                    machine: here.clone(),
+                    machine: machines.node(option(args, "--host")?)?,
                     session: value.clone(),
                 })
             };
             task.set_target(&target, &who, now()).map_err(fail)?;
         }
-        "send" => extra = send(&here, &store, &mut task, project_name, args, &who)?,
-        "cancel" => extra = cancel(&here, &store, &mut task, project_name, args, &who)?,
+        "send" => extra = send(here, &store, &mut task, project_name, args, &who)?,
+        "cancel" => extra = cancel(here, &store, &mut task, project_name, args, &who)?,
         _ => return Err(fail(USAGE)),
     }
     store.save(&mut task).map_err(fail)?;
-    let mut out = json!({"task": view(&task, project_name, true)?});
+    let mut out = json!({"task": view(&task, project_name, true, &machines)?});
     for (key, value) in extra.as_object().expect("object") {
         out[key] = value.clone();
     }
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
(cd crates/replica && cargo +1.99.1 fmt --check && cargo +1.99.1 clippy --locked --all-targets -q -- -D warnings && cargo +1.99.1 test --locked -q)
cargo +1.99.1 clippy --target-dir .ci-build/cargo --all-targets -q -- -D warnings
cargo +1.99.1 build --target-dir .ci-build/cargo -q && HGS_TEST_BIN=.ci-build/cargo/debug/hgs python3 -m unittest tests.test_tasks
```
Expected: no format or lint output; the crate `40 passed` and the vectors `2 passed`; `Ran 19 tests … OK`.

- [ ] **Step 5: Commit**

```bash
git add crates/replica/src/schema.rs \
  crates/replica/src/task.rs \
  crates/replica/tests/vectors.rs \
  docs/reference.md \
  src/tasks.rs \
  tests/test_tasks.py
git commit -m "Report stage times and other computers in hgs task"
```

---

### Task 2: Task listings in the GUI

**Files:**
- Create: `tray/tests/test_taskboard.cpp`, `tray/src/TaskBoard.cpp`, `tray/src/TaskBoard.h`
- Modify: `tray/tests/CMakeLists.txt`, `tray/tests/test_hgsclient.cpp`, `tray/CMakeLists.txt`, `tray/src/HgsClient.cpp`, `tray/src/HgsClient.h`

**Interfaces:**
- Produces: `HgsClient::requestTask(const QStringList &arguments, const QString &input = {}) -> quint64` with signals `taskReady(quint64, const QJsonObject &)` and `taskFailed(quint64, const QString &)` ("Invalid task response" for output that is not a JSON object, hgs's stderr otherwise). `TaskItem` (the fields of one task view), `enum class TaskAction { Edit, ToBacklog, Top, SendNow, ToTodo, Cancel, BackToTodo, Accept }` and `TaskBoard` with `fromListing(QJsonObject)`, `item(QJsonObject)`, `belongsTo`, `keptFor`, `View { host, session, only, now }`, `rows(stage, view, older = false)`, `count(stage, view)`, `stages()`, `stageLabel`, `stageIcon`, `actions(stage)`, `actionLabel`, `actionIcon`, `RecentDoneMs`.

`TaskBoard` lives in `hgs-tray-core`: it needs no widgets. Rows keep hgs order for To do and Backlog (rank), oldest first for In review and In progress, newest first for Done; Done holds the last 7 days and `older` asks for the rest. To do numbers are the project-wide queue positions.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/CMakeLists.txt b/tray/tests/CMakeLists.txt
index 8d4e8e6..6c771d9 100644
--- a/tray/tests/CMakeLists.txt
+++ b/tray/tests/CMakeLists.txt
@@ -253,6 +253,12 @@ target_include_directories(test_fleetstate PRIVATE ../src)
 target_link_libraries(test_fleetstate PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
 zerus_add_test(fleetstate test_fleetstate)
 
+# Project tasks as the Tasks tab shows them (TaskBoard.h).
+add_executable(test_taskboard test_taskboard.cpp)
+target_include_directories(test_taskboard PRIVATE ../src)
+target_link_libraries(test_taskboard PRIVATE Qt6::Test Qt6::Core hgs-tray-core)
+zerus_add_test(taskboard test_taskboard)
+
 # Folder paths per machine (MachinePlatform.h, header-only): both styles everywhere.
 add_executable(test_machineplatform test_machineplatform.cpp)
 target_include_directories(test_machineplatform PRIVATE ../src)
````

Apply this change to `tray/tests/test_hgsclient.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_hgsclient.cpp b/tray/tests/test_hgsclient.cpp
index aa9a787..cadac98 100644
--- a/tray/tests/test_hgsclient.cpp
+++ b/tray/tests/test_hgsclient.cpp
@@ -36,6 +36,7 @@ private slots:
     void parsesBox();
     void parsesSharedReadState();
     void readMarksUseStdinAndAcceptOnlyHostAnswers();
+    void taskCommandsUseStdinAndReportFailures();
     void cliVersionUsesConfiguredExecutableAndRefreshes();
     void parsesSavedSessions();
     void parsesActivityMetadata();
@@ -1683,6 +1684,35 @@ void TestHgsClient::parsesSharedReadState()
     QVERIFY(!box.sessions[2].sharedRead);
 }
 
+void TestHgsClient::taskCommandsUseStdinAndReportFailures()
+{
+    QTemporaryDir directory; QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
+    script.write(R"(#!/usr/bin/env python3
+import json,pathlib,sys
+text=sys.stdin.read()
+pathlib.Path(__file__).with_name('task.json').write_text(json.dumps({'argv':sys.argv[1:],'stdin':text}))
+if sys.argv[2]=='fail': print('no task T-zz in this project',file=sys.stderr);sys.exit(1)
+if sys.argv[2]=='garbage': print('not json');sys.exit(0)
+print(json.dumps({'task':{'id':'T-1'}}))
+)");
+    script.close(); QVERIFY(FakeCli::makeRunnable(script.fileName()));
+    HgsClient client(FakeCli::program(script.fileName()));
+    QSignalSpy ready(&client, &HgsClient::taskReady), failed(&client, &HgsClient::taskFailed);
+    const QString prompt = "Fix '$HOME' and `quotes`\nsecond line";
+    const auto id = client.requestTask({"add", "--project", "orchard"}, prompt);
+    QTRY_COMPARE(ready.size(), 1); QCOMPARE(ready[0][0].toULongLong(), id);
+    QCOMPARE(ready[0][1].toJsonObject().value("task").toObject().value("id").toString(), QString("T-1"));
+    QFile log(directory.filePath("task.json")); QVERIFY(log.open(QIODevice::ReadOnly));
+    const auto call = QJsonDocument::fromJson(log.readAll()).object();
+    QCOMPARE(call.value("argv").toArray(), QJsonArray({"task", "add", "--project", "orchard"}));
+    QCOMPARE(call.value("stdin").toString(), prompt);
+    const auto missing = client.requestTask({"fail"});
+    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][0].toULongLong(), missing);
+    QCOMPARE(failed[0][1].toString(), QString("no task T-zz in this project"));
+    client.requestTask({"garbage"});
+    QTRY_COMPARE(failed.size(), 2); QCOMPARE(failed[1][1].toString(), QString("Invalid task response"));
+}
+
 void TestHgsClient::readMarksUseStdinAndAcceptOnlyHostAnswers()
 {
     QTemporaryDir directory; QFile script(directory.filePath("hgs")); QVERIFY(script.open(QIODevice::WriteOnly));
````

Create `tray/tests/test_taskboard.cpp`:

````cpp
#include "TaskBoard.h"
#include <QJsonArray>
#include <QTest>

namespace {
constexpr qint64 Day = 24LL * 60 * 60 * 1000, Now = 100 * Day;
QJsonObject session(const QString &name, const QJsonValue &host = QString()) {
    return {{"kind", "session"}, {"machine", "m1"}, {"session", name}, {"host", host}};
}
QJsonObject task(const QString &id, const QString &stage, const QJsonObject &extra = {}) {
    QJsonObject t{{"uuid", "uuid-" + id}, {"id", id}, {"ref", "orchard/" + id}, {"text", "Task " + id}, {"stage", stage},
                  {"summary", ""}, {"cancel_reason", ""}, {"cancelled_at", QJsonValue()}, {"created_at", 1}, {"stage_at", Now - Day},
                  {"writable", true}, {"author", QJsonObject{{"kind", "person"}}}, {"target", QJsonObject{{"kind", "any"}}},
                  {"session", QJsonValue()}, {"rank", "V"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) t[it.key()] = it.value();
    return t;
}
TaskBoard board() {
    QJsonObject sent{{"machine", "m1"}, {"session", "codex/api"}, {"host", ""}};
    QJsonObject remote{{"machine", "m2"}, {"session", "codex/web"}, {"host", "orchard-mac"}};
    return TaskBoard::fromListing({{"project", QJsonObject{{"id", "project-1"}, {"name", "orchard"}}},
        {"damaged", QJsonArray{QJsonObject{{"uuid", "bad"}, {"error", "not loro"}}}},
        {"tasks", QJsonArray{
            task("T-r2", "review", {{"session", sent}, {"stage_at", Now - 2 * Day}, {"summary", "Fixed."}}),
            task("T-r1", "review", {{"session", remote}, {"stage_at", Now - 3 * Day}}),
            task("T-a1", "active", {{"session", sent}}),
            task("T-t1", "todo", {{"target", session("codex/api")}}),
            task("T-t2", "todo"),
            task("T-t3", "todo", {{"target", session("codex/web", "orchard-mac")}, {"author", QJsonObject{{"kind", "agent"}, {"machine", "m2"}, {"session", "codex/web"}}}}),
            task("T-b1", "backlog", {{"target", session("codex/ghost", QJsonValue())}}),
            task("T-d1", "done", {{"session", sent}, {"stage_at", Now - 2 * Day}}),
            task("T-d2", "done", {{"session", sent}, {"stage_at", Now - Day}}),
            task("T-d3", "done", {{"session", remote}, {"stage_at", Now - 9 * Day}}),
        }}});
}
QStringList ids(const QList<TaskItem> &rows) { QStringList out; for (const auto &row : rows) out << row.id; return out; }
}

class TestTaskBoard : public QObject {
    Q_OBJECT
private slots:
    void readsTheListing();
    void rowsFollowEachStageOrder();
    void theOpenSessionsTasksAndTheFilter();
    void actionsFollowTheStage();
};

void TestTaskBoard::readsTheListing() {
    const auto b = board();
    QCOMPARE(b.projectId, QString("project-1")); QCOMPARE(b.projectName, QString("orchard"));
    QCOMPARE(b.tasks.size(), 10); QCOMPARE(b.damaged, 1);
    const auto byId = [&](const QString &id) { for (const auto &t : b.tasks) if (t.id == id) return t; return TaskItem{}; };
    QCOMPARE(byId("T-t1").targetSession, QString("codex/api")); QCOMPARE(byId("T-t1").targetHost, QString());
    QVERIFY(byId("T-t2").targetSession.isEmpty());
    QCOMPARE(byId("T-t3").targetHost, QString("orchard-mac")); QCOMPARE(byId("T-t3").author, QString("codex/web"));
    QVERIFY(!byId("T-b1").targetKnown);
    QCOMPARE(byId("T-r2").sessionName, QString("codex/api")); QCOMPARE(byId("T-r2").summary, QString("Fixed."));
    QCOMPARE(byId("T-r1").sessionHost, QString("orchard-mac"));
    // To do keeps the project's queue positions.
    QCOMPARE(byId("T-t1").queue, 1); QCOMPARE(byId("T-t3").queue, 3); QCOMPARE(byId("T-b1").queue, 0);
}

void TestTaskBoard::rowsFollowEachStageOrder() {
    const auto b = board(); const TaskBoard::View all{"", "codex/api", false, Now};
    QCOMPARE(ids(b.rows("review", all)), QStringList({"T-r1", "T-r2"}));       // oldest report first
    QCOMPARE(ids(b.rows("todo", all)), QStringList({"T-t1", "T-t2", "T-t3"})); // the queue
    // Done shows the last 7 days, newest first; older ones come apart.
    QCOMPARE(ids(b.rows("done", all)), QStringList({"T-d2", "T-d1"}));
    QCOMPARE(ids(b.rows("done", all, true)), QStringList({"T-d3"}));
    QCOMPARE(b.count("done", all), 2); QCOMPARE(b.count("active", all), 1);
}

void TestTaskBoard::theOpenSessionsTasksAndTheFilter() {
    const auto b = board(); QHash<QString, TaskItem> t; for (const auto &item : b.tasks) t.insert(item.id, item);
    QVERIFY(TaskBoard::belongsTo(t["T-t1"], "", "codex/api"));          // pinned to it
    QVERIFY(!TaskBoard::belongsTo(t["T-t2"], "", "codex/api"));         // Any session
    QVERIFY(TaskBoard::belongsTo(t["T-r2"], "", "codex/api"));          // sent to it
    QVERIFY(!TaskBoard::belongsTo(t["T-r1"], "", "codex/web"));         // same name, another computer
    QVERIFY(TaskBoard::belongsTo(t["T-r1"], "orchard-mac", "codex/web"));
    QVERIFY(!TaskBoard::belongsTo(t["T-b1"], "", "codex/ghost"));       // unknown computer
    const TaskBoard::View only{"", "codex/api", true, Now};
    QCOMPARE(ids(b.rows("todo", only)), QStringList({"T-t1", "T-t2"}));
    QCOMPARE(ids(b.rows("review", only)), QStringList({"T-r2"}));
    QCOMPARE(b.count("backlog", only), 0);
    // Queue numbers stay project-wide while others are hidden.
    QCOMPARE(b.rows("todo", only).at(1).queue, 2);
}

void TestTaskBoard::actionsFollowTheStage() {
    using A = TaskAction;
    QCOMPARE(TaskBoard::actions("todo"), QList<A>({A::Edit, A::ToBacklog, A::Top, A::SendNow}));
    QCOMPARE(TaskBoard::actions("backlog"), QList<A>({A::Edit, A::ToTodo, A::SendNow}));
    QCOMPARE(TaskBoard::actions("active"), QList<A>({A::Cancel}));
    QCOMPARE(TaskBoard::actions("review"), QList<A>({A::BackToTodo, A::Accept}));
    QCOMPARE(TaskBoard::actions("done"), QList<A>({A::BackToTodo}));
    QCOMPARE(TaskBoard::stages(), QStringList({"review", "active", "todo", "backlog", "done"}));
    QCOMPARE(TaskBoard::stageLabel("active"), QString("In progress"));
    QCOMPARE(TaskBoard::actionLabel(A::Top), QString("Move to the top of To do"));
}

QTEST_MAIN(TestTaskBoard)
#include "test_taskboard.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_taskboard test_hgsclient -j 8`
Expected: FAIL to compile — `TaskBoard.h: No such file or directory`, and `taskReady` / `requestTask` are not members of `HgsClient`.

- [ ] **Step 3: Implement**

Apply this change to `tray/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/CMakeLists.txt b/tray/CMakeLists.txt
index 9514a25..b3c44b2 100644
--- a/tray/CMakeLists.txt
+++ b/tray/CMakeLists.txt
@@ -113,7 +113,7 @@ include(cmake/BuildVersion.cmake)
 add_library(hgs-tray-core STATIC src/ProcessRunner.cpp src/HgsClient.cpp src/FleetState.cpp
     src/ReadMarkSync.cpp src/AppConfig.cpp src/AccountUsageStore.cpp src/ComposerDraftStore.cpp
     src/SwarmController.cpp src/DesktopPresence.cpp src/AttentionNotifier.h src/CommandBackend.cpp
-    src/LocalSessionHost.cpp src/DownloadCache.cpp src/AutomaticApprovals.cpp
+    src/LocalSessionHost.cpp src/DownloadCache.cpp src/AutomaticApprovals.cpp src/TaskBoard.cpp
     # Main-thread watchdog (MainThreadWatchdog.h): every platform, prompted by macOS.
     src/MainThreadWatchdog.cpp)
 if(NOT WIN32)
````

Apply this change to `tray/src/HgsClient.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/HgsClient.cpp b/tray/src/HgsClient.cpp
index 68fb976..9081aa2 100644
--- a/tray/src/HgsClient.cpp
+++ b/tray/src/HgsClient.cpp
@@ -510,6 +510,29 @@ quint64 HgsClient::requestSwarm(const QStringList &arguments, const QJsonObject
     return request;
 }
 
+quint64 HgsClient::requestTask(const QStringList &arguments, const QString &input)
+{
+    const quint64 request = ++m_taskRequest;
+    QStringList args{QStringLiteral("task")}; args.append(arguments);
+    // A send waits for hgs to accept the paste; everything else is a local file.
+    auto call = processRequest(m_hgs, args); call.timeoutMs = 30000;
+    // stdin always closes, even empty: a command that reads a text never waits on the GUI.
+    call.input = input.toUtf8();
+    ProcessRunner::run(std::move(call), this, [this, request](const ProcessRunner::Result &result) {
+        if (result.outcome == ProcessRunner::Result::TimedOut) return emit taskFailed(request, tr("hgs task timed out"));
+        if (result.outcome == ProcessRunner::Result::FailedToStart)
+            return emit taskFailed(request, tr("cannot start '%1': %2").arg(m_hgs, result.errorString));
+        if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0) {
+            const QString detail = QString::fromLocal8Bit(result.standardError).trimmed().left(4000);
+            return emit taskFailed(request, detail.isEmpty() ? tr("exit code %1").arg(result.exitCode) : detail);
+        }
+        QJsonParseError error; const auto data = QJsonDocument::fromJson(result.standardOutput, &error);
+        if (error.error != QJsonParseError::NoError || !data.isObject()) emit taskFailed(request, tr("Invalid task response"));
+        else emit taskReady(request, data.object());
+    });
+    return request;
+}
+
 quint64 HgsClient::requestSessionHost(const QString &action)
 {
     const quint64 request = ++m_sessionHostRequest;
````

Apply this change to `tray/src/HgsClient.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/HgsClient.h b/tray/src/HgsClient.h
index b667978..0856757 100644
--- a/tray/src/HgsClient.h
+++ b/tray/src/HgsClient.h
@@ -189,6 +189,9 @@ public:
     // reaches. Probes the host CLI first, so an older one is never asked.
     quint64 requestMobile(const QString &host, const QStringList &arguments);
     quint64 requestSwarm(const QStringList &arguments = {QStringLiteral("get")}, const QJsonObject &input = {});
+    // `hgs task <arguments>` on this computer, whose replica holds the project's
+    // tasks. A prompt, report or comment travels on stdin; the reply is the JSON object.
+    quint64 requestTask(const QStringList &arguments, const QString &input = {});
     // `hgs session-host <action> --json` on this computer (Windows, spec D2):
     // install enables local sessions and stages the stable CLI and a host image.
     quint64 requestSessionHost(const QString &action);
@@ -319,6 +322,8 @@ signals:
     void machinesFailed(quint64 request, const QString &error);
     void swarmReady(quint64 request, const QJsonObject &result);
     void swarmFailed(quint64 request, const QString &error);
+    void taskReady(quint64 request, const QJsonObject &result);
+    void taskFailed(quint64 request, const QString &error);
     void sessionHostReady(quint64 request, const QJsonObject &result);
     void sessionHostFailed(quint64 request, const QString &error);
     void presenceFinished(const QString &error); // Empty on success.
@@ -419,6 +424,7 @@ private:
     quint64 m_machineRequest = 0;
     quint64 m_mobileRequest = 0;
     quint64 m_swarmRequest = 0;
+    quint64 m_taskRequest = 0;
     quint64 m_sessionHostRequest = 0;
     quint64 m_accountRequest = 0;
     bool m_machineSetupInFlight = false;
````

Create `tray/src/TaskBoard.cpp`:

````cpp
#include "TaskBoard.h"
#include <QJsonArray>
#include <algorithm>

namespace {
bool listed(const QString &stage) { return stage == QLatin1String("todo") || stage == QLatin1String("backlog"); }
// A session reference: (host, name, known). A null host is a computer this one
// has not verified.
void session(const QJsonValue &value, QString &host, QString &name, bool &known) {
    const auto object = value.toObject();
    name = object.value("session").toString(); host = object.value("host").toString();
    known = !object.contains("host") || !object.value("host").isNull();
}
}

TaskItem TaskBoard::item(const QJsonObject &t) {
    TaskItem item;
    item.ref = t.value("ref").toString(); item.id = t.value("id").toString(); item.uuid = t.value("uuid").toString();
    item.text = t.value("text").toString(); item.stage = t.value("stage").toString();
    item.summary = t.value("summary").toString(); item.cancelReason = t.value("cancel_reason").toString();
    item.createdAt = qint64(t.value("created_at").toDouble()); item.stageAt = qint64(t.value("stage_at").toDouble());
    item.cancelledAt = qint64(t.value("cancelled_at").toDouble()); item.writable = t.value("writable").toBool(true);
    const auto target = t.value("target").toObject();
    if (target.value("kind").toString() == QLatin1String("session")) session(target, item.targetHost, item.targetSession, item.targetKnown);
    if (t.value("session").isObject()) session(t.value("session"), item.sessionHost, item.sessionName, item.sessionKnown);
    const auto author = t.value("author").toObject();
    if (author.value("kind").toString() == QLatin1String("agent")) item.author = author.value("session").toString();
    return item;
}

TaskBoard TaskBoard::fromListing(const QJsonObject &listing) {
    TaskBoard board;
    const auto project = listing.value("project").toObject();
    board.projectId = project.value("id").toString(); board.projectName = project.value("name").toString();
    board.damaged = int(listing.value("damaged").toArray().size());
    int queue = 0;
    for (const auto &value : listing.value("tasks").toArray()) {
        auto task = item(value.toObject());
        if (task.stage == QLatin1String("todo")) task.queue = ++queue;
        board.tasks << task;
    }
    return board;
}

bool TaskBoard::belongsTo(const TaskItem &t, const QString &host, const QString &name) {
    if (listed(t.stage)) return t.targetKnown && !t.targetSession.isEmpty() && t.targetHost == host && t.targetSession == name;
    return t.sessionKnown && !t.sessionName.isEmpty() && t.sessionHost == host && t.sessionName == name;
}

bool TaskBoard::keptFor(const TaskItem &t, const QString &host, const QString &name) {
    return belongsTo(t, host, name) || (listed(t.stage) && t.targetSession.isEmpty());
}

QList<TaskItem> TaskBoard::rows(const QString &stage, const View &view, bool older) const {
    QList<TaskItem> out;
    const bool done = stage == QLatin1String("done");
    for (const auto &t : tasks) {
        if (t.stage != stage || (view.only && !keptFor(t, view.host, view.session))) continue;
        if (done && (view.now - t.stageAt > RecentDoneMs) != older) continue;
        out << t;
    }
    if (done) std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.stageAt > b.stageAt; });
    else if (!listed(stage)) std::stable_sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.stageAt < b.stageAt; });
    return out;
}

const QStringList &TaskBoard::stages() {
    static const QStringList order{"review", "active", "todo", "backlog", "done"};
    return order;
}

QString TaskBoard::stageLabel(const QString &stage) {
    if (stage == QLatin1String("review")) return QStringLiteral("In review");
    if (stage == QLatin1String("active")) return QStringLiteral("In progress");
    if (stage == QLatin1String("todo")) return QStringLiteral("To do");
    if (stage == QLatin1String("backlog")) return QStringLiteral("Backlog");
    return QStringLiteral("Done");
}

// A stage's icon is the icon of the action that moves a task there.
QString TaskBoard::stageIcon(const QString &stage) {
    if (stage == QLatin1String("review")) return QStringLiteral("task-inbox");
    if (stage == QLatin1String("active")) return QStringLiteral("play");
    if (stage == QLatin1String("todo")) return QStringLiteral("task-list");
    if (stage == QLatin1String("backlog")) return QStringLiteral("task-clock");
    return QStringLiteral("task-check");
}

QList<TaskAction> TaskBoard::actions(const QString &stage) {
    using A = TaskAction;
    if (stage == QLatin1String("todo")) return {A::Edit, A::ToBacklog, A::Top, A::SendNow};
    if (stage == QLatin1String("backlog")) return {A::Edit, A::ToTodo, A::SendNow};
    if (stage == QLatin1String("active")) return {A::Cancel};
    if (stage == QLatin1String("review")) return {A::BackToTodo, A::Accept};
    if (stage == QLatin1String("done")) return {A::BackToTodo};
    return {};
}

QString TaskBoard::actionLabel(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QStringLiteral("Edit task");
    case TaskAction::ToBacklog: return QStringLiteral("Move to Backlog");
    case TaskAction::Top: return QStringLiteral("Move to the top of To do");
    case TaskAction::SendNow: return QStringLiteral("Send now");
    case TaskAction::ToTodo: return QStringLiteral("Move to To do");
    case TaskAction::Cancel: return QStringLiteral("Cancel task…");
    case TaskAction::BackToTodo: return QStringLiteral("Back to To do");
    case TaskAction::Accept: return QStringLiteral("Accept");
    }
    return {};
}

QString TaskBoard::actionIcon(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QStringLiteral("task-edit");
    case TaskAction::ToBacklog: return QStringLiteral("task-clock");
    case TaskAction::Top: return QStringLiteral("task-top");
    case TaskAction::SendNow: return QStringLiteral("play");
    case TaskAction::ToTodo: case TaskAction::BackToTodo: return QStringLiteral("task-list-add");
    case TaskAction::Cancel: return QStringLiteral("close");
    case TaskAction::Accept: return QStringLiteral("task-check");
    }
    return {};
}
````

Create `tray/src/TaskBoard.h`:

````cpp
#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// One task as `hgs task list` reports it.
struct TaskItem {
    QString ref, id, uuid, text, stage, summary, cancelReason;
    // A pinned session: its computer ("" for this one, otherwise its SSH alias) and
    // name. An empty name is Any session; targetKnown is false for a computer this
    // one has not verified.
    QString targetHost, targetSession;
    bool targetKnown = true;
    // The session the task was last sent to; empty when it was never sent.
    QString sessionHost, sessionName;
    bool sessionKnown = true;
    // Who filed it: empty for the person, otherwise the agent's session name.
    QString author;
    qint64 createdAt = 0, stageAt = 0, cancelledAt = 0;
    bool writable = true;
    int queue = 0;   // position in the project's To do, from 1
};

enum class TaskAction { Edit, ToBacklog, Top, SendNow, ToTodo, Cancel, BackToTodo, Accept };

// A project's tasks and the rules the Tasks tab shows them by
// (docs/superpowers/specs/2026-10-13-project-task-queue-design.md, Desktop interface).
class TaskBoard {
public:
    static constexpr qint64 RecentDoneMs = 7LL * 24 * 60 * 60 * 1000;
    static TaskBoard fromListing(const QJsonObject &listing);
    static TaskItem item(const QJsonObject &task);
    QString projectId, projectName;
    QList<TaskItem> tasks;   // in hgs order: each stage's tasks by rank
    int damaged = 0;

    // The open session's tasks: pinned to it in To do and Backlog, sent to it later.
    static bool belongsTo(const TaskItem &task, const QString &host, const QString &session);
    // "Only this session" keeps those and the Any session tasks in To do and Backlog.
    static bool keptFor(const TaskItem &task, const QString &host, const QString &session);
    struct View { QString host, session; bool only = false; qint64 now = 0; };
    // A stage's rows in display order: To do and Backlog by rank, In review and In
    // progress oldest first, Done newest first. Done holds the last 7 days; `older`
    // asks for the rest of it.
    QList<TaskItem> rows(const QString &stage, const View &view, bool older = false) const;
    int count(const QString &stage, const View &view) const { return int(rows(stage, view).size()); }

    static const QStringList &stages();   // the tiles' order
    static QString stageLabel(const QString &stage);
    static QString stageIcon(const QString &stage);
    // The same actions in the same order on every surface; the main one is last.
    static QList<TaskAction> actions(const QString &stage);
    static QString actionLabel(TaskAction action);
    static QString actionIcon(TaskAction action);
};
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake --build .ci-build/gui --target test_taskboard test_hgsclient -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskboard
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_hgsclient
```
Expected: `test_taskboard` `Totals: 6 passed`; `test_hgsclient` all pass, including `taskCommandsUseStdinAndReportFailures`.

- [ ] **Step 5: Commit**

```bash
git add tray/CMakeLists.txt \
  tray/src/HgsClient.cpp \
  tray/src/HgsClient.h \
  tray/src/TaskBoard.cpp \
  tray/src/TaskBoard.h \
  tray/tests/CMakeLists.txt \
  tray/tests/test_hgsclient.cpp \
  tray/tests/test_taskboard.cpp
git commit -m "Read project tasks in the GUI through hgs task"
```

---

### Task 3: The task input

**Files:**
- Modify: `tray/tests/test_messagecomposer.cpp`, `tray/src/ComposerDraftStore.cpp`, `tray/src/ComposerDraftStore.h`, `tray/src/MessageComposer.cpp`, `tray/src/MessageComposer.h`

**Interfaces:**
- Produces: `MessageComposer::setTaskMode(bool)` (no toolbar, attach, model settings or Stop; attachments refused with "A task is text only."), `setSendLabel(const QString &)`, `setPlaceholder(const QString &)`. `ComposerDraftOwner::Kind::Task` for keys `task-draft\n<session key>`, named "task prompt" in Saved drafts.

The task input is the Activity input in task mode, so drafts survive restarts like message drafts.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/test_messagecomposer.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_messagecomposer.cpp b/tray/tests/test_messagecomposer.cpp
index 171ab27..18d63a5 100644
--- a/tray/tests/test_messagecomposer.cpp
+++ b/tray/tests/test_messagecomposer.cpp
@@ -148,6 +148,7 @@ class TestMessageComposer : public QObject {
     Q_OBJECT
 private slots:
     void unsupportedAttachmentsPreserveTextAndAllowExplicitRemoval();
+    void taskModeAddsTextOnlyWithTheStageButton();
     void initTestCase();
     void voiceInsertionRequiresUnchangedDraftAndNavigation();
     void init() { ComposerDraftStore::flushWrites(); QVERIFY(QDir(ComposerDraftStore::directory()).removeRecursively()); }
@@ -236,6 +237,25 @@ void TestMessageComposer::unsupportedAttachmentsPreserveTextAndAllowExplicitRemo
     composer.setRecording(true);QVERIFY(!send->isEnabled());composer.setRecording(false);QVERIFY(send->isEnabled());
 }
 
+void TestMessageComposer::taskModeAddsTextOnlyWithTheStageButton()
+{
+    // Saved drafts names a task prompt by the session it is for.
+    const auto owner=ComposerDraftStore::owner("task-draft\nmac\ncodex/api");
+    QCOMPARE(int(owner.kind),int(ComposerDraftOwner::Kind::Task));QCOMPARE(owner.machine,QString("mac"));QCOMPARE(owner.session,QString("codex/api"));
+    MessageComposer composer;composer.setTaskMode(true);composer.setSendLabel("Backlog");composer.setPlaceholder("Task prompt for orchard / codex/api…");
+    composer.resize(600,160);composer.show();
+    composer.setSessionKey("task-draft\n\ncodex/api");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.setAvailability(true);
+    QVERIFY(composer.toolbar()->isHidden());
+    for(const char *name:{"attachMessageFile","sessionModelSettings","interruptAgent"})QVERIFY2(composer.findChild<QPushButton *>(name)->isHidden(),name);
+    auto *send=composer.findChild<QPushButton *>("sendMessage");QCOMPARE(send->text(),QString("Backlog"));QVERIFY(!send->isEnabled());
+    QCOMPARE(composer.editor()->placeholderText(),QString("Task prompt for orchard / codex/api…"));
+    QVERIFY(!composer.canAttachFiles());QVERIFY(!composer.addAttachment("a.txt","text/plain","x"));
+    QSignalSpy requested(&composer,&MessageComposer::sendRequested);
+    composer.editor()->setFocus();QTest::keyClicks(composer.editor(),"Add rate limiting");QVERIFY(send->isEnabled());
+    QTest::keyClick(composer.editor(),Qt::Key_Return);
+    QTRY_COMPARE(requested.size(),1);QCOMPARE(requested[0][1].toString(),QString("Add rate limiting"));
+}
+
 void TestMessageComposer::initTestCase()
 {
     QVERIFY(m_settings.isValid());
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_messagecomposer -j 8`
Expected: FAIL to compile — `Task` is not a member of `ComposerDraftOwner::Kind`; `setTaskMode`, `setSendLabel`, `setPlaceholder` are not members of `MessageComposer`.

- [ ] **Step 3: Implement**

Apply this change to `tray/src/ComposerDraftStore.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/ComposerDraftStore.cpp b/tray/src/ComposerDraftStore.cpp
index 50dcadc..536bb80 100644
--- a/tray/src/ComposerDraftStore.cpp
+++ b/tray/src/ComposerDraftStore.cpp
@@ -246,9 +246,11 @@ QHash<QString, ComposerDraftSummary> ComposerDraftStore::indexDirect() const
 ComposerDraftOwner ComposerDraftStore::owner(const QString &key)
 {
     static const QString Voice = QStringLiteral("voice-message-draft\n"), Question = QStringLiteral("question\n");
-    if (key.startsWith(Voice)) {
-        auto result = owner(key.mid(Voice.size()));
-        if (result.kind == ComposerDraftOwner::Kind::Message) { result.kind = ComposerDraftOwner::Kind::Voice; return result; }
+    static const QString Task = QStringLiteral("task-draft\n");
+    for (const auto &[prefix, kind] : {std::pair{Voice, ComposerDraftOwner::Kind::Voice}, std::pair{Task, ComposerDraftOwner::Kind::Task}}) {
+        if (!key.startsWith(prefix)) continue;
+        auto result = owner(key.mid(prefix.size()));
+        if (result.kind == ComposerDraftOwner::Kind::Message) { result.kind = kind; return result; }
     }
     if (key.startsWith(Question)) {
         // A question identity is a compact JSON array, so it holds no line break.
````

Apply this change to `tray/src/ComposerDraftStore.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/ComposerDraftStore.h b/tray/src/ComposerDraftStore.h
index 8bbf8e1..4671fa3 100644
--- a/tray/src/ComposerDraftStore.h
+++ b/tray/src/ComposerDraftStore.h
@@ -25,10 +25,10 @@ struct ComposerDraftSummary {
     int attachments = 0;
 };
 
-// The conversation a stored draft belongs to. Message, subagent, question-answer
-// and voice drafts share one store and name their session differently.
+// The conversation a stored draft belongs to. Message, subagent, question-answer,
+// voice and task-prompt drafts share one store and name their session differently.
 struct ComposerDraftOwner {
-    enum class Kind { Unknown, Message, Subagent, Question, Voice };
+    enum class Kind { Unknown, Message, Subagent, Question, Voice, Task };
     Kind kind = Kind::Unknown;
     // The host alias, empty for this computer; a subagent key holds the machine
     // name instead. Archive is set for an archived session.
````

Apply this change to `tray/src/MessageComposer.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/MessageComposer.cpp b/tray/src/MessageComposer.cpp
index dbd8c0a..e9d7a82 100644
--- a/tray/src/MessageComposer.cpp
+++ b/tray/src/MessageComposer.cpp
@@ -412,9 +412,22 @@ QString MessageComposer::suggestion() const { return isWaitingToSend(m_key) ? QS
 void MessageComposer::updatePlaceholder()
 {
     const QString text = suggestion();
-    m_editor->setPlaceholderText(text.isEmpty() ? tr("Message this agent…") : text);
+    m_editor->setPlaceholderText(!text.isEmpty() ? text : !m_placeholder.isEmpty() ? m_placeholder : tr("Message this agent…"));
 }
 
+void MessageComposer::setTaskMode(bool taskMode)
+{
+    m_taskMode = taskMode;
+    m_toolbar->setVisible(!taskMode);
+    m_editor->setAccessibleName(taskMode ? tr("Task prompt") : tr("Message to selected agent"));
+    setAttachmentAvailability(!taskMode, taskMode ? tr("A task is text only.") : QString());
+    updateControls();
+}
+
+void MessageComposer::setSendLabel(const QString &label) { m_sendLabel = label; updateControls(); }
+
+void MessageComposer::setPlaceholder(const QString &placeholder) { m_placeholder = placeholder; updatePlaceholder(); }
+
 bool MessageComposer::requestInterrupt()
 {
     if (m_key.isEmpty() || m_stop->isHidden() || !m_stop->isEnabled()) return false;
@@ -870,8 +883,11 @@ void MessageComposer::updateControls()
     m_attach->setToolTip(m_attachmentsAvailable?tr("Attach files, paste an image, or drop files onto this session"):m_attachmentsReason);
     for (auto *button : m_attachmentList->findChildren<QPushButton *>("removeAttachment")) button->setEnabled(!locked);
     m_send->setEnabled(!m_key.isEmpty() && m_available && hasContent && (draft.attachments.isEmpty()||m_attachmentsAvailable) && !tooLong && !locked && !draft.uncertain);
-    m_send->setText(draft.sending ? tr("Sending…") : waiting ? tr("Waiting to send…") : tr("Send"));
-    m_send->setToolTip(waiting ? m_waitingSends.value(m_key) : tr("Send: Enter; new line: Shift+Enter"));
+    m_attach->setVisible(!m_taskMode);
+    m_send->setText(draft.sending ? (m_taskMode ? tr("Adding…") : tr("Sending…")) : waiting ? tr("Waiting to send…")
+        : m_taskMode ? m_sendLabel : tr("Send"));
+    m_send->setToolTip(waiting ? m_waitingSends.value(m_key) : m_taskMode ? tr("Add the task: Enter; new line: Shift+Enter")
+        : tr("Send: Enter; new line: Shift+Enter"));
     m_retry->setVisible(draft.uncertain && !operationLocked);
     m_refreshStatus->setVisible(!m_sessionStatus.isEmpty() && m_sessionStatusError);
     m_refreshStatus->setEnabled(!operationLocked);
@@ -897,7 +913,7 @@ void MessageComposer::updateControls()
     const bool showSettings = !m_narrow || status.isEmpty();
     const auto focus = QApplication::focusWidget();
     const bool returnFocus = !showSettings && (m_settings->hasFocus() || (focus && m_settingsPopup->isAncestorOf(focus)));
-    m_settings->setVisible(showSettings);
+    m_settings->setVisible(showSettings && !m_taskMode);
     if (!showSettings) m_settingsPopup->hide();
     // Do not transfer an unfinished settings interaction to the Stop action.
     if (returnFocus) m_editor->setFocus(Qt::OtherFocusReason);
@@ -946,6 +962,7 @@ void MessageComposer::showSavedDrafts()
                 if (owner.kind == ComposerDraftOwner::Kind::Subagent) title << tr("subagent");
                 else if (owner.kind == ComposerDraftOwner::Kind::Question) title << tr("question answer");
                 else if (owner.kind == ComposerDraftOwner::Kind::Voice) title << tr("voice message");
+                else if (owner.kind == ComposerDraftOwner::Kind::Task) title << tr("task prompt");
                 if (!owner.archive.isEmpty()) title << tr("archived");
             }
             if (summary.modified.isValid()) detail << QLocale().toString(summary.modified, QLocale::ShortFormat);
@@ -967,6 +984,7 @@ void MessageComposer::showSavedDrafts()
             if(owner.kind==ComposerDraftOwner::Kind::Subagent){context<<tr("Subagent");item->setData(SessionRoles::Title,item->data(SessionRoles::Title).toString()+" / "+owner.subagent);}
             else if(owner.kind==ComposerDraftOwner::Kind::Question)context<<tr("Question answer");
             else if(owner.kind==ComposerDraftOwner::Kind::Voice)context<<tr("Voice message");
+            else if(owner.kind==ComposerDraftOwner::Kind::Task)context<<tr("Task prompt");
             if(!owner.archive.isEmpty())context<<tr("Archived");
             if(summary.attachments&&!summary.preview.isEmpty())context<<tr("%n attachment(s)",nullptr,summary.attachments);
             if(!context.isEmpty()){const auto meta=item->data(SessionRoles::Meta).toString();if(!meta.isEmpty())context.prepend(meta);item->setData(SessionRoles::Meta,context.join(" / "));}
````

Apply this change to `tray/src/MessageComposer.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/MessageComposer.h b/tray/src/MessageComposer.h
index 1960a25..5df39d8 100644
--- a/tray/src/MessageComposer.h
+++ b/tray/src/MessageComposer.h
@@ -100,6 +100,12 @@ public:
     // Send and push now (message.push) in this field; off by default. Only the
     // main message field pushes: a subagent message bypasses the session's queue.
     void setPushEnabled(bool enabled);
+    // The Tasks tab's input: text only, no toolbar, model settings or Stop; the
+    // send button carries the stage a new task goes to. Drafts keep their keys.
+    void setTaskMode(bool taskMode);
+    void setSendLabel(const QString &label);
+    // Shown in an empty field when the agent has no suggestion.
+    void setPlaceholder(const QString &placeholder);
 
 signals:
     void sessionRefreshRequested(const QString &sessionKey);
@@ -193,4 +199,6 @@ private:
     // Composer-scope commands (Shortcuts.h) on the field.
     QAction *m_sendAction, *m_pushAction;
     bool m_pushEnabled = false, m_pushing = false;
+    bool m_taskMode = false;
+    QString m_sendLabel, m_placeholder;
 };
````

- [ ] **Step 4: Run them to see them pass**

Run: `cmake --build .ci-build/gui --target test_messagecomposer -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_messagecomposer`
Expected: all pass (`74 passed, 1 skipped` on the reference machine), including `taskModeAddsTextOnlyWithTheStageButton`.

- [ ] **Step 5: Commit**

```bash
git add tray/src/ComposerDraftStore.cpp \
  tray/src/ComposerDraftStore.h \
  tray/src/MessageComposer.cpp \
  tray/src/MessageComposer.h \
  tray/tests/test_messagecomposer.cpp
git commit -m "Let the message input file task prompts"
```

---

### Task 4: The Tasks tab

**Files:**
- Create: `tray/tests/support/FakeTasksCli.h`, `tray/tests/test_tasksview.cpp`, `tray/src/TaskRow.cpp`, `tray/src/TaskRow.h`, `tray/src/TasksView.cpp`, `tray/src/TasksView.h`
- Modify: `tray/tests/CMakeLists.txt`, `tray/CMakeLists.txt`, `tray/src/WorkspaceIcons.h`

**Interfaces:**
- Consumes: `TaskBoard`, `TaskAction`, `HgsClient::requestTask`, `MessageComposer` task mode (Tasks 2–3).
- Produces: `struct TaskSession { QString host, name, status, blocker; bool running = true; }`; `TasksView(HgsClient *, QWidget * = nullptr)` with `setContext(projectId, projectName, host, session, sessionKey, sessions)`, `setTheme(bool)`, `setPolling(bool)`, `setRefreshIntervals(int visibleMs, int hiddenMs)`, `refresh()`, `board()`, `selectedStage()`, `composer()`, signals `countsChanged(int review, int todo)` and `notice(const QString &, bool error)`; `TaskTabBadge` with `setCounts(int review, int todo)` and `setTheme(bool)`; `TaskRow`, `TaskColors`, `TaskRowState`. objectNames: `taskTile:<stage>`, `taskOnlySession`, `taskSettings`, `taskRow:<id>`, `taskGlyph`, `taskMeta`, `taskText`, `taskAction:<edit|toBacklog|top|sendNow|toTodo|cancel|backToTodo|accept>`, `taskEmpty`, `tasksError`, `showOlderTasks`, `taskComposer`, `taskTabBadge`.

Every command names the task by UUID and the project by catalog ID (`… --project <id>`), so renames never break it. Send now goes to the pinned session, or to the open session for an Any session task; it is disabled with the reason when that session is gone, not running, waiting for input or on another computer. The input files a task pinned to the open session, in To do or, while Backlog is open, in Backlog, and keeps its key only while the tab shows. The task settings, cancel and project-settings dialogs come in Task 5; until then Edit, Cancel, a row click and the gear do nothing.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/CMakeLists.txt b/tray/tests/CMakeLists.txt
index 6c771d9..c3de7ff 100644
--- a/tray/tests/CMakeLists.txt
+++ b/tray/tests/CMakeLists.txt
@@ -242,6 +242,13 @@ target_link_libraries(test_swarmcontroller PRIVATE Qt6::Test Qt6::Widgets hgs-tr
 zerus_add_test(swarmcontroller test_swarmcontroller)
 set_tests_properties(swarmcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
 
+# The Tasks tab over a fake `hgs task` (support/FakeTasksCli.h).
+add_executable(test_tasksview test_tasksview.cpp)
+target_include_directories(test_tasksview PRIVATE ../src)
+target_link_libraries(test_tasksview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
+zerus_add_test(tasksview test_tasksview)
+set_tests_properties(tasksview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
+
 add_executable(test_processview test_processview.cpp)
 target_include_directories(test_processview PRIVATE ../src)
 target_link_libraries(test_processview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
````

Create `tray/tests/support/FakeTasksCli.h`:

````cpp
#pragma once
// A fake `hgs task …` over a JSON file beside the script (tasks.json): enough of
// the real command's replies for the Tasks tab. Every call is appended to
// calls.jsonl as {"argv", "stdin"}. A file named "fail" holding a subcommand
// (list, add, send, …) makes that subcommand fail with "hgs refused: <name>".
#include "FakeCli.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace FakeTasksCli {
inline const char *script() {
    return R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
here=pathlib.Path(__file__).resolve().parent
a=sys.argv[1:]
text=sys.stdin.read()
with open(here/'calls.jsonl','a') as f: f.write(json.dumps({'argv':a,'stdin':text})+'\n')
fail=here/'fail'
if a[:1]!=['task']: print('unexpected command',file=sys.stderr); sys.exit(2)
if fail.exists() and a[1]==fail.read_text().strip(): print('hgs refused: '+a[1],file=sys.stderr); sys.exit(1)
state=here/'tasks.json'
data=json.loads(state.read_text()) if state.exists() else {}
data.setdefault('tasks',[]); data.setdefault('next',1)
data.setdefault('settings',{'task_template':'[Zerus task {ref}]\n{prompt}','cancel_template':'[Zerus task {ref} cancelled]','agents_other':True,'agent_limit':0})
project={'id':'project-1','name':'orchard'}
now=int(time.time()*1000)
def opt(k): return a[a.index(k)+1] if k in a else None
def find(u): return next(t for t in data['tasks'] if t['uuid']==u or t['id']==u)
def save(): state.write_text(json.dumps(data))
def out(o): print(json.dumps(o))
def log(t,kind,body=''): t.setdefault('log',[]).append({'kind':kind,'actor':{'kind':'person'},'at':now,'text':body})
def top(t): data['tasks'].remove(t); data['tasks'].insert(0,t)
order=['review','active','todo','backlog','done']
cmd=a[1]
if cmd=='list':
    tasks=sorted(data['tasks'],key=lambda t: order.index(t['stage']))
    out({'project':project,'tasks':[{k:v for k,v in t.items() if k!='log'} for t in tasks],'damaged':[]}); sys.exit(0)
if cmd=='settings':
    if '--set' in a: data['settings'].update(json.loads(text)); save()
    out({'project':project,'settings':data['settings']}); sys.exit(0)
if cmd=='add':
    n=data['next']; data['next']=n+1
    target={'kind':'session','machine':'m-'+(opt('--host') or 'here'),'session':opt('--target'),'host':opt('--host') or ''} if opt('--target') else {'kind':'any'}
    t={'uuid':'uuid-%d'%n,'id':'T-%06d'%n,'ref':'orchard/T-%06d'%n,'text':text,'stage':'backlog' if '--backlog' in a else 'todo',
       'summary':'','cancel_reason':'','cancelled_at':None,'created_at':now,'stage_at':now,'writable':True,
       'author':{'kind':'person'},'target':target,'session':None,'rank':'V'}
    log(t,'created'); data['tasks'].append(t); save(); out({'task':t}); sys.exit(0)
t=find(a[2])
if cmd=='get': out({'project':project,'task':t}); sys.exit(0)
if cmd=='stage':
    t['stage']=a[3]; t['stage_at']=now; log(t,'stage',a[3])
    if a[3] in ('todo','backlog'): top(t)
elif cmd=='move': top(t)
elif cmd=='send':
    session=opt('--session') or t['target']['session']
    t['stage']='active'; t['stage_at']=now; t['session']={'machine':'m-here','session':session,'host':''}; log(t,'sent')
elif cmd=='cancel':
    t['stage']='review'; t['stage_at']=now; t['cancelled_at']=now; t['cancel_reason']=opt('--reason') or ''; log(t,'cancelled')
elif cmd=='edit': t['text']=text; log(t,'edited')
elif cmd=='target':
    t['target']={'kind':'any'} if a[3]=='any' else {'kind':'session','machine':'m-'+(opt('--host') or 'here'),'session':a[3],'host':opt('--host') or ''}
    log(t,'target',a[3])
elif cmd=='comment': log(t,'comment',text)
else: print('unknown task command '+cmd,file=sys.stderr); sys.exit(2)
save(); out({'task':t})
)PY";
}

// Installs the fake as dir/hgs, with `tasks` as its starting store.
inline bool install(const QDir &dir, const QJsonArray &tasks = {}) {
    QFile state(dir.filePath("tasks.json"));
    if (!state.open(QIODevice::WriteOnly)) return false;
    state.write(QJsonDocument(QJsonObject{{"tasks", tasks}, {"next", int(tasks.size()) + 1}}).toJson(QJsonDocument::Compact));
    state.close();
    return FakeCli::install(dir.filePath("hgs"), script());
}

// A task view as `hgs task list` prints it. Extra fields replace the defaults.
inline QJsonObject task(int n, const QString &stage, const QJsonObject &extra = {}) {
    const QString id = QString("T-%1").arg(n, 6, 10, QLatin1Char('0'));
    QJsonObject t{{"uuid", QString("uuid-%1").arg(n)}, {"id", id}, {"ref", "orchard/" + id}, {"text", QString("Task %1").arg(n)},
                  {"stage", stage}, {"summary", ""}, {"cancel_reason", ""}, {"cancelled_at", QJsonValue()},
                  {"created_at", 1}, {"stage_at", 1}, {"writable", true}, {"author", QJsonObject{{"kind", "person"}}},
                  {"target", QJsonObject{{"kind", "any"}}}, {"session", QJsonValue()}, {"rank", "V"}};
    for (auto it = extra.begin(); it != extra.end(); ++it) t[it.key()] = it.value();
    return t;
}

inline QJsonObject pinned(const QString &session, const QString &host = {}) {
    return {{"kind", "session"}, {"machine", "m-" + (host.isEmpty() ? QString("here") : host)}, {"session", session}, {"host", host}};
}

// The calls so far, as their argv lists.
inline QList<QStringList> calls(const QDir &dir) {
    QFile file(dir.filePath("calls.jsonl")); QList<QStringList> out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    while (!file.atEnd()) {
        QStringList argv;
        for (const auto &value : QJsonDocument::fromJson(file.readLine()).object().value("argv").toArray()) argv << value.toString();
        out << argv;
    }
    return out;
}

inline QStringList stdins(const QDir &dir) {
    QFile file(dir.filePath("calls.jsonl")); QStringList out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    while (!file.atEnd()) out << QJsonDocument::fromJson(file.readLine()).object().value("stdin").toString();
    return out;
}
}
````

Create `tray/tests/test_tasksview.cpp`:

````cpp
#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "MessageComposer.h"
#include "TaskBoard.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

namespace {
const qint64 Hour = 60LL * 60 * 1000;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QList<TaskSession> sessions() {
    return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""},
            {"orchard-mac", "codex/ios", "Ready", ""}, {"", "codex/stopped", "Stopped", "The agent is not running. Resume it before sending."}};
}
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
QToolButton *action(QWidget *row, const QString &name) { return row ? row->findChild<QToolButton *>("taskAction:" + name) : nullptr; }
QStringList visibleRows(TasksView &view) {
    QStringList out;
    for (auto *w : view.findChildren<QWidget *>()) if (w->objectName().startsWith("taskRow:") && w->isVisibleTo(&view)) out << w->objectName().mid(8);
    std::sort(out.begin(), out.end(), [&](const QString &a, const QString &b) {
        return view.findChild<QWidget *>("taskRow:" + a)->y() < view.findChild<QWidget *>("taskRow:" + b)->y(); });
    return out;
}
}

class TestTasksView : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase();
    void init() { QSettings().remove("tasks"); }
    void tilesCountAndChooseTheFirstStage();
    void rowsShowTheQueueAndTheOpenSessionsTasks();
    void actionsRunHgsAndRefresh();
    void sendNowNamesWhyASessionCannotTakeIt();
    void theInputFilesATaskForTheOpenSession();
    void onlyThisSessionFiltersAndIsRemembered();
    void doneShowsTheLastWeekAndOlderOnRequest();
    void aFailedListingIsShownInsteadOfTasks();
    void aSessionOutsideProjectsExplainsWhy();
    void theTabBadgeCountsReviewAndTodo();
};

void TestTasksView::initTestCase() {
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("tasks-view");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestTasksView::tilesCountAndChooseTheFirstStage() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}, {"summary", "Fixed."}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    QSignalSpy counts(&view, &TasksView::countsChanged);
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2);
    QCOMPARE(tile(view, "review")->property("count").toInt(), 1);
    // In review opens first while it has tasks.
    QVERIFY(tile(view, "review")->isChecked()); QCOMPARE(view.selectedStage(), QString("review"));
    QCOMPARE(visibleRows(view), QStringList({"T-000003"}));
    QVERIFY(!counts.isEmpty()); QCOMPARE(counts.last().value(0).toInt(), 1); QCOMPARE(counts.last().value(1).toInt(), 2);
    QCOMPARE(FakeTasksCli::calls(dir.path()).first(), QStringList({"task", "list", "--project", "project-1"}));
    tile(view, "todo")->click(); QCOMPARE(visibleRows(view), QStringList({"T-000001", "T-000002"}));
    QVERIFY(view.findChild<QToolButton *>("taskOnlySession")); QVERIFY(view.findChild<QToolButton *>("taskSettings"));
    const auto preview = qEnvironmentVariable("HGS_TASKS_PREVIEW");
    if (!preview.isEmpty()) for (const bool dark : {true, false}) {
        view.setTheme(dark); QTest::qWait(50); QVERIFY(view.grab().save(preview + (dark ? "-dark.png" : "-light.png")));
    }
}

void TestTasksView::rowsShowTheQueueAndTheOpenSessionsTasks() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}, {"text", "Add rate limiting to POST /v1/orders"}}),
        FakeTasksCli::task(3, "todo"),
        FakeTasksCli::task(4, "active", {{"session", FakeTasksCli::pinned("codex/web")}, {"author", QJsonObject{{"kind", "agent"}, {"machine", "m"}, {"session", "codex/web"}}}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_VERIFY(row(view, 2)); tile(view, "todo")->click();
    QCOMPARE(row(view, 1)->findChild<QLabel *>("taskGlyph")->text(), QString("1"));
    QCOMPARE(row(view, 2)->findChild<QLabel *>("taskGlyph")->text(), QString("2"));
    // The open session's own task is highlighted; the others are not.
    QVERIFY(row(view, 2)->property("mine").toBool()); QVERIFY(!row(view, 1)->property("mine").toBool());
    QVERIFY(row(view, 2)->findChild<QLabel *>("taskText")->text().startsWith("Add rate limiting"));
    QVERIFY(row(view, 1)->findChild<QLabel *>("taskMeta")->text().contains("codex/web"));
    QVERIFY(row(view, 3)->findChild<QLabel *>("taskMeta")->text().contains("any session"));
    for (const char *name : {"edit", "toBacklog", "top", "sendNow"}) QVERIFY2(action(row(view, 1), name), name);
    tile(view, "active")->click();
    const auto meta = row(view, 4)->findChild<QLabel *>("taskMeta")->text();
    QVERIFY2(meta.contains("Working") && meta.contains("filed by codex/web"), qPrintable(meta));
    QVERIFY(action(row(view, 4), "cancel"));
}

void TestTasksView::actionsRunHgsAndRefresh() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"), FakeTasksCli::task(2, "todo"),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 2); tile(view, "todo")->click();
    action(row(view, 2), "top")->click();
    QTRY_COMPARE(visibleRows(view), QStringList({"T-000002", "T-000001"}));
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "move", "uuid-2", "--top", "--project", "project-1"})));
    action(row(view, 1), "toBacklog")->click();
    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 1);
    // An Any session task goes to the open session.
    action(row(view, 2), "sendNow")->click();
    QTRY_COMPARE(tile(view, "active")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "send", "uuid-2", "--session", "codex/api", "--project", "project-1"})));
    tile(view, "review")->click(); action(row(view, 3), "accept")->click();
    QTRY_COMPARE(tile(view, "done")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "stage", "uuid-3", "done", "--project", "project-1"})));
    // A refused command is reported and changes nothing.
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("stage"); fail.close();
    QSignalSpy notices(&view, &TasksView::notice);
    tile(view, "done")->click(); action(row(view, 3), "backToTodo")->click();
    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: stage")); QVERIFY(notices[0][1].toBool());
    QCOMPARE(tile(view, "done")->property("count").toInt(), 1);
}

void TestTasksView::sendNowNamesWhyASessionCannotTakeIt() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {
        FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/stopped")}}),
        FakeTasksCli::task(3, "todo", {{"target", FakeTasksCli::pinned("codex/ios", "orchard-mac")}}),
        FakeTasksCli::task(4, "todo", {{"target", FakeTasksCli::pinned("codex/web")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_VERIFY(row(view, 4)); tile(view, "todo")->click();
    const auto blocked = [&](int n) { auto *b = action(row(view, n), "sendNow"); return b->isEnabled() ? QString() : b->toolTip(); };
    QCOMPARE(blocked(1), QString("codex/gone no longer exists — choose another session"));
    QCOMPARE(blocked(2), QString("The agent is not running. Resume it before sending."));
    QCOMPARE(blocked(3), QString("codex/ios runs on another computer; sending there comes with replication"));
    QCOMPARE(blocked(4), QString());
}

void TestTasksView::theInputFilesATaskForTheOpenSession() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path()));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    auto *composer = view.composer(); auto *send = composer->findChild<QPushButton *>("sendMessage");
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QCOMPARE(composer->sessionKey(), QString("task-draft\n\ncodex/api")); QCOMPARE(send->text(), QString("To do"));
    composer->editor()->setFocus(); QTest::keyClicks(composer->editor(), "Add rate limiting");
    QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 1);
    QTRY_VERIFY(composer->editor()->toPlainText().isEmpty());
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "add", "--project", "project-1", "--target", "codex/api"})));
    QVERIFY(FakeTasksCli::stdins(dir.path()).contains("Add rate limiting"));
    // While Backlog is open, a new task goes there; a remote session names its computer.
    view.setContext("project-1", "orchard", "orchard-mac", "codex/ios", "orchard-mac\ncodex/ios", sessions());
    tile(view, "backlog")->click(); QCOMPARE(send->text(), QString("Backlog"));
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QTest::keyClicks(composer->editor(), "Ship the iOS build"); QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(tile(view, "backlog")->property("count").toInt(), 1);
    QVERIFY(FakeTasksCli::calls(dir.path()).contains(QStringList({"task", "add", "--project", "project-1", "--target", "codex/ios", "--host", "orchard-mac", "--backlog"})));
    // A refused add keeps the prompt and reports why.
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("add"); fail.close();
    QSignalSpy notices(&view, &TasksView::notice);
    QTest::keyClicks(composer->editor(), "Kept"); QTest::keyClick(composer->editor(), Qt::Key_Return);
    QTRY_COMPARE(notices.size(), 1); QCOMPARE(notices[0][0].toString(), QString("hgs refused: add"));
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("Kept"));
}

void TestTasksView::onlyThisSessionFiltersAndIsRemembered() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo", {{"target", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(2, "todo", {{"target", FakeTasksCli::pinned("codex/api")}}), FakeTasksCli::task(3, "todo")}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 3);
    auto *only = view.findChild<QToolButton *>("taskOnlySession"); only->click();
    QCOMPARE(tile(view, "todo")->property("count").toInt(), 2); QCOMPARE(visibleRows(view), QStringList({"T-000002", "T-000003"}));
    // Queue numbers stay the project's.
    QCOMPARE(row(view, 3)->findChild<QLabel *>("taskGlyph")->text(), QString("3"));
    QVERIFY(QSettings().value("tasks/onlySession").toBool());
    TasksView again(&client); again.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QVERIFY(again.findChild<QToolButton *>("taskOnlySession")->isChecked());
}

void TestTasksView::doneShowsTheLastWeekAndOlderOnRequest() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "done", {{"stage_at", now() - 2 * Hour}}),
        FakeTasksCli::task(2, "done", {{"stage_at", now() - 10 * 24 * Hour}}), FakeTasksCli::task(3, "done", {{"stage_at", now() - Hour}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "done")->property("count").toInt(), 2);
    tile(view, "done")->click(); QCOMPARE(visibleRows(view), QStringList({"T-000003", "T-000001"}));
    auto *older = view.findChild<QPushButton *>("showOlderTasks"); QVERIFY(older->isVisible()); QCOMPARE(older->text(), QString("Show older (1)"));
    older->click(); QCOMPARE(visibleRows(view), QStringList({"T-000003", "T-000001", "T-000002"})); QCOMPARE(older->text(), QString("Hide older"));
}

void TestTasksView::aFailedListingIsShownInsteadOfTasks() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo")}));
    QFile fail(dir.filePath("fail")); QVERIFY(fail.open(QIODevice::WriteOnly)); fail.write("list"); fail.close();
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    auto *error = view.findChild<QLabel *>("tasksError");
    QTRY_VERIFY(error->isVisible()); QVERIFY(error->text().contains("hgs refused: list"));
    QVERIFY(visibleRows(view).isEmpty());
}

void TestTasksView::aSessionOutsideProjectsExplainsWhy() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path()));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext({}, {}, "", "codex/api", "\ncodex/api", sessions());
    auto *empty = view.findChild<QLabel *>("taskEmpty");
    QVERIFY(empty->isVisible()); QVERIFY(empty->text().startsWith("Tasks belong to a project"));
    QVERIFY(!view.composer()->findChild<QPushButton *>("sendMessage")->isEnabled());
    QTest::qWait(100); QVERIFY(FakeTasksCli::calls(dir.path()).isEmpty());
}

void TestTasksView::theTabBadgeCountsReviewAndTodo() {
    QWidget host; TaskTabBadge badge(&host); host.show();
    QVERIFY(!badge.isVisible());
    badge.setCounts(1, 12); QVERIFY(badge.isVisible());
    QCOMPARE(badge.property("review").toInt(), 1); QCOMPARE(badge.property("todo").toInt(), 12);
    QCOMPARE(badge.accessibleName(), QString("1 to review, 12 to do"));
    QVERIFY(badge.sizeHint().width() > badge.sizeHint().height());
    badge.setCounts(0, 0); QVERIFY(!badge.isVisible());
}

QTEST_MAIN(TestTasksView)
#include "test_tasksview.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S tray -B .ci-build/gui >/dev/null && cmake --build .ci-build/gui --target test_tasksview -j 8`
Expected: FAIL to compile — `TasksView.h: No such file or directory`.

- [ ] **Step 3: Implement**

Apply this change to `tray/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/CMakeLists.txt b/tray/CMakeLists.txt
index b3c44b2..72dfbe3 100644
--- a/tray/CMakeLists.txt
+++ b/tray/CMakeLists.txt
@@ -145,7 +145,7 @@ add_library(hgs-tray-ui STATIC src/SessionOrganization.cpp src/SessionList.cpp s
     src/MachineFilter.cpp src/ProjectFilter.cpp src/SessionSearch.cpp src/MachinesPage.cpp src/AccountsPage.cpp
     src/ComposerToolbar.cpp src/MessageComposer.cpp src/QuestionCard.cpp src/DirectoryDialog.cpp
     src/NewSessionDialog.cpp src/ProjectsDialog.cpp src/WorktreesDialog.cpp src/SwarmDialog.cpp src/SwarmConflictReview.cpp
-    src/ProcessView.cpp src/TrayMenu.cpp src/IconFactory.cpp src/Shortcuts.cpp src/DownloadCacheSettings.cpp src/GoToDialog.cpp
+    src/ProcessView.cpp src/TaskRow.cpp src/TasksView.cpp src/TrayMenu.cpp src/IconFactory.cpp src/Shortcuts.cpp src/DownloadCacheSettings.cpp src/GoToDialog.cpp
     src/KeyboardSettings.cpp src/ShortcutsDialog.cpp src/AutomaticApprovalSettings.cpp src/PhonePairing.cpp
     # Tray icon decorator (QuietTrayIcon.h): portable, but wrapped around the icon
     # only on macOS; makeTrayIcon() in TrayAgent.cpp explains why not on Linux.
````

Create `tray/src/TaskRow.cpp`:

````cpp
#include "TaskRow.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

TaskColors TaskColors::of(bool dark) {
    if (dark) return {QColor("#e8edf4"), QColor("#9eabba"), QColor("#242d36"), QColor("#34404a"), QColor("#8bdfc0"),
                      QColor("#2b3540"), QColor("#edbd77"), QColor("#98bdeb"), QColor("#c5a8f5")};
    return {QColor("#1a2733"), QColor("#657487"), QColor("#f3f6f8"), QColor("#dbe3e9"), QColor("#167357"),
            QColor("#edf2f5"), QColor("#9b6216"), QColor("#366ba9"), QColor("#6c43b8")};
}

QColor TaskColors::stage(const QString &stage) const {
    if (stage == QLatin1String("review")) return warn;
    if (stage == QLatin1String("active")) return accent;
    if (stage == QLatin1String("todo")) return blue;
    if (stage == QLatin1String("done")) return violet;
    return muted;
}

namespace {
QString sessionLabel(const QString &host, const QString &name) {
    return host.isEmpty() ? name : name + QStringLiteral(" @") + host;
}
QString age(qint64 ms) {
    const qint64 minutes = qMax<qint64>(0, ms / 60000);
    if (minutes < 60) return QStringLiteral("%1 min").arg(minutes);
    if (minutes < 24 * 60) return QStringLiteral("%1 h").arg(minutes / 60);
    return QStringLiteral("%1 d").arg(minutes / (24 * 60));
}
// The text as at most `lines` lines of `width`, the last one elided.
QString clamp(const QString &text, const QFont &font, int width, int lines) {
    if (width <= 0) return text;
    QTextLayout layout(text, font); layout.beginLayout();
    QStringList out;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        if (int(out.size()) == lines - 1) {
            const QString rest = text.mid(line.textStart()).simplified();
            out << QFontMetrics(font).elidedText(rest, Qt::ElideRight, width); break;
        }
        out << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return out.join(QLatin1Char('\n'));
}
}

QString TaskRow::actionKey(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QStringLiteral("edit");
    case TaskAction::ToBacklog: return QStringLiteral("toBacklog");
    case TaskAction::Top: return QStringLiteral("top");
    case TaskAction::SendNow: return QStringLiteral("sendNow");
    case TaskAction::ToTodo: return QStringLiteral("toTodo");
    case TaskAction::Cancel: return QStringLiteral("cancel");
    case TaskAction::BackToTodo: return QStringLiteral("backToTodo");
    case TaskAction::Accept: return QStringLiteral("accept");
    }
    return {};
}

TaskRow::TaskRow(QWidget *parent) : QWidget(parent), m_colors(TaskColors::of(true)) {
    setAttribute(Qt::WA_Hover); setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);   // rows never take the list's spare height
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(12, 6, 12, 10); layout->setSpacing(11);
    m_glyph = new QLabel; m_glyph->setObjectName("taskGlyph"); m_glyph->setFixedSize(18, 18); m_glyph->setAlignment(Qt::AlignCenter);
    auto *glyphColumn = new QVBoxLayout; glyphColumn->setContentsMargins(0, 4, 0, 0); glyphColumn->addWidget(m_glyph); glyphColumn->addStretch();
    layout->addLayout(glyphColumn);
    auto *main = new QVBoxLayout; main->setContentsMargins(0, 0, 0, 0); main->setSpacing(0); layout->addLayout(main, 1);
    // The header's height is fixed, so showing the icons never moves anything.
    auto *header = new QWidget; header->setFixedHeight(26);
    auto *headerLayout = new QHBoxLayout(header); headerLayout->setContentsMargins(0, 0, 0, 0); headerLayout->setSpacing(8);
    m_meta = new QLabel; m_meta->setObjectName("taskMeta"); m_meta->setTextFormat(Qt::RichText);
    m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_actions = new QWidget; m_actions->setObjectName("taskActions");
    auto *actionsLayout = new QHBoxLayout(m_actions); actionsLayout->setContentsMargins(0, 0, 0, 0); actionsLayout->setSpacing(2);
    headerLayout->addWidget(m_meta, 1); headerLayout->addWidget(m_actions);
    main->addWidget(header);
    m_text = new QLabel; m_text->setObjectName("taskText"); m_text->setTextFormat(Qt::PlainText);
    m_text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    main->addWidget(m_text);
    m_summary = new QLabel; m_summary->setObjectName("taskSummary"); m_summary->setTextFormat(Qt::RichText); m_summary->setWordWrap(true);
    main->addSpacing(2); main->addWidget(m_summary); m_summary->hide();
}

void TaskRow::setTask(const TaskItem &task, const TaskRowState &state, const TaskColors &colors, qint64 now) {
    const bool rebuild = task.stage != m_task.stage || m_buttons.isEmpty();
    m_task = task; m_state = state; m_colors = colors;
    setProperty("mine", state.mine);
    const QColor stage = colors.stage(task.stage);
    const QString glyph = task.stage == QLatin1String("todo") ? QString::number(task.queue)
        : task.stage == QLatin1String("review") || task.stage == QLatin1String("done") ? QStringLiteral("✓")
        : task.stage == QLatin1String("active") ? QStringLiteral("•") : QString();
    m_glyph->setText(glyph);
    setWorkspaceStyle(m_glyph, QStringLiteral("border:2px solid %1;border-radius:9px;color:%1;font-size:9.5px;font-weight:700;").arg(stage.name()));
    // Meta: ID, the session (or the target), the session's status for In
    // progress, the time, and who filed it.
    const auto who = [&](const QString &text) {
        return state.mine ? QStringLiteral("<span style='color:%1;font-weight:600'>%2</span>").arg(colors.accent.name(), text.toHtmlEscaped())
                          : text.toHtmlEscaped();
    };
    QStringList meta{QStringLiteral("<span style='font-family:monospace;font-size:10.5px'>%1</span>").arg(task.id.toHtmlEscaped())};
    if (task.stage == QLatin1String("todo") || task.stage == QLatin1String("backlog")) {
        meta << (task.targetSession.isEmpty()
            ? QStringLiteral("→ any session") + (state.anyNow.isEmpty() ? QString() : QStringLiteral(" · now ") + state.anyNow.toHtmlEscaped())
            : who(QStringLiteral("→ ") + sessionLabel(task.targetHost, task.targetSession)));
    } else if (!task.sessionName.isEmpty()) {
        meta << who(sessionLabel(task.sessionHost, task.sessionName));
    }
    if (task.stage == QLatin1String("active")) {
        if (!state.sessionStatus.isEmpty()) meta << state.sessionStatus.toHtmlEscaped();
        meta << age(now - task.stageAt);
    }
    if (task.stage == QLatin1String("done") && now - task.stageAt > TaskBoard::RecentDoneMs)
        meta << QStringLiteral("accepted ") + QDateTime::fromMSecsSinceEpoch(task.stageAt).toString(QStringLiteral("d MMM"));
    if (!task.author.isEmpty())
        meta << QStringLiteral("<span style='color:%1'>filed by %2</span>").arg(colors.violet.name(), task.author.toHtmlEscaped());
    m_meta->setText(meta.join(QStringLiteral("&nbsp;&nbsp;")));
    setWorkspaceStyle(m_meta, QStringLiteral("color:%1;font-size:11px;").arg(colors.muted.name()));
    const bool quiet = task.stage == QLatin1String("backlog") || task.stage == QLatin1String("done");
    setWorkspaceStyle(m_text, QStringLiteral("color:%1;").arg((quiet ? colors.muted : colors.fg).name()));
    // In review: the agent's latest report, or the cancellation.
    QString summary;
    if (task.stage == QLatin1String("review")) {
        if (task.cancelledAt > 0)
            summary = QStringLiteral("<b>Cancelled at %1%2</b> %3").arg(QDateTime::fromMSecsSinceEpoch(task.cancelledAt).toString(QStringLiteral("HH:mm")),
                task.cancelReason.isEmpty() ? QString() : QStringLiteral(":"), task.cancelReason.toHtmlEscaped());
        else if (!task.summary.isEmpty())
            summary = QStringLiteral("<b>%1 reported:</b> %2").arg(sessionLabel(task.sessionHost, task.sessionName).toHtmlEscaped(), task.summary.toHtmlEscaped());
    }
    m_summary->setText(summary); m_summary->setVisible(!summary.isEmpty());
    setWorkspaceStyle(m_summary, QStringLiteral("color:%1;border-left:2px solid %2;padding:1px 0 1px 10px;").arg(colors.muted.name(), stage.name()));
    if (rebuild) {
        qDeleteAll(m_buttons); m_buttons.clear();
        const auto actions = TaskBoard::actions(task.stage);
        for (const auto action : actions) {
            auto *button = new QToolButton(m_actions); button->setObjectName(QStringLiteral("taskAction:") + actionKey(action));
            button->setFixedSize(26, 26); button->setIconSize(QSize(15, 15)); button->setAutoRaise(true); button->setCursor(Qt::ArrowCursor);
            connect(button, &QToolButton::clicked, this, [this, action] { emit actionRequested(action); });
            m_actions->layout()->addWidget(button); m_buttons.insert(int(action), button);
        }
        m_actions->setFixedWidth(int(actions.size()) * 26 + qMax(0, int(actions.size()) - 1) * 2);
    }
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        const auto action = TaskAction(it.key()); const QString why = state.blocked.value(it.key());
        it.value()->setIcon(workspaceIcon(TaskBoard::actionIcon(action), colors.muted));
        it.value()->setEnabled(why.isEmpty()); it.value()->setToolTip(why.isEmpty() ? TaskBoard::actionLabel(action) : why);
        it.value()->setAccessibleName(TaskBoard::actionLabel(action));
        setWorkspaceStyle(it.value(), QStringLiteral("QToolButton{border:0;border-radius:6px;background:transparent;}QToolButton:hover{background:%1;}").arg(colors.hover.name()));
    }
    showActions(m_hovered);
    layoutText(); update();
}

void TaskRow::showActions(bool shown) {
    // The container keeps its width, so the meta never moves.
    for (auto *button : std::as_const(m_buttons)) button->setVisible(shown);
}

void TaskRow::layoutText() {
    m_text->setText(clamp(m_task.text, m_text->font(), m_text->width() > 0 ? m_text->width() : width() - 60, 2));
}

void TaskRow::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); layoutText(); }

void TaskRow::enterEvent(QEnterEvent *event) { QWidget::enterEvent(event); m_hovered = true; showActions(true); update(); }

void TaskRow::leaveEvent(QEvent *event) { QWidget::leaveEvent(event); m_hovered = false; showActions(false); update(); }

void TaskRow::mouseReleaseEvent(QMouseEvent *event) {
    QWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit opened();
}

void TaskRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QColor tint = m_state.mine ? m_colors.accent : m_colors.fg;
    tint.setAlphaF(m_state.mine ? (m_hovered ? 0.11 : 0.07) : (m_hovered ? 0.04 : 0));
    p.fillRect(rect(), tint);
    if (m_state.mine) { p.setPen(Qt::NoPen); p.setBrush(m_colors.accent); p.drawRoundedRect(QRectF(0, 6, 3, height() - 12), 1.5, 1.5); }
    if (property("separated").toBool()) { p.setPen(QPen(m_colors.border, 1)); p.drawLine(41, 0, width(), 0); }
}
````

Create `tray/src/TaskRow.h`:

````cpp
#pragma once
#include "TaskBoard.h"
#include <QColor>
#include <QHash>
#include <QWidget>

class QLabel;
class QToolButton;

// The Tasks tab's colours, from design/task-tracker/index.html.
struct TaskColors {
    QColor fg, muted, surface, border, accent, hover, warn, blue, violet;
    static TaskColors of(bool dark);
    QColor stage(const QString &stage) const;
};

// What a row shows beyond the task itself.
struct TaskRowState {
    bool mine = false;               // the open session's task: accent tint and bar
    QString sessionStatus;           // In progress: the session's status label
    QHash<int, QString> blocked;     // TaskAction → why it is unavailable now
    QString anyNow;                  // To do and Backlog: the session an Any task goes to now
};

// One task in the list: a fixed-height header with the meta on the left and the
// stage's action icons on the right (shown on hover), then the text clamped to
// two lines, and for In review the report or the cancellation.
class TaskRow : public QWidget {
    Q_OBJECT
public:
    explicit TaskRow(QWidget *parent = nullptr);
    void setTask(const TaskItem &task, const TaskRowState &state, const TaskColors &colors, qint64 now);
    const TaskItem &task() const { return m_task; }
    static QString actionKey(TaskAction action);
signals:
    void actionRequested(TaskAction action);
    void opened();
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void showActions(bool shown);
    void layoutText();
    TaskItem m_task;
    TaskRowState m_state;
    TaskColors m_colors;
    QLabel *m_glyph, *m_meta, *m_text, *m_summary;
    QWidget *m_actions;
    QHash<int, QToolButton *> m_buttons;
    bool m_hovered = false;
};
````

Create `tray/src/TasksView.cpp`:

````cpp
#include "TasksView.h"
#include "HgsClient.h"
#include "MessageComposer.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QColor mix(const QColor &color, double share, const QColor &base) {
    return QColor::fromRgbF(base.redF() + (color.redF() - base.redF()) * share, base.greenF() + (color.greenF() - base.greenF()) * share,
                            base.blueF() + (color.blueF() - base.blueF()) * share);
}
QColor alpha(QColor color, double a) { color.setAlphaF(a); return color; }
QString sessionLabel(const QString &host, const QString &name) { return host.isEmpty() ? name : name + QStringLiteral(" @") + host; }
}

// A stage tile: its icon on a tinted circle, its name and its count.
class TaskTile : public QAbstractButton {
public:
    TaskTile(const QString &stage, QWidget *parent) : QAbstractButton(parent), m_stage(stage), m_colors(TaskColors::of(true)) {
        setObjectName(QStringLiteral("taskTile:") + stage); setCheckable(true); setFixedHeight(34); setMinimumWidth(72);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setCursor(Qt::PointingHandCursor); setAttribute(Qt::WA_Hover);
        setToolTip(TaskBoard::stageLabel(stage)); setCount(0);
    }
    void setCount(int count) {
        setProperty("count", count); setAccessibleName(QStringLiteral("%1, %2").arg(TaskBoard::stageLabel(m_stage)).arg(count)); update();
    }
    void setColors(const TaskColors &colors) { m_colors = colors; update(); }
    QSize sizeHint() const override { return {120, 34}; }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QColor color = m_colors.stage(m_stage);
        const QColor fill = isChecked() ? mix(color, 0.17, m_colors.surface) : m_colors.surface;
        const QColor edge = isChecked() ? alpha(color, 0.45) : underMouse() ? m_colors.border : Qt::transparent;
        p.setPen(edge.alpha() ? QPen(edge, 1) : Qt::NoPen); p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
        p.setPen(Qt::NoPen); p.setBrush(alpha(color, 0.18)); p.drawEllipse(QRectF(10, 6, 22, 22));
        workspaceIcon(TaskBoard::stageIcon(m_stage), color).paint(&p, QRect(14, 10, 14, 14));
        QFont count = font(); count.setPixelSize(15); count.setBold(true); p.setFont(count); p.setPen(m_colors.fg);
        const QString number = QString::number(property("count").toInt());
        const int numberWidth = QFontMetrics(count).horizontalAdvance(number);
        p.drawText(QRect(width() - 10 - numberWidth, 0, numberWidth, height()), Qt::AlignVCenter, number);
        QFont label = font(); label.setPixelSize(12); label.setWeight(QFont::DemiBold); p.setFont(label);
        p.setPen(isChecked() ? m_colors.fg : m_colors.muted);
        const QRect text(38, 0, qMax(0, width() - 38 - numberWidth - 16), height());
        p.drawText(text, Qt::AlignVCenter, QFontMetrics(label).elidedText(TaskBoard::stageLabel(m_stage), Qt::ElideRight, text.width()));
    }
private:
    QString m_stage;
    TaskColors m_colors;
};

TaskTabBadge::TaskTabBadge(QWidget *parent) : QWidget(parent) { setObjectName("taskTabBadge"); setCounts(0, 0); }

void TaskTabBadge::setCounts(int review, int todo) {
    m_review = review; m_todo = todo; setProperty("review", review); setProperty("todo", todo);
    setAccessibleName(tr("%1 to review, %2 to do").arg(review).arg(todo));
    setVisible(review > 0 || todo > 0); updateGeometry(); update();
}

void TaskTabBadge::setTheme(bool dark) { m_dark = dark; update(); }

QSize TaskTabBadge::sizeHint() const {
    QFont small = font(); small.setPixelSize(10); small.setBold(true); const QFontMetrics metrics(small);
    int width = 0;
    for (const int count : {m_review, m_todo}) if (count) width += qMax(16, metrics.horizontalAdvance(QString::number(count)) + 10) + 4;
    return {qMax(0, width), 16};
}

void TaskTabBadge::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    QFont small = font(); small.setPixelSize(10); small.setBold(true); p.setFont(small); const QFontMetrics metrics(small);
    int x = 0;
    const auto pill = [&](int count, const QColor &fill, const QColor &text) {
        if (!count) return;
        const QString number = QString::number(count); const int width = qMax(16, metrics.horizontalAdvance(number) + 10);
        p.setPen(Qt::NoPen); p.setBrush(fill); p.drawRoundedRect(QRectF(x, (height() - 16) / 2.0, width, 16), 8, 8);
        p.setPen(text); p.drawText(QRect(x, (height() - 16) / 2, width, 16), Qt::AlignCenter, number); x += width + 4;
    };
    pill(m_review, QColor(m_dark ? "#ffda76" : "#f4ce65"), QColor("#392900"));
    pill(m_todo, QColor(m_dark ? "#303942" : "#e5ebef"), QColor(m_dark ? "#a2adbc" : "#627082"));
}

TasksView::TasksView(HgsClient *client, QWidget *parent) : QWidget(parent), m_client(client) {
    setObjectName("tasksView");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16, 12, 16, 0); layout->setSpacing(0);
    auto *tiles = new QHBoxLayout; tiles->setSpacing(8);
    for (const auto &stage : TaskBoard::stages()) {
        auto *tile = new TaskTile(stage, this); m_tiles.insert(stage, tile); tiles->addWidget(tile, 1);
        connect(tile, &QAbstractButton::clicked, this, [this, stage] { selectStage(stage); });
    }
    const auto square = [this](const QString &name) {
        auto *button = new QToolButton(this); button->setObjectName(name); button->setFixedSize(34, 34); button->setIconSize(QSize(17, 17));
        button->setCursor(Qt::PointingHandCursor); return button;
    };
    m_only = square("taskOnlySession"); m_only->setCheckable(true);
    m_only->setChecked(QSettings().value("tasks/onlySession").toBool());
    connect(m_only, &QToolButton::toggled, this, [this](bool only) {
        QSettings().setValue("tasks/onlySession", only);
        const auto colors = TaskColors::of(m_dark); m_only->setIcon(workspaceIcon(QStringLiteral("task-funnel"), only ? colors.accent : colors.muted));
        render();
    });
    m_settings = square("taskSettings");
    connect(m_settings, &QToolButton::clicked, this, &TasksView::openSettings);
    tiles->addWidget(m_only); tiles->addWidget(m_settings);
    layout->addLayout(tiles); layout->addSpacing(14);
    m_errorLabel = new QLabel; m_errorLabel->setObjectName("tasksError"); m_errorLabel->setWordWrap(true); m_errorLabel->setTextFormat(Qt::PlainText);
    m_errorLabel->hide(); layout->addWidget(m_errorLabel);
    m_scroll = new QScrollArea; m_scroll->setObjectName("taskList"); m_scroll->setWidgetResizable(true); m_scroll->setFrameShape(QFrame::NoFrame);
    // The list's surface holds only its rows; the spare height below stays the page's.
    auto *page = new QWidget; page->setObjectName("taskListPage");
    auto *pageLayout = new QVBoxLayout(page); pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
    m_listHost = new QWidget; m_listHost->setObjectName("taskListHost"); pageLayout->addWidget(m_listHost); pageLayout->addStretch();
    m_list = new QVBoxLayout(m_listHost); m_list->setContentsMargins(0, 0, 0, 0); m_list->setSpacing(0);
    m_empty = new QLabel; m_empty->setObjectName("taskEmpty"); m_empty->setAlignment(Qt::AlignCenter); m_empty->setMargin(18);
    m_list->addWidget(m_empty);
    m_scroll->setWidget(page); layout->addWidget(m_scroll, 1);
    m_older = new QPushButton; m_older->setObjectName("showOlderTasks"); m_older->setFlat(true); m_older->setCursor(Qt::PointingHandCursor); m_older->hide();
    connect(m_older, &QPushButton::clicked, this, [this] { m_showOlder = !m_showOlder; render(); });
    layout->addWidget(m_older, 0, Qt::AlignLeft);
    m_composer = new MessageComposer(this); m_composer->setObjectName("taskComposer"); m_composer->setTaskMode(true);
    connect(m_composer, &MessageComposer::sendRequested, this, [this](const QString &key, const QString &text) { addTask(key, text); });
    layout->addWidget(m_composer);
    connect(m_client, &HgsClient::taskReady, this, [this](quint64 id, const QJsonObject &result) {
        const auto pending = m_pending.take(id); if (pending.first) pending.first(result);
    });
    connect(m_client, &HgsClient::taskFailed, this, [this](quint64 id, const QString &error) {
        if (!m_pending.contains(id)) return;
        const auto pending = m_pending.take(id); if (pending.second) pending.second(error);
    });
    m_timer.setSingleShot(true); connect(&m_timer, &QTimer::timeout, this, &TasksView::refresh);
    setTheme(true);
}

void TasksView::setContext(const QString &projectId, const QString &projectName, const QString &host, const QString &session,
                           const QString &sessionKey, const QList<TaskSession> &sessions) {
    const bool project = projectId != m_projectId;
    m_projectName = projectName; m_host = host; m_session = session; m_sessions = sessions;
    if (project) {
        m_projectId = projectId; ++m_generation; m_board = {}; m_loaded = false; m_error.clear(); m_showOlder = false; m_stage = QStringLiteral("todo");
        for (auto *row : std::as_const(m_rows)) { row->hide(); row->setObjectName({}); row->deleteLater(); }
        m_rows.clear();
    }
    m_sessionKey = sessionKey; applyComposerKey();
    m_composer->setAvailability(!m_projectId.isEmpty() && !m_session.isEmpty(), tr("Open a session to add tasks for it."));
    render();
    if (project) refresh();
}

void TasksView::setPolling(bool polling) {
    m_polling = polling;
    if (!polling) m_timer.stop(); else schedule();
}

void TasksView::setRefreshIntervals(int visibleMs, int hiddenMs) { m_visibleMs = visibleMs; m_hiddenMs = hiddenMs; schedule(); }

void TasksView::showEvent(QShowEvent *event) { QWidget::showEvent(event); applyComposerKey(); if (m_loaded) refresh(); }

void TasksView::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); applyComposerKey(); schedule(); }

// The input holds the open session's task draft only while the tab shows: a hidden
// tab keeps no enabled field in the window.
void TasksView::applyComposerKey() {
    const QString key = isVisible() && !m_sessionKey.isEmpty() ? QStringLiteral("task-draft\n") + m_sessionKey : QString();
    if (key != m_composer->sessionKey()) m_composer->setSessionKey(key);
}

void TasksView::schedule() {
    if (m_polling && !m_projectId.isEmpty() && !m_listing) m_timer.start(isVisible() ? m_visibleMs : m_hiddenMs);
}

QStringList TasksView::withProject(QStringList arguments) const { return arguments << QStringLiteral("--project") << m_projectId; }

void TasksView::request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                        std::function<void(const QString &)> failed) {
    if (!failed) failed = [this](const QString &error) { emit notice(error, true); refresh(); };
    m_pending.insert(m_client->requestTask(arguments, input), {std::move(done), std::move(failed)});
}

void TasksView::refresh() {
    m_timer.stop();
    if (m_projectId.isEmpty() || m_listing) return;
    const auto generation = m_generation; m_listing = generation + 1;
    request({QStringLiteral("list"), QStringLiteral("--project"), m_projectId}, {},
        [this, generation](const QJsonObject &listing) { m_listing = 0; received(listing, generation); },
        [this, generation](const QString &error) {
            m_listing = 0;
            if (generation != m_generation) return refresh();
            m_error = error; render(); schedule();
        });
}

void TasksView::received(const QJsonObject &listing, quint64 generation) {
    if (generation != m_generation) return refresh();   // the project changed meanwhile
    m_error.clear(); m_board = TaskBoard::fromListing(listing);
    if (!m_loaded) { m_loaded = true; m_stage = m_board.count("review", view()) ? QStringLiteral("review") : QStringLiteral("todo"); }
    render();
    int review = 0, todo = 0;
    for (const auto &task : std::as_const(m_board.tasks)) { review += task.stage == QLatin1String("review"); todo += task.stage == QLatin1String("todo"); }
    emit countsChanged(review, todo);
    schedule();
}

TaskBoard::View TasksView::view() const { return {m_host, m_session, m_only->isChecked(), QDateTime::currentMSecsSinceEpoch()}; }

const TaskSession *TasksView::session(const QString &host, const QString &name) const {
    for (const auto &session : m_sessions) if (session.host == host && session.name == name) return &session;
    return nullptr;
}

// Why Send now cannot paste into the session the task would go to, as hgs would say it.
QString TasksView::sendBlocker(const TaskItem &task) const {
    const bool any = task.targetSession.isEmpty();
    const QString host = any ? m_host : task.targetHost, name = any ? m_session : task.targetSession;
    if (name.isEmpty()) return tr("Open the session to send this task to");
    if (!any && !task.targetKnown) return tr("%1 is on a computer this one has not verified").arg(name);
    const auto *found = session(host, name);
    if (!found) return tr("%1 no longer exists — choose another session").arg(name);
    if (!host.isEmpty()) return tr("%1 runs on another computer; sending there comes with replication").arg(name);
    return found->blocker;
}

void TasksView::selectStage(const QString &stage) {
    if (stage != m_stage) m_showOlder = false;
    m_stage = stage; render();
}

void TasksView::render() {
    const auto v = view(); const auto colors = TaskColors::of(m_dark);
    for (const auto &stage : TaskBoard::stages()) {
        auto *tile = m_tiles.value(stage); tile->setCount(m_board.count(stage, v)); tile->setChecked(stage == m_stage);
    }
    m_errorLabel->setText(tr("Tasks could not be read: %1").arg(m_error)); m_errorLabel->setVisible(!m_error.isEmpty());
    const bool done = m_stage == QLatin1String("done");
    QList<TaskItem> rows = m_error.isEmpty() ? m_board.rows(m_stage, v) : QList<TaskItem>();
    const auto older = done && m_error.isEmpty() ? m_board.rows(m_stage, v, true) : QList<TaskItem>();
    if (m_showOlder) rows += older;
    QSet<QString> shown; int index = 0;
    for (const auto &task : std::as_const(rows)) {
        auto *row = m_rows.value(task.uuid);
        if (!row) {
            row = new TaskRow(m_listHost); m_rows.insert(task.uuid, row);
            connect(row, &TaskRow::actionRequested, this, [this, row](TaskAction action) { act(row->task(), action); });
            connect(row, &TaskRow::opened, this, [this, row] { openTask(row->task()); });
        }
        row->setObjectName(QStringLiteral("taskRow:") + task.id);
        TaskRowState state; state.mine = TaskBoard::belongsTo(task, m_host, m_session); state.anyNow = sessionLabel(m_host, m_session);
        if (const auto *ran = session(task.sessionHost, task.sessionName); ran && task.stage == QLatin1String("active")) state.sessionStatus = ran->status;
        if (TaskBoard::actions(task.stage).contains(TaskAction::SendNow))
            if (const auto why = sendBlocker(task); !why.isEmpty()) state.blocked.insert(int(TaskAction::SendNow), why);
        if (!task.writable)
            for (const auto action : TaskBoard::actions(task.stage)) if (action != TaskAction::Edit) state.blocked.insert(int(action), tr("Update Zerus to edit this task"));
        row->setProperty("separated", index > 0);
        row->setTask(task, state, colors, v.now);
        m_list->removeWidget(row); m_list->insertWidget(index++, row); row->show(); shown.insert(task.uuid);
    }
    for (auto it = m_rows.begin(); it != m_rows.end();) {
        if (shown.contains(it.key())) { ++it; continue; }
        it.value()->hide(); it.value()->setObjectName({}); it.value()->deleteLater(); it = m_rows.erase(it);
    }
    const int hidden = m_board.rows(m_stage, {m_host, m_session, false, v.now}).size() - m_board.rows(m_stage, v).size();
    const QString empty = v.only && hidden > 0 ? tr("No tasks of %1 here").arg(sessionLabel(m_host, m_session))
        : m_stage == QLatin1String("review") ? tr("Nothing to review") : m_stage == QLatin1String("active") ? tr("No session is running a task")
        : m_stage == QLatin1String("todo") ? tr("Empty — write a prompt in the input below") : m_stage == QLatin1String("backlog") ? tr("Empty")
        : tr("Nothing yet");
    m_empty->setText(m_projectId.isEmpty() && !m_session.isEmpty() ? tr("Tasks belong to a project. Move this session into one to keep tasks for it.") : empty);
    m_empty->setVisible(rows.isEmpty() && m_error.isEmpty() && (!m_projectId.isEmpty() || !m_session.isEmpty()));
    m_older->setText(m_showOlder ? tr("Hide older") : tr("Show older (%1)").arg(older.size())); m_older->setVisible(!older.isEmpty());
    m_only->setToolTip(m_only->isChecked() ? tr("Show the tasks of every session of %1").arg(m_projectName)
                                           : tr("Show only the tasks of %1").arg(sessionLabel(m_host, m_session)));
    m_only->setAccessibleName(m_only->toolTip());
    m_settings->setToolTip(tr("Task settings for %1").arg(m_projectName)); m_settings->setAccessibleName(m_settings->toolTip());
    updateComposer();
}

void TasksView::updateComposer() {
    const QString target = m_stage == QLatin1String("backlog") ? tr("Backlog") : tr("To do");
    const QString session = sessionLabel(m_host, m_session);
    m_composer->setSendLabel(target);
    m_composer->setPlaceholder(m_session.isEmpty() ? tr("Task prompt…") : tr("Task prompt for %1 / %2…").arg(m_projectName, session));
    m_composer->setSessionStatus(m_session.isEmpty() ? QString() : tr("Task for %1 · Enter adds to %2 · Shift+Enter for a new line").arg(session, target));
}

QList<TasksView::Step> TasksView::steps(const TaskItem &task, TaskAction action) const {
    switch (action) {
    case TaskAction::ToBacklog: return {{withProject({"stage", task.uuid, "backlog"}), {}}};
    case TaskAction::Top: return {{withProject({"move", task.uuid, "--top"}), {}}};
    case TaskAction::ToTodo: case TaskAction::BackToTodo: return {{withProject({"stage", task.uuid, "todo"}), {}}};
    case TaskAction::Accept: return {{withProject({"stage", task.uuid, "done"}), {}}};
    case TaskAction::SendNow: {
        // An Any session task goes to the open session.
        QStringList arguments{"send", task.uuid};
        if (task.targetSession.isEmpty()) arguments << "--session" << m_session;
        return {{withProject(arguments), {}}};
    }
    case TaskAction::Edit: case TaskAction::Cancel: break;
    }
    return {};
}

void TasksView::run(QList<Step> steps) {
    if (steps.isEmpty()) return refresh();
    auto step = steps.takeFirst();
    request(step.arguments, step.input, [this, steps, then = step.then](const QJsonObject &reply) {
        if (then) then(reply);
        run(steps);
    });
}

void TasksView::act(const TaskItem &task, TaskAction action) {
    if (action == TaskAction::Edit) return openTask(task);
    if (action == TaskAction::Cancel) return confirmCancel(task);
    if (action == TaskAction::SendNow && !sendBlocker(task).isEmpty()) return;
    run(steps(task, action));
}

void TasksView::addTask(const QString &key, const QString &text) {
    if (key != m_composer->sessionKey() || m_projectId.isEmpty() || m_session.isEmpty()) return;
    const QString stage = m_stage == QLatin1String("backlog") ? QStringLiteral("backlog") : QStringLiteral("todo");
    QStringList arguments{"add", "--project", m_projectId, "--target", m_session};
    if (!m_host.isEmpty()) arguments << "--host" << m_host;
    if (stage == QLatin1String("backlog")) arguments << "--backlog";
    m_composer->prepareSendingAsync(key, false, [this, key, arguments, text, stage](bool ready) {
        if (!ready) return;
        request(arguments, text,
            [this, key, stage](const QJsonObject &) { m_composer->deliveryFinished(key, true); selectStage(stage); refresh(); },
            [this, key](const QString &error) { m_composer->deliveryFinished(key, false, error); emit notice(error, true); });
    });
}

// The task settings, the cancel confirmation and the project settings come with
// their dialogs (TaskDialogs.h).
void TasksView::openTask(const TaskItem &) {}

void TasksView::confirmCancel(const TaskItem &) {}

void TasksView::openSettings() {}

void TasksView::setTheme(bool dark) {
    m_dark = dark; const auto colors = TaskColors::of(dark);
    for (auto *tile : std::as_const(m_tiles)) tile->setColors(colors);
    for (auto *button : {m_only, m_settings}) {
        const bool on = button->isChecked();
        button->setIcon(workspaceIcon(button == m_only ? QStringLiteral("task-funnel") : QStringLiteral("settings"), on ? colors.accent : colors.muted));
        setWorkspaceStyle(button, QStringLiteral("QToolButton{border:1px solid %1;border-radius:9px;background:transparent;}"
            "QToolButton:hover{border-color:%2;}QToolButton:checked{border-color:%3;background:%4;}")
            .arg(colors.border.name(), colors.muted.name(), alpha(colors.accent, 0.5).name(QColor::HexArgb), alpha(colors.accent, 0.12).name(QColor::HexArgb)));
    }
    setWorkspaceStyle(m_scroll, QStringLiteral("QScrollArea,#taskListPage{background:transparent;}") + workspaceScrollbars(dark));
    setWorkspaceStyle(m_listHost, QStringLiteral("#taskListHost{background:%1;border-radius:10px;}").arg(colors.surface.name()));
    setWorkspaceStyle(m_empty, QStringLiteral("color:%1;").arg(colors.muted.name()));
    setWorkspaceStyle(m_errorLabel, QStringLiteral("color:%1;padding:0 0 10px 0;").arg(dark ? "#f4ab9b" : "#a13224"));
    setWorkspaceStyle(m_older, QStringLiteral("QPushButton{border:0;color:%1;padding:6px 2px;font-size:12px;}").arg(colors.accent.name()));
    m_composer->setTheme(dark);
    render();
}
````

Create `tray/src/TasksView.h`:

````cpp
#pragma once
#include "TaskBoard.h"
#include "TaskRow.h"
#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

class HgsClient;
class MessageComposer;
class QAbstractButton;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class TaskTile;

// A session of the open project as the Tasks tab sees it.
struct TaskSession {
    QString host, name;   // host: "" for this computer, otherwise its SSH alias
    QString status;       // its status label, such as "Working"
    QString blocker;      // why it takes no prompt now; empty when it does
    bool running = true;  // whether its agent runs, for the cancel dialog
};

// The Tasks tab label's counts: In review as an attention badge, then To do.
class TaskTabBadge : public QWidget {
    Q_OBJECT
public:
    explicit TaskTabBadge(QWidget *parent = nullptr);
    void setCounts(int review, int todo);
    void setTheme(bool dark);
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    int m_review = 0, m_todo = 0;
    bool m_dark = true;
};

// The Tasks tab: the open project's tasks in the local replica, read and changed
// through `hgs task` (docs/superpowers/specs/2026-10-13-project-task-queue-design.md,
// Desktop interface). Stage tiles above one list, and the task input below.
class TasksView : public QWidget {
    Q_OBJECT
public:
    explicit TasksView(HgsClient *client, QWidget *parent = nullptr);
    // The open session's project, the open session (its window key keeps the
    // input's draft) and the project's sessions.
    void setContext(const QString &projectId, const QString &projectName, const QString &host, const QString &session,
                    const QString &sessionKey, const QList<TaskSession> &sessions);
    void setTheme(bool dark);
    // While the window shows: refresh every visibleMs while the tab is shown,
    // every hiddenMs otherwise, so the tab label stays current.
    void setPolling(bool polling);
    void setRefreshIntervals(int visibleMs, int hiddenMs);
    void refresh();
    const TaskBoard &board() const { return m_board; }
    QString selectedStage() const { return m_stage; }
    MessageComposer *composer() const { return m_composer; }
signals:
    // The project's tasks in In review and in To do, for the tab label.
    void countsChanged(int review, int todo);
    void notice(const QString &text, bool error);
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                 std::function<void(const QString &)> failed = {});
    void received(const QJsonObject &listing, quint64 generation);
    void render();
    void selectStage(const QString &stage);
    // One `hgs task` call of a sequence; `then` sees its reply.
    struct Step { QStringList arguments; QString input; std::function<void(const QJsonObject &)> then = {}; };
    QList<Step> steps(const TaskItem &task, TaskAction action) const;
    // Runs the steps in order and stops at the first failure; refreshes after.
    void run(QList<Step> steps);
    void act(const TaskItem &task, TaskAction action);
    void openTask(const TaskItem &task);
    void confirmCancel(const TaskItem &task);
    void openSettings();
    void addTask(const QString &key, const QString &text);
    void updateComposer();
    void applyComposerKey();
    void schedule();
    QString sendBlocker(const TaskItem &task) const;
    const TaskSession *session(const QString &host, const QString &name) const;
    QStringList withProject(QStringList arguments) const;
    TaskBoard::View view() const;
    HgsClient *m_client;
    TaskBoard m_board;
    QString m_projectId, m_projectName, m_host, m_session, m_sessionKey, m_stage, m_error;
    QList<TaskSession> m_sessions;
    QHash<QString, TaskTile *> m_tiles;
    QToolButton *m_only, *m_settings;
    QLabel *m_errorLabel, *m_empty;
    QScrollArea *m_scroll;
    QWidget *m_listHost;
    QVBoxLayout *m_list;
    QHash<QString, TaskRow *> m_rows;
    QPushButton *m_older;
    MessageComposer *m_composer;
    QHash<quint64, std::pair<std::function<void(const QJsonObject &)>, std::function<void(const QString &)>>> m_pending;
    QTimer m_timer;
    quint64 m_generation = 0, m_listing = 0;
    int m_visibleMs = 2500, m_hiddenMs = 15000;
    bool m_polling = true, m_dark = true, m_showOlder = false, m_loaded = false;
};
````

Apply this change to `tray/src/WorkspaceIcons.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/WorkspaceIcons.h b/tray/src/WorkspaceIcons.h
index 3b64ae8..f1d19e5 100644
--- a/tray/src/WorkspaceIcons.h
+++ b/tray/src/WorkspaceIcons.h
@@ -201,6 +201,30 @@ inline QIcon workspaceIcon(const QString &name, const QColor &color)
             }
             else { p.drawLine(6, 9, 12, 15); p.drawLine(12, 15, 18, 9); }
         } else if (name == "more") { p.drawPoint(5, 12); p.drawPoint(12, 12); p.drawPoint(19, 12); }
+        // Project tasks (TasksView): a stage's icon is the icon of the action that moves a task there.
+        else if (name == "task-inbox") {
+            QPainterPath tray; tray.moveTo(4, 13.5); tray.lineTo(6.5, 6); tray.lineTo(17.5, 6); tray.lineTo(20, 13.5);
+            tray.lineTo(20, 19); tray.lineTo(4, 19); tray.closeSubpath(); p.drawPath(tray);
+            QPainterPath slot; slot.moveTo(4, 13.5); slot.lineTo(8.5, 13.5); slot.lineTo(10, 16); slot.lineTo(14, 16);
+            slot.lineTo(15.5, 13.5); slot.lineTo(20, 13.5); p.drawPath(slot);
+        } else if (name == "task-list") {
+            p.drawLine(4, 6, 20, 6); p.drawLine(4, 11, 20, 11); p.drawLine(4, 16, 14, 16);
+        } else if (name == "task-list-add") {
+            p.drawLine(4, 6, 16, 6); p.drawLine(4, 11, 16, 11); p.drawLine(4, 16, 11, 16);
+            p.drawLine(QPointF(17.5, 14), QPointF(17.5, 21)); p.drawLine(QPointF(14, 17.5), QPointF(21, 17.5));
+        } else if (name == "task-clock") {
+            p.drawEllipse(QPointF(12, 12), 8.5, 8.5); p.drawLine(QPointF(12, 7.5), QPointF(12, 12)); p.drawLine(12, 12, 15, 14);
+        } else if (name == "task-check") {
+            p.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawLine(5, 12, 10, 17); p.drawLine(10, 17, 19, 7);
+        } else if (name == "task-funnel") {
+            QPainterPath funnel; funnel.moveTo(4, 5); funnel.lineTo(20, 5); funnel.lineTo(14, 12.5); funnel.lineTo(14, 19);
+            funnel.lineTo(10, 17); funnel.lineTo(10, 12.5); funnel.closeSubpath(); p.drawPath(funnel);
+        } else if (name == "task-edit") {
+            QPainterPath pencil; pencil.moveTo(4, 20); pencil.lineTo(8, 20); pencil.lineTo(19, 9); pencil.lineTo(15, 5);
+            pencil.lineTo(4, 16); pencil.closeSubpath(); p.drawPath(pencil); p.drawLine(13, 7, 17, 11);
+        } else if (name == "task-top") {
+            p.drawLine(7, 12, 12, 7); p.drawLine(12, 7, 17, 12); p.drawLine(7, 18, 12, 13); p.drawLine(12, 13, 17, 18);
+        }
         p.end(); icon.addPixmap(pm);
     }
     cache->insert(key, icon);
````

- [ ] **Step 4: Run them to see them pass**

Run: `cmake -S tray -B .ci-build/gui >/dev/null && cmake --build .ci-build/gui --target test_tasksview -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview`
Expected: `Totals: 12 passed`.

- [ ] **Step 5: Look at it**

Run: `HGS_TASKS_PREVIEW=$(mktemp -d)/tasks QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview tilesCountAndChooseTheFirstStage`
Expected: it passes and writes `tasks-dark.png` and `tasks-light.png` beside the given path. Compare them with the Tasks page of `design/task-tracker/index.html`: five stage tiles and two square tiles, the list's surface only as tall as its rows, the input with the "To do" button.

- [ ] **Step 6: Commit**

```bash
git add tray/CMakeLists.txt \
  tray/src/TaskRow.cpp \
  tray/src/TaskRow.h \
  tray/src/TasksView.cpp \
  tray/src/TasksView.h \
  tray/src/WorkspaceIcons.h \
  tray/tests/CMakeLists.txt \
  tray/tests/support/FakeTasksCli.h \
  tray/tests/test_tasksview.cpp
git commit -m "Show project tasks in a Tasks tab"
```

---

### Task 5: The task dialogs

**Files:**
- Create: `tray/tests/test_taskdialogs.cpp`, `tray/src/TaskDialogs.cpp`, `tray/src/TaskDialogs.h`
- Modify: `tray/tests/CMakeLists.txt`, `tray/CMakeLists.txt`, `tray/src/TasksView.cpp`, `tray/src/TasksView.h`

**Interfaces:**
- Consumes: `TasksView`, `TaskSession`, `TaskRow::actionKey` (Task 4).
- Produces: `TaskTarget { bool any; QString host, session; }`, `TaskEdits { std::optional<QString> text; std::optional<TaskTarget> target; }`; `TaskDialog(task view with log, sessions, sendBlocker, dark, parent)` with signals `saveRequested(TaskEdits)`, `actionRequested(TaskEdits, TaskAction)`, `commentRequested(QString)` and `setHistory(QJsonArray)`, `clearComment()`; `TaskCancelDialog(TaskItem, sessionStatus, running, parent)` with `reason()`; `TaskSettingsDialog(projectName, settings, parent)` with `changes()`. objectNames: `taskDialog`, `taskPrompt`, `taskSession:any`, `taskSession:<host>/<session>`, `taskHistory`, `taskComment`, `addTaskComment`, `taskDialogAction:<key>`, `saveTask`, `discardTaskEdits`, `closeTask`, `taskDialogSession`, `taskDialogSummary`, `taskCancelDialog`, `taskCancelWarning`, `taskCancelReason`, `keepTaskRunning`, `confirmTaskCancel`, `taskSettingsDialog`, `taskTemplate`, `cancelTemplate`, `agentsOther`, `agentLimit`, `saveTaskSettings`, `cancelTaskSettings`.

A click on a row or Edit opens the task settings after `hgs task get`. Save applies the prompt (`edit`, stdin) and the session (`target`); an action first applies them, then meets the edited task (a task re-pinned before Send now goes to its new session). A comment is added in place and the history follows from the reply. Cancel asks first: "T-… moves to In review, and <session> gets the cancel message."; its reason is passed as `--reason` and becomes a comment, never pasted; a refused cancel message is reported, the task is cancelled all the same. The gear opens the project's task settings; Save sends only the changed fields to `hgs task settings --set`.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/CMakeLists.txt b/tray/tests/CMakeLists.txt
index c3de7ff..882c44b 100644
--- a/tray/tests/CMakeLists.txt
+++ b/tray/tests/CMakeLists.txt
@@ -249,6 +249,12 @@ target_link_libraries(test_tasksview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
 zerus_add_test(tasksview test_tasksview)
 set_tests_properties(tasksview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
 
+add_executable(test_taskdialogs test_taskdialogs.cpp)
+target_include_directories(test_taskdialogs PRIVATE ../src)
+target_link_libraries(test_taskdialogs PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
+zerus_add_test(taskdialogs test_taskdialogs)
+set_tests_properties(taskdialogs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
+
 add_executable(test_processview test_processview.cpp)
 target_include_directories(test_processview PRIVATE ../src)
 target_link_libraries(test_processview PRIVATE Qt6::Test Qt6::Widgets hgs-tray-ui)
````

Create `tray/tests/test_taskdialogs.cpp`:

````cpp
#include "FakeTasksCli.h"
#include "HgsClient.h"
#include "TaskDialogs.h"
#include "TasksView.h"
#include <QAbstractButton>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QToolButton>

namespace {
QList<TaskSession> sessions() {
    TaskSession stopped{"", "codex/stopped", "Stopped", "The agent is not running. Resume it before sending."}; stopped.running = false;
    return {{"", "codex/api", "Ready", ""}, {"", "codex/web", "Working", ""}, stopped};
}
QJsonObject withLog(QJsonObject task, const QJsonArray &log) { task["log"] = log; return task; }
QJsonObject entry(const QString &kind, const QString &text = {}, const QJsonObject &extra = {}) {
    QJsonObject e{{"kind", kind}, {"actor", QJsonObject{{"kind", "person"}}}, {"at", 1760000000000.0}, {"text", text}};
    for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
    return e;
}
QWidget *row(TasksView &view, int n) { return view.findChild<QWidget *>(QString("taskRow:T-%1").arg(n, 6, 10, QLatin1Char('0'))); }
QAbstractButton *tile(TasksView &view, const QString &stage) { return view.findChild<QAbstractButton *>("taskTile:" + stage); }
void click(QWidget *widget) { QTest::mouseClick(widget, Qt::LeftButton, {}, QPoint(widget->width() / 2, 8)); }
bool called(const QDir &dir, const QStringList &argv) { return FakeTasksCli::calls(dir).contains(argv); }
}

class TestTaskDialogs : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase();
    void theTaskDialogEditsAListedTaskAndShowsItsHistory();
    void aReviewTaskIsReadOnlyWithItsActions();
    void theCancelAndProjectSettingsDialogs();
    void theTabOpensTheDialogsAndAppliesThem();
};

void TestTaskDialogs::initTestCase() {
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("task-dialogs");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestTaskDialogs::theTaskDialogEditsAListedTaskAndShowsItsHistory() {
    const auto task = withLog(FakeTasksCli::task(7, "todo", {{"target", FakeTasksCli::pinned("codex/gone")}, {"text", "Old prompt"}}),
        {entry("created"), entry("sent", {}, {{"session", QJsonObject{{"session", "codex/api"}, {"host", ""}}}}), entry("comment", "Use the config value.")});
    TaskDialog dialog(task, sessions(), {}, true); dialog.show();
    auto *prompt = dialog.findChild<QPlainTextEdit *>("taskPrompt"); QVERIFY(!prompt->isReadOnly());
    // Any session, the project's sessions, and the pinned session that no longer exists.
    for (const char *name : {"taskSession:any", "taskSession:/codex/api", "taskSession:/codex/web", "taskSession:/codex/gone"})
        QVERIFY2(dialog.findChild<QPushButton *>(name), name);
    QVERIFY(dialog.findChild<QPushButton *>("taskSession:/codex/gone")->isChecked());
    const auto history = dialog.findChild<QTextBrowser *>("taskHistory")->toPlainText();
    QVERIFY2(history.contains("created the task") && history.contains("sent it to codex/api") && history.contains("commented: Use the config value."), qPrintable(history));
    for (const char *name : {"taskDialogAction:toBacklog", "taskDialogAction:top", "taskDialogAction:sendNow"}) QVERIFY2(dialog.findChild<QPushButton *>(name), name);
    QVERIFY(!dialog.findChild<QPushButton *>("taskDialogAction:edit"));
    QSignalSpy saved(&dialog, &TaskDialog::saveRequested);
    prompt->setPlainText("New prompt"); dialog.findChild<QPushButton *>("taskSession:any")->click();
    dialog.findChild<QPushButton *>("saveTask")->click();
    QCOMPARE(saved.size(), 1); const auto edits = saved[0][0].value<TaskEdits>();
    QCOMPARE(edits.text.value_or(QString()), QString("New prompt")); QVERIFY(edits.target && edits.target->any);
    QVERIFY(!dialog.isVisible());
}

void TestTaskDialogs::aReviewTaskIsReadOnlyWithItsActions() {
    const auto task = withLog(FakeTasksCli::task(8, "review", {{"session", FakeTasksCli::pinned("codex/api")}, {"summary", "Token bucket; 6 tests."}}), {});
    TaskDialog dialog(task, sessions(), {}, false); dialog.show();
    QVERIFY(dialog.findChild<QPlainTextEdit *>("taskPrompt")->isReadOnly());
    QVERIFY(!dialog.findChild<QPushButton *>("saveTask")); QVERIFY(dialog.findChild<QPushButton *>("closeTask"));
    QCOMPARE(dialog.findChild<QLabel *>("taskDialogSession")->text(), QString("codex/api"));
    QVERIFY(dialog.findChild<QLabel *>("taskDialogSummary")->text().contains("Token bucket; 6 tests."));
    QSignalSpy acted(&dialog, &TaskDialog::actionRequested);
    QVERIFY(dialog.findChild<QPushButton *>("taskDialogAction:backToTodo"));
    dialog.findChild<QPushButton *>("taskDialogAction:accept")->click();
    QCOMPARE(acted.size(), 1); QCOMPARE(acted[0][1].value<TaskAction>(), TaskAction::Accept);
    const auto edits = acted[0][0].value<TaskEdits>(); QVERIFY(!edits.text && !edits.target);
}

void TestTaskDialogs::theCancelAndProjectSettingsDialogs() {
    auto item = TaskBoard::item(FakeTasksCli::task(9, "active", {{"session", FakeTasksCli::pinned("codex/web")}}));
    TaskCancelDialog working(item, "Working", true);
    QCOMPARE(working.findChild<QLabel *>("taskCancelWarning")->text(),
             QString("T-000009 moves to In review, and codex/web gets the cancel message. codex/web is working; it reads the message when its turn ends."));
    QVERIFY(working.findChild<QPushButton *>("keepTaskRunning")->isDefault());
    working.findChild<QLineEdit *>("taskCancelReason")->setText("  Wrong approach  "); QCOMPARE(working.reason(), QString("Wrong approach"));
    TaskCancelDialog stopped(item, "Stopped", false);
    QVERIFY(stopped.findChild<QLabel *>("taskCancelWarning")->text().contains("The agent is not running, so hgs will refuse the message"));
    const QJsonObject settings{{"task_template", "[Zerus task {ref}]\n{prompt}"}, {"cancel_template", "[Zerus task {ref} cancelled]"},
                               {"agents_other", true}, {"agent_limit", 0}};
    TaskSettingsDialog dialog("orchard", settings);
    QCOMPARE(dialog.windowTitle(), QString("Project tasks · orchard")); QVERIFY(dialog.changes().isEmpty());
    QCOMPARE(dialog.findChild<QSpinBox *>("agentLimit")->text(), QString("no limit"));
    dialog.findChild<QSpinBox *>("agentLimit")->setValue(3); dialog.findChild<QCheckBox *>("agentsOther")->setChecked(false);
    QCOMPARE(dialog.changes(), QJsonObject({{"agent_limit", 3}, {"agents_other", false}}));
}

void TestTaskDialogs::theTabOpensTheDialogsAndAppliesThem() {
    QTemporaryDir dir; QVERIFY(FakeTasksCli::install(dir.path(), {FakeTasksCli::task(1, "todo"),
        FakeTasksCli::task(2, "active", {{"session", FakeTasksCli::pinned("codex/web")}}),
        FakeTasksCli::task(3, "review", {{"session", FakeTasksCli::pinned("codex/api")}})}));
    HgsClient client(FakeCli::program(dir.filePath("hgs"))); TasksView view(&client); view.resize(760, 560); view.show();
    view.setContext("project-1", "orchard", "", "codex/api", "\ncodex/api", sessions());
    QTRY_COMPARE(tile(view, "todo")->property("count").toInt(), 1); tile(view, "todo")->click();
    // A click on the row opens its settings; an action there first applies the edits.
    click(row(view, 1)); TaskDialog *dialog = nullptr; QTRY_VERIFY((dialog = view.findChild<TaskDialog *>("taskDialog")));
    QVERIFY(called(dir.path(), {"task", "get", "uuid-1", "--project", "project-1"}));
    dialog->findChild<QPlainTextEdit *>("taskPrompt")->setPlainText("Edited prompt");
    dialog->findChild<QPushButton *>("taskSession:/codex/web")->click();
    dialog->findChild<QPushButton *>("taskDialogAction:sendNow")->click();
    QTRY_COMPARE(tile(view, "active")->property("count").toInt(), 2);
    const auto calls = FakeTasksCli::calls(dir.path());
    const int edit = calls.indexOf(QStringList({"task", "edit", "uuid-1", "--project", "project-1"}));
    const int target = calls.indexOf(QStringList({"task", "target", "uuid-1", "codex/web", "--project", "project-1"}));
    const int send = calls.indexOf(QStringList({"task", "send", "uuid-1", "--project", "project-1"}));
    QVERIFY(edit >= 0 && edit < target && target < send);
    QVERIFY(FakeTasksCli::stdins(dir.path()).contains("Edited prompt"));
    // Cancel asks first; the reason goes to hgs as a comment, never to the agent.
    tile(view, "active")->click(); row(view, 2)->findChild<QToolButton *>("taskAction:cancel")->click();
    TaskCancelDialog *cancel = nullptr; QTRY_VERIFY((cancel = view.findChild<TaskCancelDialog *>("taskCancelDialog")));
    cancel->findChild<QLineEdit *>("taskCancelReason")->setText("Wrong approach");
    cancel->findChild<QPushButton *>("confirmTaskCancel")->click();
    QTRY_VERIFY(called(dir.path(), {"task", "cancel", "uuid-2", "--reason", "Wrong approach", "--project", "project-1"}));
    QTRY_COMPARE(tile(view, "review")->property("count").toInt(), 2);
    // A comment is added in place and the history follows.
    tile(view, "review")->click(); click(row(view, 3));
    QTRY_VERIFY((dialog = view.findChild<TaskDialog *>("taskDialog")) && dialog->isVisible());
    dialog->findChild<QLineEdit *>("taskComment")->setText("Looks good");
    dialog->findChild<QPushButton *>("addTaskComment")->click();
    QTRY_VERIFY(dialog->findChild<QTextBrowser *>("taskHistory")->toPlainText().contains("commented: Looks good"));
    QVERIFY(dialog->findChild<QLineEdit *>("taskComment")->text().isEmpty());
    QVERIFY(called(dir.path(), {"task", "comment", "uuid-3", "--project", "project-1"}));
    dialog->reject();
    // The gear opens the project's task settings; Save sends only what changed.
    view.findChild<QToolButton *>("taskSettings")->click();
    TaskSettingsDialog *settings = nullptr; QTRY_VERIFY((settings = view.findChild<TaskSettingsDialog *>("taskSettingsDialog")));
    settings->findChild<QSpinBox *>("agentLimit")->setValue(2); settings->findChild<QPushButton *>("saveTaskSettings")->click();
    QTRY_VERIFY(called(dir.path(), {"task", "settings", "--project", "project-1", "--set"}));
    QCOMPARE(QJsonDocument::fromJson(FakeTasksCli::stdins(dir.path()).last().toUtf8()).object(), QJsonObject({{"agent_limit", 2}}));
}

QTEST_MAIN(TestTaskDialogs)
#include "test_taskdialogs.moc"
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S tray -B .ci-build/gui >/dev/null && cmake --build .ci-build/gui --target test_taskdialogs -j 8`
Expected: FAIL to compile — `TaskDialogs.h: No such file or directory`.

- [ ] **Step 3: Implement**

Apply this change to `tray/CMakeLists.txt` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/CMakeLists.txt b/tray/CMakeLists.txt
index 72dfbe3..606df29 100644
--- a/tray/CMakeLists.txt
+++ b/tray/CMakeLists.txt
@@ -145,7 +145,7 @@ add_library(hgs-tray-ui STATIC src/SessionOrganization.cpp src/SessionList.cpp s
     src/MachineFilter.cpp src/ProjectFilter.cpp src/SessionSearch.cpp src/MachinesPage.cpp src/AccountsPage.cpp
     src/ComposerToolbar.cpp src/MessageComposer.cpp src/QuestionCard.cpp src/DirectoryDialog.cpp
     src/NewSessionDialog.cpp src/ProjectsDialog.cpp src/WorktreesDialog.cpp src/SwarmDialog.cpp src/SwarmConflictReview.cpp
-    src/ProcessView.cpp src/TaskRow.cpp src/TasksView.cpp src/TrayMenu.cpp src/IconFactory.cpp src/Shortcuts.cpp src/DownloadCacheSettings.cpp src/GoToDialog.cpp
+    src/ProcessView.cpp src/TaskRow.cpp src/TasksView.cpp src/TaskDialogs.cpp src/TrayMenu.cpp src/IconFactory.cpp src/Shortcuts.cpp src/DownloadCacheSettings.cpp src/GoToDialog.cpp
     src/KeyboardSettings.cpp src/ShortcutsDialog.cpp src/AutomaticApprovalSettings.cpp src/PhonePairing.cpp
     # Tray icon decorator (QuietTrayIcon.h): portable, but wrapped around the icon
     # only on macOS; makeTrayIcon() in TrayAgent.cpp explains why not on Linux.
````

Create `tray/src/TaskDialogs.cpp`:

````cpp
#include "TaskDialogs.h"
#include "TasksView.h"
#include "WorkspaceIcons.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
QString sessionLabel(const QString &host, const QString &name) { return host.isEmpty() ? name : name + QStringLiteral(" @") + host; }
QLabel *section(const QString &text) {
    auto *label = new QLabel(text); label->setProperty("taskSection", true);
    QFont font = label->font(); font.setPixelSize(11); font.setWeight(QFont::DemiBold); label->setFont(font);
    return label;
}
QString time(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("d MMM HH:mm")); }
QString actor(const QJsonObject &actor) {
    return actor.value("kind").toString() == QLatin1String("agent") ? actor.value("session").toString() : QObject::tr("You");
}
// One history line: who, when and what happened.
QString historyLine(const QJsonObject &entry) {
    const QString kind = entry.value("kind").toString(), text = entry.value("text").toString();
    QString what;
    if (kind == QLatin1String("created")) what = QObject::tr("created the task");
    else if (kind == QLatin1String("edited")) what = QObject::tr("edited the prompt");
    else if (kind == QLatin1String("stage")) what = QObject::tr("moved it to %1").arg(TaskBoard::stageLabel(text));
    else if (kind == QLatin1String("target")) what = QObject::tr("changed the session: %1").arg(text);
    else if (kind == QLatin1String("sent")) {
        const auto session = entry.value("session").toObject();
        what = QObject::tr("sent it to %1").arg(sessionLabel(session.value("host").toString(), session.value("session").toString()));
    } else if (kind == QLatin1String("report")) what = QObject::tr("reported: %1").arg(text);
    else if (kind == QLatin1String("cancelled")) what = QObject::tr("cancelled it");
    else if (kind == QLatin1String("comment")) what = QObject::tr("commented: %1").arg(text);
    else what = text.isEmpty() ? kind : text;
    return QStringLiteral("<p style='margin:0 0 6px 0'><b>%1</b> <span style='opacity:.7'>%2</span> · %3</p>")
        .arg(actor(entry.value("actor").toObject()).toHtmlEscaped(), time(qint64(entry.value("at").toDouble())), what.toHtmlEscaped());
}
}

TaskDialog::TaskDialog(const QJsonObject &task, const QList<TaskSession> &sessions, const QString &sendBlocker, bool dark, QWidget *parent)
    : QDialog(parent), m_task(TaskBoard::item(task)) {
    setObjectName("taskDialog"); setWindowTitle(tr("Task %1").arg(m_task.id)); resize(640, 640);
    m_editable = m_task.writable && (m_task.stage == QLatin1String("todo") || m_task.stage == QLatin1String("backlog"));
    m_target = {m_task.targetSession.isEmpty(), m_task.targetHost, m_task.targetSession};
    const auto colors = TaskColors::of(dark);
    auto *layout = new QVBoxLayout(this); layout->setSpacing(8);
    auto *header = new QHBoxLayout; header->setSpacing(10);
    auto *stage = new QLabel(TaskBoard::stageLabel(m_task.stage)); stage->setObjectName("taskDialogStage");
    stage->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;border-radius:9px;padding:1px 8px;font-size:11px;font-weight:600;")
        .arg(colors.stage(m_task.stage).name()));
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(m_task.id.toHtmlEscaped()));
    auto *meta = new QLabel((m_task.author.isEmpty() ? tr("You") : m_task.author) + QStringLiteral(" · ") + time(m_task.createdAt));
    meta->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(colors.muted.name()));
    header->addWidget(stage); header->addWidget(title); header->addWidget(meta); header->addStretch(); layout->addLayout(header);
    layout->addWidget(section(tr("Prompt")));
    m_prompt = new QPlainTextEdit(m_task.text); m_prompt->setObjectName("taskPrompt"); m_prompt->setReadOnly(!m_editable);
    m_prompt->setAccessibleName(tr("Task prompt")); layout->addWidget(m_prompt, 2);
    if (m_task.stage == QLatin1String("review") && !m_task.summary.isEmpty()) {
        auto *summary = new QLabel(QStringLiteral("<b>%1 reported:</b> %2").arg(sessionLabel(m_task.sessionHost, m_task.sessionName).toHtmlEscaped(),
            m_task.summary.toHtmlEscaped()));
        summary->setObjectName("taskDialogSummary"); summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("border-left:2px solid %1;padding:1px 0 1px 10px;").arg(colors.warn.name()));
        layout->addWidget(summary);
    }
    layout->addWidget(section(tr("Session")));
    if (m_editable) {
        auto *chips = new QHBoxLayout; chips->setSpacing(6); m_sessions = new QButtonGroup(this);
        m_choices << TaskTarget{true, {}, {}};
        for (const auto &session : sessions) m_choices << TaskTarget{false, session.host, session.name};
        // A pinned session that no longer exists stays a choice until it is changed.
        if (!m_target.any && !m_choices.contains(m_target)) m_choices << m_target;
        for (int i = 0; i < m_choices.size(); ++i) {
            const auto &choice = m_choices.at(i);
            auto *chip = new QPushButton(choice.any ? tr("Any session") : sessionLabel(choice.host, choice.session));
            chip->setObjectName(QStringLiteral("taskSession:") + (choice.any ? QStringLiteral("any") : choice.host + QLatin1Char('/') + choice.session));
            chip->setCheckable(true); chip->setChecked(choice == m_target); m_sessions->addButton(chip, i); chips->addWidget(chip);
            for (const auto &session : sessions)
                if (!choice.any && session.host == choice.host && session.name == choice.session) chip->setToolTip(session.status);
        }
        chips->addStretch(); layout->addLayout(chips);
    } else {
        auto *fixed = new QLabel(m_task.sessionName.isEmpty() ? (m_task.targetSession.isEmpty() ? tr("Any session")
            : sessionLabel(m_task.targetHost, m_task.targetSession)) : sessionLabel(m_task.sessionHost, m_task.sessionName));
        fixed->setObjectName("taskDialogSession"); layout->addWidget(fixed);
    }
    layout->addWidget(section(tr("History")));
    m_history = new QTextBrowser; m_history->setObjectName("taskHistory"); m_history->setOpenLinks(false); layout->addWidget(m_history, 1);
    setHistory(task.value("log").toArray());
    auto *comment = new QHBoxLayout;
    m_comment = new QLineEdit; m_comment->setObjectName("taskComment");
    m_comment->setPlaceholderText(tr("Comment — people and agents see it; it is not sent to the session"));
    auto *add = new QPushButton(tr("Add")); add->setObjectName("addTaskComment"); add->setAutoDefault(false);
    const auto commentNow = [this] { if (!m_comment->text().trimmed().isEmpty()) emit commentRequested(m_comment->text().trimmed()); };
    connect(add, &QPushButton::clicked, this, commentNow); connect(m_comment, &QLineEdit::returnPressed, this, commentNow);
    comment->addWidget(m_comment, 1); comment->addWidget(add); layout->addLayout(comment);
    auto *actions = new QHBoxLayout; actions->setSpacing(8);
    const auto stageActions = TaskBoard::actions(m_task.stage);
    for (const auto action : stageActions) {
        if (action == TaskAction::Edit) continue;
        auto *button = new QPushButton(workspaceIcon(TaskBoard::actionIcon(action), colors.muted), TaskBoard::actionLabel(action).remove(QChar(0x2026)));
        button->setObjectName(QStringLiteral("taskDialogAction:") + TaskRow::actionKey(action)); button->setAutoDefault(false);
        const QString why = !m_task.writable ? tr("Update Zerus to edit this task") : action == TaskAction::SendNow ? sendBlocker : QString();
        button->setEnabled(why.isEmpty()); button->setToolTip(why.isEmpty() ? TaskBoard::actionLabel(action) : why);
        if (action == stageActions.last()) button->setStyleSheet(QStringLiteral("color:%1;").arg(colors.accent.name()));
        connect(button, &QPushButton::clicked, this, [this, action] { emit actionRequested(edits(), action); accept(); });
        actions->addWidget(button);
    }
    actions->addStretch();
    if (m_editable) {
        auto *cancel = new QPushButton(tr("Cancel")); cancel->setObjectName("discardTaskEdits"); cancel->setAutoDefault(false);
        auto *save = new QPushButton(tr("Save")); save->setObjectName("saveTask"); save->setDefault(true);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(save, &QPushButton::clicked, this, [this] { emit saveRequested(edits()); accept(); });
        actions->addWidget(cancel); actions->addWidget(save);
    } else {
        auto *close = new QPushButton(tr("Close")); close->setObjectName("closeTask"); close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::reject); actions->addWidget(close);
    }
    layout->addLayout(actions);
}

TaskEdits TaskDialog::edits() const {
    TaskEdits edits;
    if (!m_editable) return edits;
    if (const QString text = m_prompt->toPlainText(); text != m_task.text && !text.trimmed().isEmpty()) edits.text = text;
    if (m_sessions && m_sessions->checkedId() >= 0)
        if (const auto chosen = m_choices.value(m_sessions->checkedId()); !(chosen == m_target)) edits.target = chosen;
    return edits;
}

void TaskDialog::setHistory(const QJsonArray &log) {
    QString html;
    for (const auto &entry : log) html += historyLine(entry.toObject());
    m_history->setHtml(html);
}

void TaskDialog::clearComment() { m_comment->clear(); }

TaskCancelDialog::TaskCancelDialog(const TaskItem &task, const QString &sessionStatus, bool running, QWidget *parent) : QDialog(parent) {
    setObjectName("taskCancelDialog"); setWindowTitle(tr("Cancel %1?").arg(task.id));
    auto *layout = new QVBoxLayout(this); layout->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Cancel %1?").arg(task.id).toHtmlEscaped())); layout->addWidget(title);
    auto *text = new QLabel(task.text); text->setObjectName("taskCancelText"); text->setWordWrap(true); text->setTextFormat(Qt::PlainText);
    text->setMaximumHeight(text->fontMetrics().lineSpacing() * 4 + 4); layout->addWidget(text);
    const QString session = sessionLabel(task.sessionHost, task.sessionName);
    QString what = tr("%1 moves to In review, and %2 gets the cancel message.").arg(task.id, session);
    if (!running) what += QLatin1Char(' ') + tr("The agent is not running, so hgs will refuse the message; the task is cancelled all the same.");
    else if (sessionStatus == QLatin1String("Working")) what += QLatin1Char(' ') + tr("%1 is working; it reads the message when its turn ends.").arg(session);
    auto *warning = new QLabel(what); warning->setObjectName("taskCancelWarning"); warning->setWordWrap(true); layout->addWidget(warning);
    layout->addWidget(section(tr("Reason (optional) — saved as a comment on the task, not sent to the agent")));
    m_reason = new QLineEdit; m_reason->setObjectName("taskCancelReason"); m_reason->setPlaceholderText(tr("Why it is cancelled"));
    layout->addWidget(m_reason);
    auto *buttons = new QHBoxLayout;
    auto *keep = new QPushButton(tr("Keep running")); keep->setObjectName("keepTaskRunning"); keep->setDefault(true);
    auto *cancel = new QPushButton(tr("Cancel task")); cancel->setObjectName("confirmTaskCancel"); cancel->setAutoDefault(false);
    cancel->setStyleSheet(QStringLiteral("color:#e06c75;"));
    connect(keep, &QPushButton::clicked, this, &QDialog::reject); connect(cancel, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(keep); buttons->addStretch(); buttons->addWidget(cancel); layout->addLayout(buttons);
}

QString TaskCancelDialog::reason() const { return m_reason->text().trimmed(); }

TaskSettingsDialog::TaskSettingsDialog(const QString &projectName, const QJsonObject &settings, QWidget *parent)
    : QDialog(parent), m_settings(settings) {
    setObjectName("taskSettingsDialog"); setWindowTitle(tr("Project tasks · %1").arg(projectName)); resize(620, 560);
    auto *layout = new QVBoxLayout(this); layout->setSpacing(8);
    layout->addWidget(section(tr("Task prompt (Send now)")));
    m_task = new QPlainTextEdit(settings.value("task_template").toString()); m_task->setObjectName("taskTemplate"); layout->addWidget(m_task, 1);
    layout->addWidget(section(tr("Cancel message")));
    m_cancel = new QPlainTextEdit(settings.value("cancel_template").toString()); m_cancel->setObjectName("cancelTemplate"); layout->addWidget(m_cancel, 1);
    auto *hint = new QLabel(tr("Placeholders: {ref}, {author}, {prompt}, {tools}, {history}")); hint->setObjectName("taskTemplateHint");
    layout->addWidget(hint);
    layout->addWidget(section(tr("Agents")));
    m_agentsOther = new QCheckBox(tr("Agents may start and cancel tasks in other sessions")); m_agentsOther->setObjectName("agentsOther");
    m_agentsOther->setChecked(settings.value("agents_other").toBool(true)); layout->addWidget(m_agentsOther);
    auto *limit = new QHBoxLayout;
    m_limit = new QSpinBox; m_limit->setObjectName("agentLimit"); m_limit->setRange(0, 1000); m_limit->setSpecialValueText(tr("no limit"));
    m_limit->setValue(settings.value("agent_limit").toInt());
    limit->addWidget(new QLabel(tr("Limit tasks started by agents"))); limit->addWidget(m_limit); limit->addWidget(new QLabel(tr("per hour")));
    limit->addStretch(); layout->addLayout(limit);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    auto *cancel = new QPushButton(tr("Cancel")); cancel->setObjectName("cancelTaskSettings"); cancel->setAutoDefault(false);
    auto *save = new QPushButton(tr("Save")); save->setObjectName("saveTaskSettings"); save->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject); connect(save, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(cancel); buttons->addWidget(save); layout->addLayout(buttons);
}

QJsonObject TaskSettingsDialog::changes() const {
    QJsonObject changes;
    const auto changed = [&](const char *key, const QJsonValue &value) { if (m_settings.value(key) != value) changes.insert(key, value); };
    changed("task_template", m_task->toPlainText()); changed("cancel_template", m_cancel->toPlainText());
    changed("agents_other", m_agentsOther->isChecked()); changed("agent_limit", m_limit->value());
    return changes;
}
````

Create `tray/src/TaskDialogs.h`:

````cpp
#pragma once
#include "TaskBoard.h"
#include "TaskRow.h"
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <optional>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTextBrowser;
struct TaskSession;

// The session a task is pinned to; any is Any session.
struct TaskTarget {
    bool any = true;
    QString host, session;
    bool operator==(const TaskTarget &other) const { return any == other.any && host == other.host && session == other.session; }
};

// Unsaved changes in the task settings: only what the person changed.
struct TaskEdits {
    std::optional<QString> text;
    std::optional<TaskTarget> target;
};

// A task's settings: its prompt and session (editable in To do and Backlog), its
// history, a comment field and the stage's actions. An action first applies the
// unsaved prompt and session; Cancel or Escape discards them.
class TaskDialog : public QDialog {
    Q_OBJECT
public:
    // task: the `hgs task get` view, history included.
    TaskDialog(const QJsonObject &task, const QList<TaskSession> &sessions, const QString &sendBlocker, bool dark, QWidget *parent = nullptr);
    TaskEdits edits() const;
    void setHistory(const QJsonArray &log);
    void clearComment();
signals:
    void saveRequested(const TaskEdits &edits);
    void actionRequested(const TaskEdits &edits, TaskAction action);
    void commentRequested(const QString &text);
private:
    TaskItem m_task;
    TaskTarget m_target;
    QPlainTextEdit *m_prompt;
    QButtonGroup *m_sessions = nullptr;
    QList<TaskTarget> m_choices;
    QTextBrowser *m_history;
    QLineEdit *m_comment;
    bool m_editable;
};

// Confirms a Cancel: the task moves to In review and its session gets the cancel
// message; an optional reason becomes a comment and is never sent.
class TaskCancelDialog : public QDialog {
    Q_OBJECT
public:
    TaskCancelDialog(const TaskItem &task, const QString &sessionStatus, bool running, QWidget *parent = nullptr);
    QString reason() const;
private:
    QLineEdit *m_reason;
};

// The project's task settings: the prompt templates, the switch for agents acting
// on other sessions and the limit on tasks agents start per hour.
class TaskSettingsDialog : public QDialog {
    Q_OBJECT
public:
    TaskSettingsDialog(const QString &projectName, const QJsonObject &settings, QWidget *parent = nullptr);
    // The fields the person changed, for `hgs task settings --set`.
    QJsonObject changes() const;
private:
    QJsonObject m_settings;
    QPlainTextEdit *m_task, *m_cancel;
    QCheckBox *m_agentsOther;
    QSpinBox *m_limit;
};
````

Apply this change to `tray/src/TasksView.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.cpp b/tray/src/TasksView.cpp
index 78f8e02..4b6b908 100644
--- a/tray/src/TasksView.cpp
+++ b/tray/src/TasksView.cpp
@@ -1,11 +1,13 @@
 #include "TasksView.h"
 #include "HgsClient.h"
+#include "TaskDialogs.h"
 #include "MessageComposer.h"
 #include "WorkspaceIcons.h"
 #include "WorkspaceStyle.h"
 #include <QAbstractButton>
 #include <QDateTime>
 #include <QHBoxLayout>
+#include <QJsonDocument>
 #include <QLabel>
 #include <QPainter>
 #include <QPushButton>
@@ -308,6 +310,17 @@ QList<TasksView::Step> TasksView::steps(const TaskItem &task, TaskAction action)
     return {};
 }
 
+QList<TasksView::Step> TasksView::editSteps(const TaskItem &task, const TaskEdits &edits) const {
+    QList<Step> out;
+    if (edits.text) out << Step{withProject({"edit", task.uuid}), *edits.text};
+    if (edits.target) {
+        QStringList arguments{"target", task.uuid, edits.target->any ? QStringLiteral("any") : edits.target->session};
+        if (!edits.target->any && !edits.target->host.isEmpty()) arguments << "--host" << edits.target->host;
+        out << Step{withProject(arguments), {}};
+    }
+    return out;
+}
+
 void TasksView::run(QList<Step> steps) {
     if (steps.isEmpty()) return refresh();
     auto step = steps.takeFirst();
@@ -338,13 +351,61 @@ void TasksView::addTask(const QString &key, const QString &text) {
     });
 }
 
-// The task settings, the cancel confirmation and the project settings come with
-// their dialogs (TaskDialogs.h).
-void TasksView::openTask(const TaskItem &) {}
+void TasksView::openTask(const TaskItem &task) {
+    request(withProject({"get", task.uuid}), {}, [this, task](const QJsonObject &reply) {
+        for (auto *open : findChildren<TaskDialog *>()) open->reject();
+        auto *dialog = new TaskDialog(reply.value("task").toObject(), m_sessions, sendBlocker(task), m_dark, this);
+        dialog->setAttribute(Qt::WA_DeleteOnClose);
+        connect(dialog, &TaskDialog::saveRequested, this, [this, task](const TaskEdits &edits) { run(editSteps(task, edits)); });
+        connect(dialog, &TaskDialog::actionRequested, this, [this, task](const TaskEdits &edits, TaskAction action) {
+            // The action meets the task as the edits leave it.
+            auto edited = task;
+            if (edits.target) { edited.targetSession = edits.target->any ? QString() : edits.target->session; edited.targetHost = edits.target->host; }
+            if (action == TaskAction::Cancel) { run(editSteps(task, edits)); return confirmCancel(edited); }
+            run(editSteps(task, edits) + steps(edited, action));
+        });
+        QPointer<TaskDialog> guard(dialog);
+        connect(dialog, &TaskDialog::commentRequested, this, [this, guard, uuid = task.uuid](const QString &text) {
+            request(withProject({"comment", uuid}), text, [guard](const QJsonObject &reply) {
+                if (!guard) return;
+                guard->clearComment(); guard->setHistory(reply.value("task").toObject().value("log").toArray());
+            });
+        });
+        dialog->open();
+    });
+}
 
-void TasksView::confirmCancel(const TaskItem &) {}
+void TasksView::confirmCancel(const TaskItem &task) {
+    const auto *ran = session(task.sessionHost, task.sessionName);
+    auto *dialog = new TaskCancelDialog(task, ran ? ran->status : QString(), ran && ran->running, this);
+    dialog->setAttribute(Qt::WA_DeleteOnClose);
+    connect(dialog, &QDialog::accepted, this, [this, dialog, task] {
+        QStringList arguments{"cancel", task.uuid};
+        if (!dialog->reason().isEmpty()) arguments << "--reason" << dialog->reason();
+        run({{withProject(arguments), {}, [this, task](const QJsonObject &reply) {
+            // The task is cancelled even when hgs refused the cancel message.
+            const auto message = reply.value("cancel_message").toObject();
+            if (!message.value("sent").toBool(true))
+                emit notice(tr("%1 is cancelled; its session did not get the cancel message: %2").arg(task.id, message.value("error").toString()), true);
+        }}});
+    });
+    dialog->open();
+}
 
-void TasksView::openSettings() {}
+void TasksView::openSettings() {
+    if (m_projectId.isEmpty()) return;
+    request({"settings", "--project", m_projectId}, {}, [this](const QJsonObject &reply) {
+        auto *dialog = new TaskSettingsDialog(m_projectName, reply.value("settings").toObject(), this);
+        dialog->setAttribute(Qt::WA_DeleteOnClose);
+        connect(dialog, &QDialog::accepted, this, [this, dialog] {
+            const auto changes = dialog->changes();
+            if (!changes.isEmpty())
+                request({"settings", "--project", m_projectId, "--set"}, QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)),
+                        [](const QJsonObject &) {});
+        });
+        dialog->open();
+    });
+}
 
 void TasksView::setTheme(bool dark) {
     m_dark = dark; const auto colors = TaskColors::of(dark);
````

Apply this change to `tray/src/TasksView.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/TasksView.h b/tray/src/TasksView.h
index d5f98e9..16cc0c5 100644
--- a/tray/src/TasksView.h
+++ b/tray/src/TasksView.h
@@ -26,6 +26,8 @@ struct TaskSession {
     bool running = true;  // whether its agent runs, for the cancel dialog
 };
 
+struct TaskEdits;
+
 // The Tasks tab label's counts: In review as an attention badge, then To do.
 class TaskTabBadge : public QWidget {
     Q_OBJECT
@@ -77,6 +79,7 @@ private:
     // One `hgs task` call of a sequence; `then` sees its reply.
     struct Step { QStringList arguments; QString input; std::function<void(const QJsonObject &)> then = {}; };
     QList<Step> steps(const TaskItem &task, TaskAction action) const;
+    QList<Step> editSteps(const TaskItem &task, const TaskEdits &edits) const;
     // Runs the steps in order and stops at the first failure; refreshes after.
     void run(QList<Step> steps);
     void act(const TaskItem &task, TaskAction action);
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake -S tray -B .ci-build/gui >/dev/null && cmake --build .ci-build/gui --target test_taskdialogs test_tasksview -j 8
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_taskdialogs
QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_tasksview
```
Expected: `test_taskdialogs` `Totals: 6 passed`; `test_tasksview` `Totals: 12 passed`.

- [ ] **Step 5: Commit**

```bash
git add tray/CMakeLists.txt \
  tray/src/TaskDialogs.cpp \
  tray/src/TaskDialogs.h \
  tray/src/TasksView.cpp \
  tray/src/TasksView.h \
  tray/tests/CMakeLists.txt \
  tray/tests/test_taskdialogs.cpp
git commit -m "Edit, cancel and configure tasks in dialogs"
```

---

### Task 6: The tab in the window

**Files:**
- Modify: `tray/tests/test_sessionswindow.cpp`, `docs/reference.md`, `tray/src/SessionsWindow.cpp`, `tray/src/SessionsWindow.h`, `tray/src/SessionsWindowGoTo.cpp`, `tray/src/SessionsWindowShortcuts.cpp`

**Interfaces:**
- Consumes: `TasksView`, `TaskTabBadge`, `TaskSession` (Tasks 4–5); `SessionOrganization::groupFor`/`group`, `SessionPresentation::status`, `SessionInfo::needsAction`.
- Produces: the detail tabs Activity, Terminal, Tasks, Native UI, Processes (Tasks third, Native UI and Processes found by widget, never by index); `SessionsWindow::updateTasks(const Entry *)`; the inspector's first tab "Plan && agents".

The tab follows the open session: its project (none for `ungrouped`), the project's other sessions with their status, whether their agent runs and why one cannot take a prompt now. Its label carries the badge; its notices go to the window's status bar; it polls only while the window shows. Files are never dropped on it (a task is text only), F6 and Next tab land in its input, and Go to session for a task-prompt draft opens it.

- [ ] **Step 1: Write the failing tests**

Apply this change to `tray/tests/test_sessionswindow.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/tests/test_sessionswindow.cpp b/tray/tests/test_sessionswindow.cpp
index ebf307c..48e85f2 100644
--- a/tray/tests/test_sessionswindow.cpp
+++ b/tray/tests/test_sessionswindow.cpp
@@ -3,6 +3,8 @@
 #include <QApplication>
 #include <QFrame>
 #include <QToolButton>
+#include <QAbstractButton>
+#include <tuple>
 #include <QComboBox>
 #include <QCheckBox>
 #include <QDateTime>
@@ -57,6 +59,7 @@
 #include <utility>
 #include "SessionsWindow.h"
 #include "ProcessView.h"
+#include "TasksView.h"
 #include "ActivityView.h"
 #include "AttachmentViewer.h"
 #include "ExternalFile.h"
@@ -320,6 +323,7 @@ private slots:
     void sessionRowsHaveNoHoverPopup();
     void subagentGroupsAndUnavailableActivity();
     void inspectorPreservesWorkspaceAndReportsTasks();
+    void tasksTabFollowsTheOpenSessionsProject();
     void detailsPollingPreservesReadingPosition();
     void nativeGoalIsIndependentOfTurnState();
     void fileReferencesKeepMachineAndWorkspace();
@@ -959,6 +963,49 @@ void TestSessionsWindow::projectFilterCombinesScopesCountsAndNavigation()
     window.showSession("mac","claude/infra/review"); QVERIFY(projects->selection().isEmpty()); QCOMPARE(parents(),5);
 }
 
+void TestSessionsWindow::tasksTabFollowsTheOpenSessionsProject()
+{
+    SessionOrganization org; const auto work=org.createGroup("Work");
+    org.moveSession(org.observe("arch","codex/hgs/dashboard",""),work); org.moveSession(org.observe("arch","kimi/docs/research",""),work);
+    QSettings().setValue("workspace/organization",QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
+    // The fake replica: one task in review and two in To do, one pinned to the
+    // project's other session.
+    QJsonArray tasks;
+    for (const auto &[id, stage, session] : QList<std::tuple<QString, QString, QString>>{{"T-000001","review",""},{"T-000002","todo","kimi/docs/research"},{"T-000003","todo",""}}) {
+        QJsonObject task{{"uuid",id},{"id",id},{"ref","Work/"+id},{"text","Task "+id},{"stage",stage},{"summary",""},{"cancel_reason",""},
+            {"cancelled_at",QJsonValue()},{"created_at",1},{"stage_at",1},{"writable",true},{"author",QJsonObject{{"kind","person"}}},
+            {"target",session.isEmpty()?QJsonObject{{"kind","any"}}:QJsonObject{{"kind","session"},{"machine","m"},{"session",session},{"host",""}}},
+            {"session",QJsonValue()},{"rank","V"}};
+        tasks<<task;
+    }
+    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly));
+    const QByteArray answer = "elif c=='task':\n open(__file__+'.tasks','a').write(json.dumps(a)+'\\n')\n print(json.dumps({'project':{'id':a[a.index('--project')+1],'name':'Work'},'tasks':json.loads("
+        + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(tasks).toJson(QJsonDocument::Compact))) + "),'damaged':[]}))\n";
+    const QString fixture = m_dir->filePath("tasks-hgs");
+    QVERIFY(FakeCli::install(fixture, original.readAll().replace("if False:pass\n", "if False:pass\n" + answer)));
+    SessionsWindow window(FakeCli::program(fixture)); keepDetailsPollsOut(window); window.setFleet(fleet()); window.show();
+    window.showSession({}, "codex/hgs/dashboard");
+    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *view = window.findChild<TasksView *>();
+    auto *badge = window.findChild<TaskTabBadge *>("taskTabBadge"); QVERIFY(tabs && view && badge);
+    QCOMPARE(tabs->tabBar()->tabButton(tabs->indexOf(view), QTabBar::RightSide), badge);
+    // The label counts the project's tasks in In review and To do.
+    QTRY_COMPARE(badge->property("todo").toInt(), 2); QCOMPARE(badge->property("review").toInt(), 1); QVERIFY(badge->isVisible());
+    QFile calls(fixture + ".tasks"); QVERIFY(calls.open(QIODevice::ReadOnly));
+    QCOMPARE(QJsonDocument::fromJson(calls.readLine()).array(), QJsonArray({"task", "list", "--project", work}));
+    tabs->setCurrentWidget(view); QTRY_VERIFY(view->isVisible());
+    QCOMPARE(view->composer()->sessionKey(), QString("task-draft\n\ncodex/hgs/dashboard"));
+    // The project's other session exists, so its pinned task can be sent.
+    view->findChild<QAbstractButton *>("taskTile:todo")->click();
+    auto *row = view->findChild<QWidget *>("taskRow:T-000002"); QVERIFY(row);
+    QVERIFY(row->findChild<QToolButton *>("taskAction:sendNow")->isEnabled());
+    QVERIFY(row->findChild<QLabel *>("taskMeta")->text().contains("kimi/docs/research"));
+    // The side panel's first tab is the agent's own plan, not the project's tasks.
+    QCOMPARE(window.findChild<QTabWidget *>("sessionInspector")->tabText(0), QString("Plan && agents"));
+    // A session in no project explains where tasks live.
+    window.showSession({}, "codex/website/navigation");
+    QTRY_VERIFY(view->findChild<QLabel *>("taskEmpty")->text().startsWith("Tasks belong to a project"));
+}
+
 void TestSessionsWindow::projectHeaderFiltersOnlyThisProject()
 {
     SessionOrganization org; const auto work=org.createGroup("Work"), docs=org.createGroup("Docs");
@@ -2621,20 +2668,21 @@ if a[:1]!=['a']:
     constexpr int activityMs=1000;window.setDetailsPollForTests(100,activityMs);
     auto log=[&]{QList<QJsonArray> result;QFile file(temp.filePath("inspections.jsonl"));if(file.open(QIODevice::ReadOnly))while(!file.atEnd())result<<QJsonDocument::fromJson(file.readLine()).array();return result;};
     auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");QVERIFY(tabs);
+    const int processes=tabs->indexOf(window.findChild<ProcessView *>());QCOMPARE(processes,4);
     QTRY_VERIFY(!log().isEmpty());QTest::qWait(200);
-    QVERIFY(!tabs->isTabVisible(3));for(const auto &call:log())QVERIFY(call.contains("--skip-processes"));
+    QVERIFY(!tabs->isTabVisible(processes));for(const auto &call:log())QVERIFY(call.contains("--skip-processes"));
     auto *settings=window.findChild<QWidget *>("settingsPage");
     settings->findChild<QDoubleSpinBox *>("activeSeconds")->setValue(1);
     settings->findChild<QDoubleSpinBox *>("backgroundSeconds")->setValue(5);
     QCOMPARE(QSettings().value("processes/activeSeconds").toDouble(),1.);
     ProcessSettings::setPollFloorForTests(0.2);QSettings().setValue("processes/activeSeconds",0.2);
     const auto before=log().size();QTRY_VERIFY(log().size()>before);
-    QVERIFY(log().last().contains("--skip-processes"));QCOMPARE(tabs->tabText(3),QString("Processes"));
-    auto *enabled=settings->findChild<QCheckBox *>("processesEnabled");QVERIFY(enabled);enabled->setChecked(true);QVERIFY(tabs->isTabVisible(3));
+    QVERIFY(log().last().contains("--skip-processes"));QCOMPARE(tabs->tabText(processes),QString("Processes"));
+    auto *enabled=settings->findChild<QCheckBox *>("processesEnabled");QVERIFY(enabled);enabled->setChecked(true);QVERIFY(tabs->isTabVisible(processes));
     // Only the Processes interval can poll the open tab again within 3 s: Activity
     // now waits a minute and Terminal 5 s.
     window.setDetailsPollForTests(100,60000);
-    tabs->setCurrentIndex(3);QTRY_VERIFY(!log().last().contains("--skip-processes"));
+    tabs->setCurrentIndex(processes);QTRY_VERIFY(!log().last().contains("--skip-processes"));
     const auto active=log().size();QTRY_VERIFY_WITH_TIMEOUT(log().size()>active,3000);QVERIFY(!log().last().contains("--skip-processes"));
     window.setDetailsPollForTests(100,activityMs);
     tabs->setCurrentIndex(1);settle();const auto terminal=log().size();QTest::qWait(activityMs+400);QCOMPARE(log().size(),terminal);
@@ -2643,7 +2691,7 @@ if a[:1]!=['a']:
     // Every Activity poll after settle() is at least the background interval, here
     // 0.2 s, after the last Processes poll: it would include processes if enabled.
     QSettings().setValue("processes/backgroundSeconds",0.2);
-    tabs->setCurrentIndex(3);QTest::qWait(200);enabled->setChecked(false);QVERIFY(!tabs->isTabVisible(3));QCOMPARE(tabs->currentIndex(),0);
+    tabs->setCurrentIndex(processes);QTest::qWait(200);enabled->setChecked(false);QVERIFY(!tabs->isTabVisible(processes));QCOMPARE(tabs->currentIndex(),0);
     settle();const auto disabled=log().size();QTRY_VERIFY(log().size()>disabled);
     for(const auto &call:log().sliced(disabled))QVERIFY(call.contains("--skip-processes"));
     window.hide();settle();const auto hidden=log().size();QTest::qWait(activityMs+400);QCOMPARE(log().size(),hidden);
@@ -5471,9 +5519,10 @@ void TestSessionsWindow::inspectorPreservesWorkspaceAndReportsTasks()
     auto *splitter = window.findChild<QSplitter *>("sessionWorkSplitter");
     auto *editor = window.findChild<MessageComposer *>("messageComposer")->findChild<QPlainTextEdit *>("messageInput");
     QVERIFY(tabs && inspector && toggle && panel && splitter && editor);
-    QCOMPARE(tabs->count(), 4); QCOMPARE(tabs->tabText(0), QString("Activity")); QCOMPARE(tabs->tabText(1), QString("Terminal"));
-    QCOMPARE(tabs->tabText(3),QString("Processes"));QVERIFY(tabs->isTabVisible(3));
-    QVERIFY(tabs->isTabVisible(1)); QVERIFY(!tabs->isTabVisible(2));
+    QCOMPARE(tabs->count(), 5); QCOMPARE(tabs->tabText(0), QString("Activity")); QCOMPARE(tabs->tabText(1), QString("Terminal"));
+    QCOMPARE(tabs->tabText(2), QString("Tasks")); QCOMPARE(tabs->tabText(3), QString("Native UI"));
+    QCOMPARE(tabs->tabText(4),QString("Processes"));QVERIFY(tabs->isTabVisible(4));
+    QVERIFY(tabs->isTabVisible(1)); QVERIFY(tabs->isTabVisible(2)); QVERIFY(!tabs->isTabVisible(3));
     QVERIFY(panel->isHidden()); QCOMPARE(tabs->cornerWidget(), toggle);
     editor->setPlainText("Keep this draft"); const int originalWidth = tabs->width();
     QTest::mouseClick(toggle, Qt::LeftButton); QTest::qWait(40);
@@ -5504,8 +5553,8 @@ void TestSessionsWindow::inspectorPreservesWorkspaceAndReportsTasks()
         QJsonObject{{"id","build"},{"command","cargo build --release"},{"description","Build the updated CLI"},{"owner","main"},{"cwd","/workspace/zerus"},{"status","running"},{"started_at",QDateTime::currentSecsSinceEpoch()-23}},
         QJsonObject{{"id","tests"},{"command","python3 tests/test_dsh_native.py -v"},{"owner","main"},{"cwd","/workspace/zerus"},{"status","completed"},{"started_at",QDateTime::currentSecsSinceEpoch()-65},{"ended_at",QDateTime::currentSecsSinceEpoch()-12},{"exit_code",0}}
     }}};
-    client->inspectionReady({},"codex/hgs/dashboard",data);QCOMPARE(tabs->tabText(3),QString("Processes (1)"));
-    tabs->setCurrentIndex(3);QVERIFY(window.findChild<QTreeWidget*>("processList")->isVisible());QCOMPARE(editor->toPlainText(),QString("Keep this draft"));
+    client->inspectionReady({},"codex/hgs/dashboard",data);QCOMPARE(tabs->tabText(tabs->indexOf(window.findChild<ProcessView *>())),QString("Processes (1)"));
+    tabs->setCurrentWidget(window.findChild<ProcessView *>());QVERIFY(window.findChild<QTreeWidget*>("processList")->isVisible());QCOMPARE(editor->toPlainText(),QString("Keep this draft"));
     if(!qEnvironmentVariable("HGS_PROCESSES_WINDOW_PREVIEW").isEmpty()){
         QTest::mouseClick(toggle,Qt::LeftButton);QTest::qWait(80);QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PROCESSES_WINDOW_PREVIEW")));QTest::mouseClick(toggle,Qt::LeftButton);
     }
@@ -8423,7 +8472,7 @@ void TestSessionsWindow::openCodeCapabilitiesPreserveDraftWithoutTerminal()
     SessionsWindow window(fake);window.setFleet(state);window.show();window.showSession({},name);
     auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *client=window.findChild<HgsClient *>();
     settleSelectedInspection(composer,client,{},name,details);setComposerText(composer,"Keep my local model draft");
-    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");QVERIFY(tabs);QVERIFY(!tabs->isTabVisible(1));QVERIFY(!tabs->isTabVisible(2));
+    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");QVERIFY(tabs);QVERIFY(!tabs->isTabVisible(1));QVERIFY(!tabs->isTabVisible(3));
     QTRY_VERIFY2(composer->findChild<QPushButton *>("sendMessage")->isEnabled(),qPrintable(composer->findChild<QLabel *>("messageStatus")->text()+" key="+composer->sessionKey()+" draft="+composer->editor()->toPlainText()));QVERIFY(!composer->canAttachFiles());
     QVERIFY(!composer->findChild<QPushButton *>("attachMessageFile")->isEnabled());
     QVERIFY(!window.findChild<QPushButton *>("clearSessionAction")->isEnabled());QVERIFY(!window.findChild<QAction *>("forgetSessionAction")->isEnabled());
@@ -8812,7 +8861,7 @@ else: print('{}')
     // no browser endpoint. It must not open an external browser automatically.
     QSignalSpy external(client, &HgsClient::writeDone), connection(client, &HgsClient::nativeUiFailed);
     auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
-    QVERIFY(!tabs->isTabVisible(1)); QVERIFY(tabs->isTabVisible(2));
+    QVERIFY(!tabs->isTabVisible(1)); QCOMPARE(tabs->tabText(3), QString("Native UI")); QVERIFY(tabs->isTabVisible(3));
     window.findChild<QPushButton *>("nativeSignInButton")->click();
     QCOMPARE(tabs->currentWidget()->objectName(), QString("nativeUiView"));
     QTRY_COMPARE(connection.size(), 1); QCOMPARE(external.size(), 0);
@@ -8849,7 +8898,7 @@ else: print('{}')
     QTRY_VERIFY(!inspected.isEmpty());
     client->inspectionReady({}, session.name, details);
     auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
-    QVERIFY(!tabs->isTabVisible(1)); QVERIFY(!tabs->isTabVisible(2));
+    QVERIFY(!tabs->isTabVisible(1)); QVERIFY(!tabs->isTabVisible(3));
     QSignalSpy written(client, &HgsClient::writeDone);
     window.findChild<QPushButton *>("nativeSignInButton")->click();
     QTRY_COMPARE(written.size(), 1); QCOMPARE(tabs->currentIndex(), 0);
@@ -10027,9 +10076,9 @@ void TestSessionsWindow::shortcutsCycleSessionTabs()
         window.showSession({}, "codex/hgs/dashboard");
         auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *terminal = window.findChild<TerminalView *>();
         auto *processView = window.findChild<ProcessView *>(); QVERIFY(tabs); QVERIFY(terminal); QVERIFY(processView);
-        QVERIFY(!tabs->isTabVisible(2)); // Native UI belongs to DeepSeek sessions.
+        QVERIFY(!tabs->isTabVisible(3)); // Native UI belongs to DeepSeek sessions.
         QCOMPARE(tabs->isTabVisible(tabs->indexOf(processView)), processes);
-        QList<QWidget *> order{tabs->widget(0), terminal}; if (processes) order << processView;
+        QList<QWidget *> order{tabs->widget(0), terminal, window.findChild<TasksView *>()}; if (processes) order << processView;
         auto *composer = window.findChild<MessageComposer *>("messageComposer");
         composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
         for (int step = 1; step <= order.size(); ++step) {
@@ -10185,7 +10234,7 @@ void TestSessionsWindow::reboundChordsApplyAtOnce()
     pressSequence(&window, previousTab); // No longer bound: Terminal keeps it.
     QCOMPARE(tabs->currentWidget(), terminal);
     pressCommand(&window, "tab.next");
-    QCOMPARE(tabs->currentIndex(), 0);
+    QCOMPARE(tabs->currentWidget(), window.findChild<TasksView *>());
 }
 
 void TestSessionsWindow::projectListKeepsItsReorderKeys()
````

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build .ci-build/gui --target test_sessionswindow -j 8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_sessionswindow tasksTabFollowsTheOpenSessionsProject inspectorPreservesWorkspaceAndReportsTasks shortcutsCycleSessionTabs`
Expected: FAIL — the window has no `TasksView` and four tabs (`Actual (tabs->count()): 4`, `Expected (5)`).

- [ ] **Step 3: Implement**

Apply this change to `docs/reference.md` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/docs/reference.md b/docs/reference.md
index afd0cf5..820ef16 100644
--- a/docs/reference.md
+++ b/docs/reference.md
@@ -702,7 +702,7 @@ application and window switchers.
 | Rename session | F2 | F2 |
 | Search sessions | Ctrl+F | ⌘F |
 | Refresh | Ctrl+R, F5 | ⌘R, F5 |
-| Next / previous tab (Activity, Terminal, Processes, Native UI) | Ctrl+Tab / Ctrl+Shift+Tab | ⌃Tab / ⌃⇧Tab |
+| Next / previous tab (Activity, Terminal, Tasks, Native UI, Processes) | Ctrl+Tab / Ctrl+Shift+Tab | ⌃Tab / ⌃⇧Tab |
 | Switch between Activity and Terminal | Ctrl+` | ⌃` |
 | Show or hide the side panel | Ctrl+I | ⌘I |
 | Next / previous area (list, conversation, message field, side panel) | F6 / Shift+F6 | F6 / ⇧F6 |
@@ -1371,9 +1371,10 @@ Claude's recorded compaction boundary replaces the earlier context size with its
 reported summary size. It invalidates the earlier prompt cache estimate; a new
 warm countdown requires fresh native usage with a reported TTL.
 
-**Tasks & agents**, **Details** and **Worktrees** occupy the resizable inspector.
+**Plan & agents**, **Details** and **Worktrees** occupy the resizable inspector.
 Its width, tab and open state are local; toggling it preserves Activity, drafts and
-Terminal attachment. Successful native task/plan tools supply recorded pending,
+Terminal attachment. Plan & agents shows the agent's own plan, not the project's
+Zerus tasks (the Tasks tab). Successful native task/plan tools supply recorded pending,
 in-progress and completed items. Tool failures and Stop do not complete tasks.
 There are up to 100 items per list and 65 lists, with truncation disclosed.
 Old data shows **Last recorded**; unavailable history is not reconstructed.
@@ -1399,7 +1400,7 @@ The expanded session tree hides children whose latest reported turn is finished
 or failed by default. Session and Overview cards then show only the active count, omitting
 an empty indicator when no active or uncertain children remain.
 **Settings → Sessions → Show completed subagents** restores completed rows and
-active/total card counts. The **Tasks & agents** inspector always shows the full
+active/total card counts. The **Plan & agents** inspector always shows the full
 reported roster and its active/total counts, including completed turns.
 Unknown states, paused agents and input/approval requests remain visible.
 An open child's Activity and draft stay in place when its turn finishes. A child's
@@ -1892,8 +1893,17 @@ verified member, or null.
 
 Tasks are Loro documents below `HGS_STATE_DIR/replica`, one directory per
 project; a task file that fails to load is listed under `damaged` and left in
-place. Replication between computers, the Tasks view and the MCP tools come in
-later releases.
+place. Replication between computers and the MCP tools come in later releases.
+
+In the desktop, the **Tasks** tab beside Activity and Terminal shows the open
+session's project. Stage tiles (In review, In progress, To do, Backlog, Done)
+filter one list; the funnel tile keeps only the open session's tasks and the Any
+session tasks it can take, and the gear opens the project's task settings. A row
+shows its actions on hover, and a click opens the task: its prompt and session
+(editable in To do and Backlog), history and comments. The input below files a
+task for the open session in To do, or in Backlog while Backlog is open. The tab
+label counts the tasks In review and in To do. Done lists the last 7 days, and
+Show older adds the rest. A session outside every project has no tasks.
 
 ## Machines and accounts
 
````

Apply this change to `tray/src/SessionsWindow.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionsWindow.cpp b/tray/src/SessionsWindow.cpp
index bbe69be..cb57322 100644
--- a/tray/src/SessionsWindow.cpp
+++ b/tray/src/SessionsWindow.cpp
@@ -28,6 +28,7 @@
 #include "NewSessionDialog.h"
 #include "ActivityView.h"
 #include "ProcessView.h"
+#include "TasksView.h"
 #include "AttachmentViewer.h"
 #include "NativeBrowserPlugin.h"
 #include "NativeUiView.h"
@@ -1192,8 +1193,13 @@ SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
     });
     m_info = browser(); m_info->setObjectName("sessionInfo"); m_info->setAccessibleName(tr("Session details"));
     tabs->addTab(m_activityStack, tr("Activity")); tabs->addTab(m_terminal, tr("Terminal"));
+    // The open session's project tasks; the label counts In review and To do.
+    m_tasks = new TasksView(&m_client); tabs->addTab(m_tasks, tr("Tasks"));
+    m_tasksBadge = new TaskTabBadge; tabs->tabBar()->setTabButton(tabs->indexOf(m_tasks), QTabBar::RightSide, m_tasksBadge);
+    connect(m_tasks, &TasksView::countsChanged, m_tasksBadge, &TaskTabBadge::setCounts);
+    connect(m_tasks, &TasksView::notice, this, &SessionsWindow::showNotice);
     m_nativeUi = new NativeUiView(&m_client, [] { return NativeBrowserPlugin::factory(); });
-    tabs->addTab(m_nativeUi, tr("Native UI")); tabs->setTabVisible(2, false);
+    tabs->addTab(m_nativeUi, tr("Native UI")); tabs->setTabVisible(tabs->indexOf(m_nativeUi), false);
     m_processes=new ProcessView(&m_client);tabs->addTab(m_processes,tr("Processes"));tabs->setTabVisible(tabs->indexOf(m_processes),ProcessSettings::enabled());
     connect(m_processes,&ProcessView::refreshRequested,this,&SessionsWindow::inspect);
     for(auto *view:{m_activityView,m_subagentView})connect(view,&ActivityView::processRequested,this,[this](const QString &id){if(!ProcessSettings::enabled())return;m_detailTabs->setCurrentWidget(m_processes);m_processes->selectProcess(id);});
@@ -1237,7 +1243,8 @@ SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
     m_detailsClear->setToolTip(tr("Start this agent's conversation from scratch, like /clear in Terminal. Your unsent draft is kept."));
     connect(m_detailsClear,&QPushButton::clicked,this,&SessionsWindow::clearContext);
     contextRow->addWidget(m_detailsContext,1);contextRow->addWidget(m_detailsClear);detailsLayout->addLayout(contextRow);
-    m_inspector->addTab(m_children, tr("Tasks && agents")); m_inspector->addTab(detailsPage, tr("Details"));
+    // The agent's own plan, not the project's Zerus tasks (the Tasks tab).
+    m_inspector->addTab(m_children, tr("Plan && agents")); m_inspector->addTab(detailsPage, tr("Details"));
     m_inspector->addTab(m_worktrees, tr("Worktrees"));
     connect(m_inspector,&QTabWidget::currentChanged,this,[this]{updateWorktrees();});
     inspectorLayout->addWidget(m_inspector);
@@ -1246,7 +1253,7 @@ SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
     m_inspectorToggle = new QPushButton; m_inspectorToggle->setObjectName("toggleInspector");
     m_inspectorToggle->setProperty("glyph", "inspector"); m_inspectorToggle->setCheckable(true);
     m_inspectorToggle->setProperty("sessionHeaderAction", true);
-    configureWorkspaceIconButton(m_inspectorToggle, "inspector", tr("Show tasks, agents, details and worktrees"), m_dark);
+    configureWorkspaceIconButton(m_inspectorToggle, "inspector", tr("Show plan, agents, details and worktrees"), m_dark);
     tabs->setCornerWidget(m_inspectorToggle);
     connect(m_inspectorToggle, &QPushButton::toggled, this, &SessionsWindow::setInspectorVisible);
     connect(m_inspector, &QTabWidget::currentChanged, this, [](int index) { QSettings().setValue("workspace/inspectorTab", index); });
@@ -1266,7 +1273,8 @@ SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
     });
     detail->addWidget(m_workSplitter, 1); m_detailStack->addWidget(details); m_splitter->addWidget(m_detailStack);
     m_fileDrop=new SessionFileDrop(details,[this]()->MessageComposer *{
-        if(!selected()||m_detailTabs->currentWidget()==m_terminal)return nullptr;
+        // A task is text only: nothing is dropped on the Tasks tab.
+        if(!selected()||m_detailTabs->currentWidget()==m_terminal||m_detailTabs->currentWidget()==m_tasks)return nullptr;
         if(m_detailTabs->currentIndex()==0&&!m_subagentId.isEmpty())return m_subagentComposer->isInputVisible()?m_subagentComposer:nullptr;
         return m_composer;
     },[this](MessageComposer *composer){if(composer==m_composer)closeSubagent();m_detailTabs->setCurrentIndex(0);},
@@ -1633,6 +1641,7 @@ void SessionsWindow::applyTheme()
         QWidget#settingsPage, QWidget#recoverySettingsPage, QWidget#sessionsWindow, QWidget#machinesPage, QWidget#accountsPage, QDialog#accountDialog, QDialog#accountPermissionsDialog, QDialog#machineSetupDialog, QDialog#projectsPage, QDialog#projectColorDialog, QDialog#projectFolderDialog, QDialog#deleteProjectDialog, QDialog#newSessionDialog, QDialog#folderBrowser, QDialog#forkSessionDialog, QDialog#renameSessionDialog, QDialog#workspaceSettingsDialog, QDialog#sessionFileDialog, QDialog#swarmDialog, QDialog#speechProfileEditor { background:%1; color:%2; }
         QColorDialog#workspaceColorDialog { background:%1; color:%2; }
         QDialog#savedDraftsDialog { background:%1; color:%2; }
+        QDialog#taskDialog, QDialog#taskCancelDialog, QDialog#taskSettingsDialog { background:%1; color:%2; }
         QWidget#automaticApprovalSettings { background:transparent; color:%2; }
         QPlainTextEdit#approvalJournalDetail { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:10px; selection-background-color:%10; selection-color:%2; }
         QTableWidget#approvalJournal::item { padding:7px 10px; border-bottom:1px solid %4; }
@@ -1797,6 +1806,7 @@ void SessionsWindow::applyTheme()
     m_machineFilter->setTheme(m_dark); m_projectFilter->setTheme(m_dark); m_dashboard->setTheme(m_dark); m_searchResults->setTheme(m_dark); m_activityView->setTheme(m_dark); m_composer->setTheme(m_dark); m_question->setTheme(m_dark); m_terminal->setTheme(m_dark);
     m_subagentView->setTheme(m_dark); m_subagentComposer->setTheme(m_dark);
     m_processes->setTheme(m_dark);
+    m_tasks->setTheme(m_dark); m_tasksBadge->setTheme(m_dark);
     m_childrenHtml.clear(); m_sessions->viewport()->update(); renderDetails();
 }
 
@@ -1946,7 +1956,7 @@ void SessionsWindow::setInspectorVisible(bool visible)
     const QSignalBlocker blocker(m_inspectorToggle); m_inspectorToggle->setChecked(visible);
     m_inspectorToggle->setProperty("glyph", visible ? "collapse-panel" : "inspector");
     m_inspectorToggle->setIcon(workspaceIcon(m_inspectorToggle->property("glyph").toString(), QColor(m_muted)));
-    m_inspectorToggle->setAccessibleName(visible ? tr("Collapse side panel") : tr("Show tasks, agents, details and worktrees"));
+    m_inspectorToggle->setAccessibleName(visible ? tr("Collapse side panel") : tr("Show plan, agents, details and worktrees"));
     m_inspectorToggle->setToolTip(ShortcutRegistry::instance().tooltip(m_inspectorToggle->accessibleName(), "panel.toggle"));
     m_inspectorToggle->show();
     if (restoreFocus) m_inspectorToggle->setFocus(keyboardFocus ? Qt::TabFocusReason : Qt::MouseFocusReason);
@@ -3224,11 +3234,31 @@ template<typename Control> void explainContextAction(Control *control, const QSt
 }
 }
 
+void SessionsWindow::updateTasks(const Entry *entry)
+{
+    if (!entry || entry->session.state == "archived") { m_tasks->setContext({}, {}, {}, {}, {}, {}); return; }
+    // Sessions outside every catalog project have no tasks.
+    const QString projectId = m_organization.groupFor(entry->identity);
+    const auto *project = m_organization.group(projectId);
+    const bool real = project && projectId != QLatin1String("ungrouped");
+    QList<TaskSession> sessions;
+    for (const auto &other : std::as_const(m_entries)) {
+        if (other.session.state == "archived" || m_organization.groupFor(other.identity) != projectId) continue;
+        TaskSession session{other.host, other.session.name, status(other.session, other.online), {}, true};
+        session.running = other.online && other.session.state == "running" && other.session.processState != "exited";
+        if (!session.running) session.blocker = tr("The agent is not running. Resume it before sending.");
+        else if (other.session.needsAction()) session.blocker = tr("agent is waiting for input, approval, or has an unknown state; open Terminal");
+        sessions << session;
+    }
+    m_tasks->setContext(real ? projectId : QString(), real ? project->name : QString(), entry->host, entry->session.name, entry->key, sessions);
+}
+
 void SessionsWindow::renderDetails()
 {
     updateSessionLoading();
     m_compactCancel->setVisible(m_compacts.contains(m_selectedKey) || (m_startSend.key == m_selectedKey && !m_startSend.key.isEmpty()));
     const auto *entry = selected(); m_detailStack->setCurrentIndex(entry ? 1 : 0);
+    updateTasks(entry);
     const auto recoveryDetails=entry && entry->session.state != "archived"
         && m_details.value("provider_status_at").toDouble() >= entry->session.providerStatusAt ? m_details : QJsonObject();
     const auto recoveryState=recoveryDetails.value("recovery").toObject().value("state").toString();
@@ -3469,10 +3499,11 @@ void SessionsWindow::renderDetails()
     const bool terminalTab = terminalAvailable && m_subagentId.isEmpty();
     // Loads the optional plugin on the first DeepSeek session, not at startup.
     const bool nativeTab = s.cmd == "dsh" && m_subagentId.isEmpty() && NativeBrowserPlugin::factory();
-    if (m_detailTabs->isTabVisible(1) != terminalTab) m_detailTabs->setTabVisible(1, terminalTab);
-    if (m_detailTabs->isTabVisible(2) != nativeTab) m_detailTabs->setTabVisible(2, nativeTab);
-    if (!terminalTab && m_detailTabs->currentIndex() == 1) m_detailTabs->setCurrentIndex(0);
-    if (!nativeTab && m_detailTabs->currentIndex() == 2) m_detailTabs->setCurrentIndex(0);
+    const int terminalIndex = m_detailTabs->indexOf(m_terminal), nativeIndex = m_detailTabs->indexOf(m_nativeUi);
+    if (m_detailTabs->isTabVisible(terminalIndex) != terminalTab) m_detailTabs->setTabVisible(terminalIndex, terminalTab);
+    if (m_detailTabs->isTabVisible(nativeIndex) != nativeTab) m_detailTabs->setTabVisible(nativeIndex, nativeTab);
+    if (!terminalTab && m_detailTabs->currentIndex() == terminalIndex) m_detailTabs->setCurrentIndex(0);
+    if (!nativeTab && m_detailTabs->currentIndex() == nativeIndex) m_detailTabs->setCurrentIndex(0);
     m_terminal->setAvailable(entry->online && s.state == "running" && terminalAvailable,
         !entry->online ? tr("Machine unavailable. Reconnect when it is online.") :
         !terminalAvailable ? NativeSessionCapabilities::terminalReason(s,m_details) : tr("Resume this session before opening its terminal."));
@@ -5064,7 +5095,7 @@ void SessionsWindow::terminateSession()
 void SessionsWindow::showEvent(QShowEvent *event)
 {
     QWidget::showEvent(event); rebuild(); if (m_dashboardStale) updateDashboard();
-    m_timer.start(); m_readTimer.start(); emit refreshRequested(); inspect();
+    m_timer.start(); m_readTimer.start(); emit refreshRequested(); inspect(); m_tasks->setPolling(true);
 }
 bool SessionsWindow::eventFilter(QObject *watched, QEvent *event)
 {
@@ -5089,7 +5120,7 @@ bool SessionsWindow::eventFilter(QObject *watched, QEvent *event)
     }
     return QWidget::eventFilter(watched, event);
 }
-void SessionsWindow::hideEvent(QHideEvent *event) { m_readTimer.stop(); m_readCandidate.clear(); m_timer.stop(); m_terminal->disconnectSession(); QWidget::hideEvent(event); }
+void SessionsWindow::hideEvent(QHideEvent *event) { m_readTimer.stop(); m_readCandidate.clear(); m_timer.stop(); m_tasks->setPolling(false); m_terminal->disconnectSession(); QWidget::hideEvent(event); }
 void SessionsWindow::closeEvent(QCloseEvent *event)
 {
     QSettings settings; settings.setValue("sessions/geometry", saveGeometry()); settings.setValue("workspace/splitter", m_splitter->saveState());
````

Apply this change to `tray/src/SessionsWindow.h` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionsWindow.h b/tray/src/SessionsWindow.h
index ce72d4f..161f203 100644
--- a/tray/src/SessionsWindow.h
+++ b/tray/src/SessionsWindow.h
@@ -147,6 +147,8 @@ private:
     bool entryNeedsAttention(const Entry &entry) const;
     bool entryHasDraft(const Entry &entry) const;
     void renderDetails();
+    // The Tasks tab follows the open session's project and its sessions.
+    void updateTasks(const Entry *entry);
     void updateWorktrees();
     void filterFolder(const QString &host, const QString &path, bool checkout);
     void openRelatedSession(const QString &host, const QString &name, const QString &archive);
@@ -453,6 +455,8 @@ private:
     QTextBrowser *m_children = nullptr, *m_info = nullptr;
     ActivityView *m_activityView = nullptr;
     class ProcessView *m_processes = nullptr;
+    class TasksView *m_tasks = nullptr;
+    class TaskTabBadge *m_tasksBadge = nullptr;
     QWidget *m_nativeSignIn = nullptr;
     // A stopped OpenCode service: started only by this explicit action.
     QWidget *m_openCodeService = nullptr;
````

Apply this change to `tray/src/SessionsWindowGoTo.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionsWindowGoTo.cpp b/tray/src/SessionsWindowGoTo.cpp
index 9557f7d..a1d6450 100644
--- a/tray/src/SessionsWindowGoTo.cpp
+++ b/tray/src/SessionsWindowGoTo.cpp
@@ -1,6 +1,7 @@
 // Go to (Ctrl+K) and the keyboard cheat sheet (F1) of the workspace window.
 // See docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md, "Go to".
 #include "SessionsWindow.h"
+#include "TasksView.h"
 #include "AccountsPage.h"
 #include "ComposerDraftStore.h"
 #include "GoToDialog.h"
@@ -147,6 +148,9 @@ void SessionsWindow::openDraftSession(const QString &draftKey)
     }
     if (!m_subagentId.isEmpty()) closeSubagent();
     if (owner.kind == ComposerDraftOwner::Kind::Message && m_composer->isInputVisible()) m_composer->editor()->setFocus(Qt::OtherFocusReason);
+    if (owner.kind == ComposerDraftOwner::Kind::Task) {
+        m_detailTabs->setCurrentWidget(m_tasks); m_tasks->composer()->editor()->setFocus(Qt::OtherFocusReason);
+    }
 }
 
 void SessionsWindow::activateGoTo(const GoToItem &item)
````

Apply this change to `tray/src/SessionsWindowShortcuts.cpp` (save it and run `git apply`, or edit by hand):

````diff
diff --git a/tray/src/SessionsWindowShortcuts.cpp b/tray/src/SessionsWindowShortcuts.cpp
index b2a1215..0640869 100644
--- a/tray/src/SessionsWindowShortcuts.cpp
+++ b/tray/src/SessionsWindowShortcuts.cpp
@@ -2,6 +2,7 @@
 // rebound from ShortcutRegistry. See Shortcuts.h and
 // docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md.
 #include "SessionsWindow.h"
+#include "TasksView.h"
 #include "ActivityView.h"
 #include "ContentScale.h"
 #include "MachinesPage.h"
@@ -408,6 +409,7 @@ QWidget *SessionsWindow::viewFocusTarget() const
 {
     QWidget *view = m_detailTabs->currentWidget();
     if (view == m_activityStack) return (m_activityStack->currentIndex() == 1 ? m_subagentView : m_activityView)->browser();
+    if (view == m_tasks) return m_tasks->composer()->editor();
     return focusTarget(view);
 }
 
````

- [ ] **Step 4: Run them to see them pass**

Run:
```bash
cmake --build .ci-build/gui -j 8
ctest --test-dir .ci-build/gui --parallel 8 --timeout 600 --output-on-failure
```
Expected: every suite passes, `tasksTabFollowsTheOpenSessionsProject` and `multiSelectionKeepsConversationAndGroupMenu` included. If `colorPickerUsesWorkspaceThemeAndCancelsOutside(dark)` fails in shard 7, run that shard on the branch's previous commit: it fails there too on affected machines (Known limits).

- [ ] **Step 5: Check the repository**

Run: `python3 scripts/ci/check-source.py`
Expected: `Source syntax, privacy, child process and release version checks passed.`

- [ ] **Step 6: Commit**

```bash
git add docs/reference.md \
  tray/src/SessionsWindow.cpp \
  tray/src/SessionsWindow.h \
  tray/src/SessionsWindowGoTo.cpp \
  tray/src/SessionsWindowShortcuts.cpp \
  tray/tests/test_sessionswindow.cpp
git commit -m "Put the Tasks tab beside Activity and Terminal"
```

---
