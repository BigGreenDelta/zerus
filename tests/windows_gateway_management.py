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
        # Regular private stdin avoids a separate potentially blocked writer.
        # JSON uses its default ASCII escaping, exactly as the prior run(input=).
        self.failed = True  # A partial file/setup failure also retains evidence.
        with exclusive(self.folder / "stdin.json") as source, \
                exclusive(self.folder / "stdout") as out, exclusive(self.folder / "stderr") as err:
            source.write(json.dumps(payload or {}).encode("ascii"))
            source.flush()
            source.seek(0)
            try:
                self.process = subprocess.Popen(argv, stdin=source, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, env=env, creationflags=creationflags)
                self.exited = False
                deadline = time.monotonic() + SECONDS
                self.process.wait(timeout=max(0, deadline - time.monotonic()))
                self.exited = True
                buffers = [bytearray(), bytearray()]
                pending = [(self.process.stdout, out, buffers[0]), (self.process.stderr, err, buffers[1])]
                while pending:
                    remaining = []
                    for pipe, retained, buffer in pending:
                        data = self.read_available(pipe)
                        if data is None:
                            continue
                        if data:
                            retained.write(data)
                            retained.flush()
                            buffer.extend(data)
                            if len(buffer) > OUTPUT_LIMIT:
                                raise RuntimeError("manager output exceeds its limit")
                        remaining.append((pipe, retained, buffer))
                    pending = remaining
                    if pending:
                        left = deadline - time.monotonic()
                        if left <= 0:
                            raise subprocess.TimeoutExpired(argv, SECONDS)
                        time.sleep(min(0.01, left))
                result = subprocess.CompletedProcess(argv, self.process.returncode,
                    buffers[0].decode("utf-8"), buffers[1].decode("utf-8"))
                self.pipes_ended = True
                self.failed = result.returncode != 0
                return result
            except BaseException as error:
                self.failed = True
                self.stop_direct(error)
                raise
            finally:
                if self.process is not None:
                    # No background reader holds these files' Python locks.
                    for pipe in (self.process.stdout, self.process.stderr):
                        if pipe is not None:
                            pipe.close()
