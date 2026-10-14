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

Tracked agents (Claude Code, Codex, Kimi) run with their hooks (spec D12);
test_windows_hooks.py launches them, with private agent homes.

`Attach` runs `hgs a` and a launch without `-d` on a pseudoconsole of its own, as
the GUI's embedded terminal runs `hgs.exe a NAME --existing --run-id ID` (spec
D13): keys and the prefix bindings reach the agent, the pane follows the client's
size, a changed run is refused, and the session's end and the host's death end
the client. Without a console `hgs a` refuses. `Listings` checks `local_host` in
the local box of `hgs ls --json` (spec D2).

`AgentInstall` runs `hgs account install` with only fakes and System32 on PATH:
npm runs through Node.js's `npm.cmd` (missing npm is reported), Claude Code and
Kimi Code use their vendors' PowerShell installers, never install.sh, a failed
download runs nothing and leaves no file, and an agent in Kimi Code's install
folder is found before PATH lists it.

`Supervisor` covers the supervisor channel (spec D10, D11, D14) with untracked
sessions: input checks of the console the supervisor reports (a pager the agent
starts, cooked input, an agent that is gone while its supervisor runs on), the
resume fallback after a quick failure and after crashes, a pause and a resume of
`kimi` (a renamed fake agent), terminate's pause before its hang-up, and the
recovery worker that a running host supervises.

