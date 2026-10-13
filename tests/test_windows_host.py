#!/usr/bin/env python3
"""The Windows session host (spec D1-D6, D9, D13) through `hgs.exe __host`.

Windows only. HGS_TEST_BIN names the hgs.exe under test (default
target/debug/hgs.exe); the fake agent cases also need
target/debug/examples/fake_agent.exe (`cargo build --locked --example
fake_agent`; HGS_TEST_FAKE_AGENT names another build).

Every case has its own HGS_STATE_DIR, USERPROFILE and HOME, so it reaches only
a host it started itself, and stops what it started: its sessions are killed,
the host is asked to stop and, as a last resort, its process is ended (the
host's kill-on-close job then ends anything left). Consoles stay hidden.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs.exe")).resolve()
FAKE_AGENT = Path(os.environ.get("HGS_TEST_FAKE_AGENT",
                                 REPO / "target/debug/examples/fake_agent.exe")).resolve()
HIDDEN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
# The host's linger (registry::LINGER) plus margin.
IDLE_EXIT = 30
LIMIT = 30


@unittest.skipUnless(os.name == "nt", "the session host runs on Windows only")
class HostCase(unittest.TestCase):
    def setUp(self):
        if not HGS.is_file():
            raise AssertionError(f"{HGS} is missing; run cargo build --locked")
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-host-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.home = self.root / "home"
        self.work = self.root / "work"
        for path in (self.state, self.home, self.work):
            path.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.upper().startswith(("HGS_", "FAKE_AGENT_", "TMUX"))}
        self.env.update(HGS_STATE_DIR=str(self.state), USERPROFILE=str(self.home), HOME=str(self.home))
        self.unique = f"hgs-host-{os.getpid()}-{time.monotonic_ns()}"
        self.launches = 0
        self.addCleanup(self.stop_host)

    # The host and its tools.

    def run_hgs(self, *args, timeout=60):
        return subprocess.run([str(HGS), "__host", *args], env=self.env, capture_output=True, text=True,
                              timeout=timeout, creationflags=HIDDEN)

    def call(self, request, start=False):
        """The exit code and the printed response of one request."""
        args = ["call", "--start"] if start else ["call"]
        result = self.run_hgs(*args, json.dumps(request))
        response = json.loads(result.stdout) if result.stdout.strip() else None
        return result.returncode, response, result.stderr

    def ok(self, request, start=False):
        code, response, stderr = self.call(request, start)
        self.assertEqual(code, 0, f"{request}: {response} {stderr}")
        return response["ok"]

    def error(self, request):
        code, response, stderr = self.call(request)
        self.assertEqual(code, 1, f"{request}: {response} {stderr}")
        return response["error"]

    def status(self):
        result = self.run_hgs("status")
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def wait_for(self, what, predicate, timeout=LIMIT):
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value:
                return value
            if time.monotonic() > deadline:
                self.fail(f"timed out waiting for {what}")
            time.sleep(0.1)

    def stop_host(self):
        status = self.status()
        if not status.get("running"):
            return
        for pane in self.ok({"op": "list"})["panes"]:
            self.call({"op": "kill", "target": {"pane": pane["pane"]}})
        deadline = time.monotonic() + LIMIT
        while time.monotonic() < deadline and self.ok({"op": "list"})["sessions"]:
            time.sleep(0.2)
        self.call({"op": "stop_host"})
        deadline = time.monotonic() + LIMIT
        while time.monotonic() < deadline:
            if not self.status().get("running"):
                return
            time.sleep(0.2)
        os.kill(status["host"]["pid"], signal.SIGTERM)

    # Sessions.

    def create(self, name, argv, launch_id=None, size=(100, 30), start=True):
        if launch_id is None:
            self.launches += 1
            launch_id = f"{self.unique}-{self.launches}"
        request = {
            "op": "create_session",
            "launch_id": launch_id,
            "name": name,
            "cwd": str(self.work),
            "env": [f"{key}={value}" for key, value in self.env.items()],
            "argv": [str(word) for word in argv],
            "size": {"cols": size[0], "rows": size[1]},
        }
        return self.ok(request, start=start)

    def cmd(self, name):
        """A cmd.exe session with a prompt of its own."""
        prompt = f"{self.unique}-{name}"
        comspec = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
        created = self.create(name, [comspec, "/d", "/q", "/k", f"prompt {prompt}$G"])
        self.wait_for("the prompt", lambda: prompt + ">" in self.capture(created["pane"]))
        return created, prompt

    def capture(self, pane, styled=False):
        return self.ok({"op": "capture", "target": {"pane": pane}, "styled": styled})["text"]

    def info(self, pane):
        return self.ok({"op": "pane_info", "target": {"pane": pane}})

    def keys(self, pane, *keys):
        self.ok({"op": "send_keys", "target": {"pane": pane}, "keys": list(keys)})

    def attach(self, request, steps):
        result = self.run_hgs("attach", json.dumps(request), json.dumps(steps))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)


class Lifecycle(HostCase):
    def test_a_started_host_runs_cmd_sessions_and_exits_once_idle(self):
        # A listing never starts a host.
        code, response, stderr = self.call({"op": "list"})
        self.assertEqual((code, response), (3, None))
        self.assertIn("no session host runs", stderr)
        self.assertEqual(self.status(), {"running": False})

        created, prompt = self.cmd("cmd/a")
        self.assertEqual((created["session"][0], created["pane"][0], created["existing"]), ("$", "%", False))
        status = self.status()
        host = status["host"]
        self.assertTrue(status["running"])
        self.assertFalse(host["in_job"])
        self.assertIn(status["status"]["conpty"], ("bundled", "inbox"))
        self.assertEqual(status["status"]["sessions"], 1)
        pane = created["pane"]

        marker = f"{self.unique}-echo"
        self.keys(pane, {"literal": f"echo {marker}"}, {"named": "Enter"})
        self.wait_for("the echo", lambda: marker in self.capture(pane).splitlines())
        pasted = f"{self.unique}-paste"
        # cmd.exe never enables bracketed paste, so the text arrives plain.
        self.ok({"op": "paste", "target": {"pane": pane}, "text": f"echo {pasted}", "bracketed": True})
        self.keys(pane, {"named": "Enter"})
        self.wait_for("the paste", lambda: pasted in self.capture(pane).splitlines())
        self.assertIn(prompt, self.capture(pane, styled=True))

        details = self.info(pane)
        self.assertEqual((details["width"], details["height"]), (100, 30))
        self.assertEqual(details["pane_pid"], created["pid"])
        self.assertEqual(details["server_pid"], host["pid"])
        self.assertEqual(details["session_name"], "cmd/a")
        self.assertFalse(details["dead"] or details["in_mode"] or details["bracketed_paste"])
        self.assertEqual(details["current_command"], "cmd")

        self.ok({"op": "set_environment", "target": {"name": "cmd/a"}, "name": "@hgs_run", "value": "run-1"})
        listed = self.ok({"op": "list"})
        self.assertEqual([session["name"] for session in listed["sessions"]], ["cmd/a"])
        self.assertEqual(listed["panes"][0]["run_marker"], "run-1")
        self.assertEqual(listed["panes"][0]["pid"], created["pid"])
        for target in ({"name": "cmd/a"}, {"session": created["session"]}, {"pane": pane}):
            resolved = self.ok({"op": "resolve_target", "target": target})
            self.assertEqual(resolved, {"session": created["session"], "name": "cmd/a", "pane": pane})

        self.ok({"op": "kill", "target": {"pane": pane}})
        self.wait_for("the end", lambda: not self.ok({"op": "list"})["sessions"])
        # The idle host lingers, closes its pipe and exits; nothing starts it again.
        self.wait_for("the idle exit", lambda: not self.status()["running"], timeout=IDLE_EXIT)
        code, _, stderr = self.call({"op": "list"})
        self.assertEqual(code, 3, stderr)

    def test_a_retried_create_starts_nothing_and_names_stay_unique(self):
        created, _ = self.cmd("cmd/once")
        launch_id = f"{self.unique}-1"
        comspec = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
        again = self.create("cmd/once", [comspec, "/d", "/q", "/k"], launch_id=launch_id)
        self.assertEqual((again["session"], again["pane"], again["pid"], again["existing"]),
                         (created["session"], created["pane"], created["pid"], True))
        self.assertEqual(len(self.ok({"op": "list"})["sessions"]), 1)
        code, response, _ = self.call({
            "op": "create_session", "launch_id": f"{self.unique}-other", "name": "cmd/once",
            "cwd": str(self.work), "env": [f"{key}={value}" for key, value in self.env.items()],
            "argv": [str(comspec)], "size": {"cols": 80, "rows": 24}})
        self.assertEqual((code, response["error"]["code"]), (1, "exists"))

    def test_a_second_host_is_refused_by_the_lock(self):
        self.cmd("cmd/lock")
        first = self.status()["host"]["pid"]
        result = self.run_hgs("serve", timeout=LIMIT)
        self.assertEqual(result.returncode, 3, result.stderr)
        self.assertEqual(self.status()["host"]["pid"], first)
        log = (self.state / "host.log").read_text(errors="replace")
        self.assertIn("another session host holds the lock", log)

    def test_attach_repaints_follows_the_client_size_and_detaches(self):
        created, prompt = self.cmd("cmd/attach")
        pane = created["pane"]
        attached = self.attach(
            {"target": {"pane": pane}, "size": {"cols": 90, "rows": 25}, "run_id": None},
            [{"wait_ms": 500}, {"resize": {"cols": 120, "rows": 40}}, {"wait_ms": 500},
             {"send": "echo attached-input\r"}, {"wait_ms": 500}, {"detach": True}])
        self.assertEqual(attached["attached"]["pane"], pane)
        self.assertTrue(attached["sent"])
        output = bytes.fromhex(attached["output_hex"]).decode("utf-8", "replace")
        self.assertIn(prompt, output, "the repaint shows the screen")
        self.wait_for("the input", lambda: "attached-input" in self.capture(pane).splitlines())
        # With no client left the last size stays.
        details = self.info(pane)
        self.assertEqual((details["width"], details["height"]), (120, 40))
        self.assertEqual(self.ok({"op": "list_clients", "target": None})["clients"], [])


@unittest.skipUnless(FAKE_AGENT.is_file(), "build the fake agent: cargo build --locked --example fake_agent")
class FakeAgent(HostCase):
    def setUp(self):
        super().setUp()
        self.record = self.root / "record.jsonl"
        self.ready = self.root / "ready.json"
        self.control = self.root / "control"

    def events(self, kind=None):
        lines = self.record.read_text(errors="replace").split("\n")[:-1] if self.record.exists() else []
        events = [json.loads(line) for line in lines]
        return [event for event in events if kind is None or event["kind"] == kind]

    def send_control(self, *lines):
        staged = self.root / "control.tmp"
        staged.write_text("\n".join(lines) + "\n")
        os.replace(staged, self.control)
        self.wait_for("the control file", lambda: not self.control.exists())

    def replies(self):
        return [(event.get("query"), bool(event.get("unsolicited"))) for event in self.events("reply")]

    def test_modes_pastes_and_query_answers_reach_the_agent_once(self):
        created = self.create("fake/a", [
            FAKE_AGENT, "--record", self.record, "--ready", self.ready, "--control", self.control,
            "--modes", "paste", "--query", "dsr,da1,da2,status,osc10,osc11,xtversion",
            "--query-wait", "3000", "--prompt", "fake> "])
        pane = created["pane"]
        self.wait_for("the agent", self.ready.exists)
        conpty = self.status()["status"]["conpty"]
        # Every query gets at most one answer: from the host's screen model for
        # what reaches it (the bundled ConPTY forwards DA1 and the cursor
        # position), from the inbox ConPTY for what it answers itself. Colours,
        # and with the bundled pair the terminal version, stay unanswered, as
        # tmux leaves them.
        answered = self.replies()
        self.assertFalse([query for query, unsolicited in answered if unsolicited], answered)
        queries = [query for query, _ in answered]
        for query in set(queries):
            self.assertEqual(queries.count(query), 1, answered)
        self.assertTrue({"dsr", "da1"} <= set(queries), answered)
        unanswered = {"osc10", "osc11", "xtversion"} if conpty == "bundled" else {"osc10", "osc11"}
        self.assertFalse(unanswered & set(queries), answered)

        # Bracketed paste is on: the model sees DECSET 2004 through ConPTY.
        self.wait_for("bracketed paste", lambda: self.info(pane)["bracketed_paste"])
        self.ok({"op": "paste", "target": {"pane": pane}, "text": "line one\nline two", "bracketed": True})
        self.wait_for("the paste", lambda: [event["text"] for event in self.events("paste")] == ["line one\nline two"])

        # An attached client's terminal answers too; only its typing arrives.
        replies_before = len(self.events("reply"))
        attached = self.attach(
            {"target": {"pane": pane}, "size": {"cols": 100, "rows": 30}, "run_id": None},
            [{"wait_ms": 300}, {"send_hex": b"\x1b[?1;2c\x1b[5;1R\x1b]11;rgb:0/0/0\x07".hex()},
             {"send": "typed"}, {"wait_ms": 500}, {"detach": True}])
        self.assertTrue(attached["sent"])
        output = bytes.fromhex(attached["output_hex"])
        for query in (b"\x1b[6n", b"\x1b[c", b"\x1b[>c", b"\x1b]10;?", b"\x1b]11;?"):
            self.assertNotIn(query, output)
        self.wait_for("the typing", lambda: "typed" in "".join(event.get("text", "") for event in self.events("text")))
        self.assertEqual(len(self.events("reply")), replies_before, self.events("reply"))

        # Mode toggles reach the model.
        self.send_control("modes no-paste,cursor")
        self.wait_for("paste off", lambda: not self.info(pane)["bracketed_paste"])
        self.keys(pane, {"named": "Up"})
        self.wait_for("the cursor key", lambda: any(event.get("hex") == "1b4f41" for event in self.events("key")))
        self.ok({"op": "paste", "target": {"pane": pane}, "text": "plain", "bracketed": True})
        self.wait_for("the plain paste", lambda: "plain" in "".join(event.get("text", "") for event in self.events("text")))
        self.assertEqual(len(self.events("paste")), 1)

        self.send_control("exit 0")
        self.wait_for("the end", lambda: not self.ok({"op": "list"})["sessions"])
        self.assertEqual(self.events("exit")[-1]["code"], 0)


if __name__ == "__main__":
    unittest.main()
