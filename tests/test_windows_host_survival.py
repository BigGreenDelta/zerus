#!/usr/bin/env python3
"""What the Windows session host outlives and what ends with it (spec D2-D4,
D9), through `hgs.exe __host` and `hgs session-host`.

Windows only, with the isolation, helpers and clean-up of `HostCase` in
tests/test_windows_host.py (HGS_TEST_BIN, HGS_TEST_FAKE_AGENT). `Breakaway`
starts the host from a helper process that first joins named kill-on-close
jobs, as a shell inside sshd's job runs hgs (D4): from a job that allows
breakaway the host and its session outlive that job; a job that forbids it
starts no host; a host that breakaway leaves in an outer job is refused by its
launcher and exits once idle. A host also outlives the client that started it.
`Ownership` ends a host and checks that every process of its sessions ends
with it (D3, D9). `StableCli` installs another build while the staged stable
CLI runs (D2). Every case closes its jobs, stops its helpers and ends any
process it pinned that still runs, besides what `HostCase` stops.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import unittest

import test_windows_host as windows_host
from test_windows_host import FAKE_AGENT, HGS, HIDDEN, IDLE_EXIT, LIMIT

# How long a case waits for a helper's report; the helper gives its launcher 60 s.
LAUNCH_LIMIT = 90
# How long the stable CLI stays attached while another build is installed.
ATTACH_MS = 20000
# The interpreter for helper processes. In a virtual environment sys.executable
# is a redirector that starts the real interpreter as its child in a job of its
# own, which would stand between a case's jobs and the launcher; the helpers
# need only the standard library (-I).
PYTHON = getattr(sys, "_base_executable", None) or sys.executable

# Run by a helper process (python -I <file> HGS REQUEST REPORT exit|hold JOB...):
# join the named jobs in order, each nested in the one before, note whether it
# is in each, run `hgs __host call --start` with the request in REQUEST and
# report the launcher's result in REPORT. A holding helper then stays until its
# jobs end it or the case closes its input. Only the case's handle may keep a
# job open, so that closing it ends the job.
HELPER = r'''
import ctypes, json, os, subprocess, sys
from ctypes import wintypes

hgs, request, report, hold, *jobs = sys.argv[1:]
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenJobObjectW.restype = wintypes.HANDLE
kernel32.OpenJobObjectW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
kernel32.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
kernel32.IsProcessInJob.restype = wintypes.BOOL
kernel32.IsProcessInJob.argtypes = (wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL))
kernel32.GetCurrentProcess.restype = wintypes.HANDLE
kernel32.GetCurrentProcess.argtypes = ()
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
JOB_OBJECT_ASSIGN_PROCESS = 0x0001
JOB_OBJECT_QUERY = 0x0004

outcome = {"pid": os.getpid(), "jobs": [], "code": None, "stdout": "", "stderr": ""}
try:
    for name in jobs:
        job = kernel32.OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_QUERY, False, name)
        if not job:
            raise OSError(f"OpenJobObjectW {name}: error {ctypes.get_last_error()}")
        try:
            if not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
                raise OSError(f"AssignProcessToJobObject {name}: error {ctypes.get_last_error()}")
            inside = wintypes.BOOL()
            if not kernel32.IsProcessInJob(kernel32.GetCurrentProcess(), job, ctypes.byref(inside)):
                raise OSError(f"IsProcessInJob {name}: error {ctypes.get_last_error()}")
            outcome["jobs"].append(bool(inside.value))
        finally:
            kernel32.CloseHandle(job)
    with open(request, encoding="utf-8") as file:
        text = file.read()
    result = subprocess.run([hgs, "__host", "call", "--start", text], stdin=subprocess.DEVNULL,
                            capture_output=True, encoding="utf-8", errors="replace", timeout=60,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    outcome.update(code=result.returncode, stdout=result.stdout, stderr=result.stderr)
except Exception as error:
    outcome["stderr"] += f"helper: {error!r}"
with open(report + ".tmp", "w", encoding="utf-8") as file:
    json.dump(outcome, file)
os.replace(report + ".tmp", report)
if hold == "hold":
    sys.stdin.read()
'''

# Typed into a cmd.exe session (python -I <file> PIDFILE): start a process
# with a hidden console of its own, outside the session's pseudoconsole, as an
# agent may start a tool, and write its process ID to PIDFILE. Only the jobs
# the host made end it.
SPAWNER = r'''
import os, subprocess, sys

child = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(600)"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=subprocess.CREATE_NO_WINDOW)
with open(sys.argv[1] + ".tmp", "w") as file:
    file.write(str(child.pid))
os.replace(sys.argv[1] + ".tmp", sys.argv[1])
'''

if os.name == "nt":
    from ctypes import wintypes

    KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)

    def bind(name, restype, *argtypes):
        function = getattr(KERNEL32, name)
        function.restype, function.argtypes = restype, argtypes

    bind("CreateJobObjectW", wintypes.HANDLE, ctypes.c_void_p, wintypes.LPCWSTR)
    # The information class is an enumeration (JOBOBJECTINFOCLASS), passed as an int.
    bind("SetInformationJobObject", wintypes.BOOL, wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    bind("QueryInformationJobObject", wintypes.BOOL, wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
         ctypes.POINTER(wintypes.DWORD))
    bind("IsProcessInJob", wintypes.BOOL, wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL))
    bind("GetCurrentProcess", wintypes.HANDLE)
    bind("OpenProcess", wintypes.HANDLE, wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    bind("GetProcessTimes", wintypes.BOOL, wintypes.HANDLE, *(ctypes.POINTER(wintypes.FILETIME),) * 4)
    bind("WaitForSingleObject", wintypes.DWORD, wintypes.HANDLE, wintypes.DWORD)
    bind("TerminateProcess", wintypes.BOOL, wintypes.HANDLE, wintypes.UINT)
    bind("CloseHandle", wintypes.BOOL, wintypes.HANDLE)

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", wintypes.LARGE_INTEGER),
                    ("PerJobUserTimeLimit", wintypes.LARGE_INTEGER),
                    ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD),
                    ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD),
                    ("SchedulingClass", wintypes.DWORD)]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
                    ("IoInfo", IO_COUNTERS),
                    ("ProcessMemoryLimit", ctypes.c_size_t),
                    ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    # JobObjectExtendedLimitInformation in JOBOBJECTINFOCLASS.
    JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
    JOB_OBJECT_LIMIT_BREAKAWAY_OK = 0x00000800
    JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK = 0x00001000
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    ERROR_ALREADY_EXISTS = 183
    SYNCHRONIZE = 0x00100000
    PROCESS_TERMINATE = 0x0001
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    WAIT_OBJECT_0 = 0
    WAIT_TIMEOUT = 0x00000102
    # Seconds from 1601-01-01 (FILETIME) to 1970-01-01 (Unix time).
    EPOCH_DIFFERENCE = 11644473600


def alive(handle):
    """Whether the process behind `handle` still runs."""
    return KERNEL32.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT


def ended(handle, seconds):
    """Whether the process behind `handle` has ended or ends within `seconds`."""
    return KERNEL32.WaitForSingleObject(handle, max(0, int(seconds * 1000))) == WAIT_OBJECT_0


def creation_ticks(handle):
    """The creation time of a process in 100 ns ticks since 1601, or None."""
    times = [wintypes.FILETIME() for _ in range(4)]
    if not KERNEL32.GetProcessTimes(handle, *(ctypes.byref(value) for value in times)):
        return None
    return (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime


def ticks_now():
    return int((time.time() + EPOCH_DIFFERENCE) * 10**7)


def placement():
    """Where this test process runs, for failure messages: a host started from
    here must leave that job (D4)."""
    inside = wintypes.BOOL()
    if not KERNEL32.IsProcessInJob(KERNEL32.GetCurrentProcess(), None, ctypes.byref(inside)):
        return f"in a job or not (IsProcessInJob: error {ctypes.get_last_error()})"
    if not inside.value:
        return "in no job"
    limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    # A null job is this process's immediate job.
    if not KERNEL32.QueryInformationJobObject(None, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, ctypes.byref(limits),
                                              ctypes.sizeof(limits), None):
        return "in a job"
    allowing = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK
    verb = "allows" if limits.BasicLimitInformation.LimitFlags & allowing else "forbids"
    return f"in a job that {verb} breakaway"


def in_use(path):
    """Whether a process runs or loads the file at `path`: Windows then refuses
    to open it for writing, as an install checks (`staging::in_use`). Opening
    changes nothing."""
    try:
        with open(path, "r+b"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return True


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


class SurvivalCase(windows_host.HostCase):
    """`HostCase` with named jobs, helper processes and pinned processes. When it
    stops it closes its jobs (which ends what is still in them), lets its helpers
    finish or ends them, stops its hosts and sessions as `HostCase` does, and then
    ends every pinned process that still runs: a case that passed left none."""

    def setUp(self):
        self.jobs = {}
        self.job_count = 0
        self.helpers = []
        self.pinned = []
        self.helper_runs = 0
        super().setUp()

    def stop_host(self):
        try:
            try:
                for handle in self.jobs.values():
                    KERNEL32.CloseHandle(handle)
                self.jobs.clear()
                self.stop_helpers()
            finally:
                # Also when a helper resisted: the hosts and sessions stop anyway.
                super().stop_host()
        finally:
            for handle in self.pinned:
                if alive(handle):
                    KERNEL32.TerminateProcess(handle, 1)
                    KERNEL32.WaitForSingleObject(handle, LIMIT * 1000)
                KERNEL32.CloseHandle(handle)
            self.pinned.clear()

    def stop_helpers(self):
        helpers, self.helpers = self.helpers, []
        for helper in helpers:
            if helper.returncode is None:
                try:
                    # Closes a holding helper's input.
                    helper.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    helper.kill()
                    try:
                        helper.communicate(timeout=LIMIT)
                    except subprocess.TimeoutExpired:
                        pass  # Ended; only a pipe outlived it.
            for stream in (helper.stdin, helper.stdout, helper.stderr):
                if stream:
                    stream.close()

    # Jobs and launches.

    def job(self, breakaway_ok):
        """A new kill-on-close job, by name. The case holds its only handle until
        `close_job` or its end."""
        self.job_count += 1
        name = f"Local\\{self.unique}-job-{self.job_count}"
        ctypes.set_last_error(0)
        handle = KERNEL32.CreateJobObjectW(None, name)
        error = ctypes.get_last_error()
        self.assertTrue(handle, f"CreateJobObjectW: error {error}")
        self.jobs[name] = handle
        self.assertNotEqual(error, ERROR_ALREADY_EXISTS, f"{name} exists already")
        limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (
            JOB_OBJECT_LIMIT_BREAKAWAY_OK if breakaway_ok else 0)
        self.assertTrue(KERNEL32.SetInformationJobObject(handle, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                                         ctypes.byref(limits), ctypes.sizeof(limits)),
                        f"SetInformationJobObject: error {ctypes.get_last_error()}")
        return name

    def close_job(self, name):
        """Close the case's handle on a job: the last one, so the job ends what is
        still in it."""
        self.assertTrue(KERNEL32.CloseHandle(self.jobs.pop(name)), f"CloseHandle: error {ctypes.get_last_error()}")

    def launch(self, request, jobs=(), hold=False):
        """Run `hgs __host call --start` with `request` from a helper process that
        first joins `jobs`; the helper and its report (`HELPER`)."""
        self.helper_runs += 1
        script = self.root / "launch-helper.py"
        if not script.exists():
            script.write_text(HELPER, encoding="utf-8")
        requested = self.root / f"request-{self.helper_runs}.json"
        report = self.root / f"launch-{self.helper_runs}.json"
        log = self.root / f"launch-{self.helper_runs}.log"
        requested.write_text(json.dumps(request), encoding="utf-8")
        with log.open("w", encoding="utf-8") as errors:
            helper = subprocess.Popen(
                [PYTHON, "-I", str(script), str(HGS), str(requested), str(report),
                 "hold" if hold else "exit", *jobs],
                env=self.env, stdin=subprocess.PIPE if hold else subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=errors, creationflags=HIDDEN)
        self.helpers.append(helper)
        deadline = time.monotonic() + LAUNCH_LIMIT
        while not report.exists():
            # A helper that exited may have written its report just now.
            if (helper.poll() is not None and not report.exists()) or time.monotonic() > deadline:
                self.fail(f"the launch helper reported nothing: {log.read_text(errors='replace')}")
            time.sleep(0.1)
        return helper, json.loads(report.read_text(encoding="utf-8"))

    def launched(self, report):
        """The session a helper's launcher created."""
        self.assertEqual(report["code"], 0, f"{report}; this test process runs {placement()}")
        return json.loads(report["stdout"])["ok"]

    # Processes.

    def pin(self, pid, what, created=None, created_after=None):
        """A handle on running process `pid`, which pins its ID; the case ends the
        process if it still runs at the end. `created` (its creation time in
        ticks, as hgs records it) or `created_after` rejects another process
        that reuses the ID."""
        handle = KERNEL32.OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                      False, pid)
        self.assertTrue(handle, f"cannot open {what}, process {pid}: error {ctypes.get_last_error()}")
        self.pinned.append(handle)
        ticks = creation_ticks(handle)
        if created is not None:
            self.assertEqual(ticks, int(created), f"process {pid} is not {what}")
        if created_after is not None:
            self.assertGreaterEqual(ticks or 0, created_after, f"process {pid} is not {what}")
        self.assertTrue(alive(handle), f"{what}, process {pid}, has ended")
        return handle

    def pane_entry(self, pane):
        entries = [entry for entry in self.ok({"op": "list"})["panes"] if entry["pane"] == pane]
        self.assertEqual(len(entries), 1, f"{pane} is not listed")
        return entries[0]

    def pin_pane(self, pane):
        """A handle on the process of the live `pane`. The host keeps its own
        handle on it, so its ID cannot be reused while the host lists the pane
        alive with that ID before and after the open."""
        pid = self.pane_entry(pane)["pid"]
        handle = self.pin(pid, f"the process of {pane}")
        entry = self.pane_entry(pane)
        self.assertEqual((entry["pid"], entry["dead"]), (pid, False), entry)
        return handle


