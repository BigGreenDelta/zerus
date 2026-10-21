"""Offline result/admission controls; no SDK, child, device or network is used."""
import copy
import ast
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import textwrap
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("android_project_ui", ROOT / "scripts/ci/android-project-ui.py")
UI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UI)

TOUCH_SPEC = importlib.util.spec_from_file_location("android_touch_observer_controls", ROOT / "tests/android_touch_observer_controls.py")
TOUCH_CONTROLS = importlib.util.module_from_spec(TOUCH_SPEC)
TOUCH_SPEC.loader.exec_module(TOUCH_CONTROLS)
OwnedWindowTouchObserverTests = TOUCH_CONTROLS.OwnedWindowTouchObserverTests


def output(ids=UI.CASES, count="6", code="-1", stream="Session project smoke passed: synthetic fixture"):
    return (f"INSTRUMENTATION_RESULT: project_cases={count}\n"
            f"INSTRUMENTATION_RESULT: project_case_ids={ids}\n"
            f"INSTRUMENTATION_RESULT: stream={stream}\n\nINSTRUMENTATION_CODE: {code}\n")


def manifest(runner=UI.RUNNER, target="app.zerus.mobile"):
    return ('E: manifest (line=1)\n  E: instrumentation (line=5)\n'
            f'    A: android:name(0x01010003)="{runner}" (Raw: "{runner}")\n'
            f'    A: android:targetPackage(0x01010021)="{target}" (Raw: "{target}")\n'
            '  E: application (line=8)\n')


def dex(descriptor, defined=True):
    """Minimal table fixture: reference text alone must never prove a class."""
    data = bytearray(160)
    data[:8] = b"dex\n035\0"
    for offset, value in ((40, 0x12345678), (56, 1), (60, 112), (64, 1), (68, 116), (96, int(defined)), (100, 120), (112, 160)):
        struct.pack_into("<I", data, offset, value)
    data.extend(bytes([len(descriptor)]) + descriptor.encode("ascii") + b"\0")
    struct.pack_into("<I", data, 32, len(data))
    return data


class InstrumentationOutcomeTests(unittest.TestCase):
    def test_complete_exact_ordered_result_is_accepted(self):
        self.assertEqual(UI.instrumentation_result(output())["project_cases"], "6")

    def test_approval_diagnostic_statuses_cannot_qualify_the_project_runner(self):
        for status in ('INSTRUMENTATION_STATUS: zerus_approval_progress=1|1|ON_START|-|0|1001\nINSTRUMENTATION_STATUS_CODE: 1\n',
                       'INSTRUMENTATION_STATUS_CODE: 1\n'):
            with self.subTest(status=status), self.assertRaises(AssertionError):
                UI.instrumentation_result(status + output())

    def test_failed_or_unknown_finish_code_is_rejected(self):
        for code in ("0", "1", "-2", "-1 extra"):
            with self.subTest(code=code), self.assertRaises(AssertionError):
                UI.instrumentation_result(output(code=code))

    def test_partial_count_or_missing_case_is_rejected(self):
        for text in (output(count="5"), output(ids=UI.CASES.rsplit(",", 1)[0]), output(count="06")):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                UI.instrumentation_result(text)

    def test_duplicate_case_id_is_rejected(self):
        with self.assertRaises(AssertionError):
            UI.instrumentation_result(output(ids=UI.CASES.rsplit(",", 1)[0] + ",current_selection"))

    def test_reordered_or_additional_case_is_rejected(self):
        for ids in (",".join(reversed(UI.CASES.split(","))), UI.CASES + ",unexpected"):
            with self.subTest(ids=ids), self.assertRaises(AssertionError):
                UI.instrumentation_result(output(ids=ids))

    def test_duplicate_result_or_finish_record_is_rejected(self):
        for extra in ("INSTRUMENTATION_RESULT: project_cases=6\n", "INSTRUMENTATION_RESULT: project_case_ids=" + UI.CASES + "\n", "INSTRUMENTATION_CODE: -1\n"):
            with self.subTest(extra=extra), self.assertRaises(AssertionError):
                UI.instrumentation_result(output() + extra)

    def test_failed_stream_unknown_field_or_truncated_output_is_rejected(self):
        for text in (output(stream="Session project smoke failed: assertion"), output() + "INSTRUMENTATION_FAILED: abort\n", output() + "INSTRUMENTATION_RESULT: unexpected=1\n", output().split("INSTRUMENTATION_CODE")[0]):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                UI.instrumentation_result(text)


