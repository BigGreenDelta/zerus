"""Public analytics CLI, entirely in private synthetic homes; no native agents."""
import hashlib
import json
import os
import shlex
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
MACHINE = "11111111-1111-4111-8111-111111111111"
NIL = "00000000-0000-0000-0000-000000000000"
READ = dict(schema=1, action="summary", **{"from": "2026-10-01", "through": "2026-10-18"})


class AnalyticsCli(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="zerus-analytics-cli-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.config = self.root / "config"
        self.state = self.root / "state"
        self.config.mkdir()
        self.state.mkdir()
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        # Any accidental session/native dispatch fails and leaves an explicit marker.
        tmux = bin_dir / "tmux"
        tmux.write_text("#!/bin/sh\ntouch \"$HGS_CONFIG_DIR/native-called\"\nexit 99\n")
        tmux.chmod(0o755)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("HGS_") and key not in {"TMUX", "TMUX_PANE"}}
        self.env.update(HOME=str(self.root), HGS_CONFIG_DIR=str(self.config),
                        HGS_STATE_DIR=str(self.state), PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])
        self.addCleanup(lambda: self.assertFalse((self.config / "native-called").exists()))

    def identity(self):
        (self.config / "swarm").mkdir()
        (self.config / "swarm/catalog.json").write_text(json.dumps(dict(node_id=MACHINE)))

    def call(self, payload, ok=True):
        result = subprocess.run([str(HGS), "analytics", "--json"], input=json.dumps(payload),
                                env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode == 0, ok, result.stderr)
        return json.loads(result.stdout) if ok else result.stderr

    def files(self):
        return {str(path.relative_to(self.root)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.root.rglob("*") if path.is_file()}

    def test_read_without_identity_does_not_initialize_a_computer(self):
        before = self.files()
        self.assertIn("identity unavailable", self.call(READ, ok=False))
        self.assertEqual(self.files(), before)

    def test_disabled_read_is_pure_and_rejects_unbounded_or_unknown_fields(self):
        self.identity()
        before = self.files()
        for _ in range(2):
            reply = self.call(READ)
            self.assertEqual(reply["machine_id"], MACHINE)
            self.assertFalse(reply["collection"]["enabled"])
            self.assertEqual(reply["source_epoch"], NIL)
            self.assertEqual(reply["buckets"], [])
        for invalid in [dict(READ, prompt="PRIVATE_MARKER"), dict(READ, through="2027-10-18")]:
            self.call(invalid, ok=False)
        self.assertEqual(self.files(), before)

    def test_local_configuration_and_purge_have_exact_epoch_guards_and_preserve_receipts(self):
        self.identity()
        native = self.state / "input_receipts"
        native.mkdir()
        receipt = native / "synthetic-receipt.json"
        receipt.write_text('{"request_id":"synthetic","status":"submitted"}')
        content = receipt.read_bytes()
        enabled = self.call(dict(schema=1, action="configure", expected_epoch=NIL, enabled=True, retention_days=30))
        self.assertTrue(enabled["collection"]["enabled"])
        epoch = enabled["source_epoch"]
        self.assertNotEqual(epoch, NIL)
        self.call(dict(schema=1, action="configure", expected_epoch=NIL, enabled=False, retention_days=30), ok=False)
        self.call(dict(schema=1, action="purge", expected_epoch=epoch, confirm=False), ok=False)
        purged = self.call(dict(schema=1, action="purge", expected_epoch=epoch, confirm=True))
        self.assertTrue(purged["collection"]["enabled"])
        self.assertNotEqual(purged["source_epoch"], epoch)
        self.call(dict(schema=1, action="purge", expected_epoch=epoch, confirm=True), ok=False)
        disabled = self.call(dict(schema=1, action="configure", expected_epoch=purged["source_epoch"],
                                  enabled=False, retention_days=30))
        self.assertFalse(disabled["collection"]["enabled"])
        self.assertEqual(receipt.read_bytes(), content)
        before = self.files()
        self.call(READ)
        self.assertEqual(self.files(), before)

    def test_remote_read_boundary_rejects_configuration_before_state_access(self):
        before = self.files()
        payload = dict(schema=1, action="configure", expected_epoch=NIL, enabled=True, retention_days=30)
        result = subprocess.run([str(HGS), "analytics", "--json", "--read-only"], input=json.dumps(payload),
                                env=self.env, text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("only permits summary reads", result.stderr)
        self.assertEqual(self.files(), before)
        dry = subprocess.run([str(HGS), "--dry-run", "@fixture.invalid", "analytics", "--json"],
                             env=self.env, text=True, capture_output=True, timeout=10)
        self.assertEqual(dry.returncode, 0, dry.stderr)
        self.assertIn("analytics --json --dry-run --read-only", dry.stdout)
        # The probe runs under sh whatever the peer's login shell is.
        remote = shlex.split(dry.stdout)[-3:]
        self.assertEqual(remote[:2], ["sh", "-c"])
        self.assertIn("grep -qF 'hgs [@host] analytics --json'", remote[2])


if __name__ == "__main__":
    unittest.main()
