#!/usr/bin/env python3
"""The bundled ConPTY pair in a session host's image folder (spec D2, D6).

Windows only, with the isolation, helpers and clean-up of `HostCase` in
tests/test_windows_host.py (HGS_TEST_BIN). HGS_TEST_CONPTY_DIR names a folder
holding the pinned conpty.dll and OpenConsole.exe, as `python
scripts/fetch-conpty.py --output DIR` leaves them; without it every case skips.

The host loads conpty.dll by absolute path from the folder of its own image,
and only when OpenConsole.exe sits there too; conpty.dll then starts that
OpenConsole.exe (without one next to it, it would try an x64 subfolder and then
fall back silently to System32's conhost.exe). `Bundled` copies the build under
test into a temporary image folder with the pair and starts an isolated host
from it (HGS_HOST_IMAGE): the host reports `bundled`, its cmd.exe session works
and its pseudoconsole is the OpenConsole.exe of that folder. The same copy then
reports `inbox`, and starts no OpenConsole.exe, with only conpty.dll and with
neither file. `Portable`
enables local sessions from a folder laid out like the extracted portable zip,
as a user would: `hgs session-host install` stages the pair into the host image,
and the host started from that image uses it. Every case waits until no process
holds the files of its image folders before they are removed.
"""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

from test_windows_host import HGS, HIDDEN, LIMIT, REPO, HostCase

sys.path.insert(0, str(REPO / "scripts"))
import conpty_pair  # noqa: E402

PAIR_DIR = os.environ.get("HGS_TEST_CONPTY_DIR")
SKIP = ("set HGS_TEST_CONPTY_DIR to a folder with the pinned conpty.dll and OpenConsole.exe "
        "(python scripts/fetch-conpty.py --output DIR)")

if os.name == "nt":
    from ctypes import wintypes

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", wintypes.LONG),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]

    KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
    KERNEL32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    KERNEL32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
    KERNEL32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    KERNEL32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W))
    KERNEL32.OpenProcess.restype = wintypes.HANDLE
    KERNEL32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    KERNEL32.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                    ctypes.POINTER(wintypes.DWORD))
    KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
    TH32CS_SNAPPROCESS = 0x00000002
    INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def children(pid):
    """(process ID, executable file name) of every running process whose parent is `pid`."""
    snapshot = KERNEL32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snapshot == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        found = []
        listed = KERNEL32.Process32FirstW(snapshot, ctypes.byref(entry))
        while listed:
            if entry.th32ParentProcessID == pid:
                found.append((entry.th32ProcessID, entry.szExeFile))
            listed = KERNEL32.Process32NextW(snapshot, ctypes.byref(entry))
        return found
    finally:
        KERNEL32.CloseHandle(snapshot)


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


def in_use(path):
    """Whether a process runs or loads the file at `path`: Windows then refuses
    to open it for writing. Opening changes nothing."""
    try:
        with open(path, "r+b"):
            return False
    except FileNotFoundError:
        return False
    except PermissionError:
        return True


