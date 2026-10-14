#!/usr/bin/env python3
"""Tracking hooks on Windows (spec D12) and tracked agents through the S6 preview.

Windows only. `HookForms` runs the shell form hgs writes there,
`"C:/…/hgs.exe" __state hook`, through every program that runs it for an agent:
`CreateProcess` directly, `%COMSPEC% /d /s /c "…"` (Kimi Code, Node.js's
`shell: true`), `%COMSPEC% /C "…"` and Git Bash (Claude Code's shell form) when it
is installed, and Codex's PowerShell form
`& "C:/…/hgs.exe" __state hook; exit $LASTEXITCODE` the way Codex runs it, with
`pwsh` and Windows PowerShell as `-NoProfile -Command …` in a console of their
own. A copy of the build under test is the fake hgs, in a folder whose name has
spaces, Cyrillic and characters that `cmd.exe` treats specially outside quotes; it
runs as the hooks of an isolated test host's session do, with
`HGS_HOST_ALLOW_UNSTAGED=1` and a state directory of its own, so local sessions
need not be enabled.
Each run must reach `hgs __state hook` with its payload on stdin: the payload has
no session ID, so hgs answers with exactly that error. PowerShell reads the
quoted program of the shell form as a string and never runs hgs, which is why
Claude Code needs Git Bash for the shell form and Codex, whose hook shell is
PowerShell, gets the PowerShell form.

`TrackedHooks` enables local sessions in a temporary profile whose folder has
spaces, Cyrillic and parentheses, sets `HGS_WINDOWS_TRACKING_PREVIEW=1` with an
isolated state directory and private agent homes, and launches tracked sessions of
native fake agents named `claude`, `codex` and `kimi`. hgs installs its hooks into
their homes (Claude Code's exec form, Codex's shell form as `command` and
PowerShell form as `commandWindows`, Kimi's block); each fake reads them and runs
them as its agent does
(`examples/fake_agent.rs`), and the hooks move the session's state: the
conversation is confirmed, a prompt and its reply appear in `hgs inspect`, and a
Claude session pauses and resumes its conversation. The preview's refusals are
checked too.

HGS_TEST_BIN and HGS_TEST_FAKE_AGENT name the builds, as for
test_windows_lifecycle.py, whose hermetic rules apply.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
import unittest

from test_windows_host import FAKE_AGENT, HGS, HIDDEN, LIMIT
from test_windows_lifecycle import LifecycleCase, read_events, read_json

# What `hgs __state hook` answers a payload without a session ID.
NO_SESSION = "hook did not supply a valid session_id"
PREVIEW = "HGS_WINDOWS_TRACKING_PREVIEW"
HOMES = {"claude": "CLAUDE_CONFIG_DIR", "codex": "CODEX_HOME", "kimi": "KIMI_CODE_HOME"}
# The answers to the capability probes hgs runs before a tracked agent starts.
PROBES = {"claude": "2.1.296 (Claude Code)", "codex": "codex-cli 0.162.0", "kimi": "kimi 2.1.1"}


def git_bash():
    """The Git Bash that Claude Code would use, or None."""
    candidates = [Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"]
    git = shutil.which("git")
    if git:
        candidates.append(Path(git).resolve().parent.parent / "bin" / "bash.exe")
    return next((path for path in candidates if path.is_file()), None)


def shell_form(hgs):
    return f'"{str(hgs).replace(os.sep, "/")}" __state hook'


def powershell_form(hgs):
    return f'& "{str(hgs).replace(os.sep, "/")}" __state hook; exit $LASTEXITCODE'


def powershells():
    """The PowerShells Codex may run its hooks with (its `default_user_shell`): pwsh, Windows PowerShell."""
    pwsh = shutil.which("pwsh.exe") or Path(os.environ.get("ProgramFiles", r"C:\Program Files"), "PowerShell", "7",
                                            "pwsh.exe")
    windows = shutil.which("powershell.exe") or Path(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0",
                                                     "powershell.exe")
    return [str(path) for path in (pwsh, windows) if Path(path).is_file()]


@unittest.skipUnless(os.name == "nt", "the Windows hook forms run on Windows only")
class HookForms(unittest.TestCase):
    def setUp(self):
        if not HGS.is_file():
            raise AssertionError(f"{HGS} is missing; run cargo build --locked")
        temp = tempfile.TemporaryDirectory(prefix="hgs-hooks-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        folder = self.root / "Hook dir & (1)" / "Кириллица ^x;y'z"
        folder.mkdir(parents=True)
        self.hgs = folder / "hgs.exe"
        shutil.copy2(HGS, self.hgs)
        (self.root / "state").mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.upper().startswith(("HGS_", "FAKE_AGENT_", "TMUX", "XDG_"))}
        # Local sessions may be off on this computer: the runs are the hooks of
        # an isolated test host's session, whose override admits `__state`
        # (src/host/gate.rs). A control console refuses it, as
        # test_windows_client.py checks.
        self.env.update(HGS_SESSION="claude/probe/hook", HGS_RUN_ID="probe-run",
                        HGS_STATE_DIR=str(self.root / "state"), HGS_HOST_ALLOW_UNSTAGED="1")

    def run_form(self, argv):
        return subprocess.run(argv, input="{}", env=self.env, cwd=self.root, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=LIMIT, creationflags=HIDDEN)

    def test_the_shell_form_reaches_hgs_through_every_runner(self):
        command = shell_form(self.hgs)
        self.assertTrue(command.startswith('"C:/') or command.startswith('"//'), command)
        comspec = os.environ.get("COMSPEC") or str(Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe")
        # A string is the command line CreateProcess parses itself.
        runners = {
            "CreateProcess": command,
            "cmd /C": f'"{comspec}" /C "{command}"',
            "cmd /d /s /c (Kimi Code, Node.js)": f'"{comspec}" /d /s /c "{command}"',
        }
        bash = git_bash()
        if bash:
            runners["Git Bash (Claude Code's shell form)"] = [str(bash), "-c", command]
        for name, argv in runners.items():
            with self.subTest(runner=name):
                result = self.run_form(argv)
                self.assertEqual(result.returncode, 1, f"{name}: {result.stdout}{result.stderr}")
                self.assertIn(NO_SESSION, result.stderr, name)
        # Claude Code's exec form: the program and its arguments, no shell.
        result = self.run_form([str(self.hgs).replace(os.sep, "/"), "__state", "hook"])
        self.assertIn(NO_SESSION, result.stderr)
        shells = powershells()
        self.assertTrue(shells, "no PowerShell")
        for powershell in shells:
            with self.subTest(powershell=powershell):
                # Codex's form, as Codex runs it: hgs reads the payload PowerShell left unread.
                result = self.run_form([powershell, "-NoProfile", "-Command", powershell_form(self.hgs)])
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(NO_SESSION, result.stderr)
                # PowerShell reads the shell form's quoted program as a string expression.
                result = self.run_form([powershell, "-NoProfile", "-NonInteractive", "-Command", command])
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(NO_SESSION, result.stderr + result.stdout)


@unittest.skipUnless(os.name == "nt", "local sessions on Windows run on Windows only")
@unittest.skipUnless(FAKE_AGENT.is_file(), "build the fake agent: cargo build --locked --example fake_agent")
class TrackedHooks(LifecycleCase):
    PROFILE = "Профиль Ivan (1)"

    def setUp(self):
        super().setUp()
        self.homes = {}
        for agent, variable in HOMES.items():
            shutil.copy2(FAKE_AGENT, self.bin / f"{agent}.exe")
            home = self.root / "agents" / agent
            home.mkdir(parents=True)
            self.homes[agent] = home
            self.env[variable] = str(home)
        self.env[PREVIEW] = "1"
        self.transcripts = self.root / "transcripts"
        # Hooks name the canonical stable CLI: long names, no verbatim prefix.
        self.stable = os.path.realpath(self.cli).replace(os.sep, "/")

    def agent_env(self, agent, tag):
        paths = {kind: self.root / f"{agent}-{tag}.{kind}" for kind in ("jsonl", "ready")}
        env = dict(self.env, FAKE_AGENT_PROBE_TEXT=PROBES[agent], FAKE_AGENT_RECORD=str(paths["jsonl"]),
                   FAKE_AGENT_READY=str(paths["ready"]), FAKE_AGENT_PROMPT=f"{agent}> ",
                   FAKE_AGENT_TRANSCRIPT_DIR=str(self.transcripts))
        return env, paths["jsonl"], paths["ready"]

    def inspect(self, name):
        return json.loads(self.hgs("inspect", name).stdout)

    def launch_tracked(self, agent, tag, *extra, previous=None):
        env, record, ready = self.agent_env(agent, tag)
        name = f"{agent}/project/{tag}"
        result = self.hgs(agent, str(self.project), "--new", "-n", tag, "-d", *extra, env=env)
        self.assertEqual(result.stdout, f"hgs: started {name}\n", result.stderr)
        started = self.wait_for("the agent", lambda: self.started(ready, previous))
        self.status()  # Remember the host process for the cleanup.
        return name, record, started

    def started(self, ready, previous=None):
        record = read_json(ready)
        return record if record and (previous is None or record["pid"] != previous["pid"]) else None

    def ended(self, name):
        """Whether session `name` has ended: gone from the listing, or saved as stopped or paused."""
        row = self.sessions().get(name)  # One listing: the session may go between two.
        return row is None or row.get("state") in ("stopped", "paused")

    def hooks_ran(self, record, event):
        return [hook for hook in read_events(record, "hook") if hook["event"] == event]

    def check_hooks(self, record):
        """Every hook the fake ran reached hgs, which answered 0 within its timeout."""
        hooks = read_events(record, "hook")
        self.assertTrue(hooks)
        for hook in hooks:
            self.assertEqual((hook.get("status"), hook.get("timeout"), hook.get("error")), (0, False, None), hook)
            self.assertLess(hook["ms"], 5000, hook)
        return hooks

    def converse(self, name, record, ready, prompt):
        """Confirm the conversation, send `prompt` and wait for its turn to end."""
        self.wait_for("the confirmed conversation",
                      lambda: self.inspect(name).get("conversation_id") == ready["session_id"])
        pane = self.pane(name)["pane"]
        self.backend({"op": "send", "pane": pane, "keys": [{"literal": prompt}, {"named": "Enter"}]})
        self.wait_for("the Stop hook", lambda: self.hooks_ran(record, "Stop"))
        state = self.wait_for("the idle turn", lambda: (lambda s: s if s.get("prompt") == prompt
                                                        and s.get("activity") == "idle" else None)(self.inspect(name)))
        return state

    def test_installed_hooks_reach_hgs_and_track_each_agent(self):
        for agent in HOMES:
            with self.subTest(agent=agent):
                name, record, ready = self.launch_tracked(agent, "track")
                self.assertEqual(ready["name"], agent)
                prompt = f"Hello from the {agent} hook test"
                state = self.converse(name, record, ready, prompt)
                self.assertEqual(state["conversation_id"], ready["session_id"])
                # The journal holds the hook events.
                self.assertIn("UserPromptSubmit", json.dumps(state.get("events", [])), state)
                hooks = self.check_hooks(record)
                self.assertEqual({hook["event"] for hook in hooks}, {"SessionStart", "UserPromptSubmit", "Stop"})
                self.assertEqual(self.sessions()[name].get("conversation_id"), ready["session_id"])
                self.hgs("kill", name)
                self.wait_for_exit(ready)
                self.wait_for("the end", lambda: self.ended(name))

        # The bytes hgs wrote: each agent's own form, naming the stable CLI.
        shell = f'"{self.stable}" __state hook'
        powershell = f'& "{self.stable}" __state hook; exit $LASTEXITCODE'
        settings = json.loads((self.homes["claude"] / "settings.json").read_text(encoding="utf-8"))
        for event, groups in settings["hooks"].items():
            self.assertEqual(groups, [{"hooks": [{"type": "command", "command": self.stable,
                                                  "args": ["__state", "hook"], "timeout": 5}]}], event)
        hooks = json.loads((self.homes["codex"] / "hooks.json").read_text(encoding="utf-8"))
        for event, groups in hooks["hooks"].items():
            self.assertEqual(groups, [{"hooks": [{"type": "command", "command": shell,
                                                  "commandWindows": powershell, "timeout": 5}]}], event)
        kimi = (self.homes["kimi"] / "config.toml").read_text(encoding="utf-8")
        self.assertEqual(kimi.count("# BEGIN hgs session tracking"), 1)
        self.assertEqual({hook["command"] for hook in tomllib.loads(kimi)["hooks"]}, {shell})

    def test_a_tracked_claude_pauses_and_resumes_its_conversation(self):
        name, record, first = self.launch_tracked("claude", "pause")
        self.converse(name, record, first, "Before the pause")
        self.hgs("pause", name)
        self.wait_for_exit(first)
        self.wait_for("the paused session", lambda: self.sessions().get(name, {}).get("state") == "paused")
        self.assertEqual([event["signal"] for event in read_events(record, "signal")], ["CTRL_BREAK"])

        self.hgs("resume", name, "-d", env=self.agent_env("claude", "pause")[0])
        again = self.wait_for("the resumed agent", lambda: self.started(
            self.root / "claude-pause.ready", first))
        self.assertEqual((again["session_id"], again["resumed"]), (first["session_id"], True), again)
        self.wait_for("the resumed conversation", lambda: (lambda s: s.get("conversation_id") == first["session_id"]
                                                           and not s.get("expected_id"))(self.inspect(name)))
        starts = self.hooks_ran(record, "SessionStart")
        self.assertEqual(len(starts), 2, starts)
        self.check_hooks(record)
        self.hgs("kill", name)
        self.wait_for_exit(again)

    def test_the_preview_runs_only_isolated_agents(self):
        settings = self.homes["claude"] / "settings.json"
        # Without the preview, tracked agents wait for D12 as before.
        refused = self.hgs("claude", str(self.project), "--new", "-n", "a", "-d", rc=2,
                           env={k: v for k, v in self.env.items() if k != PREVIEW})
        self.assertIn("HGS_TRACKING=0", refused.stderr)
        # The agent's home must be private.
        refused = self.hgs("claude", str(self.project), "--new", "-n", "b", "-d", rc=2,
                           env={k: v for k, v in self.env.items() if k != "CLAUDE_CONFIG_DIR"})
        self.assertIn("CLAUDE_CONFIG_DIR", refused.stderr)
        default = self.home / ".claude"
        default.mkdir()
        refused = self.hgs("claude", str(self.project), "--new", "-n", "c", "-d", rc=2,
                           env=dict(self.env, CLAUDE_CONFIG_DIR=str(default)))
        self.assertIn("CLAUDE_CONFIG_DIR", refused.stderr)
        # So must the state directory: the profile's default one is not isolated.
        state = self.home / ".local" / "state" / "hgs" / "sessions"
        state.mkdir(parents=True)
        refused = self.hgs("claude", str(self.project), "--new", "-n", "d", "-d", rc=2,
                           env=dict(self.env, HGS_STATE_DIR=str(state)))
        self.assertIn("HGS_STATE_DIR", refused.stderr)
        self.assertFalse(settings.exists())
        self.assertFalse((default / "settings.json").exists())
        self.assertEqual(self.status(), {"running": False})


if __name__ == "__main__":
    unittest.main()
