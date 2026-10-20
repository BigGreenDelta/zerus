"""Deadline-effective manager capture; the original pipes must reach EOF.

No reader thread or communicate() is used. A manager's descendant retaining a
writer is a timeout even after the manager exits. Only the retained direct
Popen handle may be stopped; no descendant/PID/group is adopted or signalled.
"""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

SECONDS = 15
REAP_SECONDS = 5
OUTPUT_LIMIT = 65536


def available(pipe):
    """Read only bytes already available; None is the actual broken-pipe EOF."""
    import msvcrt
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    peek = kernel.PeekNamedPipe
    peek.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                     ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p)
    peek.restype = wintypes.BOOL
    count = wintypes.DWORD()
    if not peek(msvcrt.get_osfhandle(pipe.fileno()), None, 0, None, ctypes.byref(count), None):
        error = ctypes.get_last_error()
        if error == 109:  # ERROR_BROKEN_PIPE: every writer is closed.
            return None
        raise OSError(error, "manager pipe could not be observed")
    return os.read(pipe.fileno(), min(count.value, OUTPUT_LIMIT + 1)) if count.value else b""


def exclusive(path):
    return os.fdopen(os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600), "w+b")


class ManagementCapture:
    def __init__(self, root, read_available=available):
        self.folder = Path(tempfile.mkdtemp(prefix="manage-", dir=root))
        self.read_available = read_available
        self.process = None
        self.exited = True  # No process exists before Popen succeeds.
        self.failed = False
        self.pipes_ended = False

    @property
    def may_remove_root(self):
        return self.exited and not self.failed

    def stop_direct(self, original):
        """Bounded cleanup never replaces the original timeout/error."""
        if self.process is None or self.exited:
            return
        try:
            self.process.kill()
        except Exception as error:
            original.add_note("direct manager stop: " + type(error).__name__)
        try:
            self.process.wait(timeout=REAP_SECONDS)
            self.exited = True
        except Exception as error:
            original.add_note("direct manager exit UNKNOWN: " + type(error).__name__)

    def run(self, argv, payload, env, creationflags):
        # Keep failure latched until the command, EOF, deadline AND every close
        # succeed. Manual ownership lets every finalizer run without replacing
        # an earlier timeout/error with a later context-manager close error.
        self.failed = True
        files, primary, result = [], None, None
        def own_file(name):
            stream = exclusive(self.folder / name)
            files.append((name, stream))
            return stream
        def close_error(label, error):
            nonlocal primary
            if primary is None:
                primary = error
            primary.add_note(label + ": " + type(error).__name__)
        try:
            # JSON keeps its prior ASCII escaping; regular stdin needs no writer.
            source, out, err = own_file("stdin.json"), own_file("stdout"), own_file("stderr")
            source.write(json.dumps(payload or {}).encode("ascii"))
            source.flush()
            source.seek(0)
            self.process = subprocess.Popen(argv, stdin=source, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=env, creationflags=creationflags)
            self.exited = False
            deadline = time.monotonic() + SECONDS
            def before_deadline():
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(argv, SECONDS)
            self.process.wait(timeout=max(0, deadline - time.monotonic()))
            self.exited = True
            before_deadline()
            buffers = [bytearray(), bytearray()]
            pending = [("stdout", self.process.stdout, out, buffers[0]),
                       ("stderr", self.process.stderr, err, buffers[1])]
            while pending:
                remaining = []
                for name, pipe, retained, buffer in pending:
                    before_deadline()
                    data = self.read_available(pipe)
                    if data:
                        prefix = data[:OUTPUT_LIMIT - len(buffer)]
                        observed = len(buffer) + len(data)
                        retained.write(prefix)
                        retained.flush()
                        buffer.extend(prefix)
                        if len(prefix) != len(data):
                            metadata = own_file("truncated.json")
                            metadata.write(json.dumps({"stream": name, "retained_bytes": len(buffer),
                                "observed_at_least": observed, "truncated": True}).encode("ascii"))
                            metadata.flush()
                            before_deadline()
                            raise RuntimeError("manager output exceeds its limit; retained prefix truncated")
                    # Keep any collected bytes before refusing a late read/EOF.
                    before_deadline()
                    if data is None:
                        continue
                    remaining.append((name, pipe, retained, buffer))
                pending = remaining
                if pending:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise subprocess.TimeoutExpired(argv, SECONDS)
                    time.sleep(min(0.01, left))
            before_deadline()
            result = subprocess.CompletedProcess(argv, self.process.returncode,
                buffers[0].decode("utf-8"), buffers[1].decode("utf-8"))
            before_deadline()
        except BaseException as error:
            primary = error
            try:
                self.stop_direct(primary)
            except BaseException as error:
                close_error("direct manager cleanup", error)
        finally:
            if self.process is not None:
                # No reader thread holds these Python locks. Each pipe closes
                # independently, even if its peer or a private file refuses.
                for name, pipe in (("stdout", self.process.stdout), ("stderr", self.process.stderr)):
                    if pipe is not None:
                        try:
                            pipe.close()
                        except BaseException as error:
                            close_error("manager " + name + " close", error)
            for name, stream in reversed(files):
                try:
                    stream.close()
                except BaseException as error:
                    close_error("private " + name + " close", error)
        if primary is not None:
            raise primary
        before_deadline()  # Finalization cannot turn a late capture into success.
        self.pipes_ended = True
        self.failed = result.returncode != 0
        return result
