"""zerus-setup --mobile-connector protect, migrate and rollback against a fake
user manager: one drop-in each, a reload, and never a start, stop or restart."""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/install-mobile-connector-service.py"
UNIT = "zerus-mobile-connector.service"

# The fake merges the fragment and every drop-in, sorted by file name across
# FAKE_DROPIN_DIRS, as systemd does, but only on daemon-reload: `show` reports
# the definition the last reload loaded.
SYSTEMCTL = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
state = Path(os.environ["FAKE_STATE"])
with open(state / "calls", "a") as log:
    log.write(" ".join(sys.argv[1:]) + "\n")
args = sys.argv[1:]
assert args[0] == "--user", args

def load():
    fragment = Path(os.environ["FAKE_FRAGMENT"])
    if not fragment.exists():
        return {"LoadState": "not-found"}
    files = [fragment] + sorted((path for directory in os.environ["FAKE_DROPIN_DIRS"].split(":")
                                 for path in Path(directory).glob("*.conf")), key=lambda path: path.name)
    kill, commands = "control-group", []
    for path in files:
        section = None
        for line in path.read_text().splitlines():
            line = line.strip()
            if line.startswith("["):
                section = line
            elif section == "[Service]" and "=" in line and not line.startswith("#"):
                key, value = line.split("=", 1)
                if key == "KillMode":
                    kill = value
                elif key == "ExecStart":
                    commands = [] if not value else commands + [value]
    shown = " ".join("{ path=%s ; argv[]=%s ; ignore_errors=no ; start_time=[n/a] ; stop_time=[n/a] ; pid=0 ; "
                     "code=(null) ; status=0/0 }" % (command.split()[0], command) for command in commands)
    return {"LoadState": "loaded", "KillMode": kill, "ExecStart": shown, "FragmentPath": str(fragment),
            "DropInPaths": " ".join(str(path) for path in files[1:])}

if args[1] == "daemon-reload":
    (state / "loaded.json").write_text(json.dumps(load()))
elif args[1] == "show":
    loaded = json.loads((state / "loaded.json").read_text())
    for key in args[args.index("-p") + 1].split(","):
        if key in loaded:
            print(f"{key}={loaded[key]}")