@unittest.skipUnless(PAIR_DIR, SKIP)
class ConptyCase(HostCase):
    """`HostCase` with image folders of the build under test, with or without the
    pinned pair. When it stops it also waits until nothing holds their files."""

    def setUp(self):
        self.folders = []
        super().setUp()
        # Only the pinned build counts as the bundled pair.
        self.pair = conpty_pair.check_folder(PAIR_DIR)

    def stop_host(self):
        try:
            super().stop_host()
        finally:
            # The host's OpenConsole.exe ends with its pseudoconsoles; a scanner
            # may hold a fresh copy for a moment.
            files = [path for folder in self.folders if folder.is_dir() for path in folder.iterdir()]
            self.wait_for("the image files to be released", lambda: not any(map(in_use, files)))

    def image(self, name, pair=True):
        """A folder with a copy of the build under test, and the pinned pair."""
        folder = self.root / name
        folder.mkdir()
        self.folders.append(folder)
        shutil.copyfile(HGS, folder / "hgs.exe")
        if pair:
            for file, path in self.pair.items():
                shutil.copyfile(path, folder / file)
        return folder

    def started(self, name, folder):
        """Start the host with a cmd.exe session, check that it runs from
        `folder` and that the session works; the host status and the host's line
        in host.log."""
        created, _ = self.cmd(name)
        status = self.status()
        host = status["host"]
        self.assertTrue(os.path.samefile(host["image"], folder / "hgs.exe"), host)
        marker = f"{self.unique}-{name}-echo"
        self.keys(created["pane"], {"literal": f"echo {marker}"}, {"named": "Enter"})
        self.wait_for("the echo", lambda: marker in self.capture(created["pane"]).splitlines())
        # Lines are "<seconds> <process ID> <message>"; the last start of this
        # process ID is this host's.
        log = (self.state / "host.log").read_text(errors="replace")
        started = [line for line in log.splitlines()
                   if line.split(" ", 2)[1:2] == [str(host["pid"])] and "started: image " in line]
        self.assertTrue(started, log)
        return status, started[-1]

    def consoles(self, pid):
        """The OpenConsole.exe processes host `pid` started, by executable path."""
        return [image_of(child) for child, name in children(pid) if name.lower() == "openconsole.exe"]

    def assert_bundled(self, status, line, folder):
        self.assertEqual(status["status"]["conpty"], "bundled", status)
        self.assertIn(" conpty bundled ", line)
        consoles = self.consoles(status["host"]["pid"])
        self.assertTrue(consoles, "the bundled conpty.dll started no OpenConsole.exe")
        for path in consoles:
            self.assertTrue(path and os.path.samefile(path, folder / "OpenConsole.exe"),
                            f"the pseudoconsole runs {path}, not the OpenConsole.exe next to conpty.dll")

    def assert_inbox(self, status, line, reason):
        self.assertEqual(status["status"]["conpty"], "inbox", status)
        self.assertIn(f" conpty inbox ({reason}) ", line)
        self.assertEqual(self.consoles(status["host"]["pid"]), [])


class Bundled(ConptyCase):
    def test_the_host_uses_the_pair_next_to_its_image_and_inbox_without_it(self):
        folder = self.image("image")
        self.env["HGS_HOST_IMAGE"] = str(folder / "hgs.exe")
        status, line = self.started("cmd/bundled", folder)
        self.assert_bundled(status, line, folder)
        self.stop_host()

        # Half a pair is not used: conpty.dll would run System32's conhost.exe.
        (folder / "OpenConsole.exe").unlink()
        status, line = self.started("cmd/half", folder)
        self.assert_inbox(status, line, "conpty.dll and OpenConsole.exe are not both present")
        self.stop_host()

        (folder / "conpty.dll").unlink()
        status, line = self.started("cmd/inbox", folder)
        self.assert_inbox(status, line, "no bundled conpty.dll and OpenConsole.exe")


class Portable(ConptyCase):
    """Like `Staged` in tests/test_windows_host.py: no test host variables, local
    sessions enabled in the case's temporary profile."""

    def setUp(self):
        super().setUp()
        for key in ("HGS_HOST_IMAGE", "HGS_HOST_ALLOW_UNSTAGED"):
            del self.env[key]

    def test_session_host_install_stages_the_pair_and_its_host_uses_it(self):
        portable = self.image("zerus-windows-x86_64")
        result = subprocess.run([str(portable / "hgs.exe"), "session-host", "install", "--json"], env=self.env,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=LIMIT * 2,
                                creationflags=HIDDEN)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        image = json.loads(result.stdout)["image"]
        self.assertEqual((image["pair"], image["conpty"]), ("bundled", "bundled"), image)
        staged = Path(image["path"])
        self.folders.append(staged)
        for name, pinned in conpty_pair.PIN["files"].items():
            self.assertEqual(conpty_pair.sha256(staged / name), pinned["sha256"], name)

        # The launcher, the build under test, starts the staged image.
        status, line = self.started("cmd/portable", staged)
        self.assert_bundled(status, line, staged)
        self.assertEqual(status["host"]["image_hash"], image["sha256"])


if __name__ == "__main__":
    unittest.main()