class ApkIdentityTests(unittest.TestCase):
    def test_exact_runner_and_target_are_accepted(self):
        UI.instrumentation_manifest(manifest())

    def test_wrong_runner_or_target_is_rejected(self):
        for text in (manifest(runner="app.zerus.mobile.OtherRunner"), manifest(target="app.zerus.other")):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                UI.instrumentation_manifest(text)

    def test_duplicate_runner_attribute_or_instrumentation_is_rejected(self):
        for text in (manifest().replace('  E: application', f'    A: android:name="{UI.RUNNER}"\n  E: application'), manifest() + manifest()):
            with self.subTest(text=text), self.assertRaises(AssertionError):
                UI.instrumentation_manifest(text)

    def test_equal_single_actual_certificates_are_accepted(self):
        certificate = "a1" * 32
        self.assertEqual(UI.matching_signers(f"Signer #1 certificate SHA-256 digest: {certificate}\n", f"Signer #1 certificate SHA-256 digest: {certificate.upper()}\n"), certificate)

    def test_wrong_missing_or_multiple_signers_are_rejected(self):
        app = "Signer #1 certificate SHA-256 digest: " + "a1" * 32 + "\n"
        for test in ("", app.replace("a1", "b2"), app + app.replace("#1", "#2"), app.replace("#1", "#2"), app.replace("a1", "zz")):
            with self.subTest(test=test), self.assertRaises(AssertionError):
                UI.matching_signers(app, test)

    def test_defined_multidex_class_is_linked_through_tables(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "test.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.writestr("classes.dex", dex("Lfixture/ReferenceOnly;", False))
                archive.writestr("classes10.dex", dex("Lfixture/Defined;"))
                archive.writestr("classes100.dex", dex("Lfixture/AlsoDefined;"))
            self.assertEqual(UI.defined_classes(apk), {"Lfixture/Defined;", "Lfixture/AlsoDefined;"})

    def test_unlinked_descriptor_or_invalid_table_cannot_prove_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            apk = Path(directory) / "test.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                archive.writestr("classes.dex", dex("Lapp/zerus/mobile/SessionProjectSmokeInstrumentation;", False))
            self.assertNotIn("Lapp/zerus/mobile/SessionProjectSmokeInstrumentation;", UI.defined_classes(apk))
            with zipfile.ZipFile(apk, "w") as archive:
                invalid = dex("Lfixture/Defined;")
                struct.pack_into("<I", invalid, 120, 99)
                archive.writestr("classes.dex", invalid)
            with self.assertRaises(AssertionError):
                UI.defined_classes(apk)


class InputAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.pins = json.loads((ROOT / "scripts/ci/android-project-ui-pins.json").read_text())
        self.env = {"EXPECTED_SOURCE": "a" * 40, "EXPECTED_EMULATOR_REVISION": "37.2.12", "EXPECTED_SYSTEM_IMAGE_REVISION": "9.0.0", "REQUESTED_EMULATOR_REVISION": "37.2.12", "REQUESTED_SYSTEM_IMAGE_REVISION": "9.0.0"}

    def test_exact_reviewed_inputs_are_accepted(self):
        UI.validate_inputs(self.env, self.pins)

    def test_invalid_source_and_missing_input_are_rejected(self):
        for source in ("A" * 40, "a" * 39, "main", "a" * 40 + "\n", ""):
            with self.subTest(source=source), self.assertRaises(AssertionError):
                UI.validate_inputs({**self.env, "EXPECTED_SOURCE": source}, self.pins)
        missing = dict(self.env)
        del missing["REQUESTED_SYSTEM_IMAGE_REVISION"]
        with self.assertRaises(KeyError):
            UI.validate_inputs(missing, self.pins)

    def test_unreviewed_or_noncanonical_revision_is_rejected(self):
        for key in ("EXPECTED_EMULATOR_REVISION", "REQUESTED_EMULATOR_REVISION", "EXPECTED_SYSTEM_IMAGE_REVISION", "REQUESTED_SYSTEM_IMAGE_REVISION"):
            for value in ("latest", "37.3.4", "9.0", "09.0.0", "9.0.0\n", ""):
                with self.subTest(key=key, value=value), self.assertRaises(AssertionError):
                    UI.validate_inputs({**self.env, key: value}, self.pins)

    def test_preview_unknown_package_or_archive_origin_is_rejected(self):
        for change in ("channel", "origin", "checksum", "extra"):
            pins = copy.deepcopy(self.pins)
            if change == "channel":
                pins["packages"]["emulator"]["channel"] = "channel-2"
            elif change == "origin":
                pins["packages"]["emulator"]["archive"]["url"] = "https://example.invalid/emulator.zip"
            elif change == "checksum":
                pins["packages"]["emulator"]["archive"]["checksum"] = "unknown"
            else:
                pins["packages"]["unknown"] = {}
            with self.subTest(change=change), self.assertRaises(AssertionError):
                UI.validate_inputs(self.env, pins)

    def test_installed_revision_file_uses_canonical_numeric_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            xml = Path(directory) / "package.xml"
            xml.write_text('<repository><localPackage><revision><major>9</major></revision></localPackage></repository>')
            self.assertEqual(UI.revision(xml), "9.0.0")
            xml.write_text('<repository><revision><major>latest</major></revision></repository>')
            with self.assertRaises(AssertionError):
                UI.revision(xml)


class HostedAdbCommandTests(unittest.TestCase):
    def test_package_probe_requires_successful_empty_filtered_listing(self):
        device = ["/fixture/adb", "-s", "emulator-5554", "shell"]
        for package in ("app.zerus.mobile", "app.zerus.mobile.test"):
            with self.subTest(package=package):
                self.assertEqual(UI.package_absence_command(device, package), [*device, "pm", "list", "packages", "--user", "0", package])
        UI.package_absence_result("", "")
        for stdout, stderr in (("package:app.zerus.mobile\n", ""), ("Error: service unavailable\n", ""), ("", "Error: unknown package manager\n")):
            with self.subTest(stdout=stdout, stderr=stderr), self.assertRaises(AssertionError):
                UI.package_absence_result(stdout, stderr)
        with self.assertRaises(AssertionError):
            UI.package_absence_command(device, "unreviewed.package")

    def test_only_the_fixed_port_loopback_foreground_server_is_started(self):
        command = UI.adb_server_command(Path("/fixture/sdk/platform-tools/adb"))
        self.assertEqual(command, ["/fixture/sdk/platform-tools/adb", "-L", "tcp:5037", "nodaemon", "server"])
        self.assertNotIn("-a", command)
        self.assertNotIn("tcp:127.0.0.1:5037", command)
        tree = ast.parse((ROOT / "scripts/ci/android-project-ui.py").read_text())
        main = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "main")
        calls = [node for node in ast.walk(main) if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "spawn" and node.args and isinstance(node.args[0], ast.Constant) and node.args[0].value == "adb-server"]
        self.assertEqual(len(calls), 1)
        self.assertEqual(ast.dump(calls[0].args[1]), ast.dump(ast.parse("adb_server_command(adb)", mode="eval").body))


