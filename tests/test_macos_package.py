"""The macOS bundle must reference only itself and the system, for Apple Silicon only."""
import ast
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check = load("check_macos_bundle", "check-macos-bundle.py")
package = load("package_macos", "package-macos.py")
sign = load("sign_macos", "sign-macos.py")
sys.path.insert(0, str(ROOT / "scripts"))
import macos_release  # noqa: E402

OTOOL = """Zerus.app/Contents/MacOS/hgs-tray:
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.0
      sdk 15.5
   ntools 1
Load command 12
          cmd LC_LOAD_DYLIB
      cmdsize 88
         name @rpath/QtWidgets.framework/Versions/A/QtWidgets (offset 24)
   time stamp 2 Thu Jan  1 03:00:02 1970
Load command 13
          cmd LC_LOAD_DYLIB
      cmdsize 56
         name /usr/lib/libc++.1.dylib (offset 24)
Load command 14
          cmd LC_LOAD_WEAK_DYLIB
      cmdsize 88
         name /System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (offset 24)
Load command 20
          cmd LC_RPATH
      cmdsize 48
         path @executable_path/../Frameworks (offset 12)
"""

BAD = """Zerus.app/Contents/MacOS/hgs-tray:
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 15.0
      sdk 15.5
Load command 12
          cmd LC_LOAD_DYLIB
      cmdsize 88
         name /opt/homebrew/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore (offset 24)
Load command 13
          cmd LC_REEXPORT_DYLIB
      cmdsize 88
         name /Users/example/qt/lib/QtGui.framework/Versions/A/QtGui (offset 24)
Load command 20
          cmd LC_RPATH
      cmdsize 48
         path /Users/example/qt/lib (offset 12)
"""


