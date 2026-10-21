#!/usr/bin/env python3
"""SDK-free preservation checks, not Kotlin compilation or Compose/native evidence."""
import hashlib
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = "mobile/android/app/src/main/java/app/zerus/mobile/"
FIXTURE = "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt"
ORIGINAL_UI = "64b2090f4bc155e8c574c8e206dfeef9e33ccb2b955924ed0d743e418027352d"
ORIGINAL_FIXTURE = "36a5575726a3d6bec552fed168a034516a7ba3d9c8ee7b97a6840f7b56204420"


def strip_probe(source, expected):
    pattern = re.compile(r"^([ ]*)// BEGIN QUESTION_UI_PROBE ([a-z-]+)\n[\s\S]*?^\1// END QUESTION_UI_PROBE \2\n", re.M)
    names = [match[2] for match in pattern.finditer(source)]
    if names != expected:
        raise ValueError("unexpected probe seams")
    return pattern.sub("", source)


def original_ui(source):
    return hashlib.sha256(strip_probe(source, ["entry", "request", "committed", "showing"]).encode()).hexdigest() == ORIGINAL_UI


def original_fixture(source):
    source = strip_probe(source, ["fixture-report"])
    flat = r' | ${actionProbeEvidence().replace("\n", " | ")}'
    raw = r'\n${actionProbeEvidence()}'
    if source.count("    private fun smokeBody(store: PrivateStore) {\n") != 1 or source.count(flat) != 1 or source.count(raw) != 1:
        raise ValueError("unexpected fixture edits")
    source = source.replace("    private fun smokeBody(store: PrivateStore) {\n", "    private fun smoke(store: PrivateStore) {\n")
    source = source.replace(flat, "").replace(raw, "")
    return hashlib.sha256(source.encode()).hexdigest() == ORIGINAL_FIXTURE