class HostedJavaAdmissionTests(unittest.TestCase):
    def test_actual_full_hosted_release_and_wrong_patch_or_build(self):
        raw = (ROOT / "tests/fixtures/android-project-ui-java-release.txt").read_text()
        identity = UI.java_release(raw)
        self.assertEqual(identity["JAVA_VERSION"], "17.0.20.1")
        self.assertEqual(identity["IMPLEMENTOR_VERSION"], "Temurin-17.0.20.1+1")
        for field in ("JAVA_RUNTIME_VERSION", "FULL_VERSION", "SEMANTIC_VERSION", "JVM_VERSION"):
            self.assertEqual(identity[field], "17.0.20.1+1")
        for altered in (raw.replace("17.0.20.1", "17.0.20"), raw.replace("17.0.20.1", "17.0.20.2"), raw.replace("+1", "+2"),
                        raw.replace('FULL_VERSION="17.0.20.1+1"', 'FULL_VERSION="17.0.20.1+2"')):
            with self.subTest(altered=altered[:150]), self.assertRaises(AssertionError):
                UI.java_release(altered)

    def test_published_base_build_and_recognized_lts_spelling(self):
        for fields in ({"JAVA_VERSION": "17.0.20.1", "JAVA_RUNTIME_VERSION": "17.0.20.1+1"},
                       {"JAVA_VERSION": "17.0.20.1", "JAVA_RUNTIME_VERSION": "17.0.20.1+1-LTS", "IMPLEMENTOR_VERSION": "Temurin-17.0.20.1+1"},
                       {"JAVA_VERSION": "17.0.20.1", "IMPLEMENTOR_VERSION": "Temurin-17.0.20.1+1-LTS"},
                       {"JAVA_VERSION": "17.0.20.1+1-LTS"}):
            with self.subTest(fields=fields):
                text = "".join(f'{key}="{value}"\n' for key, value in fields.items())
                self.assertEqual(UI.java_release(text), fields)

    def test_wrong_or_unknown_build_fields_never_fall_back(self):
        for text in ('JAVA_VERSION="17.0.21"\nJAVA_RUNTIME_VERSION="17.0.21+1"\n',
                     'JAVA_VERSION="17.0.20.1"\nJAVA_RUNTIME_VERSION="17.0.20.1+2"\n',
                     'JAVA_VERSION="17.0.20.1"\nJAVA_RUNTIME_VERSION="17.0.20.1+1-unknown"\n',
                     'JAVA_VERSION="17.0.20.1"\nJAVA_RUNTIME_VERSION="17.0.20.1+1"\nIMPLEMENTOR_VERSION="Temurin-17.0.20.1+2"\n',
                     'JAVA_VERSION="17.0.20.1"\n', 'JAVA_VERSION="17.0.20.1+1"\nJAVA_VERSION="17.0.20.1+2"\n',
                     'JAVA_VERSION="17.0.20.1+1"\nINVALID\n', 'x' * 65537):
            with self.subTest(text=text[:200]), self.assertRaises(AssertionError):
                UI.java_release(text)

    def test_precheckout_and_helper_use_identical_java_validation(self):
        workflow = (ROOT / ".github/workflows/android-project-ui.yml").read_text()
        block = workflow.split("python3 -I -B - <<'PY'\n", 1)[1].split("          PY\n", 1)[0]
        nodes = ast.parse(textwrap.dedent(block)).body
        inline = next(node for node in nodes if isinstance(node, ast.FunctionDef) and node.name == "java_release")
        helper = next(node for node in ast.parse((ROOT / "scripts/ci/android-project-ui.py").read_text()).body if isinstance(node, ast.FunctionDef) and node.name == "java_release")
        self.assertEqual(ast.dump(inline), ast.dump(helper))
        for literal in ("raw = source.read(65537)", "java-release.txt", "failed_guard", "Hosted admission refused:"):
            self.assertIn(literal, block)

    def test_precheckout_retains_actual_release_and_names_refused_guard(self):
        workflow = (ROOT / ".github/workflows/android-project-ui.yml").read_text()
        block = textwrap.dedent(workflow.split("python3 -I -B - <<'PY'\n", 1)[1].split("          PY\n", 1)[0])
        for refused, image in ((None, "20261012.334.1"), (None, "20261018.339.1"), ("hosted_image", "unknown"), ("java_base_build", "20261012.334.1")):
            with self.subTest(refused=refused, image=image), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                java = root / "jdk"
                java.mkdir()
                raw = b'JAVA_VERSION="17.0.20.1"\nJAVA_RUNTIME_VERSION="17.0.20.1+1-LTS"\n'
                if refused == "java_base_build":
                    raw = raw.replace(b"+1", b"+2")
                (java / "release").write_bytes(raw)
                env = {"RUNNER_TEMP": str(root), "ImageVersion": image, "RUNNER_ENVIRONMENT": "github-hosted", "EXPECTED_SOURCE": "a" * 40,
                       "EXPECTED_EMULATOR_REVISION": "37.2.12", "REQUESTED_EMULATOR_REVISION": "37.2.12",
                       "EXPECTED_SYSTEM_IMAGE_REVISION": "9.0.0", "REQUESTED_SYSTEM_IMAGE_REVISION": "9.0.0", "JAVA_HOME_17_X64": str(java)}
                with patch.dict(os.environ, env, clear=True):
                    if refused:
                        with self.assertRaisesRegex(SystemExit, "Hosted admission refused: " + refused):
                            exec(compile(block, "<hosted-admission>", "exec"), {})
                    else:
                        exec(compile(block, "<hosted-admission>", "exec"), {})
                admission = root / "android-project-ui-admission"
                record = json.loads((admission / "runner.json").read_text())
                self.assertEqual(record.get("failed_guard"), refused)
                if refused != "hosted_image":
                    self.assertEqual((admission / "java-release.txt").read_bytes(), raw)