class BundleChecks(unittest.TestCase):
    @unittest.skipUnless(Path("/bin/bash").exists(), "requires Bash")
    def test_unattended_cask_execution_requires_prior_installed_gatekeeper_acceptance(self):
        script = (ROOT / "scripts/ci/macos-cask-install.sh").read_text()
        segment = script.split("app=/Applications/Zerus.app\n", 1)[1].split('\nhome="', 1)[0]
        with tempfile.TemporaryDirectory(prefix="zerus-cask-gate-") as temporary:
            directory = Path(temporary)
            log = directory / "calls.jsonl"
            for name in ("python3", "xattr"):
                stub = directory / name
                stub.write_text(f"#!{sys.executable}\n" + '''import json, os, pathlib, sys
with open(os.environ["ZERUS_CASK_TEST_LOG"], "a") as log:
    log.write(json.dumps([pathlib.Path(sys.argv[0]).name, *sys.argv[1:]]) + "\\n")
if pathlib.Path(sys.argv[0]).name == "python3" and os.environ["ZERUS_CASK_TEST_REJECT"] == "1":
    sys.exit(7)
''')
                stub.chmod(0o755)
            for rejected in (False, True):
                with self.subTest(rejected=rejected):
                    log.write_text("")
                    env = dict(os.environ, PATH=f"{directory}:/usr/bin:/bin",
                               ZERUS_CASK_TEST_LOG=str(log), ZERUS_CASK_TEST_REJECT=str(int(rejected)))
                    command = 'set -euo pipefail\napp="$1"\nversion=0.42.2\nrelease=0.42.2\nteam=ABCDE12345\n' + segment
                    result = subprocess.run(["/bin/bash", "-c", command, "cask-gate", str(directory / "Zerus.app")],
                                            env=env, capture_output=True, text=True)
                    calls = [json.loads(line) for line in log.read_text().splitlines()]
                    self.assertEqual(result.returncode, 7 if rejected else 0, result.stderr)
                    self.assertEqual([call[0] for call in calls], ["python3"] if rejected else
                                     ["python3", "xattr", "python3"])
                    self.assertIn("--team-id", calls[0])
                    self.assertNotIn("--selftest", calls[0])
                    if not rejected:
                        self.assertEqual(calls[1], ["xattr", "-dr", "com.apple.quarantine", str(directory / "Zerus.app")])
                        self.assertIn("--selftest", calls[2])

    @unittest.skipUnless(Path("/bin/bash").exists(), "requires Bash")
    def test_cask_metadata_is_validated_by_the_system_bash_before_installation(self):
        script = ROOT / "scripts/ci/macos-cask-install.sh"
        # Execute the real pre-installation section, including its quoted
        # Python heredoc, with Apple's Bash 3.2 on macOS and Bash on Linux.
        preflight = script.read_text().split('\nasset="$signed/', 1)[0]
        preflight += '\nprintf "%s\\n" "$cask" "$release" "$team"\n'
        with tempfile.TemporaryDirectory(prefix="zerus-cask-metadata-") as temporary:
            directory = Path(temporary)
            metadata = directory / "macos-info.json"
            for channel, release, cask in (("stable", "0.42.2", "zerus"),
                    ("nightly", "0.42.2.r812.gabcdef0.n41", "zerus@nightly")):
                info = {"asset": f"zerus-{release}-macos-arm64.zip", "qt_version": "6.11.2",
                        "team_id": "ABCDE12345", "minimum_system_version": "14.0",
                        "notarization_id": "0123abcd-4567-89ef-0123-456789abcdef",
                        "dmg": {"asset": f"zerus-{release}-macos-arm64.dmg",
                                "notarization_id": "1123abcd-4567-89ef-0123-456789abcdef"}}
                metadata.write_text(json.dumps(info))
                command = ["/bin/bash", "-c", preflight, str(script), str(directory), channel]
                with self.subTest(channel=channel):
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.splitlines(), [cask, release, "ABCDE12345"])
                for invalid in ({**info, "team_id": "invalid"},
                                {key: value for key, value in info.items() if key != "dmg"}):
                    metadata.write_text(json.dumps(invalid))
                    result = subprocess.run(command, capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, "", "invalid metadata must stop before installation")

    def test_selftest_exposes_tmux_with_an_isolated_home_and_socket_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tmux = root / "tmux"
            tmux.write_text("#!/bin/sh\nprintf 'tmux fixture\\n'\n")
            tmux.chmod(0o755)
            bundle = root / "Zerus.app"
            binaries = bundle / "Contents/MacOS"
            binaries.mkdir(parents=True)
            for name in ("hgs", "hgs-tray"):
                executable = binaries / name
                executable.write_text(f"#!{sys.executable}\n" + '''import os, pathlib, subprocess
assert os.environ["HOME"] != "/nonexistent/parent-home"
assert pathlib.Path(os.environ["HOME"]).is_dir()
assert os.environ["TMUX_TMPDIR"] == os.environ["HOME"]
assert "TMUX" not in os.environ and "HGS_SESSION" not in os.environ
assert "/nonexistent/parent-bin" not in os.environ["PATH"]
assert subprocess.check_output(["tmux", "-V"], text=True).strip() == "tmux fixture"
''')
                executable.chmod(0o755)
            with mock.patch.object(check.shutil, "which", return_value=str(tmux)), \
                    mock.patch.dict(os.environ, {"HOME": "/nonexistent/parent-home", "TMUX": "parent-socket",
                                                 "HGS_SESSION": "parent-session", "PATH": "/nonexistent/parent-bin"}):
                self.assertEqual(check.run_selftest(bundle), [])

    def test_selftest_reports_missing_tmux_before_executing_the_bundle(self):
        with mock.patch.object(check.shutil, "which", return_value=None):
            self.assertEqual(check.run_selftest(Path("/nonexistent/Zerus.app")),
                             ["bundle self-tests require tmux (brew install tmux)"])

    def test_load_commands_are_parsed(self):
        self.assertEqual(check.load_commands(OTOOL), [
            ("LC_BUILD_VERSION", "13.0"),
            ("LC_LOAD_DYLIB", "@rpath/QtWidgets.framework/Versions/A/QtWidgets"),
            ("LC_LOAD_DYLIB", "/usr/lib/libc++.1.dylib"),
            ("LC_LOAD_WEAK_DYLIB", "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit"),
            ("LC_RPATH", "@executable_path/../Frameworks"),
        ])

    def test_bundle_and_system_references_are_allowed(self):
        self.assertEqual(check.reference_findings("hgs-tray", check.load_commands(OTOOL), "13.0"), [])

    def test_foreign_references_and_newer_minimum_are_reported(self):
        findings = check.reference_findings("hgs-tray", check.load_commands(BAD), "13.0")
        self.assertEqual(len(findings), 4, findings)
        joined = "\n".join(findings)
        for expected in ("/opt/homebrew", "/Users/example/qt/lib/QtGui", "rpath /Users/example/qt/lib", "15.0"):
            self.assertIn(expected, joined)

    def test_relative_references_must_resolve_inside_the_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / "Zerus.app"
            main = bundle / "Contents/MacOS/hgs-tray"
            framework = bundle / "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
            helper = bundle / "Contents/Frameworks/QtHelpers.framework/Helpers/Helper.app/Contents/MacOS/Helper"
            for path in (main, framework, helper):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"")
            commands = [("LC_RPATH", "@executable_path/../Frameworks"),
                        ("LC_LOAD_DYLIB", "@rpath/QtCore.framework/Versions/A/QtCore"),
                        ("LC_LOAD_DYLIB", "/usr/lib/libc++.1.dylib")]
            self.assertEqual(check.unresolved_references(bundle, main, commands), [])
            nested = [("LC_RPATH", "@executable_path/../../../../../../Frameworks"),
                      ("LC_LOAD_DYLIB", "@rpath/QtCore.framework/Versions/A/QtCore")]
            self.assertEqual(check.unresolved_references(bundle, helper, nested), [])
            missing = commands + [("LC_LOAD_DYLIB", "@rpath/QtMultimedia.framework/Versions/A/QtMultimedia"),
                                  ("LC_LOAD_DYLIB", "@loader_path/libmissing.dylib")]
            findings = check.unresolved_references(bundle, main, missing)
            self.assertEqual(len(findings), 2, findings)
            self.assertIn("QtMultimedia", findings[0])

    def test_plist_identity_and_minimum_system_are_required(self):
        good = {"CFBundleIdentifier": "com.hgdev.hgs-tray", "CFBundleExecutable": "hgs-tray",
                "CFBundleName": "Zerus", "CFBundleDisplayName": "Zerus", "CFBundleShortVersionString": "0.38.0",
                "CFBundleVersion": "0.38.0", "LSMinimumSystemVersion": "14.0",
                "NSAppleEventsUsageDescription": "Zerus opens your agent sessions in Terminal.",
                "ZerusReleaseVersion": "0.38.0"}
        self.assertEqual(check.plist_findings(good, "0.38.0"), [])
        missing = dict(good)
        del missing["NSAppleEventsUsageDescription"]
        self.assertTrue(check.plist_findings(missing, "0.38.0"), "hardened runtime needs the Apple Events purpose")
        for key, value in (("CFBundleIdentifier", "dev.zerus.app"), ("CFBundleShortVersionString", "0.37.0"),
                           ("LSMinimumSystemVersion", "15.0"), ("CFBundleName", "hgs zerus")):
            self.assertTrue(check.plist_findings(dict(good, **{key: value}), "0.38.0"), key)

    def test_plist_records_the_channel_release_version(self):
        good = {"CFBundleIdentifier": "com.hgdev.hgs-tray", "CFBundleExecutable": "hgs-tray",
                "CFBundleName": "Zerus", "CFBundleDisplayName": "Zerus", "CFBundleShortVersionString": "0.38.0",
                "CFBundleVersion": "0.38.0", "LSMinimumSystemVersion": "14.0",
                "NSAppleEventsUsageDescription": "Zerus opens your agent sessions in Terminal."}
        nightly = "0.38.0.r812.gabcdef0.n41"
        self.assertTrue(check.plist_findings(good, "0.38.0"), "the release version key is required")
        self.assertEqual(check.plist_findings(dict(good, ZerusReleaseVersion="0.38.0"), "0.38.0"), [])
        self.assertEqual(check.plist_findings(dict(good, ZerusReleaseVersion=nightly), "0.38.0", nightly), [])
        self.assertTrue(check.plist_findings(dict(good, ZerusReleaseVersion="0.38.0"), "0.38.0", nightly))

    def test_required_files_include_setup_and_notices(self):
        names = set(check.REQUIRED_FILES)
        for name in ("Contents/MacOS/hgs", "Contents/Resources/bin/zerus-setup",
                     "Contents/Resources/setup/install-macos-session-service.py",
                     "Contents/Resources/licenses/qt/LGPL-3.0.txt",
                     "Contents/Resources/licenses/qt/qtmultimedia.spdx.json",
                     "Contents/Resources/licenses/qt/SOURCE.md",
                     "Contents/Resources/licenses/rust/inventory.json",
                     "Contents/Resources/licenses/codex-LICENSE.txt"):
            self.assertIn(name, names)

    def test_qt_webengine_never_ships_in_the_bundle(self):
        self.assertFalse([name for name in check.REQUIRED_FILES if "webengine" in name.lower()])
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / "Zerus.app"
            (bundle / "Contents/Frameworks/QtWebEngineCore.framework").mkdir(parents=True)
            (bundle / "Contents/Info.plist").write_bytes(plistlib.dumps({}))
            findings = check.bundle_findings(bundle, "0.38.0")
            self.assertIn("Contents/Frameworks/QtWebEngineCore.framework: Qt WebEngine is not part of the macOS bundle",
                          findings)

    def test_setup_scripts_ship_with_their_local_imports(self):
        local = {path.stem for path in (ROOT / "scripts").glob("*.py")}
        for name in package.SETUP_SCRIPTS:
            self.assertIn(f"Contents/Resources/setup/{name}", check.REQUIRED_FILES)
            tree = ast.parse((ROOT / "scripts" / name).read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                modules = [alias.name for alias in node.names] if isinstance(node, ast.Import) else \
                    [node.module] if isinstance(node, ast.ImportFrom) and node.module else []
                for module in modules:
                    if module in local:
                        self.assertIn(f"{module}.py", package.SETUP_SCRIPTS, f"{name} imports {module}")

    def test_minimum_system_matches_the_desktop_build(self):
        cmake = (ROOT / "tray/CMakeLists.txt").read_text(encoding="utf-8")
        floor = re.search(r'CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "([0-9.]+)"', cmake).group(1)
        for module in (package, check, sign, macos_release):
            self.assertEqual(module.MINIMUM_SYSTEM, floor, module.__name__)
        names = {"13.0": ":ventura", "14.0": ":sonoma", "15.0": ":sequoia"}
        for template in sorted((ROOT / "packaging/homebrew").glob("*.rb.in")):
            self.assertIn(f'depends_on macos: {names[floor]}', template.read_text(encoding="utf-8"),
                          template.name)


if __name__ == "__main__":
    unittest.main()
