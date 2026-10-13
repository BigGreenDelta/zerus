#!/usr/bin/env python3
"""Windows local sessions through the hgs CLI and its terminal backend (spec D2, D3, D8).

Windows only. Each case enables local sessions in a temporary profile with
`hgs session-host install` (the marker, the stable CLI and a host image), then
starts untracked sessions of the native fake agent the way the GUI starts any
program, `hgs fake_agent PROJECT --new -n TAG -d --launch-id UUID -- ARGS`, and
PowerShell with `hgs sh`. The session host starts on demand from the staged image;
listings never start it. Sessions are listed with `hgs ls --json`, renamed and
killed with the CLI, and driven through `hgs __host backend`, which runs one
operation of the terminal backend hgs uses for them (`src/state/backend/host.rs`).

Tracked agents (Claude Code, Codex, Kimi) are refused on Windows until their hook
form is settled (spec D12), and attaching a terminal waits for the attach client
(M5); both refusals are checked here.

HGS_TEST_BIN names the hgs.exe under test (default target/debug/hgs.exe), the
"portable" copy that every case launches from; HGS_TEST_FAKE_AGENT names the fake
agent (default target/debug/examples/fake_agent.exe, `cargo build --locked
--example fake_agent`). Cases share the hermetic rules of test_windows_host.py.
"""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest
import uuid

from test_windows_host import FAKE_AGENT, HGS, HIDDEN, LIMIT, HostCase, open_host

if os.name == "nt":
    from ctypes import wintypes

    KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    KERNEL32.OpenProcess.restype = wintypes.HANDLE
    KERNEL32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    KERNEL32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                    ctypes.POINTER(wintypes.DWORD))
    KERNEL32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 0x102


def image_of(pid):
    """The executable of process `pid`, or None."""
    handle = KERNEL32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        if not KERNEL32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return None
        return buffer.value
    finally:
        KERNEL32.CloseHandle(handle)


def read_json(path):
    """The JSON object in `path` once it is complete, else None."""
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None


@unittest.skipUnless(os.name == "nt", "local sessions on Windows run on Windows only")
@unittest.skipUnless(FAKE_AGENT.is_file(), "build the fake agent: cargo build --locked --example fake_agent")
class LifecycleCase(HostCase):
    """A temporary profile with local sessions enabled, a project folder and the fake agent on PATH."""

    def setUp(self):
        super().setUp()
        # The staged image, not the build under test, serves as the host (spec D2).
        for key in ("HGS_HOST_IMAGE", "HGS_HOST_ALLOW_UNSTAGED"):
            del self.env[key]
        self.bin = self.root / "bin"
        self.project = self.work / "project"
        appdata = self.root / "appdata"
        for path in (self.bin, self.project, appdata):
            path.mkdir()
        shutil.copy2(FAKE_AGENT, self.bin / "fake_agent.exe")
        self.env.update(PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""), APPDATA=str(appdata),
                        HGS_SELF="test", HGS_PEERS="", HGS_TAB="0")
        self.cli = self.home / ".local" / "bin" / "hgs.exe"
        result = subprocess.run([str(HGS), "session-host", "install", "--json"], env=self.env, capture_output=True,
                                text=True, timeout=LIMIT * 2, creationflags=HIDDEN, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.cli.is_file())
        self.record = self.root / "record.jsonl"
        self.ready = self.root / "ready.json"
        self.control = self.root / "control"

    def hgs(self, *args, rc=0, env=None):
        """Run the build under test, a portable copy, as the GUI would; never on the caller's console."""
        result = subprocess.run([str(HGS), *args], env=env or self.env, cwd=self.work, capture_output=True,
                                text=True, timeout=LIMIT * 2, creationflags=HIDDEN, stdin=subprocess.DEVNULL)
        if rc is not None:
            self.assertEqual(result.returncode, rc, f"hgs {' '.join(args)}\n{result.stdout}{result.stderr}")
        return result

    def sessions(self):
        listing = json.loads(self.hgs("ls", "--json", "--local").stdout)
        return {session["name"]: session for session in listing["sessions"]}

    def backend(self, operation, ok=True):
        result = self.run_hgs("backend", json.dumps(operation))
        answer = json.loads(result.stdout) if result.stdout.strip() else None
        self.assertEqual(result.returncode, 0 if ok else 1, f"{operation}: {answer} {result.stderr}")
        return answer["ok"] if ok else answer["error"]

    def events(self, kind=None):
        lines = self.record.read_text(errors="replace").split("\n")[:-1] if self.record.exists() else []
        return [event for event in map(json.loads, lines) if kind is None or event["kind"] == kind]

    def launch(self, tag="one", *extra, launch_id=None, env=None):
        """Start the fake agent in a session of its own; its name and ready record."""
        launch_id = launch_id or str(uuid.uuid4())
        name = f"fake_agent/project/{tag}"
        result = self.hgs("fake_agent", str(self.project), "--new", "-n", tag, "-d", "--launch-id", launch_id, "--",
                          "--record", str(self.record), "--ready", str(self.ready), "--control", str(self.control),
                          "--prompt", "fake> ", *extra, env=env)
        self.assertEqual(result.stdout, f"hgs: started {name}\n")
        ready = self.wait_for("the agent", lambda: read_json(self.ready))
        self.status()  # Remember the host process for the cleanup.
        return name, ready

    def pane(self, name):
        return next(pane for pane in self.ok({"op": "list"})["panes"] if pane["session"] == name)

    def wait_for_exit(self, ready):
        """Wait until the agent of `ready` has exited."""
        handle = open_host(ready["pid"], ready["process_start"])
        if handle:
            try:
                self.assertEqual(KERNEL32.WaitForSingleObject(handle, LIMIT * 1000), WAIT_OBJECT_0)
            finally:
                KERNEL32.CloseHandle(handle)

    def alive(self, ready):
        handle = open_host(ready["pid"], ready["process_start"])
        if not handle:
            return False
        try:
            return KERNEL32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT
        finally:
            KERNEL32.CloseHandle(handle)