class PhoneServiceReadinessTests(unittest.TestCase):
    def subject(self, replies, *, now=0.0, end=5.0, duration=0.0, alive_duration=0.0):
        state = {"now": now, "calls": [], "sleeps": [], "replies": list(replies)}
        device = ["owned-adb", "-s", "emulator-5554", "shell"]
        def sleep(seconds):
            self.assertGreaterEqual(seconds, 0)
            self.assertLessEqual(seconds, 1)
            state["sleeps"].append(seconds)
            state["now"] += seconds
        def alive():
            state["now"] += alive_duration
        def run(name, argv, cap):
            self.assertGreater(cap, 0)
            self.assertLessEqual(state["now"] + cap, end)
            state["calls"].append((name, argv, cap))
            state["now"] += duration
            reply = state["replies"].pop(0)
            if isinstance(reply, BaseException):
                raise reply
            return reply
        def invoke():
            with patch.object(UI, "time", SimpleNamespace(monotonic=lambda: state["now"], sleep=sleep)):
                UI.phone_service_ready(run, alive, device, end)
        return invoke, state, device

    def test_ready_and_missing_then_ready_use_only_the_fixed_service_metadata(self):
        for replies in (["Service phone: found\n"], ["Service phone: not found\n", "Service phone: found\n"]):
            with self.subTest(replies=replies):
                invoke, state, device = self.subject(replies, end=30, duration=.25)
                invoke()
                self.assertEqual([row[0] for row in state["calls"]], ["phone-ready-" + str(i) for i in range(len(replies))])
                self.assertTrue(all(row[1] == device + ["service", "check", "phone"] and row[2] == 10 for row in state["calls"]))
                self.assertEqual(len(state["sleeps"]), len(replies) - 1)

    def test_permanently_missing_expires_the_same_end_without_an_extra_command(self):
        invoke, state, _ = self.subject(["Service phone: not found"] * 8, end=4, duration=1)
        with self.assertRaisesRegex(AssertionError, "phone service boot deadline"):
            invoke()
        self.assertEqual(len(state["calls"]), 2)
        self.assertEqual(state["now"], 4)

    def test_late_found_and_expiry_before_or_during_alive_cannot_authorize_readiness(self):
        for parameters, count in [({"end": 3, "duration": 3}, 1),
                                  ({"now": 3, "end": 3}, 0),
                                  ({"now": 2, "end": 3, "alive_duration": 1}, 0)]:
            with self.subTest(parameters=parameters):
                invoke, state, _ = self.subject(["Service phone: found"], **parameters)
                with self.assertRaisesRegex(AssertionError, "phone service boot deadline"):
                    invoke()
                self.assertEqual(len(state["calls"]), count)

    def test_command_error_and_unknown_response_are_not_retried_or_ignored(self):
        original = RuntimeError("owned command failed")
        for response in (original, "Service phone: found\nextra", "", "1"):
            with self.subTest(response=response):
                invoke, state, _ = self.subject([response, "Service phone: found"])
                with self.assertRaises((RuntimeError, AssertionError)) as error:
                    invoke()
                if response is original:
                    self.assertIs(error.exception, original)
                self.assertEqual(len(state["calls"]), 1)
                self.assertEqual(state["sleeps"], [])

    def test_remaining_original_budget_caps_the_command_and_main_preserves_disable_order(self):
        invoke, state, _ = self.subject(["Service phone: found"], now=4.5, end=5, duration=.25)
        invoke()
        self.assertEqual(state["calls"][0][2], .5)
        source = (ROOT / "scripts/ci/android-project-ui.py").read_text()
        self.assertEqual(source.count('boot_end = min(end - 20, time.monotonic() + 180)'), 1)
        self.assertIn('assert booted\n        device = [*adb_args, "-s", SERIAL, "shell"]\n        phone_service_ready(run, alive, device, boot_end)', source)
        self.assertIn('(("wifi-off", ["svc", "wifi", "disable"]), ("data-off", ["svc", "data", "disable"]),\n                           ("airplane", ["cmd", "connectivity", "airplane-mode", "enable"]))', source)
        self.assertIn('if "Active default network: none" in state and time.monotonic() < network_end:', source)
        function = source.split('def phone_service_ready(', 1)[1].split('\ndef failure_diagnostics(', 1)[0]
        self.assertNotIn('180', function)
        self.assertNotIn('except', function)


