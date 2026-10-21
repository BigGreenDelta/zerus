#!/usr/bin/env python3
"""One opt-in Windows source-bound RED for desktop request registration.

The ordinary Qt suites must first pass all six new fixed regressions. This gate
then restores only three inline starts in a private git-archive copy and builds
only test_hgsclient. It never changes the candidate, normal build or package
inputs. The exact-source pins deliberately belong to the opt-in qualification,
not to ordinary main/PR testing. No command, executable or address mode is exposed.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
CLIENT = "tray/src/HgsClient.cpp"
TEST = "tray/tests/test_hgsclient.cpp"
PINS = {
    CLIENT: "a75d609a45e6d865235e556cbf79e0c10231601997b9d2081f1d1383b5829477",
    TEST: "a5ff0f5fffb17e498ed3995b81f3154c0de0ee609dbd1e23d07384ea7d38be8e",
    "tray/tests/test_sessionswindow.cpp": "03df316f82c02c9552710a670c4cb3232875149117e2fee4cb054551cbc895bd",
}
BASELINE_CLIENT = "d9fb7f0695048bfc98d90c21a3f00a0448404a9a4fba648f1bdb65d3b4ffc060"
METHOD = "writeStartupFailuresAreReportedAfterRequest"
CONDITIONS = {row: "process->error() == QProcess::UnknownError" for row in ("message", "queue", "settings")}
FIXED = {
    ("TestHgsClient", f"{METHOD}({row})") for row in CONDITIONS
} | {
    ("TestSessionsWindow", "messageStartupFailureRestoresThePendingDraft"),
    ("TestSessionsWindow", "queueStartupFailureReleasesThePendingAction"),
    ("TestSessionsWindow", "settingsStartupFailureReleasesThePendingChange"),
}
WHOLE = 480
CLEANUP = 60
LOG_CAP = 8 * 1024 * 1024
MAX_INPUT = 512 * 1024 * 1024
EXPORT_IGNORED = (".beads", ".agents", ".github", "design", "AGENTS.md", "CLAUDE.md")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def before(end, clock=time.monotonic):
    remaining = end - clock()
    if remaining <= 0:
        raise RuntimeError("Original qualification deadline expired")
    return remaining


def read_bytes(path, end, limit=MAX_INPUT):
    before(end)
    size = path.stat().st_size
    if size > limit:
        raise RuntimeError("Input exceeds the qualification bound")
    with path.open("rb") as source:
        chunks = []
        total = 0
        while True:
            before(end)
            chunk = source.read(min(1024 * 1024, limit - total + 1))
            before(end)
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise RuntimeError("Input grew beyond the qualification bound")
            chunks.append(chunk)
    before(end)
    return b"".join(chunks)


def check_fixed(documents):
    """Require each actual fixed row once; a job exit code is insufficient."""
    found = []
    total = cases = 0
    for document in documents:
        total += len(document)
        if total > 64 * 1024 * 1024:
            raise RuntimeError("Fixed XML input exceeds its total bound")
        root = ET.fromstring(document)
        for case in root.iter("testcase"):
            cases += 1
            if cases > 100000:
                raise RuntimeError("Fixed XML case inventory exceeds bound")
            identity = (case.get("classname"), case.get("name"))
            if identity in FIXED:
                if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
                    raise RuntimeError("A required fixed Windows row did not pass")
                found.append(identity)
    if len(found) != len(FIXED) or set(found) != FIXED:
        raise RuntimeError("Missing or duplicate fixed Windows regression row")
    return sorted(found)


def reverse_start_admission(data):
    if sha(data) != PINS[CLIENT]:
        raise RuntimeError("This opt-in RED requires the reviewed fixed product postimage")
    current = (b"    // Return the request ID before native startup can reenter the event loop.\n"
               b"    QTimer::singleShot(0, process, [process, timer, reported, timeout] {\n"
               b"        if (*reported) { process->deleteLater(); return; }\n"
               b"        if (timer->remainingTime() <= 0) { timeout(); process->deleteLater(); return; }\n"
               b"        process->start();\n"
               b"    });")
    if data.count(current) != 3:
        raise RuntimeError("The exact three-start admission transform is not unique")
    data = data.replace(current, b"    process->start();")
    if sha(data) != BASELINE_CLIENT:
        raise RuntimeError("The inline-start product file differs from the complete reviewed baseline")
    return data


def assertion_line(test_source):
    if sha(test_source) != PINS[TEST]:
        raise RuntimeError("The reviewed sharing-denial regression source changed")
    marker = b"    QVERIFY(process->error() == QProcess::UnknownError);"
    lines = test_source.splitlines()
    if lines.count(marker) != 1:
        raise RuntimeError("The pre-admission assertion is not unique")
    return lines.index(marker) + 1


def check_red(document, raw_log, row, code, expected_source, line):
    """Only pre-admission failure after real QProcess prerequisites is a RED."""
    root = ET.fromstring(document)
    cases = list(root.iter("testcase"))
    wanted = ["initTestCase", f"{METHOD}({row})", "cleanupTestCase"]
    if code != 1 or [case.get("name") for case in cases] != wanted:
        raise RuntimeError("Old row exit or inventory is not the causal assertion RED")
    if any(root.get(key) != value for key, value in
           {"tests": "3", "failures": "1", "errors": "0", "skipped": "0"}.items()):
        raise RuntimeError("Setup/error/skip/unknown accounting cannot qualify as RED")
    if any(case.get("classname") != "TestHgsClient" for case in cases):
        raise RuntimeError("Old row class differs")
    failures = [(case, item) for case in cases for item in case.findall("failure")]
    if len(failures) != 1 or failures[0][0] is not cases[1]:
        raise RuntimeError("Only the named body assertion may fail")
    if any(case.find(tag) is not None for case in cases for tag in ("error", "skipped")):
        raise RuntimeError("An error or skip is not a behavioral RED")
    message = f"'{CONDITIONS[row]}' returned FALSE. ()"
    if failures[0][1].get("type") != "fail" or failures[0][1].get("message") != message:
        raise RuntimeError("Wrong assertion: the real startup prerequisites must pass first")
    text = raw_log.decode("utf-8", "replace")
    expected = f"TestHgsClient::{METHOD}({row}) {message}"
    if text.count("FAIL!  : ") != 1 or expected not in text:
        raise RuntimeError("Raw pre-admission assertion provenance is missing")
    locations = re.findall(r"Loc: \[(.+test_hgsclient\.cpp)\((\d+)\)\]", text)
    normalized = [(os.path.normcase(os.path.normpath(path)), int(number)) for path, number in locations]
    if normalized != [(os.path.normcase(os.path.normpath(str(expected_source))), line)]:
        raise RuntimeError("Raw failure is not at the bound pre-admission assertion")
    return {"row": row, "condition": CONDITIONS[row], "source_line": line,
            "actual_exit": code, "failures": 1, "errors": 0, "skips": 0}


def regular(path):
    attributes = path.lstat()
    if not path.is_file() or path.is_symlink() or getattr(attributes, "st_file_attributes", 0) & 0x400:
        raise RuntimeError("A source/build input is not a regular non-reparse file")


def file_map(paths, end):
    result = {}
    total = 0
    for name, path in paths:
        regular(path)
        content = read_bytes(path, end)
        total += len(content)
        if len(result) >= 50000 or total > 4 * 1024**3 or name in result:
            raise RuntimeError("Protected input map is duplicate or exceeds its bound")
        result[name] = {"bytes": len(content), "sha256": sha(content)}
    return result


def extract_archive(archive, destination, expected, end):
    """The current source has only regular Git blobs; refuse any other member."""
    seen = set()
    with tarfile.open(archive, "r:") as source:
        for count, member in enumerate(source):
            before(end)
            path = PurePosixPath(member.name)
            if count >= 10000 or path.is_absolute() or any(part in ("", ".", "..") for part in path.parts) \
                    or "\\" in member.name or ":" in member.name:
                raise RuntimeError("Unsafe or excessive private archive member")
            if member.isdir():
                if not any(name.startswith(path.as_posix() + "/") for name in expected):
                    raise RuntimeError("Unexpected private archive directory")
                continue
            name = path.as_posix()
            if not member.isfile() or name not in expected or name in seen or member.size != expected[name]["bytes"]:
                raise RuntimeError("Private archive member differs from exact source")
            seen.add(name)
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as original, target.open("xb") as output:
                content = original.read(member.size + 1)
                if len(content) != member.size or sha(content) != expected[name]["sha256"]:
                    raise RuntimeError("Private archive bytes differ from exact source")
                output.write(content)
            before(end)
    if seen != set(expected):
        raise RuntimeError("Private archive has missing source files")


class WindowsJob:
    """Fixed gate's synchronous commands, held by kernel handles in one own job.

    The job limit and API signatures follow test_windows_host_survival.py; the
    STARTUPINFOEX handle list follows test_windows_lifecycle.py. No breakaway,
    process discovery, PID reopen or detached-service cleanup is supported.
    """
    def __init__(self):
        if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise RuntimeError("This causal gate requires Windows x64")
        from ctypes import wintypes as w
        self.w = w
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        def bind(name, result, *arguments):
            function = getattr(self.api, name)
            function.restype, function.argtypes = result, arguments
        bind("CreateJobObjectW", w.HANDLE, ctypes.c_void_p, w.LPCWSTR)
        bind("SetInformationJobObject", w.BOOL, w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD)
        bind("QueryInformationJobObject", w.BOOL, w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p)
        bind("AssignProcessToJobObject", w.BOOL, w.HANDLE, w.HANDLE)
        bind("TerminateJobObject", w.BOOL, w.HANDLE, w.UINT)
        bind("TerminateProcess", w.BOOL, w.HANDLE, w.UINT)
        bind("WaitForSingleObject", w.DWORD, w.HANDLE, w.DWORD)
        bind("GetExitCodeProcess", w.BOOL, w.HANDLE, ctypes.POINTER(w.DWORD))
        bind("ResumeThread", w.DWORD, w.HANDLE)
        bind("CloseHandle", w.BOOL, w.HANDLE)
        bind("InitializeProcThreadAttributeList", w.BOOL, ctypes.c_void_p, w.DWORD, w.DWORD, ctypes.POINTER(ctypes.c_size_t))
        bind("UpdateProcThreadAttribute", w.BOOL, ctypes.c_void_p, w.DWORD, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p)
        bind("DeleteProcThreadAttributeList", None, ctypes.c_void_p)
        class Startup(ctypes.Structure):
            _fields_ = [("cb", w.DWORD), ("lpReserved", w.LPWSTR), ("lpDesktop", w.LPWSTR), ("lpTitle", w.LPWSTR),
                        ("dwX", w.DWORD), ("dwY", w.DWORD), ("dwXSize", w.DWORD), ("dwYSize", w.DWORD),
                        ("dwXCountChars", w.DWORD), ("dwYCountChars", w.DWORD), ("dwFillAttribute", w.DWORD),
                        ("dwFlags", w.DWORD), ("wShowWindow", w.WORD), ("cbReserved2", w.WORD),
                        ("lpReserved2", ctypes.c_void_p), ("hStdInput", w.HANDLE), ("hStdOutput", w.HANDLE), ("hStdError", w.HANDLE)]
        class StartupEx(ctypes.Structure):
            _fields_ = [("StartupInfo", Startup), ("lpAttributeList", ctypes.c_void_p)]
        class Process(ctypes.Structure):
            _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE), ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]
        self.StartupEx, self.Process = StartupEx, Process
        bind("CreateProcessW", w.BOOL, w.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
             w.BOOL, w.DWORD, ctypes.c_void_p, w.LPCWSTR, ctypes.c_void_p, ctypes.POINTER(Process))
        self.job = self.api.CreateJobObjectW(None, None)
        self.pending = None
        self.problems = []
        self.termination_requested = False
        if not self.job:
            raise ctypes.WinError(ctypes.get_last_error())
        # Exact x64 JOBOBJECT_EXTENDED_LIMIT_INFORMATION layout used by outside-job:
        # only KILL_ON_JOB_CLOSE; neither BREAKAWAY_OK flag is admitted.
        info = (ctypes.c_ubyte * 144)()
        ctypes.c_uint32.from_buffer(info, 16).value = 0x2000
        if not self.api.SetInformationJobObject(self.job, 9, info, len(info)):
            self.api.CloseHandle(self.job)
            self.job = None
            raise ctypes.WinError(ctypes.get_last_error())

    def suspend(self, argv, cwd, environment, output):
        import msvcrt
        needed = ctypes.c_size_t()
        self.api.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(needed))
        storage = ctypes.create_string_buffer(needed.value)
        if not self.api.InitializeProcThreadAttributeList(storage, 1, 0, ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        with open(os.devnull, "rb") as stdin:
            handles = (msvcrt.get_osfhandle(stdin.fileno()), msvcrt.get_osfhandle(output.fileno()))
            inherited = [os.get_handle_inheritable(handle) for handle in handles]
            try:
                for handle in handles:
                    os.set_handle_inheritable(handle, True)
                allowed = (self.w.HANDLE * len(handles))(*handles)
                if not self.api.UpdateProcThreadAttribute(storage, 0, 0x20002, allowed, ctypes.sizeof(allowed), None, None):
                    raise ctypes.WinError(ctypes.get_last_error())
                startup = self.StartupEx()
                startup.StartupInfo.cb = ctypes.sizeof(startup)
                startup.StartupInfo.dwFlags = 0x100  # STARTF_USESTDHANDLES
                startup.StartupInfo.hStdInput, startup.StartupInfo.hStdOutput = handles
                startup.StartupInfo.hStdError = handles[1]
                startup.lpAttributeList = ctypes.cast(storage, ctypes.c_void_p)
                command = ctypes.create_unicode_buffer(subprocess.list2cmdline([str(word) for word in argv]))
                block = ctypes.create_unicode_buffer("".join(f"{key}={value}\0" for key, value in sorted(environment.items())) + "\0")
                process = self.Process()
                # SUSPENDED | UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT.
                if not self.api.CreateProcessW(str(argv[0]), command, None, None, True, 0x80404,
                                               block, str(cwd), ctypes.byref(startup), ctypes.byref(process)):
                    raise ctypes.WinError(ctypes.get_last_error())
                self.pending = process  # retained before any fallible handle restoration
            finally:
                self.api.DeleteProcThreadAttributeList(storage)
                for handle, previous in zip(handles, inherited):
                    try:
                        os.set_handle_inheritable(handle, previous)
                    except OSError as error:
                        self.problems.append("Standard handle restoration: " + type(error).__name__)
        return self.pending

    def assign(self, process):
        if not self.api.AssignProcessToJobObject(self.job, process.hProcess):
            raise ctypes.WinError(ctypes.get_last_error())

    def resume(self, process):
        if self.api.ResumeThread(process.hThread) != 1:
            raise RuntimeError("Owned thread was not suspended exactly once")

    def wait(self, process, milliseconds):
        state = self.api.WaitForSingleObject(process.hProcess, milliseconds)
        if state not in (0, 0x102):
            raise RuntimeError("Owned process wait is unknown")
        return state == 0

    def code(self, process):
        value = self.w.DWORD()
        if not self.api.GetExitCodeProcess(process.hProcess, ctypes.byref(value)):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.value

    def empty(self):
        # JOBOBJECT_BASIC_ACCOUNTING_INFORMATION, ActiveProcesses at offset40.
        info = (ctypes.c_ubyte * 48)()
        if not self.api.QueryInformationJobObject(self.job, 1, info, len(info), None):
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.c_uint32.from_buffer(info, 40).value == 0

    def terminate(self, process, assigned):
        if assigned:
            if self.termination_requested:
                raise RuntimeError("Own job termination was already attempted")
            self.termination_requested = True
        function = self.api.TerminateJobObject if assigned else self.api.TerminateProcess
        handle = self.job if assigned else process.hProcess
        if not function(handle, 1):
            raise ctypes.WinError(ctypes.get_last_error())

    def release(self, process):
        for handle in (process.hThread, process.hProcess):
            if not self.api.CloseHandle(handle):
                self.problems.append("Owned process handle close is unknown")
        self.pending = None

    def close(self):
        if self.job and not self.api.CloseHandle(self.job):
            self.problems.append("Owned job handle close is unknown")
        self.job = None


def wait_empty(kernel, end, clock=time.monotonic, nap=time.sleep):
    while not kernel.empty():
        nap(min(0.01, before(end, clock)))
    before(end, clock)


def run_owned(kernel, argv, cwd, environment, log, end, cap, report, clock=time.monotonic, persist=lambda: None, nap=time.sleep):
    """No child executes before private-job assignment; no numeric cleanup."""
    started = clock()
    phase_end = min(end - CLEANUP, started + cap)
    before(phase_end, clock)
    process = None
    assigned = False
    ended = False
    entry = {"argv": [str(word) for word in argv], "cwd": str(cwd), "started": started, "end": phase_end}
    report.append(entry)
    persist()  # save and fsync command intent before even a suspended child exists
    try:
        with log.open("xb", buffering=0) as output:
            process = kernel.suspend(argv, cwd, environment, output)
            entry.update(pid=process.dwProcessId, created_suspended=True)  # diagnostic only
            persist()
            if kernel.problems:
                raise RuntimeError("Owned startup cleanup is unknown")
            before(phase_end, clock)
            kernel.assign(process)
            assigned = True
            before(phase_end, clock)
            kernel.resume(process)
            while not kernel.wait(process, min(50, max(0, int(before(phase_end, clock) * 1000)))):
                if log.stat().st_size > LOG_CAP:
                    raise RuntimeError("Owned command log cap exceeded")
            before(phase_end, clock)
            ended = True
            entry["exit"] = kernel.code(process)
            if log.stat().st_size > LOG_CAP:
                raise RuntimeError("Owned output bound exceeded")
            wait_empty(kernel, phase_end, clock, nap)
            return entry["exit"]
    finally:
        process = process or kernel.pending
        if process is not None:
            try:
                if not ended or not kernel.empty():
                    kernel.terminate(process, assigned)
                if not kernel.wait(process, min(5000, max(0, int(before(end, clock) * 1000)))):
                    raise RuntimeError("Owned kernel handle/job did not reach exited-and-empty")
                wait_empty(kernel, end, clock, nap)
                before(end, clock)
                entry["cleanup"] = "exited-and-empty"
            finally:
                primary_error = sys.exc_info()[0]
                kernel.release(process)
                if kernel.problems:
                    entry["cleanup_unknown"] = list(kernel.problems)
                    if primary_error is None:
                        raise RuntimeError("Owned command handle cleanup is unknown")
                entry["finished"] = clock()
                persist()
                if primary_error is None:
                    before(phase_end, clock)


def private_environment(root, original):
    names = {"PATH", "SYSTEMROOT", "WINDIR", "SYSTEMDRIVE", "COMSPEC", "PATHEXT", "PROGRAMFILES", "PROGRAMFILES(X86)",
             "PROGRAMW6432", "INCLUDE", "LIB", "LIBPATH", "VCINSTALLDIR", "VSINSTALLDIR", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION",
             "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "WINDOWSLIBPATH", "UNIVERSALCRTSDKDIR", "UCRTVERSION", "VSLANG",
             "VSCMD_ARG_HOST_ARCH", "VSCMD_ARG_TGT_ARCH", "VSCMD_VER", "QT_ROOT_DIR"}
    environment = {key.upper(): value for key, value in original.items() if key.upper() in names}
    for name in ("home", "tmp", "config", "data", "cache", "state", "appdata", "localappdata", "hgs-config"):
        (root / name).mkdir()
    environment.update(HOME=str(root / "home"), USERPROFILE=str(root / "home"), TEMP=str(root / "tmp"), TMP=str(root / "tmp"),
                       APPDATA=str(root / "appdata"), LOCALAPPDATA=str(root / "localappdata"), XDG_CONFIG_HOME=str(root / "config"),
                       XDG_DATA_HOME=str(root / "data"), XDG_CACHE_HOME=str(root / "cache"), XDG_STATE_HOME=str(root / "state"),
                       HGS_CONFIG_DIR=str(root / "hgs-config"), HGS_STATE_DIR=str(root / "state"), PYTHONDONTWRITEBYTECODE="1",
                       QT_QPA_PLATFORM="offscreen", QT_FORCE_STDERR_LOGGING="1", QT_QUICK_BACKEND="software", CCACHE_DISABLE="1",
                       CMAKE_BUILD_PARALLEL_LEVEL="2", CTEST_PARALLEL_LEVEL="1", ZERUS_BUILD_JOBS="2", ZERUS_TEST_JOBS="1",
                       GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_OPTIONAL_LOCKS="0")
    qt = Path(environment["QT_ROOT_DIR"])
    environment["PATH"] = str(qt / "bin") + os.pathsep + environment["PATH"]
    environment["QT_QPA_FONTDIR"] = str(Path(environment["SYSTEMROOT"]) / "Fonts")
    return environment


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", required=True)
    parser.add_argument("--end", required=True, type=float, help="Original monotonic end, set before MSVC environment setup")
    args = parser.parse_args(argv)
    if os.name != "nt" or not re.fullmatch(r"[0-9a-f]{40}", args.source) or not 0 < args.end - time.monotonic() <= WHOLE:
        raise RuntimeError("Windows/source/original deadline admission refused")
    evidence = ROOT / "artifacts/test-results/windows-write-startup-causal"
    evidence.mkdir(exist_ok=False)
    result = {"status": "STOP", "source": args.source, "original_end": args.end, "commands": [], "rows": [],
              "variant": "three-inline-starts-only", "malformed_image_timeout_cause": "UNKNOWN",
              "historical_pvfa14_pvfa15_causes": "UNKNOWN", "resources_retained": True}
    kernel = None
    protected = None
    tracked = None
    private = None
    source = None
    old_expected = None
    tools_before = None
    tool_paths = []
    def save():
        data = (json.dumps(result, indent=2) + "\n").encode()
        temporary = evidence / "result.pending"
        with temporary.open("wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, evidence / "result.json")
        if (evidence / "result.json").read_bytes() != data:
            raise RuntimeError("Command intent readback differs")
    try:
        before(args.end - CLEANUP)
        documents = sorted((ROOT / "artifacts/test-results/qt-functions").glob("*.xml"))
        if len(documents) > 1024:
            raise RuntimeError("Fixed Qt XML inventory exceeds bound")
        result["fixed_six"] = check_fixed(read_bytes(path, args.end - CLEANUP, 8 * 1024 * 1024) for path in documents)
        # Nothing executable and no RED-variant allocation precedes fixed-six qualification.
        for name, expected in PINS.items():
            if sha(read_bytes(ROOT / name, args.end - CLEANUP)) != expected:
                raise RuntimeError("This opt-in gate is not bound to the reviewed source postimages")
        line = assertion_line(read_bytes(ROOT / TEST, args.end - CLEANUP))
        private = Path(tempfile.mkdtemp(prefix="dw-", dir=os.environ["RUNNER_TEMP"]))
        environment = private_environment(private, os.environ)
        if environment.get("VSCMD_ARG_TGT_ARCH") != "x64":
            raise RuntimeError("The existing reviewed MSVC x64 environment is required")
        kernel = WindowsJob()
        tools = {name: shutil.which(name, path=environment["PATH"]) for name in ("git", "cmake", "ninja", "cl", "link")}
        if not all(tools.values()):
            raise RuntimeError("An existing build tool is missing; no download is permitted")
        qt = Path(environment["QT_ROOT_DIR"])
        tool_paths = [(name, Path(path)) for name, path in tools.items()] + [("python", Path(sys.executable))]
        tool_paths += [("qt/" + name, qt / name) for name in
                       ("bin/Qt6Core.dll", "bin/Qt6Test.dll", "bin/Qt6Network.dll", "plugins/platforms/qoffscreen.dll")]
        tools_before = file_map(tool_paths, args.end - CLEANUP)
        result["tools"] = tools_before
        result["environment"] = environment
        result["private_root"] = str(private)
        def run(label, words, cap=20, cwd=ROOT):
            log = evidence / (label + ".log")
            return run_owned(kernel, words, cwd, environment, log, args.end, cap, result["commands"], persist=save)
        def checked(label, words, cap=20, cwd=ROOT):
            if run(label, words, cap, cwd) != 0:
                raise RuntimeError(label + " failed; no retry")
            return read_bytes(evidence / (label + ".log"), args.end - CLEANUP, LOG_CAP)
        if checked("head", [tools["git"], "rev-parse", "HEAD"]).decode().strip() != args.source:
            raise RuntimeError("The exact tested source commit differs")
        if checked("status", [tools["git"], "status", "--porcelain"]):
            raise RuntimeError("The tested source is dirty")
        entries = checked("tracked", [tools["git"], "ls-tree", "-rz", args.source])
        names = []
        for item in entries.split(b"\0"):
            if item:
                meta, name = item.split(b"\t", 1)
                if meta.split()[0] not in (b"100644", b"100755"):
                    raise RuntimeError("Only regular tracked source is admitted")
                names.append(name.decode())
        tracked = file_map([(name, ROOT / name) for name in names], args.end - CLEANUP)
        ignored = {name for name in names if any(name == prefix or name.startswith(prefix + "/") for prefix in EXPORT_IGNORED)}
        attributes = read_bytes(ROOT / ".gitattributes", args.end - CLEANUP).decode()
        if set(re.findall(r"(?m)^(\S+) export-ignore$", attributes)) != set(EXPORT_IGNORED):
            raise RuntimeError("The reviewed git-archive export exclusions changed")
        exported = {name: value for name, value in tracked.items() if name not in ignored}
        result["archive_export_ignored"] = sorted(ignored)
        protected_paths = [(str(path.relative_to(ROOT)), path) for parent in (ROOT / ".ci-build/gui", ROOT / "artifacts/test-results/qt-functions")
                           for path in sorted(parent.rglob("*")) if path.is_file()]
        protected_paths += [(name, ROOT / name) for name in ("target/release/hgs.exe", "target/release/zerus-mobile-connector.exe", "artifacts/test-results/qt.xml")]
        protected = file_map(protected_paths, args.end - CLEANUP)
        (evidence / "candidate-source-before.json").write_text(json.dumps(tracked, indent=2) + "\n")
        (evidence / "candidate-build-before.json").write_text(json.dumps(protected, indent=2) + "\n")
        archive = private / "source.tar"
        checked("archive", [tools["git"], "archive", "--format=tar", "--output", str(archive), args.source])
        if archive.stat().st_size > MAX_INPUT:
            raise RuntimeError("Archive exceeds bound")
        source = private / "source"
        source.mkdir()
        extract_archive(archive, source, exported, args.end - CLEANUP)
        (source / CLIENT).write_bytes(reverse_start_admission(read_bytes(source / CLIENT, args.end - CLEANUP)))
        old_expected = dict(exported)
        old_expected[CLIENT] = {"bytes": (source / CLIENT).stat().st_size, "sha256": BASELINE_CLIENT}
        if file_map([(name, source / name) for name in exported], args.end - CLEANUP) != old_expected:
            raise RuntimeError("Inline-start variant differs outside the three startup admission blocks")
        (evidence / "old-source.json").write_text(json.dumps(old_expected, indent=2) + "\n")
        build = private / "build"
        checked("configure", [tools["cmake"], "-S", str(source / "tray"), "-B", str(build), "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl",
                "-DCMAKE_AUTOGEN_PARALLEL=1", "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER=",
                "-DCMAKE_PREFIX_PATH=" + environment["QT_ROOT_DIR"], "-DPython3_EXECUTABLE=" + sys.executable,
                "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON"], 60)
        checked("build", [tools["cmake"], "--build", str(build), "--target", "test_hgsclient", "--parallel", "2"], 300)
        executable = build / "tests/test_hgsclient.exe"
        result["old_binary"] = file_map([("test_hgsclient", executable)], args.end - CLEANUP)
        for row in CONDITIONS:
            xml = evidence / ("old-" + row + ".xml")
            code = run("old-" + row, [str(executable), METHOD + ":" + row, "-o", str(xml) + ",junitxml", "-o", "-,txt"], 20)
            result["rows"].append(check_red(read_bytes(xml, args.end - CLEANUP, 4 * 1024 * 1024),
                read_bytes(evidence / ("old-" + row + ".log"), args.end - CLEANUP, LOG_CAP), row, code, source / TEST, line))
        if checked("head-after", [tools["git"], "rev-parse", "HEAD"]).decode().strip() != args.source:
            raise RuntimeError("The candidate commit changed during the old-source gate")
        if checked("status-after", [tools["git"], "status", "--porcelain"]):
            raise RuntimeError("The candidate became dirty during the old-source gate")
        result["status"] = "PASS"
    except BaseException as error:
        result["reason"] = type(error).__name__ + ": " + str(error)
    finally:
        if kernel is not None:
            try:
                if not kernel.empty():
                    if not kernel.termination_requested:
                        kernel.terminate(None, True)
                    wait_empty(kernel, args.end)
                result["job_empty"] = True
            except BaseException as error:
                result["status"] = "STOP"
                result["cleanup_unknown"] = type(error).__name__
            finally:
                try:
                    kernel.close()
                except BaseException as error:
                    result["status"] = "STOP"
                    result["job_handle_close_unknown"] = type(error).__name__
                if kernel.problems:
                    result["status"] = "STOP"
                    result["handle_cleanup_unknown"] = kernel.problems
        for label, original, paths in [
            ("source", tracked, [(name, ROOT / name) for name in tracked or ()]),
            ("candidate_build", protected, [(name, ROOT / name) for name in protected or ()]),
            ("tools", tools_before, tool_paths),
            ("variant_source", old_expected, [(name, source / name) for name in old_expected or ()]),
        ]:
            if original is not None:
                try:
                    after = file_map(paths, args.end)
                    result[label + "_unchanged"] = after == original
                    if after != original:
                        result["status"] = "STOP"
                    (evidence / (label + "-after.json")).write_text(json.dumps(after, indent=2) + "\n")
                except BaseException as error:
                    result["status"] = "STOP"
                    result[label + "_seal_unknown"] = type(error).__name__
        result["within_original_end"] = time.monotonic() < args.end
        if not result["within_original_end"]:
            result["status"] = "STOP"
        try:
            before(args.end)
            save()
            files = sorted(path for path in evidence.rglob("*") if path.is_file())
            lines = [sha(read_bytes(path, args.end)) + "  " + path.relative_to(evidence).as_posix() + "\n" for path in files]
            with (evidence / "SHA256SUMS").open("x", encoding="utf-8") as output:
                output.write("".join(lines))
                output.flush()
                os.fsync(output.fileno())
            before(args.end)
        except BaseException as error:
            result["status"] = "STOP"
            result["final_seal_unknown"] = type(error).__name__
            save()  # failure diagnostic only; never authorizes later work
        print("Windows startup causal gate: " + result["status"])
    return 0 if result["status"] == "PASS" and time.monotonic() < args.end else 1


if __name__ == "__main__":
    sys.exit(main())
