"""The zerus-mobile compatibility command delegates to the Rust relay."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid


def zerus_mobile(*arguments):
    return subprocess.run([sys.executable, '-m', 'zerus_mobile', *arguments],
                          text=True, capture_output=True, timeout=60)


@unittest.skipUnless(shutil.which('zerus-relay'), 'Rust executable required for compatibility CLI')
class CompatibilityCliTests(unittest.TestCase):
    def test_administration_execs_the_rust_relay(self):
        # A private temporary database keeps the default under HOME untouched.
        with tempfile.TemporaryDirectory(prefix='zerus-relay-cli-') as directory:
            completed = zerus_mobile('--database', str(Path(directory) / 'relay.sqlite3'),
                                     'revoke-computer', '--id', str(uuid.uuid4()))
            self.assertEqual(completed.returncode, 2, completed)
            self.assertIn('zerus-relay: computer not found', completed.stderr)

    def test_offline_migration_command_is_removed(self):
        with tempfile.TemporaryDirectory(prefix='zerus-relay-cli-') as directory:
            completed = zerus_mobile('--database', str(Path(directory) / 'relay.sqlite3'), 'migrate-sqlite',
                                     '--source', str(Path(directory) / 'source.sqlite3'), '--source-offline')
            self.assertEqual(completed.returncode, 2, completed)
            self.assertIn("invalid choice: 'migrate-sqlite'", completed.stderr)
            self.assertNotIn('zerus-relay:', completed.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
