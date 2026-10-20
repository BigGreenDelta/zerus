#!/usr/bin/env python3
"""SDK-free admission/result/project controls; never count these as native UI results."""
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