class Launch(LifecycleCase):
    def test_a_launch_starts_the_host_and_runs_the_program_under_the_stable_cli(self):
        # A listing never starts a host (D3).
        self.assertEqual(self.sessions(), {})
        self.assertEqual(self.hgs("ls", "--local").stdout.splitlines()[0], "  (no sessions)")
        self.assertEqual(self.status(), {"running": False})

        # Launched from inside another session: its identity stays behind (D8).
        outer = dict(self.env, HGS_SESSION="claude/outer/main", HGS_RUN_ID="outer-run", HGS_TRACKING="1",
                     COLORTERM="truecolor")
        launch_id = str(uuid.uuid4())
        started = time.time()
        name, ready = self.launch("one", launch_id=launch_id, env=outer)
        status = self.status()
        self.assertTrue(status["running"] and not status["host"]["in_job"], status)
        self.assertTrue(os.path.samefile(Path(status["host"]["image"]).parent.parent,
                                         self.home / ".local" / "share" / "hgs" / "host"), status)
        rows = self.sessions()
        self.assertEqual(set(rows), {name})
        row = rows[name]
        self.assertEqual({key: row[key] for key in ("cmd", "project", "tag", "attached", "clients", "launch_id")},
                         dict(cmd="fake_agent", project="project", tag="one", attached=0, clients=[],
                              launch_id=launch_id))
        self.assertTrue(started - 2 <= row["created"] <= time.time() + 1, row)
        self.assertFalse(row.get("conversation_id"))
        self.assertIn(name, self.hgs("ls", "--local").stdout)
        self.assertEqual(ready["argv"], [])

        # The pane's root is the stable CLI's supervisor, which started the agent.
        pane = self.pane(name)
        self.assertTrue(os.path.samefile(image_of(pane["pid"]), self.cli), image_of(pane["pid"]))
        self.assertEqual(self.backend({"op": "on_terminal", "pid": str(ready["pid"]), "tty": f"conpty:{pane['pane']}"}),
                         {"attached": True})
        self.assertEqual(self.backend({"op": "on_terminal", "pid": str(pane["pid"]), "tty": f"conpty:{pane['pane']}"}),
                         {"attached": False})

        # A name is created once.
        again = self.hgs("fake_agent", str(self.project), "--new", "-n", "one", "-d", rc=1)
        self.assertIn("already exists", again.stderr)
        self.assertEqual(len(self.events("start")), 1)

    def prompt(self, pane):
        """Whether PowerShell in `pane` waits at an empty prompt, which may wrap: the screen ends with it."""
        text = self.backend({"op": "capture", "pane": pane})["text"].rstrip()
        return "PS " in text and text.endswith(">")

    def test_powershell_sessions_get_the_session_environment(self):
        outer = dict(self.env, HGS_SESSION="claude/outer/main", HGS_RUN_ID="outer-run", COLORTERM="truecolor")
        # Neither the user's profile nor their PowerShell history takes part.
        result = self.hgs("sh", str(self.project), "--new", "-n", "ps", "-d", "--", "-NoProfile", "-NoExit",
                          "-Command", "try { Set-PSReadLineOption -HistorySaveStyle SaveNothing } catch {}", env=outer)
        name = "sh/project/ps"
        self.assertEqual(result.stdout, f"hgs: started {name}\n")
        self.status()
        pane = self.pane(name)
        # PowerShell runs under the stable CLI's supervisor, like any program (D8, D11).
        self.assertTrue(os.path.samefile(image_of(pane["pid"]), self.cli), image_of(pane["pid"]))
        self.wait_for("the prompt", lambda: self.prompt(pane["pane"]))
        output = self.root / "environment.json"
        command = ("[ordered]@{exe=$env:HGS_EXECUTABLE; pane=$env:HGS_HOST_PANE; host_session=$env:HGS_HOST_SESSION; "
                   "session=$env:HGS_SESSION; run=$env:HGS_RUN_ID; term=$env:TERM; colorterm=$env:COLORTERM; "
                   "client=$env:HGS_CLIENT; cwd=(Get-Location).Path; pid=$PID; "
                   "parent=(Get-CimInstance Win32_Process -Filter \"ProcessId=$PID\").ParentProcessId} | "
                   f"ConvertTo-Json | Set-Content -Encoding UTF8 -LiteralPath '{output}'")
        self.backend({"op": "send", "pane": pane["pane"], "keys": [{"literal": command}, {"named": "Enter"}]})
        environment = self.wait_for("the environment", lambda: read_json(output))
        self.assertTrue(os.path.samefile(environment["exe"], self.cli), environment)
        self.assertEqual((environment["pane"], environment["host_session"]), (pane["pane"], pane["session_id"]))
        self.assertEqual((environment["session"], environment["run"], environment["colorterm"]), (None, None, None))
        self.assertEqual((environment["term"], environment["client"]), ("xterm-256color", "local"))
        self.assertTrue(os.path.samefile(environment["cwd"], self.project), environment)
        self.assertEqual(environment["parent"], pane["pid"], environment)
        self.assertIn(Path(image_of(environment["pid"])).name.lower(), ("pwsh.exe", "powershell.exe"))

        # The host starts the root process as a new process group, which ignores
        # Ctrl+C; the supervisor turns it back on for PowerShell (D11).
        sleeping, interrupted = self.root / "sleeping.txt", self.root / "interrupted.txt"
        self.wait_for("the prompt", lambda: self.prompt(pane["pane"]))
        self.backend({"op": "send", "pane": pane["pane"], "keys": [
            {"literal": f"Set-Content -LiteralPath '{sleeping}' 1; Start-Sleep -Seconds 600"}, {"named": "Enter"}]})
        self.wait_for("the sleep", sleeping.exists)
        self.backend({"op": "send", "pane": pane["pane"], "keys": [{"named": "C-c"}]})
        self.wait_for("Ctrl+C to stop the sleep", lambda: self.prompt(pane["pane"]))
        self.backend({"op": "send", "pane": pane["pane"], "keys": [
            {"literal": f"Set-Content -LiteralPath '{interrupted}' 1"}, {"named": "Enter"}]})
        self.wait_for("the next command", interrupted.exists)
        self.hgs("kill", name)
        self.wait_for("the end", lambda: name not in self.sessions())

    def test_tracked_agents_attach_and_other_executables_are_refused(self):
        # Tracked agents wait for their Windows hook form (D12); nothing starts.
        refused = self.hgs("claude", str(self.project), "--new", "-n", "t", "-d", rc=2)
        self.assertIn("HGS_TRACKING=0", refused.stderr)
        # Attaching a terminal waits for the attach client (M5), and is refused first.
        refused = self.hgs("fake_agent", str(self.project), "--new", "-n", "attached", rc=2)
        self.assertIn("not available yet", refused.stderr)
        # Sessions name only the stable CLI (D8).
        portable = dict(self.env, HGS_EXECUTABLE=str(HGS))
        refused = self.hgs("fake_agent", str(self.project), "--new", "-n", "portable", "-d", env=portable, rc=1)
        self.assertIn("only the stable hgs CLI", refused.stderr)
        self.assertEqual(self.status(), {"running": False})
        self.assertEqual(self.sessions(), {})

        name, _ = self.launch("one")
        refused = self.hgs("a", name, rc=2)
        self.assertIn("not available yet", refused.stderr)

    def test_dry_runs_print_the_session_host_requests(self):
        self.assertEqual(self.hgs("ls", "--json", "--local", "--dry-run").stdout, "session-host list\n")
        printed = self.hgs("--dry-run", "fake_agent", str(self.project), "--new", "-n", "dry", "-d", "--", "--x").stdout
        self.assertTrue(printed.startswith("session-host create-session -d -s fake_agent/project/dry -c "), printed)
        self.assertIn(" -e HGS_CLIENT=local -- ", printed)
        self.assertTrue(printed.rstrip().endswith(" --run 0 fake_agent --x"), printed)
        # The supervisor is the stable CLI, quoted as a command line quotes a program.
        command = printed.split(" -- ", 1)[1]
        program = command[1:].split('"', 1)[0] if command.startswith('"') else command.split(" ", 1)[0]
        self.assertTrue(os.path.samefile(program, self.cli), program)
        self.assertEqual(self.hgs("--dry-run", "kill", "fake_agent/project/dry").stdout,
                         "session-host kill -t fake_agent/project/dry\n")
        self.assertEqual(self.status(), {"running": False})