HGS_TEST_BIN names the hgs.exe under test (default target/debug/hgs.exe), the
"portable" copy that every case launches from; HGS_TEST_FAKE_AGENT names the fake
agent (default target/debug/examples/fake_agent.exe, `cargo build --locked
--example fake_agent`). Cases share the hermetic rules of test_windows_host.py.
"""
import csv
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import threading
import time
import unittest
import uuid

from test_windows_host import FAKE_AGENT, HGS, HIDDEN, LIMIT, HostCase, open_host

if os.name == "nt":
    import _winapi
    from ctypes import wintypes

    KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    KERNEL32.OpenProcess.restype = wintypes.HANDLE
    KERNEL32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    KERNEL32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                    ctypes.POINTER(wintypes.DWORD))
    KERNEL32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    KERNEL32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 0x102

    class COORD(ctypes.Structure):
        _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
                    ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                    ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
                    ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD),
                    ("dwFlags", wintypes.DWORD), ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                    ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                    ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE)]

    class STARTUPINFOEXW(ctypes.Structure):
        _fields_ = [("StartupInfo", STARTUPINFOW), ("lpAttributeList", ctypes.c_void_p)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE), ("dwProcessId", wintypes.DWORD),
                    ("dwThreadId", wintypes.DWORD)]

    KERNEL32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
    KERNEL32.CreatePipe.argtypes = (ctypes.POINTER(wintypes.HANDLE), ctypes.POINTER(wintypes.HANDLE),
                                    ctypes.c_void_p, wintypes.DWORD)
    KERNEL32.ReadFile.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                  ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    KERNEL32.WriteFile.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    KERNEL32.InitializeProcThreadAttributeList.argtypes = (ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                                           ctypes.POINTER(ctypes.c_size_t))
    KERNEL32.UpdateProcThreadAttribute.argtypes = (ctypes.c_void_p, wintypes.DWORD, ctypes.c_size_t,
                                                   ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                                   ctypes.c_void_p)
    KERNEL32.DeleteProcThreadAttributeList.argtypes = (ctypes.c_void_p,)
    KERNEL32.CreateProcessW.argtypes = (wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                        wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                        ctypes.c_void_p, ctypes.POINTER(PROCESS_INFORMATION))
    CONPTY = hasattr(KERNEL32, "CreatePseudoConsole")
    if CONPTY:
        KERNEL32.CreatePseudoConsole.argtypes = (COORD, wintypes.HANDLE, wintypes.HANDLE, wintypes.DWORD,
                                                 ctypes.POINTER(ctypes.c_void_p))
        KERNEL32.CreatePseudoConsole.restype = ctypes.c_long
        KERNEL32.ResizePseudoConsole.argtypes = (ctypes.c_void_p, COORD)
        KERNEL32.ResizePseudoConsole.restype = ctypes.c_long
        KERNEL32.ClosePseudoConsole.argtypes = (ctypes.c_void_p,)
        KERNEL32.ClosePseudoConsole.restype = None
    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
    EXTENDED_STARTUPINFO_PRESENT = 0x00080000
    CREATE_UNICODE_ENVIRONMENT = 0x00000400
    STARTF_USESTDHANDLES = 0x00000100
else:
    CONPTY = False

# Terminal sequences, which a pseudoconsole mixes into what it draws.
SEQUENCE = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[()][0-9A-Za-z]|\x1b.")


def squeeze(data):
    """Text a pseudoconsole drew, without terminal sequences, blanks or line breaks:
    ConPTY may draw blanks as cursor moves."""
    text = SEQUENCE.sub(b"", data).decode("utf-8", "replace")
    return "".join(character for character in text if character.isprintable() and not character.isspace())


class PseudoConsole:
    """A program on a pseudoconsole of its own, as the GUI's embedded terminal runs
    `hgs.exe a`: the test types into it, resizes it and reads what it draws. The
    program's standard handles are the pseudoconsole's, never the test's."""

    def __init__(self, argv, env, cwd, size=(100, 30)):
        self.output = bytearray()
        self.lock = threading.Lock()
        self.console = ctypes.c_void_p()
        self.process = None
        console_input, self.input = wintypes.HANDLE(), wintypes.HANDLE()
        self.reader, console_output = wintypes.HANDLE(), wintypes.HANDLE()
        if not KERNEL32.CreatePipe(ctypes.byref(console_input), ctypes.byref(self.input), None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        if not KERNEL32.CreatePipe(ctypes.byref(self.reader), ctypes.byref(console_output), None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            result = KERNEL32.CreatePseudoConsole(COORD(*size), console_input, console_output, 0,
                                                  ctypes.byref(self.console))
            if result != 0:
                raise OSError(f"CreatePseudoConsole failed with {result & 0xFFFFFFFF:#010x}")
            needed = ctypes.c_size_t()
            KERNEL32.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(needed))
            attributes = ctypes.create_string_buffer(needed.value)
            if not KERNEL32.InitializeProcThreadAttributeList(attributes, 1, 0, ctypes.byref(needed)):
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                if not KERNEL32.UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                                          self.console, ctypes.sizeof(ctypes.c_void_p), None, None):
                    raise ctypes.WinError(ctypes.get_last_error())
                startup = STARTUPINFOEXW()
                startup.StartupInfo.cb = ctypes.sizeof(STARTUPINFOEXW)
                # NULL standard handles become the pseudoconsole's.
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES
                startup.lpAttributeList = ctypes.cast(attributes, ctypes.c_void_p)
                variables = sorted(env.items(), key=lambda item: item[0].upper())
                block = "".join(f"{key}={value}\0" for key, value in variables)
                environment = ctypes.create_string_buffer((block + "\0").encode("utf-16-le") + b"\0\0")
                command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(word) for word in argv]))
                information = PROCESS_INFORMATION()
                if not KERNEL32.CreateProcessW(None, command, None, None, False,
                                               EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                               environment, str(cwd), ctypes.byref(startup),
                                               ctypes.byref(information)):
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                KERNEL32.DeleteProcThreadAttributeList(attributes)
            self.process = information.hProcess
            KERNEL32.CloseHandle(information.hThread)
        except BaseException:
            self.close()
            raise
        finally:
            # The pseudoconsole holds its own copies.
            KERNEL32.CloseHandle(console_input)
            KERNEL32.CloseHandle(console_output)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        buffer = ctypes.create_string_buffer(4096)
        read = wintypes.DWORD()
        while KERNEL32.ReadFile(self.reader, buffer, len(buffer), ctypes.byref(read), None) and read.value:
            with self.lock:
                self.output += buffer.raw[:read.value]

    def keys(self, data):
        written = wintypes.DWORD()
        if not KERNEL32.WriteFile(self.input, data, len(data), ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())

    def resize(self, cols, rows):
        result = KERNEL32.ResizePseudoConsole(self.console, COORD(cols, rows))
        if result != 0:
            raise OSError(f"ResizePseudoConsole failed with {result & 0xFFFFFFFF:#010x}")

    def shown(self):
        with self.lock:
            return squeeze(bytes(self.output))

    def shows(self, text):
        return squeeze(text.encode()) in self.shown()

    def wait(self, timeout=LIMIT):
        """The program's exit code once it has exited, with everything it wrote
        drawn. A pseudoconsole draws in frames of its own, the last ones after
        its program has ended, so the test then does what the GUI's embedded
        terminal does (tray/src/PtyProcess_win.cpp): it closes the
        pseudoconsole, which paints its last frame and ends its output, and
        reads to the end."""
        if KERNEL32.WaitForSingleObject(self.process, int(timeout * 1000)) != WAIT_OBJECT_0:
            raise AssertionError(f"the program did not end; it shows {self.shown()!r}")
        code = wintypes.DWORD()
        if not KERNEL32.GetExitCodeProcess(self.process, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        if self.console:
            KERNEL32.ClosePseudoConsole(self.console)
            self.console = ctypes.c_void_p()
        self.thread.join(timeout)
        return code.value

    def close(self):
        """Close the pseudoconsole (its programs get CTRL_CLOSE_EVENT) and end
        what is left."""
        if self.console:
            KERNEL32.ClosePseudoConsole(self.console)
            self.console = ctypes.c_void_p()
        if self.process:
            if KERNEL32.WaitForSingleObject(self.process, 5000) != WAIT_OBJECT_0:
                KERNEL32.TerminateProcess(self.process, 1)
            KERNEL32.CloseHandle(self.process)
            self.process = None
        for handle in ("input", "reader"):
            if getattr(self, handle):
                if handle == "reader" and getattr(self, "thread", None):
                    self.thread.join(LIMIT)
                KERNEL32.CloseHandle(getattr(self, handle))
                setattr(self, handle, None)


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


def read_events(path, kind=None):
    """The complete records a fake agent appended to `path`, of `kind` when given."""
    lines = path.read_text(errors="replace").split("\n")[:-1] if path.exists() else []
    return [event for event in map(json.loads, lines) if kind is None or event["kind"] == kind]


def deny_execute(case, path):
    """Deny this user the right to run `path`, as for an agent installed by another
    account; the case gives it back when it ends."""
    user = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], capture_output=True, text=True,
                          creationflags=HIDDEN, stdin=subprocess.DEVNULL, check=True).stdout
    sid = next(csv.reader([user.strip()]))[1]

    def icacls(*args):
        return subprocess.run(["icacls", str(path), *args], capture_output=True, text=True,
                              creationflags=HIDDEN, stdin=subprocess.DEVNULL)

    denied = icacls("/deny", f"*{sid}:(X)")
    case.assertEqual(denied.returncode, 0, denied.stdout + denied.stderr)
    case.addCleanup(icacls, "/remove:d", f"*{sid}")


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
        return read_events(self.record, kind)

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

    def test_a_program_that_cannot_start_fails_its_launch(self):
        # The GUI's launch: detached, with a launch ID. The supervisor cannot start
        # the program, says why on the pane and ends at once, before any record
        # exists. The launch fails with what it said instead of reporting a start
        # that the GUI would wait for (zerus-ziq3.10).
        denied = self.bin / "denied_agent.exe"
        shutil.copy2(FAKE_AGENT, denied)
        deny_execute(self, denied)
        for program, code, said in (("missing_agent", 127, "hgs: missing_agent: not found on PATH"),
                                    ("denied_agent", 126, "hgs: denied_agent: Access is denied. (os error 5)")):
            with self.subTest(program=program):
                name = f"{program}/project/x"
                result = self.hgs(program, str(self.project), "--new", "-n", "x", "-d", "--launch-id",
                                  str(uuid.uuid4()), rc=1)
                self.status()  # Remember the host process for the cleanup.
                self.assertEqual(result.stdout, "")
                self.assertIn(f"hgs: session {name} ended immediately (exit {code}):\n", result.stderr)
                self.assertIn(said, result.stderr)
                self.assertNotIn(name, self.sessions())
                self.assertIn(f"ended: exit {code} ", (self.state / "host.log").read_text(errors="replace"))
        # A program that runs is started once it has run for the startup time.
        started = time.monotonic()
        self.launch("runs")
        self.assertGreaterEqual(time.monotonic() - started, 3)

    def test_unix_only_commands_are_refused_instead_of_launched(self):
        # Only Unix dispatches `notify`, `clip` and `paste`; here they fail
        # rather than start a program of that name, such as clip.exe (zerus-pxjr).
        for command, args in (("notify", ["notify", "claude/p"]), ("clip", ["clip"]),
                              ("paste", ["paste", "claude/p", "%1"]), ("clip", ["--dry-run", "clip"])):
            with self.subTest(args=args):
                result = self.hgs(*args, rc=2)
                self.assertEqual((result.stdout, result.stderr),
                                 ("", f"hgs: {command} is not supported on Windows\n"))
        self.assertEqual(self.sessions(), {})
        self.assertEqual(self.status(), {"running": False})

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

    def test_attach_without_a_console_and_other_executables_are_refused(self):
        # Attaching a terminal needs a console (D13): pipes are refused first.
        refused = self.hgs("fake_agent", str(self.project), "--new", "-n", "attached", rc=1)
        self.assertIn("attaching needs a console", refused.stderr)
        # Sessions name only the stable CLI (D8).
        portable = dict(self.env, HGS_EXECUTABLE=str(HGS))
        refused = self.hgs("fake_agent", str(self.project), "--new", "-n", "portable", "-d", env=portable, rc=1)
        self.assertIn("only the stable hgs CLI", refused.stderr)
        self.assertEqual(self.status(), {"running": False})
        self.assertEqual(self.sessions(), {})

        name, _ = self.launch("one")
        refused = self.hgs("a", name, rc=1)
        self.assertIn("attaching needs a console", refused.stderr)
        self.assertEqual(self.sessions()[name]["attached"], 0)

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
        # A session takes its folder's letter case on disk; a junction keeps its own name (spec D15).
        _winapi.CreateJunction(str(self.project), str(self.work / "site"))
        for folder, name in (("PROJECT", "fake_agent/project/cased"), ("site", "fake_agent/site/cased")):
            printed = self.hgs("--dry-run", "fake_agent", str(self.work / folder), "--new", "-n", "cased", "-d").stdout
            self.assertTrue(printed.startswith(f"session-host create-session -d -s {name} -c "), printed)
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
        # The supervisor's channel answers input checks for its agent (D10).
        state = self.backend({"op": "input_state", "pane": pane, "name": name, "run_id": "run-1",
                              "agent": "claude", "pid": ready["pid"]})
        self.assertEqual(set(state), {"cursor_x", "cursor_y"})
        # Terminate asks the supervisor to pause the agent first (D11).
        self.backend({"op": "kill_session", "target": {"session": session_id}})
        self.wait_for_exit(ready)
        self.wait_for("the end", lambda: not self.backend({"op": "listing"})["sessions"])
        self.assertEqual(self.backend({"op": "session_name", "id": session_id}), {"name": None})
        self.assertEqual([event["signal"] for event in self.events("signal")], ["CTRL_BREAK"])


