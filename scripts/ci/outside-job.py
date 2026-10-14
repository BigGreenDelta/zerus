#!/usr/bin/env python3
"""Run a test command outside the CI step's job object (Windows CI only).

GitHub's Windows runner starts every step inside a job that forbids breakaway, so
a session host started from a step lands in that job and hgs refuses it (spec D4,
docs/windows-host-testing.md). This is a CI harness for the host suites only: the
product keeps D4 and never escapes a job through WMI.

WMI's Win32_Process.Create starts this script again outside any job. That copy
restores the step's environment explicitly (WMI processes do not inherit it;
credentials are left out), joins a kill-on-close job of its own that allows
breakaway, as an sshd session does, runs the command there with stdin from NUL,
and writes its output and exit status to files that this side follows. When the
command ends, or this side times out and ends the copy, the job ends everything
the command left, and session hosts that legitimately broke away from it are
ended by executable path (--end, a PowerShell -like pattern). Usage:

    python scripts/ci/outside-job.py --log FILE [--timeout SECONDS] [--end EXE]... -- COMMAND...

It exits with the command's status, or 124 after the timeout.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

# Step variables that grant access; the command never needs them.
SECRET = re.compile(r"TOKEN|SECRET|PASSWORD|^ACTIONS_(RUNTIME|CACHE|RESULTS|ID_TOKEN)", re.IGNORECASE)


def in_job():
    result = ctypes.c_int()
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.IsProcessInJob.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int))
    if not kernel32.IsProcessInJob(kernel32.GetCurrentProcess(), None, ctypes.byref(result)):
        raise OSError(ctypes.get_last_error(), "IsProcessInJob failed")
    return bool(result.value)


def exited(pid):
    """Whether process `pid` has ended (or cannot be opened, so it has)."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return True
    try:
        return kernel32.WaitForSingleObject(handle, 0) == 0
    finally:
        kernel32.CloseHandle(handle)


def join_kill_on_close_job():
    """Put this process in a new job that ends its members when the last handle closes
    and lets children break away, as Win32-OpenSSH does for a session. The handle is
    kept until this process exits."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.CreateJobObjectW.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p)
    kernel32.SetInformationJobObject.argtypes = (ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32)
    kernel32.AssignProcessToJobObject.argtypes = (ctypes.c_void_p, ctypes.c_void_p)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    job = kernel32.CreateJobObjectW(None, None)
    # JOBOBJECT_EXTENDED_LIMIT_INFORMATION (144 bytes on x64); LimitFlags at offset 16.
    info = (ctypes.c_ubyte * 144)()
    ctypes.c_uint32.from_buffer(info, 16).value = 0x2000 | 0x800  # KILL_ON_JOB_CLOSE | BREAKAWAY_OK
    if not job or not kernel32.SetInformationJobObject(job, 9, info, len(info)) \
            or not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
        raise OSError(ctypes.get_last_error(), "cannot create the command's job")
    return job


def end_executables(patterns):
    """End every process whose executable path matches one of these -like patterns
    (left behind by a failed suite)."""
    for pattern in patterns:
        subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                        "Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -like $env:OUTSIDE_JOB_EXE } "
                        "| ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; "
                        "'outside-job: ended leftover ' + $_.ProcessId }"],
                       env=dict(os.environ, OUTSIDE_JOB_EXE=pattern), stdin=subprocess.DEVNULL, timeout=120)


def child(spec_path):
    spec = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    Path(spec_path).unlink()
    with open(spec["log"], "ab", buffering=0) as log:
        try:
            log.write(f"outside-job: pid {os.getpid()}, in_job {in_job()} before its own job\n".encode())
            job = join_kill_on_close_job()  # noqa: F841 - held until this process exits
            code = subprocess.run(spec["argv"], cwd=spec["cwd"], env=spec["env"], stdin=subprocess.DEVNULL,
                                  stdout=log, stderr=subprocess.STDOUT).returncode
        except OSError as error:
            log.write(f"outside-job: cannot run {spec['argv']}: {error}\n".encode())
            code = 127
        log.flush()
        end_executables(spec["end"])
    temporary = spec["status"] + ".tmp"
    Path(temporary).write_text(str(code), encoding="utf-8")
    os.replace(temporary, spec["status"])


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--child", help=argparse.SUPPRESS)
    parser.add_argument("--log")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--end", action="append", default=[],
                        help="end processes whose executable path is -like this pattern afterwards")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.child:
        child(args.child)
        return 0
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if os.name != "nt" or not command or not args.log:
        parser.error("Windows only; give --log and a command after --")
    log = Path(args.log).resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_bytes(b"")
    work = Path(tempfile.mkdtemp(prefix="outside-job-"))
    status = work / "status"
    spec = work / "spec.json"
    spec.write_text(json.dumps({
        "argv": command, "cwd": os.getcwd(), "log": str(log), "status": str(status),
        "end": args.end,
        "env": {key: value for key, value in os.environ.items() if not SECRET.search(key)},
    }), encoding="utf-8")
    # The base interpreter: a virtual environment's launcher would add a job of its own.
    python = getattr(sys, "_base_executable", None) or sys.executable
    line = subprocess.list2cmdline([python, "-I", str(Path(__file__).resolve()), "--child", str(spec)])
    created = subprocess.run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command",
         "$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments "
         "@{ CommandLine = $env:OUTSIDE_JOB_LINE; CurrentDirectory = $env:OUTSIDE_JOB_CWD }; "
         "'{0} {1}' -f $r.ReturnValue, $r.ProcessId"],
        env=dict(os.environ, OUTSIDE_JOB_LINE=line, OUTSIDE_JOB_CWD=os.getcwd()),
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
    fields = created.stdout.split()
    if created.returncode != 0 or len(fields) != 2 or fields[0] != "0":
        sys.stderr.write(f"outside-job: Win32_Process.Create failed: {created.stdout}{created.stderr}\n")
        return 1
    pid = int(fields[1])
    print(f"outside-job: started pid {pid} outside the step's job", flush=True)
    deadline = time.monotonic() + args.timeout
    gone = None
    with open(log, "rb") as follow:
        while True:
            chunk = follow.read()
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            if status.exists():
                sys.stdout.write(follow.read().decode("utf-8", "replace"))
                code = int(status.read_text(encoding="utf-8"))
                break
            # A copy that ended without a status never started its command.
            if gone is None and exited(pid):
                gone = time.monotonic()
            if gone is not None and time.monotonic() - gone > 5:
                sys.stderr.write(f"outside-job: pid {pid} ended without a status\n")
                code = 1
                break
            if time.monotonic() > deadline:
                # Ending the copy closes its job, which ends the command's processes.
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)], stdin=subprocess.DEVNULL,
                               capture_output=True)
                end_executables(args.end)
                sys.stderr.write(f"outside-job: timed out after {args.timeout:.0f} s; ended pid {pid}\n")
                code = 124
                break
            time.sleep(1)
    for path in (spec, status):
        path.unlink(missing_ok=True)
    work.rmdir()
    return code


if __name__ == "__main__":
    sys.exit(main())
