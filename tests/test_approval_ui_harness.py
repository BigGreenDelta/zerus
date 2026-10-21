#!/usr/bin/env python3
"""SDK-free admission/result/project controls; never count these as native UI results."""
import importlib.util
import ast
import hashlib
import json
import math
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


def finite_source_predicate(expression, values):
    """Finite source-expression stand-in only; never Kotlin/Swift compilation or native API evidence."""
    tree = ast.parse(expression.strip(), mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not, ast.Compare,
               ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE, ast.Name, ast.Load, ast.Constant)
    assert all(isinstance(node, allowed) for node in ast.walk(tree))
    assert {node.id for node in ast.walk(tree) if isinstance(node, ast.Name)} <= values.keys()
    return eval(compile(tree, "<finite-source-predicate>", "eval"), {"__builtins__": {}}, values)


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

    def test_notification_header_is_fixed_fixture_only_and_bound_in_both_unsigned_targets(self):
        ios = module("ios_approval_ui_project", "scripts/ci/ios-approval-ui-project.py")
        header = "mobile/ios/ApprovalUIFixture/ApprovalUIFixture-Bridging-Header.h"
        raw = (ROOT / header).read_bytes()
        self.assertEqual(raw.decode().splitlines(), [
            "// Public Apple notification declarations for the disposable fixture and its UI tests only.",
            "#include <notify.h>"])
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "new"
            project = ios.generate(ROOT, output)
            body = (project / "project.pbxproj").read_text()
            declaration = 'SWIFT_OBJC_BRIDGING_HEADER = ' + json.dumps(str(ROOT / header)) + ';'
            self.assertEqual(body.count(declaration), 2)
            for config in ("A00000000000000000000017", "A00000000000000000000018"):
                settings = body.split(config + " = { isa = XCBuildConfiguration;", 1)[1].split("\n", 1)[0]
                self.assertIn(declaration, settings)
                self.assertIn("CODE_SIGNING_ALLOWED = NO", settings)
            identities = json.loads((output / "source-identities.json").read_text())["sources"]
            self.assertEqual(identities[header], hashlib.sha256(raw).hexdigest())
            self.assertNotIn("@_silgen_name", body)

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
        self.assertIn("@MainActor private func launchFixture(_ mode: Mode = .normal)", source)
        self.assertNotIn("override func setUp", source)
        self.assertNotIn("override func tearDown", source)
        entries = re.findall(r"func (test\w+)\(\) \{\n        defer \{ closeFixture\(\) \}\n        launchFixture\((?:\.(?:countdown|feedback|replacement|unavailable))?\)", source)
        self.assertEqual(len(entries), 7)
        self.assertEqual(set(entries), set(ios.CASES))
        self.assertEqual(len(re.findall(r"^        launchFixture\(", source, re.M)), 7)

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
            "request-scope", "metadata-origin", "metadata-path", "authorization", "request-json", "operation", "answer-admission", "answer-payload",
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

    def test_android_failure_phase_buffer_is_bounded_and_does_not_change_pass_schema(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        self.assertIn("private val phases = ArrayDeque<String>()", source)
        phase = source.split("    private fun phase(", 1)[1].split("    override fun onCreate", 1)[0]
        self.assertIn(".take(160)", phase)
        self.assertIn("if (phases.size == 64) phases.removeFirst()", phase)
        self.assertIn("phases.addLast(record)", phase)
        self.assertIn('${error.stackTraceToString()}\\n${phases.joinToString("\\n")}\\n', source)
        success = source.split('            result.putString("approval_cases"', 1)[1].split('        } catch (error: Throwable)', 1)[0]
        self.assertNotIn("phases", success)
        self.assertIn('finish(Activity.RESULT_OK, result)', success)

    def test_android_early_windows_use_owned_lifecycle_and_scoped_fresh_identity_cache(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        button = source.split("    private fun button(", 1)[1].split("    private fun tap(", 1)[0]
        self.assertIn('earlySend?.takeIf { label == "Send answer" }', button)
        self.assertIn('check(cached.current())', button)
        self.assertIn('cached.node.refresh() && cached.node.windowId == cached.window && exactVisible(cached.node, label)', button)
        self.assertIn('} else node(label)', button)
        helper = source.split("        fun captureEarlySend(", 1)[1].split("        try {\n            await(\"private model storage\")", 1)[0]
        for identity in ('model.selected?.target == target && model.activityVerified', 'card.id == id && card.hash == hash',
                         'card.run == target.run && card.conversation == target.conversation'):
            self.assertIn(identity, helper)
        self.assertLess(helper.index('earlySend = captured'), helper.index('sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)'))
        self.assertLess(helper.index('!main { activity.hasWindowFocus() }'), helper.index('val resumeStart = SystemClock.elapsedRealtime()'))
        self.assertLess(helper.index('val resumeStart = SystemClock.elapsedRealtime()'), helper.index('targetContext.startActivity('))
        self.assertLess(helper.index('main { activity.hasWindowFocus() }'), helper.index('checkEarly()'))
        self.assertLess(helper.index('checkEarly()'), helper.index('val elapsed = SystemClock.elapsedRealtime() - resumeStart'))
        self.assertIn('elapsed >= 0 && elapsed < 3_000', helper)
        self.assertIn('finally {\n                earlySend = null', helper)
        self.assertEqual(re.findall(r'earlyReview\("([a-z]+)"\)', source), ['countdown', 'replacement', 'feedback'])
        self.assertLess(source.index('await("replacement drops old confirmation")'), source.index('earlyReview("replacement")'))
        self.assertLess(source.index('check(editor.performAction(AccessibilityNodeInfo.ACTION_FOCUS))'), source.index('earlyReview("feedback")'))
        lifecycle = source.split('boundary("begin", "lifecycle_no_submit")', 1)[1].split('boundary("complete", "lifecycle_no_submit")', 1)[0]
        self.assertNotIn('earlyReview(', lifecycle)
        self.assertEqual(lifecycle.count('sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)'), 1)
        self.assertEqual(lifecycle.count('targetContext.startActivity('), 1)
        self.assertLess(lifecycle.index('val lifecycleSend = captureEarlySend()'), lifecycle.index('tap("Send answer")'))
        self.assertLess(lifecycle.index('earlySend = lifecycleSend'), lifecycle.index('sendKeyDownUpSync(KeyEvent.KEYCODE_HOME)'))
        self.assertLess(lifecycle.index('val resumeStart = SystemClock.elapsedRealtime()'), lifecycle.index('targetContext.startActivity('))
        self.assertLess(lifecycle.index('check(count() == 4 && !button("Send answer").isEnabled)'), lifecycle.index('val elapsed = '))
        self.assertIn('elapsed >= 0 && elapsed < 3_000', lifecycle)
        self.assertIn('finally {\n                earlySend = null', lifecycle)
        self.assertIn('check(count() == 4 && !button("Send answer").isEnabled)', lifecycle)
        for forbidden in ('setActivityVerified', 'setDeclaredField("activeFrameVerified"', 'setReviewDelayForTests', 'CountDownLatch'):
            self.assertNotIn(forbidden, source)

    def test_ios_controls_bind_modes_generation_first_consumption_and_checked_cleanup(self):
        host = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        tests = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        controls = host.split("final class FixtureControls {", 1)[1].split("// This file belongs only", 1)[0]
        self.assertIn('case beginFeedbackReview = "begin-feedback-review", replace', controls)
        self.assertIn("case .normal: []; case .feedback: [.beginFeedbackReview]; case .replacement: [.replace]", controls)
        self.assertIn("!closed, failure.isEmpty, actions.contains(action), !consumed.contains(action), tokens[action] == token", controls)
        self.assertLess(controls.index("tokens.removeValue(forKey: action)"), controls.index("guard notify_cancel(token) == 0"))
        self.assertLess(controls.index("guard notify_cancel(token) == 0"), controls.index("consumed.insert(action)"))
        self.assertIn('if notify_cancel(token) != 0 { failed("close-cancel") }', controls)
        self.assertIn('if failure.isEmpty { status = "closed" }', controls)
        self.assertIn("notify_register_dispatch(pointer, &token, DispatchQueue.main)", controls)
        self.assertIn("MainActor.assumeIsolated { self?.consume(action, token: received) }", controls)
        for forbidden in ("sendQuestion(", "callbacks.append(", "URLSession(", "FileManager(", "notify_register_signal("):
            self.assertNotIn(forbidden, controls)
        self.assertIn("let actualGeneration = UUID().uuidString", host)
        self.assertIn("generation = actualGeneration", host)
        self.assertIn('fixtureLaunch = UUID().uuidString.lowercased()', tests)
        self.assertIn('XCTAssertEqual(app.staticTexts["fixture-generation"].label, fixtureGeneration)', tests)
        self.assertIn('XCTAssertTrue(posted.insert(action).inserted, "No control replays")', tests)
        self.assertIn("notify_post($0)", tests)
        self.assertIn("Owned registration cleanup must be acknowledged", tests)
        self.assertEqual(host.count("test.example.zerus.ui."), 1)
        self.assertEqual(tests.count("test.example.zerus.ui."), 2)

    def test_ios_feedback_review_and_replacement_handshakes_preserve_real_assertions(self):
        host = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        tests = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        self.assertIn("available = selectedMode != .feedback", host)
        self.assertIn('"text": "Use a safer comman"', host)
        self.assertIn('question.id == "fixture-approval" && question.hash == "fixture-hash-0"', host)
        feedback = tests.split("    func testFeedbackKeyboardCannotBypassReview()", 1)[1].split("    func testBackground", 1)[0]
        for earlier, later in [('field.tap()', 'beginReview("begin-feedback-review")'), ('beginReview("begin-feedback-review")', 'field.typeText("d\\n")'),
                               ('field.typeText("d\\n")', 'let appBegan = ack('), ('let appBegan = ack(', 'count(0)'),
                               ('count(0)', 'reviewed(); field.typeText("\\n")')]:
            self.assertLess(feedback.index(earlier), feedback.index(later))
        self.assertIn("XCTAssertLessThan(earlyElapsed, 3", feedback)
        self.assertIn('answer.contains("Use a safer command")', feedback)
        self.assertIn('XCTAssertEqual(answer, "fixture-hash-0|[', feedback)
        replacement = tests.split("    func testReplacementRetiresAnOpenConfirmation()", 1)[1].split("    func testUnavailable", 1)[0]
        self.assertLess(replacement.index('XCTAssertTrue(app.buttons["Confirm"].waitForExistence'), replacement.index('post("replace")'))
        self.assertLess(replacement.index('post("replace")'), replacement.index('XCTWaiter.wait(for: [retired]'))
        self.assertIn('count(0); XCTAssertFalse(send.isEnabled)', replacement)
        self.assertNotIn('Replace in 2 seconds', host + tests)
        self.assertIn("changes the agent's permissions beyond this request.\\n\\nExact command for this session only", tests)

    def test_android_metadata_source_predicate_refuses_every_unbound_origin_and_extra_path(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        metadata = source.split('                if (request.method == "GET") {', 1)[1].split('                check(request.url.host == "relay.example.test"', 1)[0]
        condition = metadata.split("                    check(", 1)[1].split(')\n                    guard = "metadata-path"', 1)[0]
        fields = {"request.url.scheme": "scheme", "request.url.host": "host", "request.url.port": "port",
                  "request.url.encodedUsername": "username", "request.url.encodedPassword": "password",
                  "request.url.encodedQuery": "query", "request.url.encodedFragment": "fragment", "request.body": "body",
                  'request.header("Authorization")': "authorization"}
        for old, new in fields.items():
            condition = condition.replace(old, new)
        condition = condition.replace(".isEmpty()", ' == ""').replace("&&", "and").replace("null", "None")
        condition = " ".join(condition.split())
        valid = dict(scheme="https", host="relay.example.test", port=443, username="", password="", query=None,
                     fragment=None, body=None, authorization="Bearer fixture-token")
        self.assertTrue(finite_source_predicate(condition, valid))
        for field, value in dict(scheme="http", host="example.test", port=8443, username="user", password="password", query="",
                                 fragment="", body="", authorization="Bearer other").items():
            with self.subTest(field=field):
                self.assertFalse(finite_source_predicate(condition, {**valid, field: value}))
        paths = re.search(r'encodedPath in setOf\(([^\n]+)\)', metadata)[1]
        allowed = re.findall(r'"([^"]+)"', paths)
        self.assertEqual(allowed, ["/v1/capabilities", "/v1/computers"])
        for path in ("/v1/requests", "/v1/computers/", "/v1/computers/other", "/v1/capabilities/", "/", ""):
            with self.subTest(path=path):
                self.assertNotIn(path, allowed)
        self.assertIn('check(count <= 64)', metadata)
        self.assertIn('JSONArray(listOf("inspect", "answer"))', metadata)
        self.assertIn('synchronized(raw) { JSONObject(raw.toString()) }', metadata)
        self.assertIn('put("sessions", JSONArray().put(current))', metadata)
        self.assertIn('return@addInterceptor Response.Builder()', metadata)
        self.assertNotIn("chain.proceed(", source)

    def test_android_broader_proof_and_failure_windows_do_not_rewrite_or_hide_primary_outcome(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        proof = source.split("        fun proveBroaderDraft()", 1)[1].split("        fun expect(", 1)[0]
        for value in ('model.draft(target, question)', 'QuestionPolicies.broaderChoice(question, mapOf("approval" to ids))',
                      'ids == setOf("2")', 'answer.string("text").isEmpty()', 'draft.status == "editing"',
                      'question.run == target.run', 'question.conversation == target.conversation', 'scope?.id == "2"'):
            self.assertIn(value, proof)
        for forbidden in ("model.edit(", "set(model", "tap(", "expected.set("):
            self.assertNotIn(forbidden, proof)
        diagnostics = source.split("    private fun windowEvidence()", 1)[1].split("    private fun set(", 1)[0]
        for required in ("windows.size in 1..4", "depth <= 16", "++visited <= 128", "++matched <= 8", ".take(160)",
                         'root.packageName?.toString() != targetContext.packageName', 'owned-window-evidence UNKNOWN'):
            self.assertIn(required, diagnostics)
        for forbidden in ("performAction(", "injectInputEvent(", "request.body", "Authorization"):
            self.assertNotIn(forbidden, diagnostics)
        failure = source.split('            result.putString("stream", "Approval choices smoke failed:', 1)[1].split("finish(Activity.RESULT_CANCELED", 1)[0]
        self.assertLess(failure.index("error.stackTraceToString()"), failure.index("$failureEvidence"))

    def test_android_receipt_applied_source_predicate_requires_every_exact_settled_identity(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        helper = source.split("        fun receiptApplied(number: Int)", 1)[1].split("        fun open(", 1)[0]
        condition = helper.split("                    val applied =", 1)[1].split("\n                    applied", 1)[0]
        fields = {
            "!model.answerInFlight(draft)": "not in_flight", "!model.detailBusy": "not detail_busy",
            "model.selected?.target": "selected_target", "question?.id": "current_id", "question?.hash": "current_hash",
            "question?.run": "current_run", "question?.conversation": "current_conversation", "draft != null": "draft_present",
            "draft.target": "draft_target", "draft.questionId": "draft_id", "draft.questionHash": "draft_hash",
            "draft.requestId": "draft_request", "draft.answers": "draft_answers", "draft.status": "draft_status",
            "model.pendingQuestionSend": "pending_consent", "model.activityVerified": "verified", "expected.get()": "expected_answer",
            "target.run": "target_run", "target.conversation": "target_conversation",
        }
        for old, new in fields.items():
            condition = condition.replace(old, new)
        condition = " ".join(condition.replace("&&", "and").replace("null", "None").split())
        full_answers = '[{"question_id":"approval","selected_option_ids":["2"],"text":""}]'
        valid = dict(target="target", selected_target="target", questionId="card", current_id="card", questionHash="hash", current_hash="hash",
                     target_run="run", current_run="run", target_conversation="conversation", current_conversation="conversation", draft_present=True,
                     draft_target="target", draft_id="card", draft_hash="hash", requestId="request", draft_request="request", answers=full_answers,
                     draft_answers=full_answers, draft_status="submitted", in_flight=False, pending_consent=None, verified=True, detail_busy=False, expected_answer=None)
        self.assertTrue(finite_source_predicate(condition, valid))
        departures = dict(selected_target="other", current_id="older-card", current_hash="older-hash", current_run="older-run",
                          current_conversation="older-conversation", draft_present=False, draft_target="other", draft_id="older-card",
                          draft_hash="older-hash", draft_request="older-request", draft_answers='[{"selected_option_ids":["1"]}]',
                          draft_status="submitting", in_flight=True, pending_consent="pending", verified=False, detail_busy=True, expected_answer="unconsumed")
        for field, value in departures.items():
            with self.subTest(field=field):
                self.assertFalse(finite_source_predicate(condition, {**valid, field: value}))
        for answer in ('[]', full_answers.replace('"2"', '"1"'), full_answers.replace('"text":""', '"text":"other"'),
                       full_answers.replace('"approval"', '"other"')):
            with self.subTest(answers=answer):
                self.assertFalse(finite_source_predicate(condition, {**valid, "draft_answers": answer}))

    def test_android_receipt_gates_follow_original_counts_without_writes_or_unsent_card_admission(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        helper = source.split("        fun receiptApplied(number: Int)", 1)[1].split("        fun open(", 1)[0]
        for required in ('synchronized(calls)', 'check(calls.size == number)', 'JSONObject(calls[number - 1].toString())',
                         'payload.getString("request_id")', 'payload.getString("question_id")', 'payload.getString("expected_question_hash")',
                         'payload.getJSONArray("answers").toString()', 'requestId.isNotBlank() && questionId.isNotBlank() && questionHash.isNotBlank()',
                         'payload.string("expected_run_id") == target.run', 'payload.string("expected_conversation_id") == target.conversation',
                         'model.drafts.singleOrNull { it.target == target && it.questionId == questionId && it.questionHash == questionHash }'):
            self.assertIn(required, helper)
        for forbidden in ('model.draft(', 'model.edit(', 'model.send(', 'set(model', 'expected.set(', 'model.refreshActivity(', 'store.'):
            self.assertNotIn(forbidden, helper)
        self.assertEqual(re.findall(r'^            receiptApplied\((\d)\)$', source, re.M), list("1234"))
        for number, description in enumerate(("once receipt", "broader receipt", "new card reject receipt", "one feedback receipt"), 1):
            wait = source.index('await("' + description + '") { count() == ' + str(number) + ' }')
            gate = source.index('            receiptApplied(' + str(number) + ')')
            self.assertLess(wait, gate)
            self.assertNotIn('open("', source[wait:gate])
        unsent = source.split('            open("replacement")', 1)[1].split('            reviewed(); expect("3")', 1)[0]
        self.assertNotIn('receiptApplied(', unsent)
        self.assertIn('reviewed(); proveBroaderDraft(); tap("Send answer"); node("Confirm this option?")', unsent)
        lifecycle = source.split('            open("background")', 1)[1].split('            boundary("begin", "readonly_stale_identity")', 1)[0]
        self.assertLess(lifecycle.index('proveBroaderDraft()'), lifecycle.index('val lifecycleSend = captureEarlySend()'))
        self.assertEqual(source.count('proveBroaderDraft()'), 5)  # Definition plus the four actual broader Send paths.

    def test_android_failure_is_captured_before_teardown_and_primary_survives_diagnostic_or_cleanup_error(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        outer = source.split('    override fun onStart()', 1)[1].split('    private fun <T> main(', 1)[0]
        self.assertNotIn('windowEvidence()', outer)
        self.assertNotIn('screenshot("failure")', outer)
        tail = source.split('        var primaryFailure: Throwable? = null', 1)[1]
        failure, cleanup = tail.rsplit('        } catch (error: Throwable) {', 1)[1].split('        } finally {', 1)
        for earlier, later in [('primaryFailure = error', 'failureEvidence = windowEvidence()'),
                               ('failureEvidence = windowEvidence()', 'screenshot("failure")'), ('screenshot("failure")', 'throw error')]:
            self.assertLess(failure.index(earlier), failure.index(later))
        self.assertEqual(failure.count('catch (diagnostic: Throwable) { if (diagnostic !== error) error.addSuppressed(diagnostic) }'), 2)
        self.assertNotIn('model.back()', failure)
        self.assertNotIn('activity.finish()', failure)
        self.assertIn('try { main { model.back(); activity.finish() } }', cleanup)
        self.assertIn('val original = primaryFailure', cleanup)
        self.assertIn('if (original == null) throw cleanup else if (cleanup !== original) original.addSuppressed(cleanup)', cleanup)

    def test_android_tap_diagnostics_refresh_and_bind_existing_one_physical_touch_without_new_actions(self):
        source = (ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt").read_text()
        tap = source.split('    private fun tap(label: String)', 1)[1].split('    private fun reveal(', 1)[0]
        self.assertLess(tap.index('check(action.refresh())'), tap.index('check(action.isEnabled && action.isClickable)'))
        for required in ('action.windowId', 'action.isVisibleToUser', 'action.isSelected', 'action.isChecked',
                         'phase("touch-geometry:$bounds:x=${bounds.exactCenterX()}:y=${bounds.exactCenterY()}")'):
            self.assertIn(required, tap)
        self.assertLess(tap.index('phase("touch-node:'), tap.index('val down = SystemClock.uptimeMillis()'))
        self.assertLess(tap.index('phase("touch-geometry:'), tap.index('val down = SystemClock.uptimeMillis()'))
        self.assertEqual(tap.count('uiAutomation.injectInputEvent(event, true)'), 1)
        self.assertIn('for (type in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP))', tap)
        self.assertIn('bounds.exactCenterX(), bounds.exactCenterY(), 0)', tap)
        for forbidden in ('performAction(', 'ACTION_CLICK', 'tap(', 'while ', 'Thread.sleep('):
            self.assertNotIn(forbidden, tap)

    def test_ios_reverse_ack_source_token_guard_nonce_order_and_teardown_refuse_unknown(self):
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        receiver = source.split("private final class ReviewAcknowledgement", 1)[1].split("/// Native SwiftUI", 1)[0]
        guard = re.search(r'guard ([^\n]+) else \{ failure = "stale-or-repeated-reverse-ack"', receiver)[1]
        condition = guard.replace("!consumed", "not consumed").replace(",", " and")
        self.assertTrue(finite_source_predicate(condition, dict(consumed=False, token=17, received=17)))
        for values in (dict(consumed=True, token=17, received=17), dict(consumed=False, token=None, received=17),
                       dict(consumed=False, token=17, received=18)):
            with self.subTest(values=values):
                self.assertFalse(finite_source_predicate(condition, values))
        self.assertLess(receiver.index("token = nil //"), receiver.index("guard notify_cancel(received) == 0"))
        self.assertLess(receiver.index("guard notify_cancel(received) == 0"), receiver.index("receivedAt ="))
        self.assertNotIn("@unchecked", receiver)
        begin = source.split("    private func beginReview(", 1)[1].split("    private func ack(", 1)[0]
        self.assertIn("addTeardownBlock { () async in await receiver.close() }", begin)
        for earlier, later in [('addTeardownBlock', 'receiver.register(reverse)'), ('receiver.register(reverse)', 'let began ='),
                               ('let began =', 'notify_post($0)'), ('notify_post($0)', 'receiver.wait()')]:
            self.assertLess(begin.index(earlier), begin.index(later))
        self.assertNotIn('staticTexts[', begin.split("let began =", 1)[1])
        for code in ("c", "f"):
            name = "zui.ack." + ".".join(["a" * 36, "b" * 36, "c" * 36, code])
            self.assertLessEqual(len(name), 128)
        host = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        self.assertIn('selectedAcknowledgement != selectedLaunch && selectedAcknowledgement != generation.lowercased()', host)
        self.assertLess(host.index('status = "ack|'), host.index('guard reverse.withCString'))
        self.assertIn('case .replacement: [.replace]; case .countdown: [.beginCountdownReview]', host)
        countdown = source.split("    func testRealCountdownAndOneTimeChoice()", 1)[1].split("    func testBroader", 1)[0]
        for earlier, later in [('launchFixture(.countdown)', 'choice("Approve once").tap()'), ('choice("Approve once").tap()', 'beginReview("begin-countdown-review")'),
                               ('beginReview("begin-countdown-review")', 'XCTAssertFalse(send.isEnabled); count(0)'),
                               ('XCTAssertLessThan(earlyElapsed, 3', 'ack("begin-countdown-review", serial: 0)')]:
            self.assertLess(countdown.index(earlier), countdown.index(later))
        self.assertNotIn('press(.home)', countdown)

    def test_ios_adaptive_cancel_requires_owned_popover_and_passive_outside_anchor(self):
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        cancel = source.split("    private func cancelBroaderConfirmation()", 1)[1].split("    func testRealCountdown", 1)[0]
        for required in ('if app.buttons["Cancel"].exists', 'XCTAssertEqual(app.popovers.count, 1',
                         'popover.buttons["Confirm"].exists && popover.staticTexts[disclosure].exists',
                         'XCTAssertTrue(anchor.exists)', 'XCTAssertEqual(anchor.identifier, "fixture-dismiss-anchor")',
                         'XCTAssertEqual(anchor.label, "Synthetic UI only — no native delivery")', '["regular", "compact"].contains(trait)',
                         'candidate.staticTexts["fixture-dismiss-anchor"].exists && candidate.popovers.count == 1',
                         'geometryCheck(windows.count == 1', 'positiveFinite(actual) && actual.contains(popup)',
                         'frame.intersection(window).intersection(canvas)', '!region.intersects(popup)',
                         'let coordinate = anchor.coordinate(withNormalizedOffset: offset)',
                         'let point = coordinate.screenPoint', 'XCTAssertEqual(app.state, .runningForeground)',
                         'geometryCheck(pointInside,', 'coordinate.tap()',
                         'XCTWaiter.wait(for: [retired], timeout: 2)'):
            self.assertIn(required, cancel)
        self.assertLess(cancel.index('XCTAssertEqual(app.popovers.count, 1'), cancel.index('anchor.coordinate('))
        self.assertLess(cancel.index('let point = coordinate.screenPoint'), cancel.index('geometryCheck(pointInside,'))
        self.assertLess(cancel.index('geometryCheck(pointInside,'), cancel.index('coordinate.tap()'))
        self.assertEqual(cancel.count('coordinate.tap()'), 1)
        for forbidden in ('notify_post(', 'while ', 'try?', 'coordinate(withOffset:', 'broader =', 'available =', '.isHittable',
                          '!popup.intersects(frame)', 'actual.contains(frame)'):
            self.assertNotIn(forbidden, cancel)
        host = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        anchor = host.split('Text("Synthetic UI only — no native delivery")', 1)[1].split('Text("Callbacks:', 1)[0]
        self.assertIn('.accessibilityIdentifier("fixture-dismiss-anchor")', anchor)
        self.assertNotIn("Button(", anchor)
        self.assertNotIn("onTapGesture", anchor)

    def test_ios_outside_coordinate_source_point_refuses_edges_other_windows_and_popover(self):
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        condition = source.split('            let pointInside = ', 1)[1].split('\n            guard geometryCheck(pointInside,', 1)[0]
        condition = condition.replace('!popup.contains(point)', 'not popup_contains').replace('&&', ' and ')
        for value in ('point.x', 'point.y', 'frame.minX', 'frame.maxX', 'frame.minY', 'frame.maxY',
                      'window.minX', 'window.maxX', 'window.minY', 'window.maxY',
                      'canvas.minX', 'canvas.maxX', 'canvas.minY', 'canvas.maxY',
                      'region.minX', 'region.maxX', 'region.minY', 'region.maxY'):
            condition = condition.replace(value, value.replace('.', '_'))
        condition = '(' + condition + ')'
        good = dict(point_x=50, point_y=50, frame_minX=40, frame_maxX=60, frame_minY=40, frame_maxY=60,
                    window_minX=0, window_maxX=100, window_minY=0, window_maxY=100,
                    canvas_minX=0, canvas_maxX=100, canvas_minY=0, canvas_maxY=100,
                    region_minX=45, region_maxX=55, region_minY=45, region_maxY=55, popup_contains=False)
        self.assertTrue(finite_source_predicate(condition, good))
        departures = [dict(popup_contains=True), dict(point_x=float('nan')), dict(point_y=float('inf'))]
        for axis in ('X', 'Y'):
            for owner in ('frame', 'window', 'canvas', 'region'):
                departures += [{owner + '_min' + axis: 50}, {owner + '_max' + axis: 50}]
        for change in departures:
            with self.subTest(change=change):
                self.assertFalse(finite_source_predicate(condition, {**good, **change}))

    def test_ios_outside_geometry_uses_finite_clipped_unoccluded_source_regions(self):
        """Finite arithmetic controls for the source; no CGRect, Swift compiler or native gesture claim."""
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        helper = source.split('    private func positiveFinite(', 1)[1].split('    private func cancelBroader', 1)[0]
        fields = re.search(r'\[([^\n]+)\]\.allSatisfy \{ \$0.isFinite \}', helper)[1].split(', ')
        self.assertEqual(fields, ['rect.minX', 'rect.minY', 'rect.maxX', 'rect.maxY', 'rect.width', 'rect.height'])
        positive = helper.split(' && ', 1)[1].split('\n', 1)[0].replace('rect.', '').replace('&&', 'and')
        def qualifies(rect):
            return all(math.isfinite(rect[field.split('.')[1]]) for field in fields) and finite_source_predicate(positive, rect)
        valid = dict(minX=0, minY=0, maxX=100, maxY=100, width=100, height=100)
        self.assertTrue(qualifies(valid))
        for field in valid:
            for invalid in (float('nan'), float('inf'), -float('inf')):
                with self.subTest(field=field, invalid=invalid):
                    self.assertFalse(qualifies({**valid, field: invalid}))
        for field in ('width', 'height'):
            for invalid in (0, -1):
                self.assertFalse(qualifies({**valid, field: invalid}))
        bounds = source.split('let bounds: [(CGFloat, CGFloat, CGFloat, CGFloat)] = ', 1)[1].split('\n            guard let selected', 1)[0]
        bounds = re.sub(r'(visible|popup)\.(minX|minY|maxX|maxY)', r'\1_\2', bounds)
        tree = ast.parse(bounds, mode='eval')
        allowed = (ast.Expression, ast.List, ast.Tuple, ast.Name, ast.Load, ast.Call)
        self.assertTrue(all(isinstance(node, allowed) for node in ast.walk(tree)))
        self.assertEqual({node.func.id for node in ast.walk(tree) if isinstance(node, ast.Call)}, {'min', 'max'})
        selector = re.search(r'bounds.first\(where: \{ ([^\n]+) \}\)', source)[1]
        for index, name in enumerate(('minX', 'minY', 'maxX', 'maxY')):
            selector = selector.replace('$0.' + str(index), name)
        selector = selector.replace('&&', 'and')
        def region(visible, popup):
            values = {owner + '_' + field: value for owner, rect in (('visible', visible), ('popup', popup))
                      for field, value in zip(('minX', 'minY', 'maxX', 'maxY'), rect)}
            self.assertLessEqual({node.id for node in ast.walk(tree) if isinstance(node, ast.Name)}, values.keys() | {'min', 'max'})
            candidates = eval(compile(tree, '<finite-source-regions>', 'eval'), {'__builtins__': {}, 'min': min, 'max': max}, values)
            self.assertEqual(len(candidates), 4)  # One geometric choice before the single gesture, never tap retries.
            return next((row for row in candidates if finite_source_predicate(selector, dict(zip(('minX', 'minY', 'maxX', 'maxY'), row)))), None)
        visible = (10, 10, 90, 90)
        for popup in ((20, 20, 80, 80), (0, 0, 100, 50), (50, 0, 100, 100), (0, 0, 50, 100),
                      (0, 90, 100, 100), (90, 0, 100, 100), (100, 100, 120, 120)):
            with self.subTest(popup=popup):
                selected = region(visible, popup)
                self.assertIsNotNone(selected)
                left, top, right, bottom = selected
                x, y = (left + right) / 2, (top + bottom) / 2
                self.assertTrue(visible[0] < x < visible[2] and visible[1] < y < visible[3])
                self.assertFalse(popup[0] <= x <= popup[2] and popup[1] <= y <= popup[3])
        for popup in (visible, (0, 0, 100, 100)):
            self.assertIsNone(region(visible, popup))
        cancel = source.split('    private func cancelBroaderConfirmation()', 1)[1].split('    func testRealCountdown', 1)[0]
        self.assertLess(cancel.index('phase("cancel-geometry-popup-'), cancel.index('geometryCheck(positiveFinite(popup)'))
        self.assertLess(cancel.index('phase("cancel-geometry-window-'), cancel.index('positiveFinite(actual)'))
        self.assertLess(cancel.index('phase("cancel-geometry-visible-anchor-'), cancel.index('geometryCheck(positiveFinite(visible)'))
        diagnostic = source.split('    private func geometryCheck(', 1)[1].split('    private func positiveFinite(', 1)[0]
        self.assertLess(diagnostic.index('phase('), diagnostic.index('XCTAssertTrue(condition, criterion)'))

    def test_ios_unavailable_event_follows_original_dialog_and_is_single_mode_bound(self):
        host = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalUIFixtureApp.swift").read_text()
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        controls = host.split('final class FixtureControls {', 1)[1].split('// This file belongs only', 1)[0]
        self.assertIn('case becomeUnavailable = "become-unavailable"', controls)
        self.assertIn('case .unavailable: [.becomeUnavailable]', controls)
        self.assertIn('case .becomeUnavailable: available = false', controls)
        self.assertNotIn('Unavailable in 2 seconds', host + source)
        self.assertNotIn('Task.sleep', controls)
        self.assertIn('case .replace, .becomeUnavailable: nil', controls)
        actions = controls.split('    private var actions: [Action] {', 1)[1].split('    private func name(', 1)[0]
        mode_actions = re.findall(r'case \.(\w+): \[([^\]]*)\]', actions)
        self.assertEqual(len(mode_actions), 5)
        self.assertEqual(dict(mode_actions), dict(normal='', feedback='.beginFeedbackReview', replacement='.replace',
                                                  countdown='.beginCountdownReview', unavailable='.becomeUnavailable'))
        raw = re.search(r'guard (!closed, failure.isEmpty, actions.contains\(action\), !consumed.contains\(action\), tokens\[action\] == token) else', controls)[1]
        predicate = raw.replace('!closed', 'not closed').replace('failure.isEmpty', 'failure_empty').replace('actions.contains(action)', 'mode_admits').replace('!consumed.contains(action)', 'not consumed').replace('tokens[action]', 'owned_token').replace(',', ' and')
        for mode in ('normal', 'feedback', 'replacement', 'countdown', 'unavailable'):
            valid = dict(closed=False, failure_empty=True, mode_admits='.becomeUnavailable' in dict(mode_actions)[mode], consumed=False, owned_token=17, token=17)
            self.assertEqual(finite_source_predicate(predicate, valid), mode == 'unavailable')
        valid = dict(closed=False, failure_empty=True, mode_admits=True, consumed=False, owned_token=17, token=17)
        for changed in (dict(closed=True), dict(failure_empty=False), dict(consumed=True), dict(owned_token=None), dict(token=18)):
            self.assertFalse(finite_source_predicate(predicate, {**valid, **changed}))
        case = source.split('    func testUnavailableRetiresAnOpenConfirmation()', 1)[1].split('    func testFeedback', 1)[0]
        for earlier, later in [('launchFixture(.unavailable)', 'send.tap()'), ('send.tap()', 'waitForExistence(timeout: 1)'),
                               ('waitForExistence(timeout: 1)', 'count(0)'), ('count(0)', 'app.staticTexts['),
                               ('app.staticTexts[', 'post("become-unavailable")'), ('post("become-unavailable")', 'XCTWaiter.wait(for: [retired]'),
                               ('XCTWaiter.wait(for: [retired]', 'ack("become-unavailable", serial: 0)'),
                               ('ack("become-unavailable", serial: 0)', 'count(0); XCTAssertFalse(send.isEnabled)')]:
            self.assertLess(case.index(earlier), case.index(later))
        self.assertIn('changes the agent\'s permissions beyond this request.\\n\\nExact command for this session only', case)
        self.assertIn('NSPredicate(format: "exists == false")', case)
        self.assertIn('XCTWaiter.wait(for: [retired], timeout: 4)', case)
        self.assertEqual(case.count('post("become-unavailable")'), 1)

    def test_ios_feedback_refocus_preserves_real_early_return_text_and_final_oracle(self):
        source = (ROOT / "mobile/ios/ApprovalUIFixture/ApprovalChoicesUITests.swift").read_text()
        feedback = source.split('    func testFeedbackKeyboardCannotBypassReview()', 1)[1].split('    func testBackground', 1)[0]
        early, late = feedback.split('        count(0)\n', 1)
        self.assertIn('field.typeText("d\\n")', early)
        self.assertIn('XCTAssertLessThan(earlyElapsed, 3', early)
        self.assertIn('ack("begin-feedback-review", serial: 0)', early)
        self.assertEqual(early.count('field.tap()'), 1)
        for earlier, later in [('XCTAssertEqual(field.value as? String, "Use a safer command")', 'field.tap()'),
                               ('field.tap()', 'count(0)'), ('count(0)', 'reviewed(); field.typeText("\\n")')]:
            self.assertLess(late.index(earlier), late.index(later))
        self.assertEqual(late.count('field.tap()'), 1)
        self.assertEqual(late.count('XCTAssertEqual(field.value as? String, "Use a safer command")'), 2)
        self.assertEqual(feedback.count('field.typeText('), 2)
        self.assertIn('XCTAssertEqual(answer, "fixture-hash-0|[', late)
        self.assertNotIn('typeText("Use a safer command', feedback)
        self.assertNotIn('sleep(', feedback)


if __name__ == "__main__":
    unittest.main()