class Supervisor(LifecycleCase):
    """The `hgs.exe --run` supervisor's channel to the host (spec D10, D11): input
    checks judge the console it reports, a pause is its Ctrl+Break for the process
    group it shares with its agent, and terminate pauses before it hangs up."""

    def setUp(self):
        super().setUp()
        # A pager and Kimi stand-ins: renamed fake agents, configured by variables.
        for name in ("less.exe", "kimi.exe"):
            shutil.copy2(FAKE_AGENT, self.bin / name)

    def claim(self, name, pane, pid, run_id="run-1"):
        return {"op": "input_state", "pane": pane, "name": name, "run_id": run_id, "agent": "fake_agent",
                "pid": pid}

    def ready_for_input(self, name, pane):
        """The run marker a tracked launch sets, and the agent's bracketed paste."""
        self.backend({"op": "set_run_marker", "name": name, "run_id": "run-1"})
        self.wait_for("bracketed paste", lambda: self.backend(
            {"op": "pane_info", "target": {"pane": pane}, "fields": ["bracketed_paste"]})["text"] == "1")

    def refusal(self, claim):
        """The error of an input check, or None when it is accepted."""
        result = self.run_hgs("backend", json.dumps(claim))
        answer = json.loads(result.stdout)
        return answer.get("error")

    def kimi_env(self, tag):
        """A launch of `kimi` untracked, whose resume attempt has a minute to count
        as a resume; its records, ready file and control file are its own."""
        paths = {kind: self.root / f"{tag}.{kind}" for kind in ("jsonl", "ready", "control")}
        env = dict(self.env, HGS_TRACKING="0", HGS_FALLBACK_SECS="60", FAKE_AGENT_RECORD=str(paths["jsonl"]),
                   FAKE_AGENT_READY=str(paths["ready"]), FAKE_AGENT_CONTROL=str(paths["control"]),
                   FAKE_AGENT_PROMPT=f"{tag}> ")
        return env, paths["jsonl"], paths["ready"]

    def launch_kimi(self, tag, env, ready, previous=None):
        """`hgs kimi PROJECT -n TAG -d -c`: `--run 2 kimi -c kimi`. Its session and
        the ready record of an agent other than `previous`."""
        name = f"kimi/project/{tag}"
        result = self.hgs("kimi", str(self.project), "-n", tag, "-d", "-c", env=env)
        self.assertEqual(result.stdout, f"hgs: started {name}\n")
        started = self.wait_for("the agent", lambda: self.started(ready, previous))
        self.status()
        return name, started

    def started(self, ready, previous=None):
        record = read_json(ready)
        return record if record and (previous is None or record["pid"] != previous["pid"]) else None

    def end_process(self, ready, code):
        """End the agent of `ready` as a crash or a kill does, with exit `code`."""
        handle = open_host(ready["pid"], ready["process_start"])
        self.assertTrue(handle, "the agent runs")
        try:
            self.assertTrue(KERNEL32.TerminateProcess(handle, code))
            self.assertEqual(KERNEL32.WaitForSingleObject(handle, LIMIT * 1000), WAIT_OBJECT_0)
        finally:
            KERNEL32.CloseHandle(handle)

    def host_log(self):
        path = self.state / "host.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def test_input_checks_judge_the_console_its_supervisor_reports(self):
        less = f'"{self.bin / "less.exe"}"'
        env = dict(self.env, FAKE_AGENT_ON_EOF="ignore")
        name, ready = self.launch("one", "--hook-command", less, "--hook-events", "submit", "--hook-timeout",
                                  "8000", env=env)
        pane = self.pane(name)["pane"]
        self.ready_for_input(name, pane)
        # An idle agent: raw input, alive in the session's job, nothing else attached.
        state = self.backend(self.claim(name, pane, ready["pid"]))
        self.assertEqual(set(state), {"cursor_x", "cursor_y"})
        self.assertIn("not attached", self.refusal(self.claim(name, pane, ready["pid"] + 4)))
        self.assertIn("expected agent input mode", self.refusal(self.claim(name, pane, ready["pid"], "run-2")))

        # A pager the agent started takes over the console while it runs.
        staged = self.root / "control.tmp"
        staged.write_text("hook submit look\n")
        os.replace(staged, self.control)
        refused = self.wait_for("the pager", lambda: self.refusal(self.claim(name, pane, ready["pid"])))
        self.assertIn("expected agent input mode", refused)
        hook, = self.wait_for("the pager's end", lambda: self.events("hook"))
        self.assertTrue(hook["timeout"], hook)
        self.assertIsNone(self.refusal(self.claim(name, pane, ready["pid"])))
        self.hgs("kill", name)
        self.wait_for("the end", lambda: not self.sessions())

    def test_cooked_input_is_refused(self):
        name, ready = self.launch("cooked", "--raw", "0")
        pane = self.pane(name)["pane"]
        self.ready_for_input(name, pane)
        self.assertIn("does not own an interactive input terminal",
                      self.refusal(self.claim(name, pane, ready["pid"])))

    def test_a_quick_failed_resume_starts_fresh_and_its_agent_is_refused(self):
        env, record, ready = self.kimi_env("quick")
        name, first = self.launch_kimi("quick", env, ready)
        self.assertEqual(first["argv"], ["-c"])
        pane = self.pane(name)
        self.ready_for_input(name, pane["pane"])
        self.assertIsNone(self.refusal(self.claim(name, pane["pane"], first["pid"])))

        # The resume attempt fails at once: nothing to resume, so the supervisor
        # stays and starts the agent afresh. The ended agent's raw console stays.
        self.end_process(first, 1)
        fresh = self.wait_for("the fresh start", lambda: self.started(ready, first))
        self.assertEqual(fresh["argv"], [])
        self.assertEqual(self.pane(name)["pid"], pane["pid"])
        self.assertIn("not attached", self.refusal(self.claim(name, pane["pane"], first["pid"])))
        self.wait_for("the fresh agent's input", lambda: self.refusal(
            self.claim(name, pane["pane"], fresh["pid"])) is None)
        self.assertIn("nothing to resume", self.backend({"op": "capture", "pane": pane["pane"]})["text"])
        self.hgs("kill", name)
        self.wait_for("the end", lambda: not self.sessions())

    def test_a_crashed_resume_ends_the_session_without_a_fresh_start(self):
        # An access violation and a missing DLL read as signals (spec D9), not as
        # "nothing to resume".
        for tag, status in (("segv", 0xC0000005), ("dll", 0xC0000135)):
            env, record, ready = self.kimi_env(tag)
            name, first = self.launch_kimi(tag, env, ready)
            session = self.pane(name)["session_id"]
            logged = len(self.host_log())
            self.end_process(first, status)
            self.wait_for("the end", lambda: name not in self.sessions())
            self.assertEqual(len(read_events(record, "start")), 1, tag)
            self.assertIn(f"session {session} ended: exit {134 if tag == 'dll' else 139} ",
                          self.host_log()[logged:])

    def test_a_pause_ends_the_agent_and_a_resume_relaunches_it(self):
        env, record, ready = self.kimi_env("paused")
        name, first = self.launch_kimi("paused", env, ready)
        pane = self.pane(name)
        logged = len(self.host_log())
        # `hgs pause` asks the backend to stop the agent; the supervisor raises
        # Ctrl+Break for its group (D11), and the agent ends as Codex and Kimi do.
        started = time.monotonic()
        self.backend({"op": "request_exit", "pane": pane["pane"], "pid": first["pid"]})
        self.wait_for_exit(first)
        self.wait_for("the end", lambda: name not in self.sessions())
        self.assertLess(time.monotonic() - started, 10)
        self.assertEqual([event["signal"] for event in read_events(record, "signal")], ["CTRL_BREAK"])
        self.assertEqual(read_events(record, "exit")[-1]["cause"], "signal:CTRL_BREAK")
        # A paused resume is final: the supervisor recorded the stop and ended
        # with STATUS_CONTROL_C_EXIT read as SIGINT, without a fresh start.
        self.assertEqual(len(read_events(record, "start")), 1)
        self.assertIn(f"session {pane['session_id']} ended: exit 130 (0x00000082)", self.host_log()[logged:])

        # A resume starts it again, with its resume arguments.
        name, again = self.launch_kimi("paused", env, ready, previous=first)
        self.assertEqual(again["argv"], ["-c"])
        self.assertEqual(len(read_events(record, "start")), 2)
        self.hgs("kill", name)
        self.wait_for("the end", lambda: not self.sessions())

    def test_a_paused_resume_is_final_whatever_its_exit_code(self):
        # An agent that exits with an ordinary failure on Ctrl+Break: only the
        # stop request the supervisor recorded keeps it from starting fresh.
        env, record, ready = self.kimi_env("ordinary")
        env["FAKE_AGENT_ON_SIGNAL"] = "CTRL_BREAK=exit:1"
        name, first = self.launch_kimi("ordinary", env, ready)
        pane = self.pane(name)
        logged = len(self.host_log())
        self.backend({"op": "request_exit", "pane": pane["pane"], "pid": first["pid"]})
        self.wait_for("the end", lambda: name not in self.sessions())
        self.assertEqual(read_events(record, "exit")[-1]["code"], 1)
        self.assertEqual(len(read_events(record, "start")), 1)
        self.assertIn(f"session {pane['session_id']} ended: exit 1 (0x00000001)", self.host_log()[logged:])

    def test_terminate_pauses_first_and_hangs_up_an_agent_that_stays(self):
        name, ready = self.launch("stays", "--on-signal", "CTRL_BREAK=ignore")
        started = time.monotonic()
        self.backend({"op": "kill_session", "target": {"name": name}})
        self.wait_for_exit(ready)
        elapsed = time.monotonic() - started
        self.wait_for("the end", lambda: not self.sessions())
        # The pause, the 10 s grace for the supervisor, then the closed console,
        # which ends the input and sends CTRL_CLOSE in either order.
        self.assertGreaterEqual(elapsed, 9, elapsed)
        self.assertLess(elapsed, 25, elapsed)
        signals = [event["signal"] for event in self.events("signal")]
        self.assertEqual(signals[0], "CTRL_BREAK", signals)
        self.assertLessEqual(set(signals), {"CTRL_BREAK", "CTRL_CLOSE"}, signals)
        self.assertIn(self.events("exit")[-1]["cause"], ("eof", "signal:CTRL_CLOSE"))

    def test_the_host_supervises_the_recovery_worker_while_it_runs(self):
        heartbeat = self.state / "recovery" / "heartbeat.json"

        def recovery(*args, payload=None):
            result = subprocess.run([str(HGS), "recovery", *args], env=self.env, cwd=self.work, input=payload,
                                    capture_output=True, text=True, timeout=LIMIT * 2, creationflags=HIDDEN)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return json.loads(result.stdout)

        # Enabled without a host: nothing starts a worker, and no host starts.
        self.assertTrue(recovery("set", payload=json.dumps({"enabled": True}))["policy"]["enabled"])
        self.assertEqual(self.status(), {"running": False})
        self.assertFalse(heartbeat.exists())

        # With a host, the next request has it run the stable CLI's worker (D14).
        name, _ = self.launch("one")
        recovery("get")
        beat = self.wait_for("the worker", lambda: read_json(heartbeat))
        self.assertTrue(beat["enabled"], beat)

        # The worker ends with the host.
        self.hgs("kill", name)
        self.wait_for("the idle exit", lambda: not self.status()["running"], timeout=LIMIT + 10)
        self.wait_for_hosts()
        written = heartbeat.stat().st_mtime_ns
        time.sleep(8)
        self.assertEqual(heartbeat.stat().st_mtime_ns, written, "no worker runs without a host")


