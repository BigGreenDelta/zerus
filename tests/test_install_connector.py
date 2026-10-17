"""install.sh and the Rust mobile connector: built only on request or to update an
installed copy, installed only when changed, never enabled, started or restarted."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CARGO = """#!/bin/sh
# Records each build and writes a binary whose bytes name its source revision.
printf '%s\\n' "$*" >> "$FIXTURE/cargo.calls"
manifest="" target=""
while [ $# -gt 0 ]; do
    case "$1" in
        --manifest-path) manifest="$2"; shift ;;
        --target-dir) target="$2"; shift ;;
    esac
    shift
done
target="${target:-$CARGO_TARGET_DIR}"
mkdir -p "$target/release"
case "$manifest" in
    */services/mobile/connector/Cargo.toml) name=zerus-mobile-connector ;;
    *) name=hgs ;;
esac
printf '#!/bin/sh\\necho "%s %s"\\n' "$name" "$(cat "$FIXTURE/revision")" > "$target/release/$name"
chmod 755 "$target/release/$name"
"""

SYSTEMCTL = """#!/bin/sh
printf '%s\\n' "$*" >> "$FIXTURE/systemctl.calls"
case "$*" in
    "--user is-active --quiet zerus-mobile-connector.service") [ -e "$FIXTURE/active" ] ;;
    "--user show zerus-mobile-connector.service -p ExecStart") cat "$FIXTURE/exec-start" 2>/dev/null ;;
    *) exit 0 ;;
esac
"""


@unittest.skipUnless(sys.platform.startswith("linux"), "the Linux branch of install.sh")
class InstallConnector(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="zerus-install-connector-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.fixture, self.bin, self.prefix = self.root / "fixture", self.root / "bin", self.root / "prefix"
        for path in (self.fixture, self.bin):
            path.mkdir()
        for name, text in (("cargo", CARGO), ("systemctl", SYSTEMCTL)):
            (self.bin / name).write_text(text)
            (self.bin / name).chmod(0o755)
        self.revision("one")
        self.connector = self.prefix / "bin/zerus-mobile-connector"

    def revision(self, value):
        (self.fixture / "revision").write_text(value)

    def install(self, setting=None, code=0):
        env = {key: value for key, value in os.environ.items() if not key.startswith(("HGS_", "CARGO"))}
        # A prefix keeps the worker installers and every real service out of reach.
        env.update(PATH=f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin", HOME=str(self.root / "home"),
                   HGS_INSTALL_PREFIX=str(self.prefix), CARGO_TARGET_DIR=str(self.root / "target"),
                   FIXTURE=str(self.fixture))
        if setting is not None:
            env["HGS_MOBILE_CONNECTOR"] = setting
        for name in ("cargo.calls", "systemctl.calls"):
            (self.fixture / name).unlink(missing_ok=True)
        result = subprocess.run(["bash", str(ROOT / "install.sh")], env=env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result.stdout

    def calls(self, name):
        path = self.fixture / f"{name}.calls"
        return path.read_text().splitlines() if path.exists() else []

    def connector_builds(self):
        return [call for call in self.calls("cargo") if "services/mobile/connector/Cargo.toml" in call]

    def assert_never_started(self):
        for call in self.calls("systemctl"):
            self.assertNotRegex(call, r"\b(start|restart|enable|reload|daemon-reload|kickstart)\b", call)

    def test_default_skips_the_connector_until_one_is_installed(self):
        output = self.install()
        self.assertEqual(self.connector_builds(), [])
        self.assertFalse(self.connector.exists())
        self.assertNotIn("zerus-mobile-connector", output)
        self.assertEqual(self.calls("systemctl"), [])

    def test_opt_in_installs_once_and_then_updates_the_installed_copy(self):
        output = self.install("1")
        self.assertEqual(len(self.connector_builds()), 1)
        self.assertIn("--locked --release", self.connector_builds()[0])
        self.assertIn(f"--target-dir {self.root / 'target'}", self.connector_builds()[0])
        self.assertIn("Installed zerus-mobile-connector.", output)
        self.assertIn("zerus-mobile-connector one", output)
        self.assertEqual(os.stat(self.connector).st_mode & 0o777, 0o755)
        inode = self.connector.stat().st_ino
        # Unchanged: rebuilt because it is installed, but left in place.
        output = self.install()
        self.assertEqual(len(self.connector_builds()), 1)
        self.assertNotIn("Installed zerus-mobile-connector.", output)
        self.assertEqual(self.connector.stat().st_ino, inode)
        self.revision("two")
        output = self.install()
        self.assertIn("Installed zerus-mobile-connector.", output)
        self.assertIn("zerus-mobile-connector two", output)
        self.assertEqual(list(self.connector.parent.glob(".zerus-mobile-connector.*")), [])
        self.assert_never_started()

    def test_zero_always_skips(self):
        self.install("1")
        self.revision("two")
        self.install("0")
        self.assertEqual(self.connector_builds(), [])
        self.assertIn("zerus-mobile-connector one", subprocess.check_output([str(self.connector)], text=True))

    def test_a_changed_binary_the_running_unit_uses_gets_a_hint_and_no_restart(self):
        self.install("1")
        self.revision("two")
        (self.fixture / "active").touch()
        # The Python connector's unit runs another program: no hint.
        (self.fixture / "exec-start").write_text(
            "ExecStart={ path=/example/venv/bin/zerus-mobile-connector ; argv[]=/example/venv/bin/zerus-mobile-connector "
            "--config /example/connector.json ; }\n")
        self.assertNotIn("still uses its previous binary", self.install())
        self.revision("three")
        (self.fixture / "exec-start").write_text(
            f"ExecStart={{ path={self.connector} ; argv[]={self.connector} --config /example/connector.json ; }}\n")
        output = self.install()
        self.assertIn("still uses its previous binary", output)
        self.assertIn("then restart it: systemctl --user restart zerus-mobile-connector.service", output)
        self.assertEqual(self.calls("systemctl"), ["--user is-active --quiet zerus-mobile-connector.service",
                                                   "--user show zerus-mobile-connector.service -p ExecStart"])
        self.assert_never_started()

    def test_other_values_are_refused_before_building_it(self):
        self.install("yes", code=2)
        self.assertEqual(self.connector_builds(), [])


if __name__ == "__main__":
    unittest.main()
