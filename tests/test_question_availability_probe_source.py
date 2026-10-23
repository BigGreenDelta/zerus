#!/usr/bin/env python3
"""Authored SDK-free source contracts; not Kotlin or runtime qualification."""
import hashlib
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
_focus_spec = importlib.util.spec_from_file_location("review_focus_projection", ROOT / "tests/review_focus_projection.py")
_focus_projection = importlib.util.module_from_spec(_focus_spec)
_focus_spec.loader.exec_module(_focus_projection)
original_review_fixture = _focus_projection.original_review_fixture
# Fixture pin includes one-time post-resume live-label preparation; the approval harness retains exact 95ef by reverse projection.
MAIN = ROOT / "mobile/android/app/src/main/java/app/zerus/mobile"
# Keep the reviewed 9b217ddd/b9c1e29c reconstruction pins unchanged. The exact
# fixtures first reverse the composed 3d33de39 workspace changes to ac601e36,
# then the 962699e6 read-catalog/badge update and c1eebe96 mobile checkout
# additions. None changes question eligibility or the ten verification writes.
# No Git history or Android SDK is needed at run time.
MODEL_SHA = "a94c04695da3626c49f1cc5a875c82f8b3893688d256c7cfa3ebf66cad9e619a"
MAIN_SHA = "7c676d33a2218da83511697f5a597c4876b946e1c130d601c265e855b7259f4b"
UPDATES = ROOT / "tests/fixtures/question-availability-reviewed-updates.patch"
UPDATES_SHA = "5088def68877668c9a7e261851d552cd30b0fee1806aa072a911172f6ff4f60a"
WORKSPACE_UPDATES = ROOT / "tests/fixtures/question-availability-workspace-updates.patch"
WORKSPACE_UPDATES_SHA = "02c52292e8713aa238f159054907c41a8bc9f605860dde2d38575dd379ca06fd"
WORKSPACE_BASE_SHA = {
    "ZerusViewModel.kt": "5932d92ca74ccf37da9595a580bcb1457c93c83951976606c8defa8d1ee83326",
    "MainActivity.kt": "f68902349d1e0ac939582e9e139548663f5763c430a3c566f8823d9f045645f0",
}
WRITERS = ["INSTALL_WAIT_PRESENTATION", "INSTALL_PRESENTATION", "PREPARE_PRESENTATION", "TERMINAL_CLOSED",
           "ACTION_RESULT", "CONNECTION_FORGOTTEN", "OPEN_SESSION", "BACK", "CLEAR_UNCONFIRMED", "INSPECT_FAILED"]


def strip_availability(source):
    source = re.sub(r"^    // BEGIN QUESTION_AVAILABILITY_PROBE ([a-z-]+)\n[\s\S]*?^    // END QUESTION_AVAILABILITY_PROBE \1\n", "", source, flags=re.M)
    return re.sub(r" /\* BEGIN QUESTION_AVAILABILITY_PROBE (writer-[a-z-]+) \*/[\s\S]*?/\* END QUESTION_AVAILABILITY_PROBE \1 \*/", "", source)


def reverse_reviewed_patch(name, source, fixture, fixture_sha):
    """Reverse exact reviewed hunks, never an arbitrary function/line range."""
    raw = fixture.read_bytes()
    if hashlib.sha256(raw).hexdigest() != fixture_sha:
        raise AssertionError("reviewed source-update fixture changed")
    prefix = "mobile/android/app/src/main/java/app/zerus/mobile/"
    files = {}
    for section in raw.decode().split("diff --git ")[1:]:
        header, body = section.split("\n", 1)
        before, after = header.split()
        filename = before.removeprefix("a/" + prefix)
        if (filename not in ("ZerusViewModel.kt", "MainActivity.kt")
                or before != "a/" + prefix + filename or after != "b/" + prefix + filename
                or filename in files):
            raise AssertionError("unexpected source-update fixture path")
        hunks = []
        for hunk in re.split(r"(?m)^@@[^\n]*\n", body)[1:]:
            lines = hunk.splitlines(keepends=True)
            if not lines or any(line[:1] not in (" ", "+", "-") for line in lines):
                raise AssertionError("unsupported source-update fixture hunk")
            old = "".join(line[1:] for line in lines if line[0] in " -")
            new = "".join(line[1:] for line in lines if line[0] in " +")
            if not old or not new or old == new:
                raise AssertionError("empty source-update fixture hunk")
            hunks.append((old, new))
        if not hunks:
            raise AssertionError("missing source-update fixture hunks")
        files[filename] = hunks
    if set(files) != {"ZerusViewModel.kt", "MainActivity.kt"} or name not in files:
        raise AssertionError("incomplete source-update fixture")
    for old, new in files[name]:
        if source.count(new) != 1:
            raise AssertionError("reviewed source-update hunk is missing, changed or duplicated")
        source = source.replace(new, old, 1)
    return source


def reverse_reviewed_updates(name, source):
    source = reverse_reviewed_patch(name, source, WORKSPACE_UPDATES, WORKSPACE_UPDATES_SHA)
    return reverse_reviewed_patch(name, source, UPDATES, UPDATES_SHA)


def reconstructed(name, source):
    return strip_availability(reverse_reviewed_updates(name, source))


