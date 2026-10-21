"""Portable admission/accounting controls; no Windows API or process is used."""
import importlib.util
import io
from pathlib import Path
import re
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("windows_write_startup_causal", ROOT / "scripts/ci/windows-write-startup-causal.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


def fixed_document():
    root = ET.Element("testsuite")
    for name, method in sorted(gate.FIXED):
        ET.SubElement(root, "testcase", classname=name, name=method)
    return root


def red_document(row, condition=None):
    root = ET.Element("testsuite", tests="3", failures="1", errors="0", skipped="0")
    ET.SubElement(root, "testcase", classname="TestHgsClient", name="initTestCase")
    body = ET.SubElement(root, "testcase", classname="TestHgsClient", name=f"{gate.METHOD}({row})")
    message = f"'{condition or gate.CONDITIONS[row]}' returned FALSE. ()"
    ET.SubElement(body, "failure", type="fail", message=message)
    ET.SubElement(root, "testcase", classname="TestHgsClient", name="cleanupTestCase")
    return root, message


def raw(row, message, path=Path("fixture/test_hgsclient.cpp"), line=123):
    return f"FAIL!  : TestHgsClient::{gate.METHOD}({row}) {message}\n   Loc: [{path}({line})]\n".encode()


class Qualification(unittest.TestCase):
    def test_all_six_fixed_rows_are_required(self):
        self.assertEqual(gate.check_fixed([ET.tostring(fixed_document())]), sorted(gate.FIXED))

    def test_missing_or_duplicate_fixed_row_refuses(self):
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate):
                root = fixed_document()
                root.append(ET.fromstring(ET.tostring(root[0]))) if duplicate else root.remove(root[0])
                with self.assertRaises(RuntimeError):
                    gate.check_fixed([ET.tostring(root)])

    def test_failure_error_or_skip_is_not_fixed_pass(self):
        for kind in ("failure", "error", "skipped"):
            with self.subTest(kind=kind):
                root = fixed_document()
                ET.SubElement(root[0], kind)
                with self.assertRaises(RuntimeError):
                    gate.check_fixed([ET.tostring(root)])

    def test_three_exact_old_assertions_qualify(self):
        for row in gate.CONDITIONS:
            with self.subTest(row=row):
                root, message = red_document(row)
                answer = gate.check_red(ET.tostring(root), raw(row, message), row, 1, Path("fixture/test_hgsclient.cpp"), 123)
                self.assertEqual((answer["row"], answer["failures"], answer["errors"], answer["skips"]), (row, 1, 0, 0))

    def test_precondition_failure_never_qualifies_as_red(self):
        for condition in ("process", "QFileInfo(program).isExecutable()", "process->state() == QProcess::NotRunning"):
            with self.subTest(condition=condition):
                root, message = red_document("message", condition)
                with self.assertRaises(RuntimeError):
                    gate.check_red(ET.tostring(root), raw("message", message), "message", 1, Path("fixture/test_hgsclient.cpp"), 123)

    def test_other_row_assertion_does_not_qualify(self):
        root, message = red_document("message", gate.CONDITIONS["queue"])
        with self.assertRaises(RuntimeError):
            gate.check_red(ET.tostring(root), raw("message", message), "message", 1, Path("fixture/test_hgsclient.cpp"), 123)

    def test_exit_or_suite_errors_never_qualify_as_red(self):
        for status, attribute, value in ((0, "failures", "1"), (2, "failures", "1"), (1, "errors", "1"),
                                         (1, "skipped", "1"), (1, "failures", "0"), (1, "tests", "4")):
            with self.subTest(status=status, attribute=attribute):
                root, message = red_document("message")
                root.set(attribute, value)
                with self.assertRaises(RuntimeError):
                    gate.check_red(ET.tostring(root), raw("message", message), "message", status, Path("fixture/test_hgsclient.cpp"), 123)

    def test_setup_or_cleanup_failure_is_not_body_red(self):
        for index in (0, 2):
            with self.subTest(index=index):
                root, message = red_document("message")
                item = root[1].find("failure")
                root[1].remove(item)
                root[index].append(item)
                with self.assertRaises(RuntimeError):
                    gate.check_red(ET.tostring(root), raw("message", message), "message", 1, Path("fixture/test_hgsclient.cpp"), 123)

    def test_changed_location_or_missing_raw_provenance_refuses(self):
        root, message = red_document("message")
        for data in (b"", raw("message", message, line=124), raw("message", message, Path("elsewhere/test_hgsclient.cpp")),
                     raw("message", message) + raw("message", message)):
            with self.subTest(data=data):
                with self.assertRaises(RuntimeError):
                    gate.check_red(ET.tostring(root), data, "message", 1, Path("fixture/test_hgsclient.cpp"), 123)

    def test_archive_safe_source_and_exact_members(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = root / "source.tar"
            with tarfile.open(archive, "w") as output:
                item = tarfile.TarInfo("tray/file.cpp")
                item.size = 3
                output.addfile(item, io.BytesIO(b"abc"))
            destination = root / "source"
            destination.mkdir()
            gate.extract_archive(archive, destination, {"tray/file.cpp": {"bytes": 3, "sha256": gate.sha(b"abc")}}, float("inf"))
            self.assertEqual((destination / "tray/file.cpp").read_bytes(), b"abc")

    def test_archive_escape_link_duplicate_and_wrong_bytes_refuse(self):
        for name, kind, duplicate in (("../escape", "file", False), ("C:/escape", "file", False),
                                      ("tray/file.cpp", "link", False), ("tray/file.cpp", "file", True),
                                      ("tray/file.cpp", "file", False)):
            with self.subTest(name=name, kind=kind, duplicate=duplicate), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                archive = root / "source.tar"
                with tarfile.open(archive, "w") as output:
                    for _ in range(2 if duplicate else 1):
                        item = tarfile.TarInfo(name)
                        if kind == "link":
                            item.type, item.linkname = tarfile.SYMTYPE, "../escape"
                            output.addfile(item)
                        else:
                            item.size = 3
                            output.addfile(item, io.BytesIO(b"abc"))
                destination = root / "source"
                destination.mkdir()
                expected_sha = gate.sha(b"bad" if name == "tray/file.cpp" and kind == "file" and not duplicate else b"abc")
                with self.assertRaises(RuntimeError):
                    gate.extract_archive(archive, destination, {"tray/file.cpp": {"bytes": 3, "sha256": expected_sha}}, float("inf"))
                self.assertFalse((root / "escape").exists())

    def test_variant_full_file_pin_and_exact_three_replacements(self):
        message = (b"        // A failed start can be reported inside start(), before the caller has\n"
                   b"        // registered this request. Keep the claim synchronous, the result queued.\n"
                   b"        QTimer::singleShot(0, this, [this, request, host, name, detail, uncertain] {\n"
                   b"            emit messageFailed(request, host, name, detail, uncertain);\n"
                   b"        });")
        current = b"// fixed context\n" + message
        expected = b"// fixed context\n        emit messageFailed(request, host, name, detail, uncertain);"
        for name in ("queueSendFinished", "settingsFinished"):
            current += ("\n        if (*reported) return; *reported = true;\n"
                        "        QTimer::singleShot(0, this, [this, request, error] { emit " + name + "(request, false, {}, error); });").encode()
            expected += ("\n        if (*reported) return; *reported = true; emit " + name + "(request, false, {}, error);").encode()
        # Portable controls use an independent memory file. Unrelated future
        # product edits must not turn the one-time opt-in pin into a main gate.
        with patch.dict(gate.PINS, {gate.CLIENT: gate.sha(current)}), patch.object(gate, "BASELINE_CLIENT", gate.sha(expected)):
            self.assertEqual(gate.reverse_wrappers(current), expected)
            with self.assertRaises(RuntimeError):
                gate.reverse_wrappers(current + b"changed context")
            with patch.object(gate, "BASELINE_CLIENT", "0" * 64), self.assertRaises(RuntimeError):
                gate.reverse_wrappers(current)
            duplicated = current + message
            with patch.dict(gate.PINS, {gate.CLIENT: gate.sha(duplicated)}), self.assertRaises(RuntimeError):
                gate.reverse_wrappers(duplicated)

    def test_complete_baseline_mismatch_refuses(self):
        with self.assertRaises(RuntimeError):
            gate.reverse_wrappers(b"unreviewed product")

    def test_assertion_line_requires_bound_test_file(self):
        memory = (b"void actual() {\n    QCOMPARE(process->state(), QProcess::NotRunning);\n"
                  b"    QVERIFY(failed.isEmpty()); QVERIFY(queued.isEmpty()); QVERIFY(settings.isEmpty());\n}\n")
        with patch.dict(gate.PINS, {gate.TEST: gate.sha(memory)}):
            self.assertEqual(gate.assertion_line(memory), 3)
            with self.assertRaises(RuntimeError):
                gate.assertion_line(memory + b"changed")

    def test_private_environment_drops_session_and_secret_values(self):
        with tempfile.TemporaryDirectory() as folder:
            original = {"PATH": "fixture-tools", "SYSTEMROOT": "fixture-system", "QT_ROOT_DIR": "fixture-qt",
                        "VCINSTALLDIR": "fixture-vc", "HGS_SESSION": "foreign", "OPENAI_API_KEY": "do-not-inherit",
                        "GITHUB_TOKEN": "do-not-inherit", "CCACHE_DIR": "shared", "HOME": "foreign"}
            answer = gate.private_environment(Path(folder), original)
            for key in ("HGS_SESSION", "OPENAI_API_KEY", "GITHUB_TOKEN", "CCACHE_DIR"):
                self.assertNotIn(key, answer)
            self.assertEqual(answer["HOME"], str(Path(folder) / "home"))
            self.assertEqual(answer["ZERUS_BUILD_JOBS"], "2")
            self.assertEqual(answer["CCACHE_DISABLE"], "1")
            self.assertEqual(answer["VCINSTALLDIR"], "fixture-vc")

    def test_workflow_opt_in_is_pr_only_and_budget_unchanged(self):
        workflow = (ROOT / ".github/workflows/checks.yml").read_text()
        body = workflow.split("  windows-desktop:\n", 1)[1].split("\n  windows-", 1)[0]
        step = body.split("      - name: Opt-in desktop write-startup causal assertions\n", 1)[1].split("\n      - name:", 1)[0]
        expression = re.search(r"(?m)^        if: (.+)$", step)[1]
        expression = expression.replace("github.event_name", "event").replace("&&", "and").replace("||", "or")
        expression = expression.replace("contains(github.event.pull_request.labels.*.name, 'windows-write-startup-causal')", "'windows-write-startup-causal' in labels")
        marker = "<!-- zerus-write-startup-causal-v1 -->"
        expression = expression.replace("contains(github.event.pull_request.body, '" + marker + "')", "marker in (body or '')")
        for event, labels, body, expected in (("pull_request", ["windows-write-startup-causal"], None, True),
                                              ("pull_request", [], marker, True), ("pull_request", [], "unmarked", False),
                                              ("pull_request", [], None, False), ("push", ["windows-write-startup-causal"], marker, False),
                                              ("workflow_dispatch", [], marker, False), ("pull_request_target", [], marker, False)):
            self.assertEqual(bool(eval(expression, {"__builtins__": {}}, {"event": event, "labels": labels,
                                                                                "body": body, "marker": marker})), expected)
        self.assertIn("    timeout-minutes: 90\n", body)
        self.assertIn("        timeout-minutes: 8\n", step)
        self.assertLess(body.index("- name: Offscreen Qt suites"), body.index("- name: Opt-in desktop"))
        self.assertLess(body.index("- name: Opt-in desktop"), body.index("- name: Portable zip"))
        self.assertNotIn("secrets", step)
        self.assertNotIn("cache-mode", step)
        self.assertNotIn("permissions", step)


class Clock:
    def __init__(self):
        self.now = 0

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class Kernel:
    def __init__(self):
        self.events = []
        self.pending = None
        self.problems = []
        self.after_create = self.after_assign = self.after_resume = lambda: None
        self.empty_results = []
        self.wait_results = []

    def suspend(self, argv, cwd, environment, output):
        self.events.append("suspend")
        self.pending = SimpleNamespace(dwProcessId=123)
        self.after_create()
        return self.pending

    def assign(self, process):
        self.events.append("assign")
        self.after_assign()

    def resume(self, process):
        self.events.append("resume")
        self.after_resume()

    def wait(self, process, milliseconds):
        self.events.append("wait")
        return self.wait_results.pop(0) if self.wait_results else True

    def code(self, process):
        self.events.append("code")
        return 0

    def empty(self):
        self.events.append("empty")
        return self.empty_results.pop(0) if self.empty_results else True

    def terminate(self, process, assigned):
        self.events.append("terminate-job" if assigned else "terminate-process")

    def release(self, process):
        self.events.append("release")
        self.pending = None


class Custody(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.clock = Clock()
        self.kernel = Kernel()
        self.records = []

    def run_gate(self, persist=lambda: None):
        return gate.run_owned(self.kernel, ["fixed.exe"], self.root, {}, self.root / "log", 480, 20,
                              self.records, clock=self.clock, persist=persist, nap=self.clock.advance)

    def test_assignment_precedes_resume_and_normal_exit_never_signals(self):
        self.assertEqual(self.run_gate(), 0)
        self.assertEqual(self.kernel.events[:3], ["suspend", "assign", "resume"])
        self.assertEqual(self.kernel.events[-1], "release")
        self.assertNotIn("terminate-job", self.kernel.events)
        self.assertNotIn("terminate-process", self.kernel.events)
        self.assertEqual(self.records[0]["cleanup"], "exited-and-empty")

    def test_intent_is_saved_before_suspended_allocation(self):
        snapshots = []
        self.run_gate(lambda: snapshots.append((list(self.kernel.events), len(self.records))))
        self.assertEqual(snapshots[0], ([], 1))
        self.assertEqual(snapshots[1], (["suspend"], 1))

    def test_failed_intent_write_starts_nothing(self):
        def fail():
            raise OSError("private record failure")
        with self.assertRaises(OSError):
            self.run_gate(fail)
        self.assertEqual(self.kernel.events, [])

    def test_post_create_record_failure_retains_handle_and_never_resumes(self):
        writes = []
        def record():
            writes.append(True)
            if len(writes) == 2:
                raise OSError("private post-create record failure")
        with self.assertRaises(OSError):
            self.run_gate(record)
        self.assertNotIn("assign", self.kernel.events)
        self.assertNotIn("resume", self.kernel.events)
        self.assertEqual(self.kernel.events.count("terminate-process"), 1)
        self.assertIsNone(self.kernel.pending)

    def test_failed_assignment_cannot_resume_and_stops_exact_suspended_handle(self):
        def fail():
            raise OSError("nested job refused")
        self.kernel.after_assign = fail
        with self.assertRaises(OSError):
            self.run_gate()
        self.assertNotIn("resume", self.kernel.events)
        self.assertEqual(self.kernel.events.count("terminate-process"), 1)
        self.assertNotIn("terminate-job", self.kernel.events)
        self.assertEqual(self.kernel.events[-1], "release")

    def test_post_create_error_keeps_pending_handle_for_cleanup(self):
        def fail():
            raise OSError("after process creation")
        self.kernel.after_create = fail
        with self.assertRaises(OSError):
            self.run_gate()
        self.assertNotIn("resume", self.kernel.events)
        self.assertEqual(self.kernel.events.count("terminate-process"), 1)
        self.assertIsNone(self.kernel.pending)

    def test_late_assignment_never_resumes_or_renews_end(self):
        self.kernel.after_assign = lambda: self.clock.advance(20)
        with self.assertRaises(RuntimeError):
            self.run_gate()
        self.assertNotIn("resume", self.kernel.events)
        self.assertEqual(self.records[0]["end"], 20)
        self.assertEqual(self.kernel.events.count("terminate-job"), 1)

    def test_transient_accounting_settles_passively_under_same_end(self):
        self.kernel.empty_results = [False, True]
        self.assertEqual(self.run_gate(), 0)
        self.assertGreater(self.clock.now, 0)
        self.assertEqual(self.records[0]["end"], 20)
        self.assertNotIn("terminate-job", self.kernel.events)

    def test_expired_work_budget_allocates_no_process(self):
        self.clock.now = 421
        with self.assertRaises(RuntimeError):
            self.run_gate()
        self.assertEqual(self.kernel.events, [])

    def test_unknown_handle_cleanup_is_not_pass(self):
        release = self.kernel.release
        def uncertain(process):
            release(process)
            self.kernel.problems.append("unknown handle close")
        self.kernel.release = uncertain
        with self.assertRaises(RuntimeError):
            self.run_gate()
        self.assertEqual(self.records[0]["cleanup_unknown"], ["unknown handle close"])

    def test_wait_error_still_attempts_exact_owned_job_cleanup(self):
        wait = self.kernel.wait
        attempts = []
        def uncertain(process, milliseconds):
            attempts.append(True)
            if len(attempts) == 1:
                raise OSError("unknown wait")
            return wait(process, milliseconds)
        self.kernel.wait = uncertain
        with self.assertRaises(OSError):
            self.run_gate()
        self.assertEqual(self.kernel.events.count("terminate-job"), 1)
        self.assertEqual(self.kernel.events[-1], "release")

    def test_descendant_that_never_drains_cannot_renew_phase_end(self):
        stopped = []
        self.kernel.empty = lambda: bool(stopped)
        terminate = self.kernel.terminate
        def stop(process, assigned):
            terminate(process, assigned)
            stopped.append(True)
        self.kernel.terminate = stop
        def advance(seconds):
            self.clock.advance(10)
        with self.assertRaises(RuntimeError):
            gate.run_owned(self.kernel, ["fixed.exe"], self.root, {}, self.root / "log", 480, 20,
                           self.records, clock=self.clock, nap=advance)
        self.assertEqual(self.records[0]["end"], 20)
        self.assertEqual(self.kernel.events.count("terminate-job"), 1)
        self.assertEqual(self.kernel.events[-1], "release")

    def test_unknown_job_accounting_never_becomes_empty(self):
        def unknown():
            raise OSError("unknown job accounting")
        self.kernel.empty = unknown
        with self.assertRaises(OSError):
            self.run_gate()
        self.assertEqual(self.kernel.events[-1], "release")
        self.assertNotIn("cleanup", self.records[0])

    def test_late_final_close_is_not_success(self):
        release = self.kernel.release
        def late(process):
            release(process)
            self.clock.advance(480)
        self.kernel.release = late
        with self.assertRaises(RuntimeError):
            self.run_gate()
        self.assertEqual(self.records[0]["end"], 20)


if __name__ == "__main__":
    unittest.main()