class Listings(LifecycleCase):
    def local_box(self):
        return json.loads(self.hgs("ls", "--json", "--local").stdout)

    def test_the_local_box_reports_the_session_host(self):
        none = dict(pid=None, protocol=None, conpty=None, integrity=None, windows_session=None)
        # Enabled, no host yet: local_sessions stays absent, which means true (D2).
        box = self.local_box()
        self.assertNotIn("local_sessions", box)
        self.assertEqual(box["local_host"], dict(state="stopped", **none))
        self.assertEqual(self.status(), {"running": False})

        self.launch("one")
        status = self.status()
        host = self.local_box()["local_host"]
        self.assertEqual({key: host[key] for key in ("state", "pid", "protocol", "integrity", "windows_session")},
                         dict(state="running", pid=status["host"]["pid"], protocol=status["host"]["protocol"],
                              integrity=status["host"]["integrity"]["name"],
                              windows_session=status["host"]["windows_session"]))
        self.assertEqual(host["conpty"], status["status"]["conpty"])
        self.assertIn(host["conpty"], ("bundled", "inbox"))
        self.hgs("kill", "fake_agent/project/one")
        self.wait_for("the idle exit", lambda: not self.status()["running"], timeout=LIMIT + 10)
        self.wait_for_hosts()

        # Disabled: a control console, whose host is not installed.
        result = subprocess.run([str(HGS), "session-host", "uninstall", "--json"], env=self.env,
                                capture_output=True, text=True, timeout=LIMIT, creationflags=HIDDEN,
                                stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stderr)
        box = self.local_box()
        self.assertIs(box["local_sessions"], False)
        self.assertEqual(box["local_host"], dict(state="not-installed", **none))


