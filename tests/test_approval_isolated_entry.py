"""Run the real SDK-free entry points with Python's hosted isolated-mode flags."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ENTRIES = (
    "test_approval_ui_harness.py",
    "test_question_ui_probe_source.py",
    "test_question_availability_probe_source.py",
    "test_early_review_focus_wait_source.py",
)


class ApprovalIsolatedEntryTests(unittest.TestCase):
    def test_actual_entries_ignore_poisoned_pythonpath_without_discovery_recursion(self):
        with tempfile.TemporaryDirectory(prefix="zerus-isolated-entry-") as directory:
            scratch = Path(directory)
            marker = scratch / "unexpected-helper-import"
            (scratch / "review_focus_projection.py").write_text(
                "from pathlib import Path\n"
                f"Path({str(marker)!r}).write_text('wrong helper')\n"
                "raise AssertionError('PYTHONPATH helper must never load')\n"
            )
            env = {**os.environ, "PYTHONPATH": str(scratch),
                   "PYTHONHOME": str(scratch / "unavailable-python-home")}
            for name in ENTRIES:
                entry = ROOT / "tests" / name
                self.assertNotEqual(entry, Path(__file__).resolve())
                with self.subTest(entry=name):
                    result = subprocess.run([sys.executable, "-I", "-B", str(entry)],
                        cwd=scratch, env=env, capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertRegex(result.stderr, r"Ran [1-9][0-9]* tests? in ")
                    self.assertRegex(result.stderr, r"\nOK\s*\Z")
            self.assertFalse(marker.exists(), "A helper was imported through poisoned PYTHONPATH")


if __name__ == "__main__":
    unittest.main()
