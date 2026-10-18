#!/usr/bin/env python3
"""`hgs task` through the real CLI: an isolated catalog and state; sends go to
a scripted agent on a private tmux socket."""
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
        self.assertIn("pinned to its own session", self.hgs("add", "--project", "orchard", "--target", "any", text="x", session="codex/api", ok=False))

    def test_settings(self):
        settings = self.hgs("settings", "--project", "orchard")["settings"]
        self.assertTrue(settings["task_template"].startswith("[Zerus task {ref}]"))
        settings.update(agents_other=False, agent_limit=4)
        self.assertEqual(self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings))["settings"]["agent_limit"], 4)
        settings["task_template"] = "{prompt}"
        self.assertIn("must start with", self.hgs("settings", "--project", "orchard", "--set", text=json.dumps(settings), ok=False))

    def test_concurrent_adds_all_land(self):
        processes = [subprocess.Popen([str(HGS), "task", "add", "--project", "orchard"], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, text=True)
                     for _ in range(6)]
        # Every process has its text before any of them finishes: all six race for the project.
        for index, process in enumerate(processes):
            process.stdin.write("task %d" % index)
            process.stdin.close()
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


if __name__ == "__main__":
    unittest.main()