class AgentInstall(LifecycleCase):
    """`hgs account install` on a computer with local sessions (src/accounts/install.rs)."""

    def setUp(self):
        super().setUp()
        # Only the fakes and System32: no real npm, installer or agent can run.
        self.env["PATH"] = os.pathsep.join((str(self.bin), str(Path(os.environ["SystemRoot"]) / "System32")))

    def test_npm_runs_through_its_command_file(self):
        # std looks only for npm.exe; Node.js installs npm.cmd.
        (self.bin / "npm.cmd").write_bytes(b'@echo off\r\n>"%~dp0npm-args.txt" echo %*\r\n')
        result = self.hgs("account", "install", "codex")
        self.assertIn("Installing codex on this machine.", result.stdout)
        self.assertEqual((self.bin / "npm-args.txt").read_text().strip(), "install -g @openai/codex")

    def test_without_npm_codex_asks_for_node_js(self):
        result = self.hgs("account", "install", "codex", rc=1)
        self.assertIn("Install Node.js and npm on this machine, then retry Install agent.", result.stderr)

    def test_a_failed_installer_download_runs_nothing_and_leaves_no_file(self):
        # hgs runs System32 curl, which no fake replaces; a proxy port that refuses
        # every connection fails the download before anything leaves this computer.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        temp = self.root / "temp"
        temp.mkdir()
        env = {key: value for key, value in self.env.items() if key.upper() not in ("NO_PROXY", "ALL_PROXY")}
        env.update(HTTPS_PROXY=f"http://127.0.0.1:{port}", TEMP=str(temp), TMP=str(temp))
        result = self.hgs("account", "install", "kimi", rc=1, env=env)
        self.assertIn("Could not download the agent installer. Nothing was executed.", result.stderr)
        self.assertEqual(list(temp.iterdir()), [])

    def test_claude_and_kimi_use_the_vendors_powershell_installers(self):
        for provider, url in (("claude", "https://claude.ai/install.ps1"),
                              ("kimi", "https://code.kimi.com/kimi-code/install.ps1")):
            with self.subTest(provider=provider):
                planned = json.loads(self.hgs("account", "install", provider, "--dry-run").stdout)
                self.assertEqual(planned, dict(provider=provider, package="", installer_url=url, dry_run=True))

    def test_kimi_code_is_found_in_its_install_folder_before_path_lists_it(self):
        # Its installer adds the folder to the user's PATH, which the running GUI
        # and the hgs it starts do not see until they are restarted.
        folder = self.home / ".kimi-code" / "bin"
        folder.mkdir(parents=True)

        def kimi():
            listing = json.loads(self.hgs("account", "ls").stdout)
            return next(row for row in listing["profiles"] if row["id"] == "native-kimi")

        self.assertFalse(kimi()["installed"])
        shutil.copy2(FAKE_AGENT, folder / "kimi.exe")
        self.assertTrue(kimi()["installed"])


