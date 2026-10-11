"""Exercise the checked-in workflow trust predicate and final success gate."""
import json
from itertools import takewhile
import os
from pathlib import Path
import re
import textwrap
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def job(text, name):
    match = re.search(rf"(?ms)^  {name}:\n(.*?)(?=^  [a-zA-Z][\w-]*:|\Z)", text)
    if not match:
        raise AssertionError(f"Missing workflow job: {name}")
    return match[1]


def field(text, name):
    match = re.search(rf"(?m)^    {name}: (.*)$", text)
    if not match:
        raise AssertionError(f"Missing job field: {name}")
    if match[1] == ">-":
        lines = text[match.end():].splitlines()[1:]
        return " ".join(line.strip() for line in takewhile(lambda line: line.startswith("      "), lines))
    return match[1]


class WorkflowSecurityTests(unittest.TestCase):
    def setUp(self):
        self.ci = (ROOT / ".github/workflows/ci.yml").read_text()

    def selected(self, name, event, ref="refs/heads/main", test_ref="", repository="ufna/zerus"):
        # These are the actual committed conditions, evaluated over synthetic
        # event fixtures. No user strings become executable test expressions.
        expression = field(job(self.ci, name), "if")
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"!(?!=)", "not ", expression)
        return bool(eval(expression, {"__builtins__": {}}, {
            "github": SimpleNamespace(repository=repository, ref=ref, event_name=event, sha="a" * 40),
            "inputs": SimpleNamespace(ref=test_ref),
        }))

    def test_only_exact_upstream_main_push_or_schedule_can_write_caches(self):
        fixtures = [
            ("push", "refs/heads/main", "", "ufna/zerus", True),
            ("schedule", "refs/heads/main", "a" * 40, "ufna/zerus", True),
            ("push", "refs/heads/main", "refs/pull/123/head", "ufna/zerus", False),
            ("pull_request", "refs/pull/123/merge", "", "ufna/zerus", False),
            ("pull_request_target", "refs/heads/main", "", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "refs/pull/123/head", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "b" * 40, "ufna/zerus", False),
            ("push", "refs/heads/feature", "", "ufna/zerus", False),
            ("push", "refs/heads/main", "", "example/zerus", False),
        ]
        for event, ref, test_ref, repository, trusted in fixtures:
            with self.subTest(event=event, ref=ref, test_ref=test_ref, repository=repository):
                self.assertEqual(self.selected("automatic", event, ref, test_ref, repository), trusted)
                self.assertEqual(self.selected("review", event, ref, test_ref, repository), not trusted)
        self.assertEqual(field(job(self.ci, "automatic"), "cache-mode"), "write")
        self.assertEqual(field(job(self.ci, "review"), "cache-mode"), "read")
        for name in ("automatic", "review"):
            self.assertEqual(field(job(self.ci, name), "uses"), "./.github/workflows/checks.yml")
        # A callee cannot request broader permissions than its caller's limit.
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        self.assertNotRegex(checks, r"(?m)^\s*cache-mode:")

    def test_final_gate_rejects_failure_cancellation_and_all_skipped(self):
        gate = job(self.ci, "gate")
        self.assertEqual(field(gate, "cache-mode"), "none")
        script = re.search(r"(?s)python3 - <<'PY'\n(.*?)\n          PY", gate)[1]
        code = compile(textwrap.dedent(script), "workflow-CI-gate", "exec")
        for first, second, success in [
            ("success", "skipped", True), ("skipped", "success", True),
            ("failure", "skipped", False), ("cancelled", "skipped", False),
            ("skipped", "skipped", False), ("success", "success", False),
            ("success", "failure", False),
        ]:
            results = {"automatic": {"result": first}, "review": {"result": second}}
            with self.subTest(results=results), patch.dict(os.environ, {"RESULTS": json.dumps(results)}):
                if success:
                    exec(code, {})
                else:
                    with self.assertRaises(SystemExit):
                        exec(code, {})

    def test_privileged_publishers_have_no_cache_access(self):
        for name in ("publish.yml", "publish-nightly.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                self.assertRegex(text, r"(?m)^cache-mode: none$")
                self.assertNotIn("actions/cache@", text)
                self.assertNotRegex(text, r"(?m)^    cache-mode:")

    def test_candidate_verification_has_a_read_only_reusable_chain(self):
        for name in ("release.yml", "nightly.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                verify = job(text, "verify")
                self.assertEqual(field(verify, "cache-mode"), "read")
                self.assertEqual(field(verify, "uses"), "./.github/workflows/checks.yml")
                checks = (ROOT / ".github/workflows/checks.yml").read_text()
                # GitHub validates nested permissions even for skipped jobs.
                # A candidate must never request publication write permissions.
                self.assertNotRegex(checks, r"(?m)^\s*contents:\s*write\s*$")
                self.assertNotIn("./.github/workflows/dev.yml", checks)
                self.assertIn("run_linux: true", verify)
                self.assertIn("run_arch: true", verify)
                self.assertIn("run_macos:", verify)


if __name__ == "__main__":
    unittest.main()
