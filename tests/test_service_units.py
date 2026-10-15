"""systemd units of Zerus workers kill only their own process, never the agents it started."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INSTALLERS = {"install-recovery-service.py": "hgs-recovery.service",
              "install-swarm-service.py": "hgs-swarm.service",
              "install-voice-service.py": "hgs-voice.service"}


def service(text):
    """The [Service] settings of a unit, each as the list of its values."""
    settings, section = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("["):
            section = line
        elif section == "[Service]" and "=" in line and not line.startswith(("#", ";")):
            key, value = line.split("=", 1)
            settings.setdefault(key.strip(), []).append(value.strip())
    return settings


class PackagedUnits(unittest.TestCase):
    def test_packaged_and_source_gui_units_kill_only_their_main_process(self):
        units = sorted((ROOT / "packaging/linux").glob("*.service")) + [ROOT / "tray/packaging/hgs-tray.service"]
        self.assertIn(ROOT / "packaging/linux/hgs-recovery.service", units)
        self.assertIn(ROOT / "packaging/linux/hgs-swarm.service", units)
        for unit in units:
            with self.subTest(unit=unit.name):
                self.assertEqual(service(unit.read_text(encoding="utf-8")).get("KillMode"), ["process"])

    def test_documented_connector_unit_and_protection_drop_in_kill_only_the_connector(self):
        guide = (ROOT / "docs/mobile-deployment.md").read_text(encoding="utf-8")
        blocks = [block for block in re.findall(r"```ini\n(.*?)```", guide, re.S) if "zerus-mobile-connector" in block]
        self.assertEqual(len(blocks), 1)
        self.assertEqual(service(blocks[0]).get("KillMode"), ["process"])
        drop_in = "~/.config/systemd/user/zerus-mobile-connector.service.d/50-zerus-killmode.conf"
        self.assertIn(f"printf '[Service]\\nKillMode=process\\n' \\\n  > {drop_in}", guide)
        self.assertIn(drop_in, (ROOT / "tray/src/RelaySettings.h").read_text(encoding="utf-8"))


@unittest.skipUnless(sys.platform.startswith("linux"), "systemd user units are written on Linux")
class SourceInstallers(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="hgs-service-units-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.home, self.bin = self.root / "home", self.root / "bin"
        self.home.mkdir(); self.bin.mkdir()
        self.calls = self.root / "systemctl.calls"
        systemctl = self.bin / "systemctl"
        systemctl.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> '{self.calls}'\n")
        systemctl.chmod(0o755)
        self.hgs = self.root / "hgs"
        self.hgs.write_text("#!/bin/sh\nexit 0\n"); self.hgs.chmod(0o755)

    def install(self, script, *arguments):
        environment = {key: value for key, value in os.environ.items() if not key.startswith("HGS_")}
        environment.update(HOME=str(self.home), PATH=f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin",
                           PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, str(ROOT / "scripts" / script), str(self.hgs), *arguments],
                                env=environment, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text().splitlines()
        self.calls.unlink()
        return calls

    def test_installers_write_units_that_kill_only_the_worker(self):
        for script, unit in INSTALLERS.items():
            with self.subTest(script=script):
                self.assertEqual(self.install(script), ["--user daemon-reload", f"--user enable --now {unit}"])
                settings = service((self.home / ".config/systemd/user" / unit).read_text(encoding="utf-8"))
                self.assertEqual(settings.get("KillMode"), ["process"])
                self.assertEqual(settings.get("Restart"), ["always"])

    def test_upgrade_rewrites_an_old_unit_and_reloads_before_restarting(self):
        # Before KillMode=process, a restart killed every process the worker started.
        for script, unit in INSTALLERS.items():
            with self.subTest(script=script):
                path = self.home / ".config/systemd/user" / unit
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("[Service]\nType=simple\nExecStart=/old/hgs worker\nRestart=always\n")
                self.assertEqual(self.install(script, "--restart"), [
                    "--user daemon-reload", f"--user enable --now {unit}", f"--user restart {unit}"])
                self.assertEqual(service(path.read_text(encoding="utf-8")).get("KillMode"), ["process"])


if __name__ == "__main__":
    unittest.main()