@unittest.skipUnless(CONPTY, "pseudoconsoles need Windows 10 1809 or later")
class Attach(LifecycleCase):
    """`hgs a` and an attached launch on a pseudoconsole (spec D13)."""

    INPUT = {"text", "paste", "enter", "esc", "ctrl-c", "control", "key", "reply"}

    def console(self, *args, size=(100, 30)):
        console = PseudoConsole([HGS, *args], self.env, self.work, size)
        self.addCleanup(console.close)
        return console

    def received(self):
        """Every byte the fake agent read, in order."""
        return bytes.fromhex("".join(event["hex"] for event in self.events()
                                     if event["kind"] in self.INPUT and "hex" in event))

    def row(self, name):
        return self.sessions()[name]

    def test_keys_the_prefix_and_the_size_reach_the_agent_until_ctrl_b_d(self):
        name, ready = self.launch("one")
        console = self.console("a", name)
        self.wait_for("the repaint", lambda: console.shows("fake>"))
        console.keys(b"hi")
        self.wait_for("the keys", lambda: self.received().endswith(b"hi"))
        # Listed with an opaque client ID instead of a tty (D13).
        row = self.row(name)
        self.assertEqual(row["attached"], 1, row)
        self.assertEqual(len(row["clients"]), 1, row)
        self.assertRegex(row["clients"][0], r"^c[0-9]+$")
        # Ctrl+B Ctrl+B sends one Ctrl+B; Ctrl+B and another key pass through.
        console.keys(b"\x02\x02")
        console.keys(b"\x02x")
        self.wait_for("the prefix keys", lambda: self.received().endswith(b"hi\x02\x02x"))
        # The pane follows the client's window.
        console.resize(120, 40)
        self.wait_for("the resize", lambda: any((event["cols"], event["rows"]) == (120, 40)
                                                for event in self.events("resize")))
        console.keys(b"\x02d")
        self.assertEqual(console.wait(), 0)
        self.assertTrue(console.shows(f"[detached (from session {name})]"), console.shown())
        time.sleep(0.5)
        self.assertTrue(self.received().endswith(b"hi\x02\x02x"), self.received())
        self.assertEqual((self.row(name)["attached"], self.row(name)["clients"]), (0, []))
        self.assertTrue(self.alive(ready))

    def test_run_ids_ctrl_b_ctrl_d_and_the_session_end(self):
        name, ready = self.launch("one", "--exit-keys", "quit")
        self.backend({"op": "set_run_marker", "name": name, "run_id": "run-1"})
        # The GUI's form: a changed run is refused, the current one attaches.
        console = self.console("a", name, "--existing", "--run-id", "run-2")
        self.assertEqual(console.wait(), 1)
        self.assertTrue(console.shows("session run changed; refresh before reconnecting"), console.shown())
        console = self.console("a", name, "--existing", "--run-id", "run-1")
        self.wait_for("the repaint", lambda: console.shows("fake>"))
        console.keys(b"\x02\x04")
        self.assertEqual(console.wait(), 0)
        self.assertTrue(console.shows("[detached"), console.shown())

        # The session's end ends the client as tmux's [exited] does.
        console = self.console("a", name)
        self.wait_for("the repaint", lambda: console.shows("fake>"))
        console.keys(b"quit")
        self.assertEqual(console.wait(), 0)
        self.assertTrue(console.shows("[exited]"), console.shown())
        self.wait_for("the end", lambda: name not in self.sessions())
        # --existing never restores or starts a session.
        console = self.console("a", name, "--existing")
        self.assertEqual(console.wait(), 1)
        self.assertTrue(console.shows("session is no longer running; resume it explicitly"), console.shown())
        self.assertEqual(len(self.events("start")), 1)

    def test_a_launch_without_d_attaches_at_the_console_size(self):
        name = "fake_agent/project/live"
        console = self.console("fake_agent", str(self.project), "--new", "-n", "live", "--",
                               "--record", str(self.record), "--ready", str(self.ready), "--prompt", "fake> ",
                               "--exit-keys", "quit", size=(110, 35))
        self.wait_for("the agent", lambda: read_json(self.ready))
        self.status()  # Remember the host process for the cleanup.
        self.wait_for("the prompt", lambda: console.shows("fake>"))
        pane = self.pane(name)["pane"]
        size = self.backend({"op": "pane_info", "target": {"pane": pane}, "fields": ["width", "height"]})["text"]
        self.assertEqual(size.split(), ["110", "35"])
        console.keys(b"quit")
        self.assertEqual(console.wait(), 0)
        self.assertTrue(console.shows("[exited]"), console.shown())

    def test_the_host_death_restores_the_console_and_is_reported(self):
        name, _ = self.launch("one")
        console = self.console("a", name)
        self.wait_for("the repaint", lambda: console.shows("fake>"))
        host = self.status()["host"]
        handle = open_host(host["pid"], host["created"])
        self.assertTrue(handle, "the host runs")
        try:
            self.assertTrue(KERNEL32.TerminateProcess(handle, 1))
            self.assertEqual(KERNEL32.WaitForSingleObject(handle, LIMIT * 1000), WAIT_OBJECT_0)
        finally:
            KERNEL32.CloseHandle(handle)
        self.assertEqual(console.wait(), 1)
        self.assertTrue(console.shows("hgs: the session host died; terminal restored, sessions on this box are gone"),
                        console.shown())


if __name__ == "__main__":
    unittest.main()
