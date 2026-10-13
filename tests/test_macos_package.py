"""The macOS bundle must reference only itself and the system, for Apple Silicon only."""
import ast
import importlib.util
from pathlib import Path
import plistlib
import re
import sys
import tempfile
import unittest

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
            self.assertIn(f'depends_on macos: ">= {names[floor]}"', template.read_text(encoding="utf-8"),
                          template.name)


if __name__ == "__main__":
    unittest.main()
