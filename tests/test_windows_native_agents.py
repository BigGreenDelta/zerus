#!/usr/bin/env python3
"""Real Claude Code and Codex in the Windows session host, keyless (spec M7).

Windows only. HGS_TEST_BIN names the hgs.exe under test (the release build in CI);
HGS_CLAUDE_TEST_BIN and HGS_CODEX_TEST_BIN name the native executables taken from the
pinned win32-x64 npm packages (scripts/ci/agent-compatibility.json), and
HGS_TEST_CONPTY_DIR, when set, the pinned ConPTY pair that the portable zip ships
(scripts/fetch-conpty.py). A class skips only off Windows or without its executable or
HGS_TEST_BIN: scripts/ci/agent-compatibility.py counts a skip as a failure, and a
skipped native suite is no compatibility evidence. Whatever the agents do wrong is a
failure, never a skip.

Each case enables local sessions in a temporary profile as tests/test_windows_lifecycle.py
does: `hgs session-host install`, run from a portable copy of the build (beside the ConPTY
pair when given, as in the zip), stages the stable CLI, a host image and the marker there,
and the first launch starts the host from that image. The agent's executable is linked
into the profile's `.local\\bin`, which hgs puts first on PATH (src/platform/windows.rs
`prepare_path`; agents are found on that PATH, src/state/backend/host.rs), so only that
copy runs. Its home (CLAUDE_CONFIG_DIR, CODEX_HOME), where hgs installs its hooks (spec
D12), is the case's own, as are APPDATA, LOCALAPPDATA and TEMP; Codex still resolves
`~` to the real profile (FOLDERID_Profile), see `project_parent`. The agents reach only
tests/model_api_fixture.py on 127.0.0.1, run with `python -I -X utf8`, with a dummy key;
any other HTTP(S) request goes to a dead local proxy, and updates, telemetry and other
nonessential traffic are off. Codex runs with full access and no approvals, as in spike
S6, so it never needs its Windows sandbox; the CI job never extracts the sandbox setup
program, and Codex never runs it.

Sessions are driven only through hgs, as the GUI drives them: a tracked launch,
`ls --json`, `inspect`, `send --json`, `answer --json`, `clear-context --json`, `pause`,
`resume -d` and `terminate`. What an agent accepted is read from its own transcript,
which hgs never writes. Each case covers one area for one agent:

- launch: the tracked launch reaches the composer, its hooks confirm the conversation in
  the listing, `inspect` and the journal; `hgs send` delivers a plain, a non-ASCII, a
  backslash-terminated and a multiline message, each exactly once, and a replayed
  request delivers nothing.
- question: the fake model asks (Claude's AskUserQuestion, Codex's
  request_user_input_async) and `hgs answer` answers from Activity.
- approval: Claude's Write permission prompt, and Codex's review of the hooks hgs
  installed, answered with `hgs answer`.
- answer channel (Claude): the PermissionRequest hook hgs registers in exec form takes
  the question's answers and Bash approvals (once, feedback, No), a Bash panel too tall
  for the host's 80x24 too; Terminal answers and a killed hook end it, and the verified
  screen takes over only from a hook that took nothing.
- clear: `hgs clear-context` starts a new conversation in the same agent process. For
  Codex, hgs must first not take the header above a message for its reset panel
  (zerus-g9dd, which a project folder short enough to show whole exposes).
- lifecycle: `hgs pause`, `hgs resume -d` on the same conversation, then `hgs terminate`.

Model and effort pickers and compaction are not reviewed on Windows and stay out. Every
wait has a deadline; a failure shows the session's screen, the host's log and the fake
model's requests. Each case stops what it started, its sessions, the host and the fake
model, and waits for every agent and supervisor through handles opened while they ran,
and until nothing holds its executables, before its folders go.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

import model_api_fixture as api
import test_windows_host as host

TESTS = Path(__file__).resolve().parent
EXE = ".exe" if os.name == "nt" else ""
HGS_SET = bool(os.environ.get("HGS_TEST_BIN"))
CLAUDE = os.environ.get("HGS_CLAUDE_TEST_BIN")
CODEX = os.environ.get("HGS_CODEX_TEST_BIN")
CONPTY = os.environ.get("HGS_TEST_CONPTY_DIR")
KEY = "fixture-key"
DEAD_PROXY = "http://127.0.0.1:9"
# Seconds an agent may take to start, finish a turn, pause or exit: Claude Code starts
# in about 9 s under ConPTY on the development PC (spike S6), and a hosted runner scans
# a freshly linked executable on its first start.
WAIT = 120
# What the native test runner and the user's shell may export that no agent of a case
# may inherit: other agent accounts, providers and homes, terminal identities, CI tokens.
CLEARED_PREFIXES = ("CLAUDE", "ANTHROPIC_", "CODEX_", "OPENAI_", "KIMI_", "VSCODE_", "TERM_", "WT_", "CONEMU",
                    "ACTIONS_")
CLEARED = {"COLORTERM", "FORCE_COLOR", "NO_COLOR", "GITHUB_TOKEN", "GH_TOKEN", "HTTP_PROXY", "HTTPS_PROXY",
           "ALL_PROXY", "NO_PROXY", "NODE_OPTIONS"}
# Messages `hgs send` must deliver exactly: plain, non-ASCII, ending in a backslash
# (which Claude reads as a line continuation before Enter), and multiline.
MESSAGES = ("Plain fixture message.", "Привет, ünïcödé ✓ 漢字.", "Continue the fixture\\",
            "Two fixture lines:\nC:\\fixture\\path and more.")


def path_forms(path):
    """The spellings an agent may key a folder by on Windows: separators and drive letter case."""
    native = str(path)
    forms = [native, native.replace("\\", "/")]
    if len(native) > 1 and native[1] == ":":
        forms += [native[0].lower() + native[1:], (native[0].lower() + native[1:]).replace("\\", "/")]
    return list(dict.fromkeys(forms))


def below(path, folder):
    """Whether `path` lies in `folder`, without case or 8.3 short names on Windows: a
    hosted runner's TEMP is `C:\\Users\\RUNNER~1\\…` below `C:\\Users\\runneradmin`."""
    path, folder = (os.path.normcase(os.path.realpath(value)) for value in (path, folder))
    return path == folder or path.startswith(folder.rstrip(os.sep) + os.sep)


