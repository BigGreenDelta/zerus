#!/usr/bin/env python3
"""Authored SDK-free source contracts; not Kotlin or runtime qualification."""
import hashlib
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
# Fixture pin includes one-time post-resume live-label preparation; the approval harness retains exact 95ef by reverse projection.
MAIN = ROOT / "mobile/android/app/src/main/java/app/zerus/mobile"
# Product baseline includes prepared inspection publication before installation.
MODEL_SHA = "36d8d64399e3ed9dc54bc0815165c2d7d61d6b70ba33ed432682d6750bd0fd83"
MAIN_SHA = "aeadbc12421065f20ce51957a9504e071d65f3f85321153706ebdaf6e900fdfa"
WRITERS = ["INSTALL_WAIT_PRESENTATION", "INSTALL_PRESENTATION", "PREPARE_PRESENTATION", "TERMINAL_CLOSED",
           "ACTION_RESULT", "CONNECTION_FORGOTTEN", "OPEN_SESSION", "BACK", "CLEAR_UNCONFIRMED", "INSPECT_FAILED"]


def strip_availability(source):
    source = re.sub(r"^    // BEGIN QUESTION_AVAILABILITY_PROBE ([a-z-]+)\n[\s\S]*?^    // END QUESTION_AVAILABILITY_PROBE \1\n", "", source, flags=re.M)
    return re.sub(r" /\* BEGIN QUESTION_AVAILABILITY_PROBE (writer-[a-z-]+) \*/[\s\S]*?/\* END QUESTION_AVAILABILITY_PROBE \1 \*/", "", source)