class QuestionUiProbeSourceTests(unittest.TestCase):
    def test_pass_serializer_keeps_multiline_probe_inside_one_field_for_the_actual_parser(self):
        source = (ROOT / FIXTURE).read_text()
        lines = [line for line in source.splitlines() if 'result.putString("stream", "Approval choices smoke passed:' in line]
        self.assertEqual(len(lines), 1)
        flat = r' | ${actionProbeEvidence().replace("\n", " | ")}'
        old = lines[0].replace(flat, r'\n${actionProbeEvidence()}')
        self.assertNotEqual(old, lines[0])
        spec = importlib.util.spec_from_file_location("probe_pass_parser", ROOT / "scripts/ci/android-project-ui.py")
        parser = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(parser)
        parser.select_suite(["--approval-choices"])
        cases = parser.CASES.split(",")
        self.assertEqual(len(cases), 7)
        probe = "compose-action status=observed closed=true fault=NONE records=1 schema=1\ncompose-action Record(stage=ENTRY, synthetic)"
        def serialized(line):
            prefix, suffix = '            result.putString("stream", "', '")'
            self.assertTrue(line.startswith(prefix) and line.endswith(suffix))
            template = line[len(prefix):-len(suffix)]
            replacements = {r'${cases.joinToString(",")}': ",".join(cases),
                r'${touchResultEvidence().replace("\n", " | ")}': "owned-touch observed | restored=true",
                r'${actionProbeEvidence().replace("\n", " | ")}': probe.replace("\n", " | "),
                r'${actionProbeEvidence()}': probe}
            for expression, value in replacements.items():
                template = template.replace(expression, value)
            self.assertNotIn("${", template)
            stream = template.replace(r"\n", "\n")
            return ("INSTRUMENTATION_RESULT: approval_cases=7\nINSTRUMENTATION_RESULT: approval_case_ids=" + ",".join(cases) +
                "\nINSTRUMENTATION_RESULT: stream=" + stream + "\nINSTRUMENTATION_CODE: -1\n")
        with self.assertRaises(AssertionError):
            parser.instrumentation_result(serialized(old))
        actual = parser.instrumentation_result(serialized(lines[0]))
        self.assertEqual(actual["approval_cases"], "7")
        self.assertEqual(actual["approval_case_ids"], parser.CASES)
        self.assertIn(" | compose-action Record(", actual["stream"])
        self.assertIn(r'\n${actionProbeEvidence()}\n")', source)  # Original FAIL stream remains raw.

    def test_original_actions_guards_bindings_and_seven_case_oracles_reconstruct_exactly(self):
        ui = (ROOT / MAIN / "QuestionUi.kt").read_text()
        fixture = (ROOT / FIXTURE).read_text()
        self.assertTrue(original_ui(ui))
        self.assertTrue(original_fixture(fixture))
        # A changed delegate/guard/oracle outside the observation cannot pass this admission.
        for old, new in [("::primaryAction", "{ primaryAction() }"), ("else if (reviewed)", "else if (true)"),
                         ("Button(onClick = onClick", "Button(onClick = {}")]:
            self.assertIn(old, ui)
            self.assertFalse(original_ui(ui.replace(old, new, 1)))
        self.assertFalse(original_fixture(fixture.replace('cases == CASES', 'cases.size <= CASES.size', 1)))

    def test_entry_keeps_exact_original_free_variables_and_commit_reads_before_side_effect(self):
        ui = (ROOT / MAIN / "QuestionUi.kt").read_text()
        self.assertIn("if (QuestionUiProbe.enabled) QuestionUiProbe.entry(question, options, step.nextPage != null, step.canAdvance, editing, submitPending, choices, reviewed, broader.open)", ui)
        self.assertIn("if (QuestionUiProbe.enabled) QuestionUiProbe.requested(question, options, confirmation)", ui)
        committed = ui.split("// BEGIN QUESTION_UI_PROBE committed\n", 1)[1].split("// END QUESTION_UI_PROBE committed", 1)[0]
        self.assertLess(committed.index("val committed = QuestionUiProbe.Committed("), committed.index("SideEffect"))
        self.assertIn("SideEffect { QuestionUiProbe.committed(question, committedOptions, committed) }", committed)
        self.assertNotIn("remember", committed)
        self.assertNotIn("DisposableEffect", committed)
        self.assertEqual(ui.count("::primaryAction"), 2)

    def test_inactive_observer_returns_before_any_clock_fingerprint_record_or_detail(self):
        probe = (ROOT / MAIN / "QuestionUiProbe.kt").read_text()
        observe = probe.split("private inline fun observe(", 1)[1].split("    fun entry(", 1)[0]
        guard = "if (!BuildConfig.DEBUG || !owned.healthy()) return"
        self.assertLess(observe.index("val owned = session ?: return"), observe.index(guard))
        for work in ("SystemClock.uptimeMillis()", "fingerprint(", "selection(options)", "Record(", "detail()"):
            self.assertLess(observe.index(guard), observe.index(work))
        self.assertIn("@Volatile private var session: Session? = null", probe)
        self.assertIn("check(BuildConfig.DEBUG && session == null)", probe)
        self.assertIn("check(session === owned)", probe)

    def test_recorder_is_bounded_typed_and_catches_only_its_own_observation(self):
        probe = (ROOT / MAIN / "QuestionUiProbe.kt").read_text()
        self.assertIn("const val MAX_RECORDS = 256", probe)
        self.assertIn("if (records.size == MAX_RECORDS) { fault = Fault.OVERFLOW; return }", probe)
        self.assertIn("catch (_: Throwable) { owned.failed() }", probe)
        self.assertIn("parts.size <= 40", probe)
        self.assertIn("options.size <= 4", probe)
        self.assertIn("it.size <= 8", probe)
        self.assertIn("it.length <= 256", probe)
        self.assertIn('MessageDigest.getInstance("SHA-256")', probe)
        self.assertIn("left.selection == right.selection && left.detail == right.detail", probe)
        self.assertNotIn("left.uptimeMs", probe)
        # The diagnostic has no delegate/model/action callback, raw text field or IO/network facility.
        for forbidden in ("submitAnswer(", ".request(", ".send(", "ZerusViewModel", "Context", "java.io", "okhttp", "question.prompts", "choice.label", "choice.description", "() -> Unit"):
            self.assertNotIn(forbidden, probe)

    def test_fixture_closes_owned_session_and_keeps_original_exception_and_result_schema(self):
        source = (ROOT / FIXTURE).read_text()
        report = source.split("// BEGIN QUESTION_UI_PROBE fixture-report\n", 1)[1].split("// END QUESTION_UI_PROBE fixture-report", 1)[0]
        self.assertIn("val owned = QuestionUiProbe.install()", report)
        self.assertIn("try { smokeBody(store) }", report)
        self.assertIn("catch (error: Throwable) { primary = error; throw error }", report)
        self.assertIn("finally {", report)
        self.assertIn("main { QuestionUiProbe.close(owned) }", report)
        self.assertIn("original.addSuppressed(cleanup)", report)
        self.assertIn('else "UNKNOWN"', report)
        self.assertIn("it.length <= 1024", report)
        self.assertIn("it.length <= 512 * 1024", report)
        self.assertTrue(original_fixture(source))


if __name__ == "__main__":
    unittest.main()