def in_use(path):
    """Whether a process runs or loads the file at `path`: Windows then refuses to
    open it for writing, as tests/test_windows_host_conpty.py checks. Opening changes
    nothing; a read-only file is never taken for one in use."""
    try:
        with open(path, "r+b"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return os.access(path, os.W_OK)


def project_parent():
    """Where a case's project folder goes. Codex prints a folder below the real profile
    (FOLDERID_Profile, not USERPROFILE) as `~\\…`, which hgs reads against the case's
    profile, so a project outside the real profile keeps its reset panel confirmable.
    The real profile is this process's USERPROFILE, which scripts/ci/agent-compatibility.py
    leaves unchanged for the suites. A hosted runner's TEMP lies inside the profile and
    RUNNER_TEMP outside it."""
    default = tempfile.gettempdir()
    profile = os.environ.get("USERPROFILE")
    runner = os.environ.get("RUNNER_TEMP")
    if profile and below(default, profile) and runner and os.path.isdir(runner) and not below(runner, profile):
        return runner
    return default


class NativeAgent:
    """One real agent in the session host of a temporary profile; the classes below name it."""

    AGENT = None
    BINARY = None
    # The agent's arguments after `--` in every launch.
    ARGS = ()
    # The glyph that starts the agent's composer row.
    COMPOSER = None
    CLEARS_WITH_KEYS = False

    def setUp(self):
        # Handles on the agents and supervisors this case saw running, by (PID, start), and
        # its sessions, for the cleanup that HostCase registers.
        self.pinned, self.names, self.project_parent = {}, [], None
        super().setUp()
        # Agents and hgs compare folders as they print them: no 8.3 short names.
        self.root = self.root.resolve()
        self.state, self.home, self.work = (self.root / name for name in ("state", self.PROFILE, "work"))
        self.project_parent = Path(tempfile.mkdtemp(prefix="hgs-native-", dir=project_parent())).resolve()
        self.project = self.project_parent / "project"
        self.agent_home = self.root / f"{self.AGENT}-home"
        self.api_root = self.root / "api"
        for folder in (self.project, self.agent_home, self.api_root, *(self.root / name for name in
                                                                         ("appdata", "localappdata", "temp"))):
            folder.mkdir(parents=True)
        self.env = {key: value for key, value in self.env.items()
                    if key.upper() not in CLEARED and not key.upper().startswith(CLEARED_PREFIXES)}
        self.env.update(APPDATA=str(self.root / "appdata"), LOCALAPPDATA=str(self.root / "localappdata"),
                        TEMP=str(self.root / "temp"), TMP=str(self.root / "temp"),
                        HGS_STATE_DIR=str(self.state), USERPROFILE=str(self.home), HOME=str(self.home),
                        HGS_SELF="test", HGS_PEERS="", HGS_TAB="0")
        self.api_url = f"http://127.0.0.1:{self.start_api()}"
        self.env.update(
            ANTHROPIC_BASE_URL=self.api_url, ANTHROPIC_API_KEY=KEY, OPENAI_API_KEY=KEY,
            CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1", DISABLE_AUTOUPDATER="1", DISABLE_UPDATES="1",
            DISABLE_INSTALLATION_CHECKS="1", CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL="1",
            DISABLE_TELEMETRY="1", DISABLE_ERROR_REPORTING="1",
            HTTPS_PROXY=DEAD_PROXY, HTTP_PROXY=DEAD_PROXY, NO_PROXY="127.0.0.1,localhost")
        self.configure()
        self.install()
        self.place_agent()

    # The profile, the agent and the fake model.

    def start_api(self):
        """Start the fake model for this case; its port."""
        output = open(self.api_root / "output.txt", "wb")
        self.api = subprocess.Popen(
            [sys.executable, "-I", "-X", "utf8", str(TESTS / "model_api_fixture.py"), "--root", str(self.api_root),
             "--project", str(self.project)], cwd=self.api_root, stdin=subprocess.DEVNULL, stdout=output,
            stderr=subprocess.STDOUT, creationflags=host.HIDDEN)
        self.addCleanup(self.stop_api, output)
        port = self.api_root / "port"
        self.until("the model API fixture", lambda: port.is_file() or self.api.poll() is not None, timeout=30)
        if not port.is_file():
            self.fail("the model API fixture exited:\n" + (self.api_root / "output.txt").read_text(errors="replace"))
        return int(port.read_text())

    def stop_api(self, output):
        self.api.kill()
        self.api.wait(timeout=30)
        output.close()

    def install(self):
        """Enable local sessions in the profile from a portable copy of the build under test,
        beside the pinned ConPTY pair when one is given (spec D2, D6)."""
        for key in ("HGS_HOST_IMAGE", "HGS_HOST_ALLOW_UNSTAGED"):
            self.env.pop(key, None)
        portable = self.root / "portable"
        portable.mkdir()
        self.hgs_exe = portable / "hgs.exe"
        shutil.copy2(host.HGS, self.hgs_exe)
        if CONPTY:
            for name in ("conpty.dll", "OpenConsole.exe"):
                shutil.copy2(Path(CONPTY) / name, portable / name)
        self.hgs("session-host", "install", "--json")
        self.bin = self.home / ".local" / "bin"
        self.assertTrue((self.bin / "hgs.exe").is_file(), "the stable CLI is missing after install")

    def place_agent(self):
        """Link the pinned executable into the profile's `.local\\bin`, which hgs puts first on
        PATH; a copy where a link cannot be made."""
        target = self.bin / f"{self.AGENT}{EXE}"
        try:
            os.link(self.BINARY, target)
        except OSError:
            shutil.copy2(self.BINARY, target)

    def configure(self):
        """The agent's private home, as a user who has finished onboarding left it."""
        raise NotImplementedError

    # hgs and what it reports.

    def hgs(self, *args, input=None, rc=0):
        result = subprocess.run([str(self.hgs_exe), *args], env=self.env, cwd=self.work, input=input,
                                stdin=None if input is not None else subprocess.DEVNULL, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=WAIT,
                                creationflags=host.HIDDEN)
        if rc is not None and result.returncode != rc:
            self.fail(f"hgs {' '.join(args)}: exit {result.returncode}\n{result.stdout}{result.stderr}"
                      + self.describe())
        return result

    def json(self, *args, input=None):
        return json.loads(self.hgs(*args, input=input).stdout)

    def rows(self):
        return {row["name"]: row for row in self.json("ls", "--json", "--local")["sessions"]}

    def row(self, name):
        return self.rows().get(name) or {}

    def inspect(self, name):
        return self.json("inspect", name)

    def record(self, name):
        """The session's live record (state directory), or {}."""
        path = self.state / (hashlib.sha256(name.encode()).hexdigest() + ".json")
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def journal(self, run_id):
        """The event types the journal holds for one run, in order."""
        path = self.state / "events.sqlite3"
        database = sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True, timeout=10)
        try:
            rows = database.execute("SELECT payload FROM events ORDER BY seq").fetchall()
        finally:
            database.close()
        events = [json.loads(payload) for payload, in rows]
        return [event.get("type") for event in events if event.get("run_id") == run_id]

    def capture(self, name):
        """The session's screen as the host draws it (UTF-8, whatever the code page)."""
        result = self.hgs("__host", "call", json.dumps({"op": "capture", "target": {"name": name}, "styled": False}),
                          rc=None)
        return json.loads(result.stdout)["ok"]["text"] if result.returncode == 0 else f"(no capture: {result.stderr})"

    def describe(self):
        """The screens, the listing, the host's log and the fake model's requests, for failure messages."""
        parts = []
        for name in self.names:
            try:
                parts.append(f"screen of {name}:\n{self.capture(name)}")
                parts.append(f"record of {name}: " + json.dumps(
                    {key: self.record(name).get(key) for key in ("activity", "phase", "conversation_id", "run_id",
                                                               "expected_id", "error", "input_pending_at",
                                                               "last_event_at")}))
            except Exception as error:  # noqa: BLE001 (diagnostics never hide the failure)
                parts.append(f"{name}: {error}")
        try:
            log = (self.state / "host.log").read_text(encoding="utf-8", errors="replace").splitlines()
            parts.append("host.log:\n" + "\n".join(log[-20:]))
        except OSError:
            pass
        try:
            requests = [{key: request.get(key) for key in ("route", "prompt", "answer", "results")}
                        for request in api.requests(self.api_root)[-8:]]
            parts.append("model requests:\n" + "\n".join(json.dumps(request, ensure_ascii=False)[:600]
                                                          for request in requests))
        except (OSError, ValueError) as error:
            parts.append(f"model requests: {error}")
        return "\n" + "\n".join(parts)

    def until(self, what, predicate, timeout=WAIT):
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value:
                return value
            if time.monotonic() > deadline:
                self.fail(f"timed out after {timeout} s waiting for {what}" + self.describe())
            time.sleep(0.25)

    # Processes.

    def pin_session(self, name):
        """Keep handles on the agent and supervisor of a session while they run."""
        record = self.record(name)
        supervisor = record.get("supervisor") if isinstance(record.get("supervisor"), dict) else {}
        for pid, started in ((record.get("pid"), record.get("process_start")),
                             (supervisor.get("pid"), supervisor.get("start"))):
            if isinstance(pid, int) and isinstance(started, str) and started and (pid, started) not in self.pinned:
                handle = host.open_host(pid, started)
                if handle:
                    self.pinned[pid, started] = handle

    def alive(self, pid, started):
        """Whether this exact process still runs: its creation time matches and it has not exited."""
        handle = host.open_host(pid, started)
        if not handle:
            return False
        try:
            return host.KERNEL32.WaitForSingleObject(handle, 0) != host.WAIT_OBJECT_0
        finally:
            host.KERNEL32.CloseHandle(handle)

    def stop_host(self):
        """Kill the sessions and stop the host (HostCase), then wait for every agent and
        supervisor of the case, and for its executables to be released, before its
        folders go; end and report any process that stays."""
        try:
            for name in self.names:
                self.pin_session(name)
            super().stop_host()
        finally:
            left = []
            deadline = time.monotonic() + WAIT
            for (pid, _), handle in self.pinned.items():
                remaining = max(0, int((deadline - time.monotonic()) * 1000))
                if host.KERNEL32.WaitForSingleObject(handle, remaining) != host.WAIT_OBJECT_0:
                    left.append(pid)
                    host.KERNEL32.TerminateProcess(handle, 1)
                    host.KERNEL32.WaitForSingleObject(handle, WAIT * 1000)
                host.KERNEL32.CloseHandle(handle)
            self.pinned.clear()
            # The host's OpenConsole.exe ends after its pseudoconsoles, and a scanner may
            # hold a fresh copy for a moment (tests/test_windows_host_conpty.py): wait
            # until no process holds an executable or library of the case.
            files = [path for pattern in ("*.exe", "*.dll") for path in self.root.rglob(pattern)]
            deadline = time.monotonic() + host.LIMIT
            while time.monotonic() < deadline and any(map(in_use, files)):
                time.sleep(0.25)
            if self.project_parent:
                shutil.rmtree(self.project_parent, ignore_errors=True)
        if left:
            raise AssertionError(f"agent processes survived the session host: {sorted(left)}")

    # Sessions.

    def launch(self, tag, *extra):
        """A tracked session of the agent in the project, started as the GUI starts one; its name."""
        name = f"{self.AGENT}/project/{tag}"
        launch_id = str(uuid.uuid4())
        args = [*self.ARGS, *extra]
        result = self.hgs(self.AGENT, str(self.project), "--new", "-n", tag, "-d", "--launch-id", launch_id,
                          *(["--", *args] if args else []))
        self.assertEqual(result.stdout, f"hgs: started {name}\n", result.stderr)
        self.names.append(name)
        status = self.status()  # Also remembers the host process for the cleanup.
        if CONPTY and status.get("running"):
            # The staged image carries the pair, as the zip's does (spec D6).
            self.assertEqual(status["status"]["conpty"], "bundled", status)
        row = self.until(f"{name} in the listing", lambda: self.row(name).get("runtime_state") == "live"
                         and self.row(name))
        self.assertEqual((row["cmd"], row["project"], row["tag"], row["launch_id"], row["process_state"]),
                         (self.AGENT, "project", tag, launch_id, "running"))
        self.pin_session(name)
        return name

    def settled(self, name):
        """The screen once it shows the agent's composer and stays the same for half a second."""
        def same():
            first = self.capture(name)
            time.sleep(0.5)
            second = self.capture(name)
            composer = any(line.strip().startswith(self.COMPOSER) for line in second.splitlines())
            return second if first == second and composer else None
        return self.until(f"{name}'s composer", same)

    def ready(self, name):
        """The listing row once the session takes a message: what hgs itself reports for a
        conversation the agent has yet to confirm, else idle with a settled composer."""
        def check():
            row, state = self.row(name), self.inspect(name)
            if row.get("runtime_state") != "live" or state.get("pending_questions"):
                return None
            if not row.get("conversation_id"):
                return row if state.get("first_message_can_send") else None
            if "resume_message_can_send" in state:
                return row if state["resume_message_can_send"] else None
            return row if row.get("activity") == "idle" and row.get("phase") == "idle" else None
        row = self.until(f"{name} to take a message", check)
        self.settled(name)
        return self.row(name) or row

    def message(self, row, text):
        return dict(request_id=str(uuid.uuid4()), text=text, attachments=[], expected_run_id=row["run_id"],
                    expected_conversation_id=row.get("conversation_id") or "")

    def send(self, name, payload):
        return json.loads(self.hgs("send", name, "--json", input=json.dumps(payload)).stdout)

    def turn_ended(self, name, since):
        """inspect once a Stop hook later than `since` has left the session idle."""
        state = self.inspect(name)
        stops = [event for event in state.get("events", []) if event.get("type") == "Stop"
                 and event.get("at", 0) >= since]
        return state if stops and state.get("activity") == "idle" and state.get("phase") == "idle" else None

    def deliver(self, name, text):
        """Send `text` once the session is ready and wait for its turn to end; the request and receipt."""
        row = self.ready(name)
        payload = self.message(row, text)
        receipt = self.send(name, payload)
        self.assertEqual((receipt["status"], receipt["submitted_text"], receipt["run_id"]),
                         ("submitted", text, row["run_id"]), receipt)
        self.until(f"the turn of {text!r}", lambda: self.turn_ended(name, receipt["submitted_at"]))
        self.until(f"{text!r} in the transcript", lambda: self.accepted(name, text))
        return payload, receipt

    def card(self, name, source):
        """The pending card of `source` once hgs can answer it."""
        seen = {}

        def answerable():
            cards = [card for card in self.inspect(name).get("pending_questions", []) if card.get("source") == source]
            if cards:
                seen["card"] = cards[0]
            return cards[0] if cards and cards[0].get("can_answer") else None
        try:
            return self.until(f"an answerable {source} card", answerable)
        except AssertionError as error:
            raise AssertionError(f"{error}\nlast card: {json.dumps(seen.get('card'), ensure_ascii=False)}") from None

    def answer(self, name, card, answers, statuses=("answered",)):
        """Answer a card through `hgs answer`; the request and its receipt, whose status is one
        of `statuses`."""
        row = self.row(name)
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=row["run_id"],
                       expected_conversation_id=row.get("conversation_id") or "", question_id=card["question_id"],
                       expected_question_hash=card["question_hash"], answers=answers)
        receipt = self.json("answer", name, "--json", input=json.dumps(payload))
        self.assertIn(receipt["status"], statuses, receipt)
        return payload, receipt

    def check_clear(self, name, status):
        """`hgs clear-context` once the session is idle; the row and record before it and the
        receipt, whose status is `status` (either outcome for None)."""
        row, record = self.ready(name), self.record(name)
        request = dict(request_id=str(uuid.uuid4()), expected_run_id=row["run_id"],
                       expected_conversation_id=row["conversation_id"])
        receipt = self.json("clear-context", name, "--json", input=json.dumps(request))
        self.assertEqual((receipt["run_id"], receipt["conversation_id"]), (row["run_id"], row["conversation_id"]))
        self.assertIn(receipt["status"], (status,) if status else ("confirmed", "submitted"), receipt)
        return row, record, receipt

    def check_after_clear(self, name, row, record):
        """The same agent process takes the next message in a new conversation; the old
        conversation keeps its own messages."""
        text = "After the clear."
        self.deliver(name, text)
        cleared, current = self.row(name), self.record(name)
        self.assertNotIn(cleared["conversation_id"], ("", row["conversation_id"]))
        self.assertEqual((cleared["run_id"], current["pid"], current["process_start"]),
                         (row["run_id"], record["pid"], record["process_start"]))
        self.assertEqual(self.accepted(name, text, cleared["conversation_id"]), 1, self.describe())
        self.assertEqual(self.accepted(name, text, row["conversation_id"]), 0)
        self.assertEqual(self.accepted(name, MESSAGES[0], row["conversation_id"]), 1)
        self.assertIn(row["conversation_id"], self.inspect(name)["cleared_conversations"])

    def model_results(self):
        """The tool results the fake model has received, in order."""
        return [result for request in api.requests(self.api_root) for result in request.get("results") or []]

    # The native transcript.

    def transcript_rows(self, conversation_id):
        """The rows of the agent's own transcript of one conversation."""
        raise NotImplementedError

    def accepted(self, name, text, conversation_id=None):
        """How many user messages of exactly `text` the agent recorded in a conversation."""
        raise NotImplementedError

    # Cases shared by both agents.

    def check_messages(self, name):
        """Each message of MESSAGES starts exactly one turn; a replayed request delivers nothing."""
        for text in MESSAGES:
            payload, receipt = self.deliver(name, text)
            self.assertEqual(self.accepted(name, text), 1, self.describe())
            if text == MESSAGES[0]:
                self.assertEqual(self.send(name, payload), receipt)
        # Every turn has ended; the replay typed nothing that a later turn could hold.
        self.settled(name)
        self.assertEqual([self.accepted(name, text) for text in MESSAGES], [1] * len(MESSAGES), self.describe())

    def keys(self, name, *keys):
        """Keys to the session's pane through `hgs __host backend` op `send`, the path hgs uses."""
        panes = json.loads(self.hgs("__host", "call", json.dumps({"op": "list"})).stdout)["ok"]["panes"]
        pane = next(pane["pane"] for pane in panes if pane["session"] == name)
        self.hgs("__host", "backend", json.dumps({"op": "send", "pane": pane, "keys": list(keys)}))

    def composer(self, name):
        """The text after the agent's composer marker on its last composer row, or None."""
        rows = [line.strip() for line in self.capture(name).splitlines() if line.strip().startswith(self.COMPOSER)]
        return rows[-1][len(self.COMPOSER):].strip() if rows else None

    def check_keys(self, name):
        """Spike S2 through the host (zerus-ziq3.7.13): text typed after a lone Esc arrives whole,
        Ctrl+C clears a draft, Ctrl+S neither freezes the screen (XOFF) nor stops input, and the
        keys hgs itself clears Claude's input with (C-e C-u: claude_question.rs, interrupt.rs;
        hgs never clears Codex's composer with keys) empty it. Each step is checked on the screen,
        and the empty composer by an exact `hgs send`, which refuses a non-empty one. Codex
        resolves Ctrl+letter in the keyboard layout (zerus-c8r9): a non-Latin layout reproduces
        what a hosted runner's US layout cannot."""
        self.ready(name)
        pid = self.record(name).get("pid")
        shows = lambda text: lambda: text in (self.composer(name) or "")  # noqa: E731
        cleared = lambda text: lambda: (lambda row: row is not None and text not in row)(self.composer(name))  # noqa: E731
        # A lone Esc, then text: nothing of it is lost or taken as Alt+key, and nothing is cancelled.
        self.keys(name, {"named": "Escape"})
        time.sleep(0.5)
        self.keys(name, {"literal": "esc probe"})
        self.until("the text typed after a lone Esc", shows("esc probe"))
        # Ctrl+C clears a draft (one press on a draft: a second one on an empty composer quits).
        self.keys(name, {"named": "C-c"})
        self.until("Ctrl+C to clear the draft", cleared("esc probe"))
        if self.CLEARS_WITH_KEYS:
            self.keys(name, {"literal": "clear probe"})
            self.until("the draft before C-e C-u", shows("clear probe"))
            self.keys(name, {"named": "C-e"}, {"named": "C-u"})
            self.until("C-e C-u to empty the composer", cleared("clear probe"))
        # Ctrl+S on the empty composer (Claude stashes a draft with it): what follows still shows.
        time.sleep(2)  # Past the agents' "Ctrl+C again to quit" window.
        self.keys(name, {"named": "C-s"})
        self.keys(name, {"literal": "ctrl s probe"})
        self.until("text typed after Ctrl+S", shows("ctrl s probe"))
        self.keys(name, {"named": "C-c"})
        self.until("Ctrl+C to clear the draft", cleared("ctrl s probe"))
        # The same agent process takes the next message exactly once; no probe became one.
        self.deliver(name, MESSAGES[0])
        self.assertEqual(self.record(name).get("pid"), pid, self.describe())
        self.assertEqual([self.accepted(name, text) for text in (MESSAGES[0], "esc probe", "clear probe",
                                                                 "ctrl s probe")],
                         [1, 0, 0, 0], self.describe())

    def check_lifecycle(self, name):
        """Pause ends the agent, resume continues the same conversation, terminate archives it."""
        self.deliver(name, MESSAGES[0])
        row, record = self.row(name), self.record(name)
        process = (record["pid"], record["process_start"])
        self.assertEqual(self.hgs("pause", name).stdout, f"hgs: paused {name}\n")
        paused = self.until("the paused session", lambda: (lambda row: row.get("state") == "paused" and row)(
            self.row(name)))
        self.assertEqual({key: paused.get(key) for key in ("runtime_state", "process_state", "resumable",
                                                           "conversation_id")},
                         dict(runtime_state="stopped", process_state="exited", resumable=True,
                              conversation_id=row["conversation_id"]))
        self.until("the paused agent to exit", lambda: not self.alive(*process))
        self.hgs("resume", name, "-d")
        self.until("the resumed session", lambda: (lambda row: row.get("runtime_state") == "live"
                                                   and row.get("run_id") != record["run_id"])(self.row(name)))
        self.pin_session(name)
        # Codex confirms a resumed conversation only with its next message.
        text = "After the resume."
        self.deliver(name, text)
        # Once it has answered, the record names the agent rather than its supervisor.
        self.pin_session(name)
        resumed = self.record(name)
        self.assertNotEqual((resumed.get("pid"), resumed.get("process_start")), process)
        self.assertEqual(self.row(name)["conversation_id"], row["conversation_id"])
        self.assertEqual(self.accepted(name, text, row["conversation_id"]), 1, self.describe())
        self.assertEqual(self.accepted(name, MESSAGES[0], row["conversation_id"]), 1, self.describe())
        process = (resumed["pid"], resumed["process_start"])
        result = self.hgs("terminate", name)
        self.assertTrue(result.stdout.startswith(f"hgs: terminated and archived {name} ("), result.stdout)
        archived = self.row(name)
        self.assertEqual({key: archived.get(key) for key in ("state", "runtime_state", "conversation_id")},
                         dict(state="archived", runtime_state="stopped", conversation_id=row["conversation_id"]))
        self.until("the terminated agent to exit", lambda: not self.alive(*process))