class QuestionAvailabilityProbeSourceTests(unittest.TestCase):
    def test_original_model_and_main_reconstruct_including_every_policy_and_assignment(self):
        for name, digest in [("ZerusViewModel.kt", MODEL_SHA), ("MainActivity.kt", MAIN_SHA)]:
            source = (MAIN / name).read_text()
            self.assertEqual(hashlib.sha256(strip_availability(source).encode()).hexdigest(), digest)
            changed = source.replace("activityVerified || model.demo", "true", 1) if name == "MainActivity.kt" else source.replace("it.blocksSending", "false", 1)
            self.assertNotEqual(hashlib.sha256(strip_availability(changed).encode()).hexdigest(), digest)

    def test_each_existing_write_is_followed_by_its_fixed_reason_without_changing_rhs(self):
        source = (MAIN / "ZerusViewModel.kt").read_text()
        writes = re.findall(r"\bactivityVerified\s*=(?!=)\s*(false|verified|activeFrameVerified) /\* BEGIN QUESTION_AVAILABILITY_PROBE (writer-[a-z-]+) \*/; questionVerificationProbe\(QuestionUiProbe.VerificationWriter\.([A-Z_]+)\) /\* END QUESTION_AVAILABILITY_PROBE \2 \*/", source)
        self.assertEqual([w[2] for w in writes], WRITERS)
        self.assertEqual([w[0] for w in writes], ["false", "verified", "activeFrameVerified"] + ["false"] * 7)
        self.assertEqual(len(re.findall(r"\bactivityVerified\s*=(?!=)", strip_availability(source))), 10)

    def test_availability_inputs_are_captured_before_side_effect_and_preserve_the_actual_expression(self):
        model = (MAIN / "ZerusViewModel.kt").read_text().split("internal fun questionAvailabilityProbe(", 1)[1].split("// END QUESTION_AVAILABILITY_PROBE definitions", 1)[0]
        self.assertLess(model.index("if (!QuestionUiProbe.enabled) return null"), model.index("val verified = activityVerified"))
        self.assertIn("val blocked = contextBlocked(target)", model)
        for term in ("sessionActions.any { it.target == target && it.blocksSending }", "clearExpectation?.let { ContextPolicies.sameSessionRun(it.target, target) } == true", "contextOperations.any { ContextPolicies.sameSessionRun(it.target, target) && it.blocksSending }"):
            self.assertIn(term, model)
        self.assertIn("online && verified && !blocked", model)
        main = (MAIN / "MainActivity.kt").read_text().split("// BEGIN QUESTION_AVAILABILITY_PROBE committed", 1)[1].split("// END QUESTION_AVAILABILITY_PROBE committed", 1)[0]
        self.assertLess(main.index("if (QuestionUiProbe.enabled)"), main.index("model.questionAvailabilityProbe(target, online)"))
        self.assertIn("SideEffect { QuestionUiProbe.availability(target, observedAvailability) }", main)
        self.assertNotIn("model.", main.split("SideEffect", 1)[1])

    def test_model_observation_is_default_null_bounded_typed_and_catches_only_observer_work(self):
        model = (MAIN / "ZerusViewModel.kt").read_text().split("private fun questionVerificationProbe(", 1)[1].split("internal fun questionAvailabilityProbe(", 1)[0]
        self.assertLess(model.index("if (!QuestionUiProbe.enabled) return"), model.index("try {"))
        self.assertLess(model.index("try {"), model.index("val frame = activeFrame"))
        self.assertIn("catch (_: Throwable) { QuestionUiProbe.observationFailed() }", model)
        probe = (MAIN / "QuestionUiProbe.kt").read_text()
        observer = probe.split("private inline fun observeTarget(", 1)[1].split("    fun verification(", 1)[0]
        guard = "if (!BuildConfig.DEBUG || !owned.healthy()) return"
        self.assertLess(observer.index("val owned = session ?: return"), observer.index(guard))
        for work in ("SystemClock.uptimeMillis()", "target?.let", "fingerprint(identity)", "Record(", "detail()"):
            self.assertLess(observer.index(guard), observer.index(work))
        self.assertIn("catch (_: Throwable) { owned.failed() }", observer)
        self.assertIn("const val MAX_RECORDS = 256", probe)
        self.assertIn("if (records.size == MAX_RECORDS) { fault = Fault.OVERFLOW; return }", probe)
        self.assertIn("record.stage == Stage.AVAILABILITY_COMMIT && same(availability, record)", probe)
        self.assertIn("data class Verification(val writer: VerificationWriter", probe)
        for forbidden in ("ZerusViewModel", "JSONObject", "question.prompts", "choice.label", "java.io", "okhttp", "Thread(", "launch(", "delay("):
            self.assertNotIn(forbidden, probe)

    def test_question_ui_and_entire_physical_fixture_match_the_reviewed_baselines(self):
        self.assertEqual(hashlib.sha256((MAIN / "QuestionUi.kt").read_bytes()).hexdigest(),
                         "841c0d5b2df250497b0f54ef8854574ca386e83cb735dc1a0873096d719e22a2")
        fixture = ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt"
        # The fixture baseline includes the parent-selection and Compose role-child corrections;
        # physical case oracles stay exact.
        self.assertEqual(hashlib.sha256(fixture.read_bytes()).hexdigest(),
                         "73cee450f08e33c007ad6f3188a6a69135674e2503f80ea48582ba8aa394a9f7")

    def test_prepared_publication_uses_the_actual_bounded_owner_before_any_atomic_install(self):
        model = (MAIN / "ZerusViewModel.kt").read_text()
        normalized = model.split("private fun frameInstall(", 1)[1].split("private fun installFrame(", 1)[0]
        self.assertNotIn("currentHistoryHeads-=", normalized)
        self.assertNotRegex(normalized, r"historyWindows\[[^\n]+\]\s*=")
        body = model.split("val result = InspectionPublication.prepare(", 1)[1].split("if (result == InspectionPublication.Result.SUPERSEDED)", 1)[0]
        for guard in ("activeFrame === preparationOwner", "clearExpectation == clearOwner", "messageState === captured.state",
                      "presentationRevision == captured.revision", "historyWindows[frame.target.key] === captured.window",
                      "outgoing.filter { it.belongsTo(frame.target) } == captured.messages",
                      "selected === captured.selection", "catalogSessions === captured.catalog",
                      "historyComplete == captured.complete", "historyIndexing == captured.indexing"):
            self.assertIn(guard, body)
        self.assertIn("withContext(Dispatchers.Default)", body)
        self.assertIn("Snapshot.withMutableSnapshot", body)
        self.assertLess(body.index("captured.install.frame.presentation = prepared"), body.index("installFrame(frame, true, captured.install)"))
        self.assertIn("if (result == InspectionPublication.Result.SUPERSEDED) return@launch", model)
        self.assertIn("check(result == InspectionPublication.Result.PUBLISHED)", model)
        helper = (MAIN / "InspectionPublication.kt").read_text()
        self.assertIn("const val MAX_ATTEMPTS = 3", helper)
        self.assertIn("repeat(MAX_ATTEMPTS)", helper)
        self.assertEqual(helper.count("currentCoroutineContext().ensureActive()"), 2)
        self.assertLess(helper.index("if (!owns()) return Result.SUPERSEDED", helper.index("val prepared = build(snapshot)")), helper.index("publish(snapshot, prepared)"))
        for forbidden in ("delay(", "launch(", "async(", "api.", "installFrame(", "while ("):
            self.assertNotIn(forbidden, helper)


if __name__ == "__main__":
    unittest.main()
