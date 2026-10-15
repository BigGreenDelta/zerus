"""The zerus-mobile compatibility command delegates to the Rust relay."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid


def zerus_mobile(home, *arguments):
    # A private HOME holds the default database, so the real one stays untouched.
    return subprocess.run([sys.executable, '-m', 'zerus_mobile', *arguments],
                          text=True, capture_output=True, timeout=60, env={**os.environ, 'HOME': home})


@unittest.skipUnless(shutil.which('zerus-relay'), 'Rust executable required for compatibility CLI')
class CompatibilityCliTests(unittest.TestCase):
    def test_administration_execs_the_rust_relay(self):
        with tempfile.TemporaryDirectory(prefix='zerus-relay-cli-') as directory:
            completed = zerus_mobile(directory, 'revoke-computer', '--id', str(uuid.uuid4()))
            self.assertEqual(completed.returncode, 2, completed)
            self.assertIn('zerus-relay: computer not found', completed.stderr)

    def test_offline_migration_command_is_removed(self):
        with tempfile.TemporaryDirectory(prefix='zerus-relay-cli-') as directory:
            completed = zerus_mobile(directory, 'migrate-sqlite', '--source',
                                     str(Path(directory) / 'source.sqlite3'), '--source-offline')
            self.assertEqual(completed.returncode, 2, completed)
            self.assertIn("invalid choice: 'migrate-sqlite'", completed.stderr)
            self.assertNotIn('zerus-relay:', completed.stderr)
            self.assertEqual(list(Path(directory).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