class Breakaway(SurvivalCase):
    def test_a_host_started_inside_a_job_that_allows_breakaway_outlives_it(self):
        # Win32-OpenSSH puts a session's processes in such a job (D4).
        job = self.job(breakaway_ok=True)
        prompt = f"{self.unique}-sshd"
        helper, report = self.launch(self.create_request("cmd/sshd", self.cmd_argv(prompt)), jobs=[job], hold=True)
        self.assertEqual(report["jobs"], [True], report)
        pane = self.launched(report)["pane"]
        self.wait_for("the prompt", lambda: prompt + ">" in self.capture(pane))
        hello = self.status()["host"]
        self.assertFalse(hello["in_job"], f"{hello}; this test process runs {placement()}")
        self.assertIn(hello["pid"], self.hosts)
        host = self.hosts[hello["pid"]]
        session = self.pin_pane(pane)

        # The SSH session ends: its job ends what is still in it.
        self.close_job(job)
        try:
            helper.wait(timeout=LIMIT)
        except subprocess.TimeoutExpired:
            self.fail("closing the job did not end the helper inside it")
        self.assertFalse(ended(host, 1), "the host ended with the job it was started from")
        self.assertTrue(alive(session), "the session ended with the job its host was started from")
        status = self.status()
        self.assertEqual((status["host"]["pid"], status["host"]["in_job"], status["status"]["sessions"]),
                         (hello["pid"], False, 1))
        self.assertFalse(self.pane_entry(pane)["dead"])
        marker = f"{self.unique}-after"
        self.keys(pane, {"literal": f"echo {marker}"}, {"named": "Enter"})
        self.wait_for("the echo", lambda: marker in self.capture(pane).splitlines())

    def test_a_job_that_forbids_breakaway_starts_no_host(self):
        job = self.job(breakaway_ok=False)
        helper, report = self.launch(self.create_request("cmd/kept", self.cmd_argv("kept")), jobs=[job])
        self.assertEqual(report["jobs"], [True], report)
        self.assertEqual((report["code"], report["stdout"]), (4, ""), report)
        self.assertIn("forbids breakaway", report["stderr"])
        self.assertEqual(helper.wait(timeout=LIMIT), 0)
        # D4: nothing started that would end with the job.
        self.assertEqual(self.status(), {"running": False})
        self.assertFalse((self.state / "host.log").exists(), "a host started")

    def test_a_host_that_breakaway_leaves_in_an_outer_job_is_refused(self):
        # Breakaway leaves the inner job and stops at the outer one, which
        # forbids it; CreateProcess succeeds, so only the host can tell (D4).
        outer = self.job(breakaway_ok=False)
        inner = self.job(breakaway_ok=True)
        helper, report = self.launch(self.create_request("cmd/outer", self.cmd_argv("outer")), jobs=[outer, inner])
        self.assertEqual(report["jobs"], [True, True], report)
        self.assertEqual((report["code"], report["stdout"]), (4, ""), report)
        self.assertIn("runs inside a job that does not let it break away", report["stderr"])
        self.assertEqual(helper.wait(timeout=LIMIT), 0)
        log = (self.state / "host.log").read_text(errors="replace")
        self.assertIn("in_job true", log)
        # It created nothing and exits once idle, although the jobs stay open.
        status = self.status()
        if status["running"]:
            self.assertTrue(status["host"]["in_job"], status)
            self.assertEqual(status["status"]["sessions"], 0, status)
        self.wait_for("the refused host's exit", lambda: not self.status()["running"], timeout=IDLE_EXIT)
        for handle in self.hosts.values():
            self.assertTrue(ended(handle, LIMIT), "the refused host still runs")

    def test_a_host_outlives_the_client_that_started_it(self):
        # As when the desktop app that started a session exits.
        prompt = f"{self.unique}-gui"
        helper, report = self.launch(self.create_request("cmd/gui", self.cmd_argv(prompt)))
        pane = self.launched(report)["pane"]
        self.assertEqual(helper.wait(timeout=LIMIT), 0)
        self.wait_for("the prompt", lambda: prompt + ">" in self.capture(pane))
        status = self.status()
        self.assertEqual((status["host"]["in_job"], status["status"]["sessions"]), (False, 1), status)
        self.assertTrue(alive(self.pin_pane(pane)))


