#!/usr/bin/env python3
"""SDK-free admission/result/project controls; never count these as native UI results."""
import importlib.util
import os
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class ApprovalHarnessTests(unittest.TestCase):
    def setUp(self):
        self.ui = module("android_approval_ui", "scripts/ci/android-project-ui.py")

    def output(self, ids=None, count="7", code="-1"):
        return (f"INSTRUMENTATION_RESULT: approval_cases={count}\n"
                f"INSTRUMENTATION_RESULT: approval_case_ids={ids or self.ui.CASES}\n"
                "INSTRUMENTATION_RESULT: stream=Approval choices smoke passed: synthetic only\n\n"
                f"INSTRUMENTATION_CODE: {code}\n")

    def test_original_project_suite_is_the_default(self):
        self.ui.select_suite([])
        self.assertEqual(self.ui.RUNNER, "app.zerus.mobile.SessionProjectSmokeInstrumentation")
        self.assertEqual(self.ui.RESULT_NAMESPACE, "project")
        self.assertEqual(len(self.ui.CASES.split(",")), 6)

    def test_approval_requires_all_seven_exact_cases_in_order(self):
        self.ui.select_suite(["--approval-choices"])
        self.assertEqual(self.ui.instrumentation_result(self.output())["approval_cases"], "7")
        for value in (self.output(count="6"), self.output(count="07"), self.output(ids=",".join(reversed(self.ui.CASES.split(",")))),
                      self.output(ids=self.ui.CASES + ",extra"), self.output(code="0"), self.output().replace("passed:", "failed:")):
            with self.subTest(value=value), self.assertRaises(AssertionError):
                self.ui.instrumentation_result(value)

    def test_project_result_cannot_qualify_an_approval_campaign(self):
        self.ui.select_suite(["--approval-choices"])
        with self.assertRaises(AssertionError):
            self.ui.instrumentation_result(self.output().replace("approval_cases", "project_cases"))
        self.assertEqual(self.ui.RUNNER, "app.zerus.mobile.ApprovalChoicesSmokeInstrumentation")
        self.assertIn("Lapp/zerus/mobile/QuestionUiKt;", self.ui.APP_CLASSES)

    def test_unknown_runner_and_extra_arguments_refuse_before_runtime(self):
        for arguments in (["other"], ["--approval-choices", "extra"], ["--approval-choices", "--approval-choices"]):
            with self.subTest(arguments=arguments), self.assertRaises(AssertionError):
                self.ui.select_suite(arguments)

    def test_android_runner_case_literal_matches_admitted_inventory(self):
        self.ui.select_suite(["--approval-choices"])
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        literal = re.search(r"val CASES = listOf\(([^\n]+)\)", source)[1]
        self.assertEqual(re.findall(r'"([a-z_]+)"', literal), self.ui.CASES.split(","))

    def test_simulator_project_references_exact_production_sources_and_local_package(self):
        ios = module("ios_approval_ui_project", "scripts/ci/ios-approval-ui-project.py")
        with tempfile.TemporaryDirectory() as folder:
            project = ios.generate(ROOT, Path(folder) / "new")
            body = (project / "project.pbxproj").read_text()
            self.assertIn(str(ROOT / "mobile/ios/Zerus/Conversation/QuestionCard.swift"), body)
            self.assertIn(str(ROOT / "mobile/ios/Zerus/Common/MarkdownView.swift"), body)
            self.assertIn(str(ROOT / "mobile/ios/ZerusKit"), body)
            self.assertNotIn("App/ZerusApp.swift", body)
            self.assertNotIn("App/AppModel.swift", body)
            self.assertNotIn("CODE_SIGNING_ALLOWED = YES", body)
            self.assertIn("CODE_SIGNING_ALLOWED = NO", body)
            self.assertIn("SUPPORTED_PLATFORMS = iphonesimulator", body)
            self.assertIn("ONLY_ACTIVE_ARCH = YES", body)
            scheme = ET.parse(project / "xcshareddata/xcschemes/ApprovalUIFixture.xcscheme").getroot()
            testable = scheme.findall("./TestAction/Testables/TestableReference")
            self.assertEqual(len(testable), 1)
            self.assertEqual(testable[0].get("skipped"), "NO")
            self.assertEqual(testable[0].find("BuildableReference").get("BlueprintName"), "ApprovalUIFixtureTests")
            with self.assertRaises(FileExistsError):
                ios.generate(ROOT, project.parent)

    def test_fixture_contains_seven_distinct_native_tests_and_no_live_client(self):
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        self.assertEqual(len(set(re.findall(r"func (test\w+)\(", source))), 7)
        app = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        self.assertNotIn("RelayClient(", app)
        self.assertNotIn("KeychainConnectionStore(", app)
        self.assertNotIn("URLSession(", app)

    def test_ios_result_requires_each_original_native_method_once(self):
        ios = module("ios_approval_ui", "scripts/ci/ios-approval-ui.py")
        raw = "\n".join(f"Test Case '-[ApprovalUIFixtureTests.ApprovalChoicesUITests {name}]' started.\n"
                        f"Test Case '-[ApprovalUIFixtureTests.ApprovalChoicesUITests {name}]' passed (3.123 seconds)."
                        for name in ios.CASES) + "\n** TEST SUCCEEDED **\n"
        self.assertEqual(len(ios.results(raw)["passed"]), 7)
        for broken in (raw.replace(ios.CASES[0], "testUnreviewed"), raw + raw,
                       raw.replace("passed (3.123 seconds).", "skipped (3.123 seconds).", 1),
                       raw.replace("TEST SUCCEEDED", "TEST FAILED")):
            with self.subTest(broken=broken), self.assertRaises(AssertionError):
                ios.results(broken)
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        self.assertEqual(set(re.findall(r"func (test\w+)\(", source)), set(ios.CASES))

    def test_only_the_exact_created_simulator_can_be_cleaned(self):
        ios = module("ios_approval_ui", "scripts/ci/ios-approval-ui.py")
        row = {"udid": "owned", "name": "fixture", "state": "Booted"}
        inventory = {"devices": {"runtime": [row]}}
        self.assertEqual(ios.owned_device(inventory, "runtime", "owned", "fixture"), row)
        for runtime, identifier, name in (("other-runtime", "owned", "fixture"), ("runtime", "other", "fixture"), ("runtime", "owned", "other")):
            with self.subTest(runtime=runtime, identifier=identifier, name=name), self.assertRaises(AssertionError):
                ios.owned_device(inventory, runtime, identifier, name)
        with self.assertRaises(AssertionError):
            ios.owned_device({"devices": {"runtime": [row, row]}}, "runtime", "owned", "fixture")

    def test_ios_pr_uses_exact_head_and_fixed_reviewed_inputs_without_manual_fallback(self):
        workflow = (ROOT / ".github/workflows/ios-approval-ui.yml").read_text()
        self.assertIn("pull_request:\n    branches: [main]", workflow)
        self.assertIn("github.event_name == 'pull_request' && github.event.pull_request.head.sha || inputs.source_sha", workflow)
        self.assertIn("ref: ${{ env.EXPECTED_SOURCE }}", workflow)
        for value, manual in (("Xcode_26.3.app", "xcode_application"), ("17C529", "xcode_build"),
                              ("com.apple.CoreSimulator.SimRuntime.iOS-26-2", "runtime_identifier"),
                              ("com.apple.CoreSimulator.SimDeviceType.iPhone-16", "device_type_identifier")):
            self.assertIn("github.event_name == 'pull_request' && '" + value + "' || inputs." + manual, workflow)
        for path in (".github/workflows/ios-approval-ui.yml", "mobile/ios/ApprovalUIFixture/**", "scripts/ci/ios-approval-ui*.py",
                     "tests/test_approval_ui_harness.py", "mobile/ios/Zerus/Conversation/QuestionCard.swift", "mobile/ios/Zerus/App/Theme.swift",
                     "mobile/ios/Zerus/Common/MarkdownView.swift", "mobile/ios/ZerusKit/**"):
            self.assertIn("      - '" + path + "'", workflow)
        self.assertIn("permissions:\n  contents: read", workflow)
        self.assertIn("cache-mode: none", workflow)
        self.assertIn('test "$RUNNER_ENVIRONMENT" = github-hosted', workflow)
        self.assertNotIn("secrets.", workflow)

    def test_non_arm64_process_refuses_before_allocation_or_tool_execution(self):
        ios = module("ios_approval_ui", "scripts/ci/ios-approval-ui.py")
        with patch.object(ios.sys, "platform", "darwin"), patch.dict(os.environ, {"RUNNER_ENVIRONMENT": "github-hosted"}, clear=True), \
                patch.object(ios.os, "uname", return_value=SimpleNamespace(machine="x86_64"), create=True), \
                patch.object(ios.Path, "mkdir") as allocate, patch.object(ios.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(AssertionError, "reviewed hosted process architecture"):
                ios.main()
            allocate.assert_not_called()
            launch.assert_not_called()

    def test_hosted_apple_environment_preserves_only_valid_required_account_values(self):
        ios = module("ios_approval_ui", "scripts/ci/ios-approval-ui.py")
        valid = {"HOME": "/tmp/example-home", "TMPDIR": "/private/tmp/example-hosted/", "USER": "hosted", "LOGNAME": "hosted"}
        self.assertEqual(ios.hosted_user_environment({**valid, "UNRELATED": "discarded"}), valid)
        for key in valid:
            missing = dict(valid); missing.pop(key)
            with self.subTest(missing=key), self.assertRaises(KeyError):
                ios.hosted_user_environment(missing)
            for value in ("", "bad\x00value", "bad\nvalue", "x" * 4097, None):
                with self.subTest(key=key, value=value), self.assertRaises(AssertionError):
                    ios.hosted_user_environment({**valid, key: value})
        for changes in ({"HOME": "relative"}, {"TMPDIR": "relative"}, {"USER": "other"},
                        {"LOGNAME": "other"}, {"USER": "bad/name", "LOGNAME": "bad/name"}):
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                ios.hosted_user_environment({**valid, **changes})
        source = (ROOT / "scripts/ci/ios-approval-ui.py").read_text()
        self.assertIn('run("runtimes", ["/usr/bin/xcrun", "simctl", "list", "runtimes", "--json"], 120)', source)
        self.assertIn('run("cleanup-identity", ["/usr/bin/xcrun", "simctl", "list", "devices", "--json"], 8, True)', source)
        self.assertIn("end = time.monotonic() + 900", source)
        self.assertIn("(0 if cleaning else 60)", source)

    def test_android_failure_diagnostics_stay_owned_bounded_and_cannot_replace_failure(self):
        self.ui.select_suite(["--approval-choices"])
        device = ["/fixture/adb", "-H", "127.0.0.1", "-P", "5037", "-s", self.ui.SERIAL, "shell"]
        calls = []
        def capture(name, command, cap):
            calls.append((name, command, cap))
        def live():
            calls.append("owned-alive")
        self.assertEqual(self.ui.failure_diagnostics(capture, live, device)["status"], "captured")
        self.assertEqual(calls, ["owned-alive", ("failure-logcat", [*device, "logcat", "-d", "-b", "crash", "-b", "main", "-b", "system", "-t", "400",
                                                                 "ZerusApprovalUI:I", "AndroidRuntime:E", "*:S"], 5)])
        for stage in ("ownership", "capture"):
            calls.clear()
            def refused():
                raise TimeoutError("same original end")
            def capture_refused(*arguments):
                calls.append("attempted-once"); raise OSError("diagnostic unavailable")
            result = self.ui.failure_diagnostics(capture_refused, refused if stage == "ownership" else live, device)
            self.assertEqual(result["status"], "unknown")
            self.assertEqual(result["error"], "TimeoutError" if stage == "ownership" else "OSError")
            self.assertEqual(calls, [] if stage == "ownership" else ["owned-alive", "attempted-once"])
        source = (ROOT / "scripts/ci/android-project-ui.py").read_text()
        self.assertIn('if RESULT_NAMESPACE == "approval" and instrumentation_started:', source)
        self.assertIn('diagnostics = failure_diagnostics(run, alive, device)\n        raise', source)
        self.assertIn('"primary_failure": primary_failure, "diagnostics": diagnostics', source)
        self.assertIn('end = time.monotonic() + 1200', source)
        self.assertIn('remaining = min(cap, end - time.monotonic() - 20)', source)
        self.assertIn('"app.zerus.mobile.test/" + RUNNER], 90)', source)
        with self.assertRaises(AssertionError):
            self.ui.instrumentation_result("INSTRUMENTATION_RESULT: shortMsg=Process crashed.\nINSTRUMENTATION_CODE: 0\n")
        fixture = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        expected = [(event, name) for name in self.ui.CASES.split(",") for event in ("begin", "complete")]
        self.assertEqual(re.findall(r'boundary\("(begin|complete)", "([a-z_]+)"\)', fixture), expected)
        self.assertEqual(re.findall(r'cases \+= "([a-z_]+)"', fixture), self.ui.CASES.split(","))

    def test_ios_fixture_launch_and_cleanup_stay_inside_each_main_actor_test(self):
        ios = module("ios_approval_ui", "scripts/ci/ios-approval-ui.py")
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        self.assertIn("@MainActor final class ApprovalChoicesUITests: XCTestCase", source)
        self.assertIn("@MainActor private func launchFixture()", source)
        self.assertNotIn("override func setUp", source)
        self.assertNotIn("override func tearDown", source)
        entries = re.findall(r"func (test\w+)\(\) \{\n        defer \{ app\?\.terminate\(\) \}\n        launchFixture\(\)", source)
        self.assertEqual(len(entries), 7)
        self.assertEqual(set(entries), set(ios.CASES))
        self.assertEqual(source.count("        launchFixture()"), 7)

    def test_android_interceptor_failure_is_first_only_and_cannot_qualify_or_use_network(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        self.assertIn("private val interceptorFailure = AtomicReference<InterceptorFailure?>(null)", source)
        self.assertIn("interceptorFailure.compareAndSet(null, failure)", source)
        self.assertNotIn("interceptorFailure.set(", source)
        self.assertNotIn("chain.proceed(", source)
        self.assertNotIn("request.url.query", source)
        catch = source.split("            } catch (error: Throwable) {", 1)[1].split("        }.build()", 1)[0]
        self.assertIn("request.method.take(16), request.url.encodedPath.take(160)", catch)
        self.assertNotIn("request.body", catch)
        self.assertNotIn('header("Authorization")', catch)
        self.assertIn('.code(500).message("Synthetic fixture refusal")', catch)
        self.assertIn("            smoke(store)\n            requireHealthyFixture()\n            check(cases == CASES)", source)
        wait = source.split("    private fun await(", 1)[1].split("    private fun find(", 1)[0]
        self.assertEqual(wait.count("requireHealthyFixture()"), 3)
        self.assertLess(wait.index("requireHealthyFixture()"), wait.index("while (!condition())"))
        self.assertGreater(wait.rindex("requireHealthyFixture()"), wait.index("SystemClock.sleep(30)"))
        self.assertEqual(re.findall(r'(?:var guard|guard) = "([a-z-]+)"', source), [
            "request-scope", "authorization", "request-json", "operation", "answer-admission", "answer-payload",
            "answer-target", "answer-receipt-identity", "answer-question-identity", "answer-list", "answer-body", "receipt"])

    def test_android_native_text_search_keeps_exact_matches_refresh_fallback_and_real_touch(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        self.assertIn("root.findAccessibilityNodeInfosByText(label).orEmpty().firstOrNull { it.refresh() && exactVisible(it, label) }", source)
        self.assertIn("node.isVisibleToUser &&\n        (node.text?.toString() == label || node.contentDescription?.toString() == label)", source)
        self.assertIn("return find(root) { exactVisible(it, label) }", source)
        node = source.split("    private fun node(", 1)[1].split("    private fun button(", 1)[0]
        self.assertEqual(node.count("visible(label)"), 1)
        self.assertIn("return checkNotNull(matched)", node)
        self.assertIn("check(current.refresh())", source)
        self.assertIn("check(action.isEnabled && action.isClickable)", source)
        self.assertIn("check(uiAutomation.injectInputEvent(event, true))", source)
        for phase in ("open-begin:$name", "open-ready:$name", "touch-down:$label", "touch-up:$label",
                      "disabled-assert-begin:countdown", "disabled-assert-end:countdown"):
            self.assertIn('phase("' + phase + '")', source)
        self.assertIn('check(!button("Send answer").isEnabled && count() == 0)', source)
        self.assertNotIn("setReviewDelayForTests", source)


if __name__ == "__main__":
    unittest.main()