class WorkflowBoundaryTests(unittest.TestCase):
    def test_only_two_reviewed_images_are_allowed_with_the_same_version_guards(self):
        self.assertEqual(UI.HOSTED_IMAGES, {"20261012.334.1", "20261018.339.1"})
        workflow = (ROOT / ".github/workflows/android-project-ui.yml").read_text()
        block = textwrap.dedent(workflow.split("python3 -I -B - <<'PY'\n", 1)[1].split("          PY\n", 1)[0])
        assignment = next(node for node in ast.parse(block).body if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name) and node.targets[0].id == "HOSTED_IMAGES")
        self.assertEqual(ast.literal_eval(assignment.value.args[0]), UI.HOSTED_IMAGES)
        pins = json.loads((ROOT / "scripts/ci/android-project-ui-pins.json").read_text())
        self.assertEqual({image["version"] for image in pins["hosted_images"]}, UI.HOSTED_IMAGES)
        self.assertEqual(pins["jdk_release"]["version"], "17.0.20.1+1")
        self.assertEqual((pins["jdk_release"]["patch"], pins["jdk_release"]["build"]), (1, 1))
        for image in pins["hosted_images"]:
            self.assertEqual(image["jdk"], "17.0.20.1+1")
            self.assertEqual(image["sdk"], {"command_line_tools": "12.0", "platform_tools": "37.0.1", "build_tools": "36.0.0", "platform": "android-36"})
        for unknown in ("20261019.340.1", "20261018.339.2", "ubuntu-latest", "", None):
            with self.subTest(image=unknown):
                self.assertNotIn(unknown, UI.HOSTED_IMAGES)

    def test_pr_and_manual_run_the_exact_source_with_no_write_authority(self):
        workflow = (ROOT / ".github/workflows/android-project-ui.yml").read_text()
        for literal in ("pull_request:", "branches: [main]", "workflow_dispatch:", "github.event.pull_request.head.sha", "ref: ${{ env.EXPECTED_SOURCE }}", "contents: read", "persist-credentials: false", "EXPECTED_EMULATOR_REVISION: '37.2.12'", "EXPECTED_SYSTEM_IMAGE_REVISION: '9.0.0'", "require('hosted_image', os.environ.get('ImageVersion') in HOSTED_IMAGES)"):
            self.assertIn(literal, workflow)
        for forbidden in ("pull_request_target", "secrets.", "environment:", "self-hosted", "actions/cache", "permissions: write-all", "contents: write"):
            self.assertNotIn(forbidden, workflow)
        import re
        self.assertEqual(len(re.findall(r"uses: [\w/-]+@[0-9a-f]{40}", workflow)), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