class Ownership(SurvivalCase):
    def test_the_hosts_death_ends_every_process_of_its_sessions(self):
        created, _ = self.cmd("cmd/owned")
        pane = created["pane"]
        processes = {"the cmd.exe session": self.pin_pane(pane)}

        spawner = self.root / "spawner.py"
        spawner.write_text(SPAWNER, encoding="utf-8")
        pidfile = self.root / "detached.pid"
        # Two seconds of slack between this clock and the process times.
        before = ticks_now() - 2 * 10**7
        self.keys(pane, {"literal": f'"{PYTHON}" -I "{spawner}" "{pidfile}"'}, {"named": "Enter"})
        try:
            self.wait_for("the detached process", pidfile.exists)
        except AssertionError:
            self.fail(f"the session started no detached process:\n{self.capture(pane)}")
        processes["a detached process of the session"] = self.pin(
            int(pidfile.read_text()), "the detached process", created_after=before)

        if FAKE_AGENT.is_file():
            ready = self.root / "ready.json"
            self.create("fake/owned", [FAKE_AGENT, "--ready", ready])
            self.wait_for("the agent", ready.exists)
            agent = json.loads(ready.read_text())
            processes["the fake agent session"] = self.pin(agent["pid"], "the fake agent",
                                                            created=agent["process_start"])

        hello = self.status()["host"]
        self.assertIn(hello["pid"], self.hosts)
        host = self.hosts[hello["pid"]]
        for what, handle in processes.items():
            self.assertTrue(alive(handle), f"{what} ended before the host")
        self.assertTrue(KERNEL32.TerminateProcess(host, 1), f"TerminateProcess: error {ctypes.get_last_error()}")
        self.assertTrue(ended(host, LIMIT), "the host did not end")
        # Its kill-on-close job holds every session job (D3, D9).
        deadline = time.monotonic() + LIMIT
        for what, handle in processes.items():
            self.assertTrue(ended(handle, deadline - time.monotonic()), f"{what} outlived the host")
        self.assertEqual(self.status(), {"running": False})


