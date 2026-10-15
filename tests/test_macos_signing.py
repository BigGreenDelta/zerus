"""Developer ID signing must cover nested code inside-out and never release unnotarized bytes."""
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sign_macos", ROOT / "scripts/sign-macos.py")
sign = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sign)
MACH_O = b"\xcf\xfa\xed\xfe" + b"\0" * 28


class Signing(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-sign-test-")
        self.addCleanup(self.temporary.cleanup)
        self.app = Path(self.temporary.name) / "Zerus.app"
        self.write("Contents/Info.plist", plistlib.dumps({"CFBundleExecutable": "hgs-tray"}))
        self.write("Contents/MacOS/hgs-tray", MACH_O)
        self.write("Contents/MacOS/hgs", MACH_O)
        self.write("Contents/PlugIns/platforms/libqcocoa.dylib", MACH_O)
        self.write("Contents/Resources/setup/install-swarm-service.py", b"#!/usr/bin/env python3\n")
        for name in ("QtCore", "QtHelpers"):
            framework = f"Contents/Frameworks/{name}.framework"
            self.write(f"{framework}/Versions/A/{name}", MACH_O)
            os.symlink("A", self.app / framework / "Versions/Current")
            os.symlink(f"Versions/Current/{name}", self.app / framework / name)
        # A helper application nested in a framework is signed before it.
        helper = "Contents/Frameworks/QtHelpers.framework/Versions/A/Helpers/Helper.app"
        self.write(f"{helper}/Contents/Info.plist", plistlib.dumps({"CFBundleExecutable": "Helper"}))
        self.write(f"{helper}/Contents/MacOS/Helper", MACH_O)

    def write(self, name, payload):
        path = self.app / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)

    def relative(self, paths):
        return [str(path.relative_to(self.app.parent)) for path in paths]

    def test_signing_order_is_inside_out_and_complete(self):
        order = self.relative(sign.signing_order(self.app))
        helper = "Zerus.app/Contents/Frameworks/QtHelpers.framework/Versions/A/Helpers/Helper.app"
        engine = "Zerus.app/Contents/Frameworks/QtHelpers.framework/Versions/A"
        self.assertEqual(len(order), len(set(order)))
        self.assertEqual(order[-1], "Zerus.app")
        self.assertEqual(set(order), {
            "Zerus.app/Contents/PlugIns/platforms/libqcocoa.dylib", "Zerus.app/Contents/MacOS/hgs", helper,
            engine, "Zerus.app/Contents/Frameworks/QtCore.framework/Versions/A", "Zerus.app"})
        self.assertLess(order.index(helper), order.index(engine))
        for standalone in ("Zerus.app/Contents/PlugIns/platforms/libqcocoa.dylib", "Zerus.app/Contents/MacOS/hgs"):
            self.assertLess(order.index(standalone), order.index(engine))

    def test_only_the_application_gets_entitlements(self):
        entitlements = {self.relative([path])[0]: value for path in sign.signing_order(self.app)
                        if (value := sign.entitlements_for(path, self.app))}
        self.assertEqual(set(entitlements), {"Zerus.app"})
        application = plistlib.loads(Path(entitlements["Zerus.app"]).read_bytes())
        self.assertEqual(application, {"com.apple.security.automation.apple-events": True, "com.apple.security.device.audio-input": True})

    def test_codesign_command_uses_hardened_runtime_and_timestamps(self):
        command = sign.codesign_command(self.app, "Developer ID Application: Example (ABCDE12345)", None)
        self.assertIn("--options", command)
        self.assertEqual(command[command.index("--options") + 1], "runtime")
        self.assertIn("--timestamp", command)
        adhoc = sign.codesign_command(self.app, "-", None)
        self.assertIn("--timestamp=none", adhoc)

    def test_release_archive_requires_notarization(self):
        output = Path(self.temporary.name) / "release"
        with self.assertRaisesRegex(ValueError, "notariz"):
            sign.release_archive(self.app, output, "0.38.0", None, "6.11.2", "ABCDE12345")
        self.assertFalse(output.exists())

    def test_disk_image_requires_its_own_notarization_before_output_is_created(self):
        output = Path(self.temporary.name) / "release"
        image = Path(self.temporary.name) / "Zerus.dmg"
        image.write_bytes(b"disk image")
        with self.assertRaisesRegex(ValueError, "disk image"):
            sign.release_archive(self.app, output, "0.38.0", "0123abcd-4567-89ef-0123-456789abcdef",
                                 "6.11.2", "ABCDE12345", image)
        self.assertFalse(output.exists())

    def test_notary_failure_or_malformed_response_never_staples_the_disk_image(self):
        identifier = "0123abcd-4567-89ef-0123-456789abcdef"
        for code, response in ((0, []), (0, {"status": "Accepted", "id": None}),
                               (0, {"status": "Invalid", "id": identifier}),
                               (1, {"status": "Accepted", "id": identifier})):
            with self.subTest(code=code, response=response), \
                    mock.patch.object(sign, "create_disk_image"), \
                    mock.patch.object(sign, "staple") as staple, \
                    mock.patch.object(sign.subprocess, "run", return_value=subprocess.CompletedProcess(
                        [], code, json.dumps(response), "")):
                with self.assertRaisesRegex(SystemExit, "not accepted"):
                    sign.sign_disk_image(self.app, self.app.parent / "Zerus.dmg", "identity", None,
                                         Path("/keys/AuthKey.p8"), "ABC123DEF4", None)
                staple.assert_not_called()

    def test_stapler_retries_lost_connections_on_the_same_file_then_validates(self):
        command = ["xcrun", "stapler", "staple", str(self.app)]
        lost = subprocess.CompletedProcess(command, 68, "NSURLErrorDomain Code=-1005", "")
        accepted = subprocess.CompletedProcess(command, 0, "Stapled\n", "")
        with mock.patch.object(sign.subprocess, "run", side_effect=[lost, lost, accepted, accepted]) as run, \
                mock.patch.object(sign.time, "sleep") as sleep, \
                mock.patch("sys.stdout", new_callable=io.StringIO), \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            sign.staple(self.app)
        self.assertEqual(sleep.call_args_list, [mock.call(5), mock.call(15)])
        self.assertEqual(run.call_args_list, [mock.call(command, capture_output=True, text=True)] * 3 +
                         [mock.call(["xcrun", "stapler", "validate", str(self.app)], check=True)])

    def test_stapler_exhausted_transport_retries_are_fatal_without_validation(self):
        command = ["xcrun", "stapler", "staple", str(self.app)]
        lost = subprocess.CompletedProcess(command, 68, "", "NSURLErrorDomain Code=-1005")
        with mock.patch.object(sign.subprocess, "run", return_value=lost) as run, \
                mock.patch.object(sign.time, "sleep") as sleep, \
                mock.patch("sys.stdout", new_callable=io.StringIO), \
                mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(subprocess.CalledProcessError) as error:
                sign.staple(self.app)
        self.assertEqual(error.exception.returncode, 68)
        self.assertEqual(run.call_count, 5)
        self.assertTrue(all(call.args[0] == command for call in run.call_args_list))
        self.assertEqual(sleep.call_args_list, [mock.call(n) for n in (5, 15, 30, 60)])

    def test_stapler_does_not_retry_permanent_errors_or_failed_validation(self):
        command = ["xcrun", "stapler", "staple", str(self.app)]
        for code, message in ((68, "Record not found"), (1, "NSURLErrorDomain Code=-1005"),
                              (68, "The ticket is invalid")):
            with self.subTest(code=code, message=message), \
                    mock.patch.object(sign.subprocess, "run", return_value=subprocess.CompletedProcess(
                        command, code, message, "")) as run, \
                    mock.patch.object(sign.time, "sleep") as sleep, \
                    mock.patch("sys.stdout", new_callable=io.StringIO), \
                    mock.patch("sys.stderr", new_callable=io.StringIO):
                with self.assertRaises(subprocess.CalledProcessError):
                    sign.staple(self.app)
                self.assertEqual(run.call_count, 1)
                sleep.assert_not_called()
        validation = ["xcrun", "stapler", "validate", str(self.app)]
        with mock.patch.object(sign.subprocess, "run", side_effect=[
                subprocess.CompletedProcess(command, 0, "", ""),
                subprocess.CalledProcessError(65, validation)]) as run, \
                mock.patch.object(sign.time, "sleep") as sleep:
            with self.assertRaises(subprocess.CalledProcessError):
                sign.staple(self.app)
            self.assertEqual(run.call_count, 2)
            sleep.assert_not_called()

    @unittest.skipUnless(sys.platform == "darwin", "disk images require macOS")
    def test_readonly_disk_image_preserves_bundle_and_applications_link(self):
        image = Path(self.temporary.name) / "Zerus.dmg"
        sign.create_disk_image(self.app, image)
        original = image.read_bytes()
        with self.assertRaisesRegex(ValueError, "overwrite"):
            sign.create_disk_image(self.app, image)
        self.assertEqual(image.read_bytes(), original)
        mount = Path(self.temporary.name) / "mount"
        mount.mkdir()
        subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", str(mount),
                        str(image)], check=True, capture_output=True)
        try:
            self.assertTrue((mount / "Applications").is_symlink())
            self.assertEqual(os.readlink(mount / "Applications"), "/Applications")
            for source in self.app.rglob("*"):
                target = mount / "Zerus.app" / source.relative_to(self.app)
                if source.is_symlink():
                    self.assertTrue(target.is_symlink())
                    self.assertEqual(os.readlink(target), os.readlink(source))
                elif source.is_file():
                    self.assertEqual(target.read_bytes(), source.read_bytes())
        finally:
            subprocess.run(["hdiutil", "detach", str(mount)], check=True, capture_output=True)

    @unittest.skipUnless(sys.platform == "darwin", "ditto archives exist only on macOS")
    def test_release_archive_is_named_after_the_channel_release(self):
        output = Path(self.temporary.name) / "release"
        nightly = "0.38.0.r812.gabcdef0.n41"
        asset = sign.release_archive(self.app, output, nightly, "0123abcd-4567-89ef-0123-456789abcdef",
                                     "6.11.2", "ABCDE12345")
        self.assertEqual(asset.name, f"zerus-{nightly}-macos-arm64.zip")
        self.assertEqual(sorted(path.name for path in output.iterdir()), sorted([asset.name, "macos-info.json"]))

    def test_release_versions_are_stable_or_nightly(self):
        for value in ("0.38.0", "0.38.0.r812.gabcdef0.n41"):
            self.assertTrue(sign.RELEASE.fullmatch(value), value)
        for value in ("0.38", "0.38.0-rc1", "0.38.0.r812.gABCDEF0.n41", "../0.38.0"):
            self.assertFalse(sign.RELEASE.fullmatch(value), value)

    def test_notarization_accepts_team_and_individual_api_keys(self):
        key = Path("/keys/AuthKey.p8")
        self.assertEqual(sign.notary_credentials(key, "ABC123DEF4", "0123abcd-4567-89ef-0123-456789abcdef"),
                         ["--key", str(key), "--key-id", "ABC123DEF4",
                          "--issuer", "0123abcd-4567-89ef-0123-456789abcdef"])
        self.assertEqual(sign.notary_credentials(key, "ABC123DEF4", None),
                         ["--key", str(key), "--key-id", "ABC123DEF4"])
        self.assertEqual(sign.notary_credentials(key, "ABC123DEF4", ""),
                         ["--key", str(key), "--key-id", "ABC123DEF4"])


if __name__ == "__main__":
    unittest.main()