'''


@unittest.skipUnless(sys.platform.startswith("linux"), "systemd user units")
class MobileConnectorService(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="zerus-connector-service-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.home, self.bin, self.state = self.root / "home", self.root / "bin", self.root / "fake"
        for path in (self.home, self.bin, self.state):
            path.mkdir()
        (self.bin / "systemctl").write_text(SYSTEMCTL)
        (self.bin / "systemctl").chmod(0o755)
        self.units = self.home / ".config/systemd/user"
        self.units.mkdir(parents=True)
        self.drop_ins = self.units / (UNIT + ".d")
        self.admin = self.root / "etc-drop-ins"
        self.admin.mkdir()
        self.venv = self.home / ".local/share/zerus-mobile/venv"
        (self.venv / "bin").mkdir(parents=True)
        (self.venv / "pyvenv.cfg").write_text("home = /usr/bin\n")
        self.python = self.venv / "bin/zerus-mobile-connector"
        self.executable(self.python)
        self.config = self.home / ".config/hgs/mobile/connector.json"
        self.config.parent.mkdir(parents=True)
        self.journal = self.root / "state"
        self.journal.mkdir()
        self.config.write_text(json.dumps({"server_url": "https://relay.example.invalid", "node_token": "x",
                                           "state_dir": str(self.journal)}))
        self.rust = self.home / ".local/bin/zerus-mobile-connector"
        self.executable(self.rust)
        self.unit(f"ExecStart={self.python} --config {self.config}\nRestart=on-failure\n")

    def executable(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)

    def unit(self, service):
        (self.units / UNIT).write_text(f"[Unit]\nDescription=Zerus mobile computer connector\n\n[Service]\n{service}")
        self.reload()

    def reload(self):
        self.fake("daemon-reload")
        (self.state / "calls").unlink()

    def fake(self, *args):
        subprocess.run([str(self.bin / "systemctl"), "--user", *args], env=self.environment(), check=True,
                       capture_output=True)

    def environment(self):
        env = {key: value for key, value in os.environ.items() if not key.startswith(("HGS_", "XDG_"))}
        env.update(HOME=str(self.home), PATH=f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin",
                   FAKE_STATE=str(self.state), FAKE_FRAGMENT=str(self.units / UNIT),
                   FAKE_DROPIN_DIRS=f"{self.drop_ins}:{self.admin}")
        return env

    def run_action(self, *args, code=0):
        result = subprocess.run([sys.executable, str(SCRIPT), *args], env=self.environment(),
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        calls = (self.state / "calls").read_text().splitlines() if (self.state / "calls").exists() else []
        (self.state / "calls").unlink(missing_ok=True)
        for call in calls:
            self.assertRegex(call, r"^--user (show zerus-mobile-connector\.service |daemon-reload$)", call)
        return result, calls

    def shown(self):
        return json.loads((self.state / "loaded.json").read_text())

    def files(self):
        return sorted(str(path.relative_to(self.home)) for path in self.home.rglob("*") if path.is_file())

    def test_protect_writes_one_drop_in_once_and_reloads(self):
        before = self.files()
        result, calls = self.run_action("protect")
        self.assertIn("--user daemon-reload", calls)
        self.assertEqual(self.shown()["KillMode"], "process")
        self.assertEqual(sorted(set(self.files()) - set(before)),
                         [f".config/systemd/user/{UNIT}.d/50-zerus-killmode.conf"])
        # Idempotent: already process, nothing written or reloaded.
        result, calls = self.run_action("protect")
        self.assertIn("already uses KillMode=process", result.stdout)
        self.assertNotIn("--user daemon-reload", calls)

    def test_protect_leaves_an_administrator_drop_in_alone(self):
        (self.admin / "10-admin.conf").write_text("[Service]\nKillMode=process\n")
        self.reload()
        before = self.files()
        result, calls = self.run_action("protect")
        self.assertIn("already uses KillMode=process", result.stdout)
        self.assertEqual(self.files(), before)
        self.assertEqual(calls, [f"--user show {UNIT} -p LoadState,KillMode,ExecStart,FragmentPath,DropInPaths --no-pager"])

    def test_protect_removes_its_drop_in_when_a_later_one_wins(self):
        (self.admin / "90-admin.conf").write_text("[Service]\nKillMode=mixed\n")
        self.reload()
        result, _ = self.run_action("protect", code=1)
        self.assertIn("later drop-in keeps another KillMode", result.stderr)
        self.assertFalse((self.drop_ins / "50-zerus-killmode.conf").exists())
        self.assertEqual(self.shown()["KillMode"], "mixed")

    def test_foreign_and_missing_units_are_refused(self):
        self.unit("ExecStart=/usr/bin/sleep infinity\n")
        result, _ = self.run_action("protect", code=1)
        self.assertIn("not a zerus-mobile-connector", result.stderr)
        result, _ = self.run_action("migrate", code=1)
        self.assertIn("migrate it by hand", result.stderr)
        (self.units / UNIT).unlink()
        self.reload()
        result, _ = self.run_action("protect", code=1)
        self.assertIn("not installed", result.stderr)
        self.assertFalse(self.drop_ins.exists())

    def test_migrate_switches_the_venv_unit_and_rollback_restores_it(self):
        result, calls = self.run_action("migrate")
        self.assertIn("--user daemon-reload", calls)
        self.assertIn(f"argv[]={self.rust} --config {self.config} ;", self.shown()["ExecStart"])
        self.assertEqual(self.shown()["KillMode"], "process")
        drop_in = (self.drop_ins / "60-zerus-rust-connector.conf").read_text()
        self.assertTrue(drop_in.startswith("# Written by zerus-setup --mobile-connector migrate"))
        self.assertIn("\nExecStart=\nExecStart=", drop_in)
        self.assertIn("systemctl --user start", result.stdout)
        result, calls = self.run_action("migrate")
        self.assertIn("already runs", result.stdout)
        self.assertNotIn("--user daemon-reload", calls)
        result, calls = self.run_action("rollback")
        self.assertIn("--user daemon-reload", calls)
        self.assertFalse((self.drop_ins / "60-zerus-rust-connector.conf").exists())
        self.assertIn(f"argv[]={self.python} --config {self.config} ;", self.shown()["ExecStart"])
        result, _ = self.run_action("rollback")
        self.assertIn("nothing to roll back", result.stdout)

    def test_rollback_keeps_the_protection_and_files_it_did_not_write(self):
        self.run_action("protect")
        self.run_action("migrate")
        self.run_action("rollback")
        self.assertTrue((self.drop_ins / "50-zerus-killmode.conf").exists())
        (self.drop_ins / "60-zerus-rust-connector.conf").write_text("[Service]\nExecStart=\nExecStart=/x\n")
        result, _ = self.run_action("rollback", code=1)
        self.assertIn("not written by this tool", result.stderr)
        self.assertTrue((self.drop_ins / "60-zerus-rust-connector.conf").exists())

    def test_migrate_is_refused_while_a_connector_holds_the_journal(self):
        lock = os.open(self.journal / "connector.lock", os.O_RDWR | os.O_CREAT, 0o600)
        self.addCleanup(os.close, lock)
        holder = subprocess.Popen([sys.executable, "-c", "import fcntl, sys, time\n"
                                   f"fcntl.flock({lock}, fcntl.LOCK_EX)\nprint('held', flush=True)\ntime.sleep(60)"],
                                  pass_fds=(lock,), stdout=subprocess.PIPE, text=True)
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        self.assertEqual(holder.stdout.readline(), "held\n")
        result, calls = self.run_action("migrate", code=1)
        self.assertIn("still holds this configuration's journal", result.stderr)
        self.assertNotIn("--user daemon-reload", calls)
        self.assertFalse(self.drop_ins.exists())

    def test_a_later_sorting_override_undoes_the_migration(self):
        (self.admin / "override.conf").write_text(f"[Service]\nExecStart=\nExecStart={self.python} --config {self.config}\n")
        self.reload()
        result, _ = self.run_action("migrate", code=1)
        self.assertIn("later-sorting drop-in", result.stderr)
        self.assertFalse((self.drop_ins / "60-zerus-rust-connector.conf").exists())
        self.assertIn(f"argv[]={self.python} ", self.shown()["ExecStart"])

    def test_both_installed_binaries_need_an_explicit_choice(self):
        system = self.root / "usr-bin/zerus-mobile-connector"
        self.executable(system)
        env_script = SCRIPT.read_text().replace('Path("/usr/bin/zerus-mobile-connector")', repr(str(system)).join(("Path(", ")")))
        script = self.root / "install-mobile-connector-service.py"
        script.write_text(env_script)
        result = subprocess.run([sys.executable, str(script), "migrate"], env=self.environment(),
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("choose one with --binary", result.stderr)
        result = subprocess.run([sys.executable, str(script), "migrate", "--binary", str(system)],
                                env=self.environment(), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"argv[]={system} --config", self.shown()["ExecStart"])

    def test_a_unit_without_the_documented_venv_shape_is_not_migrated(self):
        for command in (f"{self.python} --config {self.config} --allow-insecure-localhost",
                        f"/opt/connector/zerus-mobile-connector --config {self.config}"):
            with self.subTest(command=command):
                self.unit(f"ExecStart={command}\n")
                result, _ = self.run_action("migrate", code=1)
                self.assertIn("migrate it by hand", result.stderr)
                self.assertFalse(self.drop_ins.exists())


if __name__ == "__main__":
    unittest.main()
