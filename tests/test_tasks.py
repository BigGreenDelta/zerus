#!/usr/bin/env python3
"""`hgs task` through the real CLI: an isolated catalog and state; sends go to
a scripted agent on a private tmux socket."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
TMUX = shutil.which("tmux")

# Records every byte the pane receives; never echoes, so the composer stays empty.
AGENT = r'''
import os, pathlib, sys, termios, tty
root = pathlib.Path(os.environ['TASK_FIXTURE'])
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, b'\x1b[?2004h\x1b[2J\x1b[H' + '› '.encode())
(root / 'agent-pid').write_text(str(os.getpid()))
while True:
    value = os.read(fd, 65536)
    if not value: break
    with (root / 'received').open('ab') as file: file.write(value)
'''


class Fixture(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-tasks-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.bin = self.root / ".local/bin"
        self.bin.mkdir(parents=True)
        self.state = self.root / "state"
        self.state.mkdir()
        self.socket = self.root / "socket"
        self.env = dict(os.environ, HOME=str(self.root), TASK_FIXTURE=str(self.root),
                        HGS_CONFIG_DIR=str(self.root / "config"), HGS_STATE_DIR=str(self.state),
                        HGS_SELF="test", HGS_TAB="0", PATH=str(self.bin) + ":" + os.environ["PATH"])
        for variable in ("TMUX", "TMUX_PANE", "HGS_RUN_ID", "HGS_SESSION", "HGS_EXECUTABLE", "HGS_SSH", "HGS_PEERS"):
            self.env.pop(variable, None)
        # hgs puts $HOME/.local/bin first on PATH: the shim there keeps tmux private.
        # Each test stops its own private server, never another one.
        if TMUX:
            self.addCleanup(lambda: subprocess.run([TMUX, "-S", str(self.socket), "kill-server"], capture_output=True))
            self.script("tmux", '#!/bin/sh\nexec ' + TMUX + ' -S "' + str(self.socket) + '" -f /dev/null "$@"\n')
        else:
            self.script("tmux", "#!/bin/sh\nexit 1\n")
        catalog = {"projects": [{"id": "project-1", "name": "orchard", "color": "#123456", "folders": [], "sessions": []},
                                {"id": "project-2", "name": "team/web", "color": "#654321", "folders": [], "sessions": []}]}
        result = subprocess.run([str(HGS), "swarm", "initialize"], input=json.dumps(catalog), env=self.env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def script(self, name, value):
        path = self.bin / name
        path.write_text(value)
        path.chmod(0o755)

    def hgs(self, *args, text=None, ok=True, session=None):
        env = dict(self.env)
        if session:
            # An agent, as hooks know it: its session and its run.
            env.update(HGS_SESSION=session, HGS_RUN_ID="run-test")
        result = subprocess.run([str(HGS), "task", *args], input=text, env=env, text=True,
                                capture_output=True, timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr

    def add(self, text, *args, session=None):
        return self.hgs("add", "--project", "orchard", *args, text=text, session=session)["task"]

    def ids(self, stage):
        listing = self.hgs("list", "--project", "orchard", "--stage", stage)
        return [task["id"] for task in listing["tasks"]]


class TaskCommands(Fixture):
    def test_add_list_and_get(self):
        task = self.add("Add rate limiting")
        self.assertRegex(task["id"], r"^T-[0-9a-z]{6}$")
        self.assertEqual((task["stage"], task["target"], task["author"]), ("todo", {"kind": "any"}, {"kind": "person"}))
        self.assertEqual(task["ref"], "orchard/" + task["id"])
        backlog = self.add("Later", "--backlog")
        self.assertEqual(self.ids("todo"), [task["id"]])
        self.assertEqual(self.ids("backlog"), [backlog["id"]])
        got = self.hgs("get", task["ref"])["task"]
        self.assertEqual(got["text"], "Add rate limiting")
        self.assertEqual([entry["kind"] for entry in got["log"]], ["created"])
        self.assertEqual(self.hgs("get", task["id"], "--project", "project-1")["task"]["uuid"], got["uuid"])
        self.assertIn("no project", self.hgs("list", "--project", "missing", ok=False))
        self.assertIn("no task", self.hgs("get", "orchard/T-zzzzzz", ok=False))

    def test_order_and_positions(self):
        first, second, third = (self.add(t)["id"] for t in ("one", "two", "three"))
        self.assertEqual(self.ids("todo"), [first, second, third])
        self.hgs("move", "orchard/" + third, "--top")
        self.assertEqual(self.ids("todo"), [third, first, second])
        self.hgs("move", "orchard/" + third, "--after", first)
        self.assertEqual(self.ids("todo"), [first, third, second])
        fourth = self.add("four", "--position", "1")["id"]
        self.assertEqual(self.ids("todo"), [fourth, first, third, second])

    def test_stages_edits_targets_and_comments(self):
        ref = self.add("Draft")["ref"]
        self.hgs("edit", ref, text="Final text")
        self.hgs("stage", ref, "backlog")
        self.hgs("stage", ref, "todo")
        self.assertIn("cannot go from To do to Done", self.hgs("stage", ref, "done", ok=False))
        self.assertIn(ref.split("/")[1] + " is already in To do", self.hgs("stage", ref, "todo", ok=False))
        self.hgs("target", ref, "codex/api")
        task = self.hgs("comment", ref, text="Use Redis")["task"]
        self.assertEqual(task["text"], "Final text")
        self.assertEqual(task["target"]["session"], "codex/api")
        self.assertEqual([e["kind"] for e in task["log"]], ["created", "edited", "stage", "stage", "target", "comment"])
        self.assertIn("longer than 64 KiB", self.hgs("comment", ref, text="x" * (64 * 1024 + 1), ok=False))

    def test_an_agent_files_tasks_for_its_own_session(self):
        task = self.add("Document the setup", session="codex/api")
        self.assertEqual(task["author"]["kind"], "agent")
        self.assertEqual(task["target"]["session"], "codex/api")
        # The author and the history's actors name their computer like sessions do.
        self.assertEqual(task["author"]["host"], "")
        self.assertEqual(task["log"][0]["actor"]["host"], "")
        self.assertIn("pinned to its own session", self.hgs("add", "--project", "orchard", "--target", "any", text="x", session="codex/api", ok=False))

    def test_settings(self):
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        self.assertTrue(settings["task_template"].startswith("[Zerus task {ref}]"))
        settings.update(agents_other=False, agent_limit=4)
        self.assertEqual(self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings))["settings"]["agent_limit"], 4)
        settings["task_template"] = "{prompt}"
        self.assertIn("must start with", self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings), ok=False))

    def test_settings_change_only_the_fields_given(self):
        self.hgs("settings", "--project", "orchard", "--set", text=json.dumps({"agent_limit": 2}))
        self.hgs("settings", "--project", "orchard", "--set", text=json.dumps({"agents_other": False}))
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        self.assertEqual((settings["agent_limit"], settings["agents_other"]), (2, False))
        self.assertTrue(settings["task_template"].startswith("[Zerus task {ref}]"))
        typo = self.hgs("settings", "--project", "orchard", "--set", text=json.dumps({"agent_limt": 3}), ok=False)
        self.assertIn("agent_limt", typo)

    def test_concurrent_adds_all_land(self):
        self.hgs("list", "--project", "orchard")  # creates the project's lock file
        lock = self.state / "replica" / hashlib.sha256(b"project-1").hexdigest() / ".lock"
        with open(lock, "a") as held:
            # The test holds the project lock while all six start, so they all
            # wait for it at once and race when it is released.
            fcntl.flock(held, fcntl.LOCK_EX)
            processes = [subprocess.Popen([str(HGS), "task", "add", "--project", "orchard"], stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, text=True)
                         for _ in range(6)]
            for index, process in enumerate(processes):
                process.stdin.write("task %d" % index)
                process.stdin.close()
            time.sleep(.3)
            self.assertEqual([process.poll() for process in processes], [None] * 6, "an add did not wait for the lock")
            fcntl.flock(held, fcntl.LOCK_UN)
        for process in processes:
            self.assertEqual(process.wait(timeout=30), 0, process.stderr.read())
        tasks = self.hgs("list", "--project", "orchard")["tasks"]
        self.assertEqual(sorted(t["text"] for t in tasks), ["task %d" % i for i in range(6)])
        self.assertEqual(len({t["id"] for t in tasks}), 6)
        self.assertEqual(len({t["rank"] for t in tasks}), 6)

    def test_texts_that_cannot_be_sent_are_refused(self):
        self.assertIn("control characters", self.hgs("add", "--project", "orchard", text="\x1b[31mred", ok=False))
        self.assertIn("control characters", self.hgs("add", "--project", "orchard", text="line one\r\n", ok=False))
        # Fits 64 KiB alone, but not inside the task prompt.
        self.assertIn("rendered prompt", self.hgs("add", "--project", "orchard", text="x" * 65450, ok=False))
        ref = self.add("Short")["ref"]
        self.assertIn("rendered prompt", self.hgs("edit", ref, text="x" * 65450, ok=False))
        self.assertEqual(self.hgs("get", ref)["task"]["text"], "Short")
        self.assertEqual(self.ids("todo"), [ref.split("/")[-1]])

    def test_a_task_that_changes_list_goes_to_its_top(self):
        first, second = self.add("one")["id"], self.add("two")["id"]
        later, other = self.add("later", "--backlog")["id"], self.add("other", "--backlog")["id"]
        self.hgs("stage", "orchard/" + later, "todo")
        self.assertEqual((self.ids("todo"), self.ids("backlog")), ([later, first, second], [other]))
        self.hgs("stage", "orchard/" + second, "backlog")
        self.assertEqual((self.ids("todo"), self.ids("backlog")), ([later, first], [second, other]))

    def test_tasks_name_the_computer_of_their_session(self):
        here = self.add("Local work", "--target", "codex/api")
        self.assertEqual(here["target"]["host"], "")
        refused = self.hgs("add", "--project", "orchard", "--target", "codex/api", "--host", "orchard-mac",
                           text="Remote work", ok=False)
        self.assertIn("orchard-mac is not a verified computer", refused)
        # --host names the computer of a --target session, so it needs one.
        for args in (("--host", "orchard-mac"), ("--target", "any", "--host", "orchard-mac")):
            self.assertIn("--host needs a --target session", self.hgs("add", "--project", "orchard", *args, text="x", ok=False))
        self.assertIn("--host needs a --target session", self.hgs("target", here["ref"], "any", "--host", "orchard-mac", ok=False))
        # This computer verified orchard-mac itself: tasks can be pinned to its sessions.
        catalog = self.root / "config" / "swarm" / "catalog.json"
        data = json.loads(catalog.read_text())
        mac = str(uuid.uuid4())
        data.setdefault("bindings", {})["orchard-mac"] = {"id": mac, "verified": True}
        catalog.write_text(json.dumps(data))
        there = self.hgs("add", "--project", "orchard", "--target", "codex/web", "--host", "orchard-mac",
                         text="Remote work")["task"]
        self.assertEqual((there["target"]["machine"], there["target"]["host"]), (mac, "orchard-mac"))
        moved = self.hgs("target", here["ref"], "codex/web", "--host", "orchard-mac")["task"]
        self.assertEqual(moved["target"]["host"], "orchard-mac")
        listed = {t["id"]: t for t in self.hgs("list", "--project", "orchard")["tasks"]}
        self.assertEqual(listed[there["id"]]["target"]["host"], "orchard-mac")
        self.assertIn("on another computer", self.hgs("send", there["ref"], ok=False))

    def test_a_project_name_with_a_slash(self):
        task = self.hgs("add", "--project", "team/web", text="Ship it")["task"]
        self.assertEqual(task["ref"], "team/web/" + task["id"])
        self.assertEqual(self.hgs("get", task["ref"])["task"]["uuid"], task["uuid"])

    def test_a_damaged_task_is_reported_not_fatal(self):
        self.add("Fine")
        project = next(p for p in (self.state / "replica").iterdir() if p.is_dir())
        (project / "tasks" / (str(uuid.uuid4()) + ".loro")).write_bytes(b"broken")
        listing = self.hgs("list", "--project", "orchard")
        self.assertEqual(len(listing["tasks"]), 1)
        self.assertEqual(len(listing["damaged"]), 1)


@unittest.skipUnless(TMUX, "tmux is required for delivery")
class TaskDelivery(Fixture):
    def setUp(self):
        super().setUp()
        (self.root / "agent.py").write_text(AGENT)
        self.name = "codex/task-test"
        self.run_id = str(uuid.uuid4())
        self.tmux("new-session", "-d", "-s", self.name, "-x", "100", "-y", "30", "python3", str(self.root / "agent.py"))
        self.addCleanup(self.stop)
        for _ in range(200):
            pid_file = self.root / "agent-pid"
            if pid_file.exists() and pid_file.read_text().strip():
                break
            time.sleep(.02)
        pid = int((self.root / "agent-pid").read_text())
        start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="],
                                        env=dict(self.env, LC_ALL="C", TZ="UTC"), text=True).strip()
        pane = self.tmux("display-message", "-p", "-t", "=" + self.name + ":", "#{pane_id}").strip()
        self.tmux("set-option", "-t", "=" + self.name + ":", "@hgs_run", self.run_id)
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest() + ".json")
        self.record_path.write_text(json.dumps(dict(
            version=1, name=self.name, agent="codex", run_id=self.run_id, conversation_id=str(uuid.uuid4()),
            pane=pane, pid=pid, process_start=start, activity="idle", phase="idle", active_tools={},
            subagents={}, last_event_at=time.time(), created=time.time())))

    def stop(self):
        # kill-server acknowledges before its panes exit; wait for them before the
        # fixture home is removed, so no pane writes into a deleted directory.
        panes = subprocess.run([str(self.bin / "tmux"), "list-panes", "-a", "-F", "#{pane_pid}"], env=self.env,
                               capture_output=True, text=True, timeout=5).stdout.split()
        pids = {pid for pid in panes if pid.isdigit()}
        subprocess.run([TMUX, "-S", str(self.socket), "kill-server"], capture_output=True)
        deadline = time.monotonic() + 2
        while pids and time.monotonic() < deadline:
            status = subprocess.run(["ps", "-p", ",".join(sorted(pids)), "-o", "pid=,stat="],
                                    capture_output=True, text=True, timeout=5)
            pids = {parts[0] for line in status.stdout.splitlines()
                    if len(parts := line.split()) >= 2 and not parts[1].startswith(("Z", "X"))}
            if pids:
                time.sleep(.02)
        self.assertFalse(pids, "owned fixture panes survived tmux shutdown")

    def tmux(self, *args):
        result = subprocess.run([str(self.bin / "tmux"), *args], env=self.env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def received(self):
        path = self.root / "received"
        return path.read_bytes().decode(errors="replace") if path.exists() else ""

    def wait_received(self, text):
        for _ in range(250):
            if text in self.received():
                return
            time.sleep(.02)
        self.fail("the agent never received " + repr(text) + "; got " + repr(self.received()))

    def confirm(self):
        # The agent's next hook event confirms the pasted input.
        record = json.loads(self.record_path.read_text())
        record["last_event_at"] = record.get("input_pending_at", time.time()) + 1
        self.record_path.write_text(json.dumps(record))

    def settings(self, **changes):
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        settings.update(changes)
        self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings))

    def test_send_report_and_a_second_send(self):
        task = self.add("Add rate limiting", "--target", self.name)
        sent = self.hgs("send", task["ref"])
        self.assertEqual(sent["task"]["stage"], "active")
        self.assertEqual(sent["task"]["session"]["session"], self.name)
        # History entries name the computer of their session too.
        entry = next(e for e in sent["task"]["log"] if e["kind"] == "sent")
        self.assertEqual((entry["session"]["session"], entry["session"]["host"]), (self.name, ""))
        self.assertEqual(sent["receipt"]["status"], "submitted")
        self.wait_received("[Zerus task orchard/" + task["id"] + "]")
        self.wait_received("Add rate limiting")
        self.wait_received("`hgs task report orchard/" + task["id"] + "`")
        self.assertIn("already In progress in " + self.name, self.hgs("send", task["ref"], ok=False))
        done = self.hgs("report", task["ref"], text="Token bucket; 6 tests.", session=self.name)
        self.assertTrue(done["moved_to_review"])
        self.assertEqual((done["task"]["stage"], done["task"]["summary"]), ("review", "Token bucket; 6 tests."))
        again = self.hgs("report", task["ref"], text="Also updated the README.", session=self.name)
        self.assertFalse(again["moved_to_review"])

    def test_an_any_session_task_needs_a_session(self):
        task = self.add("Anywhere")
        self.assertIn("--session", self.hgs("send", task["ref"], ok=False))
        sent = self.hgs("send", task["ref"], "--session", self.name)["task"]
        self.assertEqual((sent["stage"], sent["target"]), ("active", {"kind": "any"}))
        self.wait_received("Anywhere")

    def test_session_rules(self):
        pinned = self.add("Pinned", "--target", self.name)
        self.assertIn("pinned to " + self.name, self.hgs("send", pinned["ref"], "--session", "codex/other", ok=False))
        anywhere = self.add("Anywhere")
        # An agent that names the session also pins the task there.
        sent = self.hgs("send", anywhere["ref"], "--session", self.name, session="codex/planner")["task"]
        self.assertEqual(sent["target"]["session"], self.name)

    def test_a_refused_send_changes_nothing(self):
        task = self.add("Nowhere", "--target", "codex/missing")
        self.assertIn("codex/missing", self.hgs("send", task["ref"], ok=False))
        after = self.hgs("get", task["ref"])["task"]
        self.assertEqual((after["stage"], [e["kind"] for e in after["log"]]), ("todo", ["created"]))

    def test_project_settings_bound_agents(self):
        self.settings(agents_other=False)
        other = self.add("Elsewhere", "--target", "codex/other")
        self.assertIn("only in their own session", self.hgs("send", other["ref"], session=self.name, ok=False))
        self.assertEqual(self.received(), "")
        own = self.add("Mine")
        self.hgs("send", own["ref"], session=self.name)
        self.wait_received("Mine")
        self.confirm()
        self.settings(agents_other=True, agent_limit=1)
        another = self.add("One more")
        self.assertIn("1 tasks started by agents per hour", self.hgs("send", another["ref"], session=self.name, ok=False))
        # People are not limited.
        self.assertEqual(self.hgs("send", another["ref"], "--session", self.name)["task"]["stage"], "active")

    def test_cancel_moves_to_review_and_tells_the_agent(self):
        task = self.add("Refactor the parser", "--target", self.name)
        self.hgs("send", task["ref"])
        self.confirm()
        self.assertIn("a task you were sent", self.hgs("cancel", task["ref"], session=self.name, ok=False))
        out = self.hgs("cancel", task["ref"], "--reason", "Wrong approach")
        self.assertEqual((out["task"]["stage"], out["task"]["cancel_reason"]), ("review", "Wrong approach"))
        self.assertEqual([(e["kind"], e.get("text", "")) for e in out["task"]["log"][-2:]],
                         [("cancelled", ""), ("comment", "Wrong approach")])
        self.assertTrue(out["cancel_message"]["sent"])
        self.wait_received("[Zerus task orchard/" + task["id"] + " cancelled]")
        self.assertNotIn("Wrong approach", self.received())

    def test_a_refused_cancel_message_still_cancels(self):
        task = self.add("Refactor the parser", "--target", self.name)
        self.hgs("send", task["ref"])
        # No hook event since the paste: hgs refuses further input.
        out = self.hgs("cancel", task["ref"])
        self.assertEqual(out["task"]["stage"], "review")
        self.assertFalse(out["cancel_message"]["sent"])
        self.assertIn("not been confirmed", out["cancel_message"]["error"])
        self.assertEqual(out["task"]["log"][-1]["kind"], "note")


if __name__ == "__main__":
    unittest.main()
