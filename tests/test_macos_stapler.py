"""CloudKit transport recovery must never accept an invalid or unverified ticket."""
import io
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import macos_stapler as stapler  # noqa: E402


class TicketOperations(unittest.TestCase):
    def test_both_operations_retry_only_the_same_command_and_return_success(self):
        for action in ("staple", "validate"):
            for output in ("stdout", "stderr"):
                with self.subTest(action=action, output=output):
                    command = ["xcrun", "stapler", action, "/fixture/Zerus.app"]
                    lost = subprocess.CompletedProcess(command, 68, "", "")
                    setattr(lost, output, "NSURLErrorDomain Code=-1005")
                    accepted = subprocess.CompletedProcess(command, 0, "Validated", "")
                    with mock.patch.object(stapler.subprocess, "run", side_effect=[lost, lost, accepted]) as run, \
                            mock.patch.object(stapler.time, "sleep") as sleep, \
                            mock.patch("sys.stderr", new_callable=io.StringIO):
                        self.assertIs(stapler.run_stapler(action, "/fixture/Zerus.app"), accepted)
                    self.assertEqual(run.call_args_list, [mock.call(command, capture_output=True, text=True)] * 3)
                    self.assertEqual(sleep.call_args_list, [mock.call(5), mock.call(15)])

    def test_exhaustion_and_permanent_failures_remain_failures(self):
        for action in ("staple", "validate"):
            for code, message, attempts in ((68, "NSURLErrorDomain Code=-1005", 5),
                                            (68, "Record not found", 1),
                                            (65, "NSURLErrorDomain Code=-1005", 1),
                                            (65, "Invalid ticket", 1)):
                with self.subTest(action=action, code=code, message=message):
                    failed = subprocess.CompletedProcess([], code, message, "")
                    with mock.patch.object(stapler.subprocess, "run", return_value=failed) as run, \
                            mock.patch.object(stapler.time, "sleep") as sleep, \
                            mock.patch("sys.stderr", new_callable=io.StringIO):
                        self.assertIs(stapler.run_stapler(action, "/fixture/Zerus.dmg"), failed)
                    self.assertEqual(run.call_count, attempts)
                    self.assertEqual(sleep.call_count, attempts - 1)

    def test_cli_propagates_failed_validation(self):
        with mock.patch.object(sys, "argv", ["stapler", "validate", "/fixture/Zerus.dmg"]), \
                mock.patch.object(stapler, "run_stapler", return_value=subprocess.CompletedProcess(
                    [], 65, "", "Invalid ticket")) as run, \
                mock.patch("sys.stdout", new_callable=io.StringIO), \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(stapler.main(), 65)
        run.assert_called_once_with("validate", "/fixture/Zerus.dmg")