class StableCli(SurvivalCase):
    """D2: an install renames a running stable CLI aside and never writes it, and a
    later install removes what no process runs any more. Like `Staged`, without
    the test host variables, in the case's temporary profile."""

    def setUp(self):
        super().setUp()
        for key in ("HGS_HOST_IMAGE", "HGS_HOST_ALLOW_UNSTAGED"):
            del self.env[key]
        self.cli = self.home / ".local" / "bin" / "hgs.exe"
        self.images = self.home / ".local" / "share" / "hgs" / "host"

    def install(self, hgs):
        result = subprocess.run([str(hgs), "session-host", "install", "--json"], env=self.env,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=LIMIT * 2,
                                creationflags=HIDDEN)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def asides(self):
        return sorted(path.name for path in self.cli.parent.glob("hgs.exe.old-*"))

    def test_an_install_moves_a_running_stable_cli_aside(self):
        first = self.install(HGS)
        old = sha256(HGS)
        self.assertEqual((first["cli"]["sha256"], sha256(self.cli)), (old, old))
        old_image = first["image"]["name"]

        # A host from the staged image, and the staged CLI as its attach
        # client for ATTACH_MS; the client then types and detaches.
        created, _ = self.cmd("cmd/aside")
        pane = created["pane"]
        marker = f"{self.unique}-typed-after-the-update"
        request = {"target": {"pane": pane}, "size": {"cols": 100, "rows": 30}, "run_id": None}
        steps = [{"wait_ms": ATTACH_MS}, {"send": f"echo {marker}\r"}, {"wait_ms": 500}, {"detach": True}]
        client = subprocess.Popen([str(self.cli), "__host", "attach", json.dumps(request), json.dumps(steps)],
                                  env=self.env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, creationflags=HIDDEN)
        self.helpers.append(client)

        def listed():
            if client.poll() is not None:
                self.fail(f"the stable CLI's attach ended early: {client.communicate()}")
            return client.pid in [entry["pid"] for entry in self.ok({"op": "list_clients", "target": None})["clients"]]
        self.wait_for("the stable CLI's attach", listed)

        # Another build: the same executable with bytes appended, which the
        # loader ignores. Its SHA-256, and so its image, differ.
        update = self.root / "update" / "hgs.exe"
        update.parent.mkdir()
        shutil.copyfile(HGS, update)
        with update.open("ab") as file:
            file.write(f"\0{self.unique}\0".encode())
        new = sha256(update)
        installed = self.install(update)
        running = client.poll() is None
        asides = self.asides()
        self.assertTrue(running, "the stable CLI exited before the install had finished; raise ATTACH_MS")

        cli = installed["cli"]
        self.assertTrue(cli["changed"], cli)
        self.assertEqual((cli["sha256"], sha256(self.cli)), (new, new))
        self.assertEqual(len(asides), 1, f"aside files while the old CLI runs: {asides}; install: {cli}")
        self.assertRegex(asides[0], r"^hgs\.exe\.old-[0-9]+-[0-9a-f]{8}$")
        self.assertEqual(([Path(path).name for path in cli["kept_aside"]], cli["removed_aside"]), (asides, 0), cli)
        # The running file moved and was never written.
        self.assertEqual(sha256(self.cli.parent / asides[0]), old)
        # The host's image stays while the host runs from it; the pointer names the new one.
        image = installed["image"]
        self.assertEqual((image["name"], image["kept"]), (new[:16], [old_image]), image)
        self.assertEqual((self.images / "current").read_text().strip(), new[:16])
        self.assertEqual(self.status()["host"]["image_hash"], old)

        # The old CLI runs on, types into the session and exits normally.
        output, errors = client.communicate(timeout=ATTACH_MS / 1000 + LIMIT)
        self.assertEqual(client.returncode, 0, errors)
        attached = json.loads(output)
        self.assertTrue(attached["sent"], attached)
        self.assertEqual(attached["attached"]["pane"], pane)
        self.wait_for("the old CLI's input", lambda: marker in self.capture(pane).splitlines())

        # Once nothing runs the old files, the next install removes them.
        self.ok({"op": "kill", "target": {"pane": pane}})
        self.wait_for("the end", lambda: not self.ok({"op": "list"})["sessions"])
        self.call({"op": "stop_host"})
        self.wait_for("the host's exit", lambda: not self.status()["running"])
        self.wait_for_hosts()
        old_files = [self.cli.parent / asides[0], *(self.images / old_image).iterdir()]
        self.wait_for("the old files to be unused", lambda: not any(map(in_use, old_files)))
        # A scanner may still hold one of them for a moment: install again while
        # something stays, as the staging tests do (`until_released`).
        removed_asides, removed_images = 0, []
        deadline = time.monotonic() + 10
        while True:
            later = self.install(update)
            self.assertFalse(later["cli"]["changed"], later["cli"])
            removed_asides += later["cli"]["removed_aside"]
            removed_images += later["image"]["removed"]
            if not (later["cli"]["kept_aside"] or later["image"]["kept"]) or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual((removed_asides, later["cli"]["kept_aside"]), (1, []), later["cli"])
        self.assertEqual(self.asides(), [])
        self.assertEqual((removed_images, later["image"]["kept"]), ([old_image], []), later["image"])
        self.assertFalse((self.images / old_image).exists())

        # The next host starts from the new image.
        self.cmd("cmd/updated")
        self.assertEqual(self.status()["host"]["image_hash"], new)


if __name__ == "__main__":
    unittest.main()
