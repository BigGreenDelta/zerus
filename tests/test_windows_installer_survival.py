#!/usr/bin/env python3
"""The per-user installer on Windows: install, upgrade and uninstall while a
session host runs (docs/superpowers/specs/2026-10-18-windows-installer-design.md).

Windows only, with HGS_TEST_INSTALLER (the setup.exe under test) and
HGS_TEST_PORTABLE_ZIP (the zip it was built from), and the isolation of
`HostCase` in tests/test_windows_host.py (HGS_TEST_BIN, the same hgs.exe). The
installer goes to a folder of the case (/DIR) but registers its uninstaller and
Start menu shortcut in the runner's own profile, as it would for a user; the
case uninstalls it again. Local sessions are enabled from the installed folder
in the case's temporary profile, so the session host runs from its staged
image. An upgrade must leave that host, its session, the staged CLI and the
images alone, install the zip's files byte for byte and remove files an older
version left; the uninstaller removes only the installed files. Run it outside
the CI step's job (scripts/ci/outside-job.py), as the host suites run.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

import test_windows_host as windows_host
from test_windows_host import HIDDEN, LIMIT

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import windows_installer

INSTALLER = os.environ.get("HGS_TEST_INSTALLER")
PORTABLE_ZIP = os.environ.get("HGS_TEST_PORTABLE_ZIP")
# The AppId of packaging/windows/zerus.iss with Inno Setup's suffix.
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1B0C52-7E3A-4D8B-9C41-2A5E8F0D3B17}_is1"
SETUP_LIMIT = 300


def registered():
    """DisplayVersion and InstallLocation of the HKCU uninstall entry, or None."""
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY) as key:
            return {name: winreg.QueryValueEx(key, name)[0] for name in ("DisplayVersion", "InstallLocation")}
    except FileNotFoundError:
        return None


@unittest.skipUnless(os.name == "nt" and INSTALLER, "needs Windows and HGS_TEST_INSTALLER")
class Installer(windows_host.HostCase):
    def setUp(self):
        super().setUp()
        for key in ("HGS_HOST_IMAGE", "HGS_HOST_ALLOW_UNSTAGED"):
            del self.env[key]
        self.setup_exe = Path(INSTALLER).resolve()
        self.assertTrue(self.setup_exe.is_file(), self.setup_exe)
        self.assertTrue(PORTABLE_ZIP and Path(PORTABLE_ZIP).is_file(), "HGS_TEST_PORTABLE_ZIP")
        self.assertIsNone(registered(), "Zerus is already installed for this user")
        expected = tempfile.TemporaryDirectory(prefix="zerus-expected-")
        self.addCleanup(expected.cleanup)
        self.manifest, self.expected = windows_installer.stage(PORTABLE_ZIP, Path(expected.name))
        self.app = self.root / "Zerus"
        self.cli = self.home / ".local" / "bin" / "hgs.exe"
        self.images = self.home / ".local" / "share" / "hgs" / "host"
        self.marker = self.home / ".config" / "hgs" / "local-sessions"
        self.shortcut = Path(os.environ["APPDATA"]) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Zerus.lnk"
        self.logs = 0
        # Registered last, so it runs first: the host and its sessions stop afterwards.
        self.addCleanup(self.uninstall_if_installed)

    # The installer and the uninstaller run with the runner's own environment:
    # Inno Setup finds the profile's folders through the shell, not HGS_*.

    def install(self):
        self.logs += 1
        log = self.root / f"setup-{self.logs}.log"
        result = subprocess.run([str(self.setup_exe), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                 f"/DIR={self.app}", f"/LOG={log}"], stdin=subprocess.DEVNULL, capture_output=True,
                                timeout=SETUP_LIMIT, creationflags=HIDDEN)
        text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else "(no log)"
        self.assertEqual(result.returncode, 0, text[-4000:])
        return text

    def uninstall(self):
        uninstaller = self.app / "uninstall" / "unins000.exe"
        self.assertTrue(uninstaller.is_file(), uninstaller)
        result = subprocess.run([str(uninstaller), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"],
                                stdin=subprocess.DEVNULL, capture_output=True, timeout=SETUP_LIMIT,
                                creationflags=HIDDEN)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The uninstaller continues from a copy of itself in the temporary folder
        # and removes its own files last.
        self.wait_for("the uninstall", lambda: registered() is None and not any(
            path.is_file() for path in self.app.rglob("*")), timeout=SETUP_LIMIT)

    def uninstall_if_installed(self):
        if registered() is not None and (self.app / "uninstall" / "unins000.exe").is_file():
            subprocess.run([str(self.app / "uninstall" / "unins000.exe"), "/VERYSILENT", "/SUPPRESSMSGBOXES",
                            "/NORESTART"], stdin=subprocess.DEVNULL, capture_output=True, timeout=SETUP_LIMIT,
                           creationflags=HIDDEN)

    def run_installed(self, name, *args):
        result = subprocess.run([str(self.app / name), *args], env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=LIMIT * 2, creationflags=HIDDEN)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def staged(self):
        """What enabling local sessions put in the profile, by path and SHA-256."""
        files = [self.cli, self.marker, *sorted(path for path in self.images.rglob("*") if path.is_file())]
        return {str(path): windows_installer.sha256(path) for path in files}

    def responds(self, pane, marker):
        self.keys(pane, {"literal": f"echo {marker}"}, {"named": "Enter"})
        self.wait_for(marker, lambda: self.capture(pane).count(marker) >= 2)

    def test_upgrade_and_uninstall_leave_the_session_host_and_staged_files_alone(self):
        version = self.manifest["version"]
        self.install()
        self.assertEqual(windows_installer.problems(self.app, self.expected), [])
        self.assertEqual(registered(), {"DisplayVersion": version, "InstallLocation": str(self.app) + "\\"})
        self.assertTrue(self.shortcut.is_file(), self.shortcut)
        record = json.loads((self.app / windows_installer.INSTALLATION).read_text(encoding="utf-8"))
        self.assertEqual((record["kind"], record["version"]), ("installer", version))
        self.assertEqual(self.run_installed("hgs-tray.exe", "--version"), f"Zerus {version}")

        # Local sessions from the installed folder: the host runs from its image.
        installed = json.loads(self.run_installed("hgs.exe", "session-host", "install", "--json"))
        self.assertTrue(installed["enabled"], installed)
        created, _ = self.cmd("cmd/installed")
        pane = created["pane"]
        hello = self.status()["host"]
        image = Path(hello["image"])
        self.assertTrue(image.is_relative_to(self.images), image)
        before = self.staged()

        # Files an older version shipped and this one does not.
        (self.app / "platforms" / "qstale.dll").write_bytes(b"older Qt plugin")
        (self.app / "avcodec-60.dll").write_bytes(b"older FFmpeg")
        (self.app / "oldplugins").mkdir()
        (self.app / "oldplugins" / "gone.dll").write_bytes(b"older plugin folder")

        log = self.install()
        self.assertIn("Removed platforms\\qstale.dll", log)
        self.assertEqual(windows_installer.problems(self.app, self.expected), [])
        self.assertFalse((self.app / "oldplugins").exists())
        status = self.status()["host"]
        self.assertEqual((status["pid"], status["created"]), (hello["pid"], hello["created"]))
        self.responds(pane, f"{self.unique}-after-upgrade")
        self.assertEqual(self.staged(), before)

        self.uninstall()
        # Empty folders may remain until Windows lets the uninstaller remove them.
        self.assertEqual([path for path in self.app.rglob("*") if path.is_file()], [])
        self.assertFalse(self.shortcut.exists())
        status = self.status()["host"]
        self.assertEqual((status["pid"], status["created"]), (hello["pid"], hello["created"]))
        self.responds(pane, f"{self.unique}-after-uninstall")
        self.assertEqual(self.staged(), before)


if __name__ == "__main__":
    unittest.main()