def original_source(name, source):
    try:
        digest = hashlib.sha256(reconstructed(name, source).encode()).hexdigest()
    except AssertionError:
        return False
    return digest == {"ZerusViewModel.kt": MODEL_SHA, "MainActivity.kt": MAIN_SHA}[name]


class QuestionAvailabilityProbeSourceTests(unittest.TestCase):
    def test_workspace_projection_matches_reviewed_current_main_exactly(self):
        for name, digest in WORKSPACE_BASE_SHA.items():
            with self.subTest(source=name):
                projected = reverse_reviewed_patch(name, (MAIN / name).read_text(),
                                                  WORKSPACE_UPDATES, WORKSPACE_UPDATES_SHA)
                self.assertEqual(hashlib.sha256(projected.encode()).hexdigest(), digest)

    def test_original_model_and_main_reconstruct_including_every_policy_and_assignment(self):
        for name, digest in [("ZerusViewModel.kt", MODEL_SHA), ("MainActivity.kt", MAIN_SHA)]:
            source = (MAIN / name).read_text()
            self.assertEqual(hashlib.sha256(reconstructed(name, source).encode()).hexdigest(), digest)
            changed = source.replace("activityVerified || model.demo", "true", 1) if name == "MainActivity.kt" else source.replace("it.blocksSending", "false", 1)
            self.assertFalse(original_source(name, changed))

    def test_reverse_updates_keep_every_question_policy_and_verification_assignment_protected(self):
        source = (MAIN / "ZerusViewModel.kt").read_text()
        for expression in (
                "sessionActions.any { it.target == target && it.blocksSending }",
                "clearExpectation?.let { ContextPolicies.sameSessionRun(it.target, target) } == true",
                "contextOperations.any { ContextPolicies.sameSessionRun(it.target, target) && it.blocksSending }"):
            with self.subTest(policy=expression):
                self.assertIn(expression, source)
                self.assertFalse(original_source("ZerusViewModel.kt", source.replace(expression, "false", 1)))
        writes = list(re.finditer(r"\bactivityVerified\s*=(?!=)\s*(false|verified|activeFrameVerified)(?= /\* BEGIN QUESTION_AVAILABILITY_PROBE writer-)", source))
        self.assertEqual(len(writes), len(WRITERS))
        for writer, match in zip(WRITERS, writes):
            with self.subTest(writer=writer):
                changed = source[:match.start(1)] + "true" + source[match.end(1):]
                self.assertFalse(original_source("ZerusViewModel.kt", changed))
        main = (MAIN / "MainActivity.kt").read_text()
        for expression in ("model.activityVerified || model.demo",
                           "online && model.activityVerified && !model.contextBlocked(target)"):
            self.assertIn(expression, main)
            self.assertFalse(original_source("MainActivity.kt", main.replace(expression, "true", 1)))

    def test_reverse_updates_refuse_changed_missing_or_duplicate_reviewed_hunks(self):
        source = (MAIN / "ZerusViewModel.kt").read_text()
        # These are reviewed additions, not probe markers or protected old policy.
        for old, new in (("RepositoryAcquisition.statusPayload(stored,id)", "JSONObject()"),
                         ("val loaded=loadedReadRows()", "val loaded=emptyMap<Target,List<Event>>()"),
                         ('    fun openLaunch() {\n', ''),
                         ('    fun openLaunch() {\n', '    fun openLaunch() {\n    fun openLaunch() {\n')):
            with self.subTest(update=old, replacement=new):
                self.assertIn(old, source)
                self.assertFalse(original_source("ZerusViewModel.kt", source.replace(old, new, 1)))
        main = (MAIN / "MainActivity.kt").read_text()
        self.assertFalse(original_source("MainActivity.kt", main.replace("!SharedReadPolicies.canShowReplyBadge(session)", "true", 1)))

    def test_workspace_projection_refuses_changed_missing_or_duplicate_reviewed_hunks(self):
        model = (MAIN / "ZerusViewModel.kt").read_text()
        for addition in (
                "    private val api = RelayApi(workspace = store.workspaceJournal)\n",
                "    var pendingEncryptedPairings by mutableStateOf<Set<String>>(emptySet());private set\n",
                "    fun resumeEncryptedPair(connection:Connection)=viewModelScope.launch {\n",
                "                        WorkspaceCatalogCache.preserve(connection,computers,\n",
                "                sessions = preparedSessions; machines = newMachines.map { MachineNames.apply(it,messageState.machineAliases) }; machineOperations = newOperations; machineFeatures = newFeatures\n"):
            for changed in (addition.rstrip("\n") + " // changed reviewed addition\n", "", addition + addition):
                with self.subTest(addition=addition, replacement=changed):
                    self.assertIn(addition, model)
                    self.assertNotEqual(addition, changed)
                    self.assertFalse(original_source("ZerusViewModel.kt", model.replace(addition, changed, 1)))
        main = (MAIN / "MainActivity.kt").read_text()
        for addition in ("Messages are encrypted between enrolled devices.",
                         "This erases this phone's connection keys."):
            for changed in ("Changed reviewed text.", "", addition + addition):
                with self.subTest(addition=addition, replacement=changed):
                    self.assertIn(addition, main)
                    self.assertFalse(original_source("MainActivity.kt", main.replace(addition, changed, 1)))

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
        self.assertEqual(hashlib.sha256(original_review_fixture().encode()).hexdigest(),
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