class Sessions(LifecycleCase):
    def test_rename_keeps_the_agent_and_kill_ends_it(self):
        name, ready = self.launch("one")
        renamed = "fake_agent/project/two"
        self.assertEqual(self.hgs("rename", name, renamed).stdout, f"hgs: renamed {name} to {renamed}\n")
        rows = self.sessions()
        self.assertEqual(set(rows), {renamed})
        self.assertTrue(self.alive(ready))
        self.assertEqual([event["kind"] for event in self.events() if event["kind"] in ("signal", "eof", "exit")], [])
        self.assertEqual(self.backend({"op": "session_exists", "name": name}), {"exists": False})
        self.assertEqual(self.backend({"op": "session_exists", "name": renamed}), {"exists": True})

        self.hgs("kill", renamed)
        self.wait_for_exit(ready)
        self.wait_for("the end", lambda: not self.sessions())
        exit_event, = self.events("exit")
        self.assertIn(exit_event["cause"], ("eof", "signal:CTRL_CLOSE"), exit_event)
        self.assertIn("no such session", self.hgs("kill", renamed, rc=1).stderr)

    def test_the_backend_drives_the_agent_through_the_host(self):
        name, ready = self.launch("one")
        pane = self.pane(name)["pane"]
        self.assertEqual(self.backend({"op": "listing"})["sessions"][0]["name"], name)
        self.wait_for("the prompt", lambda: self.backend({"op": "capture", "pane": pane})["text"].startswith("fake>"))
        fields = ["session_id", "session_name", "pane_id", "dead", "in_mode", "tty", "current_command"]
        info = self.backend({"op": "pane_info", "target": {"pane": pane}, "fields": fields})["text"].split("\t")
        self.assertEqual(info[1:], [name, pane, "0", "0", f"conpty:{pane}", "fake_agent"])
        self.assertTrue(info[0].startswith("$"), info)
        self.wait_for("bracketed paste", lambda: self.backend(
            {"op": "pane_info", "target": {"name": name}, "fields": ["bracketed_paste"]})["text"] == "1")

        self.backend({"op": "send", "pane": pane, "keys": [{"literal": "hello"}, {"named": "Enter"}]})
        self.wait_for("the keys", lambda: self.events("enter"))
        # ConPTY may hand the text over in more than one read.
        self.assertEqual("".join(event["text"] for event in self.events("text")), "hello")
        self.backend({"op": "paste", "pane": pane, "text": "line one\nline two", "mode": "composer"})
        self.wait_for("the paste", lambda: [event["text"] for event in self.events("paste")] == ["line one\nline two"])
        self.backend({"op": "paste", "pane": pane, "text": "plain", "mode": "terminal"})
        self.wait_for("the plain paste", lambda: "plain" in "".join(event.get("text", "") for event in self.events("text")))
        self.assertEqual(len(self.events("paste")), 1)

        self.backend({"op": "set_run_marker", "name": name, "run_id": "run-1"})
        self.assertEqual(self.backend({"op": "panes"})[0]["run_marker"], "run-1")
        session_id = info[0]
        self.assertEqual(self.backend({"op": "session_name", "id": session_id}), {"name": name})
        # Input checks need the session's supervisor channel, which an untracked
        # session never opens (D10): they fail closed.
        error = self.backend({"op": "input_state", "pane": pane, "name": name, "run_id": "run-1",
                              "agent": "claude", "pid": ready["pid"]}, ok=False)
        self.assertIn("no supervisor", error)
        # Terminate asks the supervisor first; without one it ends the session
        # as tmux kill-session does.
        self.backend({"op": "kill_session", "target": {"session": session_id}})
        self.wait_for_exit(ready)
        self.wait_for("the end", lambda: not self.backend({"op": "listing"})["sessions"])
        self.assertEqual(self.backend({"op": "session_name", "id": session_id}), {"name": None})


if __name__ == "__main__":
    unittest.main()