@unittest.skipUnless(CLAUDE and HGS_SET, "set HGS_TEST_BIN and HGS_CLAUDE_TEST_BIN to run real Claude Code")
class NativeWindowsClaude(NativeAgent, host.HostCase):
    AGENT = "claude"
    BINARY = CLAUDE
    ARGS = ("--model", "claude-sonnet-4-6")
    COMPOSER = "❯"
    CLEARS_WITH_KEYS = True

    def configure(self):
        projects = {form: {"hasTrustDialogAccepted": True, "allowedTools": []} for form in path_forms(self.project)}
        (self.agent_home / ".claude.json").write_text(json.dumps({
            "hasCompletedOnboarding": True, "theme": "dark", "autoUpdates": False,
            "customApiKeyResponses": {"approved": [KEY[-20:]], "rejected": []},
            "officialMarketplaceAutoInstallAttempted": True, "hasSeenAutoDefaultNudge": True,
            "projects": projects}), encoding="utf-8")
        (self.agent_home / "settings.json").write_text(json.dumps({"permissions": {"defaultMode": "default"}}),
                                                       encoding="utf-8")
        self.env["CLAUDE_CONFIG_DIR"] = str(self.agent_home)

    def transcript_rows(self, conversation_id):
        paths = list((self.agent_home / "projects").glob(f"*/{conversation_id}.jsonl"))
        rows = []
        for path in paths:
            for line in path.read_text(encoding="utf-8").splitlines():
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass  # A line still being written.
        return rows

    def check_hook_form(self):
        """Claude Code from 2.1.296 gets the exec form naming the stable CLI (spec D12), and the
        answer channel beside its PermissionRequest hook in the same form."""
        settings = json.loads((self.agent_home / "settings.json").read_text(encoding="utf-8"))
        stable = os.path.realpath(self.bin / "hgs.exe").replace(os.sep, "/")
        hook = {"hooks": [{"type": "command", "command": stable, "args": ["__state", "hook"], "timeout": 5}]}
        self.assertEqual(settings["hooks"]["Stop"], [hook])
        self.assertEqual(settings["hooks"]["PermissionRequest"], [hook, {"hooks": [{
            "type": "command", "command": stable, "args": ["__state", "answer-channel"], "timeout": 86400}]}])

    def accepted(self, name, text, conversation_id=None):
        conversation_id = conversation_id or self.row(name).get("conversation_id")
        forms = (text, [{"type": "text", "text": text}])
        return sum(row.get("type") == "user" and row.get("message", {}).get("content") in forms
                   for row in self.transcript_rows(conversation_id))

    def test_a_tracked_launch_reports_its_hooks_and_send_delivers_exact_messages(self):
        name = self.launch("messages")
        # Claude's SessionStart hook confirms the conversation before any message.
        row = self.until("the confirmed conversation", lambda: (lambda row: row.get("conversation_id")
                                                                and row.get("activity") == "idle" and row)(
            self.row(name)))
        self.assertTrue(self.inspect(name)["tracked"])
        self.check_hook_form()
        self.check_messages(name)
        # The hook named the transcript that Claude has written since.
        self.assertTrue(os.path.samefile(self.record(name)["transcript"], next(
            (self.agent_home / "projects").glob(f"*/{row['conversation_id']}.jsonl"))))
        events = [event["type"] for event in self.inspect(name)["events"]]
        self.assertTrue({"SessionStart", "UserPromptSubmit", "Stop"} <= set(events), events)
        journal = self.journal(row["run_id"])
        self.assertEqual(journal.count("UserPromptSubmit"), len(MESSAGES), journal)

    def test_hgs_answers_ask_user_question(self):
        name = self.launch("question")
        self.deliver_prompt(name, api.QUESTIONS)
        card = self.channel_card(name, "hook")
        self.assertEqual([item["question"] for item in card["questions"]],
                         [item["question"] for item in api.QUESTION_ITEMS])
        answers = [dict(question_id="q_0", selected_option_ids=["opt_0_1"], text=""),
                   dict(question_id="q_1", selected_option_ids=["opt_1_0", "opt_1_2"], text=""),
                   dict(question_id="q_2", selected_option_ids=[], text="Привет from Zerus")]
        payload, receipt = self.answer(name, card, answers)
        # The answer channel took it, so no key reached Claude's panel.
        self.assertEqual(receipt["transport"], "claude_hook", receipt)
        self.assertEqual(len(self.channels("emitted.json")), 1)
        # The same request is acknowledged again without a second delivery.
        self.assertEqual(self.json("answer", name, "--json", input=json.dumps(payload)), receipt)
        expected = {"Which release should we prepare?": "Preview", "Which checks should we run?": "Unit, UI",
                    "What else should we know?": "Привет from Zerus"}

        def recorded():
            for row in self.transcript_rows(card["conversation_id"]):
                result = row.get("toolUseResult")
                if isinstance(result, dict) and "answers" in result:
                    return result
            return None
        result = self.until("the answers in the transcript", recorded)
        self.assertEqual(result["answers"], expected)
        self.until("the answers at the model", lambda: any(
            all(f'"{question}"="{answer}"' in str(item) for question, answer in expected.items())
            for item in self.model_results()))
        state = self.until("the end of the turn", lambda: self.turn_ended(name, receipt["submitted_at"]))
        self.assertEqual(state["pending_questions"], [])
        self.assertEqual([event["type"] for event in state["events"]].count("QuestionAnswered"), 1)

    def test_hgs_answers_a_question_taller_than_its_unviewed_pane(self):
        # Pane growth is the behavior under test, so this session has no answer
        # channel: its hooks stay in the shell form an earlier hgs wrote, which
        # hgs repeats for every event and never pairs with a channel. Claude
        # runs that form with Git Bash, found as Claude Code 2.1.296 finds it.
        git = shutil.which("git", path=self.env.get("PATH") or self.env.get("Path"))
        bashes = [Path(self.env.get("CLAUDE_CODE_GIT_BASH_PATH", "")), Path(r"C:\Program Files\Git\bin\bash.exe"),
                  Path(r"C:\Program Files (x86)\Git\bin\bash.exe"), *([Path(git).parent.parent / "bin" / "bash.exe"] if git else [])]
        self.assertTrue(any(bash.name and bash.is_file() for bash in bashes),
                        "the shell-form hooks of this case need Git for Windows")
        stable = os.path.realpath(self.bin / "hgs.exe").replace(os.sep, "/")
        settings = json.loads((self.agent_home / "settings.json").read_text(encoding="utf-8"))
        settings["hooks"] = {"PermissionRequest": [{"hooks": [
            {"type": "command", "command": f'"{stable}" __state hook', "timeout": 5}]}]}
        (self.agent_home / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        name = self.launch("tall")
        written = json.loads((self.agent_home / "settings.json").read_text(encoding="utf-8"))["hooks"]
        self.assertEqual(written["PermissionRequest"], settings["hooks"]["PermissionRequest"])
        self.assertNotIn("answer-channel", json.dumps(written))
        self.deliver_prompt(name, api.TALL_QUESTION)
        # No client views the pane, so the session host grows it from 80x24
        # until Claude redraws the whole panel; the screen is the only path here.
        card = self.card(name, "hook")
        self.assertEqual(card["answer_transport"], "claude_tui")
        details = json.loads(self.hgs("__host", "call", json.dumps(
            {"op": "pane_info", "target": {"name": name}})).stdout)["ok"]
        self.assertEqual(details["width"], 80)
        self.assertGreater(details["height"], 24)
        payload, receipt = self.answer(name, card, [dict(question_id="q_0",
                                                         selected_option_ids=["opt_0_0", "opt_0_2"], text="")])
        expected = {"Which release should we prepare?": "API-ключи, Свои адреса"}

        def recorded():
            for row in self.transcript_rows(card["conversation_id"]):
                result = row.get("toolUseResult")
                if isinstance(result, dict) and "answers" in result:
                    return result
            return None
        self.assertEqual(self.until("the answer in the transcript", recorded)["answers"], expected)

    def test_hgs_approves_a_write_once(self):
        name = self.launch("approval")
        self.deliver_prompt(name, api.WRITE)
        card = self.card(name, "claude_tool_approval")
        self.assertTrue(card["approval"])
        options = card["questions"][0]["options"]
        self.assertEqual((options[0], options[-1]["label"], options[-1]["scope"]),
                         (dict(id="1", label="Yes", scope="once"), "No", "once"))
        self.assertIn(api.APPROVED_FILE, card["questions"][0]["question"])
        approved = self.project / api.APPROVED_FILE
        self.assertFalse(approved.exists())
        _, receipt = self.answer(name, card, [dict(question_id="approval", selected_option_ids=["1"], text="")])
        self.until("the approved file", approved.is_file)
        self.assertEqual(approved.read_text(encoding="utf-8"), api.APPROVED_TEXT)
        state = self.until("the end of the turn", lambda: self.turn_ended(name, receipt["submitted_at"]))
        self.assertEqual(state["pending_questions"], [])
        # A one-time approval leaves the permission mode as it was.
        settings = json.loads((self.agent_home / "settings.json").read_text(encoding="utf-8"))
        self.assertEqual(settings["permissions"], {"defaultMode": "default"})

    def test_the_answer_channel_answers_bash_approvals(self):
        name = self.launch("channel")
        self.check_hook_form()
        approved = self.project / api.BASH_FILE
        # Yes: the command runs once and the permission mode stays as it was.
        card = self.hook_card(name, api.BASH)
        self.assertTrue(card["panel_verified"], card)
        self.assertEqual(card["questions"][0]["options"][0], dict(id="1", label="Yes", scope="once"))
        payload, receipt = self.answer(name, card, self.choose(card, "Yes"))
        self.assertEqual(receipt["transport"], "claude_hook", receipt)
        self.until("the approved file", approved.is_file)
        self.until("the end of the turn", lambda: self.turn_ended(name, receipt["submitted_at"]))
        self.assertEqual(self.json("answer", name, "--json", input=json.dumps(payload)), receipt)
        self.assertEqual(json.loads((self.agent_home / "settings.json").read_text(encoding="utf-8"))["permissions"],
                         {"defaultMode": "default"})
        approved.unlink()
        # Feedback: Claude gets the text instead of running the command and goes on.
        card = self.hook_card(name, api.BASH)
        text = "Use printf other instead"
        _, receipt = self.answer(name, card, [dict(question_id="approval", selected_option_ids=[], text=text)])
        self.assertEqual(receipt["transport"], "claude_hook", receipt)
        self.until("the feedback at the model", lambda: any(text in json.dumps(result, ensure_ascii=False)
                                                            for result in self.model_results()))
        self.until("the end of the turn", lambda: self.turn_ended(name, receipt["submitted_at"]))
        # No: Claude stops the turn as its own No does.
        card = self.hook_card(name, api.BASH)
        _, receipt = self.answer(name, card, self.choose(card, "No"))
        self.assertEqual(receipt["transport"], "claude_hook", receipt)
        self.until("Claude's interruption", lambda: "What should Claude do instead?" in self.capture(name))
        state = self.until("the interrupted turn", lambda: (lambda state: state if state.get("phase") == "interrupted"
                                                            and not state.get("pending_questions") else None)(
            self.inspect(name)))
        self.assertFalse(approved.exists(), state)
        self.assertEqual(len(self.channels("emitted.json")), 3)

    def test_a_bash_panel_taller_than_80x24_is_answered_through_the_hook(self):
        # The host's size for a session no client views (host::SIZE): the panel loses its top,
        # and keeps losing it at the most hgs grows such a pane.
        name = self.launch("clipped")
        card = self.hook_card(name, api.LONG_BASH)
        question = card["questions"][0]
        self.assertFalse(card["panel_verified"], card)
        self.assertEqual([(option["label"], option["scope"]) for option in question["options"]],
                         [("Yes", "once"), ("No", "once")])
        self.assertIn(api.BASH_INPUT["command"], question["body"])
        _, receipt = self.answer(name, card, self.choose(card, "Yes"))
        self.assertEqual(receipt["transport"], "claude_hook", receipt)
        self.until("the approved file", (self.project / api.BASH_FILE).is_file)
        self.until("the end of the turn", lambda: self.turn_ended(name, receipt["submitted_at"]))

    def test_terminal_answers_end_the_hook(self):
        name = self.launch("terminal")
        approved = self.project / api.BASH_FILE
        # Claude tells the hook nothing of a Terminal Yes: the run's end closes it.
        for key, check in (("Enter", approved.is_file),
                           ("Escape", lambda: "What should Claude do instead?" in self.capture(name))):
            card = self.hook_card(name, api.BASH)
            worker = self.worker()
            self.keys(name, {"named": key})
            self.until(f"Terminal's {key}", check)
            self.until(f"the hook to end after {key}", lambda: not self.alive(*worker))
            self.until("no pending card", lambda: not self.inspect(name).get("pending_questions"))
            self.refused(name, card, self.choose(card, "No"))
            self.assertEqual(self.channels("answer.json") + self.channels("claimed.json")
                             + self.channels("emitted.json"), [])
            if key == "Enter":
                self.until("the end of the turn", lambda: self.turn_ended(name, 0))
        self.assertFalse(self.channels("retracted.json"))

    def test_a_killed_hook_leaves_the_approval_to_the_verified_screen(self):
        name = self.launch("killed")
        self.hook_card(name, api.BASH)
        handle = host.open_host(*self.worker())
        self.assertTrue(handle)
        try:
            self.assertTrue(host.KERNEL32.TerminateProcess(handle, 1), "the owned hook did not terminate")
            self.assertEqual(host.KERNEL32.WaitForSingleObject(handle, WAIT * 1000), 0,
                             "the owned hook's handle did not signal its exit")
        finally:
            host.KERNEL32.CloseHandle(handle)
        card = self.until("the screen card", lambda: next((card for card in self.inspect(name)["pending_questions"]
                                                           if card.get("answer_transport") == "claude_tui"
                                                           and card.get("can_answer")), None))
        _, receipt = self.answer(name, card, self.choose(card, "Yes"))
        self.assertEqual(receipt["transport"], "claude_tui", receipt)
        self.until("the approved file", (self.project / api.BASH_FILE).is_file)
        self.assertEqual(self.channels("answer.json") + self.channels("claimed.json"), [])

    def test_clear_context_starts_a_new_conversation_in_the_same_agent(self):
        name = self.launch("clear")
        self.deliver(name, MESSAGES[0])
        # Claude's SessionStart hook reports the new conversation before the receipt.
        row, record, _ = self.check_clear(name, "confirmed")
        self.assertNotIn(self.row(name)["conversation_id"], ("", row["conversation_id"]))
        self.assertEqual(self.inspect(name)["session_clear"]["type"], "SessionCleared")
        self.check_after_clear(name, row, record)

    def test_pause_resume_and_terminate(self):
        name = self.launch("lifecycle")
        self.check_lifecycle(name)

    def test_input_keys_reach_claude_through_the_host(self):
        self.check_keys(self.launch("keys"))

    def deliver_prompt(self, name, text):
        """Send a prompt whose turn waits for an answer; its receipt."""
        row = self.ready(name)
        receipt = self.send(name, self.message(row, text))
        self.assertEqual(receipt["status"], "submitted", receipt)
        return receipt

    # The answer channel.

    def channels(self, stage):
        """The answer channel files named `stage` in the case's state."""
        return sorted((self.state / "native_answers").glob(f"*/*/{stage}"))

    def channel_card(self, name, source):
        """The pending card of `source` once the answer channel waits for it."""
        return self.until(f"a {source} card of the answer channel", lambda: next(
            (card for card in self.inspect(name).get("pending_questions", [])
             if card.get("source") == source and card.get("answer_transport") == "claude_hook"
             and card.get("can_answer")), None))

    def hook_card(self, name, prompt):
        """Send `prompt`, whose Bash call waits for approval; its card once the answer channel waits."""
        self.deliver_prompt(name, prompt)
        return self.channel_card(name, "claude_tool_approval")

    def worker(self):
        """The (PID, start) of the answer channel that waits now."""
        def waiting():
            for path in self.channels("waiting.json"):
                stages = {entry.name for entry in path.parent.iterdir()}
                if "closed.json" not in stages:
                    record = json.loads(path.read_text(encoding="utf-8"))
                    if self.alive(record["pid"], record["process_start"]):
                        return record["pid"], record["process_start"]
            return None
        return self.until("the waiting answer channel", waiting)

    def choose(self, card, label):
        ids = [option["id"] for option in card["questions"][0]["options"] if option["label"] == label]
        self.assertEqual(len(ids), 1, card["questions"][0]["options"])
        return [dict(question_id="approval", selected_option_ids=ids, text="")]

    def refused(self, name, card, answers):
        """`hgs answer` for a card that is no longer pending fails and sends nothing."""
        row = self.row(name)
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=row["run_id"],
                       expected_conversation_id=row.get("conversation_id") or "", question_id=card["question_id"],
                       expected_question_hash=card["question_hash"], answers=answers)
        result = self.hgs("answer", name, "--json", input=json.dumps(payload), rc=None)
        self.assertNotEqual(result.returncode, 0, result.stdout)


@unittest.skipUnless(CODEX and HGS_SET, "set HGS_TEST_BIN and HGS_CODEX_TEST_BIN to run real Codex")
class NativeWindowsCodex(NativeAgent, host.HostCase):
    AGENT = "codex"
    BINARY = CODEX
    # Spike S6's arguments, beside full access and no approvals in its home: no sandbox.
    ARGS = ("--dangerously-bypass-approvals-and-sandbox",)
    COMPOSER = "›"
    # Codex asks to review the hooks hgs installs; only the approval case answers that.
    NO_HOOK_REVIEW = "--dangerously-bypass-hook-trust"

    def configure(self):
        trusted = [f"[projects.{json.dumps(form)}]\ntrust_level = \"trusted\"\n" for form in path_forms(self.project)]
        (self.agent_home / "config.toml").write_text("\n".join([
            "# A private CODEX_HOME: the provider is tests/model_api_fixture.py on 127.0.0.1.",
            'model = "gpt-6.1-sol"', 'model_provider = "fixture"', "check_for_update_on_startup = false",
            'sandbox_mode = "danger-full-access"', 'approval_policy = "never"', "",
            # Spike S3 recorded Codex 0.162.0's asynchronous questions with this feature on;
            # 0.162.1 offers request_user_input_async either way.
            "[features]", "send_message_to_user_async = true", "",
            "[model_providers.fixture]", 'name = "Fixture"', f'base_url = "{self.api_url}/v1"',
            'env_key = "OPENAI_API_KEY"', 'wire_api = "responses"', "request_max_retries = 0",
            "stream_max_retries = 0", "", *trusted]), encoding="utf-8")
        self.env["CODEX_HOME"] = str(self.agent_home)

    def transcript_rows(self, conversation_id):
        rows = []
        for path in sorted((self.agent_home / "sessions").rglob(f"rollout-*{conversation_id}.jsonl")):
            for line in path.read_text(encoding="utf-8").splitlines():
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass  # A line still being written.
        return rows

    def check_hook_form(self):
        """The shell form as `command` and, as Codex runs hooks with PowerShell on Windows,
        the PowerShell form as `commandWindows` (spec D12)."""
        hooks = json.loads((self.agent_home / "hooks.json").read_text(encoding="utf-8"))
        stable = os.path.realpath(self.bin / "hgs.exe").replace(os.sep, "/")
        self.assertEqual(hooks["hooks"]["Stop"], [{"hooks": [{
            "type": "command", "command": f'"{stable}" __state hook',
            "commandWindows": f'& "{stable}" __state hook; exit $LASTEXITCODE', "timeout": 5}]}])

    def user_messages(self, conversation_id):
        """The user messages Codex gave its model in a conversation, as their text."""
        messages = []
        for row in self.transcript_rows(conversation_id):
            payload = row.get("payload") if isinstance(row.get("payload"), dict) else {}
            if row.get("type") == "response_item" and (payload.get("type"), payload.get("role")) == ("message", "user"):
                messages.append("".join(part.get("text", "") for part in payload.get("content") or []
                                        if isinstance(part, dict) and part.get("type") == "input_text"))
        return messages

    def accepted(self, name, text, conversation_id=None):
        conversation_id = conversation_id or self.row(name).get("conversation_id")
        return self.user_messages(conversation_id).count(text) if conversation_id else 0

    def test_a_tracked_launch_reports_its_hooks_and_send_delivers_exact_messages(self):
        name = self.launch("messages", self.NO_HOOK_REVIEW)
        # Codex 0.162 starts its conversation with the first message, whose hooks confirm it;
        # hgs sends that message to the unconfirmed session (`first_message_can_send`).
        self.check_messages(name)
        row = self.row(name)
        rollout = next((self.agent_home / "sessions").rglob(f"rollout-*{row['conversation_id']}.jsonl"))
        self.assertTrue(os.path.samefile(self.record(name)["transcript"], rollout))
        self.assertTrue(self.inspect(name)["tracked"])
        self.check_hook_form()
        events = [event["type"] for event in self.inspect(name)["events"]]
        self.assertTrue({"SessionStart", "UserPromptSubmit", "Stop"} <= set(events), events)
        journal = self.journal(row["run_id"])
        self.assertEqual(journal.count("UserPromptSubmit"), len(MESSAGES), journal)

    def test_hgs_answers_request_user_input_async(self):
        name = self.launch("question", self.NO_HOOK_REVIEW)
        # The question does not block the turn: Codex accepts it and the turn ends.
        self.deliver(name, api.QUESTIONS)
        card = self.card(name, "codex_async")
        item, = card["questions"]
        self.assertEqual((item["question"], [option["label"] for option in item["options"]]),
                         (api.CODEX_QUESTIONS[0]["title"], api.CODEX_QUESTIONS[0]["options"]))
        # hgs waits 6 s for Codex to record the answer, then reports it `submitted`: delivered,
        # not yet confirmed (codex_questions::answer). Windows can take longer; the transcript
        # below confirms it either way.
        payload, receipt = self.answer(name, card, [dict(question_id="q_0", selected_option_ids=["opt_1"], text="")],
                                       ("answered", "submitted"))
        replay = self.json("answer", name, "--json", input=json.dumps(payload))
        self.assertEqual({**replay, "status": receipt["status"]}, receipt)
        self.assertIn(replay["status"], ("answered",) if receipt["status"] == "answered" else ("answered", "submitted"), replay)
        # Codex hands the answer on as a user message naming the native question.
        tag = "send_user_message_question_reply"
        reply, = self.until("the answer in the transcript", lambda: [
            text for text in self.user_messages(card["conversation_id"]) if f"<{tag}>" in text])
        body = reply.split(f"<{tag}>", 1)[1].split(f"</{tag}>", 1)[0]
        self.assertEqual(json.loads(body), [{"questionItemId": card["native_question_id"],
                                             "question": item["question"], "answer": "Two"}])
        self.until("the answered card to go", lambda: not [
            card for card in self.inspect(name).get("pending_questions", []) if card.get("source") == "codex_async"])

    def test_hgs_trusts_the_hooks_it_installed(self):
        # Without the bypass Codex stops at its review of the new hooks before SessionStart.
        name = self.launch("approval")
        card = self.card(name, "codex_hooks_trust")
        self.assertEqual([option["id"] for option in card["questions"][0]["options"]],
                         ["review", "trust", "continue_without_trusting"])
        self.answer(name, card, [dict(question_id="hooks_trust", selected_option_ids=["trust"], text="")])
        # The trusted hooks run: the first message's hooks confirm the conversation.
        self.deliver(name, MESSAGES[0])
        row = self.row(name)
        self.assertTrue(row.get("conversation_id"), row)
        self.assertEqual(self.accepted(name, MESSAGES[0]), 1, self.describe())
        events = [event["type"] for event in self.inspect(name)["events"]]
        self.assertTrue({"SessionStart", "UserPromptSubmit", "Stop"} <= set(events), events)

    def test_clear_context_starts_a_new_conversation_in_the_same_agent(self):
        name = self.launch("clear", self.NO_HOOK_REVIEW)
        self.deliver(name, MESSAGES[0])
        self.settled(name)
        # Its header and folder still show above the first message, which is no reset panel.
        self.assertIsNone(self.inspect(name)["session_clear"],
                          "hgs took Codex's header above a message for its reset panel (zerus-g9dd)" + self.describe())
        # Codex shows its reset panel at once and reports the new conversation with the next
        # message's SessionStart. hgs confirms the panel when it names the project folder as
        # Codex prints it, which below the real profile is a `~\…` form of another home.
        outside = not below(self.project, os.environ.get("USERPROFILE") or os.path.expanduser("~"))
        row, record, _ = self.check_clear(name, "confirmed" if outside else None)
        if outside:
            self.assertEqual(self.inspect(name)["session_clear"]["type"], "SessionCleared")
        self.check_after_clear(name, row, record)
        self.assertEqual(self.inspect(name)["session_clear"]["type"], "SessionCleared")

    def test_pause_resume_and_terminate(self):
        name = self.launch("lifecycle", self.NO_HOOK_REVIEW)
        self.check_lifecycle(name)

    def test_input_keys_reach_codex_through_the_host(self):
        self.check_keys(self.launch("keys", self.NO_HOOK_REVIEW))
